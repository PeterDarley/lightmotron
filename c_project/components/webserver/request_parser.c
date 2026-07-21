#include "request_parser.h"
#include "json_helpers.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "request_parser";

#define RECV_BUF_SIZE 4096
#define MAX_BODY_SIZE (512 * 1024)

static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

void url_decode(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '%' && src[1] && src[2] && is_hex_digit(src[1]) && is_hex_digit(src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst = ' ';
            src++;
        } else {
            *dst = *src;
            src++;
        }
        dst++;
    }
    *dst = '\0';
}

cJSON *parse_query_string(const char *query)
{
    if (!query || *query == '\0') {
        return cJSON_CreateObject();
    }

    cJSON *result = cJSON_CreateObject();
    char *copy = strdup(query);
    if (!copy) {
        return result;
    }

    char *token = strtok(copy, "&");
    while (token) {
        char *eq = strchr(token, '=');
        char *key;
        char *value;
        if (eq) {
            *eq = '\0';
            key = token;
            value = eq + 1;
            url_decode(key);
            url_decode(value);
        } else {
            /* Bare key with no "=" (e.g. a checkbox/flag param). Python's
             * _parse_form_data does `key = _url_decode(pair); value = ""` in
             * this case rather than dropping the pair — match that instead of
             * silently discarding the key. */
            key = token;
            url_decode(key);
            value = "";
        }

        /* Handle multi-value (strtok never yields an empty token here, since
         * consecutive '&' delimiters collapse — matching Python's explicit
         * `else: continue` for an empty pair). */
        cJSON *existing = cJSON_GetObjectItem(result, key);
        if (existing) {
            if (cJSON_IsArray(existing)) {
                cJSON_AddItemToArray(existing, cJSON_CreateString(value));
            } else {
                cJSON *arr = cJSON_CreateArray();
                cJSON_AddItemToArray(arr, cJSON_Duplicate(existing, 1));
                cJSON_AddItemToArray(arr, cJSON_CreateString(value));
                cJSON_DeleteItemFromObject(result, key);
                cJSON_AddItemToObject(result, key, arr);
            }
        } else {
            cJSON_AddStringToObject(result, key, value);
        }
        token = strtok(NULL, "&");
    }

    free(copy);
    return result;
}

cJSON *parse_form_urlencoded(const char *body, size_t len)
{
    if (!body || len == 0) {
        return cJSON_CreateObject();
    }

    /* Make null-terminated copy */
    char *copy = malloc(len + 1);
    if (!copy) {
        return cJSON_CreateObject();
    }
    memcpy(copy, body, len);
    copy[len] = '\0';

    cJSON *result = parse_query_string(copy);
    free(copy);
    return result;
}

void parse_multipart(const char *body, size_t body_len, const char *boundary,
                     cJSON *form_data, http_file_t **files, int *file_count)
{
    if (!body || !boundary || body_len == 0) {
        return;
    }

    /* Build boundary marker */
    char delim[256];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t delim_len = strlen(delim);

    const char *ptr = body;
    const char *end = body + body_len;

    /* Skip first boundary */
    ptr = strstr(ptr, delim);
    if (!ptr) {
        return;
    }
    ptr += delim_len;
    if (*ptr == '\r') ptr++;
    if (*ptr == '\n') ptr++;

    while (ptr < end) {
        /* Check for final boundary */
        if (ptr[0] == '-' && ptr[1] == '-') {
            break;
        }

        /* Parse part headers */
        char field_name[64] = "";
        char filename[128] = "";
        char content_type[64] = ""; /* lib/webserver.py leaves this "" unless a
                                      * Content-Type header is present in the part */

        while (ptr < end && !(ptr[0] == '\r' && ptr[1] == '\n')) {
            const char *line_end = strstr(ptr, "\r\n");
            if (!line_end) {
                break;
            }

            size_t line_len = line_end - ptr;
            char line[512];
            size_t copy_len = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
            memcpy(line, ptr, copy_len);
            line[copy_len] = '\0';

            /* Parse Content-Disposition */
            if (strncasecmp(line, "Content-Disposition:", 20) == 0) {
                char *name_start = strstr(line, "name=\"");
                if (name_start) {
                    name_start += 6;
                    char *name_end = strchr(name_start, '"');
                    if (name_end) {
                        size_t nlen = name_end - name_start;
                        if (nlen < sizeof(field_name)) {
                            memcpy(field_name, name_start, nlen);
                            field_name[nlen] = '\0';
                        }
                    }
                }
                char *fn_start = strstr(line, "filename=\"");
                if (fn_start) {
                    fn_start += 10;
                    char *fn_end = strchr(fn_start, '"');
                    if (fn_end) {
                        size_t flen = fn_end - fn_start;
                        if (flen < sizeof(filename)) {
                            memcpy(filename, fn_start, flen);
                            filename[flen] = '\0';
                        }
                    }
                }
            }

            /* Parse Content-Type */
            if (strncasecmp(line, "Content-Type:", 13) == 0) {
                const char *ct = line + 13;
                while (*ct == ' ') ct++;
                strncpy(content_type, ct, sizeof(content_type) - 1);
            }

            ptr = line_end + 2;
        }

        /* Skip blank line after headers */
        if (ptr < end && ptr[0] == '\r' && ptr[1] == '\n') {
            ptr += 2;
        }

        /* Find end of part data (next boundary) */
        const char *part_end = NULL;
        const char *search = ptr;
        while (search < end - delim_len) {
            if (memcmp(search, delim, delim_len) == 0) {
                /* Back up past \r\n before boundary */
                part_end = search;
                if (part_end >= ptr + 2 && part_end[-2] == '\r' && part_end[-1] == '\n') {
                    part_end -= 2;
                }
                break;
            }
            search++;
        }

        if (!part_end) {
            part_end = end;
        }

        size_t data_len = part_end - ptr;

        if (field_name[0] != '\0' && filename[0] != '\0' && files && file_count) {
            /* File upload */
            int idx = *file_count;
            *files = realloc(*files, sizeof(http_file_t) * (idx + 1));
            if (*files) {
                strncpy((*files)[idx].field_name, field_name, sizeof((*files)[idx].field_name) - 1);
                strncpy((*files)[idx].filename, filename, sizeof((*files)[idx].filename) - 1);
                strncpy((*files)[idx].content_type, content_type, sizeof((*files)[idx].content_type) - 1);
                (*files)[idx].data = malloc(data_len);
                if ((*files)[idx].data) {
                    memcpy((*files)[idx].data, ptr, data_len);
                }
                (*files)[idx].data_len = data_len;
                (*file_count)++;
            }
        } else if (field_name[0] != '\0' && form_data) {
            /* Regular form field */
            char *value = malloc(data_len + 1);
            if (value) {
                memcpy(value, ptr, data_len);
                value[data_len] = '\0';

                cJSON *existing = cJSON_GetObjectItem(form_data, field_name);
                if (existing) {
                    if (cJSON_IsArray(existing)) {
                        cJSON_AddItemToArray(existing, cJSON_CreateString(value));
                    } else {
                        cJSON *arr = cJSON_CreateArray();
                        cJSON_AddItemToArray(arr, cJSON_Duplicate(existing, 1));
                        cJSON_AddItemToArray(arr, cJSON_CreateString(value));
                        cJSON_DeleteItemFromObject(form_data, field_name);
                        cJSON_AddItemToObject(form_data, field_name, arr);
                    }
                } else {
                    cJSON_AddStringToObject(form_data, field_name, value);
                }
                free(value);
            }
        }

        /* Move past boundary */
        ptr = search + delim_len;
        if (ptr < end && *ptr == '\r') ptr++;
        if (ptr < end && *ptr == '\n') ptr++;
    }
}

http_request_t *request_parse(int client_sock)
{
    char *recv_buf = malloc(RECV_BUF_SIZE);
    if (!recv_buf) {
        return NULL;
    }

    /* Receive initial data */
    int received = recv(client_sock, recv_buf, RECV_BUF_SIZE - 1, 0);
    if (received <= 0) {
        free(recv_buf);
        return NULL;
    }
    recv_buf[received] = '\0';

    /* Parse request line */
    char *line_end = strstr(recv_buf, "\r\n");
    if (!line_end) {
        free(recv_buf);
        return NULL;
    }

    http_request_t *req = calloc(1, sizeof(http_request_t));
    if (!req) {
        free(recv_buf);
        return NULL;
    }

    /* Extract method */
    char method_str[16] = "";
    char *space1 = strchr(recv_buf, ' ');
    if (!space1) {
        free(recv_buf);
        free(req);
        return NULL;
    }
    size_t method_len = space1 - recv_buf;
    if (method_len < sizeof(method_str)) {
        memcpy(method_str, recv_buf, method_len);
        method_str[method_len] = '\0';
    }

    if (strcmp(method_str, "GET") == 0) {
        req->method = HTTP_METHOD_GET;
    } else if (strcmp(method_str, "POST") == 0) {
        req->method = HTTP_METHOD_POST;
    } else if (strcmp(method_str, "PUT") == 0) {
        req->method = HTTP_METHOD_PUT;
    } else if (strcmp(method_str, "DELETE") == 0) {
        req->method = HTTP_METHOD_DELETE;
    } else {
        req->method = HTTP_METHOD_UNKNOWN;
    }

    /* Extract path */
    char *path_start = space1 + 1;
    char *space2 = strchr(path_start, ' ');
    if (!space2) {
        free(recv_buf);
        request_free(req);
        return NULL;
    }

    size_t raw_path_len = space2 - path_start;
    if (raw_path_len < sizeof(req->raw_path)) {
        memcpy(req->raw_path, path_start, raw_path_len);
        req->raw_path[raw_path_len] = '\0';
    }

    /* Split path and query string */
    char *query_sep = strchr(req->raw_path, '?');
    if (query_sep) {
        size_t path_len = query_sep - req->raw_path;
        memcpy(req->path, req->raw_path, path_len);
        req->path[path_len] = '\0';
        strncpy(req->query_string, query_sep + 1, sizeof(req->query_string) - 1);
    } else {
        strncpy(req->path, req->raw_path, sizeof(req->path) - 1);
        req->query_string[0] = '\0';
    }

    /* Extract HTTP version. lib/webserver.py defaults to "HTTP/1.0" when the
     * token is missing, and that default changes the keep-alive default
     * (HTTP/1.1 -> keep-alive unless "Connection: close"; anything else ->
     * closed unless "Connection: keep-alive"). */
    req->is_http_1_1 = false;
    char *version_start = space2 + 1;
    if (version_start < line_end) {
        size_t version_len = line_end - version_start;
        char version_str[16] = "";
        if (version_len < sizeof(version_str)) {
            memcpy(version_str, version_start, version_len);
            version_str[version_len] = '\0';
            if (strcmp(version_str, "HTTP/1.1") == 0) {
                req->is_http_1_1 = true;
            }
        }
    }

    /* Parse headers */
    req->headers = cJSON_CreateObject();
    char *header_start = line_end + 2;
    while (header_start < recv_buf + received) {
        char *header_end = strstr(header_start, "\r\n");
        if (!header_end || header_end == header_start) {
            header_start = header_end ? header_end + 2 : header_start;
            break;
        }

        char *colon = strchr(header_start, ':');
        if (colon && colon < header_end) {
            /* Extract key (lowercase) */
            size_t key_len = colon - header_start;
            char key[128];
            if (key_len < sizeof(key)) {
                memcpy(key, header_start, key_len);
                key[key_len] = '\0';
                for (size_t i = 0; i < key_len; i++) {
                    if (key[i] >= 'A' && key[i] <= 'Z') {
                        key[i] += 32;
                    }
                }

                /* Extract value (skip leading space) */
                char *val_start = colon + 1;
                while (*val_start == ' ') val_start++;
                size_t val_len = header_end - val_start;
                char value[512];
                if (val_len < sizeof(value)) {
                    memcpy(value, val_start, val_len);
                    value[val_len] = '\0';
                    cJSON_AddStringToObject(req->headers, key, value);
                }
            }
        }

        header_start = header_end + 2;
    }

    /* Read body if Content-Length present */
    const char *content_length_str = json_get_string(req->headers, "content-length", NULL);
    if (content_length_str) {
        size_t content_length = atoi(content_length_str);
        if (content_length > MAX_BODY_SIZE) {
            /* lib/webserver.py's _read_request aborts the request entirely
             * (`return None`) when Content-Length exceeds _MAX_REQUEST_BODY —
             * the caller just closes the connection with no response. Match
             * that instead of silently clamping/truncating: a truncated body
             * would be parsed as if it were the complete (and often corrupt)
             * payload, which is worse than dropping the connection. */
            free(recv_buf);
            request_free(req);
            return NULL;
        }

        if (content_length > 0) {
            req->body = malloc(content_length + 1);
            if (req->body) {
                /* Copy any body data already received */
                size_t header_total = header_start - recv_buf;
                size_t body_already = received - header_total;
                if (body_already > content_length) {
                    body_already = content_length;
                }
                memcpy(req->body, header_start, body_already);

                /* Receive remaining body */
                size_t total_body = body_already;
                while (total_body < content_length) {
                    int chunk = recv(client_sock, req->body + total_body,
                                     content_length - total_body, 0);
                    if (chunk <= 0) {
                        break;
                    }
                    total_body += chunk;
                }
                req->body[total_body] = '\0';
                req->body_len = total_body;
            }
        }
    }

    /* Parse form data based on Content-Type */
    const char *content_type = json_get_string(req->headers, "content-type", "");
    if (req->body && req->body_len > 0) {
        if (strstr(content_type, "application/x-www-form-urlencoded")) {
            req->form_data = parse_form_urlencoded(req->body, req->body_len);
        } else if (strstr(content_type, "multipart/form-data")) {
            /* Extract boundary */
            const char *boundary_start = strstr(content_type, "boundary=");
            if (boundary_start) {
                boundary_start += 9;
                char boundary[128];
                const char *boundary_end = strchr(boundary_start, ';');
                size_t blen = boundary_end ?
                    (size_t)(boundary_end - boundary_start) : strlen(boundary_start);
                if (blen < sizeof(boundary)) {
                    memcpy(boundary, boundary_start, blen);
                    boundary[blen] = '\0';

                    req->form_data = cJSON_CreateObject();
                    parse_multipart(req->body, req->body_len, boundary,
                                    req->form_data, &req->files, &req->file_count);
                }
            }
        }
    }

    free(recv_buf);
    return req;
}

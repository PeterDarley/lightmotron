#include "webserver.h"
#include "request_parser.h"
#include "static_files.h"
#include "template_engine.h"
#include "mime_types.h"
#include "json_helpers.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>

static const char *TAG = "webserver";

#define MAX_ROUTES 128
/* Sized for the heaviest page renders (e.g. /setup). Template includes nest
 * only ~3 deep at peak (setup.html -> base_head.html -> imports.html; the
 * dozen summary includes in setup.html are sequential and unwind between
 * each), so a handful of template_engine.c stack frames with their
 * 256-512 byte local buffers is the real ceiling - a few KB. 12KB leaves
 * ~4x margin (verified via uxTaskGetStackHighWaterMark, logged below when
 * headroom runs low).
 *
 * This MUST come from INTERNAL RAM: xTaskCreate() stacks are never placed in
 * PSRAM, and must not be here anyway, since a settings write on another task
 * can disable the flash cache (and thus PSRAM access) while a client task is
 * mid-request. Because the whole 512KB internal heap is shared with WiFi/
 * lwip/mDNS and gets fragmented, an oversized stack fails to find a
 * *contiguous* block even when plenty of total RAM is free - which is why
 * this was cut from 20KB (that caused "Failed to spawn client task" under
 * the parallel asset requests a single page load fires off). MAX_CLIENT_TASKS
 * caps how many run at once. */
#define CLIENT_TASK_STACK_SIZE 12288
/* Idle keep-alive connections block a client task (and thus a slot below) in
 * request_parse() for this long waiting for the next request. Kept short so a
 * browser's pool of idle keep-alive sockets releases slots quickly - a page's
 * asset burst all arrives within ~1s, so 3s still gets full connection reuse
 * during a load without pinning slots long after it. */
#define KEEPALIVE_TIMEOUT_SEC 3
/* Cap on concurrent client handler tasks. Bounds internal-RAM use
 * (MAX_CLIENT_TASKS * CLIENT_TASK_STACK_SIZE worst case = ~72KB) so a burst of
 * browser connections can't exhaust internal DRAM and start failing
 * xTaskCreate(). Set to 6 to match Chrome/Firefox's 6-connections-per-host
 * limit: a single page load opens up to 6 parallel keep-alive sockets, so a
 * lower cap dropped the surplus every load (browsers retry, but that showed as
 * occasional missing CSS/JS). Tracked with a counting semaphore
 * (client_slots). Affordable now that the stack is 12KB, not 20KB. */
#define MAX_CLIENT_TASKS 6

typedef struct {
    http_method_t method;
    char path[256];
    route_handler_t handler;
} route_entry_t;

static route_entry_t routes[MAX_ROUTES];
static int route_count = 0;
static SemaphoreHandle_t route_mutex = NULL;
static int server_socket = -1;
static bool server_running = false;
static TaskHandle_t server_task_handle = NULL;
/* Counting semaphore of available client-handler slots (see MAX_CLIENT_TASKS).
 * A slot is taken before spawning a client task and released when that task
 * exits, capping concurrent connections and the internal RAM they consume. */
static SemaphoreHandle_t client_slots = NULL;

void webserver_add_route(http_method_t method, const char *path, route_handler_t handler)
{
    if (route_count >= MAX_ROUTES) {
        ESP_LOGE(TAG, "Route table full");
        return;
    }

    if (!route_mutex) {
        route_mutex = xSemaphoreCreateMutex();
        if (!route_mutex) {
            ESP_LOGE(TAG, "Failed to create route_mutex");
            return;
        }
    }

    xSemaphoreTake(route_mutex, portMAX_DELAY);
    routes[route_count].method = method;
    strncpy(routes[route_count].path, path, sizeof(routes[route_count].path) - 1);
    routes[route_count].handler = handler;
    route_count++;
    xSemaphoreGive(route_mutex);
}

void webserver_add_route_both(const char *path, route_handler_t get_handler,
                              route_handler_t post_handler)
{
    if (get_handler) {
        webserver_add_route(HTTP_METHOD_GET, path, get_handler);
    }
    if (post_handler) {
        webserver_add_route(HTTP_METHOD_POST, path, post_handler);
    }
}

/**
 * Find a route handler for the given method and path.
 *
 * Tolerates trailing slash differences.
 */
static route_handler_t find_route(http_method_t method, const char *path)
{
    xSemaphoreTake(route_mutex, portMAX_DELAY);

    for (int i = 0; i < route_count; i++) {
        if (routes[i].method != method) {
            continue;
        }

        /* Exact match */
        if (strcmp(routes[i].path, path) == 0) {
            route_handler_t handler = routes[i].handler;
            xSemaphoreGive(route_mutex);
            return handler;
        }

        /* Trailing slash tolerance */
        size_t path_len = strlen(path);
        size_t route_len = strlen(routes[i].path);

        if (path_len == route_len + 1 && path[path_len - 1] == '/' &&
            strncmp(routes[i].path, path, route_len) == 0) {
            route_handler_t handler = routes[i].handler;
            xSemaphoreGive(route_mutex);
            return handler;
        }

        if (route_len == path_len + 1 && routes[i].path[route_len - 1] == '/' &&
            strncmp(routes[i].path, path, path_len) == 0) {
            route_handler_t handler = routes[i].handler;
            xSemaphoreGive(route_mutex);
            return handler;
        }
    }

    xSemaphoreGive(route_mutex);
    return NULL;
}

/**
 * Send a complete HTTP response over the socket.
 */
static void send_response(int sock, http_response_t *resp, bool keep_alive)
{
    if (!resp) {
        return;
    }

    char header_buf[1024];
    int header_len;

    if (resp->file_path) {
        /* File response — stream from disk */
        FILE *f = fopen(resp->file_path, "r");
        if (!f) {
            header_len = snprintf(header_buf, sizeof(header_buf),
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            send(sock, header_buf, header_len, 0);
            return;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        header_len = snprintf(header_buf, sizeof(header_buf),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Connection: %s\r\n",
            resp->status_code, resp->reason,
            resp->content_type,
            file_size,
            keep_alive ? "keep-alive" : "close");

        /* Add extra headers */
        if (resp->headers) {
            cJSON *h = resp->headers->child;
            while (h) {
                header_len += snprintf(header_buf + header_len,
                    sizeof(header_buf) - header_len,
                    "%s: %s\r\n", h->string, h->valuestring);
                h = h->next;
            }
        }

        header_len += snprintf(header_buf + header_len,
            sizeof(header_buf) - header_len, "\r\n");
        send(sock, header_buf, header_len, 0);

        /* Stream file in chunks */
        char chunk[4096];
        size_t bytes_read;
        while ((bytes_read = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            send(sock, chunk, bytes_read, 0);
        }
        fclose(f);

    } else {
        /* Body response */
        size_t body_len = resp->body_len;
        if (resp->body && body_len == 0) {
            body_len = strlen(resp->body);
        }

        header_len = snprintf(header_buf, sizeof(header_buf),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n",
            resp->status_code, resp->reason,
            resp->content_type,
            body_len,
            keep_alive ? "keep-alive" : "close");

        /* Add extra headers */
        if (resp->headers) {
            cJSON *h = resp->headers->child;
            while (h) {
                header_len += snprintf(header_buf + header_len,
                    sizeof(header_buf) - header_len,
                    "%s: %s\r\n", h->string, h->valuestring);
                h = h->next;
            }
        }

        header_len += snprintf(header_buf + header_len,
            sizeof(header_buf) - header_len, "\r\n");

        send(sock, header_buf, header_len, 0);

        if (resp->body && body_len > 0) {
            send(sock, resp->body, body_len, 0);
        }
    }
}

static const char *http_method_name(http_method_t method)
{
    switch (method) {
        case HTTP_METHOD_GET:    return "GET";
        case HTTP_METHOD_POST:   return "POST";
        case HTTP_METHOD_PUT:    return "PUT";
        case HTTP_METHOD_DELETE: return "DELETE";
        default:                 return "?";
    }
}

/**
 * Handle a single client connection (keep-alive loop).
 */
static void client_task(void *pvParameters)
{
    int client_sock = (int)(intptr_t)pvParameters;

    /* Resolved once per connection (not per request) since it can't change
     * across a keep-alive connection's requests. */
    char client_ip[16] = "?";
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    if (getpeername(client_sock, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
        strncpy(client_ip, inet_ntoa(peer_addr.sin_addr), sizeof(client_ip) - 1);
    }

    /* Set receive timeout for keep-alive */
    struct timeval timeout = {
        .tv_sec = KEEPALIVE_TIMEOUT_SEC,
        .tv_usec = 0,
    };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (true) {
        /* Parse request */
        http_request_t *req = request_parse(client_sock);
        if (!req) {
            break; /* Connection closed or timeout */
        }

        http_response_t *resp = NULL;

        /* Determine keep-alive default from HTTP version, matching
         * lib/webserver.py's _read_request: HTTP/1.1 defaults to keep-alive
         * unless "Connection: close"; anything else (HTTP/1.0) defaults to
         * close unless "Connection: keep-alive". */
        const char *conn = req->headers ? json_get_string(req->headers, "connection", "") : "";
        bool keep_alive;
        if (req->is_http_1_1) {
            keep_alive = strcasecmp(conn, "close") != 0;
        } else {
            keep_alive = strcasecmp(conn, "keep-alive") == 0;
        }

        /* Try to find a route handler */
        route_handler_t handler = find_route(req->method, req->path);

        if (handler) {
            resp = handler(req);
        } else if (req->method == HTTP_METHOD_GET) {
            /* Try static file */
            const char *if_none_match = NULL;
            if (req->headers) {
                cJSON *inm = cJSON_GetObjectItem(req->headers, "if-none-match");
                if (inm && inm->valuestring) {
                    if_none_match = inm->valuestring;
                }
            }
            resp = static_file_serve(req->path, if_none_match);
            if (!resp) {
                if (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0) {
                    /* lib/webserver.py's _load_index() falls back to a
                     * hardcoded placeholder page (200 OK) instead of 404 when
                     * www/index.html is missing from the filesystem, and
                     * sends it with the negotiated keep-alive state — unlike
                     * the genuine-404 paths below, which always force the
                     * connection closed. */
                    resp = response_create(200, "text/html",
                                            "<html><body><h1>OK</h1></body></html>");
                } else {
                    resp = response_create(404, "text/html", "<h1>404 Not Found</h1>");
                    keep_alive = false;
                }
            }
        } else {
            resp = response_create(404, "text/html", "<h1>404 Not Found</h1>");
            keep_alive = false;
        }

        if (resp) {
            ESP_LOGI(TAG, "%s %s %s -> %d", client_ip, http_method_name(req->method),
                     req->path, resp->status_code);
            send_response(client_sock, resp, keep_alive);
            response_free(resp);
        } else {
            /* Route handler returned NULL: lib/webserver.py's _send_result
             * treats a None handler return value as 204 No Content rather
             * than sending nothing at all — an empty reply would otherwise
             * leave the client with no response bytes for a request it made. */
            ESP_LOGI(TAG, "%s %s %s -> 204", client_ip, http_method_name(req->method), req->path);
            http_response_t *no_content = response_create(204, "text/plain", NULL);
            if (no_content) {
                strncpy(no_content->reason, "No Content", sizeof(no_content->reason) - 1);
                send_response(client_sock, no_content, keep_alive);
                response_free(no_content);
            }
        }

        request_free(req);

        if (!keep_alive) {
            break;
        }
    }

    close(client_sock);

    /* Warn if this request came close to overflowing the stack, so an
     * undersized CLIENT_TASK_STACK_SIZE surfaces as a log line rather than a
     * corruption crash. High-water mark is the minimum free stack (in words)
     * ever seen for this task. */
    UBaseType_t stack_left_words = uxTaskGetStackHighWaterMark(NULL);
    if (stack_left_words * sizeof(StackType_t) < 2048) {
        ESP_LOGW(TAG, "Client task stack low: %u bytes headroom (raise CLIENT_TASK_STACK_SIZE)",
                 (unsigned)(stack_left_words * sizeof(StackType_t)));
    }

    if (client_slots) {
        xSemaphoreGive(client_slots);   /* Release the slot taken in server_task */
    }
    vTaskDelete(NULL);
}

/**
 * Server accept loop task.
 */
static void server_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Server accept loop started");

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(server_socket,
                                  (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            if (server_running) {
                ESP_LOGW(TAG, "Accept failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        ESP_LOGD(TAG, "Client connected from " IPSTR,
                 IP2STR((esp_ip4_addr_t *)&client_addr.sin_addr));

        /* Cap concurrency: wait briefly for a free slot. If none frees up,
         * drop this connection (close the socket so it isn't leaked - the
         * browser will retry) rather than over-committing internal RAM. */
        if (client_slots && xSemaphoreTake(client_slots, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "At max %d clients, dropping connection", MAX_CLIENT_TASKS);
            close(client_sock);
            continue;
        }

        /* Spawn client handler task. On failure (typically low internal RAM),
         * close the socket and release the slot - NOT doing so previously
         * leaked an LWIP socket per failure until accept() stopped working
         * entirely (matches lib/webserver.py, which always closes the
         * socket via _handle_client's finally). */
        if (xTaskCreate(client_task, "http_client",
                        CLIENT_TASK_STACK_SIZE, (void *)(intptr_t)client_sock,
                        4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Failed to spawn client task, dropping connection "
                     "(free internal=%u, largest block=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            close(client_sock);
            if (client_slots) {
                xSemaphoreGive(client_slots);
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t webserver_start(int port)
{
    if (server_running) {
        return ESP_OK;
    }

    if (!route_mutex) {
        route_mutex = xSemaphoreCreateMutex();
        if (!route_mutex) {
            ESP_LOGE(TAG, "Failed to create route_mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!client_slots) {
        client_slots = xSemaphoreCreateCounting(MAX_CLIENT_TASKS, MAX_CLIENT_TASKS);
        if (!client_slots) {
            ESP_LOGE(TAG, "Failed to create client_slots");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create server socket */
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket < 0) {
        ESP_LOGE(TAG, "Socket creation failed");
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(server_socket);
        return ESP_FAIL;
    }

    if (listen(server_socket, 4) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        close(server_socket);
        return ESP_FAIL;
    }

    server_running = true;

    /* Start accept loop on core 1 */
    xTaskCreatePinnedToCore(server_task, "http_server", 4096, NULL, 5, &server_task_handle, 1);

    ESP_LOGI(TAG, "Web server listening on port %d", port);
    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    server_running = false;

    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }

    if (server_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(500));
        server_task_handle = NULL;
    }

    return ESP_OK;
}

/* Response helper implementations */

http_response_t *response_create(int status_code, const char *content_type, const char *body)
{
    http_response_t *resp = calloc(1, sizeof(http_response_t));
    if (!resp) {
        return NULL;
    }

    resp->status_code = status_code;
    strncpy(resp->reason, status_code == 200 ? "OK" :
                          status_code == 301 ? "Moved Permanently" :
                          status_code == 302 ? "Found" :
                          status_code == 304 ? "Not Modified" :
                          status_code == 404 ? "Not Found" :
                          status_code == 500 ? "Internal Server Error" : "OK",
            sizeof(resp->reason) - 1);

    if (content_type) {
        strncpy(resp->content_type, content_type, sizeof(resp->content_type) - 1);
    } else {
        strncpy(resp->content_type, "text/html", sizeof(resp->content_type) - 1);
    }

    if (body) {
        resp->body_len = strlen(body);
        resp->body = malloc(resp->body_len + 1);
        if (resp->body) {
            memcpy(resp->body, body, resp->body_len + 1);
            resp->free_body = true;
        }
    }

    return resp;
}

http_response_t *response_create_file(const char *file_path, const char *content_type)
{
    http_response_t *resp = calloc(1, sizeof(http_response_t));
    if (!resp) {
        return NULL;
    }

    resp->status_code = 200;
    strncpy(resp->reason, "OK", sizeof(resp->reason) - 1);

    if (content_type) {
        strncpy(resp->content_type, content_type, sizeof(resp->content_type) - 1);
    } else {
        strncpy(resp->content_type, mime_type_for_path(file_path),
                sizeof(resp->content_type) - 1);
    }

    resp->file_path = strdup(file_path);
    return resp;
}

http_response_t *response_redirect(const char *location)
{
    http_response_t *resp = calloc(1, sizeof(http_response_t));
    if (!resp) {
        return NULL;
    }

    resp->status_code = 302;
    strncpy(resp->reason, "Found", sizeof(resp->reason) - 1);
    strncpy(resp->content_type, "text/html", sizeof(resp->content_type) - 1);
    resp->headers = cJSON_CreateObject();
    cJSON_AddStringToObject(resp->headers, "Location", location);
    resp->body = NULL;
    resp->body_len = 0;

    return resp;
}

void response_free(http_response_t *resp)
{
    if (!resp) {
        return;
    }

    if (resp->body && resp->free_body) {
        free(resp->body);
    }
    if (resp->file_path) {
        free(resp->file_path);
    }
    if (resp->headers) {
        cJSON_Delete(resp->headers);
    }
    free(resp);
}

void request_free(http_request_t *req)
{
    if (!req) {
        return;
    }

    if (req->headers) {
        cJSON_Delete(req->headers);
    }
    if (req->body) {
        free(req->body);
    }
    if (req->form_data) {
        cJSON_Delete(req->form_data);
    }
    if (req->files) {
        for (int i = 0; i < req->file_count; i++) {
            if (req->files[i].data) {
                free(req->files[i].data);
            }
        }
        free(req->files);
    }
    free(req);
}

http_response_t *webserver_render_response(const char *template_file, cJSON *ctx)
{
    char *html = template_render_file(template_file, ctx);
    cJSON_Delete(ctx);
    if (!html) return response_create(500, "text/plain", "Template error");
    http_response_t *res = response_create(200, "text/html", html);
    free(html);
    return res;
}

const char *request_get_form_field(http_request_t *req, const char *field_name)
{
    if (!req || !req->form_data || !field_name) {
        return NULL;
    }

    cJSON *field = cJSON_GetObjectItem(req->form_data, field_name);
    if (field && cJSON_IsString(field)) {
        return field->valuestring;
    }
    return NULL;
}

const char *request_get_form_field_last(http_request_t *req, const char *field_name)
{
    /* request_get_form_field() returns NULL when a field arrives as a JSON
     * array (multi-value submit) — e.g. a color <select> and its paired
     * <input type=color> share a field name, with the picker's value last. */
    if (!req || !req->form_data || !field_name) return NULL;
    cJSON *field = cJSON_GetObjectItem(req->form_data, field_name);
    if (!field) return NULL;
    if (cJSON_IsString(field)) return field->valuestring;
    if (cJSON_IsArray(field)) {
        int n = cJSON_GetArraySize(field);
        if (n <= 0) return NULL;
        cJSON *last = cJSON_GetArrayItem(field, n - 1);
        return (last && cJSON_IsString(last)) ? last->valuestring : NULL;
    }
    return NULL;
}

const char *request_get_query_param(http_request_t *req, const char *param_name)
{
    if (!req || !param_name || req->query_string[0] == '\0') {
        return NULL;
    }

    /* Parse query params and look up the requested one */
    cJSON *query_params = parse_query_string(req->query_string);
    if (!query_params) {
        return NULL;
    }

    cJSON *field = cJSON_GetObjectItem(query_params, param_name);
    if (field && cJSON_IsString(field)) {
        /* Merge into form_data so the value stays accessible after we free query_params */
        if (!req->form_data) {
            req->form_data = cJSON_CreateObject();
        }
        if (!cJSON_HasObjectItem(req->form_data, param_name)) {
            cJSON_AddStringToObject(req->form_data, param_name, field->valuestring);
        }
        cJSON_Delete(query_params);
        /* Return from form_data which lives as long as the request */
        cJSON *stored = cJSON_GetObjectItem(req->form_data, param_name);
        return stored ? stored->valuestring : NULL;
    }

    cJSON_Delete(query_params);
    return NULL;
}

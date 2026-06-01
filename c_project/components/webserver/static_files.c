#include "static_files.h"
#include "mime_types.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "static_files";

#define WWW_BASE "/spiffs/www"

http_response_t *static_file_serve(const char *url_path, const char *if_none_match)
{
    if (!url_path) {
        return NULL;
    }

    /* Reject path traversal attempts */
    if (strstr(url_path, "..") != NULL) {
        return NULL;
    }

    /* Build file path */
    char file_path[256];
    if (strcmp(url_path, "/") == 0 || strcmp(url_path, "/index.html") == 0) {
        snprintf(file_path, sizeof(file_path), "%s/index.html", WWW_BASE);
    } else {
        snprintf(file_path, sizeof(file_path), "%s%s", WWW_BASE, url_path);
    }

    /* Check if file exists */
    struct stat st;
    if (stat(file_path, &st) != 0) {
        return NULL;
    }

    /* Generate ETag */
    char etag[64];
    snprintf(etag, sizeof(etag), "\"%ld-%ld\"", (long)st.st_size, (long)st.st_mtime);

    /* Check If-None-Match */
    if (if_none_match && strcmp(if_none_match, etag) == 0) {
        http_response_t *resp = calloc(1, sizeof(http_response_t));
        if (resp) {
            resp->status_code = 304;
            strncpy(resp->reason, "Not Modified", sizeof(resp->reason) - 1);
            strncpy(resp->content_type, "text/html", sizeof(resp->content_type) - 1);
        }
        return resp;
    }

    /* Determine content type */
    const char *content_type = mime_type_for_path(file_path);

    /* Create file response */
    http_response_t *resp = response_create_file(file_path, content_type);
    if (!resp) {
        return NULL;
    }

    /* Add caching headers */
    resp->headers = cJSON_CreateObject();

    /* Determine cache policy */
    const char *ext = strrchr(file_path, '.');
    if (ext && (strcmp(ext, ".js") == 0 || strcmp(ext, ".css") == 0)) {
        cJSON_AddStringToObject(resp->headers, "Cache-Control",
                                "no-cache, max-age=0, must-revalidate");
    } else {
        cJSON_AddStringToObject(resp->headers, "Cache-Control",
                                "public, max-age=2592000");
    }

    cJSON_AddStringToObject(resp->headers, "ETag", etag);

    return resp;
}

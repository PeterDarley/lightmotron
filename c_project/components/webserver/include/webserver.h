#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_err.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>

/* Forward declarations */
typedef struct http_request http_request_t;
typedef struct http_response http_response_t;

/**
 * HTTP methods.
 */
typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_UNKNOWN,
} http_method_t;

/**
 * Uploaded file information.
 */
typedef struct {
    char field_name[64];
    char filename[128];
    char content_type[64];
    uint8_t *data;
    size_t data_len;
} http_file_t;

/**
 * HTTP request structure.
 */
struct http_request {
    http_method_t method;
    char path[256];
    char query_string[512];
    char raw_path[768];
    cJSON *headers;
    char *body;
    size_t body_len;
    cJSON *form_data;
    http_file_t *files;
    int file_count;
};

/**
 * HTTP response structure.
 */
struct http_response {
    int status_code;
    char reason[64];
    char content_type[128];
    char *body;
    size_t body_len;
    cJSON *headers;
    char *file_path;          /* If set, stream this file instead of body */
    bool free_body;           /* Whether body should be freed after send */
};

/**
 * Route handler function type.
 */
typedef http_response_t *(*route_handler_t)(http_request_t *request);

/**
 * Start the web server on the given port.
 */
esp_err_t webserver_start(int port);

/**
 * Stop the web server.
 */
esp_err_t webserver_stop(void);

/**
 * Register a route with method and path.
 */
void webserver_add_route(http_method_t method, const char *path, route_handler_t handler);

/**
 * Register a route that handles both GET and POST.
 */
void webserver_add_route_both(const char *path, route_handler_t get_handler,
                              route_handler_t post_handler);

/**
 * Create a new HTTP response.
 */
http_response_t *response_create(int status_code, const char *content_type,
                                 const char *body);

/**
 * Create a response that streams a file.
 */
http_response_t *response_create_file(const char *file_path, const char *content_type);

/**
 * Create a redirect response.
 */
http_response_t *response_redirect(const char *location);

/**
 * Free an HTTP response.
 */
void response_free(http_response_t *resp);

/**
 * Free an HTTP request.
 */
void request_free(http_request_t *req);

/**
 * Render a template file with the given context.
 *
 * Returns a newly allocated string (caller must free).
 */
char *render_template(const char *template_file, cJSON *context);

/**
 * Set the global context processor function.
 */
typedef cJSON *(*context_processor_fn)(void);
void webserver_set_context_processor(context_processor_fn processor);

/**
 * Get a form field value from the request.
 */
const char *request_get_form_field(http_request_t *req, const char *field_name);

/**
 * Get a query parameter value.
 */
const char *request_get_query_param(http_request_t *req, const char *param_name);

#endif /* WEBSERVER_H */

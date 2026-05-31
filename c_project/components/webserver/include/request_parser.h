#ifndef REQUEST_PARSER_H
#define REQUEST_PARSER_H

#include "webserver.h"

/**
 * Parse a raw HTTP request from a client socket.
 *
 * Returns a newly allocated http_request_t, or NULL on error.
 * Caller must free with request_free().
 */
http_request_t *request_parse(int client_sock);

/**
 * Parse URL-encoded form data into cJSON object.
 *
 * Handles multi-value fields (converted to arrays).
 */
cJSON *parse_form_urlencoded(const char *body, size_t len);

/**
 * Parse multipart form data.
 *
 * Extracts form fields into form_data and file uploads into files array.
 */
void parse_multipart(const char *body, size_t body_len, const char *boundary,
                     cJSON *form_data, http_file_t **files, int *file_count);

/**
 * URL-decode a string in-place.
 */
void url_decode(char *str);

/**
 * Parse query string into cJSON object.
 */
cJSON *parse_query_string(const char *query);

#endif /* REQUEST_PARSER_H */

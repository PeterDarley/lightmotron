#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include "webserver.h"

/**
 * Attempt to serve a static file for the given path.
 *
 * Returns a file response if found, NULL if the file doesn't exist.
 */
http_response_t *static_file_serve(const char *url_path, const char *if_none_match);

#endif /* STATIC_FILES_H */

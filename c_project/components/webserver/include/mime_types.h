#ifndef MIME_TYPES_H
#define MIME_TYPES_H

/**
 * Get the MIME type for a file path based on its extension.
 *
 * Returns "application/octet-stream" for unknown extensions.
 */
const char *mime_type_for_path(const char *path);

#endif /* MIME_TYPES_H */

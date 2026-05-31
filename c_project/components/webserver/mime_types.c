#include "mime_types.h"

#include <string.h>

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_entry_t;

static const mime_entry_t mime_table[] = {
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".ttf",  "font/ttf"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
    {".otf",  "font/otf"},
    {".txt",  "text/plain"},
    {".xml",  "application/xml"},
    {".mp3",  "audio/mpeg"},
    {".wav",  "audio/wav"},
    {NULL, NULL},
};

const char *mime_type_for_path(const char *path)
{
    if (!path) {
        return "application/octet-stream";
    }

    /* Find the last dot in the path */
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }

    /* Search the table */
    for (int i = 0; mime_table[i].extension != NULL; i++) {
        if (strcasecmp(dot, mime_table[i].extension) == 0) {
            return mime_table[i].mime_type;
        }
    }

    return "application/octet-stream";
}

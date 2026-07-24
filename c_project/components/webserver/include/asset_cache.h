#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include <stddef.h>

/**
 * In-RAM cache of the webassets SPIFFS partition (templates + static files).
 *
 * WHY: rendering a page reads its template files from SPIFFS on every
 * request, and each SPIFFS read runs esp_flash_read(), which disables the
 * flash cache on both CPU cores for the duration. If a UART TX interrupt
 * fires during that window it deadlocks against the flash coordination on
 * the other core, and the interrupt watchdog resets the chip (observed on
 * /setup, which pulls in 9+ templates per render). Loading every asset into
 * RAM once at boot - when nothing else is running yet - means requests are
 * served from memory and never touch flash, closing that window.
 */

/**
 * Populate the cache by reading every file on the webassets partition
 * (mounted at /spiffs) into RAM. Call once at boot, AFTER the SPIFFS mount
 * and BEFORE the web server starts. Safe to call again (no-op if already
 * initialized).
 */
void asset_cache_init(void);

/**
 * Look up a cached file by its full VFS path (e.g. "/spiffs/templates/home.html").
 * On hit, returns a pointer to the file's contents (NUL-terminated, so it is
 * also usable as a C string for text assets) and writes the byte length to
 * *out_size. Returns NULL on miss. The returned buffer is owned by the cache
 * and stays valid for the lifetime of the program - do not free it.
 */
const char *asset_cache_get(const char *full_path, size_t *out_size);

#endif /* ASSET_CACHE_H */

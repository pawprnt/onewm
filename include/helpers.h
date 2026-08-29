#pragma once
#include <stddef.h>

/* Locate a file shipped with onewm. Resolution order:
 *   1. $ONEWM_WALLPAPER_DIR  (only for "wallpapers/..." paths)
 *   2. $ONEWM_DATA_DIR       (embedded-asset cache, or external data dir)
 *   3. ./data                (build tree)
 *   4. ./themes              (build tree, themes_metadata.json)
 *   5. ../themes             (build tree, run from build/)
 *   6. /usr/share/onewm      (installed)
 * Returns a pointer to a static buffer (not thread-safe), or NULL. */
char *find_data(const char *rel);

/* Read an entire file into a NUL-terminated buffer (caller must free()).
 * On success *len receives the byte count (excluding NUL). NULL on failure. */
char *read_file(const char *path, size_t *len);

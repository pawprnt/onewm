#pragma once

#include <stddef.h>

/* Embedded assets are linked into the binary as a single objcopy'd blob
   (symbols _binary_assets_bin_start/_end). asset_bootstrap() extracts them
   to a per-user cache directory and points ONEWM_DATA_DIR at it, so all
   existing find_data()/fopen/loaders work unchanged with zero external files.

   Disable extraction with ONEWM_NO_EMBED=1 (then you must supply ONEWM_DATA_DIR). */
extern const unsigned char _binary_assets_bin_start[];
extern const unsigned char _binary_assets_bin_end[];

void asset_bootstrap(void);
const char *asset_cache_dir(void);

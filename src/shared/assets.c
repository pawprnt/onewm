#define _GNU_SOURCE
#include "assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static const unsigned char *blob_start = _binary_assets_bin_start;
static const unsigned char *blob_end = _binary_assets_bin_end;

static void mkdirs(const char *path) {
	char tmp[8192];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0700);
			*p = '/';
		}
	}
	mkdir(tmp, 0700);
}

static const char *cache_dir(void) {
	static char dir[4096];
	if (dir[0])
		return dir;
	const char *r = getenv("XDG_RUNTIME_DIR");
	snprintf(dir, sizeof dir, "%s/onewm-assets-%d", r ? r : "/tmp",
	         (int)getuid());
	return dir;
}

static uint16_t read_u16(const unsigned char **p) {
	uint16_t v;
	memcpy(&v, *p, 2);
	*p += 2;
	return v;
}

static uint32_t read_u32(const unsigned char **p) {
	uint32_t v;
	memcpy(&v, *p, 4);
	*p += 4;
	return v;
}

void asset_bootstrap(void) {
	if (getenv("ONEWM_NO_EMBED"))
		return;

	const unsigned char *p = blob_start;
	if (p + 4 > blob_end || memcmp(p, "OWMA", 4) != 0)
		return;
	p += 4;
	if (p + 4 > blob_end)
		return;
	uint32_t count = read_u32(&p);

	const char *cd = cache_dir();
	mkdirs(cd);

	for (uint32_t i = 0; i < count; i++) {
		if (p + 2 > blob_end)
			break;
		uint16_t nl = read_u16(&p);
		char name[4096];
		if (nl == 0 || nl >= sizeof(name) || p + nl > blob_end)
			break;
		memcpy(name, p, nl);
		name[nl] = '\0';
		p += nl;

		if (p + 4 > blob_end)
			break;
		uint32_t dl = read_u32(&p);
		if (dl > (uintptr_t)(blob_end - p))
			break;
		const unsigned char *data = p;
		p += dl;

		/* Reject names that could escape the cache directory. */
		if (name[0] == '/' || strstr(name, "..") != NULL)
			continue;

		char full[8192];
		snprintf(full, sizeof full, "%s/%s", cd, name);
		char *slash = strrchr(full, '/');
		if (slash) {
			*slash = '\0';
			mkdirs(full);
			*slash = '/';
		}

		struct stat st;
		if (stat(full, &st) != 0) {
			FILE *f = fopen(full, "wb");
			if (f) {
				fwrite(data, 1, dl, f);
				fclose(f);
			}
		}
	}

	if (!getenv("ONEWM_DATA_DIR"))
		setenv("ONEWM_DATA_DIR", cd, 1);
}

const char *asset_cache_dir(void) {
	return cache_dir();
}

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "helpers.h"

char *find_data(const char *rel) {
    static char path[4096];

    if (strncmp(rel, "wallpapers/", 10) == 0) {
        const char *wd = getenv("ONEWM_WALLPAPER_DIR");
        if (wd) {
            snprintf(path, sizeof(path), "%s/%s", wd, rel + 10);
            if (access(path, R_OK) == 0)
                return path;
        }
    }

    const char *env = getenv("ONEWM_DATA_DIR");
    if (env) {
        snprintf(path, sizeof(path), "%s/%s", env, rel);
        if (access(path, R_OK) == 0) return path;
    }
    snprintf(path, sizeof(path), "data/%s", rel);
    if (access(path, R_OK) == 0) return path;
    snprintf(path, sizeof(path), "themes/%s", rel);
    if (access(path, R_OK) == 0) return path;
    snprintf(path, sizeof(path), "../themes/%s", rel);
    if (access(path, R_OK) == 0) return path;
    snprintf(path, sizeof(path), "/usr/share/onewm/%s", rel);
    if (access(path, R_OK) == 0) return path;
    return NULL;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    if (len) *len = (size_t)sz;
    return buf;
}

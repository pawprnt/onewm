#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <cairo-ft.h>
#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

/* Version string: v1.yy.mm.dd.hh baked in at compile time via __DATE__/__TIME__. */
static void boot_version_string(char *out, size_t n) {
    static const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
    char mon[4];
    int day = 0, year = 0, hh = 0, mo = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3 &&
        sscanf(__TIME__, "%d", &hh) == 1) {
        for (int i = 0; i < 12; i++)
            if (strncmp(mon, mons[i], 3) == 0) { mo = i + 1; break; }
        snprintf(out, n, "v1.%02d.%02d.%02d.%02d",
                 year % 100, mo, day, hh);
        return;
    }
    snprintf(out, n, "v1.00.00.00.00");
}
/* Title (the_world_machine logo) timing, matched to the game's boot frames at
   60fps (OneShotMG.decompiled.cs:6754-6760):
     BREAK_BEFORE_BIG_LOGO  60f = 1.0s black before the logo
     TWM_LOGO_TIME_BEFORE_... 60f = 1.0s bottom->top wipe in
     TWM_LOGO_TIME_AFTER_... 120f = 2.0s hold at full
     TWM_LOGO_FADEOUT_TIME 120f = 2.0s top->bottom wipe out */
#define BOOT_LOGO_BREAK 1.0
#define BOOT_FADE_IN     1.0
#define BOOT_TITLE_HOLD  2.0
#define BOOT_FADE_OUT    2.0
#define SOUND_LINE_DELAY 0.05
#define TEXTURE_LINE_DELAY 0.08

/* Boot is authored in a 960x540 design space (matching the game's
   DrawScreenSize/2 with scale=2). Coordinates below are in this space and
   upscaled to the real output by draw_frame. */
#define BOOT_DW 960.0
#define BOOT_DH 540.0
#define BOOT_FONT_DESIGN 28.0   /* terminus size in design px (tuned to game) */
#define BOOT_LINE 27.0          /* CONSOLE text line spacing (~5px more than 22) */
#define BOOT_TEXT_X 10.0
#define BOOT_TEXT_Y 150.0       /* lowered to clear the larger TWM logo */
#define BOOT_LOGO_SCALE 1.4     /* multiplier on top of the cairo us scale */
#define BOOT_VER_X 10.0 /* reserved */
#define BOOT_VER_Y 4.0
#define BOOT_PR 161.0
#define BOOT_PG 91.0
#define BOOT_PB 255.0

/* Ordered 8x8 Bayer matrix (values 0..63), matching the game's
   transitions/BayerDither8x8 texture used by the DitherShader
   (OneShotMG.decompiled.cs:38289, 38805-38814, tiled at scale=4). */
static const int boot_bayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};
#define BOOT_DITHER_CELL 4      /* device px per bayer cell (game scale=4) */
/* How much the reveal is biased to sweep bottom->top. 0 = pure uniform dither
   (fills everywhere at once), 1 = pure vertical wipe. The game's logo reveal
   reads as a dithered sweep from the bottom upward. */
#define BOOT_SWEEP 0.6

typedef struct {
    double t;
    uint32_t cp;
    int kind;   /* 0 = typed char, 1 = sound load step, 2 = texture load step */
    int idx;    /* index into sound_names / tex_names for load steps */
} reveal_t;

#define KIND_CHAR  0
#define KIND_SOUND 1
#define KIND_TEX   2

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_callback *frame_cb;
    struct wl_buffer *buffers[2];
    void *map[2];
    size_t size[2];
    int buf_w, buf_h;       /* size the buffers were allocated for */
    int buf_idx;
    int32_t width, height;
    bool configured, running;
    uint32_t seat_caps;
    struct wl_keyboard *keyboard;
} ctx_t;

typedef struct {
    reveal_t *events;
    size_t n, capacity, visible;
    double total_time;
    int sounds, textures;
    double sound_t0, sound_t1;  /* time window of the sound-load phase */
    double tex_t0, tex_t1;      /* time window of the texture-load phase */
} timeline_t;

typedef struct {
    double primary_r, primary_g, primary_b;
    cairo_surface_t *logo, *logo_full;
    PangoFontDescription *body_font, *small_font;
    char *body_text;
    char **sound_names; int sound_count;
    char **tex_names;   int tex_count;
    timeline_t tl;
    double start_time, now, title_start;
    int phase;
    int skips;
    int logo_sfx_played;
} scene_t;

static ctx_t ctx;
static scene_t scene;

static double now_sec(void);
static bool create_buffers(void);
static void recreate_buffers(void);

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static char *find_data(const char *rel) {
    static char path[4096];
    const char *env = getenv("ONEWM_DATA_DIR");
    if (env) {
        snprintf(path, sizeof(path), "%s/%s", env, rel);
        if (access(path, R_OK) == 0) return path;
    }
    snprintf(path, sizeof(path), "data/%s", rel);
    if (access(path, R_OK) == 0) return path;
    snprintf(path, sizeof(path), "/usr/share/onewm/%s", rel);
    if (access(path, R_OK) == 0) return path;
    return NULL;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    if (len) *len = sz;
    return buf;
}

/* Read a file into an array of strdup'd lines (no trailing empty lines). */
static char **read_lines(const char *path, int *n) {
    char *data = read_file(path, NULL);
    if (!data) return NULL;
    char **out = NULL;
    *n = 0;
    char *save = NULL;
    for (char *ln = strtok_r(data, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        if (ln[0] == '\0') continue;
        out = realloc(out, (*n + 1) * sizeof(char *));
        out[*n] = strdup(ln);
        (*n)++;
    }
    free(data);
    return out;
}

static int endswith_ci(const char *s, const char *ext) {
    size_t ls = strlen(s), le = strlen(ext);
    if (ls < le) return 0;
    return strcasecmp(s + ls - le, ext) == 0;
}

/* Collect basenames of files in `dir` ending with `ext` (case-insensitive).
   Returns a malloc'd array of strdup'd names and sets *n. */
static char **scan_dir(const char *dir, const char *ext, int *n) {
    char **out = NULL;
    *n = 0;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!endswith_ci(de->d_name, ext)) continue;
        out = realloc(out, (*n + 1) * sizeof(char *));
        out[*n] = strdup(de->d_name);
        (*n)++;
    }
    closedir(d);
    return out;
}

/* Recursively scan onewm's own data tree for image files and build display
   names in the game's "the_world_machine/<name>" form (without extension). */
static void scan_textures_recursive(const char *dir, char ***out, int *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    static const char *exts[] = {".png", ".jpg", ".jpeg", ".xnb", ".svg", ".webp"};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            scan_textures_recursive(path, out, n);
            continue;
        }
        int ok = 0;
        for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
            if (endswith_ci(de->d_name, exts[i])) { ok = 1; break; }
        if (!ok) continue;
        char base[1024];
        snprintf(base, sizeof(base), "%s", de->d_name);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        char disp[1024];
        snprintf(disp, sizeof(disp), "the_world_machine/%s", base);
        *out = realloc(*out, (*n + 1) * sizeof(char *));
        (*out)[*n] = strdup(disp);
        (*n)++;
    }
    closedir(d);
}

static void tl_push(timeline_t *tl, double t, uint32_t cp) {
    if (tl->n == tl->capacity) {
        tl->capacity = tl->capacity ? tl->capacity * 2 : 4096;
        tl->events = realloc(tl->events, tl->capacity * sizeof(reveal_t));
    }
    tl->events[tl->n++] = (reveal_t){t, cp, KIND_CHAR, 0};
}

static void tl_push_load(timeline_t *tl, double t, int kind, int idx) {
    if (tl->n == tl->capacity) {
        tl->capacity = tl->capacity ? tl->capacity * 2 : 4096;
        tl->events = realloc(tl->events, tl->capacity * sizeof(reveal_t));
    }
    tl->events[tl->n++] = (reveal_t){t, 0, kind, idx};
}

static void tl_append_text(timeline_t *tl, double *t, const char *s, double per_char) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        tl_push(tl, *t, *p);
        *t += per_char;
    }
}

static void timeline_build(timeline_t *tl) {
    memset(tl, 0, sizeof(*tl));
    double t = 0.0;

    /* Expanded body text, built in lockstep with the timeline so that every
       pushed character has a matching character here. This is what gets drawn,
       so the console shows the real (expanded) lines and the reveal offset
       maps 1:1 onto it. */
    size_t ecap = 4096, elen = 0;
    char *ebuf = malloc(ecap);
    ebuf[0] = '\0';
    #define EBUF_ENSURE(n) do { if (elen + (size_t)(n) + 1 > ecap) { ecap = (elen + (size_t)(n) + 1) * 2; ebuf = realloc(ebuf, ecap); } } while (0)
    #define EBUF_PUTC(c) do { EBUF_ENSURE(1); ebuf[elen++] = (char)(c); ebuf[elen] = '\0'; } while (0)
    #define EBUF_PUTS(s) do { size_t _l = strlen(s); EBUF_ENSURE(_l); memcpy(ebuf + elen, s, _l); elen += _l; ebuf[elen] = '\0'; } while (0)

    /* Resolve the sound/texture asset lists up front (the game knows these
       counts before it starts typing), so @SOUNDS can be substituted anywhere.
       Prefer a static list shipped in data/boot, else scan onewm's own folders. */
    {
        const char *list = find_data("boot/sounds.list");
        if (list) scene.sound_names = read_lines(list, &scene.sound_count);
        if (!scene.sound_names) {
            const char *sd = find_data("sfx");
            if (sd) scene.sound_names = scan_dir(sd, ".wav", &scene.sound_count);
        }
        const char *tlist = find_data("boot/textures.list");
        if (tlist) scene.tex_names = read_lines(tlist, &scene.tex_count);
        if (!scene.tex_names) {
            const char *dd = find_data("");
            scene.tex_count = 0;
            if (dd) scan_textures_recursive(dd, &scene.tex_names, &scene.tex_count);
        }
    }

    const char *files[] = { "boot/bios.txt", "boot/bios2.txt", "boot/bios3.txt" };
    int pending_sound = 0, pending_tex = 0;
    for (size_t i = 0; i < 3; i++) {
        const char *p = find_data(files[i]);
        if (!p) continue;
        char *src = read_file(p, NULL);
        if (!src) continue;

        for (char *c = src; *c;) {
            if (*c == '@') {
                if (strncmp(c, "@WAIT", 5) == 0) {
                    /* Match the game: read the number, drop the single space that
                       follows it, and preserve any newlines that come after (so
                       lines aren't merged). */
                    char *p = c + 5;
                    if (*p == ' ') p++;
                    double d = strtod(p, &p);
                    t += d;
                    c = p;
                    if (*c == ' ') c++;
                    continue;
                }
                if (strncmp(c, "@SCANSOUNDS", 11) == 0) {
                    /* Stripped from the displayed text (as in the game); the
                       actual sound-loading phase is appended after this file. */
                    pending_sound = 1;
                    c += 11;
                    continue;
                }
                if (strncmp(c, "@SCANTEXTURES", 13) == 0) {
                    pending_tex = 1;
                    c += 13;
                    continue;
                }
                if (strncmp(c, "@SOUNDS", 7) == 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d", scene.sound_count);
                    tl_append_text(tl, &t, buf, 0.0);
                    EBUF_PUTS(buf);
                    c += 7;
                    continue;
                }
            }
            tl_push(tl, t, (unsigned char)*c);
            t += 0.012;
            EBUF_PUTC(*c);
            c++;
        }
        /* Append the load phase(s) for this file after its text has fully typed,
           mirroring the game (loading follows the whole bios chunk). */
        if (pending_sound) {
            tl->sounds = scene.sound_count;
            tl->sound_t0 = t;
            for (int k = 0; k < scene.sound_count; k++) {
                tl_push_load(tl, t, KIND_SOUND, k);
                t += SOUND_LINE_DELAY;
            }
            tl->sound_t1 = t;
            pending_sound = 0;
        }
        if (pending_tex) {
            tl->textures = scene.tex_count;
            tl->tex_t0 = t;
            for (int k = 0; k < scene.tex_count; k++) {
                tl_push_load(tl, t, KIND_TEX, k);
                t += TEXTURE_LINE_DELAY;
            }
            tl->tex_t1 = t;
            pending_tex = 0;
        }
        free(src);
    }
    #undef EBUF_ENSURE
    #undef EBUF_PUTC
    #undef EBUF_PUTS
    scene.body_text = ebuf;
    tl->total_time = t;
}

static void load_theme_color(void) {
    scene.primary_r = 150.0 / 255.0;
    scene.primary_g = 100.0 / 255.0;
    scene.primary_b = 1.0;

    const char *p = find_data("themes_metadata.json");
    if (!p) return;
    char *json = read_file(p, NULL);
    if (!json) return;

    char *prim = strstr(json, "\"primary\"");
    if (prim) {
        char *obj = strchr(prim, '{');
        char *end = obj ? strchr(obj, '}') : NULL;
        if (obj && end) {
            char *r = strstr(obj, "\"r\"");
            char *g = strstr(obj, "\"g\"");
            char *b = strstr(obj, "\"b\"");
            int rv = -1, gv = -1, bv = -1;
            if (r && r < end) sscanf(strchr(r, ':'), ": %d", &rv);
            if (g && g < end) sscanf(strchr(g, ':'), ": %d", &gv);
            if (b && b < end) sscanf(strchr(b, ':'), ": %d", &bv);
            if (rv >= 0) scene.primary_r = rv / 255.0;
            if (gv >= 0) scene.primary_g = gv / 255.0;
            if (bv >= 0) scene.primary_b = bv / 255.0;
        }
    }
    free(json);
}

static void load_assets(void) {
    const char *p = find_data("boot/logo.png");
    if (p) scene.logo = cairo_image_surface_create_from_png(p);
    p = find_data("boot/logo_full.png");
    if (p) scene.logo_full = cairo_image_surface_create_from_png(p);

    const char *fonts_dir = find_data("fonts");
    if (fonts_dir) {
        DIR *d = opendir(fonts_dir);
        if (d) {
            struct dirent *ent;
            FcConfig *fc = FcConfigGetCurrent();
            while ((ent = readdir(d))) {
                size_t n = strlen(ent->d_name);
                if (n > 4 && strcasecmp(ent->d_name + n - 4, ".ttf") == 0) {
                    char path[4096];
                    snprintf(path, sizeof(path), "%s/%s", fonts_dir, ent->d_name);
                    FcConfigAppFontAddFile(fc, (const FcChar8 *)path);
                }
            }
            closedir(d);
        }
    }

    /* Boot is authored in a 960x540 design space; the engine's scale=2 is the
       design->window upscale. Draw assets at native size (the cairo scale below
       applies the x2). Font size here is in DESIGN px (= baked terminus). */
    scene.body_font = pango_font_description_new();
    pango_font_description_set_family(scene.body_font, "Terminus (TTF)");
    pango_font_description_set_absolute_size(scene.body_font, BOOT_FONT_DESIGN * PANGO_SCALE);

    scene.small_font = pango_font_description_new();
    pango_font_description_set_family(scene.small_font, "Terminus (TTF)");
    pango_font_description_set_absolute_size(scene.small_font, BOOT_FONT_DESIGN * PANGO_SCALE);
}

static void play_sfx(const char *rel) {
    const char *p = find_data(rel);
    if (!p) return;
    pid_t pid = fork();
    if (pid == 0) {
        execlp("aplay", "aplay", "-q", p, (char *)NULL);
        _exit(127);
    }
}

static void draw_version(cairo_t *cr, int W, double us) {
    char ver[64];
    boot_version_string(ver, sizeof(ver));
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_set_source_rgb(cr, BOOT_PR / 255.0, BOOT_PG / 255.0, BOOT_PB / 255.0);
    PangoLayout *vlayout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, "Terminus (TTF)");
    pango_font_description_set_absolute_size(fd, BOOT_FONT_DESIGN * us * PANGO_SCALE);
    pango_layout_set_font_description(vlayout, fd);
    pango_layout_set_text(vlayout, ver, -1);
    PangoRectangle ext;
    pango_layout_get_pixel_extents(vlayout, NULL, &ext);
    double vx = (double)W - (double)ext.width - 10.0;
    cairo_move_to(cr, vx, 10.0);
    pango_cairo_show_layout(cr, vlayout);
    g_object_unref(vlayout);
    pango_font_description_free(fd);
    cairo_restore(cr);
}

static void draw_frame(cairo_t *cr) {
    /* Fill the screen height with the 960x540 design, top-left anchored (like a
       BIOS console). This fills vertically on any aspect and keeps all
       left-anchored content visible instead of cropping it. The version string
       is drawn separately in screen space so it always sits top-right. */
    double us = ctx.height > 0 ? ctx.height / BOOT_DH : 1.0;
    cairo_scale(cr, us, us);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    if (scene.phase == 1) {
        double t = scene.now - scene.title_start;
        double tin = BOOT_LOGO_BREAK;
        double tend = BOOT_LOGO_BREAK + BOOT_FADE_IN + BOOT_TITLE_HOLD + BOOT_FADE_OUT;
        double alpha = 0.0;
        if (t < tin)
            alpha = 0.0;                                  /* black break */
        else if (t < tin + BOOT_FADE_IN)
            alpha = (t - tin) / BOOT_FADE_IN;             /* dither reveal in */
        else if (t < tin + BOOT_FADE_IN + BOOT_TITLE_HOLD)
            alpha = 1.0;                                  /* hold */
        else if (t < tend)
            alpha = 1.0 - (t - (tin + BOOT_FADE_IN + BOOT_TITLE_HOLD)) / BOOT_FADE_OUT;
        else
            alpha = 0.0;                                  /* dither reveal out */
        if (alpha < 0) alpha = 0;
        if (alpha > 1) alpha = 1;

        /* The logo reveals via an ordered-Bayer DITHER (DitherShader +
           transitions/BayerDither8x8) whose threshold sweeps BOTTOM -> TOP:
           a pixel shows only while its dither threshold <= opacity, and the
           threshold is biased so the bottom of the screen fills first and the
           line rises. On exit (opacity 1->0) the top clears first, reading as a
           TOP -> BOTTOM reveal toward the (future) desktop. The logo is
           cover-fit against the full window (OneShotMG.decompiled.cs:7666-7687).
           We build a full-screen A8 mask each frame combining the tiled bayer
           pattern with the vertical sweep. */
        if (scene.logo_full) {
            int iw = cairo_image_surface_get_width(scene.logo_full);
            int ih = cairo_image_surface_get_height(scene.logo_full);
            /* Cover: fill the whole screen, cropping the longer axis. */
            double scale = fmax((double)ctx.width / (double)iw,
                               (double)ctx.height / (double)ih);
            double x = (ctx.width - iw * scale) / 2.0;
            double y = (ctx.height - ih * scale) / 2.0;

            int W = ctx.width, H = ctx.height;
            cairo_surface_t *msk = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
            unsigned char *md = cairo_image_surface_get_data(msk);
            int mstride = cairo_image_surface_get_stride(msk);
            double invc = 1.0 / (double)BOOT_DITHER_CELL;
            for (int yy = 0; yy < H; yy++) {
                double v = 1.0 - (double)yy / (double)H;   /* 0 bottom .. 1 top */
                int cy = ((yy / BOOT_DITHER_CELL) % 8 + 8) % 8;
                unsigned char *row = md + (size_t)yy * mstride;
                for (int xx = 0; xx < W; xx++) {
                    int cx = ((xx / BOOT_DITHER_CELL) % 8 + 8) % 8;
                    double bayer = (2.0 * boot_bayer8[cy][cx] + 1.0) / 128.0;
                    double local = bayer * (1.0 - BOOT_SWEEP) + v * BOOT_SWEEP;
                    row[xx] = (local <= alpha) ? 255 : 0;
                }
            }
            cairo_surface_mark_dirty(msk);
            cairo_pattern_t *maskpat = cairo_pattern_create_for_surface(msk);
            cairo_pattern_set_extend(maskpat, CAIRO_EXTEND_PAD);

            cairo_save(cr);
            cairo_identity_matrix(cr);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_paint(cr);

            /* Render the cover-fit logo onto a temp surface, then mask it. */
            cairo_surface_t *tmp = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, W, H);
            cairo_t *tc = cairo_create(tmp);
            cairo_translate(tc, x, y);
            cairo_scale(tc, scale, scale);
            cairo_set_source_surface(tc, scene.logo_full, 0, 0);
            cairo_paint(tc);
            cairo_destroy(tc);

            cairo_set_source_surface(cr, tmp, 0, 0);
            cairo_mask(cr, maskpat);
            cairo_restore(cr);

            cairo_surface_destroy(tmp);
            cairo_pattern_destroy(maskpat);
            cairo_surface_destroy(msk);
        }
        /* The version string belongs to the BIOS console (phase 0) only; hide it
           once the title logo reveal begins, matching the game. */
        return;
    }

    /* Phase 0: TWM logo at top-left. Game draws it at scale 2 (x2 via cairo
       us); bump it a bit bigger with BOOT_LOGO_SCALE, anchored top-left. */
    if (scene.logo) {
        cairo_save(cr);
        cairo_scale(cr, BOOT_LOGO_SCALE, BOOT_LOGO_SCALE);
        cairo_set_source_surface(cr, scene.logo, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    draw_version(cr, ctx.width, us);

    /* The BIOS console text uses the theme purple (draw_version restored the
       cairo source to black via its save/restore, so re-set it here). */
    cairo_set_source_rgb(cr, BOOT_PR / 255.0, BOOT_PG / 255.0, BOOT_PB / 255.0);

    /* BIOS console body text. The timeline is revealed character-by-character.
       During the sound/texture load phases the game shows the CURRENT item on
       the last console line (postText[count-1] = NextSoundToLoad()), so we do
       the same: type the revealed characters, and if we are currently inside a
       load-phase time window, swap the final line for the active item. */
    size_t vis = scene.tl.visible;
    if (vis > scene.tl.n) vis = scene.tl.n;
    if (vis > 0) {
        char *typed = malloc(scene.tl.n + 512);
        size_t tlen = 0;
        for (size_t e = 0; e < vis; e++) {
            if (scene.tl.events[e].kind == KIND_CHAR)
                typed[tlen++] = (char)scene.tl.events[e].cp;
        }
        typed[tlen] = '\0';

        /* Which load phase (if any) is active at the current boot time?
           The game cycles the load list in its own order; mirror it here by
           walking the list from the END backward (matching the game's scan). */
        double t = scene.now - scene.start_time;
        int cur_kind = 0, cur_idx = -1;
        if (scene.tl.sounds > 0 && t >= scene.tl.sound_t0 && t <= scene.tl.sound_t1) {
            cur_kind = KIND_SOUND;
            cur_idx = scene.tl.sounds - 1 - (int)((t - scene.tl.sound_t0) / SOUND_LINE_DELAY);
            if (cur_idx < 0) cur_idx = 0;
        } else if (scene.tl.textures > 0 && t >= scene.tl.tex_t0 && t <= scene.tl.tex_t1) {
            cur_kind = KIND_TEX;
            cur_idx = scene.tl.textures - 1 - (int)((t - scene.tl.tex_t0) / TEXTURE_LINE_DELAY);
            if (cur_idx < 0) cur_idx = 0;
        }

        /* Split the typed text into individual lines. */
        char **lines = NULL;
        int cap = 0, n = 0;
        char *start = typed;
        for (char *p = typed; ; p++) {
            if (*p == '\n' || *p == '\0') {
                char saved = *p;
                *p = '\0';
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    lines = realloc(lines, cap * sizeof(char *));
                }
                lines[n++] = start;
                if (saved == '\0') break;
                start = p + 1;
            }
        }
        /* Drop trailing whitespace-only lines so the final *content* line is the
           one we replace during a load phase (instead of a blank trailing line). */
        while (n > 0) {
            const char *s = lines[n - 1];
            int only_ws = 1;
            for (const char *q = s; *q; q++)
                if (*q != ' ' && *q != '\t' && *q != '\r') { only_ws = 0; break; }
            if (!only_ws) break;
            n--;
        }

        /* During a load phase the game shows the current item on the final
           console line (postText[count-1] = NextSoundToLoad()); replace the last
           line with it so we never render a growing list. */
        if (cur_kind != 0 && cur_idx >= 0 && n > 0) {
            const char *name = (cur_kind == KIND_SOUND)
                                  ? scene.sound_names[cur_idx]
                                  : scene.tex_names[cur_idx];
            char disp[256];
            if (cur_kind == KIND_TEX) {
                const char *p = name;
                if (strncmp(p, "the_world_machine/", 18) == 0) p += 18;
                snprintf(disp, sizeof(disp), "%s.xnb", p);
            } else {
                snprintf(disp, sizeof(disp), "%s", name);
            }
            lines[n - 1] = disp;
        }

        /* After a load phase the game clears the "Loading ..." header line
           (postText[count-1] = string.Empty); blank those lines if the phase
           that owned them has finished. */
        if (t > scene.tl.sound_t1) {
            for (int i = 0; i < n; i++)
                if (strcmp(lines[i], "Loading sounds:") == 0) lines[i] = (char *)"";
        }
        if (t > scene.tl.tex_t1) {
            for (int i = 0; i < n; i++)
                if (strcmp(lines[i], "Loading system textures:") == 0) lines[i] = (char *)"";
        }

        int fit = (int)((BOOT_DH - BOOT_TEXT_Y - 4) / BOOT_LINE);
        int first = (n > fit) ? n - fit : 0;
        for (int i = first; i < n; i++) {
            PangoLayout *bl = pango_cairo_create_layout(cr);
            pango_layout_set_font_description(bl, scene.body_font);
            pango_layout_set_text(bl, lines[i], -1);
            cairo_move_to(cr, BOOT_TEXT_X,
                          BOOT_TEXT_Y + (i - first) * BOOT_LINE);
            pango_cairo_show_layout(cr, bl);
            g_object_unref(bl);
        }
        free(lines);
        free(typed);
    }
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { frame_done };

static void redraw(void) {
    int idx = ctx.buf_idx;
    /* Guarantee the backing buffer is fully cleared (no stale-frame ghosting
       left behind if the draw surface is ever smaller than the buffer). */
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
    memset(ctx.map[idx], 0, (size_t)stride * ctx.height);
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        ctx.map[idx], CAIRO_FORMAT_ARGB32, ctx.width, ctx.height, stride);
    cairo_t *cr = cairo_create(surf);

    draw_frame(cr);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    wl_surface_attach(ctx.surface, ctx.buffers[idx], 0, 0);
    wl_surface_damage_buffer(ctx.surface, 0, 0, INT32_MAX, INT32_MAX);
    if (ctx.frame_cb) wl_callback_destroy(ctx.frame_cb);
    ctx.frame_cb = wl_surface_frame(ctx.surface);
    wl_callback_add_listener(ctx.frame_cb, &frame_listener, NULL);
    wl_surface_commit(ctx.surface);
    ctx.buf_idx = 1 - ctx.buf_idx;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data; (void)time;
    if (getenv("ONEWM_BOOT_DEBUG")) {
        static int frames = 0;
        if (frames++ < 3 || frames % 120 == 0) {
            fprintf(stderr, "onewm-boot: frame %d\n", frames);
        }
    }
    if (cb) wl_callback_destroy(cb);
    ctx.frame_cb = NULL;
    if (!ctx.running) return;

    scene.now = now_sec();

    if (scene.phase == 0) {
        double t = scene.now - scene.start_time;
        while (scene.tl.visible < scene.tl.n &&
               scene.tl.events[scene.tl.visible].t <= t) {
            scene.tl.visible++;
        }
        if (getenv("ONEWM_BOOT_DEBUG")) {
            static double last_dbg = -1;
            if (t - last_dbg > 2.0) {
                last_dbg = t;
                fprintf(stderr, "onewm-boot: t=%.1f visible=%zu/%zu\n",
                        t, scene.tl.visible, scene.tl.n);
            }
        }
        if (scene.tl.visible >= scene.tl.n) {
            scene.phase = 1;
            scene.title_start = scene.now;
            scene.logo_sfx_played = 0;
        }
    } else if (scene.phase == 1) {
        double t = scene.now - scene.title_start;
        double tend = BOOT_LOGO_BREAK + BOOT_FADE_IN + BOOT_TITLE_HOLD + BOOT_FADE_OUT;
        /* Game plays twm_startup at stateTimer==30, i.e. halfway through the
           reveal (OneShotMG.decompiled.cs:7313-7316). */
        if (!scene.logo_sfx_played && t >= BOOT_LOGO_BREAK + BOOT_FADE_IN / 2.0) {
            play_sfx("sfx/twm_startup.wav");
            scene.logo_sfx_played = 1;
        }
        if (t > tend) {
            ctx.running = false;
            redraw();
            return;
        }
    }
    redraw();
}

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial,
                                    uint32_t w, uint32_t h) {
    (void)data;
    if (getenv("ONEWM_BOOT_DEBUG"))
        fprintf(stderr, "onewm-boot: configure %ux%u\n", w, h);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if ((int32_t)w > 0) ctx.width = w;
    if ((int32_t)h > 0) ctx.height = h;
    /* If the surface size changed after buffers were allocated, recreate them so
       the cairo surface always covers the full SHM buffer (otherwise clearing
       leaves a ghost of the previous frame -> "duplicated" text). */
    if (ctx.buffers[0] != NULL &&
        (ctx.buf_w != (int)ctx.width || ctx.buf_h != (int)ctx.height)) {
        recreate_buffers();
    }
    if (!ctx.configured) {
        ctx.configured = true;
        if (ctx.buffers[0] == NULL && !create_buffers()) {
            fprintf(stderr, "onewm-boot: buffer alloc failed\n");
            ctx.running = false;
            return;
        }
        scene.start_time = now_sec();
        scene.now = scene.start_time;
    }
    redraw();
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)data; (void)ls;
    ctx.running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void keyboard_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { (void)d;(void)k;(void)f;close(fd);(void)sz; }
static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) { (void)d;(void)k;(void)s;(void)sf;(void)keys; }
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d;(void)k;(void)s;(void)sf; }
static void keyboard_modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lck, uint32_t grp) { (void)d;(void)k;(void)s;(void)dep;(void)lat;(void)lck;(void)grp; }
static void keyboard_repeat_info(void *d, struct wl_keyboard *k, int32_t r, int32_t dly) { (void)d;(void)k;(void)r;(void)dly; }

static void keyboard_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)s; (void)t;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    scene.skips++;
    if (scene.phase == 0 && scene.skips == 1) {
        scene.tl.visible = scene.tl.n;
        scene.phase = 1;
        scene.title_start = now_sec();
        play_sfx("sfx/twm_startup.wav");
        scene.logo_sfx_played = 1;
    } else {
        ctx.running = false;
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    ctx.seat_caps = caps;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ctx.keyboard) {
        ctx.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ctx.keyboard, &keyboard_listener, NULL);
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

static void shm_format(void *d, struct wl_shm *shm, uint32_t format) { (void)d;(void)shm;(void)format; }
static const struct wl_shm_listener shm_listener = { .format = shm_format };

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        ctx.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        ctx.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        wl_shm_add_listener(ctx.shm, &shm_listener, NULL);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        ctx.seat = wl_registry_bind(reg, name, &wl_seat_interface, 7);
        wl_seat_add_listener(ctx.seat, &seat_listener, NULL);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        ctx.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    }
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void recreate_buffers(void) {
    for (int i = 0; i < 2; i++) {
        if (ctx.buffers[i]) wl_buffer_destroy(ctx.buffers[i]);
        if (ctx.map[i]) munmap(ctx.map[i], ctx.size[i]);
        ctx.buffers[i] = NULL;
        ctx.map[i] = NULL;
    }
    create_buffers();
}

static bool create_buffers(void) {
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
    size_t sz = (size_t)stride * ctx.height;
    ctx.buf_w = ctx.width;
    ctx.buf_h = ctx.height;

    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize < 1) pagesize = 4096;
    size_t sz_aligned = (sz + pagesize - 1) & ~(size_t)(pagesize - 1);
    size_t total = sz_aligned * 2;

    char name[] = "/onewm-boot-XXXXXX";
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, total) < 0) { close(fd); return false; }

    for (int i = 0; i < 2; i++) {
        ctx.map[i] = mmap(NULL, sz_aligned, PROT_READ | PROT_WRITE, MAP_SHARED, fd, i * sz_aligned);
        if (ctx.map[i] == MAP_FAILED) { close(fd); return false; }
        ctx.size[i] = sz_aligned;
        memset(ctx.map[i], 0, sz);

        struct wl_shm_pool *pool = wl_shm_create_pool(ctx.shm, fd, total);
        ctx.buffers[i] = wl_shm_pool_create_buffer(pool, i * sz_aligned,
                                                   ctx.width, ctx.height,
                                                   stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    return true;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    memset(&ctx, 0, sizeof(ctx));
    memset(&scene, 0, sizeof(scene));
    ctx.width = 1280;
    ctx.height = 720;

    load_theme_color();
    timeline_build(&scene.tl);

    const char *bp = find_data("boot/bios.txt");
    if (!bp) {
        fprintf(stderr, "onewm-boot: cannot find data dir (set ONEWM_DATA_DIR)\n");
        return 1;
    }

    /* body_text (expanded) is built by timeline_build() above. */

    if (getenv("ONEWM_BOOT_DUMP")) {
        fprintf(stderr, "=== body_text ===\n%s\n=== end ===\n",
                scene.body_text ? scene.body_text : "(null)");
        fprintf(stderr, "timeline events=%zu total_time=%.2f\n",
                scene.tl.n, scene.tl.total_time);
        return 0;
    }

    if (getenv("ONEWM_BOOT_PNG")) {
        int W = 1920, H = 1080;
        const char *bw = getenv("ONEWM_BOOT_W");
        const char *bh = getenv("ONEWM_BOOT_H");
        if (bw) W = atoi(bw);
        if (bh) H = atoi(bh);
        ctx.width = W; ctx.height = H;
        load_assets();
        scene.phase = 0;
        scene.start_time = 0;
        const char *ts = getenv("ONEWM_BOOT_T");
        double tt = ts ? atof(ts) : 8.0;
        scene.now = tt;
        while (scene.tl.visible < scene.tl.n &&
               scene.tl.events[scene.tl.visible].t <= scene.now - scene.start_time)
            scene.tl.visible++;
        /* Drive the phase-0 -> phase-1 transition so the title reveal can be
           exercised headlessly (normally done by the step loop). */
        if (scene.phase == 0 && scene.tl.visible >= scene.tl.n) {
            scene.phase = 1;
            scene.title_start = scene.start_time + scene.tl.total_time;
        }
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(surf);
        draw_frame(cr);
        cairo_surface_write_to_png(surf, "/tmp/boot_actual.png");
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
        fprintf(stderr, "wrote /tmp/boot_actual.png t=%.1f visible=%zu/%zu\n",
                tt, scene.tl.visible, scene.tl.n);
        return 0;
    }

    ctx.display = wl_display_connect(NULL);
    if (!ctx.display) {
        fprintf(stderr, "onewm-boot: no wayland compositor (WAYLAND_DISPLAY?)\n");
        return 1;
    }
    ctx.registry = wl_display_get_registry(ctx.display);
    wl_registry_add_listener(ctx.registry, &registry_listener, NULL);
    wl_display_roundtrip(ctx.display);

    if (!ctx.compositor || !ctx.shm || !ctx.layer_shell) {
        fprintf(stderr, "onewm-boot: missing compositor/shm/layer-shell\n");
        return 1;
    }

    ctx.surface = wl_compositor_create_surface(ctx.compositor);
    ctx.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ctx.layer_shell, ctx.surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "onewm-boot");
    zwlr_layer_surface_v1_set_anchor(ctx.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(ctx.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        ctx.layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(ctx.layer_surface,
                                       &layer_surface_listener, NULL);
    wl_surface_commit(ctx.surface);

    load_assets();

    ctx.running = true;
    while (ctx.running && wl_display_dispatch(ctx.display) != -1) {
    }

    if (ctx.frame_cb) wl_callback_destroy(ctx.frame_cb);
    if (ctx.keyboard) wl_keyboard_release(ctx.keyboard);

    for (int i = 0; i < 2; i++) {
        if (ctx.map[i]) munmap(ctx.map[i], ctx.size[i]);
        if (ctx.buffers[i]) wl_buffer_destroy(ctx.buffers[i]);
    }
    if (ctx.layer_surface) zwlr_layer_surface_v1_destroy(ctx.layer_surface);
    if (ctx.surface) wl_surface_destroy(ctx.surface);
    if (ctx.display) wl_display_disconnect(ctx.display);
    return 0;
}

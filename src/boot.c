#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define BOOT_VERSION "v1.24.12.22.8"
#define TITLE_HOLD 4.0
#define FADE 0.5
#define SOUND_LINE_DELAY 0.05
#define TEXTURE_LINE_DELAY 0.08

/* Boot is authored in a 960x540 design space (matching the game's
   DrawScreenSize/2 with scale=2). Coordinates below are in this space and
   upscaled to the real output by draw_frame. */
#define BOOT_DW 960.0
#define BOOT_DH 540.0
#define BOOT_FONT_DESIGN 16.0   /* baked terminus size in design px */
#define BOOT_LINE 22.0          /* CONSOLE text line spacing (game LINE_HEIGHT) */
#define BOOT_TEXT_X 10.0
#define BOOT_TEXT_Y 100.0
#define BOOT_PR 161.0
#define BOOT_PG 91.0
#define BOOT_PB 255.0

typedef struct {
    double t;
    uint32_t cp;
} reveal_t;

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
} timeline_t;

typedef struct {
    double primary_r, primary_g, primary_b;
    cairo_surface_t *logo, *logo_full;
    PangoFontDescription *body_font, *small_font;
    char *body_text;
    timeline_t tl;
    double start_time, now, title_start;
    int phase;
    int skips;
} scene_t;

static ctx_t ctx;
static scene_t scene;

static double now_sec(void);
static bool create_buffers(void);

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

static int count_lines(const char *path) {
    char *data = read_file(path, NULL);
    if (!data) return 0;
    int n = 0;
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) { n++; p = nl + 1; } else { n++; break; }
    }
    free(data);
    return n;
}

static void tl_push(timeline_t *tl, double t, uint32_t cp) {
    if (tl->n == tl->capacity) {
        tl->capacity = tl->capacity ? tl->capacity * 2 : 4096;
        tl->events = realloc(tl->events, tl->capacity * sizeof(reveal_t));
    }
    tl->events[tl->n++] = (reveal_t){t, cp};
}

static void tl_append_text(timeline_t *tl, double *t, const char *s, double per_char) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        tl_push(tl, *t, *p);
        *t += per_char;
    }
}

static void tl_append_file_lines(timeline_t *tl, double *t, const char *path,
                                 double per_line) {
    char *data = read_file(path, NULL);
    if (!data) return;
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        tl_append_text(tl, t, line, 0.0);
        tl_push(tl, *t, '\n');
        *t += per_line;
    }
    free(data);
}

static void timeline_build(timeline_t *tl) {
    memset(tl, 0, sizeof(*tl));
    char path[4096];
    double t = 0.0;

    const char *files[] = { "boot/bios.txt", "boot/bios2.txt", "boot/bios3.txt" };
    for (size_t i = 0; i < 3; i++) {
        const char *p = find_data(files[i]);
        if (!p) continue;
        char *src = read_file(p, NULL);
        if (!src) continue;
        snprintf(path, sizeof(path), "%s", files[i]);

        for (char *c = src; *c;) {
            if (*c == '@') {
                if (strncmp(c, "@WAIT", 5) == 0) {
                    double d = 0.0;
                    int consumed = 0;
                    if (sscanf(c, "@WAIT %lf %n", &d, &consumed) == 1) {
                        t += d;
                        c += consumed;
                    } else {
                        c += 5;
                    }
                    continue;
                }
                if (strncmp(c, "@SCANSOUNDS", 11) == 0) {
                    const char *lp = find_data("boot/sounds.list");
                    if (lp) {
                        tl->sounds = count_lines(lp);
                        tl_append_file_lines(tl, &t, lp, SOUND_LINE_DELAY);
                    }
                    c += 11;
                    continue;
                }
                if (strncmp(c, "@SCANTEXTURES", 13) == 0) {
                    const char *lp = find_data("boot/textures.list");
                    if (lp) {
                        tl->textures = count_lines(lp);
                        tl_append_file_lines(tl, &t, lp, TEXTURE_LINE_DELAY);
                    }
                    c += 13;
                    continue;
                }
                if (strncmp(c, "@SOUNDS", 7) == 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d", tl->sounds ? tl->sounds : 112);
                    tl_append_text(tl, &t, buf, 0.0);
                    c += 7;
                    continue;
                }
            }
            tl_push(tl, t, (unsigned char)*c);
            t += 0.012;
            c++;
        }
        free(src);
    }
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

static void draw_frame(cairo_t *cr) {
    /* Upscale the 960x540 design space to the real output. This single scale
       is the game's scale=2 (DrawScreenSize/2 -> window). */
    double us = ctx.height > 0 ? ctx.height / BOOT_DH : 1.0;
    cairo_scale(cr, us, us);

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);

    if (scene.phase == 1) {
        double t = scene.now - scene.title_start;
        double alpha = 1.0;
        if (t < FADE) alpha = t / FADE;
        else if (t > TITLE_HOLD) alpha = 1.0 - (t - TITLE_HOLD) / FADE;
        if (alpha < 0) alpha = 0;
        if (alpha > 1) alpha = 1;

        if (scene.logo_full) {
            int iw = cairo_image_surface_get_width(scene.logo_full);
            int ih = cairo_image_surface_get_height(scene.logo_full);
            /* Contain (fit entirely, letterbox) - matches game BIG_LOGO. */
            double scale = BOOT_DW / (double)iw;
            if (BOOT_DH / (double)ih < scale) scale = BOOT_DH / (double)ih;
            double w = iw * scale, h = ih * scale;
            cairo_set_source_surface(cr, scene.logo_full,
                                     (BOOT_DW - w) / 2, (BOOT_DH - h) / 2);
            cairo_paint_with_alpha(cr, alpha);
        }
        return;
    }

    /* Phase 0: small TWM logo at (0,0), native size (x2 is the cairo scale). */
    if (scene.logo) {
        cairo_set_source_surface(cr, scene.logo, 0, 0);
        cairo_paint(cr);
    }

    cairo_set_source_rgb(cr, BOOT_PR / 255.0, BOOT_PG / 255.0, BOOT_PB / 255.0);

    /* Version string: game math  x = W/2 - w - 10  (w = width at scale 1).
       Our font is already at the scale-2 size, so w1 = w/2. */
    char ver[64];
    snprintf(ver, sizeof(ver), "%s", BOOT_VERSION);
    PangoLayout *vlayout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(vlayout, scene.body_font);
    pango_layout_set_text(vlayout, ver, -1);
    PangoRectangle ext;
    pango_layout_get_pixel_extents(vlayout, NULL, &ext);
    double vx = BOOT_DW / 2.0 - (double)ext.width / 2.0 - 10.0;
    cairo_move_to(cr, vx, 4.0);
    pango_cairo_show_layout(cr, vlayout);
    g_object_unref(vlayout);

    /* BIOS console body text, revealed character-by-character, drawn one
       design line per bios newline (game: (10,100), LINE_HEIGHT=22). */
    size_t vis = scene.tl.visible;
    if (vis > scene.tl.n) vis = scene.tl.n;
    if (vis > 0 && scene.body_text) {
        char *buf = strndup(scene.body_text, vis);
        if (buf) {
            int line = 0;
            char *start = buf;
            for (char *p = buf; ; p++) {
                if (*p == '\n' || *p == '\0') {
                    char saved = *p;
                    *p = '\0';
                    if (start[0] != '\0' || saved == '\0') {
                        PangoLayout *bl = pango_cairo_create_layout(cr);
                        pango_layout_set_font_description(bl, scene.body_font);
                        pango_layout_set_text(bl, start, -1);
                        cairo_move_to(cr, BOOT_TEXT_X, BOOT_TEXT_Y + line * BOOT_LINE);
                        pango_cairo_show_layout(cr, bl);
                        g_object_unref(bl);
                    }
                    line++;
                    if (saved == '\0') break;
                    *p = saved;
                    start = p + 1;
                }
            }
            free(buf);
        }
    }
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { frame_done };

static void redraw(void) {
    int idx = ctx.buf_idx;
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        ctx.map[idx], CAIRO_FORMAT_ARGB32, ctx.width, ctx.height,
        cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width));
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
            play_sfx("sfx/twm_startup.wav");
        }
    } else if (scene.phase == 1) {
        if (scene.now - scene.title_start > TITLE_HOLD + FADE) {
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

static bool create_buffers(void) {
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
    size_t sz = (size_t)stride * ctx.height;

    char name[] = "/onewm-boot-XXXXXX";
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, sz * 2) < 0) { close(fd); return false; }

    for (int i = 0; i < 2; i++) {
        ctx.map[i] = mmap(NULL, sz * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, i * sz);
        if (ctx.map[i] == MAP_FAILED) { close(fd); return false; }
        ctx.size[i] = sz;
        memset(ctx.map[i], 0, sz);

        struct wl_shm_pool *pool = wl_shm_create_pool(ctx.shm, fd, sz * 2);
        ctx.buffers[i] = wl_shm_pool_create_buffer(pool, i * sz,
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

    size_t total = 0;
    const char *files[] = { "boot/bios.txt", "boot/bios2.txt", "boot/bios3.txt" };
    for (size_t i = 0; i < 3; i++) {
        const char *p = find_data(files[i]);
        if (!p) continue;
        size_t len = 0;
        char *part = read_file(p, &len);
        if (!part) continue;
        scene.body_text = realloc(scene.body_text, total + len + 1);
        memcpy(scene.body_text + total, part, len);
        total += len;
        scene.body_text[total] = '\0';
        free(part);
    }

    if (getenv("ONEWM_BOOT_DUMP")) {
        fprintf(stderr, "=== body_text ===\n%s\n=== end ===\n",
                scene.body_text ? scene.body_text : "(null)");
        fprintf(stderr, "timeline events=%zu total_time=%.2f\n",
                scene.tl.n, scene.tl.total_time);
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

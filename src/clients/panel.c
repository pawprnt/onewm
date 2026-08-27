#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cairo.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

/* Game constants: TWMTaskbar.TASKBAR_HEIGHT = 30 */
#define PANEL_H 30
#define MAX_WINDOWS 6
#define BUTTON_PAD 2
#define CLOCK_RIGHT_MARGIN 26
#define MINIMIZE_BTN_SIZE 16

struct panel {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	int width, height;
	bool configured, running;
	struct wl_buffer *buffers[2];
	void *map[2];
	size_t size[2];
	int buf_idx;
	int32_t px, py;
};
static struct panel ctx;

/* theme colors */
static float primary[3];
static float variant[3];
static float bg[3];

/* window list from compositor */
struct win_entry {
	char title[128];
	int active;
};
static struct win_entry windows[MAX_WINDOWS];
static int window_count = 0;

/* --- config / data helpers --- */

static void config_path(char *out, size_t n, const char *name) {
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";
	snprintf(out, n, "%s/.config/onewm/%s", home, name);
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
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
	buf[sz] = '\0';
	fclose(f);
	if (len) *len = sz;
	return buf;
}

static const char *match_brace_close(const char *open) {
	int d = 0;
	for (const char *p = open; *p; p++) {
		if (*p == '{') d++;
		else if (*p == '}') { d--; if (d == 0) return p; }
	}
	return NULL;
}

static void parse_rgb(const char *b, float *out) {
	int r = -1, g = -1, bv = -1;
	const char *rj = strstr(b, "\"r\"");
	const char *gj = strstr(b, "\"g\"");
	const char *bj = strstr(b, "\"b\"");
	if (rj) sscanf(strchr(rj, ':'), ": %d", &r);
	if (gj) sscanf(strchr(gj, ':'), ": %d", &g);
	if (bj) sscanf(strchr(bj, ':'), ": %d", &bv);
	if (r >= 0) out[0] = r / 255.0f;
	if (g >= 0) out[1] = g / 255.0f;
	if (bv >= 0) out[2] = bv / 255.0f;
}

static void load_theme(const char *id) {
	/* purple defaults */
	primary[0] = 150/255.f; primary[1] = 100/255.f; primary[2] = 255/255.f;
	variant[0] = 100/255.f; variant[1] = 66/255.f;  variant[2] = 165/255.f;
	bg[0] = bg[1] = bg[2] = 0.0f;

	const char *p = find_data("themes_metadata.json");
	if (!p) return;
	char *json = read_file(p, NULL);
	if (!json) return;
	const char *arr = strstr(json, "\"themes\"");
	const char *q = arr ? strchr(arr, '[') : NULL;
	if (q) {
		q++;
		while (*q) {
			const char *obj = strchr(q, '{');
			if (!obj) break;
			const char *end = match_brace_close(obj);
			if (!end) break;
			char fid[32] = {0};
			const char *k = strstr(obj, "\"id\"");
			if (k && k < end) {
				const char *s = strchr(k + 4, '"');
				if (s) { s++; const char *e = strchr(s, '"'); int l = e ? (int)(e - s) : 0; if (l > 31) l = 31; memcpy(fid, s, l); fid[l] = '\0'; }
			}
			if (strcmp(fid, id) == 0) {
				k = strstr(obj, "\"primary\"");
				if (k && k < end) { const char *o = strchr(k, '{'); if (o) parse_rgb(o, primary); }
				k = strstr(obj, "\"primaryVariant\"");
				if (k && k < end) { const char *o = strchr(k, '{'); if (o) parse_rgb(o, variant); }
				k = strstr(obj, "\"background\"");
				if (k && k < end) { const char *o = strchr(k, '{'); if (o) parse_rgb(o, bg); }
				break;
			}
			q = end + 1;
		}
	}
	free(json);
}

/* --- window list from compositor --- */

static void read_window_list(void) {
	char path[4096];
	config_path(path, sizeof path, "windows");
	FILE *f = fopen(path, "r");
	if (!f) { window_count = 0; return; }
	window_count = 0;
	char line[256];
	while (fgets(line, sizeof line, f) && window_count < MAX_WINDOWS) {
		size_t L = strlen(line);
		if (L && line[L-1] == '\n') line[--L] = '\0';
		if (!L) continue;
		char *tab = strchr(line, '\t');
		if (tab) {
			*tab = '\0';
			strncpy(windows[window_count].title, line, sizeof(windows[window_count].title) - 1);
			windows[window_count].active = atoi(tab + 1);
		} else {
			strncpy(windows[window_count].title, line, sizeof(windows[window_count].title) - 1);
			windows[window_count].active = 0;
		}
		window_count++;
	}
	fclose(f);
}

static volatile sig_atomic_t g_redraw = 0;
static void on_sig(int s) { (void)s; g_redraw = 1; }

/* --- drawing --- */

static void fill_rect(cairo_t *cr, double x, double y, double w, double h,
		float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r * a, g * a, b * a, a);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void draw_panel(cairo_t *cr, int W, int H) {
	/* Taskbar: outer 2px primary border, inner background fill.
	   Matches TWMTaskbar.Draw:
	     ColorBoxBlit(TaskbarArea, primary);    // 2px border
	     ColorBoxBlit(taskbarArea, background); // inner fill */
	fill_rect(cr, 0, 0, W, H, primary[0], primary[1], primary[2], 1.0);
	fill_rect(cr, 0, 2, W, H - 2, bg[0], bg[1], bg[2], 1.0);

	/* Read window list from compositor */
	read_window_list();

	/* Calculate button width like the game:
	   taskbarButtonWidth = (TaskbarArea.W - 26 - clockTextWidth) / 6 - 4;
	   We'll approximate clock width as ~50px for "HH:MM AM" */
	int clock_w = 60;
	int btn_w = (W - CLOCK_RIGHT_MARGIN - clock_w - 4 * BUTTON_PAD) / MAX_WINDOWS;
	if (btn_w < 30) btn_w = 30;
	if (btn_w > 120) btn_w = 120;

	/* Draw window buttons (matches WindowButton.Draw) */
	double bx = 2 + BUTTON_PAD;
	for (int i = 0; i < window_count; i++) {
		if (bx + btn_w > W - clock_w - CLOCK_RIGHT_MARGIN) break;
		double by = 2 + BUTTON_PAD;
		double bw = btn_w;
		double bh = H - 2 - 2 * BUTTON_PAD;

		bool is_active = windows[i].active;
		/* Colors: active non-minimized = bg on primary, minimized = variant border */
		float border_r, border_g, border_b;
		float inner_r, inner_g, inner_b;
		float text_r, text_g, text_b;

		if (is_active) {
			/* active window: primary border, bg inner, primary text */
			border_r = primary[0]; border_g = primary[1]; border_b = primary[2];
			inner_r = bg[0]; inner_g = bg[1]; inner_b = bg[2];
			text_r = primary[0]; text_g = primary[1]; text_b = primary[2];
		} else {
			/* inactive: variant border, bg inner, primary text */
			border_r = variant[0]; border_g = variant[1]; border_b = variant[2];
			inner_r = bg[0]; inner_g = bg[1]; inner_b = bg[2];
			text_r = primary[0]; text_g = primary[1]; text_b = primary[2];
		}

		/* Check hover */
		bool hover = (ctx.px >= (int)bx && ctx.px <= (int)(bx + bw) &&
			       ctx.py >= (int)by && ctx.py <= (int)(by + bh));
		if (hover) {
			border_r = fmin(1.0, border_r * 1.3 + 0.1);
			border_g = fmin(1.0, border_g * 1.3 + 0.1);
			border_b = fmin(1.0, border_b * 1.3 + 0.1);
		}

		/* Outer border rect */
		fill_rect(cr, bx, by, bw, bh, border_r, border_g, border_b, 1.0);
		/* Inner fill rect (2px inset) */
		fill_rect(cr, bx + 2, by + 2, bw - 4, bh - 4, inner_r, inner_g, inner_b, 1.0);

		/* Title text (truncated to fit) */
		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_width(pl, (bw - 8) * PANGO_SCALE);
		pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
		pango_layout_set_text(pl, windows[i].title, -1);
		int tw, th;
		pango_layout_get_pixel_size(pl, &tw, &th);
		cairo_set_source_rgba(cr, text_r, text_g, text_b, 1.0);
		cairo_move_to(cr, bx + 4, by + (bh - th) / 2.0);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		pango_font_description_free(fd);

		bx += btn_w + BUTTON_PAD;
	}

	/* Clock (right-aligned) */
	{
		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		char clock[16];
		strftime(clock, sizeof clock, "%I:%M %p", tm);
		/* strip leading zero */
		if (clock[0] == '0') memmove(clock, clock + 1, strlen(clock));

		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string("Sans 11");
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_text(pl, clock, -1);
		int tw, th;
		pango_layout_get_pixel_size(pl, &tw, &th);
		cairo_set_source_rgba(cr, primary[0], primary[1], primary[2], 1.0);
		cairo_move_to(cr, W - tw - CLOCK_RIGHT_MARGIN, (H - th) / 2.0);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		pango_font_description_free(fd);
	}
}

/* Render to offscreen ARGB32 surface, composite to SHM.  Cairo solid-source
   ops are broken on SHM for_data surfaces in this build. */
static void redraw(void) {
	if (!ctx.configured || ctx.buffers[0] == NULL) return;
	int idx = ctx.buf_idx;
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
	memset(ctx.map[idx], 0, (size_t)stride * ctx.height);
	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		ctx.map[idx], CAIRO_FORMAT_ARGB32, ctx.width, ctx.height, stride);
	cairo_surface_t *ui = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ctx.width, ctx.height);
	cairo_t *cr = cairo_create(ui);
	draw_panel(cr, ctx.width, ctx.height);
	cairo_destroy(cr);
	cairo_t *cr2 = cairo_create(surf);
	cairo_set_source_surface(cr2, ui, 0, 0);
	cairo_paint(cr2);
	cairo_destroy(cr2);
	cairo_surface_destroy(ui);
	cairo_surface_flush(surf);
	cairo_surface_destroy(surf);

	wl_surface_attach(ctx.surface, ctx.buffers[idx], 0, 0);
	wl_surface_damage_buffer(ctx.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(ctx.surface);
	ctx.buf_idx = 1 - ctx.buf_idx;
}

/* --- input --- */

static void launch(const char *cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)serial; (void)s;
	ctx.px = wl_fixed_to_int(x); ctx.py = wl_fixed_to_int(y); redraw();
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	(void)d; (void)p; (void)serial; (void)s;
	ctx.px = -1; ctx.py = -1; redraw();
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	(void)d; (void)p; (void)t;
	int nx = wl_fixed_to_int(x), ny = wl_fixed_to_int(y);
	if (nx != ctx.px || ny != ctx.py) { ctx.px = nx; ctx.py = ny; redraw(); }
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
		uint32_t button, uint32_t state) {
	(void)d; (void)p; (void)serial; (void)t; (void)button;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;
	if (ctx.py < 0) return;
	(void)launch;
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
	(void)d; (void)p; (void)t; (void)axis; (void)value;
}
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t s) { (void)d;(void)p;(void)a;(void)s; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
	.button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
	.axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ctx.pointer) {
		ctx.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ctx.pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* --- buffers --- */

static bool create_buffers(void) {
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
	size_t sz = (size_t)stride * ctx.height;
	long pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize < 1) pagesize = 4096;
	size_t sz_aligned = (sz + pagesize - 1) & ~(size_t)(pagesize - 1);
	size_t total = sz_aligned * 2;
	char name[] = "/onewm-panel-XXXXXX";
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
			ctx.width, ctx.height, stride, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
	}
	close(fd);
	return true;
}

/* --- registry --- */

static void layer_surface_configure(void *d, struct zwlr_layer_surface_v1 *ls,
		uint32_t serial, uint32_t w, uint32_t h) {
	(void)d;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if ((int32_t)w > 0) ctx.width = w;
	if ((int32_t)h > 0) ctx.height = h;
	bool need_realloc = (ctx.buffers[0] == NULL) ||
		(ctx.configured && w > 0 && h > 0);
	if (need_realloc) {
		for (int i = 0; i < 2; i++) {
			if (ctx.map[i]) { munmap(ctx.map[i], ctx.size[i]); ctx.map[i] = NULL; }
			if (ctx.buffers[i]) { wl_buffer_destroy(ctx.buffers[i]); ctx.buffers[i] = NULL; }
		}
		if (!create_buffers()) {
			fprintf(stderr, "onewm-panel: buffer alloc failed\n");
			return;
		}
	}
	ctx.configured = true;
	redraw();
}
static void layer_surface_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
	(void)d; (void)ls; ctx.running = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure, .closed = layer_surface_closed,
};
static void shm_format(void *d, struct wl_shm *shm, uint32_t format) { (void)d; (void)shm; (void)format; }
static const struct wl_shm_listener shm_listener = { .format = shm_format };
static void registry_global(void *d, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	(void)d; (void)version;
	if (strcmp(iface, wl_compositor_interface.name) == 0)
		ctx.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, wl_shm_interface.name) == 0) {
		ctx.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
		wl_shm_add_listener(ctx.shm, &shm_listener, NULL);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		ctx.seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(ctx.seat, &seat_listener, NULL);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
		ctx.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

int main(int argc, char **argv) {
	(void)argc; (void)argv;

	memset(&ctx, 0, sizeof(ctx));
	ctx.width = 1280;
	ctx.height = PANEL_H;
	ctx.px = -1; ctx.py = -1;
	ctx.running = true;

	const char *tid = getenv("ONEWM_THEME");
	char theme_buf[64];
	if (tid) {
		snprintf(theme_buf, sizeof theme_buf, "%s", tid);
	} else {
		/* read from config */
		char tp[4096]; config_path(tp, sizeof tp, "theme");
		FILE *f = fopen(tp, "r");
		if (f) {
			if (fgets(theme_buf, sizeof theme_buf, f)) {
				size_t L = strlen(theme_buf);
				if (L && theme_buf[L-1] == '\n') theme_buf[L-1] = '\0';
			}
			fclose(f);
		} else {
			snprintf(theme_buf, sizeof theme_buf, "purple");
		}
	}
	load_theme(theme_buf);

	/* Signal handler for redraw on compositor window list changes */
	signal(SIGUSR1, on_sig);

	ctx.display = wl_display_connect(NULL);
	if (!ctx.display) {
		fprintf(stderr, "onewm-panel: no wayland compositor\n");
		return 1;
	}
	ctx.registry = wl_display_get_registry(ctx.display);
	wl_registry_add_listener(ctx.registry, &registry_listener, NULL);
	wl_display_roundtrip(ctx.display);
	if (!ctx.compositor || !ctx.shm || !ctx.layer_shell) {
		fprintf(stderr, "onewm-panel: missing compositor/shm/layer-shell\n");
		return 1;
	}

	ctx.surface = wl_compositor_create_surface(ctx.compositor);
	ctx.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		ctx.layer_shell, ctx.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "onewm-panel");
	zwlr_layer_surface_v1_set_keyboard_interactivity(ctx.layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	zwlr_layer_surface_v1_set_anchor(ctx.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(ctx.layer_surface, 0, PANEL_H);
	zwlr_layer_surface_v1_set_exclusive_zone(ctx.layer_surface, PANEL_H);
	zwlr_layer_surface_v1_add_listener(ctx.layer_surface, &layer_surface_listener, NULL);
	wl_surface_commit(ctx.surface);

	while (ctx.running && wl_display_dispatch(ctx.display) != -1) {
		if (g_redraw) { g_redraw = 0; redraw(); }
	}

	for (int i = 0; i < 2; i++) {
		if (ctx.map[i]) munmap(ctx.map[i], ctx.size[i]);
		if (ctx.buffers[i]) wl_buffer_destroy(ctx.buffers[i]);
	}
	if (ctx.layer_surface) zwlr_layer_surface_v1_destroy(ctx.layer_surface);
	if (ctx.surface) wl_surface_destroy(ctx.surface);
	if (ctx.display) wl_display_disconnect(ctx.display);
	return 0;
}

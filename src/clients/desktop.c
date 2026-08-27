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
#include <unistd.h>

#include <cairo.h>
#include <zlib.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

#define ICON 64
/* The game lays out desktop icons in an internal coordinate space (icon 32px,
   ICON_OFFSET (26,6), label at y=36, selection box 80x54) and then upscales 2x
   for display. We render directly at the on-screen (2x) resolution, so all of
   those constants are doubled here to stay 1:1 with the game's final look. */
#define ICON_OFF_X 52   /* 26 * 2 */
#define ICON_OFF_Y 12   /* 6 * 2  */
#define LABEL_Y    72    /* 36 * 2 */
#define SELBOX_W   160   /* 80 * 2 */
#define SELBOX_H   108   /* (36 + textHeight(12) + 2 + 4) * 2 */
#define SELBOX_IN  4     /* ClickAreaForIcon offset + inner-shrink, 2 * 2 */

#define MARGIN 28
#define COL_GAP 168      /* 84 * 2 */
#define ROW_STEP 144      /* 72 * 2 */

struct icon {
	const char *name;
	const char *rel;     /* path relative to ICON_DIR */
	const char *cmd;
	int x, y;            /* top-left of the icon cell */
	cairo_surface_t *base;   /* unscaled white icon (ICON x ICON) */
	cairo_surface_t *tint;   /* base tinted to the theme primary    */
	cairo_surface_t *tint_sh;/* base tinted to the theme background */
	cairo_surface_t *label;  /* pre-rendered icon caption (white)   */
};

/* Locked layout (see desktop spec). Column 1 = left stack, column 2 = right
   of OneBoot. */
static struct icon icons[] = {
	{ "OneBoot",    "apps/oneshot.png",                       "true",            0, 0, NULL },
	{ "Firefox",    "apps/firefox-3.0.png",                   "firefox",        0, 0, NULL },
	{ "Alacritty",  "apps/Alacritty.png",                     "alacritty",      0, 0, NULL },
	{ "Files",      "places/folder-visiting.png",             "onewm filemanager", 0, 0, NULL },
	{ "Wallpapers", "apps/preferences-desktop-wallpaper.png", "onewm wallpapers", 0, 0, NULL },
	{ "Themes",     "apps/preferences-desktop-theme.png",     "onewm themes",   0, 0, NULL },
};
#define NICON (sizeof(icons) / sizeof(icons[0]))

struct desktop {
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
	char theme_id[32];
	float primary[3];
	float primary_variant[3];
	float background[3];
	cairo_surface_t *wallpaper;
	cairo_surface_t *selbox_border; /* pre-rendered focus box: 2px primary border @½α */
	cairo_surface_t *selbox_fill;   /* pre-rendered focus box: background @½α inner */
};
static struct desktop ctx;

static volatile sig_atomic_t g_reload_wp = 0;
static volatile sig_atomic_t g_reload_theme = 0;

/* --- config / data helpers --- */

static void config_path(char *out, size_t n, const char *name) {
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";
	snprintf(out, n, "%s/.config/onewm/%s", home, name);
}

static void read_line(const char *path, char *buf, size_t n) {
	buf[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fgets(buf, n, f)) {
		size_t L = strlen(buf);
		if (L && buf[L - 1] == '\n') buf[L - 1] = '\0';
	}
	fclose(f);
}

static char *find_data(const char *rel) {
	static char path[4096];

	/* Allow an external wallpaper directory (general.wallpaper_dir) to supply
	   additional/default wallpapers, taking precedence over embedded ones. */
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
	snprintf(path, sizeof(path), "/usr/share/onewm/%s", rel);
	if (access(path, R_OK) == 0) return path;
	return NULL;
}

static char *find_icon(const char *rel) {
	static char path[4096];
	const char *dir = getenv("ONEWM_ICON_DIR");
	if (!dir) dir = "/home/fox/TWM-xfce/TWM-icons/scalable";
	snprintf(path, sizeof(path), "%s/%s", dir, rel);
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

static const char *match_brace_close(const char *s) {
	int d = 0; bool started = false;
	for (; *s; s++) {
		if (*s == '{') { d++; started = true; }
		else if (*s == '}') { d--; if (started && d == 0) return s; }
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
	ctx.primary[0] = 150 / 255.0f; ctx.primary[1] = 100 / 255.0f; ctx.primary[2] = 255 / 255.0f;
	ctx.primary_variant[0] = 100 / 255.0f; ctx.primary_variant[1] = 66 / 255.0f; ctx.primary_variant[2] = 165 / 255.0f;
	ctx.background[0] = 0; ctx.background[1] = 0; ctx.background[2] = 0;
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
			const char *k = strstr(obj, "\"id\"");
			char fid[32]; fid[0] = '\0';
			if (k && k < end) {
				const char *s = strchr(k + 4, '"');
				if (s) { s++; const char *e = strchr(s, '"'); int l = e ? (int)(e - s) : 0; if (l > 31) l = 31; memcpy(fid, s, l); fid[l] = '\0'; }
			}
			if (strcmp(fid, id) == 0) {
				k = strstr(obj, "\"primary\"");
				if (k && k < end) { const char *o = strchr(k, '{'); if (o) parse_rgb(o, ctx.primary); }
				k = strstr(obj, "\"primaryVariant\"");
				if (k && k < end) { const char *o = strchr(k, '{'); if (o) parse_rgb(o, ctx.primary_variant); }
				k = strstr(obj, "\"background\"");
				if (k && k < end) { const char *o = strchr(k, '{'); if (o) parse_rgb(o, ctx.background); }
				break;
			}
			q = end + 1;
		}
	}
	free(json);
}

/* Load a PNG and copy it into a clean ARGB32 surface. cairo's PNG loader on
   some setups silently fails to rasterise images with 8-bit alpha, so we
   re-rasterise every icon/wallpaper through a known-good surface. */
static cairo_surface_t *load_png_norm(const char *p) {
	cairo_surface_t *src = cairo_image_surface_create_from_png(p);
	if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) {
		if (src) cairo_surface_destroy(src);
		return NULL;
	}
	int w = cairo_image_surface_get_width(src);
	int h = cairo_image_surface_get_height(src);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(out);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(src);
	return out;
}

static void reload_wallpaper(void) {
	char cfgp[4096];
	config_path(cfgp, sizeof cfgp, "wallpaper");
	char cfg[4096];
	read_line(cfgp, cfg, sizeof cfg);
	const char *wp = cfg[0] ? cfg : getenv("ONEWM_WALLPAPER");
	if (!wp) wp = find_data("wallpapers/catwalks.png-0.png");
	if (!wp) return;
	cairo_surface_t *nw = load_png_norm(wp);
	if (nw) {
		if (ctx.wallpaper) cairo_surface_destroy(ctx.wallpaper);
		ctx.wallpaper = nw;
	}
}

/* Scale a white icon PNG into an ICON x ICON ARGB32 surface. */
static cairo_surface_t *make_scaled(const char *p, int S) {
	cairo_surface_t *src = load_png_norm(p);
	if (!src) return NULL;
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
	cairo_t *cr = cairo_create(out);
	cairo_scale(cr, (double)S / w, (double)S / h);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(src);
	return out;
}

/* Multiply-tint a white icon by `col` (0..1), exactly like the game's
   MainBlit(icon, color): result RGB = col * whiteLuminance, alpha preserved. */
static cairo_surface_t *tint_icon(cairo_surface_t *base, const float *col) {
	int W = cairo_image_surface_get_width(base);
	int H = cairo_image_surface_get_height(base);
	cairo_surface_t *t = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
	unsigned char *d = cairo_image_surface_get_data(base);
	unsigned char *o = cairo_image_surface_get_data(t);
	unsigned char R = (unsigned char)(col[0] * 255);
	unsigned char G = (unsigned char)(col[1] * 255);
	unsigned char B = (unsigned char)(col[2] * 255);
	int npix = W * H;
	for (int k = 0; k < npix; k++) {
		unsigned char sa = d[k*4+3];
		if (sa == 0) { o[k*4] = o[k*4+1] = o[k*4+2] = o[k*4+3] = 0; continue; }
		unsigned char bs = (unsigned char)(d[k*4]   * 255 / sa);
		unsigned char gs = (unsigned char)(d[k*4+1] * 255 / sa);
		unsigned char rs = (unsigned char)(d[k*4+2] * 255 / sa);
		unsigned char lum = (unsigned char)((rs + gs + bs) / 3);
		int or_ = (int)(R * lum / 255 * sa / 255);
		int og = (int)(G * lum / 255 * sa / 255);
		int ob = (int)(B * lum / 255 * sa / 255);
		o[k*4]   = (unsigned char)ob;
		o[k*4+1] = (unsigned char)og;
		o[k*4+2] = (unsigned char)or_;
		o[k*4+3] = sa;
	}
	cairo_surface_mark_dirty(t);
	return t;
}

/* Pre-render the focus/selector box. cairo fills / solid sources are broken
   in this build, so we build two solid-colour surfaces the same way the tinted
   icons are: tint_icon() of a solid-white PNG yields a solid (primary /
   background) surface that cairo CAN paint. The box is drawn at paint time by
   compositing both surfaces (no clip / fill needed). Mirrors FileIcon.Draw:
   a 2px primary border @½α over a background @½α fill. */
/* Game's ClickAreaForIcon: outer rect at (pos+2), size 80×(36+textH+6).
   Inner shrunk by borderSize=2. For single-line labels textH=12, so
   outer=80×54, inner=76×50. Draw a solid color box via a normal cairo
   surface (solid ops work fine off-SHM), then tint with the theme color. */
static cairo_surface_t *make_color_box(int w, int h, const float *col, float alpha) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgba(cr, col[0], col[1], col[2], alpha);
	cairo_paint(cr);
	cairo_destroy(cr);
	return s;
}
static void build_selbox(void) {
	if (ctx.selbox_border) cairo_surface_destroy(ctx.selbox_border);
	if (ctx.selbox_fill) cairo_surface_destroy(ctx.selbox_fill);
	ctx.selbox_border = NULL;
	ctx.selbox_fill = NULL;
	/* Outer: primary @ ½α — FileIcon.ClickAreaForIcon 80×54 in game-space, ×2. */
	ctx.selbox_border = make_color_box(SELBOX_W, SELBOX_H, ctx.primary, 0.5);
	/* Inner: background @ ½α — shrunk by 2px game-space (4px) on each side. */
	ctx.selbox_fill   = make_color_box(SELBOX_W - 2 * SELBOX_IN, SELBOX_H - 2 * SELBOX_IN,
	                                   ctx.background, 0.5);
}

static void retint(void) {
	for (size_t i = 0; i < NICON; i++) {
		if (!icons[i].base) continue;
		if (icons[i].tint) cairo_surface_destroy(icons[i].tint);
		if (icons[i].tint_sh) cairo_surface_destroy(icons[i].tint_sh);
		icons[i].tint = tint_icon(icons[i].base, ctx.primary);
		icons[i].tint_sh = tint_icon(icons[i].base, ctx.background);
	}
	build_selbox();
}

/* Render an icon caption to an offscreen surface. The desktop surface can't
   paint solid-source text in this cairo build, so we render the (white text +
   1px shadow) onto a normal ARGB32 surface and composite it with
   set_source_surface, which does work. */
static cairo_surface_t *make_label(const char *text) {
	PangoFontDescription *fd = pango_font_description_from_string("Sans 13");
	cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *mcr = cairo_create(ms);
	PangoLayout *pl = pango_cairo_create_layout(mcr);
	pango_layout_set_font_description(pl, fd);
	pango_layout_set_text(pl, text, -1);
	int tw, th; pango_layout_get_pixel_size(pl, &tw, &th);
	g_object_unref(pl);
	cairo_destroy(mcr); cairo_surface_destroy(ms);
	if (tw <= 0 || th <= 0) { pango_font_description_free(fd); return NULL; }

	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw + 2, th + 2);
	cairo_t *cr = cairo_create(s);
	pl = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(pl, fd);
	pango_layout_set_text(pl, text, -1);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
	cairo_move_to(cr, 1, 1);
	pango_cairo_show_layout(cr, pl);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_move_to(cr, 0, 0);
	pango_cairo_show_layout(cr, pl);
	g_object_unref(pl);
	cairo_destroy(cr);
	pango_font_description_free(fd);
	return s;
}

/* Fallback icon drawn procedurally when the real asset PNG is unavailable:
   a white rounded tile that gets tinted to the theme primary by retint(). */
static cairo_surface_t *make_procedural(int idx) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ICON, ICON);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	double m = 10, r = 12;
	cairo_move_to(cr, m + r, m);
	cairo_arc(cr, ICON - m - r, m + r, r, -M_PI_2, 0);
	cairo_arc(cr, ICON - m - r, ICON - m - r, r, 0, M_PI_2);
	cairo_arc(cr, m + r, ICON - m - r, r, M_PI_2, M_PI);
	cairo_arc(cr, m + r, m + r, r, M_PI, M_PI * 1.5);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

static void load_icons(void) {
	for (size_t i = 0; i < NICON; i++) {
		char *p = find_icon(icons[i].rel);
		if (p) icons[i].base = make_scaled(p, ICON);
		else icons[i].base = make_procedural((int)i);
		icons[i].label = make_label(icons[i].name);
	}
	retint();
}

static void layout_icons(void) {
	int col1 = MARGIN;
	int col2 = MARGIN + COL_GAP;
	int y0 = MARGIN;
	icons[0].x = col1; icons[0].y = y0 + 0 * ROW_STEP;          /* OneBoot    */
	icons[4].x = col2; icons[4].y = y0 + 0 * ROW_STEP;          /* Wallpapers */
	icons[1].x = col1; icons[1].y = y0 + 1 * ROW_STEP;          /* Firefox    */
	icons[5].x = col2; icons[5].y = y0 + 1 * ROW_STEP;          /* Themes     */
	icons[2].x = col1; icons[2].y = y0 + 2 * ROW_STEP;          /* Alacritty  */
	icons[3].x = col1; icons[3].y = y0 + 3 * ROW_STEP;          /* Files      */
}

/* --- drawing --- */

static void draw_icon(cairo_t *cr, int i, bool hover) {
	struct icon *ic = &icons[i];
	int x = ic->x, y = ic->y;

	/* Game-style hover box: outer rect primary @½α at (x+2,y+2,80,54),
	   inner rect background @½α at (x+4,y+4,76,50). Matches
	   FileIcon.ClickAreaForIcon(pos) = Rect(pos.X+2, pos.Y+2, 80, 36+textH+6)
	   with textHeight=12 for single-line labels. */
	if (hover) {
		if (ctx.selbox_border) {
			cairo_set_source_surface(cr, ctx.selbox_border, x + SELBOX_IN, y + SELBOX_IN);
			cairo_paint(cr);
		}
		if (ctx.selbox_fill) {
			cairo_set_source_surface(cr, ctx.selbox_fill,
				x + 2 * SELBOX_IN, y + 2 * SELBOX_IN);
			cairo_paint(cr);
		}
	}

	/* Game-style tint: white icon × primary colour, with a 1px background-
	   coloured drop shadow (mirrors FileIcon.Draw's two MainBlit calls). The
	   icon is offset by ICON_OFF (26,6 in game-space, ×2). */
	if (ic->tint_sh) {
		cairo_set_source_surface(cr, ic->tint_sh, x + ICON_OFF_X + 1, y + ICON_OFF_Y + 1);
		cairo_paint(cr);
	}
	if (ic->tint) {
		cairo_set_source_surface(cr, ic->tint, x + ICON_OFF_X, y + ICON_OFF_Y);
		cairo_paint(cr);
	}

	/* Caption: paint the pre-rendered label surface (white text + 1px
	   shadow drawn on an offscreen surface, since solid-source text fails
	   on the desktop surface in this cairo build). Centered under the icon
	   (game centers the label at x offset 42 in game-space → 84 ×2). */
	{
		int tw = 0, th = 0;
		if (ic->label) {
			tw = cairo_image_surface_get_width(ic->label);
			th = cairo_image_surface_get_height(ic->label);
		}
		int lx = x + ICON_OFF_X + (ICON / 2) - (tw / 2);
		int ly = y + LABEL_Y;
		if (ic->label) {
			cairo_set_source_surface(cr, ic->label, lx, ly);
			cairo_paint(cr);
		}
	}
}

static void draw_desktop(cairo_t *cr, int W, int H) {
	if (ctx.wallpaper) {
		int iw = cairo_image_surface_get_width(ctx.wallpaper);
		int ih = cairo_image_surface_get_height(ctx.wallpaper);
		double s = fmax((double)W / iw, (double)H / ih);
		double dx = (W - iw * s) / 2.0, dy = (H - ih * s) / 2.0;
		cairo_save(cr);
		cairo_translate(cr, dx, dy);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, ctx.wallpaper, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
	}

	for (size_t i = 0; i < NICON; i++) {
		int ix = icons[i].x, iy = icons[i].y;
		bool hover = ctx.px >= ix + SELBOX_IN && ctx.px <= ix + SELBOX_IN + SELBOX_W &&
		             ctx.py >= iy + SELBOX_IN && ctx.py <= iy + SELBOX_IN + SELBOX_H;
		draw_icon(cr, i, hover);
	}
}

static void redraw(void) {
	if (!ctx.configured || ctx.buffers[0] == NULL) return;
	int idx = ctx.buf_idx;
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
	memset(ctx.map[idx], 0, (size_t)stride * ctx.height);
	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		ctx.map[idx], CAIRO_FORMAT_ARGB32, ctx.width, ctx.height, stride);
	cairo_t *cr = cairo_create(surf);
	draw_desktop(cr, ctx.width, ctx.height);
	cairo_destroy(cr);
	cairo_surface_flush(surf);
	cairo_surface_destroy(surf);

	wl_surface_attach(ctx.surface, ctx.buffers[idx], 0, 0);
	wl_surface_damage_buffer(ctx.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(ctx.surface);
	ctx.buf_idx = 1 - ctx.buf_idx;
}

/* --- input --- */

static void launch(int i) {
	const char *cmd = icons[i].cmd;
	pid_t pid = fork();
	if (pid == 0) {
		/* Prepend our own binary directory to PATH so the selectors
		   (onewm wallpapers / onewm themes) can be found. */
		char self[4096];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n > 0) {
			self[n] = '\0';
			char *sl = strrchr(self, '/');
			if (sl) *sl = '\0';
			char pathenv[8192];
			snprintf(pathenv, sizeof pathenv, "%s:%s", self, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
			setenv("PATH", pathenv, 1);
		}
		execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static int icon_at(int x, int y) {
	for (size_t i = 0; i < NICON; i++) {
		int ix = icons[i].x, iy = icons[i].y;
		if (x >= ix + SELBOX_IN && x <= ix + SELBOX_IN + SELBOX_W &&
		    y >= iy + SELBOX_IN && y <= iy + SELBOX_IN + SELBOX_H)
			return (int)i;
	}
	return -1;
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
	int i = icon_at(ctx.px, ctx.py);
	if (i >= 0) launch(i);
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
	(void)d; (void)p; (void)t; (void)axis; (void)value;
}
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t s) { (void)d;(void)p;(void)a;(void)s; }

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
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
static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

/* --- buffers --- */

static bool create_buffers(void) {
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
	size_t sz = (size_t)stride * ctx.height;

	long pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize < 1) pagesize = 4096;
	size_t sz_aligned = (sz + pagesize - 1) & ~(size_t)(pagesize - 1);
	size_t total = sz_aligned * 2;

	char name[] = "/onewm-desktop-XXXXXX";
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

/* --- registry / layer surface --- */

static void layer_surface_configure(void *d, struct zwlr_layer_surface_v1 *ls,
		uint32_t serial, uint32_t w, uint32_t h) {
	(void)d;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if ((int32_t)w > 0) ctx.width = w;
	if ((int32_t)h > 0) ctx.height = h;
	/* (Re)create buffers if first time or if size changed (e.g. wlr-randr mode switch) */
	bool need_realloc = (ctx.buffers[0] == NULL) ||
		(ctx.configured && w > 0 && h > 0);
	if (need_realloc) {
		/* free old buffers */
		for (int i = 0; i < 2; i++) {
			if (ctx.map[i]) { munmap(ctx.map[i], ctx.size[i]); ctx.map[i] = NULL; }
			if (ctx.buffers[i]) { wl_buffer_destroy(ctx.buffers[i]); ctx.buffers[i] = NULL; }
		}
		if (!create_buffers()) {
			fprintf(stderr, "onewm-desktop: buffer alloc failed\n");
			return;
		}
	}
	ctx.configured = true;
	redraw();
}
static void layer_surface_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
	(void)d; (void)ls;
	ctx.running = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void shm_format(void *d, struct wl_shm *shm, uint32_t format) { (void)d; (void)shm; (void)format; }
static const struct wl_shm_listener shm_listener = { .format = shm_format };

static void registry_global(void *d, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	(void)d; (void)version;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		ctx.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		ctx.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
		wl_shm_add_listener(ctx.shm, &shm_listener, NULL);
	} else if (strcmp(iface, wl_seat_interface.name) == 0) {
		ctx.seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(ctx.seat, &seat_listener, NULL);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		ctx.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	}
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void on_sig(int s) {
	if (s == SIGUSR1) g_reload_wp = 1;
	else if (s == SIGUSR2) g_reload_theme = 1;
}

int desktop_main(int argc, char **argv) {
	(void)argc; (void)argv;

	memset(&ctx, 0, sizeof(ctx));
	ctx.width = 1280;
	ctx.height = 720;
	ctx.px = -1; ctx.py = -1;
	ctx.running = true;

	/* Theme: config file wins, then env, then purple. */
	char tc[64]; config_path(tc, sizeof tc, "theme");
	char tid[64]; read_line(tc, tid, sizeof tid);
	const char *e = getenv("ONEWM_THEME");
	snprintf(ctx.theme_id, sizeof ctx.theme_id, "%s", tid[0] ? tid : (e ? e : "purple"));
	load_theme(ctx.theme_id);

	layout_icons();
	reload_wallpaper();
	load_icons();

	/* Signal the live desktop so selectors can poke it. */
	char pidp[4096]; config_path(pidp, sizeof pidp, "desktop.pid");
	FILE *pf = fopen(pidp, "w");
	if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
	signal(SIGUSR1, on_sig);
	signal(SIGUSR2, on_sig);

	if (getenv("ONEWM_DESKTOP_PNG")) {
		int W = 1280, H = 720;
		const char *bw = getenv("ONEWM_DESKTOP_W");
		const char *bh = getenv("ONEWM_DESKTOP_H");
		if (bw) W = atoi(bw);
		if (bh) H = atoi(bh);
		ctx.width = W; ctx.height = H;
		/* Mirror the live path: draw onto a heap buffer via
		   create_for_data (premultiplied ARGB32 / BGRA). cairo's own
		   write_to_png keeps alpha on a for_data surface (it drops it on a
		   plain cairo_image_surface_create surface in this build). */
		int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, W);
		unsigned char *buf = calloc((size_t)stride * H, 1);
		cairo_surface_t *surf = cairo_image_surface_create_for_data(
			buf, CAIRO_FORMAT_ARGB32, W, H, stride);
		/* simulate a pointer hovering the first icon so the selector box
		   is exercised in the offline dump */
		ctx.px = icons[0].x + 10; ctx.py = icons[0].y + 10;
		cairo_t *cr = cairo_create(surf);
		draw_desktop(cr, W, H);
		cairo_destroy(cr);
		/* cairo's write_to_png drops/garbles alpha on surfaces in this
		   build, so write the for_data buffer (premultiplied BGRA) out
		   ourselves, un-premultiplying for correct display. */
		{
			unsigned char *raw = malloc((size_t)W * H * 4 + H);
			unsigned char *rp = raw;
			for (int y = 0; y < H; y++) {
				*rp++ = 0;
				for (int x = 0; x < W; x++) {
					unsigned char *p = buf + y * stride + x * 4;
					unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
					/* un-premultiply so the PNG displays correctly */
					if (a) {
						r = (unsigned char)(r * 255 / a);
						g = (unsigned char)(g * 255 / a);
						b = (unsigned char)(b * 255 / a);
					}
					*rp++ = r; *rp++ = g; *rp++ = b; *rp++ = a;
				}
			}
			unsigned long rawlen = (unsigned long)((size_t)W*H*4+H);
			unsigned char *comp = malloc(rawlen);
			uLongf clen = rawlen;
			compress(comp, &clen, raw, rawlen);
			FILE *fp = fopen("/tmp/desktop_actual.png", "wb");
			fwrite("\x89PNG\r\n\x1a\n", 1, 8, fp);
			void wchunk(FILE*f,const char*t,void*d,unsigned n){
				unsigned char len[4]; len[0]=n>>24;len[1]=n>>16;len[2]=n>>8;len[3]=n;
				fwrite(len,1,4,fp); fwrite(t,1,4,fp); fwrite(d,1,n,fp);
				unsigned C=crc32(0,(const Bytef*)t,4); C=crc32(C,(const Bytef*)d,n);
				unsigned char crc[4]; crc[0]=C>>24;crc[1]=C>>16;crc[2]=C>>8;crc[3]=C; fwrite(crc,1,4,fp);
			}
			unsigned char ihdr[13];
			ihdr[0]=W>>24;ihdr[1]=W>>16;ihdr[2]=W>>8;ihdr[3]=W;
			ihdr[4]=H>>24;ihdr[5]=H>>16;ihdr[6]=H>>8;ihdr[7]=H;
			ihdr[8]=8;ihdr[9]=6;ihdr[10]=0;ihdr[11]=0;ihdr[12]=0;
			wchunk(fp,"IHDR",ihdr,13);
			wchunk(fp,"IDAT",comp,clen);
			wchunk(fp,"IEND",(void*)"",0);
			fclose(fp);
			free(raw); free(comp);
		}
		cairo_surface_destroy(surf);

		free(buf);
		fprintf(stderr, "wrote /tmp/desktop_actual.png theme=%s icons=%zu\n", ctx.theme_id, NICON);
		return 0;
	}

	ctx.display = wl_display_connect(NULL);
	if (!ctx.display) {
		fprintf(stderr, "onewm-desktop: no wayland compositor (WAYLAND_DISPLAY?)\n");
		return 1;
	}
	ctx.registry = wl_display_get_registry(ctx.display);
	wl_registry_add_listener(ctx.registry, &registry_listener, NULL);
	wl_display_roundtrip(ctx.display);

	if (!ctx.compositor || !ctx.shm || !ctx.layer_shell) {
		fprintf(stderr, "onewm-desktop: missing compositor/shm/layer-shell\n");
		return 1;
	}

	ctx.surface = wl_compositor_create_surface(ctx.compositor);
	ctx.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		ctx.layer_shell, ctx.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "onewm-desktop");
	zwlr_layer_surface_v1_set_anchor(ctx.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(ctx.layer_surface, 0, 0);
	zwlr_layer_surface_v1_add_listener(ctx.layer_surface, &layer_surface_listener, NULL);
	wl_surface_commit(ctx.surface);

	while (ctx.running) {
		int r = wl_display_dispatch(ctx.display);
		if (g_reload_wp) { g_reload_wp = 0; reload_wallpaper(); redraw(); }
		if (g_reload_theme) { g_reload_theme = 0; load_theme(ctx.theme_id); retint(); build_selbox(); redraw(); }
		if (r == -1) break;
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

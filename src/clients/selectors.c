#define _GNU_SOURCE
#include <dirent.h>
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

enum mode { MODE_WALLPAPERS, MODE_THEMES };
static enum mode sel_mode;

/* --- theme colors (matches TWMTheme from the game) --- */
struct theme {
	float primary[3];    /* main accent */
	float variant[3];    /* dimmer accent */
	float background[3]; /* window bg */
};
static struct theme cur_theme;

static void load_theme(void) {
	/* defaults: purple */
	cur_theme.primary[0] = 150.f/255; cur_theme.primary[1] = 100.f/255; cur_theme.primary[2] = 255.f/255;
	cur_theme.variant[0] = 100.f/255; cur_theme.variant[1] = 66.f/255;  cur_theme.variant[2] = 165.f/255;
	cur_theme.background[0] = 0; cur_theme.background[1] = 0; cur_theme.background[2] = 0;

	/* read current theme id from config */
	char tid_path[4096];
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";
	snprintf(tid_path, sizeof tid_path, "%s/.config/onewm/theme", home);
	char tid[64] = {0};
	FILE *f = fopen(tid_path, "r");
	if (f) { if (fgets(tid, sizeof tid, f)) { size_t L = strlen(tid); if (L && tid[L-1]=='\n') tid[L-1]='\0'; } fclose(f); }
	if (!tid[0]) return;

	/* find theme in themes_metadata.json */
	const char *env = getenv("ONEWM_DATA_DIR");
	char jp[4096];
	if (env) snprintf(jp, sizeof jp, "%s/themes_metadata.json", env);
	else snprintf(jp, sizeof jp, "data/themes_metadata.json");
	f = fopen(jp, "rb");
	if (!f) { snprintf(jp, sizeof jp, "/usr/share/onewm/themes_metadata.json"); f = fopen(jp, "rb"); }
	if (!f) return;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char *json = malloc(sz + 1);
	if (fread(json, 1, sz, f) != (size_t)sz) { fclose(f); free(json); return; }
	json[sz] = '\0'; fclose(f);

	/* find the theme object with matching id */
	char needle[128]; snprintf(needle, sizeof needle, "\"id\": \"%s\"", tid);
	const char *th = strstr(json, needle);
	if (!th) { free(json); return; }
	/* find the opening { before this id */
	const char *obj = th;
	while (obj > json && *(obj-1) != '{') obj--;

	/* extract primary, primaryVariant, background */
	const char *fields[3] = {"\"primary\"", "\"primaryVariant\"", "\"background\""};
	float *dests[3] = { cur_theme.primary, cur_theme.variant, cur_theme.background };
	for (int fi = 0; fi < 3; fi++) {
		const char *fk = strstr(obj, fields[fi]);
		if (!fk || fk > obj + 500) continue;
		const char *br = strchr(fk, '{');
		if (!br) continue;
		int r = -1, g = -1, b = -1;
		const char *rj = strstr(br, "\"r\"");
		const char *gj = strstr(br, "\"g\"");
		const char *bj = strstr(br, "\"b\"");
		if (rj) sscanf(strchr(rj, ':'), ": %d", &r);
		if (gj) sscanf(strchr(gj, ':'), ": %d", &g);
		if (bj) sscanf(strchr(bj, ':'), ": %d", &b);
		if (r >= 0) dests[fi][0] = r / 255.f;
		if (g >= 0) dests[fi][1] = g / 255.f;
		if (b >= 0) dests[fi][2] = b / 255.f;
	}
	free(json);
}

struct sel {
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
static struct sel ctx;

/* --- shared data helpers --- */

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

static void write_line(const char *path, const char *val) {
	FILE *f = fopen(path, "w");
	if (f) { fprintf(f, "%s\n", val); fclose(f); }
}

static void signal_desktop(int sig) {
	char pidp[4096]; config_path(pidp, sizeof pidp, "desktop.pid");
	char buf[32]; read_line(pidp, buf, sizeof buf);
	if (buf[0]) { pid_t pid = (pid_t)atoi(buf); if (pid > 0) kill(pid, sig); }
}

static void write_line_to_config(const char *v) {
	char p[4096]; config_path(p, sizeof p, "wallpaper");
	write_line(p, v);
}

static char *find_data(const char *rel) {
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
	snprintf(path, sizeof(path), "/usr/share/onewm/%s", rel);
	if (access(path, R_OK) == 0) return path;
	return NULL;
}

/* --- wallpaper list --- */

static char *wp_paths[128];
static char *wp_names[128];
static int wp_count = 0;

static void collect_wallpapers(void) {
	char dirs[8][4096];
	int n = 0;
	const char *base = getenv("ONEWM_DATA_DIR");
	if (base) { snprintf(dirs[n], sizeof dirs[n], "%s/wallpapers", base); n++; }
	snprintf(dirs[n], sizeof dirs[n], "data/wallpapers"); n++;
	snprintf(dirs[n], sizeof dirs[n], "/usr/share/onewm/wallpapers"); n++;
	const char *wd = getenv("ONEWM_WALLPAPER_DIR");
	if (wd) { snprintf(dirs[n], sizeof dirs[n], "%s", wd); n++; }

	for (int d = 0; d < n && wp_count < 128; d++) {
		DIR *dir = opendir(dirs[d]);
		if (!dir) continue;
		struct dirent *de;
		while ((de = readdir(dir)) && wp_count < 128) {
			size_t L = strlen(de->d_name);
			if (L < 4) continue;
			if (strcmp(de->d_name + L - 4, ".png") != 0) continue;
			char full[4096];
			snprintf(full, sizeof full, "%s/%s", dirs[d], de->d_name);
			wp_paths[wp_count] = strdup(full);
			char *nm = strdup(de->d_name);
			if (L > 4 && strcmp(nm + L - 4, ".png") == 0) nm[L - 4] = '\0';
			wp_names[wp_count] = nm;
			wp_count++;
		}
		closedir(dir);
	}
}

/* --- theme list --- */

struct theme_entry { char id[32]; char name[64]; float rgb[3]; };
static struct theme_entry themes[64];
static int theme_count = 0;

static void collect_themes(void) {
	const char *p = find_data("themes_metadata.json");
	if (!p) return;
	FILE *f = fopen(p, "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char *json = malloc(sz + 1);
	if (fread(json, 1, sz, f) != (size_t)sz) { fclose(f); free(json); return; }
	json[sz] = '\0';
	fclose(f);

	const char *arr = strstr(json, "\"themes\"");
	const char *q = arr ? strchr(arr, '[') : NULL;
	if (q) {
		q++;
		while (*q && theme_count < 64) {
			const char *obj = strchr(q, '{');
			if (!obj) break;
			const char *end = NULL; int dd = 0; bool st = false;
			for (const char *s = obj; *s; s++) {
				if (*s == '{') { dd++; st = true; }
				else if (*s == '}') { dd--; if (st && dd == 0) { end = s; break; } }
			}
			if (!end) break;
			struct theme_entry *te = &themes[theme_count];
			const char *k = strstr(obj, "\"id\"");
			if (k && k < end) {
				const char *s = strchr(k + 4, '"');
				if (s) { s++; const char *e = strchr(s, '"'); int l = e ? (int)(e - s) : 0; if (l > 31) l = 31; memcpy(te->id, s, l); te->id[l] = '\0'; }
			}
			k = strstr(obj, "\"name\"");
			if (k && k < end) {
				const char *s = strchr(k + 6, '"');
				if (s) { s++; const char *e = strchr(s, '"'); int l = e ? (int)(e - s) : 0; if (l > 63) l = 63; memcpy(te->name, s, l); te->name[l] = '\0'; }
			}
			te->rgb[0] = te->rgb[1] = te->rgb[2] = 0.6f;
			k = strstr(obj, "\"primary\"");
			if (k && k < end) {
				int r = -1, g = -1, b = -1;
				const char *rj = strstr(k, "\"r\"");
				const char *gj = strstr(k, "\"g\"");
				const char *bj = strstr(k, "\"b\"");
				if (rj) sscanf(strchr(rj, ':'), ": %d", &r);
				if (gj) sscanf(strchr(gj, ':'), ": %d", &g);
				if (bj) sscanf(strchr(bj, ':'), ": %d", &b);
				if (r >= 0) te->rgb[0] = r / 255.0f;
				if (g >= 0) te->rgb[1] = g / 255.0f;
				if (b >= 0) te->rgb[2] = b / 255.0f;
			}
			theme_count++;
			q = end + 1;
		}
	}
	free(json);
}

/* --- layout / hit testing (matches game's 84×72 grid) --- */

#define GRID_W 84
#define GRID_H 72
#define ICON_OFFSET_X 26
#define ICON_OFFSET_Y 6
#define ICON_SIZE 32
#define TEXT_Y 36
#define TEXT_MAX_W 76
#define PAD_X 4
#define PAD_Y 4
#define TITLE_BAR_H 20
#define SEPARATOR_H 2
#define CONTENT_Y (TITLE_BAR_H + SEPARATOR_H)

static int cell_at(int x, int y, int *out_idx) {
	int cx = (x - PAD_X) / GRID_W;
	int cy = (y - CONTENT_Y - PAD_Y) / GRID_H;
	if (cx < 0 || cy < 0) return 0;
	int cols = (ctx.width - 2 * PAD_X) / GRID_W;
	if (cols < 1) cols = 1;
	int idx = cy * cols + cx;
	if (sel_mode == MODE_WALLPAPERS) {
		if (idx < 0 || idx >= wp_count) return 0;
	} else {
		if (idx < 0 || idx >= theme_count) return 0;
	}
	*out_idx = idx;
	return 1;
}

/* --- drawing helpers --- */

static void cairo_rect(cairo_t *cr, double x, double y, double w, double h) {
	cairo_rectangle(cr, x, y, w, h);
}

/* premultiplied fill (for ARGB32 offscreen surfaces) */
static void fill_rect_premul(cairo_t *cr, double x, double y, double w, double h,
		float r, float g, float b, float a) {
	unsigned char pr = (unsigned char)(r * a * 255);
	unsigned char pg = (unsigned char)(g * a * 255);
	unsigned char pb = (unsigned char)(b * a * 255);
	unsigned char pa = (unsigned char)(a * 255);
	cairo_set_source_rgba(cr, pr/255.0, pg/255.0, pb/255.0, pa/255.0);
	cairo_rect(cr, x, y, w, h);
	cairo_fill(cr);
}

/* Draw text centered below an icon area, matching the game's style:
   black box behind text, text in theme primary color. */
static void draw_label(cairo_t *cr, const char *text,
		int cell_x, int cell_y, int cell_w) {
	PangoLayout *pl = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
	pango_layout_set_font_description(pl, fd);
	pango_layout_set_text(pl, text, -1);
	int tw, th;
	pango_layout_get_pixel_size(pl, &tw, &th);

	/* center text in the cell width */
	int tx = cell_x + (cell_w - tw) / 2;
	int ty = cell_y + TEXT_Y;

	/* black box behind text (matches GameColor.Black in FileIcon.Draw) */
	fill_rect_premul(cr, tx - 1, ty - 1, tw + 2, th + 2, 0, 0, 0, 1);

	/* text in primary color */
	cairo_set_source_rgba(cr,
		cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 1);
	cairo_move_to(cr, tx, ty);
	pango_cairo_show_layout(cr, pl);

	g_object_unref(pl);
	pango_font_description_free(fd);
}

/* --- drawing: wallpapers grid (matches BrowserWindow icon grid) --- */

static void draw_wallpapers(cairo_t *cr, int W, int H) {
	/* background: theme.Background() */
	fill_rect_premul(cr, 0, 0, W, H,
		cur_theme.background[0], cur_theme.background[1], cur_theme.background[2], 1);

	/* title bar background (slightly lighter) */
	fill_rect_premul(cr, 0, 0, W, TITLE_BAR_H,
		cur_theme.background[0] * 1.2f,
		cur_theme.background[1] * 1.2f,
		cur_theme.background[2] * 1.2f, 1);

	/* separator line: theme.Primary() */
	fill_rect_premul(cr, 0, TITLE_BAR_H, W, SEPARATOR_H,
		cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 1);

	/* title text */
	{
		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_text(pl, "Wallpapers", -1);
		cairo_set_source_rgba(cr,
			cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 1);
		cairo_move_to(cr, 8, 4);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		pango_font_description_free(fd);
	}

	/* count label (bottom-right) */
	{
		char cnt[32]; snprintf(cnt, sizeof cnt, "%d", wp_count);
		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_text(pl, cnt, -1);
		int tw, th; pango_layout_get_pixel_size(pl, &tw, &th);
		cairo_set_source_rgba(cr,
			cur_theme.variant[0], cur_theme.variant[1], cur_theme.variant[2], 1);
		cairo_move_to(cr, W - 8 - tw, H - 8 - th);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		pango_font_description_free(fd);
	}

	int hi = -1;
	if (ctx.px >= 0) cell_at(ctx.px, ctx.py, &hi);

	int cols = (W - 2 * PAD_X) / GRID_W;
	if (cols < 1) cols = 1;

	for (int i = 0; i < wp_count; i++) {
		int c = i % cols, r = i / cols;
		int cx = PAD_X + c * GRID_W;
		int cy = CONTENT_Y + PAD_Y + r * GRID_H;

		/* hover box: matches FileIcon.ClickAreaForIcon + Draw.
		   Outer: primary @ ½α. Inner: background @ ½α, shrunk by 2. */
		if (i == hi) {
			double bx = cx + 2, by = cy + 2;
			double bw = 80, bh = 36 + 12 + 6; /* textHeight=12 for single line */
			/* outer: primary @ ½α */
			fill_rect_premul(cr, bx, by, bw, bh,
				cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 0.5);
			/* inner: background @ ½α */
			fill_rect_premul(cr, bx + 2, by + 2, bw - 4, bh - 4,
				cur_theme.background[0], cur_theme.background[1], cur_theme.background[2], 0.5);
		}

		/* wallpaper thumbnail: 84×48 at 2× scale in the game,
		   but we render at cell size. Use 84×48 area centered in grid cell. */
		cairo_surface_t *img = cairo_image_surface_create_from_png(wp_paths[i]);
		if (img) {
			int iw = cairo_image_surface_get_width(img);
			int ih = cairo_image_surface_get_height(img);
			/* fit to 84×48 (game's wallpaper thumbnail size) */
			double sx = 84.0 / iw;
			double sy = 48.0 / ih;
			double s = fmin(sx, sy);
			int dw = (int)(iw * s);
			int ox = cx + (GRID_W - dw) / 2;
			int oy = cy + 6; /* WALLPAPER_ICON_OFFSET.Y */

			/* shadow at +2,+2: game draws the thumbnail silhouette in
			   theme.Background() (gameColor2). Use the image as a mask. */
			cairo_save(cr);
			cairo_translate(cr, ox + 2, oy + 2);
			cairo_scale(cr, s, s);
			cairo_set_source_rgba(cr,
				cur_theme.background[0], cur_theme.background[1],
				cur_theme.background[2], 0.4);
			cairo_mask_surface(cr, img, 0, 0);
			cairo_restore(cr);

			/* main thumbnail in white (game draws wallpaper icons with GameColor.White) */
			cairo_save(cr);
			cairo_translate(cr, ox, oy);
			cairo_scale(cr, s, s);
			cairo_set_source_surface(cr, img, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);

			cairo_surface_destroy(img);
		}

		draw_label(cr, wp_names[i], cx, cy, GRID_W);
	}
}

/* --- drawing: themes list (matches game's icon list style) --- */

static void draw_themes(cairo_t *cr, int W, int H) {
	/* background: theme.Background() */
	fill_rect_premul(cr, 0, 0, W, H,
		cur_theme.background[0], cur_theme.background[1], cur_theme.background[2], 1);

	/* title bar */
	fill_rect_premul(cr, 0, 0, W, TITLE_BAR_H,
		cur_theme.background[0] * 1.2f,
		cur_theme.background[1] * 1.2f,
		cur_theme.background[2] * 1.2f, 1);

	/* separator: theme.Primary() */
	fill_rect_premul(cr, 0, TITLE_BAR_H, W, SEPARATOR_H,
		cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 1);

	/* title text */
	{
		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_text(pl, "Themes", -1);
		cairo_set_source_rgba(cr,
			cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 1);
		cairo_move_to(cr, 8, 4);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		pango_font_description_free(fd);
	}

	/* count label */
	{
		char cnt[32]; snprintf(cnt, sizeof cnt, "%d", theme_count);
		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_text(pl, cnt, -1);
		int tw, th; pango_layout_get_pixel_size(pl, &tw, &th);
		cairo_set_source_rgba(cr,
			cur_theme.variant[0], cur_theme.variant[1], cur_theme.variant[2], 1);
		cairo_move_to(cr, W - 8 - tw, H - 8 - th);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		pango_font_description_free(fd);
	}

	int hi = -1;
	if (ctx.px >= 0) cell_at(ctx.px, ctx.py, &hi);

	for (int i = 0; i < theme_count; i++) {
		int cy = CONTENT_Y + PAD_Y + i * GRID_H;
		int cx = PAD_X;

		/* hover box: outer primary@½α, inner background@½α */
		if (i == hi) {
			double bx = cx + 2, by = cy + 2;
			double bw = W - 2 * PAD_X - 4;
			double bh = GRID_H - 4;
			fill_rect_premul(cr, bx, by, bw, bh,
				cur_theme.primary[0], cur_theme.primary[1], cur_theme.primary[2], 0.5);
			fill_rect_premul(cr, bx + 2, by + 2, bw - 4, bh - 4,
				cur_theme.background[0], cur_theme.background[1], cur_theme.background[2], 0.5);
		}

		/* theme color swatch (like FileIcon for theme files) */
		int sw = 32, sh = 32;
		int sx = cx + ICON_OFFSET_X;
		int sy = cy + ICON_OFFSET_Y;

		/* shadow */
		fill_rect_premul(cr, sx + 1, sy + 1, sw, sh,
			cur_theme.background[0], cur_theme.background[1], cur_theme.background[2], 0.6);
		/* swatch */
		fill_rect_premul(cr, sx, sy, sw, sh,
			themes[i].rgb[0], themes[i].rgb[1], themes[i].rgb[2], 1);

		/* theme name */
		draw_label(cr, themes[i].name, cx, cy, GRID_W);
	}
}

/* cairo solid-source ops are broken on the SHM for_data surface in this
   build, but work fine on a normal offscreen surface. Render to offscreen,
   then composite onto SHM with set_source_surface + paint. */
static cairo_surface_t *render_ui(int W, int H) {
	cairo_surface_t *ui = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
	cairo_t *cr = cairo_create(ui);
	if (sel_mode == MODE_WALLPAPERS) draw_wallpapers(cr, W, H);
	else draw_themes(cr, W, H);
	cairo_destroy(cr);
	return ui;
}

static void redraw(void) {
	if (!ctx.configured || ctx.buffers[0] == NULL) return;
	int idx = ctx.buf_idx;
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.width);
	memset(ctx.map[idx], 0, (size_t)stride * ctx.height);
	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		ctx.map[idx], CAIRO_FORMAT_ARGB32, ctx.width, ctx.height, stride);
	cairo_surface_t *ui = render_ui(ctx.width, ctx.height);
	cairo_t *cr = cairo_create(surf);
	cairo_set_source_surface(cr, ui, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(ui);
	cairo_surface_flush(surf);
	cairo_surface_destroy(surf);

	wl_surface_attach(ctx.surface, ctx.buffers[idx], 0, 0);
	wl_surface_damage_buffer(ctx.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(ctx.surface);
	ctx.buf_idx = 1 - ctx.buf_idx;
}

/* --- input --- */

static void choose(int idx) {
	if (sel_mode == MODE_WALLPAPERS) {
		write_line_to_config(wp_paths[idx]);
		signal_desktop(SIGUSR1);
	} else {
		char tp[4096]; config_path(tp, sizeof tp, "theme");
		write_line(tp, themes[idx].id);
		signal_desktop(SIGUSR2);
		/* Re-theme native toolkits (GTK/KDE/Qt) for the new theme. */
		pid_t pid = fork();
		if (pid == 0) {
			execl("/proc/self/exe", "onewm", "theme-apply", themes[idx].id, (char *)NULL);
			_exit(127);
		}
	}
	ctx.running = false;
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
	int idx = -1;
	if (cell_at(ctx.px, ctx.py, &idx)) choose(idx);
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
	char name[] = "/onewm-sel-XXXXXX";
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
			fprintf(stderr, "onewm-selectors: buffer alloc failed\n");
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
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d; (void)r; (void)n;
}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

int selectors_main(int argc, char **argv) {
	(void)argc; (void)argv;
	const char *sub = getenv("ONEWM_SUBCMD");
	sel_mode = (sub && strstr(sub, "wallpaper"))
		? MODE_WALLPAPERS : MODE_THEMES;

	memset(&ctx, 0, sizeof(ctx));
	ctx.px = -1; ctx.py = -1; ctx.running = true;

	load_theme();

	/* game's BrowserWindow is 360×200 contents. Scale up for modern screens. */
	if (sel_mode == MODE_WALLPAPERS) { collect_wallpapers(); ctx.width = 560; ctx.height = 440; }
	else { collect_themes(); ctx.width = 360; ctx.height = 440; }

	if (getenv("ONEWM_DUMP")) {
		ctx.px = 40; ctx.py = 50;
		cairo_surface_t *ui = render_ui(ctx.width, ctx.height);
		int W = ctx.width, H = ctx.height;
		int stride = cairo_image_surface_get_stride(ui);
		unsigned char *data = cairo_image_surface_get_data(ui);
		unsigned char *raw = malloc((size_t)W * H * 4 + H);
		unsigned char *rp = raw;
		for (int y = 0; y < H; y++) {
			*rp++ = 0;
			for (int x = 0; x < W; x++) {
				unsigned char *p = data + y * stride + x * 4;
				unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
				if (a && (r > a || g > a || b > a)) {
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
		FILE *fp = fopen("/tmp/selector_actual.png", "wb");
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
		free(raw); free(comp); cairo_surface_destroy(ui);
		return 0;
	}

	ctx.display = wl_display_connect(NULL);
	if (!ctx.display) { fprintf(stderr, "onewm-selector: no compositor\n"); return 1; }
	ctx.registry = wl_display_get_registry(ctx.display);
	wl_registry_add_listener(ctx.registry, &registry_listener, NULL);
	wl_display_roundtrip(ctx.display);
	if (!ctx.compositor || !ctx.shm || !ctx.layer_shell) { fprintf(stderr, "onewm-selector: missing globals\n"); return 1; }

	ctx.surface = wl_compositor_create_surface(ctx.compositor);
	ctx.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		ctx.layer_shell, ctx.surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "onewm-selector");
	zwlr_layer_surface_v1_set_keyboard_interactivity(ctx.layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	zwlr_layer_surface_v1_set_anchor(ctx.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(ctx.layer_surface, ctx.width, ctx.height);
	zwlr_layer_surface_v1_add_listener(ctx.layer_surface, &layer_surface_listener, NULL);
	wl_surface_commit(ctx.surface);

	while (ctx.running && wl_display_dispatch(ctx.display) != -1) {}

	for (int i = 0; i < 2; i++) {
		if (ctx.map[i]) munmap(ctx.map[i], ctx.size[i]);
		if (ctx.buffers[i]) wl_buffer_destroy(ctx.buffers[i]);
	}
	if (ctx.layer_surface) zwlr_layer_surface_v1_destroy(ctx.layer_surface);
	if (ctx.surface) wl_surface_destroy(ctx.surface);
	if (ctx.display) wl_display_disconnect(ctx.display);
	return 0;
}

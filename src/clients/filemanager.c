#define _GNU_SOURCE
#include <cairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pango/pangocairo.h>
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
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-protocol.h"

/* ── Game-accurate constants (from decompiled OneShotMG) ────────────── */
#define BROWSER_WIDTH   360
#define BROWSER_HEIGHT  200

#define GRID_ITEM_W     84
#define GRID_ITEM_H     72
#define ICON_TEXT_OFFSET 36
#define ICON_OFFSET_X   26
#define ICON_OFFSET_Y   6
#define TEXT_MAX_W      76
#define TEXT_LEFT_M     4
#define TEXT_H          12

#define WIN_BORDER      1
#define WIN_TOP         26
#define WIN_BAR_H       20
#define WIN_BTN_SIZE    16
#define WIN_BTN_GAP     2
#define WIN_ICON_LM     4
#define WIN_TITLE_LM    24

#define BREADCRUMB_H    22
#define BREADCRUMB_LM   24

#define SLIDER_W        17
#define SLIDER_BTN_H    16

#define GRID_PAD        4
#define DOUBLECLICK_F   30
#define SCROLL_PER_TICK 3

/* ── Theme ─────────────────────────────────────────────────────────── */
static float th_primary[3], th_bg[3], th_variant[3];

static void load_theme_defaults(void) {
	th_primary[0] = 150/255.f; th_primary[1] = 100/255.f; th_primary[2] = 255/255.f;
	th_variant[0] = 100/255.f; th_variant[1] = 66/255.f;  th_variant[2] = 165/255.f;
	th_bg[0] = th_bg[1] = th_bg[2] = 0.f;
}

static char *find_data(const char *rel) {
	static char path[4096];
	const char *env = getenv("ONEWM_DATA_DIR");
	if (env) { snprintf(path, sizeof path, "%s/%s", env, rel); if (access(path, R_OK)==0) return path; }
	snprintf(path, sizeof path, "data/%s", rel); if (access(path, R_OK)==0) return path;
	snprintf(path, sizeof path, "/usr/share/onewm/%s", rel); if (access(path, R_OK)==0) return path;
	return NULL;
}
static char *read_file(const char *p, size_t *len) {
	FILE *f=fopen(p,"rb"); if(!f) return NULL;
	fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
	char *b=malloc(sz+1); if(!b){fclose(f);return NULL;}
	if(fread(b,1,sz,f)!=(size_t)sz){fclose(f);free(b);return NULL;}
	b[sz]='\0'; if(len)*len=sz; fclose(f); return b;
}
static const char *brace_close(const char *o){int d=0;for(const char*p=o;*p;p++){if(*p=='{')d++;else if(*p=='}'){d--;if(!d)return p;}}return NULL;}
static void parse_rgb(const char *b,float*out){int r=-1,g=-1,bv=-1;const char*rj=strstr(b,"\"r\""),*gj=strstr(b,"\"g\""),*bj=strstr(b,"\"b\"");if(rj)sscanf(strchr(rj,':'),":%d",&r);if(gj)sscanf(strchr(gj,':'),":%d",&g);if(bj)sscanf(strchr(bj,':'),":%d",&bv);if(r>=0)out[0]=r/255.f;if(g>=0)out[1]=g/255.f;if(bv>=0)out[2]=bv/255.f;}
static void load_theme(const char *id) {
	load_theme_defaults();
	const char *p = find_data("themes_metadata.json"); if(!p) return;
	char *json = read_file(p, NULL); if(!json) return;
	const char *arr=strstr(json,"\"themes\""), *q=arr?strchr(arr,'['):NULL;
	if(q){q++;while(*q){const char*o=strchr(q,'{');if(!o)break;const char*e=brace_close(o);if(!e)break;char fid[32]={0};const char*k=strstr(o,"\"id\"");if(k&&k<e){const char*s=strchr(k+4,'"');if(s){s++;const char*en=strchr(s,'"');int l=en?(int)(en-s):0;if(l>31)l=31;memcpy(fid,s,l);fid[l]='\0';}}if(strcmp(fid,id)==0){k=strstr(o,"\"primary\"");if(k&&k<e){const char*oo=strchr(k,'{');if(oo)parse_rgb(oo,th_primary);}k=strstr(o,"\"primaryVariant\"");if(k&&k<e){const char*oo=strchr(k,'{');if(oo)parse_rgb(oo,th_variant);}k=strstr(o,"\"background\"");if(k&&k<e){const char*oo=strchr(k,'{');if(oo)parse_rgb(oo,th_bg);}break;}q=e+1;}}
	free(json);
}

/* ── File entry ────────────────────────────────────────────────────── */
enum file_type { FT_FOLDER, FT_FILE, FT_SYMLINK };
struct file_entry {
	char name[256];
	enum file_type type;
	off_t size;
};

/* ── Breadcrumb ────────────────────────────────────────────────────── */
struct crumb {
	char name[256];
	double x, w;
};

/* ── Client state ──────────────────────────────────────────────────── */
struct fm {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	int W, H;
	bool configured, running;
	struct wl_buffer *buffers[2];
	void *map[2];
	size_t map_sz[2];
	int buf_idx;
	/* input */
	int mx, my;
	/* window state */
	double win_x, win_y;
	bool dragging;
	double drag_ox, drag_oy;
	/* browser state */
	char cwd[4096];
	struct file_entry *entries;
	int entry_count, entry_cap;
	int sel;
	int scroll;
	int frames_since_click;
	int click_idx;
	bool show_hidden;
	/* breadcrumbs */
	struct crumb crumbs[64];
	int crumb_count;
	/* slider */
	int slider_grabbed;
	double slider_grab_y;
	double slider_thumb_y;
	/* hover */
	bool hover_close, hover_min, hover_up, hover_del;
	int hover_crumb;
	int hover_entry;
	bool hover_slider_up, hover_slider_down, hover_slider_thumb;
	/* delete confirm */
	bool delete_pending;
	char delete_name[256];
	/* title bar text */
	char title_text[512];
};
static struct fm ctx;

/* ── Helpers ───────────────────────────────────────────────────────── */
static void config_path(char *out, size_t n, const char *name) {
	const char *home = getenv("HOME"); if(!home) home="/tmp";
	snprintf(out, n, "%s/.config/onewm/%s", home, name);
}

static void build_title(void) {
	/* Show "~" for home, or abbreviated path */
	const char *home = getenv("HOME");
	if (home && strncmp(ctx.cwd, home, strlen(home)) == 0) {
		snprintf(ctx.title_text, sizeof ctx.title_text, "~%s", ctx.cwd + strlen(home));
	} else {
		strncpy(ctx.title_text, ctx.cwd, sizeof ctx.title_text - 1);
	}
}

static void set_cwd(const char *path) {
	strncpy(ctx.cwd, path, sizeof ctx.cwd - 1);
	ctx.cwd[sizeof ctx.cwd - 1] = '\0';
	/* normalize: remove trailing slash except for root */
	size_t l = strlen(ctx.cwd);
	if (l > 1 && ctx.cwd[l-1] == '/') ctx.cwd[--l] = '\0';
	ctx.sel = -1;
	ctx.scroll = 0;
	ctx.frames_since_click = DOUBLECLICK_F;
	ctx.delete_pending = false;
	build_title();
	/* build crumbs */
	ctx.crumb_count = 0;
	char tmp[4096]; strncpy(tmp, ctx.cwd, sizeof tmp);
	char *tok = strtok(tmp, "/");
	while (tok && ctx.crumb_count < 64) {
		strncpy(ctx.crumbs[ctx.crumb_count].name, tok, 255);
		ctx.crumb_count++;
		tok = strtok(NULL, "/");
	}
}

/* ── Filesystem reading ────────────────────────────────────────────── */
static int cmp_entries(const void *a, const void *b) {
	const struct file_entry *ea = a, *eb = b;
	if (ea->type == FT_FOLDER && eb->type != FT_FOLDER) return -1;
	if (ea->type != FT_FOLDER && eb->type == FT_FOLDER) return 1;
	return strcasecmp(ea->name, eb->name);
}

static void read_dir(void) {
	free(ctx.entries);
	ctx.entries = NULL;
	ctx.entry_count = 0;
	ctx.entry_cap = 0;

	DIR *d = opendir(ctx.cwd);
	if (!d) return;

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			if (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))
				continue;
			if (!ctx.show_hidden) continue;
		}
		if (ctx.entry_count >= ctx.entry_cap) {
			ctx.entry_cap = ctx.entry_cap ? ctx.entry_cap * 2 : 128;
			ctx.entries = realloc(ctx.entries, sizeof(*ctx.entries) * ctx.entry_cap);
		}
		struct file_entry *e = &ctx.entries[ctx.entry_count];
		strncpy(e->name, de->d_name, 255);
		e->name[255] = '\0';

		char full[4096];
		snprintf(full, sizeof full, "%s/%s", ctx.cwd, e->name);
		struct stat st;
		if (lstat(full, &st) == 0) {
			if (S_ISDIR(st.st_mode)) e->type = FT_FOLDER;
			else if (S_ISLNK(st.st_mode)) e->type = FT_SYMLINK;
			else e->type = FT_FILE;
			e->size = st.st_size;
		} else {
			e->type = FT_FILE;
			e->size = 0;
		}
		ctx.entry_count++;
	}
	closedir(d);
	qsort(ctx.entries, ctx.entry_count, sizeof(*ctx.entries), cmp_entries);
}

/* ── Grid math ─────────────────────────────────────────────────────── */
static int grid_cols(void) {
	int content_w = BROWSER_WIDTH - 8;
	return content_w / GRID_ITEM_W;
}
static int grid_rows_total(void) {
	if (ctx.entry_count == 0) return 0;
	int cols = grid_cols();
	return (ctx.entry_count + cols - 1) / cols;
}
static int grid_rows_visible(void) {
	int content_h = BROWSER_HEIGHT - BREADCRUMB_H - 2;
	return content_h / GRID_ITEM_H;
}
static int grid_max_scroll(void) {
	int total = grid_rows_total();
	int vis = grid_rows_visible();
	int m = total - vis;
	return m < 0 ? 0 : m;
}
static void clamp_scroll(void) {
	int m = grid_max_scroll();
	if (ctx.scroll > m) ctx.scroll = m;
	if (ctx.scroll < 0) ctx.scroll = 0;
}
static double grid_pos_x(int col) { return GRID_PAD + col * GRID_ITEM_W; }
static double grid_pos_y(int row) { return GRID_PAD + (row - ctx.scroll) * GRID_ITEM_H; }

static int entry_text_height(void) { return TEXT_H; }

static bool entry_click_hit(int col, int row, double px, double py) {
	int idx = row * grid_cols() + col;
	if (idx < 0 || idx >= ctx.entry_count) return false;
	double ix = grid_pos_x(col), iy = grid_pos_y(row);
	int th = entry_text_height();
	double x1 = ix + 2, y1 = iy + 2;
	double w = 80, h = 36 + th + 2 + 4;
	return px >= x1 && px <= x1 + w && py >= y1 && py <= y1 + h;
}

/* ── Cairo drawing helpers ─────────────────────────────────────────── */
static void fill_rect(cairo_t *cr, double x, double y, double w, double h,
		float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static PangoLayout *make_text(cairo_t *cr, const char *text, int max_w) {
	PangoLayout *pl = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
	pango_layout_set_font_description(pl, fd);
	pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
	if (max_w > 0) pango_layout_set_width(pl, max_w * PANGO_SCALE);
	pango_layout_set_text(pl, text, -1);
	pango_font_description_free(fd);
	return pl;
}

static int text_width(cairo_t *cr, const char *text) {
	PangoLayout *pl = make_text(cr, text, -1);
	int tw;
	pango_layout_get_pixel_size(pl, &tw, NULL);
	g_object_unref(pl);
	return tw;
}

/* Draw folder icon — 32×32 */
static void draw_folder(cairo_t *cr, double x, double y, float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r, g, b, a);
	/* body */
	cairo_rectangle(cr, x + 2, y + 12, 28, 20);
	cairo_fill(cr);
	/* tab */
	cairo_move_to(cr, x + 2, y + 12);
	cairo_line_to(cr, x + 2, y + 6);
	cairo_line_to(cr, x + 12, y + 6);
	cairo_line_to(cr, x + 14, y + 12);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* Draw file icon — page with folded corner */
static void draw_file(cairo_t *cr, double x, double y, float r, float g, float b, float a) {
	double px = x + 6, py = y + 4, pw = 20, ph = 26;
	/* body */
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_rectangle(cr, px, py, pw, ph);
	cairo_fill(cr);
	/* fold: cut triangle from bg */
	cairo_move_to(cr, px + pw - 6, py);
	cairo_line_to(cr, px + pw, py + 6);
	cairo_line_to(cr, px + pw - 6, py + 6);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, th_bg[0], th_bg[1], th_bg[2], a);
	cairo_fill(cr);
	/* fold lines */
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_set_line_width(cr, 1);
	cairo_move_to(cr, px + pw - 6, py);
	cairo_line_to(cr, px + pw - 6, py + 6);
	cairo_line_to(cr, px + pw, py + 6);
	cairo_stroke(cr);
}

/* Draw folder-up arrow 16×16 */
static void draw_arrow_up(cairo_t *cr, double x, double y, float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_set_line_width(cr, 2);
	cairo_move_to(cr, x + 8, y + 3);
	cairo_line_to(cr, x + 8, y + 13);
	cairo_stroke(cr);
	cairo_move_to(cr, x + 4, y + 7);
	cairo_line_to(cr, x + 8, y + 3);
	cairo_line_to(cr, x + 12, y + 7);
	cairo_stroke(cr);
}

/* Draw trash icon 16×16 */
static void draw_trash(cairo_t *cr, double x, double y, float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_set_line_width(cr, 1.5);
	/* lid */
	cairo_rectangle(cr, x + 3, y + 2, 10, 2);
	cairo_fill(cr);
	/* handle */
	cairo_move_to(cr, x + 6, y + 1);
	cairo_line_to(cr, x + 10, y + 1);
	cairo_stroke(cr);
	/* body */
	cairo_move_to(cr, x + 4, y + 5);
	cairo_line_to(cr, x + 5, y + 14);
	cairo_line_to(cr, x + 11, y + 14);
	cairo_line_to(cr, x + 12, y + 5);
	cairo_close_path(cr);
	cairo_stroke(cr);
	/* lines */
	cairo_move_to(cr, x + 7, y + 7);
	cairo_line_to(cr, x + 7, y + 12);
	cairo_move_to(cr, x + 9, y + 7);
	cairo_line_to(cr, x + 9, y + 12);
	cairo_stroke(cr);
}

/* Draw close X 16×16 */
static void draw_close_x(cairo_t *cr, double x, double y, float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_set_line_width(cr, 2);
	cairo_move_to(cr, x + 4, y + 4);
	cairo_line_to(cr, x + 12, y + 12);
	cairo_move_to(cr, x + 12, y + 4);
	cairo_line_to(cr, x + 4, y + 12);
	cairo_stroke(cr);
}

/* Draw minimize — 16×16 */
static void draw_min_line(cairo_t *cr, double x, double y, float r, float g, float b, float a) {
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_set_line_width(cr, 2);
	cairo_move_to(cr, x + 4, y + 8);
	cairo_line_to(cr, x + 12, y + 8);
	cairo_stroke(cr);
}

/* ── Format file size ──────────────────────────────────────────────── */
static void fmt_size(off_t sz, char *out, int max) {
	if (sz < 1024) snprintf(out, max, "%ldB", (long)sz);
	else if (sz < 1024*1024) snprintf(out, max, "%.1fK", sz/1024.0);
	else if (sz < 1024*1024*1024) snprintf(out, max, "%.1fM", sz/(1024.0*1024));
	else snprintf(out, max, "%.1fG", sz/(1024.0*1024*1024));
}

/* ── Render ────────────────────────────────────────────────────────── */
static void render(cairo_t *cr) {
	int W = ctx.W, H = ctx.H;
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	fill_rect(cr, 0, 0, W, H, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double wx = ctx.win_x, wy = ctx.win_y;

	/* ── Window outer border (primary) ── */
	fill_rect(cr, wx, wy, BROWSER_WIDTH + 2 * WIN_BORDER, BROWSER_HEIGHT + WIN_TOP + 2 * WIN_BORDER,
		th_primary[0], th_primary[1], th_primary[2], 1.0);
	/* ── Title bar (background) ── */
	fill_rect(cr, wx + WIN_BORDER, wy + WIN_BORDER,
		BROWSER_WIDTH, WIN_TOP,
		th_bg[0], th_bg[1], th_bg[2], 1.0);

	/* ── Window folder icon in title ── */
	draw_folder(cr, wx + WIN_ICON_LM, wy + WIN_ICON_LM,
		th_primary[0], th_primary[1], th_primary[2], 1.0);

	/* ── Title text ── */
	{
		int max_title_w = BROWSER_WIDTH - 24 - 2 * 18;
		PangoLayout *pl = make_text(cr, ctx.title_text, max_title_w);
		cairo_set_source_rgba(cr, th_primary[0], th_primary[1], th_primary[2], 1.0);
		cairo_move_to(cr, wx + WIN_TITLE_LM, wy + 3);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
	}

	/* ── Window buttons ── */
	double btn_y = wy + WIN_BORDER + 2;
	double close_x = wx + BROWSER_WIDTH + WIN_BORDER - WIN_BTN_SIZE - 2;
	double min_x = close_x - WIN_BTN_SIZE - WIN_BTN_GAP;

	/* close */
	fill_rect(cr, close_x, btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE,
		th_bg[0], th_bg[1], th_bg[2], 1.0);
	draw_close_x(cr, close_x, btn_y,
		ctx.hover_close ? th_variant[0] : th_primary[0],
		ctx.hover_close ? th_variant[1] : th_primary[1],
		ctx.hover_close ? th_variant[2] : th_primary[2], 1.0);
	/* minimize */
	fill_rect(cr, min_x, btn_y, WIN_BTN_SIZE, WIN_BTN_SIZE,
		th_bg[0], th_bg[1], th_bg[2], 1.0);
	draw_min_line(cr, min_x, btn_y,
		ctx.hover_min ? th_variant[0] : th_primary[0],
		ctx.hover_min ? th_variant[1] : th_primary[1],
		ctx.hover_min ? th_variant[2] : th_primary[2], 1.0);

	/* ── Content area ── */
	double cx = wx + WIN_BORDER, cy = wy + WIN_TOP;
	fill_rect(cr, cx, cy, BROWSER_WIDTH, BROWSER_HEIGHT,
		th_bg[0], th_bg[1], th_bg[2], 1.0);

	/* ── Breadcrumb bar ── */
	double bx = cx, by = cy;
	fill_rect(cr, bx, by, BROWSER_WIDTH, BREADCRUMB_H,
		th_bg[0], th_bg[1], th_bg[2], 1.0);
	/* separator */
	fill_rect(cr, bx, by + BREADCRUMB_H - 2, BROWSER_WIDTH, 2,
		th_primary[0], th_primary[1], th_primary[2], 1.0);

	/* folder-up button */
	double up_x = bx + 4, up_y = by + (BREADCRUMB_H - 16) / 2;
	draw_arrow_up(cr, up_x, up_y,
		ctx.hover_up ? th_variant[0] : th_primary[0],
		ctx.hover_up ? th_variant[1] : th_primary[1],
		ctx.hover_up ? th_variant[2] : th_primary[2], 1.0);

	/* trash button */
	double del_x = bx + BROWSER_WIDTH - 20, del_y = up_y;
	draw_trash(cr, del_x, del_y,
		ctx.hover_del ? th_variant[0] : th_primary[0],
		ctx.hover_del ? th_variant[1] : th_primary[1],
		ctx.hover_del ? th_variant[2] : th_primary[2], 1.0);

	/* breadcrumb segments */
	{
		int sw = text_width(cr, "/");
		double crumb_x = bx + BREADCRUMB_LM;
		for (int i = 0; i < ctx.crumb_count; i++) {
			int tw = text_width(cr, ctx.crumbs[i].name);
			double cw = tw + 6;
			ctx.crumbs[i].x = crumb_x;
			ctx.crumbs[i].w = cw;
			bool hov = (i == ctx.hover_crumb);

			/* "/" before segment */
			cairo_set_source_rgba(cr,
				hov ? th_primary[0] : th_bg[0],
				hov ? th_primary[1] : th_bg[1],
				hov ? th_primary[2] : th_bg[2], 1.0);
			PangoLayout *sl = make_text(cr, "/", -1);
			cairo_move_to(cr, crumb_x - sw - 1, by + 2);
			pango_cairo_show_layout(cr, sl);
			g_object_unref(sl);

			/* background rect */
			fill_rect(cr, crumb_x, by + 2, cw, 16,
				hov ? th_primary[0] : th_bg[0],
				hov ? th_primary[1] : th_bg[1],
				hov ? th_primary[2] : th_bg[2], 1.0);
			/* inner */
			fill_rect(cr, crumb_x + 1, by + 3, cw - 2, 14,
				hov ? th_bg[0] : th_primary[0],
				hov ? th_bg[1] : th_primary[1],
				hov ? th_bg[2] : th_primary[2], 1.0);
			/* text */
			PangoLayout *pl = make_text(cr, ctx.crumbs[i].name, -1);
			cairo_set_source_rgba(cr,
				hov ? th_bg[0] : th_primary[0],
				hov ? th_bg[1] : th_primary[1],
				hov ? th_bg[2] : th_primary[2], 1.0);
			cairo_move_to(cr, crumb_x + 3, by + 4);
			pango_cairo_show_layout(cr, pl);
			g_object_unref(pl);

			crumb_x += cw + sw + 2;
		}
	}

	/* ── Icon grid ── */
	int cols = grid_cols();
	int vis_rows = grid_rows_visible();
	double grid_x = cx, grid_y = cy + BREADCRUMB_H;

	for (int row = ctx.scroll; row < ctx.scroll + vis_rows && row < grid_rows_total(); row++) {
		for (int col = 0; col < cols; col++) {
			int idx = row * cols + col;
			if (idx >= ctx.entry_count) break;
			struct file_entry *e = &ctx.entries[idx];
			double ix = grid_x + grid_pos_x(col);
			double iy = grid_y + grid_pos_y(row);
			bool focused = (idx == ctx.sel);
			bool hovered = (idx == ctx.hover_entry);

			/* focus / hover box */
			if (focused || hovered) {
				double cx2 = ix + 2, cy2 = iy + 2;
				double cw = 80, ch = 36 + entry_text_height() + 2 + 4;
				float or_ = focused ? th_primary[0] : th_variant[0];
				float og = focused ? th_primary[1] : th_variant[1];
				float ob = focused ? th_primary[2] : th_variant[2];
				fill_rect(cr, cx2, cy2, cw, ch, or_, og, ob, 0.5);
				fill_rect(cr, cx2 + 2, cy2 + 2, cw - 4, ch - 4,
					th_bg[0], th_bg[1], th_bg[2], 0.5);
			}

			/* icon */
			double icon_x = ix + ICON_OFFSET_X, icon_y = iy + ICON_OFFSET_Y;
			if (e->type == FT_FOLDER) {
				draw_folder(cr, icon_x + 1, icon_y + 1,
					th_bg[0], th_bg[1], th_bg[2], 1.0);
				draw_folder(cr, icon_x, icon_y,
					th_primary[0], th_primary[1], th_primary[2], 1.0);
			} else {
				draw_file(cr, icon_x + 1, icon_y + 1,
					th_bg[0], th_bg[1], th_bg[2], 1.0);
				draw_file(cr, icon_x, icon_y,
					th_primary[0], th_primary[1], th_primary[2], 1.0);
			}

			/* text label */
			double text_x = ix + TEXT_LEFT_M - 1;
			double text_y = iy + ICON_TEXT_OFFSET;
			int th = entry_text_height();
			fill_rect(cr, text_x, text_y, TEXT_MAX_W + 2, th + 2,
				0, 0, 0, 1.0);
			PangoLayout *pl = make_text(cr, e->name, TEXT_MAX_W);
			cairo_set_source_rgba(cr,
				th_primary[0], th_primary[1], th_primary[2], 1.0);
			cairo_move_to(cr, text_x + 1, text_y + 1);
			pango_cairo_show_layout(cr, pl);
			g_object_unref(pl);
		}
	}

	/* ── Status line (item count + hidden toggle) ── */
	{
		char status[128];
		if (ctx.show_hidden)
			snprintf(status, sizeof status, "%d items (showing hidden)", ctx.entry_count);
		else
			snprintf(status, sizeof status, "%d items", ctx.entry_count);
		PangoLayout *pl = make_text(cr, status, -1);
		cairo_set_source_rgba(cr,
			th_variant[0], th_variant[1], th_variant[2], 0.6);
		cairo_move_to(cr, cx + 4, cy + BROWSER_HEIGHT - 16);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
	}

	/* ── Delete confirmation overlay ── */
	if (ctx.delete_pending) {
		/* dim background */
		fill_rect(cr, cx, cy, BROWSER_WIDTH, BROWSER_HEIGHT,
			0, 0, 0, 0.6);
		/* dialog box */
		double dw = 280, dh = 80;
		double dx = cx + (BROWSER_WIDTH - dw) / 2;
		double dy = cy + (BROWSER_HEIGHT - dh) / 2;
		/* outer border */
		fill_rect(cr, dx, dy, dw, dh,
			th_primary[0], th_primary[1], th_primary[2], 1.0);
		/* inner bg */
		fill_rect(cr, dx + 2, dy + 2, dw - 4, dh - 4,
			th_bg[0], th_bg[1], th_bg[2], 1.0);
		/* message */
		char msg[512];
		snprintf(msg, sizeof msg, "Delete \"%s\"?", ctx.delete_name);
		PangoLayout *pl = make_text(cr, msg, dw - 20);
		cairo_set_source_rgba(cr,
			th_primary[0], th_primary[1], th_primary[2], 1.0);
		cairo_move_to(cr, dx + 10, dy + 12);
		pango_cairo_show_layout(cr, pl);
		g_object_unref(pl);
		/* Yes / No buttons */
		double by2 = dy + dh - 26;
		/* Yes */
		fill_rect(cr, dx + 40, by2, 80, 20,
			th_primary[0], th_primary[1], th_primary[2], 1.0);
		PangoLayout *yp = make_text(cr, "Yes", -1);
		cairo_set_source_rgba(cr, th_bg[0], th_bg[1], th_bg[2], 1.0);
		cairo_move_to(cr, dx + 40 + (80 - text_width(cr, "Yes"))/2, by2 + 3);
		pango_cairo_show_layout(cr, yp);
		g_object_unref(yp);
		/* No */
		fill_rect(cr, dx + 140, by2, 80, 20,
			th_primary[0], th_primary[1], th_primary[2], 1.0);
		PangoLayout *np = make_text(cr, "No", -1);
		cairo_set_source_rgba(cr, th_bg[0], th_bg[1], th_bg[2], 1.0);
		cairo_move_to(cr, dx + 140 + (80 - text_width(cr, "No"))/2, by2 + 3);
		pango_cairo_show_layout(cr, np);
		g_object_unref(np);
	}

	/* ── Scrollbar ── */
	if (grid_rows_total() > vis_rows) {
		double sx = cx + BROWSER_WIDTH - SLIDER_W;
		double sy = by + BREADCRUMB_H;
		double slider_area_h = BROWSER_HEIGHT - BREADCRUMB_H - 2;
		double btn_h = SLIDER_BTN_H;

		/* up button */
		fill_rect(cr, sx, sy, SLIDER_W, btn_h,
			th_bg[0], th_bg[1], th_bg[2], 1.0);
		draw_arrow_up(cr, sx + (SLIDER_W-16)/2, sy,
			ctx.hover_slider_up ? th_variant[0] : th_primary[0],
			ctx.hover_slider_up ? th_variant[1] : th_primary[1],
			ctx.hover_slider_up ? th_variant[2] : th_primary[2], 1.0);

		/* down button */
		double dy = sy + slider_area_h - btn_h;
		fill_rect(cr, sx, dy, SLIDER_W, btn_h,
			th_bg[0], th_bg[1], th_bg[2], 1.0);
		cairo_save(cr);
		cairo_translate(cr, sx + SLIDER_W/2, dy + btn_h/2);
		cairo_rotate(cr, M_PI);
		draw_arrow_up(cr, -8, -8,
			ctx.hover_slider_down ? th_variant[0] : th_primary[0],
			ctx.hover_slider_down ? th_variant[1] : th_primary[1],
			ctx.hover_slider_down ? th_variant[2] : th_primary[2], 1.0);
		cairo_restore(cr);

		/* thumb */
		double track_y = sy + btn_h;
		double track_h = slider_area_h - 2 * btn_h;
		int total_rows = grid_rows_total();
		double thumb_h = track_h * ((double)vis_rows / total_rows);
		if (thumb_h < 16) thumb_h = 16;
		double max_s = grid_max_scroll();
		double thumb_y = track_y;
		if (max_s > 0)
			thumb_y = track_y + ((double)ctx.scroll / max_s) * (track_h - thumb_h);
		ctx.slider_thumb_y = thumb_y;

		/* track groove */
		fill_rect(cr, sx + 4, track_y, SLIDER_W - 8, track_h,
			th_primary[0], th_primary[1], th_primary[2], 0.15);

		/* thumb */
		fill_rect(cr, sx, thumb_y, SLIDER_W, thumb_h,
			ctx.hover_slider_thumb ? th_variant[0] : th_primary[0],
			ctx.hover_slider_thumb ? th_variant[1] : th_primary[1],
			ctx.hover_slider_thumb ? th_variant[2] : th_primary[2], 1.0);
		fill_rect(cr, sx + 2, thumb_y + 2, SLIDER_W - 4, thumb_h - 4,
			th_bg[0], th_bg[1], th_bg[2], 1.0);
	}
}

/* ── Buffer / SHM ─────────────────────────────────────────────────── */
static bool create_buffers(void) {
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.W);
	size_t sz = (size_t)stride * ctx.H;
	long ps = sysconf(_SC_PAGESIZE); if (ps < 1) ps = 4096;
	size_t sz_a = (sz + ps - 1) & ~(size_t)(ps - 1);
	size_t total = sz_a * 2;
	char nm[] = "/onewm-fm-XXXXXX";
	int fd = memfd_create(nm, MFD_CLOEXEC);
	if (fd < 0) return false;
	if (ftruncate(fd, total) < 0) { close(fd); return false; }
	for (int i = 0; i < 2; i++) {
		ctx.map[i] = mmap(NULL, sz_a, PROT_READ|PROT_WRITE, MAP_SHARED, fd, i*sz_a);
		if (ctx.map[i] == MAP_FAILED) { close(fd); return false; }
		ctx.map_sz[i] = sz_a;
		memset(ctx.map[i], 0, sz);
		struct wl_shm_pool *pool = wl_shm_create_pool(ctx.shm, fd, total);
		ctx.buffers[i] = wl_shm_pool_create_buffer(pool, i*sz_a, ctx.W, ctx.H, stride, WL_SHM_FORMAT_ARGB8888);
		wl_shm_pool_destroy(pool);
	}
	close(fd);
	return true;
}

static void do_redraw(void) {
	if (!ctx.configured || !ctx.buffers[0]) return;
	int idx = ctx.buf_idx;
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, ctx.W);
	memset(ctx.map[idx], 0, (size_t)stride * ctx.H);
	cairo_surface_t *shm_s = cairo_image_surface_create_for_data(
		ctx.map[idx], CAIRO_FORMAT_ARGB32, ctx.W, ctx.H, stride);
	cairo_surface_t *off = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ctx.W, ctx.H);
	cairo_t *cr = cairo_create(off);
	render(cr);
	cairo_destroy(cr);
	cairo_t *cr2 = cairo_create(shm_s);
	cairo_set_source_surface(cr2, off, 0, 0);
	cairo_paint(cr2);
	cairo_destroy(cr2);
	cairo_surface_destroy(off);
	cairo_surface_flush(shm_s);
	cairo_surface_destroy(shm_s);
	wl_surface_attach(ctx.surface, ctx.buffers[idx], 0, 0);
	wl_surface_damage_buffer(ctx.surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(ctx.surface);
	ctx.buf_idx = 1 - ctx.buf_idx;
}

/* ── Navigation ────────────────────────────────────────────────────── */
static int entry_at_pos(double px, double py) {
	double cx2 = ctx.win_x + WIN_BORDER;
	double cy2 = ctx.win_y + WIN_TOP + BREADCRUMB_H;
	double lx = px - cx2, ly = py - cy2;
	if (lx < 0 || ly < 0) return -1;
	int col = (int)(lx / GRID_ITEM_W);
	int row = (int)(ly / GRID_ITEM_H) + ctx.scroll;
	int cols = grid_cols();
	if (col >= cols) return -1;
	int idx = row * cols + col;
	if (idx < 0 || idx >= ctx.entry_count) return -1;
	if (!entry_click_hit(col, row, lx, ly)) return -1;
	return idx;
}

static void navigate_to(const char *path) {
	set_cwd(path);
	read_dir();
}

static void navigate_up(void) {
	if (strcmp(ctx.cwd, "/") == 0) return;
	char tmp[4096];
	strncpy(tmp, ctx.cwd, sizeof tmp);
	size_t l = strlen(tmp);
	if (l > 1 && tmp[l-1] == '/') tmp[--l] = '\0';
	char *sl = strrchr(tmp, '/');
	if (sl && sl == tmp) { tmp[1] = '\0'; }
	else if (sl) *sl = '\0';
	navigate_to(tmp);
}

static void open_entry(int idx) {
	if (idx < 0 || idx >= ctx.entry_count) return;
	struct file_entry *e = &ctx.entries[idx];
	if (e->type == FT_FOLDER) {
		char new_path[4096];
		snprintf(new_path, sizeof new_path, "%s/%s", ctx.cwd, e->name);
		navigate_to(new_path);
	} else {
		char cmd[4096];
		snprintf(cmd, sizeof cmd, "xdg-open '%s/%s' 2>/dev/null &", ctx.cwd, e->name);
		system(cmd);
	}
}

static void delete_entry(const char *name) {
	char path[4096];
	snprintf(path, sizeof path, "%s/%s", ctx.cwd, name);
	struct stat st;
	if (lstat(path, &st) != 0) return;
	if (S_ISDIR(st.st_mode)) {
		char cmd[4096];
		snprintf(cmd, sizeof cmd, "rm -rf '%s' 2>/dev/null", path);
		system(cmd);
	} else {
		unlink(path);
	}
	/* refresh */
	ctx.sel = -1;
	read_dir();
}

static void handle_click(double px, double py) {
	double wx = ctx.win_x, wy = ctx.win_y;

	/* ── Delete confirmation ── */
	if (ctx.delete_pending) {
		double cx2 = wx + WIN_BORDER, cy2 = wy + WIN_TOP;
		double dw = 280, dh = 80;
		double dx = cx2 + (BROWSER_WIDTH - dw) / 2;
		double dy = cy2 + (BROWSER_HEIGHT - dh) / 2;
		double by2 = dy + dh - 26;
		/* Yes */
		if (px >= dx + 40 && px <= dx + 120 && py >= by2 && py <= by2 + 20) {
			delete_entry(ctx.delete_name);
			ctx.delete_pending = false;
			return;
		}
		/* No */
		if (px >= dx + 140 && px <= dx + 220 && py >= by2 && py <= by2 + 20) {
			ctx.delete_pending = false;
			return;
		}
		return;
	}

	/* ── Close ── */
	double close_x = wx + BROWSER_WIDTH + WIN_BORDER - WIN_BTN_SIZE - 2;
	double btn_y = wy + WIN_BORDER + 2;
	if (px >= close_x && px <= close_x + WIN_BTN_SIZE &&
	    py >= btn_y && py <= btn_y + WIN_BTN_SIZE) {
		ctx.running = false;
		return;
	}

	/* ── Minimize ── */
	double min_x = close_x - WIN_BTN_SIZE - WIN_BTN_GAP;
	if (px >= min_x && px <= min_x + WIN_BTN_SIZE &&
	    py >= btn_y && py <= btn_y + WIN_BTN_SIZE) {
		ctx.win_y = -9999;
		do_redraw();
		return;
	}

	/* ── Breadcrumb area ── */
	double bx = wx + WIN_BORDER;
	double by = wy + WIN_TOP;
	if (py >= by && py < by + BREADCRUMB_H) {
		double up_x = bx + 4, up_y2 = by + (BREADCRUMB_H - 16)/2;
		if (px >= up_x && px <= up_x + 16 && py >= up_y2 && py <= up_y2 + 16) {
			navigate_up();
			return;
		}
		for (int i = 0; i < ctx.crumb_count; i++) {
			double crx = ctx.crumbs[i].x;
			if (px >= crx && px <= crx + ctx.crumbs[i].w && py >= by+2 && py <= by+18) {
				char path[4096] = "/";
				for (int j = 0; j <= i; j++) {
					strcat(path, ctx.crumbs[j].name);
					strcat(path, "/");
				}
				navigate_to(path);
				return;
			}
		}
		return;
	}

	/* ── Trash button ── */
	if (ctx.sel >= 0 && ctx.sel < ctx.entry_count) {
		double bx2 = wx + WIN_BORDER;
		double by2 = wy + WIN_TOP;
		double del_x = bx2 + BROWSER_WIDTH - 20;
		double del_y = by2 + (BREADCRUMB_H - 16)/2;
		if (px >= del_x && px <= del_x + 16 && py >= del_y && py <= del_y + 16) {
			struct file_entry *e = &ctx.entries[ctx.sel];
			strncpy(ctx.delete_name, e->name, sizeof ctx.delete_name - 1);
			ctx.delete_pending = true;
			do_redraw();
			return;
		}
	}

	/* ── Scrollbar ── */
	int vis = grid_rows_visible();
	if (grid_rows_total() > vis) {
		double sx = bx + BROWSER_WIDTH - SLIDER_W;
		double sy = by + BREADCRUMB_H;
		double slider_area_h = BROWSER_HEIGHT - BREADCRUMB_H - 2;
		double btn_h = SLIDER_BTN_H;

		if (px >= sx && px <= sx + SLIDER_W && py >= sy && py <= sy + btn_h) {
			ctx.scroll--;
			clamp_scroll();
			return;
		}
		double dy = sy + slider_area_h - btn_h;
		if (px >= sx && px <= sx + SLIDER_W && py >= dy && py <= dy + btn_h) {
			ctx.scroll++;
			clamp_scroll();
			return;
		}
		double track_y = sy + btn_h;
		double track_h = slider_area_h - 2 * btn_h;
		if (px >= sx && px <= sx + SLIDER_W && py >= track_y && py <= track_y + track_h) {
			double click_ratio = (py - track_y) / track_h;
			ctx.scroll = (int)(click_ratio * grid_max_scroll() + 0.5);
			clamp_scroll();
			return;
		}
	}

	/* ── Grid click ── */
	int idx = entry_at_pos(px, py);
	if (idx >= 0) {
		if (ctx.frames_since_click < DOUBLECLICK_F && idx == ctx.click_idx) {
			open_entry(idx);
			ctx.frames_since_click = DOUBLECLICK_F;
		} else {
			ctx.frames_since_click = 0;
			ctx.click_idx = idx;
			ctx.sel = idx;
		}
	} else {
		ctx.sel = -1;
	}
}

/* ── Hover tracking ────────────────────────────────────────────────── */
static void update_hover(double px, double py) {
	double wx = ctx.win_x, wy = ctx.win_y;
	double close_x = wx + BROWSER_WIDTH + WIN_BORDER - WIN_BTN_SIZE - 2;
	double min_x = close_x - WIN_BTN_SIZE - WIN_BTN_GAP;
	double btn_y = wy + WIN_BORDER + 2;
	ctx.hover_close = (px >= close_x && px <= close_x + WIN_BTN_SIZE &&
			   py >= btn_y && py <= btn_y + WIN_BTN_SIZE);
	ctx.hover_min = (px >= min_x && px <= min_x + WIN_BTN_SIZE &&
			   py >= btn_y && py <= btn_y + WIN_BTN_SIZE);

	double bx = wx + WIN_BORDER;
	double by = wy + WIN_TOP;
	double up_x = bx + 4, up_y = by + (BREADCRUMB_H - 16)/2;
	ctx.hover_up = (px >= up_x && px <= up_x + 16 && py >= up_y && py <= up_y + 16);
	double del_x = bx + BROWSER_WIDTH - 20;
	ctx.hover_del = (px >= del_x && px <= del_x + 16 && py >= up_y && py <= up_y + 16);

	ctx.hover_crumb = -1;
	if (py >= by && py < by + BREADCRUMB_H) {
		for (int i = 0; i < ctx.crumb_count; i++) {
			if (px >= ctx.crumbs[i].x && px <= ctx.crumbs[i].x + ctx.crumbs[i].w) {
				ctx.hover_crumb = i;
				break;
			}
		}
	}

	ctx.hover_entry = entry_at_pos(px, py);

	int vis = grid_rows_visible();
	ctx.hover_slider_up = ctx.hover_slider_down = ctx.hover_slider_thumb = false;
	if (grid_rows_total() > vis) {
		double sx = bx + BROWSER_WIDTH - SLIDER_W;
		double sy = by + BREADCRUMB_H;
		double slider_area_h = BROWSER_HEIGHT - BREADCRUMB_H - 2;
		double btn_h = SLIDER_BTN_H;
		if (px >= sx && px <= sx + SLIDER_W) {
			if (py >= sy && py <= sy + btn_h) ctx.hover_slider_up = true;
			double dy = sy + slider_area_h - btn_h;
			if (py >= dy && py <= dy + btn_h) ctx.hover_slider_down = true;
			double track_y = sy + btn_h;
			double track_h = slider_area_h - 2 * btn_h;
			double thumb_h = track_h * ((double)vis / grid_rows_total());
			if (thumb_h < 16) thumb_h = 16;
			double max_s = grid_max_scroll();
			double thumb_y = track_y;
			if (max_s > 0) thumb_y = track_y + ((double)ctx.scroll / max_s) * (track_h - thumb_h);
			if (py >= thumb_y && py <= thumb_y + thumb_h) ctx.hover_slider_thumb = true;
		}
	}
}

/* ── Pointer events ────────────────────────────────────────────────── */
static void pointer_enter(void *d, struct wl_pointer *p, uint32_t s,
		struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)p;(void)s;(void)sf;
	ctx.mx = wl_fixed_to_int(x); ctx.my = wl_fixed_to_int(y);
	update_hover(ctx.mx, ctx.my);
	do_redraw();
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
	(void)d;(void)p;(void)s;(void)sf;
	ctx.mx = -1; ctx.my = -1;
	ctx.hover_close = ctx.hover_min = ctx.hover_up = ctx.hover_del = false;
	ctx.hover_crumb = -1; ctx.hover_entry = -1;
	ctx.hover_slider_up = ctx.hover_slider_down = ctx.hover_slider_thumb = false;
	ctx.slider_grabbed = false;
	do_redraw();
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	(void)d;(void)p;(void)t;
	double nx = wl_fixed_to_int(x), ny = wl_fixed_to_int(y);

	if (ctx.dragging) {
		ctx.win_x = nx - ctx.drag_ox;
		ctx.win_y = ny - ctx.drag_oy;
		do_redraw();
		return;
	}

	if (ctx.slider_grabbed) {
		double bx = ctx.win_x + WIN_BORDER;
		double by2 = ctx.win_y + WIN_TOP + BREADCRUMB_H;
		double slider_area_h = BROWSER_HEIGHT - BREADCRUMB_H - 2;
		double btn_h = SLIDER_BTN_H;
		double track_y = by2 + btn_h;
		double track_h = slider_area_h - 2 * btn_h;
		int vis = grid_rows_visible();
		double thumb_h = track_h * ((double)vis / grid_rows_total());
		if (thumb_h < 16) thumb_h = 16;
		double ratio = (ny - ctx.slider_grab_y - track_y) / (track_h - thumb_h);
		if (ratio < 0) ratio = 0;
		if (ratio > 1) ratio = 1;
		ctx.scroll = (int)(ratio * grid_max_scroll() + 0.5);
		clamp_scroll();
		do_redraw();
		return;
	}

	if ((int)nx != ctx.mx || (int)ny != ctx.my) {
		ctx.mx = nx; ctx.my = ny;
		update_hover(nx, ny);
		do_redraw();
	}
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t s,
		uint32_t t, uint32_t button, uint32_t state) {
	(void)d;(void)p;(void)s;(void)t;(void)button;
	if (ctx.win_y < -9000) return;
	double px = ctx.mx, py = ctx.my;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* window drag on title bar */
		double wx = ctx.win_x, wy = ctx.win_y;
		double close_x = wx + BROWSER_WIDTH + WIN_BORDER - WIN_BTN_SIZE - 2;
		double min_x = close_x - WIN_BTN_SIZE - WIN_BTN_GAP;
		double btn_y = wy + WIN_BORDER + 2;
		bool on_btn = (px >= min_x && px <= close_x + WIN_BTN_SIZE &&
			       py >= btn_y && py <= btn_y + WIN_BTN_SIZE);
		if (!on_btn && py >= wy + 2 && py <= wy + 22 && px >= wx + 2 && px <= wx + BROWSER_WIDTH + 2) {
			ctx.dragging = true;
			ctx.drag_ox = px - wx;
			ctx.drag_oy = py - wy;
			return;
		}
		/* slider thumb drag */
		if (ctx.hover_slider_thumb) {
			ctx.slider_grabbed = true;
			ctx.slider_grab_y = py - ctx.slider_thumb_y;
			return;
		}
		handle_click(px, py);
		do_redraw();
	} else {
		ctx.dragging = false;
		ctx.slider_grabbed = false;
	}
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value) {
	(void)d;(void)p;(void)t;(void)axis;
	double delta = wl_fixed_to_double(value);
	int steps = (int)(fabs(delta) / 120.0);
	if (steps < 1) steps = 1;
	if (delta > 0) ctx.scroll += steps * SCROLL_PER_TICK;
	else if (delta < 0) ctx.scroll -= steps * SCROLL_PER_TICK;
	clamp_scroll();
	do_redraw();
}
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d;(void)p;(void)s; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d;(void)p;(void)t;(void)a; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t s) {
	(void)d;(void)p;(void)a;
	if (s > 0) ctx.scroll += SCROLL_PER_TICK;
	else if (s < 0) ctx.scroll -= SCROLL_PER_TICK;
	clamp_scroll();
	do_redraw();
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
	.button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
	.axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

/* ── Keyboard ──────────────────────────────────────────────────────── */
static void keyboard_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) {
	(void)d;(void)k;(void)f;(void)fd;(void)s; close(fd);
}
static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) {
	(void)d;(void)k;(void)s;(void)sf;(void)a;
}
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
	(void)d;(void)k;(void)s;(void)sf;
}
static void keyboard_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)d;(void)k;(void)serial;(void)time;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
	switch (key) {
	case 1: /* Escape */
		if (ctx.delete_pending) { ctx.delete_pending = false; do_redraw(); return; }
		ctx.running = false;
		break;
	case 28: /* Return */
		if (ctx.delete_pending) {
			delete_entry(ctx.delete_name);
			ctx.delete_pending = false;
			do_redraw();
		} else if (ctx.sel >= 0) {
			open_entry(ctx.sel);
			do_redraw();
		}
		break;
	case 14: /* Backspace = up */
		navigate_up();
		do_redraw();
		break;
	case 35: /* H = toggle hidden */
		ctx.show_hidden = !ctx.show_hidden;
		read_dir();
		do_redraw();
		break;
	}
}
static void keyboard_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
	(void)d;(void)k;(void)serial;(void)mods_depressed;(void)mods_latched;(void)mods_locked;(void)group;
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *k, int32_t r, int32_t delay) {
	(void)data;(void)k;(void)r;(void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
	.key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat_info,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ctx.pointer) {
		ctx.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ctx.pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
		struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(kb, &keyboard_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* ── Layer surface ─────────────────────────────────────────────────── */
static void layer_surface_configure(void *d, struct zwlr_layer_surface_v1 *ls,
		uint32_t serial, uint32_t w, uint32_t h) {
	(void)d;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if ((int32_t)w > 0) ctx.W = w;
	if ((int32_t)h > 0) ctx.H = h;
	bool need = (ctx.buffers[0] == NULL) || (ctx.configured && w > 0 && h > 0);
	if (need) {
		for (int i = 0; i < 2; i++) {
			if (ctx.map[i]) { munmap(ctx.map[i], ctx.map_sz[i]); ctx.map[i] = NULL; }
			if (ctx.buffers[i]) { wl_buffer_destroy(ctx.buffers[i]); ctx.buffers[i] = NULL; }
		}
		if (!create_buffers()) { fprintf(stderr, "onewm-fm: buffer alloc failed\n"); return; }
	}
	ctx.configured = true;
	do_redraw();
}
static void layer_surface_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
	(void)d;(void)ls; ctx.running = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure, .closed = layer_surface_closed,
};

/* ── Registry ──────────────────────────────────────────────────────── */
static void shm_format(void *d, struct wl_shm *s, uint32_t f) { (void)d;(void)s;(void)f; }
static const struct wl_shm_listener shm_listener = { .format = shm_format };

static void registry_global(void *d, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	(void)d;(void)version;
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
static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_remove,
};

/* ── Main ──────────────────────────────────────────────────────────── */
int filemanager_main(int argc, char **argv) {
	(void)argc;(void)argv;

	memset(&ctx, 0, sizeof(ctx));
	ctx.W = BROWSER_WIDTH + 2 * WIN_BORDER;
	ctx.H = BROWSER_HEIGHT + WIN_TOP + 2 * WIN_BORDER;
	ctx.mx = -1; ctx.my = -1;
	ctx.sel = -1;
	ctx.running = true;
	ctx.frames_since_click = DOUBLECLICK_F;
	ctx.win_x = 100; ctx.win_y = 100;

	/* theme */
	const char *tid = getenv("ONEWM_THEME");
	char theme_buf[64] = "purple";
	if (tid) { strncpy(theme_buf, tid, 63); }
	else {
		char tp[4096]; config_path(tp, sizeof tp, "theme");
		FILE *f = fopen(tp, "r");
		if (f) { if (fgets(theme_buf, sizeof theme_buf, f)) { size_t L = strlen(theme_buf); if (L && theme_buf[L-1]=='\n') theme_buf[L-1]='\0'; } fclose(f); }
	}
	load_theme(theme_buf);

	/* initial path */
	const char *start = (argc > 1) ? argv[1] : getenv("HOME");
	if (!start) start = "/";
	set_cwd(start);
	read_dir();

	/* Offline dump mode: render the browser window to a PNG without a compositor. */
	if (getenv("ONEWM_DUMP")) {
		int WW = BROWSER_WIDTH + 2 * WIN_BORDER;
		int WH = BROWSER_HEIGHT + WIN_TOP + 2 * WIN_BORDER;
		ctx.win_x = 0; ctx.win_y = 0;
		ctx.W = WW; ctx.H = WH;
		cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WW, WH);
		cairo_t *cr = cairo_create(surf);
		render(cr);
		cairo_destroy(cr);
		cairo_surface_write_to_png(surf, "/tmp/filemanager_actual.png");
		cairo_surface_destroy(surf);
		return 0;
	}

	/* wayland */
	ctx.dpy = wl_display_connect(NULL);
	if (!ctx.dpy) { fprintf(stderr, "onewm-fm: no wayland compositor\n"); return 1; }
	ctx.reg = wl_display_get_registry(ctx.dpy);
	wl_registry_add_listener(ctx.reg, &registry_listener, NULL);
	wl_display_roundtrip(ctx.dpy);
	if (!ctx.compositor || !ctx.shm || !ctx.layer_shell) {
		fprintf(stderr, "onewm-fm: missing compositor/shm/layer-shell\n"); return 1;
	}

	ctx.surface = wl_compositor_create_surface(ctx.compositor);
	ctx.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		ctx.layer_shell, ctx.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "onewm-filemanager");
	zwlr_layer_surface_v1_set_keyboard_interactivity(ctx.layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	zwlr_layer_surface_v1_set_anchor(ctx.layer_surface, 0);
	zwlr_layer_surface_v1_set_size(ctx.layer_surface, ctx.W, ctx.H);
	zwlr_layer_surface_v1_set_exclusive_zone(ctx.layer_surface, -1);
	zwlr_layer_surface_v1_add_listener(ctx.layer_surface, &layer_surface_listener, NULL);
	wl_surface_commit(ctx.surface);

	while (ctx.running && wl_display_dispatch(ctx.dpy) != -1) {}

	for (int i = 0; i < 2; i++) {
		if (ctx.map[i]) munmap(ctx.map[i], ctx.map_sz[i]);
		if (ctx.buffers[i]) wl_buffer_destroy(ctx.buffers[i]);
	}
	free(ctx.entries);
	if (ctx.layer_surface) zwlr_layer_surface_v1_destroy(ctx.layer_surface);
	if (ctx.surface) wl_surface_destroy(ctx.surface);
	if (ctx.dpy) wl_display_disconnect(ctx.dpy);
	return 0;
}

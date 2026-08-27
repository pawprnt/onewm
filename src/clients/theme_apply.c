#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Apply the active OneWM theme to native toolkits (GTK3/4, KDE/Qt) by writing
   derived color-scheme files. The single source of truth is
   themes/themes_metadata.json, exactly like the WM chrome. */

static char *find_metadata(void) {
	static char path[4096];
	const char *env = getenv("ONEWM_DATA_DIR");
	char buf[4096];
	const char *cands[8];
	int n = 0;
	if (env) {
		snprintf(buf, sizeof buf, "%s/themes_metadata.json", env);
		cands[n++] = buf;
	}
	cands[n++] = "data/themes_metadata.json";
	cands[n++] = "../themes/themes_metadata.json";
	cands[n++] = "/usr/share/onewm/themes_metadata.json";
	cands[n++] = "themes/themes_metadata.json";
	cands[n] = NULL;
	for (int i = 0; cands[i]; i++) {
		snprintf(path, sizeof path, "%s", cands[i]);
		if (access(path, R_OK) == 0) return path;
	}
	return NULL;
}

static void parse_rgb(const char *block, const char *key, int *r, int *g, int *b) {
	char pat[64];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *obj = strstr(block, pat);
	if (!obj) return;
	const char *open = strchr(obj, '{');
	if (!open) return;
	const char *e = strchr(open, '}');
	if (!e) return;
	const char *seg = open;
	size_t seglen = (size_t)(e - open) + 1;
	char buf[256];
	if (seglen >= sizeof buf) seglen = sizeof buf - 1;
	memcpy(buf, open, seglen);
	buf[seglen] = '\0';
#define GET(fld, dst) do { \
		const char *p = strstr(buf, "\"" fld "\""); \
		if (p) { \
			const char *c = strchr(p, ':'); \
			if (c) { int v = 0; sscanf(c, ":%d", &v); *(dst) = v; } \
		} \
	} while (0)
	GET("r", r); GET("g", g); GET("b", b);
#undef GET
}

static char *load_theme(const char *id, int *pr, int *pg, int *pb,
		int *vr, int *vg, int *vb, int *br, int *bg, int *bb) {
	char *mp = find_metadata();
	if (!mp) return NULL;
	FILE *f = fopen(mp, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char *buf = malloc(sz + 1);
	if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
	buf[sz] = '\0';
	fclose(f);

	/* locate the theme block for id */
	char pat[64];
	snprintf(pat, sizeof pat, "\"id\":\"%s\"", id);
	const char *blk = strstr(buf, pat);
	if (!blk) snprintf(pat, sizeof pat, "\"id\": \"%s\"", id), blk = strstr(buf, pat);
	if (!blk) { free(buf); return NULL; }
	const char *nxt = strstr(blk + 1, "\"id\"");
	size_t len = nxt ? (size_t)(nxt - blk) : (sz - (blk - buf));
	char *block = malloc(len + 1);
	memcpy(block, blk, len); block[len] = '\0';

	*pr = 150; *pg = 100; *pb = 255;
	*vr = 100; *vg = 66; *vb = 165;
	*br = 0; *bg = 0; *bb = 0;
	parse_rgb(block, "primary", pr, pg, pb);
	parse_rgb(block, "primaryVariant", vr, vg, vb);
	parse_rgb(block, "background", br, bg, bb);
	free(block);
	return buf; /* keep so caller can free */
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void write_file(const char *path, const char *content) {
	/* Safety: back up any existing file (regular file OR symlink) before
	   replacing it, so a previous config or a system theme behind a symlink
	   is never destroyed. rename(2) on a symlink moves the symlink entry
	   itself (leaving its target untouched), so e.g. a gtk.css symlink into
	   /usr/share/themes is preserved as <path>.bak instead of being clobbered. */
	struct stat st;
	if (lstat(path, &st) == 0) {
		char bak[4096];
		snprintf(bak, sizeof bak, "%s.bak", path);
		rename(path, bak);
	}
	FILE *f = fopen(path, "w");
	if (!f) return;
	fputs(content, f);
	fclose(f);
}

static void mkdirs(const char *path) {
	char tmp[4096];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
	}
	mkdir(tmp, 0755);
}

static void apply_gtk(const char *home, const char *id, int pr, int pg, int pb,
		int vr, int vg, int vb, int br, int bg, int bb, int fr, int fg, int fb) {
	char dir3[1024], dir4[1024], css[2048];
	snprintf(dir3, sizeof dir3, "%s/.config/gtk-3.0", home);
	snprintf(dir4, sizeof dir4, "%s/.config/gtk-4.0", home);
	mkdirs(dir3); mkdirs(dir4);
	snprintf(css, sizeof css,
		"@define-color wm_primary rgb(%d,%d,%d);\n"
		"@define-color wm_variant rgb(%d,%d,%d);\n"
		"@define-color wm_bg rgb(%d,%d,%d);\n"
		"@define-color wm_fg rgb(%d,%d,%d);\n"
		"window.background, .background { background-color: @wm_bg; color: @wm_fg; }\n"
		"headerbar, menubar { background-color: @wm_variant; color: @wm_fg; }\n"
		"button, entry, scale, scrollbar { background-color: @wm_variant; color: @wm_fg; }\n"
		"selection, textview selection { background-color: @wm_primary; color: @wm_fg; }\n",
		pr, pg, pb, vr, vg, vb, br, bg, bb, fr, fg, fb);
	char p3[2048], p4[2048];
	snprintf(p3, sizeof p3, "%s/.config/gtk-3.0/gtk.css", home);
	snprintf(p4, sizeof p4, "%s/.config/gtk-4.0/gtk.css", home);
	write_file(p3, css);
	write_file(p4, css);
}

static void apply_kde(const char *home, const char *id, int pr, int pg, int pb,
		int vr, int vg, int vb, int br, int bg, int bb, int fr, int fg, int fb) {
	char scheme_dir[1024], cs[2048], kg[2048];
	snprintf(scheme_dir, sizeof scheme_dir, "%s/.local/share/color-schemes", home);
	mkdirs(scheme_dir);
	const char *name = id; /* displayName not needed for scheme id */
	snprintf(cs, sizeof cs,
		"[General]\n"
		"Name=OneWM %s\n"
		"ColorScheme=OneWM %s\n"
		"[Colors:Window]\n"
		"BackgroundNormal=%d,%d,%d\n"
		"ForegroundNormal=%d,%d,%d\n"
		"DecorationFocus=%d,%d,%d\n"
		"DecorationHover=%d,%d,%d\n"
		"[Colors:Button]\n"
		"BackgroundNormal=%d,%d,%d\n"
		"ForegroundNormal=%d,%d,%d\n"
		"[Colors:View]\n"
		"BackgroundNormal=%d,%d,%d\n"
		"ForegroundNormal=%d,%d,%d\n"
		"[Colors:Selection]\n"
		"BackgroundNormal=%d,%d,%d\n"
		"ForegroundNormal=%d,%d,%d\n"
		"[Colors:Tooltip]\n"
		"BackgroundNormal=%d,%d,%d\n"
		"ForegroundNormal=%d,%d,%d\n",
		name, name,
		br, bg, bb, fr, fg, fb, pr, pg, pb, pr, pg, pb,
		vr, vg, vb, fr, fg, fb,
		br, bg, bb, fr, fg, fb,
		pr, pg, pb, fr, fg, fb,
		br, bg, bb, fr, fg, fb);
	char cpath[2048];
	snprintf(cpath, sizeof cpath, "%s/.local/share/color-schemes/onewm-%s.colors", home, id);
	write_file(cpath, cs);

	snprintf(kg, sizeof kg,
		"[General]\n"
		"ColorScheme=OneWM %s\n"
		"Name=OneWM %s\n"
		"[KDE]\n"
		"colorScheme=OneWM %s\n"
		"widgetStyle=oxygen\n", name, name, name);
	char kgpath[2048];
	snprintf(kgpath, sizeof kgpath, "%s/.config/kdeglobals", home);
	write_file(kgpath, kg);
}

static void apply_kvantum(const char *home, const char *id, int pr, int pg, int pb,
		int vr, int vg, int vb, int br, int bg, int bb, int fr, int fg, int fb) {
	char dir[1024], kv[4096];
	snprintf(dir, sizeof dir, "%s/.config/Kvantum", home);
	mkdirs(dir);
	snprintf(kv, sizeof kv,
		"[General]\n"
		"theme=OneWM%s\n"
		"gutters=0\n"
		"windowTransparency=0\n"
		"[Application]\n"
		"base=%d,%d,%d\n"
		"altBase=%d,%d,%d\n"
		"text=%d,%d,%d\n"
		"background=%d,%d,%d\n"
		"foreground=%d,%d,%d\n"
		"highlight=%d,%d,%d\n"
		"highlightedText=%d,%d,%d\n"
		"button=%d,%d,%d\n"
		"buttonText=%d,%d,%d\n"
		"tooltipBase=%d,%d,%d\n"
		"tooltipText=%d,%d,%d\n",
		id,
		br, bg, bb, vr, vg, vb, fr, fg, fb, br, bg, bb, fr, fg, fb,
		pr, pg, pb, fr, fg, fb, vr, vg, vb, fr, fg, fb, br, bg, bb, fr, fg, fb);
	char p[2048];
	snprintf(p, sizeof p, "%s/.config/Kvantum/onewm-%s.kvconfig", home, id);
	write_file(p, kv);
}

int theme_apply_main(int argc, char **argv) {
	(void)argc; (void)argv;
	if (getenv("ONEWM_NO_THEME_APPLY"))
		return 0;
	const char *home = getenv("HOME");
	if (!home) home = "/tmp";

	const char *id = NULL;
	if (argc > 1 && argv[1] && argv[1][0]) id = argv[1];
	if (!id) id = getenv("ONEWM_THEME");
	if (!id) {
		char tp[4096]; snprintf(tp, sizeof tp, "%s/.config/onewm/theme", home);
		FILE *f = fopen(tp, "r");
		if (f) { if (fgets(tp, sizeof tp, f)) { size_t L = strlen(tp); if (L && tp[L-1]=='\n') tp[--L]='\0'; id = tp[0]?tp:NULL; } fclose(f); }
	}
	if (!id) id = "purple";

	int pr, pg, pb, vr, vg, vb, br, bg, bb;
	char *buf = load_theme(id, &pr, &pg, &pb, &vr, &vg, &vb, &br, &bg, &bb);
	if (!buf) {
		fprintf(stderr, "onewm theme-apply: theme '%s' not found\n", id);
		return 1;
	}
	free(buf);

	pr = clamp255(pr); pg = clamp255(pg); pb = clamp255(pb);
	vr = clamp255(vr); vg = clamp255(vg); vb = clamp255(vb);
	br = clamp255(br); bg = clamp255(bg); bb = clamp255(bb);

	/* Foreground: game draws primary text on the background. For light
	   backgrounds that is unreadable, darken the primary instead. */
	double lum = (0.299 * br + 0.587 * bg + 0.114 * bb) / 255.0;
	int fr = pr, fg = pg, fb = pb;
	if (lum > 0.5) {
		fr = clamp255((int)(pr * 0.25)); fg = clamp255((int)(pg * 0.25)); fb = clamp255((int)(pb * 0.25));
	}

	apply_gtk(home, id, pr, pg, pb, vr, vg, vb, br, bg, bb, fr, fg, fb);
	apply_kde(home, id, pr, pg, pb, vr, vg, vb, br, bg, bb, fr, fg, fb);
	apply_kvantum(home, id, pr, pg, pb, vr, vg, vb, br, bg, bb, fr, fg, fb);

	fprintf(stderr, "onewm theme-apply: applied '%s' to GTK/KDE/Qt\n", id);
	return 0;
}

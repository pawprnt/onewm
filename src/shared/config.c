#define _GNU_SOURCE
#include "config.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_ENTRIES 1024

struct entry {
	char section[64];
	char key[96];
	char val[256];
};

static struct entry entries[MAX_ENTRIES];
static int n_entries = 0;

static void add(const char *sec, const char *key, const char *val) {
	if (n_entries >= MAX_ENTRIES)
		return;
	snprintf(entries[n_entries].section, sizeof(entries[n_entries].section), "%s", sec);
	snprintf(entries[n_entries].key, sizeof(entries[n_entries].key), "%s", key);
	snprintf(entries[n_entries].val, sizeof(entries[n_entries].val), "%s", val);
	n_entries++;
}

/* Skip whitespace and line comments (-- and #). */
static const char *skip_ws(const char *p) {
	while (*p) {
		if (isspace((unsigned char)*p)) {
			p++;
			continue;
		}
		if (p[0] == '-' && p[1] == '-') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		if (p[0] == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}
		break;
	}
	return p;
}

/* Read a bareword/identifier until a delimiter. */
static char *read_ident(char **pp, char *buf, size_t n) {
	char *p = *pp;
	size_t i = 0;
	while (isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '+' || *p == '/') {
		if (i + 1 < n)
			buf[i++] = *p;
		p++;
	}
	buf[i] = '\0';
	*pp = p;
	return buf;
}

static void read_value(char **pp, char *buf, size_t n) {
	char *p = *pp;
	while (isspace((unsigned char)*p))
		p++;
	char q = 0;
	if (*p == '"' || *p == '\'') {
		q = *p++;
		size_t i = 0;
		while (*p && *p != q) {
			if (i + 1 < n)
				buf[i++] = *p;
			p++;
		}
		if (*p == q)
			p++;
		buf[i] = '\0';
		*pp = p;
		return;
	}
	size_t i = 0;
	while (*p && !isspace((unsigned char)*p) && *p != '\n' && *p != ';' && *p != ','
	       && *p != ')' && *p != '}') {
		if (i + 1 < n)
			buf[i++] = *p;
		p++;
	}
	buf[i] = '\0';
	*pp = p;
}

/* Recursive block parser. Consumes statements until it hits the matching '}'.
   section is the current section name ("" at top level). */
static void parse_block(char **pp, const char *section) {
	char *p = *pp;
	while (1) {
		p = (char *)skip_ws(p);
		if (!*p)
			break;
		if (*p == '}') {
			p++;
			*pp = p;
			return;
		}
		char ident[128];
		read_ident(&p, ident, sizeof(ident));
		if (!ident[0]) {
			p++;
			continue;
		}
		p = (char *)skip_ws(p);
		if (*p == '=') {
			p++;
			p = (char *)skip_ws(p);
			if (*p == '{') {
				p++;
				parse_block(&p, ident);
				continue;
			}
			char val[256];
			read_value(&p, val, sizeof(val));
			add(section, ident, val);
			while (*p && *p != '\n' && *p != ';')
				p++;
			if (*p == ';')
				p++;
			continue;
		}
		if (*p == '{') {
			p++;
			parse_block(&p, ident);
			continue;
		}
		if (*p == '(') {
			p++;
			char args[6][256];
			int na = 0;
			while (*p && *p != ')') {
				while (isspace((unsigned char)*p) || *p == ',')
					p++;
				if (*p == ')' || !*p)
					break;
				read_value(&p, args[na], sizeof(args[na]));
				if (na < 6)
					na++;
			}
			if (*p == ')')
				p++;
			if (na >= 1) {
				char val[256];
				val[0] = '\0';
				for (int k = 1; k < na; k++) {
					strncat(val, args[k], sizeof(val) - strlen(val) - 1);
					if (k < na - 1)
						strncat(val, ",", sizeof(val) - strlen(val) - 1);
				}
				add(ident, args[0], val);
			}
			continue;
		}
		while (*p && *p != '\n')
			p++;
	}
	*pp = p;
}

static void push_env(const char *section, const char *key, const char *env) {
	const char *v = cfg_get(section, key);
	if (v && *v)
		setenv(env, v, 1);
}

void config_load(void) {
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;

	const char *candidates[] = {
		getenv("ONEWM_CONFIG"),
		getenv("XDG_CONFIG_HOME") ? NULL : NULL,
	};
	(void)candidates;

	char path[4096];
	path[0] = '\0';

	const char *cfg = getenv("ONEWM_CONFIG");
	if (cfg && *cfg) {
		snprintf(path, sizeof(path), "%s", cfg);
	} else {
		const char *xdg = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		const char *roots[] = {
			xdg ? NULL : NULL,
			NULL,
		};
		(void)roots;
		char base[4096];
		if (xdg && *xdg)
			snprintf(base, sizeof(base), "%s/onewm/onewm.lua", xdg);
		else if (home && *home)
			snprintf(base, sizeof(base), "%s/.config/onewm/onewm.lua", home);
		else
			base[0] = '\0';
		if (base[0] && access(base, R_OK) == 0)
			snprintf(path, sizeof(path), "%s", base);
		else if (access("/etc/onewm/onewm.lua", R_OK) == 0)
			snprintf(path, sizeof(path), "/etc/onewm/onewm.lua");
		else {
			const char *d = getenv("ONEWM_DATA_DIR");
			if (d)
				snprintf(path, sizeof(path), "%s/onewm.lua", d);
			else
				snprintf(path, sizeof(path), "data/onewm.lua");
		}
	}

	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return;
	}
	char *buf = malloc(sz + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	if (fread(buf, 1, sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return;
	}
	buf[sz] = '\0';
	fclose(f);

	char *p = buf;
	parse_block(&p, "");
	free(buf);

	/* Push well-known keys into the environment the components already read. */
	push_env("general", "theme", "ONEWM_THEME");
	push_env("general", "icon_dir", "ONEWM_ICON_DIR");
	push_env("general", "panel_height", "ONEWM_PANEL_HEIGHT");
	push_env("general", "data_dir", "ONEWM_DATA_DIR");
	const char *wp = cfg_get("general", "wallpaper");
	if (wp && *wp) {
		if (wp[0] == '/')
			setenv("ONEWM_WALLPAPER", wp, 1);
		else {
			char resolved[4096];
			const char *d = getenv("ONEWM_DATA_DIR");
			if (d)
				snprintf(resolved, sizeof(resolved), "%s/wallpapers/%s.png-0.png", d, wp);
			else
				snprintf(resolved, sizeof(resolved), "data/wallpapers/%s.png-0.png", wp);
			if (access(resolved, R_OK) == 0)
				setenv("ONEWM_WALLPAPER", resolved, 1);
		}
	}
}

const char *cfg_get(const char *section, const char *key) {
	for (int i = 0; i < n_entries; i++) {
		if (strcmp(entries[i].section, section ? section : "") == 0
		    && strcmp(entries[i].key, key) == 0)
			return entries[i].val;
	}
	return NULL;
}

int cfg_get_int(const char *section, const char *key, int def) {
	const char *v = cfg_get(section, key);
	if (!v || !*v)
		return def;
	return (int)strtol(v, NULL, 10);
}

int cfg_get_bool(const char *section, const char *key, int def) {
	const char *v = cfg_get(section, key);
	if (!v || !*v)
		return def;
	if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0)
		return 1;
	return 0;
}

void cfg_foreach(const char *section,
		void (*cb)(const char *key, const char *val, void *ud), void *ud) {
	for (int i = 0; i < n_entries; i++) {
		if (strcmp(entries[i].section, section ? section : "") == 0)
			cb(entries[i].key, entries[i].val, ud);
	}
}

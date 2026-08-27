#ifndef ONEWM_CONFIG_H
#define ONEWM_CONFIG_H

/* Hyprland-style declarative config:
 *   key = value
 *   section {
 *     key = value
 *   }
 * Parsed once at startup; values can be queried by (section, key). The
 * global section uses an empty section name (""). A few well-known keys are
 * pushed into the environment so the existing component code picks them up
 * unchanged (ONEWM_THEME, ONEWM_ICON_DIR, ONEWM_WALLPAPER, ONEWM_PANEL_HEIGHT,
 * ONEWM_DATA_DIR). */

void config_load(void);

const char *cfg_get(const char *section, const char *key);
int cfg_get_int(const char *section, const char *key, int def);
int cfg_get_bool(const char *section, const char *key, int def);

/* Walk every key/value pair in a section (used for keybinds / windowrules). */
void cfg_foreach(const char *section,
		void (*cb)(const char *key, const char *val, void *ud), void *ud);

#endif

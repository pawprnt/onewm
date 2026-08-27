#define _GNU_SOURCE
#include "onewm.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <stdio.h>
#include <string.h>

#include "assets.h"

int main(int argc, char **argv) {
	config_load();
	asset_bootstrap();

	const char *sub = (argc > 1) ? argv[1] : NULL;
	setenv("ONEWM_SUBCMD", sub ? sub : "compositor", 1);
	if (sub && strcmp(sub, "boot") == 0)
		return boot_main(argc - 1, argv + 1);
	if (sub && strcmp(sub, "panel") == 0)
		return panel_main(argc - 1, argv + 1);
	if (sub && strcmp(sub, "desktop") == 0)
		return desktop_main(argc - 1, argv + 1);
	if (sub && strcmp(sub, "filemanager") == 0)
		return filemanager_main(argc - 1, argv + 1);
	if (sub && strcmp(sub, "wallpapers") == 0)
		return selectors_main(argc - 1, argv + 1);
	if (sub && strcmp(sub, "themes") == 0)
		return selectors_main(argc - 1, argv + 1);
	if (sub && strcmp(sub, "theme-apply") == 0)
		return theme_apply_main(argc - 1, argv + 1);
	return compositor_main(argc, argv);
}

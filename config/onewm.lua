-- onewm.lua — configuration for the OneShot: World Machine Edition desktop.
-- Lua-ish syntax (Hyprland-style). Comments start with -- or #.

-- Global look & behavior. All keys map to environment variables the
-- components already read, so they take effect for the compositor, the
-- taskbar, the desktop and the file-manager uniformly.
general = {
  theme        = "purple",   -- purple | blue | teal | green | yellow | red | pink | orange | white | rainbow
  wallpaper    = "catwalks", -- name of an unlocked wallpaper (e.g. catwalks, terminal, etc.)
  icon_dir     = "/home/fox/TWM-xfce/TWM-icons/scalable",
  panel_height = 30,
  transparency = 100,        -- 20..100 (% opaque); <100 makes unfocused windows translucent
  data_dir     = "/nyaa/coding/public/customization/onewm/data",
}

-- Keybind controls. Modifier tokens: SUPER (Win/Logo), SHIFT, CTRL, ALT.
-- Actions: exec <shell>, kill (close focused), exit (quit WM).
keybind("SUPER+ENTER", "exec alacritty")
keybind("SUPER+Q",     "kill")
keybind("SUPER+E",     "exec onewm filemanager")
keybind("SUPER+ESCAPE","exit")

-- Window rules. Currently supported: opacity (0..1) overriding general.transparency
-- for the whole session, and float (reserved for future per-window rules).
windowrule("opacity", "0.95")

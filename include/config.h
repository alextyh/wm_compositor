#ifndef WMCOMP_CONFIG_H
#define WMCOMP_CONFIG_H

#include <stdbool.h>

// Per-monitor settings, matched against the real output name (e.g. "DP-1",
// "HDMI-A-1") reported by wlroots. Unset numeric fields are 0, meaning
// "use the monitor's own preferred/default".
typedef struct {
    char* name;
    int    width, height, refresh; // refresh in whole Hz
    bool    has_position;
    int      x, y;
} MonitorConfig;

// A configured keybind. Exactly one of command/action is set:
//   command — a shell command line to spawn (config value "exec:...")
//   action  — the name of a built-in compositor action (config value "action:...")
// Modifier flags are compositor-agnostic booleans; resolved against the
// real WLR_MODIFIER_* bitmask in compositor.c so this header doesn't need
// to depend on wlroots types.
typedef struct {
    bool mod_super, mod_alt, mod_shift, mod_ctrl;
    char* key; // e.g. "space", "c", "1" — resolved to an xkb keysym elsewhere
    char* command;
    char* action;
} KeybindConfig;

typedef struct {
    MonitorConfig* monitors;
    int              monitor_count;

    int    border_width;
    float border_color_focused[4];
    float border_color_unfocused[4];

    // Programs to launch once the compositor is up (pipewire, a terminal,
    // etc). Each entry is a full shell command line, run via `sh -c`.
    char** exec_cmds;
    int      exec_count;

    char** env_keys;
    char** env_values;
    int      env_count;

    KeybindConfig* keybinds;
    int               keybind_count;

    char* primary_monitor; // output name, e.g. "DP-1"; NULL = no preference
    bool    xwayland_lazy;   // default true: X server starts only when first needed
} WMConfig;

// Fills cfg with defaults, then overrides from ~/.config/.wmcompconfig if
// it exists. Always leaves cfg fully populated and safe to use.
void config_load_default(WMConfig* cfg);

// Looks up a named monitor's config by output name. Returns NULL if the
// config doesn't mention that output — caller should fall back to defaults.
const MonitorConfig* config_find_monitor(const WMConfig* cfg, const char* output_name);

// Frees everything config_load_default allocated. Call once during shutdown.
void config_free(WMConfig* cfg);

#endif

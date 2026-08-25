#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pwd.h>
#include <unistd.h>

static const float DEFAULT_FOCUSED[4]   = {0.30f, 0.60f, 1.00f, 1.0f};
static const float DEFAULT_UNFOCUSED[4] = {0.30f, 0.30f, 0.30f, 1.0f};

static bool parse_hex_color(const char* hex, float out[4]) {
    if (strlen(hex) != 6) return false;
    unsigned int r, g, b;
    if (sscanf(hex, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f; out[3] = 1.0f;
    return true;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) { *end = '\0'; end--; }
    return s;
}

static void add_exec_cmd(WMConfig* cfg, const char* cmd) {
    cfg->exec_cmds = realloc(cfg->exec_cmds, sizeof(char*) * (cfg->exec_count + 1));
    cfg->exec_cmds[cfg->exec_count] = strdup(cmd);
    cfg->exec_count++;
}

/* "BEMENU_BACKEND=curses" */
static void add_env(WMConfig* cfg, const char* value) {
    const char* eq = strchr(value, '=');
    if (!eq) { fprintf(stderr, "[Config] bad env line, missing '=': %s\n", value); return; }
    cfg->env_keys = realloc(cfg->env_keys, sizeof(char*) * (cfg->env_count + 1));
    cfg->env_values = realloc(cfg->env_values, sizeof(char*) * (cfg->env_count + 1));
    cfg->env_keys[cfg->env_count] = strndup(value, eq - value);
    cfg->env_values[cfg->env_count] = strdup(eq + 1);
    cfg->env_count++;
}

/* "DP-1:1920x1080@144:0,0"  — refresh and position both optional */
static void add_monitor(WMConfig* cfg, const char* value) {
    char name[128] = {0};
    int w = 0, h = 0, r = 0, x = 0, y = 0;
    int n = sscanf(value, "%127[^:]:%dx%d@%d:%d,%d", name, &w, &h, &r, &x, &y);
    if (n < 3) { fprintf(stderr, "[Config] bad monitor line: %s\n", value); return; }

    cfg->monitors = realloc(cfg->monitors, sizeof(MonitorConfig) * (cfg->monitor_count + 1));
    MonitorConfig* m = &cfg->monitors[cfg->monitor_count];
    m->name = strdup(name);
    m->width = w; m->height = h; m->refresh = r;
    m->has_position = (n == 6);
    m->x = x; m->y = y;
    cfg->monitor_count++;
}

/* "SUPER+ALT+space=kitty" or "SUPER+Escape=action:quit" or "SUPER+C=action:close" */
static void add_bind(WMConfig* cfg, const char* value) {
    const char* eq = strrchr(value, '=');
    if (!eq) { fprintf(stderr, "[Config] bad bind, missing '=': %s\n", value); return; }

    char keys[256];
    size_t klen = (size_t)(eq - value);
    if (klen >= sizeof(keys)) klen = sizeof(keys) - 1;
    memcpy(keys, value, klen);
    keys[klen] = '\0';
    const char* rhs = eq + 1;

    KeybindConfig kb = {0};
    char* tok = strtok(keys, "+");
    while (tok) {
        if (strcasecmp(tok, "SUPER") == 0 || strcasecmp(tok, "LOGO") == 0) kb.mod_super = true;
        else if (strcasecmp(tok, "ALT") == 0) kb.mod_alt = true;
        else if (strcasecmp(tok, "CTRL") == 0) kb.mod_ctrl = true;
        else if (strcasecmp(tok, "SHIFT") == 0) kb.mod_shift = true;
        else { free(kb.key); kb.key = strdup(tok); }
        tok = strtok(NULL, "+");
    }
    if (!kb.key) { fprintf(stderr, "[Config] bad bind, no key found: %s\n", value); return; }

    if (strncmp(rhs, "action:", 7) == 0) {
        kb.action = strdup(rhs + 7);
    } else {
        kb.command = strdup(rhs);
    }

    cfg->keybinds = realloc(cfg->keybinds, sizeof(KeybindConfig) * (cfg->keybind_count + 1));
    cfg->keybinds[cfg->keybind_count] = kb;
    cfg->keybind_count++;
}

static void add_default_bind(WMConfig* cfg, bool super, bool alt, bool shift, bool ctrl,
        const char* key, const char* action) {
    for (int i = 0; i < cfg->keybind_count; i++) {
        if (cfg->keybinds[i].action && strcmp(cfg->keybinds[i].action, action) == 0) return; // user already bound it
    }
    KeybindConfig kb = {0};
    kb.mod_super = super; kb.mod_alt = alt; kb.mod_shift = shift; kb.mod_ctrl = ctrl;
    kb.key = strdup(key);
    kb.action = strdup(action);
    cfg->keybinds = realloc(cfg->keybinds, sizeof(KeybindConfig) * (cfg->keybind_count + 1));
    cfg->keybinds[cfg->keybind_count] = kb;
    cfg->keybind_count++;
}

static char* resolve_config_path(void) {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return NULL;
    char* path = malloc(strlen(home) + strlen("/.config/.wmcompconfig") + 1);
    sprintf(path, "%s/.config/.wmcompconfig", home);
    return path;
}

void config_load_default(WMConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->border_width = 3;
    cfg->xwayland_lazy = true;
    memcpy(cfg->border_color_focused, DEFAULT_FOCUSED, sizeof(DEFAULT_FOCUSED));
    memcpy(cfg->border_color_unfocused, DEFAULT_UNFOCUSED, sizeof(DEFAULT_UNFOCUSED));

    char* path = resolve_config_path();
    FILE* f = path ? fopen(path, "r") : NULL;
    if (!f) {
        printf("[Config] No config file%s%s, using defaults.\n", path ? " at " : "", path ? path : "");
        free(path);
    } else {
        printf("[Config] Loading %s\n", path);
        char line[1024];
        int lineno = 0;
        while (fgets(line, sizeof(line), f)) {
            lineno++;
            char* trimmed = trim(line);
            if (trimmed[0] == '\0' || trimmed[0] == '#') continue;
            char* eq = strchr(trimmed, '=');
            if (!eq) { fprintf(stderr, "[Config] %s:%d: no '=', skipping\n", path, lineno); continue; }
            *eq = '\0';
            char* key = trim(trimmed);
            char* value = trim(eq + 1);

            if (strcmp(key, "border_width") == 0) cfg->border_width = atoi(value);
            else if (strcmp(key, "border_color_focused") == 0) {
                if (!parse_hex_color(value, cfg->border_color_focused))
                    fprintf(stderr, "[Config] %s:%d: bad color '%s'\n", path, lineno, value);
            } else if (strcmp(key, "border_color_unfocused") == 0) {
                if (!parse_hex_color(value, cfg->border_color_unfocused))
                    fprintf(stderr, "[Config] %s:%d: bad color '%s'\n", path, lineno, value);
            } else if (strcmp(key, "exec") == 0) add_exec_cmd(cfg, value);
            else if (strcmp(key, "env") == 0) add_env(cfg, value);
            else if (strcmp(key, "monitor") == 0) add_monitor(cfg, value);
            else if (strcmp(key, "primary_monitor") == 0) cfg->primary_monitor = strdup(value);
            else if (strcmp(key, "xwayland_lazy") == 0) cfg->xwayland_lazy = (strcmp(value, "true") == 0);
            else if (strcmp(key, "bind") == 0) add_bind(cfg, value);
            else fprintf(stderr, "[Config] %s:%d: unknown key '%s'\n", path, lineno, key);
        }
        fclose(f);
        free(path);
    }

    add_default_bind(cfg, true, false, false, false, "Escape", "quit");
    add_default_bind(cfg, true, false, false, false, "c", "close");
    add_default_bind(cfg, false, true, false, false, "Tab", "cycle_window");
    add_default_bind(cfg, true, true, true, false, "o", "toggle_omnipresent");

    for (int i = 1; i <= 10; i++) {
        char key[2] = { (char)('0' + (i % 10)), '\0' }; // 10 -> "0"
        char action[32];
        snprintf(action, sizeof(action), "workspace_%d", i);
        add_default_bind(cfg, true, true, false, false, key, action);
        snprintf(action, sizeof(action), "move_to_workspace_%d", i);
        add_default_bind(cfg, true, true, true, false, key, action);
    }
}

const MonitorConfig* config_find_monitor(const WMConfig* cfg, const char* output_name) {
    for (int i = 0; i < cfg->monitor_count; i++) {
        if (strcmp(cfg->monitors[i].name, output_name) == 0) return &cfg->monitors[i];
    }
    return NULL;
}

void config_free(WMConfig* cfg) {
    for (int i = 0; i < cfg->exec_count; i++) free(cfg->exec_cmds[i]);
    free(cfg->exec_cmds);
    for (int i = 0; i < cfg->env_count; i++) { free(cfg->env_keys[i]); free(cfg->env_values[i]); }
    free(cfg->env_keys);
    free(cfg->env_values);
    for (int i = 0; i < cfg->monitor_count; i++) free(cfg->monitors[i].name);
    free(cfg->monitors);
    for (int i = 0; i < cfg->keybind_count; i++) {
        free(cfg->keybinds[i].key);
        free(cfg->keybinds[i].command);
        free(cfg->keybinds[i].action);
    }
    free(cfg->keybinds);
    free(cfg->primary_monitor);
}

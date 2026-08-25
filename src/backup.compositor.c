#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

// FIX 1
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved_errno;
}

// =================================================================
// OUTPUT
// =================================================================

static void output_frame(struct wl_listener* listener, void* data) {
    Output* out = wl_container_of(listener, out, frame);
    struct wlr_scene_output* scene_output =
        wlr_scene_get_scene_output(out->comp->scene, out->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener* listener, void* data) {
    Output* out = wl_container_of(listener, out, request_state);
    const struct wlr_output_event_request_state* event = data;
    wlr_output_commit_state(out->wlr_output, event->state);
}

static void output_destroy(struct wl_listener* listener, void* data) {
    Output* out = wl_container_of(listener, out, destroy);
    wl_list_remove(&out->frame.link);
    wl_list_remove(&out->request_state.link);
    wl_list_remove(&out->destroy.link);
    wl_list_remove(&out->link);
    free(out);
}

// Picks the mode to use for a newly-connected output. If the config
// specifies width/height (and optionally refresh), tries to find a matching
// mode in the output's own advertised list; falls back to the monitor's
// preferred mode if the config is silent or nothing matches.
// Picks the mode to use for a newly-connected output, per-monitor (matched
// by wlr_output->name, e.g. "DP-1"). Falls back to the monitor's preferred
// mode if that output isn't mentioned in the config, or nothing matches.
static struct wlr_output_mode* pick_output_mode(const MonitorConfig* mcfg, struct wlr_output* wlr_output) {
    if (!mcfg || mcfg->width <= 0 || mcfg->height <= 0) {
        return wlr_output_preferred_mode(wlr_output);
    }

    struct wlr_output_mode* mode;
    struct wlr_output_mode* best = NULL;
    wl_list_for_each(mode, &wlr_output->modes, link) {
        if (mode->width != mcfg->width || mode->height != mcfg->height) {
            continue;
        }
        if (mcfg->refresh > 0) {
            // refresh is stored in mHz; allow a little slack for rounding
            // (e.g. 59.94Hz panels reporting as 59940 vs a config of 60).
            int target_mhz = mcfg->refresh * 1000;
            if (abs(mode->refresh - target_mhz) > 1000) continue;
            return mode;
        }
        best = mode;
    }

    if (best) return best;

    fprintf(stderr,
        "[Config] No output mode matching %dx%d%s found on '%s' — using the monitor's preferred mode instead.\n",
        mcfg->width, mcfg->height, mcfg->refresh > 0 ? "@configured refresh" : "", wlr_output->name);
    return wlr_output_preferred_mode(wlr_output);
}

static void compositor_new_output(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, new_output);
    struct wlr_output* wlr_output = data;

    const MonitorConfig* mcfg = config_find_monitor(&comp->config, wlr_output->name);

    if (!comp->primary_output ||
            (comp->config.primary_monitor && strcmp(comp->config.primary_monitor, wlr_output->name) == 0)) {
        comp->primary_output = wlr_output;
    }

    wlr_output_init_render(wlr_output, comp->allocator, comp->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode* mode = pick_output_mode(mcfg, wlr_output);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    } else if (mcfg && mcfg->width > 0 && mcfg->height > 0) {
        // Nothing in the output's own mode list matched at all (common on
        // nested/headless backends, which often report no modes) — ask for
        // exactly what the config wants via a custom mode instead.
        wlr_output_state_set_custom_mode(&state, mcfg->width, mcfg->height,
            mcfg->refresh > 0 ? mcfg->refresh * 1000 : 0);
    }

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    Output* out = calloc(1, sizeof(Output));
    out->comp = comp;
    out->wlr_output = wlr_output;

    out->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &out->frame);
    out->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &out->request_state);
    out->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &out->destroy);

    wl_list_insert(&comp->outputs, &out->link);

    // Explicit position from the config (multi-monitor layout) takes
    // priority; otherwise let wlroots auto-arrange left-to-right as before.
    struct wlr_output_layout_output* l_output;
    if (mcfg && mcfg->has_position) {
        l_output = wlr_output_layout_add(comp->output_layout, wlr_output, mcfg->x, mcfg->y);
    } else {
        l_output = wlr_output_layout_add_auto(comp->output_layout, wlr_output);
    }
    struct wlr_scene_output* scene_output =
        wlr_scene_output_create(comp->scene, wlr_output);
    wlr_scene_output_layout_add_output(comp->scene_layout, l_output, scene_output);

    printf("[Output] New output '%s' ready.\n", wlr_output->name);
}

// =================================================================
// BORDER (focus-color rectangles around each window)
// =================================================================

// Re-lays the 4 border rects around the window's current geometry.
// Call after resize/move or when a window is first mapped.
static void update_window_border(ManagedWindow* win) {
    struct wlr_box geo = win->xdg_toplevel->base->geometry;
    int bw = win->comp->config.border_width;

    wlr_scene_rect_set_size(win->border_top, geo.width + 2 * bw, bw);
    wlr_scene_node_set_position(&win->border_top->node, -bw, -bw);

    wlr_scene_rect_set_size(win->border_bottom, geo.width + 2 * bw, bw);
    wlr_scene_node_set_position(&win->border_bottom->node, -bw, geo.height);

    wlr_scene_rect_set_size(win->border_left, bw, geo.height);
    wlr_scene_node_set_position(&win->border_left->node, -bw, 0);

    wlr_scene_rect_set_size(win->border_right, bw, geo.height);
    wlr_scene_node_set_position(&win->border_right->node, geo.width, 0);
}

static void set_window_border_color(ManagedWindow* win, bool focused) {
    const float* color = focused ? win->comp->config.border_color_focused
                                  : win->comp->config.border_color_unfocused;
    wlr_scene_rect_set_color(win->border_top, color);
    wlr_scene_rect_set_color(win->border_bottom, color);
    wlr_scene_rect_set_color(win->border_left, color);
    wlr_scene_rect_set_color(win->border_right, color);
}

// Same border treatment for Xwayland windows. X11 surfaces don't have the
// xdg-shell "geometry" concept (a client-reported offset/size sub-rect) —
// their width/height live directly on the xsurface.
static void update_xwindow_border(XWindow* win) {
    if (!win->border_top) return; // override-redirect: no border was created
    int w = win->xsurface->width;
    int h = win->xsurface->height;
    int bw = win->comp->config.border_width;

    wlr_scene_rect_set_size(win->border_top, w + 2 * bw, bw);
    wlr_scene_node_set_position(&win->border_top->node, -bw, -bw);

    wlr_scene_rect_set_size(win->border_bottom, w + 2 * bw, bw);
    wlr_scene_node_set_position(&win->border_bottom->node, -bw, h);

    wlr_scene_rect_set_size(win->border_left, bw, h);
    wlr_scene_node_set_position(&win->border_left->node, -bw, 0);

    wlr_scene_rect_set_size(win->border_right, bw, h);
    wlr_scene_node_set_position(&win->border_right->node, w, 0);
}

static void set_xwindow_border_color(XWindow* win, bool focused) {
    if (!win->border_top) return; // override-redirect: no border was created
    const float* color = focused ? win->comp->config.border_color_focused
                                  : win->comp->config.border_color_unfocused;
    wlr_scene_rect_set_color(win->border_top, color);
    wlr_scene_rect_set_color(win->border_bottom, color);
    wlr_scene_rect_set_color(win->border_left, color);
    wlr_scene_rect_set_color(win->border_right, color);
}

// =================================================================
// WINDOW FOCUS
// =================================================================

// Un-focuses whatever the seat currently has focused, regardless of which
// kind of window it is. Looked up via the scene node's tagged data, same
// mechanism as window_at().
static void unfocus_current(Compositor* comp) {
    struct wlr_surface* prev_surface = comp->seat->keyboard_state.focused_surface;
    if (!prev_surface) return;

    struct wlr_xdg_toplevel* prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
    if (prev_toplevel) {
        wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        if (prev_toplevel->base->data) {
            ManagedWindow* prev_win = ((struct wlr_scene_tree*)prev_toplevel->base->data)->node.data;
            if (prev_win) set_window_border_color(prev_win, false);
        }
        return;
    }

    struct wlr_xwayland_surface* prev_xsurface = wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
    if (prev_xsurface) {
        wlr_xwayland_surface_activate(prev_xsurface, false);
        XWindow* prev_win = prev_xsurface->data;
        if (prev_win) set_xwindow_border_color(prev_win, false);
    }
}

static void focus_window(ManagedWindow* win) {
    if (!win) return;
    Compositor* comp = win->comp;
    struct wlr_seat* seat = comp->seat;
    struct wlr_surface* surface = win->xdg_toplevel->base->surface;

    if (seat->keyboard_state.focused_surface == surface) return;
    unfocus_current(comp);

    struct wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&win->scene_tree->node);
    wl_list_remove(&win->link);
    wl_list_insert(&comp->toplevels, &win->link);
    wlr_xdg_toplevel_set_activated(win->xdg_toplevel, true);
    set_window_border_color(win, true);

    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

static void focus_xwindow(XWindow* win) {
    if (!win) return;
    Compositor* comp = win->comp;
    struct wlr_seat* seat = comp->seat;
    struct wlr_surface* surface = win->xsurface->surface;

    if (seat->keyboard_state.focused_surface == surface) return;
    unfocus_current(comp);

    struct wlr_keyboard* keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&win->scene_tree->node);
    wl_list_remove(&win->link);
    wl_list_insert(&comp->xwayland_windows, &win->link);
    wlr_xwayland_surface_activate(win->xsurface, true);
    set_xwindow_border_color(win, true);

    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, surface,
            keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
    }
}

// Dispatches to the right focus_* based on kind — for call sites that got
// a generic pointer back from window_at().
static void focus_generic(void* win, WindowKind kind) {
    if (!win) return;
    if (kind == WINDOW_KIND_TOPLEVEL) {
        focus_window(win);
    } else {
        focus_xwindow(win);
    }
}

// Finds whatever window (native or Xwayland) is under the given point.
// Returns a generic pointer — cast it based on *kind_out to ManagedWindow*
// or XWindow*. Returns NULL (kind_out untouched) if nothing's there.
static void* window_at(Compositor* comp, double lx, double ly,
        struct wlr_surface** surface, double* sx, double* sy, WindowKind* kind_out) {
    struct wlr_scene_node* node = wlr_scene_node_at(&comp->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;

    struct wlr_scene_buffer* scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) return NULL;

    *surface = scene_surface->surface;
    struct wlr_scene_tree* tree = node->parent;
    while (tree && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    if (!tree || !tree->node.data) return NULL;

    *kind_out = *(WindowKind*)tree->node.data;
    return tree->node.data;
}

// =================================================================
// INTERACTIVE MOVE / RESIZE  (Alt held: M1 = move, M2 = resize)
// =================================================================

// Returns the current on-screen box (position + size) for either kind of
// window, in the common coordinate space used for move/resize math.
static struct wlr_box window_get_box(void* win, WindowKind kind) {
    struct wlr_box box = {0};
    if (kind == WINDOW_KIND_TOPLEVEL) {
        ManagedWindow* w = win;
        struct wlr_box geo = w->xdg_toplevel->base->geometry;
        box.x = w->scene_tree->node.x + geo.x;
        box.y = w->scene_tree->node.y + geo.y;
        box.width = geo.width;
        box.height = geo.height;
    } else {
        XWindow* w = win;
        box.x = w->scene_tree->node.x;
        box.y = w->scene_tree->node.y;
        box.width = w->xsurface->width;
        box.height = w->xsurface->height;
    }
    return box;
}

static void begin_interactive(void* win, WindowKind kind, CursorMode mode, uint32_t edges) {
    Compositor* comp = (kind == WINDOW_KIND_TOPLEVEL) ? ((ManagedWindow*)win)->comp : ((XWindow*)win)->comp;
    comp->grabbed_window = win;
    comp->grabbed_kind = kind;
    comp->cursor_mode = mode;

    struct wlr_box box = window_get_box(win, kind);

    if (mode == CURSOR_MOVE) {
        struct wlr_scene_tree* tree = (kind == WINDOW_KIND_TOPLEVEL)
            ? ((ManagedWindow*)win)->scene_tree : ((XWindow*)win)->scene_tree;
        comp->grab_x = comp->cursor->x - tree->node.x;
        comp->grab_y = comp->cursor->y - tree->node.y;
    } else {
        double border_x = box.x + (edges & WLR_EDGE_RIGHT ? box.width : 0);
        double border_y = box.y + (edges & WLR_EDGE_BOTTOM ? box.height : 0);
        comp->grab_x = comp->cursor->x - border_x;
        comp->grab_y = comp->cursor->y - border_y;
        comp->grab_geobox = box;
    }

    comp->resize_edges = edges;
}

static void reset_cursor_mode(Compositor* comp) {
    comp->cursor_mode = CURSOR_PASSTHROUGH;
    comp->grabbed_window = NULL;
}

static void process_cursor_move(Compositor* comp) {
    struct wlr_scene_tree* tree = (comp->grabbed_kind == WINDOW_KIND_TOPLEVEL)
        ? ((ManagedWindow*)comp->grabbed_window)->scene_tree
        : ((XWindow*)comp->grabbed_window)->scene_tree;
    double new_x = comp->cursor->x - comp->grab_x;
    double new_y = comp->cursor->y - comp->grab_y;
    wlr_scene_node_set_position(&tree->node, new_x, new_y);

    // X11 windows track their own position — the client (and other X
    // tooling) expects xsurface->x/y to stay accurate, so tell it directly.
    if (comp->grabbed_kind == WINDOW_KIND_XWAYLAND) {
        XWindow* xw = comp->grabbed_window;
        wlr_xwayland_surface_configure(xw->xsurface, new_x, new_y,
            xw->xsurface->width, xw->xsurface->height);
    }
}

static void process_cursor_resize(Compositor* comp) {
    double border_x = comp->cursor->x - comp->grab_x;
    double border_y = comp->cursor->y - comp->grab_y;
    int new_left = comp->grab_geobox.x;
    int new_right = comp->grab_geobox.x + comp->grab_geobox.width;
    int new_top = comp->grab_geobox.y;
    int new_bottom = comp->grab_geobox.y + comp->grab_geobox.height;

    if (comp->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) new_top = new_bottom - 1;
    } else if (comp->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) new_bottom = new_top + 1;
    }
    if (comp->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) new_left = new_right - 1;
    } else if (comp->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) new_right = new_left + 1;
    }

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;

    if (comp->grabbed_kind == WINDOW_KIND_TOPLEVEL) {
        ManagedWindow* win = comp->grabbed_window;
        struct wlr_box* geo_box = &win->xdg_toplevel->base->geometry;
        wlr_scene_node_set_position(&win->scene_tree->node,
            new_left - geo_box->x, new_top - geo_box->y);
        wlr_xdg_toplevel_set_size(win->xdg_toplevel, new_width, new_height);
    } else {
        XWindow* win = comp->grabbed_window;
        wlr_scene_node_set_position(&win->scene_tree->node, new_left, new_top);
        // X11 configure takes the full box, not just a size — unlike xdg-shell.
        wlr_xwayland_surface_configure(win->xsurface, new_left, new_top, new_width, new_height);
        update_xwindow_border(win);
    }
}

// =================================================================
// XDG-SHELL / WINDOWS
// =================================================================

// =================================================================
// FOREIGN-TOPLEVEL-MANAGEMENT  (lets screenshare pickers, taskbars, etc.
// see and control open windows by name — Discord's "share a window"
// list is populated through this)
// =================================================================

static void toplevel_handle_request_maximize(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, toplevel_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event* event = data;
    wlr_foreign_toplevel_handle_v1_set_maximized(win->toplevel_handle, event->maximized);
}

static void toplevel_handle_request_minimize(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, toplevel_request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event* event = data;
    wlr_foreign_toplevel_handle_v1_set_minimized(win->toplevel_handle, event->minimized);
}

static void toplevel_handle_request_activate(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, toplevel_request_activate);
    focus_window(win);
}

static void toplevel_handle_request_fullscreen(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, toplevel_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event* event = data;
    wlr_xdg_toplevel_set_fullscreen(win->xdg_toplevel, event->fullscreen);
    // window_request_fullscreen (the xdg-shell listener) does the actual
    // resize/reposition work once the client acks via its own request.
}

static void toplevel_handle_request_close(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, toplevel_request_close);
    wlr_xdg_toplevel_send_close(win->xdg_toplevel);
}

static void window_create_toplevel_handle(ManagedWindow* win) {
    Compositor* comp = win->comp;
    win->toplevel_handle = wlr_foreign_toplevel_handle_v1_create(comp->toplevel_manager);

    const char* title = win->xdg_toplevel->title;
    const char* app_id = win->xdg_toplevel->app_id;
    wlr_foreign_toplevel_handle_v1_set_title(win->toplevel_handle, title ? title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(win->toplevel_handle, app_id ? app_id : "");

    struct wlr_output* output = wlr_output_layout_output_at(comp->output_layout,
        win->scene_tree->node.x, win->scene_tree->node.y);
    if (output) {
        wlr_foreign_toplevel_handle_v1_output_enter(win->toplevel_handle, output);
    }

    win->toplevel_request_maximize.notify = toplevel_handle_request_maximize;
    wl_signal_add(&win->toplevel_handle->events.request_maximize, &win->toplevel_request_maximize);
    win->toplevel_request_minimize.notify = toplevel_handle_request_minimize;
    wl_signal_add(&win->toplevel_handle->events.request_minimize, &win->toplevel_request_minimize);
    win->toplevel_request_activate.notify = toplevel_handle_request_activate;
    wl_signal_add(&win->toplevel_handle->events.request_activate, &win->toplevel_request_activate);
    win->toplevel_request_fullscreen.notify = toplevel_handle_request_fullscreen;
    wl_signal_add(&win->toplevel_handle->events.request_fullscreen, &win->toplevel_request_fullscreen);
    win->toplevel_request_close.notify = toplevel_handle_request_close;
    wl_signal_add(&win->toplevel_handle->events.request_close, &win->toplevel_request_close);
}

static void window_map(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, map);
    Compositor* comp = win->comp;

    if (comp->primary_output) {
        struct wlr_box box;
        wlr_output_layout_get_box(comp->output_layout, comp->primary_output, &box);
        wlr_scene_node_set_position(&win->scene_tree->node, box.x + 50, box.y + 50);
    }

    wl_list_insert(&comp->toplevels, &win->link);
    update_window_border(win);
    window_create_toplevel_handle(win);
    focus_window(win);
    printf("[Window] Mapped: '%s'\n", win->xdg_toplevel->title ? win->xdg_toplevel->title : "(untitled)");
}

static void window_unmap(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, unmap);
    if (win == win->comp->grabbed_window) {
        reset_cursor_mode(win->comp);
    }
    wl_list_remove(&win->link);
}

static void window_commit(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, commit);
    if (win->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(win->xdg_toplevel, 0, 0);
    }
    update_window_border(win);
    if (win->toplevel_handle) {
        const char* title = win->xdg_toplevel->title;
        const char* app_id = win->xdg_toplevel->app_id;
        wlr_foreign_toplevel_handle_v1_set_title(win->toplevel_handle, title ? title : "");
        wlr_foreign_toplevel_handle_v1_set_app_id(win->toplevel_handle, app_id ? app_id : "");
    }
}

static void window_destroy(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, destroy);
    if (win == win->comp->grabbed_window) {
        reset_cursor_mode(win->comp);
    }
    wl_list_remove(&win->map.link);
    wl_list_remove(&win->unmap.link);
    wl_list_remove(&win->commit.link);
    wl_list_remove(&win->destroy.link);
    wl_list_remove(&win->request_move.link);
    wl_list_remove(&win->request_resize.link);
    wl_list_remove(&win->request_fullscreen.link);
    if (win->toplevel_handle) {
        wl_list_remove(&win->toplevel_request_maximize.link);
        wl_list_remove(&win->toplevel_request_minimize.link);
        wl_list_remove(&win->toplevel_request_activate.link);
        wl_list_remove(&win->toplevel_request_fullscreen.link);
        wl_list_remove(&win->toplevel_request_close.link);
        wlr_foreign_toplevel_handle_v1_destroy(win->toplevel_handle);
    }
    printf("[Window] Destroyed.\n");
    free(win);
}

// Clients can ask to be moved/resized too (e.g. dragging a CSD titlebar on a
// client that ignores server-side decoration). We honor that the same way.
static void window_request_move(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, request_move);
    begin_interactive(win, WINDOW_KIND_TOPLEVEL, CURSOR_MOVE, 0);
}

static void window_request_resize(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, request_resize);
    struct wlr_xdg_toplevel_resize_event* event = data;
    begin_interactive(win, WINDOW_KIND_TOPLEVEL, CURSOR_RESIZE, event->edges);
}

static void window_request_fullscreen(struct wl_listener* listener, void* data) {
    ManagedWindow* win = wl_container_of(listener, win, request_fullscreen);
    Compositor* comp = win->comp;
    bool want = win->xdg_toplevel->requested.fullscreen;

    if (want && !win->is_fullscreen) {
        win->saved_box = window_get_box(win, WINDOW_KIND_TOPLEVEL);

        struct wlr_output* output = win->xdg_toplevel->requested.fullscreen_output;
        if (!output) {
            output = wlr_output_layout_output_at(comp->output_layout,
                win->scene_tree->node.x, win->scene_tree->node.y);
        }
        if (!output && !wl_list_empty(&comp->outputs)) {
            Output* first = wl_container_of(comp->outputs.next, first, link);
            output = first->wlr_output;
        }
        if (output) {
            struct wlr_box box;
            wlr_output_layout_get_box(comp->output_layout, output, &box);
            wlr_scene_node_set_position(&win->scene_tree->node, box.x, box.y);
            wlr_xdg_toplevel_set_size(win->xdg_toplevel, box.width, box.height);
        }
        wlr_scene_node_set_enabled(&win->border_top->node, false);
        wlr_scene_node_set_enabled(&win->border_bottom->node, false);
        wlr_scene_node_set_enabled(&win->border_left->node, false);
        wlr_scene_node_set_enabled(&win->border_right->node, false);
        win->is_fullscreen = true;
        wlr_xdg_toplevel_set_fullscreen(win->xdg_toplevel, true);

    } else if (!want && win->is_fullscreen) {
        wlr_scene_node_set_position(&win->scene_tree->node, win->saved_box.x, win->saved_box.y);
        wlr_xdg_toplevel_set_size(win->xdg_toplevel, win->saved_box.width, win->saved_box.height);
        wlr_scene_node_set_enabled(&win->border_top->node, true);
        wlr_scene_node_set_enabled(&win->border_bottom->node, true);
        wlr_scene_node_set_enabled(&win->border_left->node, true);
        wlr_scene_node_set_enabled(&win->border_right->node, true);
        win->is_fullscreen = false;
        wlr_xdg_toplevel_set_fullscreen(win->xdg_toplevel, false);
        update_window_border(win);
    }
}

static void compositor_new_xdg_toplevel(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, new_xdg_toplevel);
    struct wlr_xdg_toplevel* xdg_toplevel = data;

    ManagedWindow* win = calloc(1, sizeof(ManagedWindow));
    win->kind = WINDOW_KIND_TOPLEVEL;
    win->comp = comp;
    win->workspace = comp->current_workspace;
    win->xdg_toplevel = xdg_toplevel;
    win->scene_tree = wlr_scene_xdg_surface_create(comp->layer_normal, xdg_toplevel->base);
    win->scene_tree->node.data = win;
    xdg_toplevel->base->data = win->scene_tree;

    win->border_top    = wlr_scene_rect_create(win->scene_tree, 0, 0, comp->config.border_color_unfocused);
    win->border_bottom = wlr_scene_rect_create(win->scene_tree, 0, 0, comp->config.border_color_unfocused);
    win->border_left   = wlr_scene_rect_create(win->scene_tree, 0, 0, comp->config.border_color_unfocused);
    win->border_right  = wlr_scene_rect_create(win->scene_tree, 0, 0, comp->config.border_color_unfocused);

    win->map.notify = window_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &win->map);
    win->unmap.notify = window_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &win->unmap);
    win->commit.notify = window_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &win->commit);
    win->destroy.notify = window_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &win->destroy);
    win->request_move.notify = window_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &win->request_move);
    win->request_resize.notify = window_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &win->request_resize);
    win->request_fullscreen.notify = window_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &win->request_fullscreen);
}

// Tell every client "the compositor will draw decorations" so well-behaved
// clients (kitty included) skip drawing their own titlebar.
static void new_toplevel_decoration(struct wl_listener* listener, void* data) {
    struct wlr_xdg_toplevel_decoration_v1* decoration = data;

    // set_mode() internally schedules a configure, which asserts the
    // underlying xdg_surface has already done its first commit. Clients are
    // allowed to request a decoration mode before that first commit (kitty
    // does), so in that case we just record the mode directly — it'll be
    // included automatically when the surface's own first configure goes out.
    if (decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        decoration->scheduled_mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    }
}

// =================================================================
// XDG-POPUP (context menus, dropdowns, tooltips)
// =================================================================

static void popup_commit(struct wl_listener* listener, void* data) {
    Popup* popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit) {
        // An empty configure is enough to let the client map the popup.
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void popup_destroy(struct wl_listener* listener, void* data) {
    Popup* popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void compositor_new_xdg_popup(struct wl_listener* listener, void* data) {
    struct wlr_xdg_popup* xdg_popup = data;

    Popup* popup = calloc(1, sizeof(Popup));
    popup->xdg_popup = xdg_popup;

    // Popups must be attached under their parent's scene node so they get
    // rendered (and positioned) relative to the window that spawned them.
    struct wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (!parent || !parent->data) {
        fprintf(stderr, "WARN: popup with no valid parent scene node, dropping\n");
        free(popup);
        return;
    }
    struct wlr_scene_tree* parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

// =================================================================
// LAYER-SHELL  (bars, wallpaper, launchers, notification popups —
// swaybg, bemenu-wayland, swaync, dunst's Wayland backend, etc.)
// =================================================================

typedef struct {
    Compositor*                       comp;
    struct wlr_layer_surface_v1*    layer_surface;
    struct wlr_scene_layer_surface_v1* scene_layer_surface;
    struct wl_listener                 map;
    struct wl_listener                 unmap;
    struct wl_listener                 destroy;
    struct wl_listener                 commit;
} LayerSurface;

static struct wlr_scene_tree* layer_tree_for(Compositor* comp, enum zwlr_layer_shell_v1_layer layer) {
    switch (layer) {
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return comp->layer_bg;
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:     return comp->layer_bottom;
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP:        return comp->layer_top;
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:    return comp->layer_overlay;
        default:                                     return comp->layer_top;
    }
}

// Re-lays the surface out per its anchor/margin/exclusive-zone against its
// assigned output. Simplified: doesn't stack multiple bars' exclusive zones
// against each other, each surface just gets the full output box.
static void layer_surface_reconfigure(LayerSurface* ls) {
    struct wlr_layer_surface_v1* layer_surface = ls->layer_surface;
    if (!layer_surface->output) return;

    struct wlr_box full_area;
    wlr_output_layout_get_box(ls->comp->output_layout, layer_surface->output, &full_area);
    struct wlr_box usable_area = full_area;
    wlr_scene_layer_surface_v1_configure(ls->scene_layer_surface, &full_area, &usable_area);
}

static void layer_surface_map(struct wl_listener* listener, void* data) {
    LayerSurface* ls = wl_container_of(listener, ls, map);
    struct wlr_layer_surface_v1* layer_surface = ls->layer_surface;

    // Only give keyboard focus to surfaces that actually asked for it
    // (launchers/bars). Notification popups etc. request NONE, so they
    // never steal focus from whatever you were doing.
    if (layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        struct wlr_keyboard* keyboard = wlr_seat_get_keyboard(ls->comp->seat);
        wlr_seat_keyboard_notify_enter(ls->comp->seat, layer_surface->surface,
            keyboard ? keyboard->keycodes : NULL,
            keyboard ? keyboard->num_keycodes : 0,
            keyboard ? &keyboard->modifiers : NULL);
    }
}

static void layer_surface_unmap(struct wl_listener* listener, void* data) {
    // Nothing to do — we don't track a "previously focused" stack for this;
    // the next click/keypress naturally refocuses a real window.
}

static void layer_surface_commit(struct wl_listener* listener, void* data) {
    LayerSurface* ls = wl_container_of(listener, ls, commit);
    layer_surface_reconfigure(ls);
}

static void layer_surface_destroy(struct wl_listener* listener, void* data) {
    LayerSurface* ls = wl_container_of(listener, ls, destroy);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    free(ls);
}

static void compositor_new_layer_surface(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, new_layer_surface);
    struct wlr_layer_surface_v1* layer_surface = data;

    if (!layer_surface->output) {
        layer_surface->output = comp->primary_output;
    }
    if (!layer_surface->output) {
        fprintf(stderr, "[LayerShell] New surface with no output available, ignoring.\n");
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    struct wlr_scene_tree* parent_tree = layer_tree_for(comp, layer_surface->pending.layer);
    struct wlr_scene_layer_surface_v1* scene_layer_surface =
        wlr_scene_layer_surface_v1_create(parent_tree, layer_surface);

    LayerSurface* ls = calloc(1, sizeof(LayerSurface));
    ls->comp = comp;
    ls->layer_surface = layer_surface;
    ls->scene_layer_surface = scene_layer_surface;

    ls->map.notify = layer_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &ls->map);
    ls->unmap.notify = layer_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &ls->unmap);
    ls->commit.notify = layer_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);
    ls->destroy.notify = layer_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &ls->destroy);

    printf("[LayerShell] New surface: '%s'\n", layer_surface->namespace ? layer_surface->namespace : "(none)");
}

// =================================================================
// XWAYLAND  (legacy X11-only apps — mate-terminal, older GTK3 apps, etc.)
// =================================================================

static void xwindow_toplevel_request_maximize(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, toplevel_request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event* event = data;
    wlr_foreign_toplevel_handle_v1_set_maximized(win->toplevel_handle, event->maximized);
}

static void xwindow_toplevel_request_minimize(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, toplevel_request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event* event = data;
    wlr_foreign_toplevel_handle_v1_set_minimized(win->toplevel_handle, event->minimized);
}

static void xwindow_toplevel_request_activate(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, toplevel_request_activate);
    focus_xwindow(win);
}

static void xwindow_toplevel_request_fullscreen(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, toplevel_request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event* event = data;
    wlr_xwayland_surface_set_fullscreen(win->xsurface, event->fullscreen);
}

static void xwindow_toplevel_request_close(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, toplevel_request_close);
    wlr_xwayland_surface_close(win->xsurface);
}

static void xwindow_create_toplevel_handle(XWindow* win) {
    Compositor* comp = win->comp;
    win->toplevel_handle = wlr_foreign_toplevel_handle_v1_create(comp->toplevel_manager);

    const char* title = win->xsurface->title;
    const char* app_id = win->xsurface->class; // X11 calls it WM_CLASS, not app_id
    wlr_foreign_toplevel_handle_v1_set_title(win->toplevel_handle, title ? title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(win->toplevel_handle, app_id ? app_id : "");

    struct wlr_output* output = wlr_output_layout_output_at(comp->output_layout,
        win->scene_tree->node.x, win->scene_tree->node.y);
    if (output) {
        wlr_foreign_toplevel_handle_v1_output_enter(win->toplevel_handle, output);
    }

    win->toplevel_request_maximize.notify = xwindow_toplevel_request_maximize;
    wl_signal_add(&win->toplevel_handle->events.request_maximize, &win->toplevel_request_maximize);
    win->toplevel_request_minimize.notify = xwindow_toplevel_request_minimize;
    wl_signal_add(&win->toplevel_handle->events.request_minimize, &win->toplevel_request_minimize);
    win->toplevel_request_activate.notify = xwindow_toplevel_request_activate;
    wl_signal_add(&win->toplevel_handle->events.request_activate, &win->toplevel_request_activate);
    win->toplevel_request_fullscreen.notify = xwindow_toplevel_request_fullscreen;
    wl_signal_add(&win->toplevel_handle->events.request_fullscreen, &win->toplevel_request_fullscreen);
    win->toplevel_request_close.notify = xwindow_toplevel_request_close;
    wl_signal_add(&win->toplevel_handle->events.request_close, &win->toplevel_request_close);
}

static void xwindow_map(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, map);
    wl_list_insert(&win->comp->xwayland_windows, &win->link);
    wlr_scene_node_set_enabled(&win->scene_tree->node, true);
    if (!win->toplevel_handle && !win->xsurface->override_redirect) {
        xwindow_create_toplevel_handle(win);
    }
    printf("[XWayland] Mapped: '%s'\n",
        win->xsurface->title ? win->xsurface->title : "(untitled)");
}

static void xwindow_unmap(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, unmap);
    if (win == win->comp->grabbed_window) {
        reset_cursor_mode(win->comp);
    }
    wlr_scene_node_set_enabled(&win->scene_tree->node, false);
    wl_list_remove(&win->link);
}

// X11 surfaces exist (xsurface) before they necessarily have a wl_surface
// behind them. "associate" fires once that wl_surface shows up — that's
// when we can actually attach it to the scene graph.
static void xwindow_commit(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, commit);
    // Catches resizes the client makes on its own, not just ones we drove
    // via process_cursor_resize()'s explicit configure calls.
    update_xwindow_border(win);
    if (win->toplevel_handle) {
        const char* title = win->xsurface->title;
        const char* app_id = win->xsurface->class;
        wlr_foreign_toplevel_handle_v1_set_title(win->toplevel_handle, title ? title : "");
        wlr_foreign_toplevel_handle_v1_set_app_id(win->toplevel_handle, app_id ? app_id : "");
    }
}

static void xwindow_associate(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, associate);

    // Override-redirect surfaces (notification toasts, tooltips, X11 context
    // menus) manage their own position/appearance — they should never get
    // our border/focus treatment, and belong above normal windows so they're
    // never obscured.
    struct wlr_scene_tree* parent_tree = win->xsurface->override_redirect
        ? win->comp->layer_overlay : win->comp->layer_normal;
    win->scene_tree = wlr_scene_subsurface_tree_create(parent_tree, win->xsurface->surface);
    // Tag the node so window_at() can find and identify this window, same
    // mechanism ManagedWindow relies on. Without this, clicks/drags on X11
    // windows are silently ignored — window_at() finds nothing here.
    win->scene_tree->node.data = win;

    // Steam names its toast notifications distinctly and consistently —
    // matching on that is exact, unlike guessing from class/position (which
    // also matched context menus/tooltips and missed real toasts).
    bool is_steam_toast = win->xsurface->title &&
        strstr(win->xsurface->title, "notificationtoasts");

    if (is_steam_toast && win->comp->primary_output) {
        struct wlr_box box;
        wlr_output_layout_get_box(win->comp->output_layout, win->comp->primary_output, &box);
        int x = box.x + box.width - win->xsurface->width - 20;
        int y = box.y + box.height - win->xsurface->height - 20;
        wlr_scene_node_set_position(&win->scene_tree->node, x, y);
        wlr_xwayland_surface_configure(win->xsurface, x, y, win->xsurface->width, win->xsurface->height);
    } else {
        wlr_scene_node_set_position(&win->scene_tree->node, win->xsurface->x, win->xsurface->y);
    }
    wlr_scene_node_set_enabled(&win->scene_tree->node, false);

    if (!win->xsurface->override_redirect) {
        win->border_top    = wlr_scene_rect_create(win->scene_tree, 0, 0, win->comp->config.border_color_unfocused);
        win->border_bottom = wlr_scene_rect_create(win->scene_tree, 0, 0, win->comp->config.border_color_unfocused);
        win->border_left   = wlr_scene_rect_create(win->scene_tree, 0, 0, win->comp->config.border_color_unfocused);
        win->border_right  = wlr_scene_rect_create(win->scene_tree, 0, 0, win->comp->config.border_color_unfocused);
        update_xwindow_border(win);
    }

    win->map.notify = xwindow_map;
    wl_signal_add(&win->xsurface->surface->events.map, &win->map);
    win->unmap.notify = xwindow_unmap;
    wl_signal_add(&win->xsurface->surface->events.unmap, &win->unmap);
    win->commit.notify = xwindow_commit;
    wl_signal_add(&win->xsurface->surface->events.commit, &win->commit);
}

static void xwindow_dissociate(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, dissociate);
    wl_list_remove(&win->map.link);
    wl_list_remove(&win->unmap.link);
    wl_list_remove(&win->commit.link);
    if (win->scene_tree) {
        // Destroying the tree also destroys the 4 border rects — they're
        // children of it, so no separate cleanup needed for those.
        wlr_scene_node_destroy(&win->scene_tree->node);
        win->scene_tree = NULL;
    }
}

// Same as window_request_move/resize, but for X11 clients (e.g. dragging a
// window via its own decorations, or a taskbar-style app requesting it).
static void xwindow_request_move(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, request_move);
    begin_interactive(win, WINDOW_KIND_XWAYLAND, CURSOR_MOVE, 0);
}

static void xwindow_request_resize(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, request_resize);
    struct wlr_xwayland_resize_event* event = data;
    begin_interactive(win, WINDOW_KIND_XWAYLAND, CURSOR_RESIZE, event->edges);
}

static void xwindow_request_fullscreen(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, request_fullscreen);
    Compositor* comp = win->comp;
    bool want = win->xsurface->fullscreen;

    if (want && !win->is_fullscreen) {
        win->saved_box = window_get_box(win, WINDOW_KIND_XWAYLAND);

        struct wlr_output* output = wlr_output_layout_output_at(comp->output_layout,
            win->scene_tree->node.x, win->scene_tree->node.y);
        if (!output && !wl_list_empty(&comp->outputs)) {
            Output* first = wl_container_of(comp->outputs.next, first, link);
            output = first->wlr_output;
        }
        if (output) {
            struct wlr_box box;
            wlr_output_layout_get_box(comp->output_layout, output, &box);
            wlr_scene_node_set_position(&win->scene_tree->node, box.x, box.y);
            wlr_xwayland_surface_configure(win->xsurface, box.x, box.y, box.width, box.height);
        }
        wlr_scene_node_set_enabled(&win->border_top->node, false);
        wlr_scene_node_set_enabled(&win->border_bottom->node, false);
        wlr_scene_node_set_enabled(&win->border_left->node, false);
        wlr_scene_node_set_enabled(&win->border_right->node, false);
        win->is_fullscreen = true;
        wlr_xwayland_surface_set_fullscreen(win->xsurface, true);

    } else if (!want && win->is_fullscreen) {
        wlr_scene_node_set_position(&win->scene_tree->node, win->saved_box.x, win->saved_box.y);
        wlr_xwayland_surface_configure(win->xsurface, win->saved_box.x, win->saved_box.y,
            win->saved_box.width, win->saved_box.height);
        wlr_scene_node_set_enabled(&win->border_top->node, true);
        wlr_scene_node_set_enabled(&win->border_bottom->node, true);
        wlr_scene_node_set_enabled(&win->border_left->node, true);
        wlr_scene_node_set_enabled(&win->border_right->node, true);
        win->is_fullscreen = false;
        wlr_xwayland_surface_set_fullscreen(win->xsurface, false);
        update_xwindow_border(win);
    }
}

// X clients propose their own geometry; we ack it (optionally overriding).
static void xwindow_request_configure(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, request_configure);
    struct wlr_xwayland_surface_configure_event* event = data;
    wlr_xwayland_surface_configure(win->xsurface, event->x, event->y, event->width, event->height);
    if (win->scene_tree) {
        wlr_scene_node_set_position(&win->scene_tree->node, event->x, event->y);
    }
}

static void xwindow_destroy(struct wl_listener* listener, void* data) {
    XWindow* win = wl_container_of(listener, win, destroy);
    if (win == win->comp->grabbed_window) {
        reset_cursor_mode(win->comp);
    }
    // associate/dissociate/destroy/request_* are wired unconditionally in
    // compositor_new_xwayland_surface, so always safe to remove here.
    wl_list_remove(&win->associate.link);
    wl_list_remove(&win->dissociate.link);
    wl_list_remove(&win->destroy.link);
    wl_list_remove(&win->request_configure.link);
    wl_list_remove(&win->request_move.link);
    wl_list_remove(&win->request_resize.link);
    wl_list_remove(&win->request_fullscreen.link);
    // map/unmap/commit are only wired if xwindow_associate ran, and already
    // removed by xwindow_dissociate if that fired first (the common case).
    // Guard on scene_tree so we don't double-remove (which would crash) if
    // destroy fires without a preceding dissociate.
    if (win->scene_tree) {
        wl_list_remove(&win->map.link);
        wl_list_remove(&win->unmap.link);
        wl_list_remove(&win->commit.link);
        wlr_scene_node_destroy(&win->scene_tree->node);
    }
    if (win->toplevel_handle) {
        wl_list_remove(&win->toplevel_request_maximize.link);
        wl_list_remove(&win->toplevel_request_minimize.link);
        wl_list_remove(&win->toplevel_request_activate.link);
        wl_list_remove(&win->toplevel_request_fullscreen.link);
        wl_list_remove(&win->toplevel_request_close.link);
        wlr_foreign_toplevel_handle_v1_destroy(win->toplevel_handle);
    }
    printf("[XWayland] Destroyed.\n");
    free(win);
}

static void compositor_new_xwayland_surface(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, new_xwayland_surface);
    struct wlr_xwayland_surface* xsurface = data;

    XWindow* win = calloc(1, sizeof(XWindow));
    win->kind = WINDOW_KIND_XWAYLAND;
    win->comp = comp;
    win->workspace = comp->current_workspace;
    win->xsurface = xsurface;
    xsurface->data = win;

    win->associate.notify = xwindow_associate;
    wl_signal_add(&xsurface->events.associate, &win->associate);
    win->dissociate.notify = xwindow_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &win->dissociate);
    win->destroy.notify = xwindow_destroy;
    wl_signal_add(&xsurface->events.destroy, &win->destroy);
    win->request_configure.notify = xwindow_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &win->request_configure);
    win->request_move.notify = xwindow_request_move;
    wl_signal_add(&xsurface->events.request_move, &win->request_move);
    win->request_resize.notify = xwindow_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &win->request_resize);
    win->request_fullscreen.notify = xwindow_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &win->request_fullscreen);

    printf("[XWayland] New surface: '%s'\n", xsurface->title ? xsurface->title : "(untitled)");
}

static void xwayland_ready(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, xwayland_ready);
    wlr_xwayland_set_seat(comp->xwayland, comp->seat);
    setenv("DISPLAY", comp->xwayland->display_name, 1);
    printf("[XWayland] Ready on DISPLAY=%s\n", comp->xwayland->display_name);
}

// =================================================================
// KEYBOARD
// =================================================================

// Cycles keyboard focus to the next window (Alt+Tab). Only native Wayland
// toplevels for now — Xwayland windows aren't in comp->toplevels.
static void cycle_focus(Compositor* comp) {
    if (wl_list_length(&comp->toplevels) < 2) return;
    ManagedWindow* next = wl_container_of(comp->toplevels.next->next, next, link);
    focus_generic(next, WINDOW_KIND_TOPLEVEL);
}

// Closes whichever window currently has keyboard focus, native or X11.
static void close_focused(Compositor* comp) {
    struct wlr_surface* surface = comp->seat->keyboard_state.focused_surface;
    if (!surface) return;
    struct wlr_xdg_toplevel* xdg = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (xdg) { wlr_xdg_toplevel_send_close(xdg); return; }
    struct wlr_xwayland_surface* xw = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (xw) { wlr_xwayland_surface_close(xw); }
}

static void spawn(const char* cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
}

#define WORKSPACE_COUNT 10

// Shows/hides one window based on whether its workspace is the active one.
static void apply_window_workspace_visibility(void* win, WindowKind kind, int active_ws) {
    if (kind == WINDOW_KIND_TOPLEVEL) {
        ManagedWindow* w = win;
        wlr_scene_node_set_enabled(&w->scene_tree->node, w->omnipresent || w->workspace == active_ws);
    } else {
        XWindow* w = win;
        if (w->scene_tree) {
            wlr_scene_node_set_enabled(&w->scene_tree->node, w->omnipresent || w->workspace == active_ws);
        }
    }
}

static void switch_workspace(Compositor* comp, int ws) {
    if (ws < 0 || ws >= WORKSPACE_COUNT || ws == comp->current_workspace) return;
    comp->current_workspace = ws;

    ManagedWindow* mwin;
    wl_list_for_each(mwin, &comp->toplevels, link) {
        apply_window_workspace_visibility(mwin, WINDOW_KIND_TOPLEVEL, ws);
    }
    XWindow* xwin;
    wl_list_for_each(xwin, &comp->xwayland_windows, link) {
        apply_window_workspace_visibility(xwin, WINDOW_KIND_XWAYLAND, ws);
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "notify-send 'Workspace' 'Switched to workspace %d'", ws + 1);
    spawn(cmd);
}

// Moves whichever window has keyboard focus to workspace `ws`, hiding it
// immediately since that's almost always a different workspace than the
// one you're currently on.
static void move_focused_to_workspace(Compositor* comp, int ws) {
    if (ws < 0 || ws >= WORKSPACE_COUNT) return;
    struct wlr_surface* surface = comp->seat->keyboard_state.focused_surface;
    if (!surface) return;

    struct wlr_xdg_toplevel* xdg = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (xdg && xdg->base->data) {
        ManagedWindow* win = ((struct wlr_scene_tree*)xdg->base->data)->node.data;
        if (win) {
            win->workspace = ws;
            apply_window_workspace_visibility(win, WINDOW_KIND_TOPLEVEL, comp->current_workspace);
        }
        return;
    }
    struct wlr_xwayland_surface* xw = wlr_xwayland_surface_try_from_wlr_surface(surface);
    if (xw && xw->data) {
        XWindow* win = xw->data;
        win->workspace = ws;
        apply_window_workspace_visibility(win, WINDOW_KIND_XWAYLAND, comp->current_workspace);
    }
}

// Toggles whether the focused window shows on every workspace or just its own.
static void toggle_omnipresent(Compositor* comp) {
    struct wlr_surface* surface = comp->seat->keyboard_state.focused_surface;
    if (!surface) return;

    const char* title = NULL;
    bool now_omnipresent = false;

    struct wlr_xdg_toplevel* xdg = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (xdg && xdg->base->data) {
        ManagedWindow* win = ((struct wlr_scene_tree*)xdg->base->data)->node.data;
        if (win) {
            win->omnipresent = !win->omnipresent;
            apply_window_workspace_visibility(win, WINDOW_KIND_TOPLEVEL, comp->current_workspace);
            title = win->xdg_toplevel->title;
            now_omnipresent = win->omnipresent;
        }
    } else {
        struct wlr_xwayland_surface* xw = wlr_xwayland_surface_try_from_wlr_surface(surface);
        if (xw && xw->data) {
            XWindow* win = xw->data;
            win->omnipresent = !win->omnipresent;
            apply_window_workspace_visibility(win, WINDOW_KIND_XWAYLAND, comp->current_workspace);
            title = win->xsurface->title;
            now_omnipresent = win->omnipresent;
        }
    }

    if (!title) return;
    // FIX
    char cmd[128];
    if (now_omnipresent) {
        snprintf(cmd, sizeof(cmd), "notify-send 'Omnipresence' '%s is now omnipresent'", title);
    } else {
        snprintf(cmd, sizeof(cmd), "notify-send 'Omnipresence' '%s is no longer omnipresent'", title);
    }
    spawn(cmd);
}

// Checks one configured bind against the currently-held modifiers + a
// pressed keysym, and runs it (spawn/quit/close/cycle_window) if it matches.
static bool try_keybind(Compositor* comp, uint32_t mods, xkb_keysym_t sym) {
    for (int i = 0; i < comp->config.keybind_count; i++) {
        KeybindConfig* kb = &comp->config.keybinds[i];
        xkb_keysym_t bound_sym = xkb_keysym_from_name(kb->key, XKB_KEYSYM_CASE_INSENSITIVE);
        if (bound_sym != sym) continue;

        bool mods_match =
            (bool)(mods & WLR_MODIFIER_LOGO)  == kb->mod_super &&
            (bool)(mods & WLR_MODIFIER_ALT)   == kb->mod_alt &&
            (bool)(mods & WLR_MODIFIER_SHIFT) == kb->mod_shift &&
            (bool)(mods & WLR_MODIFIER_CTRL)  == kb->mod_ctrl;
        if (!mods_match) continue;

        if (kb->command) {
            spawn(kb->command);
        } else if (kb->action) {
            int n;
            if (strcmp(kb->action, "quit") == 0) wl_display_terminate(comp->wl_display);
            else if (strcmp(kb->action, "close") == 0) close_focused(comp);
            else if (strcmp(kb->action, "cycle_window") == 0) cycle_focus(comp);
            else if (strcmp(kb->action, "toggle_omnipresent") == 0) toggle_omnipresent(comp);
            else if (sscanf(kb->action, "workspace_%d", &n) == 1) switch_workspace(comp, n - 1);
            else if (sscanf(kb->action, "move_to_workspace_%d", &n) == 1) move_focused_to_workspace(comp, n - 1);
        }
        return true;
    }
    return false;
}

static void keyboard_handle_modifiers(struct wl_listener* listener, void* data) {
    Keyboard* kb = wl_container_of(listener, kb, modifiers);
    wlr_seat_set_keyboard(kb->comp->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->comp->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener* listener, void* data) {
    Keyboard* kb = wl_container_of(listener, kb, key);
    Compositor* comp = kb->comp;
    struct wlr_keyboard_key_event* event = data;
    struct wlr_seat* seat = comp->seat;
    uint32_t keycode = event->keycode + 8;

    const xkb_keysym_t* bind_syms;
    int n_bind_syms = xkb_keymap_key_get_syms_by_level(
        kb->wlr_keyboard->keymap, keycode, 0, 0, &bind_syms);

    bool handled = false;
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < n_bind_syms; i++) {
            char name[64];
            xkb_keysym_get_name(bind_syms[i], name, sizeof(name));
            //i aint printing allat FIX NR 6
            //printf("[DEBUG] mods=0x%x sym='%s'\n", mods, name);
            if (try_keybind(comp, mods, bind_syms[i])) {
                handled = true;
            }
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener* listener, void* data) {
    Keyboard* kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

static void compositor_new_keyboard(Compositor* comp, struct wlr_input_device* device) {
    struct wlr_keyboard* wlr_keyboard = wlr_keyboard_from_input_device(device);

    Keyboard* kb = calloc(1, sizeof(Keyboard));
    kb->comp = comp;
    kb->wlr_keyboard = wlr_keyboard;

    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap* keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    kb->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &kb->modifiers);
    kb->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &kb->key);
    kb->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &kb->destroy);

    wlr_seat_set_keyboard(comp->seat, kb->wlr_keyboard);
    wl_list_insert(&comp->keyboards, &kb->link);

    printf("[Input] Keyboard connected.\n");
}

// =================================================================
// POINTER / CURSOR
// =================================================================

static void update_pointer_constraint(Compositor* comp, struct wlr_surface* surface);
static void clamp_cursor_to_constraint(Compositor* comp);

static void compositor_new_pointer(Compositor* comp, struct wlr_input_device* device) {
    wlr_cursor_attach_input_device(comp->cursor, device);
    printf("[Input] Pointer connected.\n");
}

static void process_cursor_motion(Compositor* comp, uint32_t time) {
    if (comp->drag_icon_tree) {
        wlr_scene_node_set_position(&comp->drag_icon_tree->node, comp->cursor->x, comp->cursor->y);
    }

    if (comp->cursor_mode == CURSOR_MOVE) {
        process_cursor_move(comp);
        return;
    } else if (comp->cursor_mode == CURSOR_RESIZE) {
        process_cursor_resize(comp);
        return;
    }

    double sx, sy;
    struct wlr_surface* surface = NULL;
    WindowKind kind;
    void* win = window_at(comp, comp->cursor->x, comp->cursor->y, &surface, &sx, &sy, &kind);

    if (!win) {
        wlr_cursor_set_xcursor(comp->cursor, comp->cursor_mgr, "default");
    }

    if (surface) {
        wlr_seat_pointer_notify_enter(comp->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(comp->seat, time, sx, sy);
        update_pointer_constraint(comp, surface);
        clamp_cursor_to_constraint(comp);
    } else {
        wlr_seat_pointer_clear_focus(comp->seat);
    }
}

static void cursor_motion(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, cursor_motion);
    struct wlr_pointer_motion_event* event = data;

    if (comp->active_constraint && comp->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        // Locked: don't move the visible cursor at all, just forward the
        // raw delta so the client can do its own relative mouse-look.
        wlr_relative_pointer_manager_v1_send_relative_motion(
            comp->relative_pointer_mgr, comp->seat,
            (uint64_t)event->time_msec * 1000, event->delta_x, event->delta_y,
            event->unaccel_dx, event->unaccel_dy);
        return;
    }

    wlr_cursor_move(comp->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(comp, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event* event = data;
    wlr_cursor_warp_absolute(comp->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(comp, event->time_msec);
}

// Alt+M1 = move, Alt+M2 = resize (from bottom-right corner). Held modifier
// checked via the keyboard state on the seat.
static bool alt_is_held(Compositor* comp) {
    struct wlr_keyboard* kb = wlr_seat_get_keyboard(comp->seat);
    if (!kb) return false;
    return wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_ALT;
}

static void cursor_button(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, cursor_button);
    struct wlr_pointer_button_event* event = data;

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_cursor_mode(comp);
        wlr_seat_pointer_notify_button(comp->seat, event->time_msec, event->button, event->state);
        return;
    }

    double sx, sy;
    struct wlr_surface* surface = NULL;
    WindowKind kind;
    void* win = window_at(comp, comp->cursor->x, comp->cursor->y, &surface, &sx, &sy, &kind);

    if (win && alt_is_held(comp)) {
        focus_generic(win, kind);
        if (event->button == BTN_LEFT) {
            begin_interactive(win, kind, CURSOR_MOVE, 0);
        } else if (event->button == BTN_RIGHT) {
            begin_interactive(win, kind, CURSOR_RESIZE, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
        }
        return; // consumed by the compositor, not forwarded to the client
    }

    wlr_seat_pointer_notify_button(comp->seat, event->time_msec, event->button, event->state);
    if (win) {
        focus_generic(win, kind);
    } else if (surface) {
        // window_at() only identifies tagged ManagedWindow/XWindow surfaces
        // — layer-shell surfaces (bemenu, etc.) fall through here. Clicking
        // one of those should still be able to grab keyboard focus.
        struct wlr_layer_surface_v1* layer_surface = wlr_layer_surface_v1_try_from_wlr_surface(surface);
        if (layer_surface && layer_surface->current.keyboard_interactive !=
                ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
            struct wlr_keyboard* keyboard = wlr_seat_get_keyboard(comp->seat);
            wlr_seat_keyboard_notify_enter(comp->seat, surface,
                keyboard ? keyboard->keycodes : NULL,
                keyboard ? keyboard->num_keycodes : 0,
                keyboard ? &keyboard->modifiers : NULL);
        }
    }
}

static void cursor_axis(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, cursor_axis);
    struct wlr_pointer_axis_event* event = data;
    wlr_seat_pointer_notify_axis(comp->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void cursor_frame(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, cursor_frame);
    wlr_seat_pointer_notify_frame(comp->seat);
}

static void compositor_new_input(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, new_input);
    struct wlr_input_device* device = data;

    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            compositor_new_keyboard(comp, device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            compositor_new_pointer(comp, device);
            break;
        default:
            break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&comp->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(comp->seat, caps);
}

static void seat_request_cursor(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event* event = data;
    struct wlr_seat_client* focused_client = comp->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(comp->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_selection(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, request_set_selection);
    struct wlr_seat_request_set_selection_event* event = data;
    wlr_seat_set_selection(comp->seat, event->source, event->serial);
}

// =================================================================
// DRAG-AND-DROP  (e.g. dragging a file from Nautilus into Firefox)
// =================================================================

static void drag_icon_destroy(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, drag_icon_destroy_listener);
    comp->drag_icon_tree = NULL;
    wl_list_remove(&listener->link);
}

static void seat_request_start_drag(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, request_start_drag);
    struct wlr_seat_request_start_drag_event* event = data;

    if (wlr_seat_validate_pointer_grab_serial(comp->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(comp->seat, event->drag, event->serial);
    } else {
        wlr_data_source_destroy(event->drag->source);
        return;
    }

    if (event->drag->icon) {
        comp->drag_icon_tree = wlr_scene_drag_icon_create(comp->layer_overlay, event->drag->icon);
        wlr_scene_node_set_position(&comp->drag_icon_tree->node, comp->cursor->x, comp->cursor->y);
        comp->drag_icon_destroy_listener.notify = drag_icon_destroy;
        wl_signal_add(&comp->drag_icon_tree->node.events.destroy, &comp->drag_icon_destroy_listener);
    }
}

// =================================================================
// POINTER LOCK / CONFINE  (pointer-constraints-v1 + relative-pointer-v1 —
// needed for FPS-style mouse-look in games, e.g. Roblox/Sober)
// =================================================================

typedef struct {
    Compositor*                          comp;
    struct wlr_pointer_constraint_v1* constraint;
    struct wl_listener                    destroy;
} ConstraintData;

static void constraint_handle_destroy(struct wl_listener* listener, void* data) {
    ConstraintData* cd = wl_container_of(listener, cd, destroy);
    if (cd->comp->active_constraint == cd->constraint) {
        cd->comp->active_constraint = NULL;
    }
    wl_list_remove(&cd->destroy.link);
    free(cd);
}

static void compositor_new_pointer_constraint(struct wl_listener* listener, void* data) {
    Compositor* comp = wl_container_of(listener, comp, new_pointer_constraint);
    struct wlr_pointer_constraint_v1* constraint = data;

    ConstraintData* cd = calloc(1, sizeof(ConstraintData));
    cd->comp = comp;
    cd->constraint = constraint;
    cd->destroy.notify = constraint_handle_destroy;
    wl_signal_add(&constraint->events.destroy, &cd->destroy);
}

// Activates/deactivates the constraint (if any) tied to whichever surface
// currently has pointer focus. Called on every pointer motion.
static void update_pointer_constraint(Compositor* comp, struct wlr_surface* surface) {
    if (comp->active_constraint && comp->active_constraint->surface != surface) {
        wlr_pointer_constraint_v1_send_deactivated(comp->active_constraint);
        comp->active_constraint = NULL;
    }
    if (!comp->active_constraint && surface) {
        struct wlr_pointer_constraint_v1* constraint =
            wlr_pointer_constraints_v1_constraint_for_surface(comp->pointer_constraints, surface, comp->seat);
        if (constraint) {
            comp->active_constraint = constraint;
            wlr_pointer_constraint_v1_send_activated(constraint);
        }
    }
}

// For CONFINED (not LOCKED) constraints: keeps the visible cursor from
// leaving the constrained window's box.
static void clamp_cursor_to_constraint(Compositor* comp) {
    if (!comp->active_constraint || comp->active_constraint->type != WLR_POINTER_CONSTRAINT_V1_CONFINED) return;
    struct wlr_surface* surface = comp->active_constraint->surface;

    struct wlr_box box = {0};
    struct wlr_xdg_toplevel* xdg = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (xdg && xdg->base->data) {
        ManagedWindow* w = ((struct wlr_scene_tree*)xdg->base->data)->node.data;
        if (w) box = window_get_box(w, WINDOW_KIND_TOPLEVEL);
    } else {
        struct wlr_xwayland_surface* xw = wlr_xwayland_surface_try_from_wlr_surface(surface);
        if (xw && xw->data) box = window_get_box(xw->data, WINDOW_KIND_XWAYLAND);
    }
    if (box.width <= 0 || box.height <= 0) return;

    double x = comp->cursor->x, y = comp->cursor->y;
    if (x < box.x) x = box.x;
    if (x > box.x + box.width) x = box.x + box.width;
    if (y < box.y) y = box.y;
    if (y > box.y + box.height) y = box.y + box.height;
    if (x != comp->cursor->x || y != comp->cursor->y) {
        wlr_cursor_warp(comp->cursor, NULL, x, y);
    }
}

// =================================================================
// CORE
// =================================================================

int compositor_init(Compositor* comp) {
    wlr_log_init(WLR_DEBUG, NULL);

    config_load_default(&comp->config);
    for (int i = 0; i < comp->config.env_count; i++) {
        setenv(comp->config.env_keys[i], comp->config.env_values[i], 1);
    }

    comp->wl_display = wl_display_create();
    if (!comp->wl_display) { fprintf(stderr, "FATAL: wl_display_create failed\n"); return 0; }

    // Create the socket now (so it exists before eager-mode Xwayland needs
    // it later), but DON'T setenv WAYLAND_DISPLAY yet — wlr_backend_autocreate()
    // below checks that var to decide DRM vs. nested-Wayland, and if it's
    // already set to our own not-yet-serving socket, it mistakes itself for
    // an outer compositor and hangs trying to connect to itself.
    const char* socket = wl_display_add_socket_auto(comp->wl_display);
    if (!socket) { fprintf(stderr, "FATAL: could not add Wayland socket\n"); return 0; }

    comp->backend = wlr_backend_autocreate(wl_display_get_event_loop(comp->wl_display), NULL);
    if (!comp->backend) { fprintf(stderr, "FATAL: wlr_backend_autocreate failed\n"); return 0; }

    // Safe to export now — backend type is already decided.
    setenv("WAYLAND_DISPLAY", socket, 1);
    printf("Wayland socket: %s\n", socket);

    comp->renderer = wlr_renderer_autocreate(comp->backend);
    if (!comp->renderer) { fprintf(stderr, "FATAL: wlr_renderer_autocreate failed\n"); return 0; }
    wlr_renderer_init_wl_display(comp->renderer, comp->wl_display);

    comp->allocator = wlr_allocator_autocreate(comp->backend, comp->renderer);
    if (!comp->allocator) { fprintf(stderr, "FATAL: wlr_allocator_autocreate failed\n"); return 0; }

    comp->wlr_compositor = wlr_compositor_create(comp->wl_display, 5, comp->renderer);
    wlr_subcompositor_create(comp->wl_display);
    wlr_data_device_manager_create(comp->wl_display);

    comp->output_layout = wlr_output_layout_create(comp->wl_display);
    wl_list_init(&comp->outputs);
    comp->new_output.notify = compositor_new_output;
    wl_signal_add(&comp->backend->events.new_output, &comp->new_output);

    // Both fully self-contained: no signals to wire, wlroots handles the
    // whole protocol internally. xdg-output lets tools like slurp query
    // real output geometry; screencopy is what grim actually captures via.
    wlr_xdg_output_manager_v1_create(comp->wl_display, comp->output_layout);
    wlr_screencopy_manager_v1_create(comp->wl_display);

    comp->scene = wlr_scene_create();
    comp->scene_layout = wlr_scene_attach_output_layout(comp->scene, comp->output_layout);

    comp->layer_bg      = wlr_scene_tree_create(&comp->scene->tree);
    comp->layer_bottom  = wlr_scene_tree_create(&comp->scene->tree);
    comp->layer_normal  = wlr_scene_tree_create(&comp->scene->tree);
    comp->layer_top     = wlr_scene_tree_create(&comp->scene->tree);
    comp->layer_overlay = wlr_scene_tree_create(&comp->scene->tree);

    comp->xdg_shell = wlr_xdg_shell_create(comp->wl_display, 3);
    wl_list_init(&comp->toplevels);
    comp->new_xdg_toplevel.notify = compositor_new_xdg_toplevel;
    wl_signal_add(&comp->xdg_shell->events.new_toplevel, &comp->new_xdg_toplevel);
    comp->new_xdg_popup.notify = compositor_new_xdg_popup;
    wl_signal_add(&comp->xdg_shell->events.new_popup, &comp->new_xdg_popup);

    comp->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(comp->wl_display);
    comp->new_toplevel_decoration.notify = new_toplevel_decoration;
    wl_signal_add(&comp->xdg_decoration_manager->events.new_toplevel_decoration,
        &comp->new_toplevel_decoration);

    comp->layer_shell = wlr_layer_shell_v1_create(comp->wl_display, 4);
    comp->new_layer_surface.notify = compositor_new_layer_surface;
    wl_signal_add(&comp->layer_shell->events.new_surface, &comp->new_layer_surface);

    comp->toplevel_manager = wlr_foreign_toplevel_manager_v1_create(comp->wl_display);

    // lazy=true: the X server only actually starts the first time an X11
    // client tries to connect, rather than eating startup time/resources
    // for sessions that never run one.
    // FIX 2
    wl_list_init(&comp->xwayland_windows);
    comp->xwayland = wlr_xwayland_create(comp->wl_display, comp->wlr_compositor, false);
    if (comp->xwayland) {
        comp->xwayland_ready.notify = xwayland_ready;
        wl_signal_add(&comp->xwayland->events.ready, &comp->xwayland_ready);
        comp->new_xwayland_surface.notify = compositor_new_xwayland_surface;
        wl_signal_add(&comp->xwayland->events.new_surface, &comp->new_xwayland_surface);
    } else {
        fprintf(stderr, "WARN: Xwayland unavailable — X11-only apps won't run. "
                         "Is 'xwayland' installed?\n");
    }

    comp->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(comp->cursor, comp->output_layout);
    comp->cursor_mode = CURSOR_PASSTHROUGH;
    comp->grabbed_window = NULL;

    comp->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    comp->cursor_motion.notify = cursor_motion;
    wl_signal_add(&comp->cursor->events.motion, &comp->cursor_motion);
    comp->cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&comp->cursor->events.motion_absolute, &comp->cursor_motion_absolute);
    comp->cursor_button.notify = cursor_button;
    wl_signal_add(&comp->cursor->events.button, &comp->cursor_button);
    comp->cursor_axis.notify = cursor_axis;
    wl_signal_add(&comp->cursor->events.axis, &comp->cursor_axis);
    comp->cursor_frame.notify = cursor_frame;
    wl_signal_add(&comp->cursor->events.frame, &comp->cursor_frame);

    wl_list_init(&comp->keyboards);
    comp->new_input.notify = compositor_new_input;
    wl_signal_add(&comp->backend->events.new_input, &comp->new_input);

    comp->seat = wlr_seat_create(comp->wl_display, "seat0");
    comp->request_cursor.notify = seat_request_cursor;
    wl_signal_add(&comp->seat->events.request_set_cursor, &comp->request_cursor);
    comp->request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&comp->seat->events.request_set_selection, &comp->request_set_selection);
    comp->request_start_drag.notify = seat_request_start_drag;
    wl_signal_add(&comp->seat->events.request_start_drag, &comp->request_start_drag);

    comp->pointer_constraints = wlr_pointer_constraints_v1_create(comp->wl_display);
    comp->new_pointer_constraint.notify = compositor_new_pointer_constraint;
    wl_signal_add(&comp->pointer_constraints->events.new_constraint, &comp->new_pointer_constraint);
    comp->relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(comp->wl_display);

    printf("--- Compositor core initialized ---\n");
    return 1;
}

// Launches every "exec=..." line from the config as a detached child process,
// each run through `sh -c` so pipes/args/env work the way people expect from
// a shell config line. SIGCHLD is ignored so children are auto-reaped
// without us needing to wait() on them.
static void run_autostart(Compositor* comp) {
//FIX 3
    struct sigaction sa = {0};
sa.sa_handler = sigchld_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;
sigaction(SIGCHLD, &sa, NULL);

    for (int i = 0; i < comp->config.exec_count; i++) {
        const char* cmd = comp->config.exec_cmds[i];
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[Config] fork() failed for autostart '%s'\n", cmd);
            continue;
        }
        if (pid == 0) {
            // Child: detach into its own session so it isn't tied to our
            // process group/terminal, then hand off to the shell.
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
            fprintf(stderr, "[Config] failed to exec autostart '%s'\n", cmd);
            _exit(127);
        }
    }
    if (comp->config.exec_count > 0) {
        printf("[Config] Launched %d autostart command(s).\n", comp->config.exec_count);
    }
}

void compositor_run(Compositor* comp) {
    if (!wlr_backend_start(comp->backend)) { fprintf(stderr, "FATAL: wlr_backend_start failed\n"); return; }

    printf("[Keybind] Super+Escape quits. Alt+LeftClick drag = move. Alt+RightClick drag = resize.\n");

    run_autostart(comp);

    wl_display_run(comp->wl_display);
}

void compositor_shutdown(Compositor* comp) {
    config_free(&comp->config);
    //FIX 6
     if (comp->xwayland) {
        wl_list_remove(&comp->xwayland_ready.link);
    wl_list_remove(&comp->new_xwayland_surface.link);
    wlr_xwayland_destroy(comp->xwayland);
    }
    wl_display_destroy_clients(comp->wl_display);
    wlr_xcursor_manager_destroy(comp->cursor_mgr);
    wlr_cursor_destroy(comp->cursor);
    wlr_allocator_destroy(comp->allocator);
    wlr_renderer_destroy(comp->renderer);
    wlr_backend_destroy(comp->backend);
    wl_display_destroy(comp->wl_display);
    printf("--- Compositor shut down ---\n");
}

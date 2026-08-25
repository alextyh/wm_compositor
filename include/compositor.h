#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/xwayland.h>
#include "config.h"
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>

typedef enum {
    CURSOR_PASSTHROUGH,
    CURSOR_MOVE,
    CURSOR_RESIZE,
} CursorMode;

// Forward declaration so Compositor can hold a pointer to it.
typedef struct ManagedWindow ManagedWindow;

// Lets window_at()/focus/move/resize treat native Wayland windows and
// Xwayland (X11) windows uniformly. Must be the FIRST member of both
// ManagedWindow and XWindow so a scene node's opaque node.data can be
// safely inspected as "what kind of window is this" before being cast
// to the real type — this is well-defined in C (a pointer to a struct,
// suitably converted, points to its first member).
typedef enum {
    WINDOW_KIND_TOPLEVEL,
    WINDOW_KIND_XWAYLAND,
} WindowKind;

typedef struct {
    struct wl_display*        wl_display;
    struct wlr_backend*       backend;
    struct wlr_renderer*      renderer;
    struct wlr_allocator*     allocator;
    struct wlr_compositor*    wlr_compositor; // kept: xwayland needs it at creation time
    struct wlr_output_layout* output_layout;
    struct wl_list             outputs;
    struct wl_listener         new_output;

    struct wlr_scene*              scene;
    struct wlr_scene_output_layout* scene_layout;

    // Z-order sublayers, in paint order (bg painted first/bottom-most).
    // Normal app windows + popups live in 'layer_normal'; layer-shell
    // clients (bars, wallpaper, notification popups) go in the other four
    // per their requested layer.
    struct wlr_scene_tree* layer_bg;
    struct wlr_scene_tree* layer_bottom;
    struct wlr_scene_tree* layer_normal;
    struct wlr_scene_tree* layer_top;
    struct wlr_scene_tree* layer_overlay;

    struct wlr_layer_shell_v1* layer_shell;
    struct wl_listener            new_layer_surface;

    struct wlr_foreign_toplevel_manager_v1* toplevel_manager;

    // The output new windows/notifications default onto. Resolved from
    // config's primary_monitor name once outputs connect; falls back to
    // whichever output connected first if unset/not found.
    struct wlr_output* primary_output;

    struct wlr_xdg_shell* xdg_shell;
    struct wl_listener      new_xdg_toplevel;
    struct wl_listener      new_xdg_popup;
    struct wl_list           toplevels;

    // xdg-decoration: tells well-behaved clients "let the compositor draw
    // decorations" so they don't draw their own titlebars.
    struct wlr_xdg_decoration_manager_v1* xdg_decoration_manager;
    struct wl_listener                      new_toplevel_decoration;

    // Xwayland: lets legacy X11-only apps (older GTK3 apps, etc.) run.
    struct wlr_xwayland* xwayland;
    struct wl_listener     xwayland_ready;
    struct wl_listener     new_xwayland_surface;
    struct wl_list           xwayland_windows;

    struct wlr_cursor*          cursor;
    struct wlr_xcursor_manager* cursor_mgr;
    struct wl_listener           cursor_motion;
    struct wl_listener           cursor_motion_absolute;
    struct wl_listener           cursor_button;
    struct wl_listener           cursor_axis;
    struct wl_listener           cursor_frame;

    // Interactive move/resize state (Alt+M1 move, Alt+M2 resize)
    CursorMode        cursor_mode;
    void*              grabbed_window; // ManagedWindow* or XWindow*, per grabbed_kind
    WindowKind         grabbed_kind;
    double             grab_x, grab_y;
    struct wlr_box     grab_geobox;
    uint32_t            resize_edges;

    struct wlr_seat*    seat;
    struct wl_listener   new_input;
    struct wl_listener   request_cursor;
    struct wl_listener   request_set_selection;
    struct wl_listener   request_start_drag;
    struct wlr_scene_tree* drag_icon_tree; // NULL when no drag is active
    struct wl_listener        drag_icon_destroy_listener;

    // Screen-space origin of whatever surface currently holds an implicit
    // pointer grab (a button is held over it). Lets us keep recomputing its
    // correct surface-local coordinates without re-resolving window_at()
    // mid-click (see process_cursor_motion).
    double grab_surface_ox, grab_surface_oy;
    struct wl_list        keyboards;

    int current_workspace; // 0-9 (shown to the user as 1-10)

    // Pointer lock/confine (needed for FPS-style games, e.g. Roblox/Sober)
    struct wlr_pointer_constraints_v1*      pointer_constraints;
    struct wl_listener                        new_pointer_constraint;
    struct wlr_relative_pointer_manager_v1* relative_pointer_mgr;
    struct wlr_pointer_constraint_v1*       active_constraint;

    // Border colors and monitor/autostart settings, loaded from
    // ~/.config/.wmcompconfig (falls back to sane defaults if absent).
    WMConfig config;
} Compositor;

typedef struct {
    struct wl_list       link;
    Compositor*           comp;
    struct wlr_output*    wlr_output;
    struct wl_listener    frame;
    struct wl_listener    request_state;
    struct wl_listener    destroy;
} Output;

typedef struct {
    struct wl_list         link;
    Compositor*             comp;
    struct wlr_keyboard*    wlr_keyboard;
    struct wl_listener      modifiers;
    struct wl_listener      key;
    struct wl_listener      destroy;
} Keyboard;

// A managed native-Wayland client window
struct ManagedWindow {
    WindowKind                  kind; // must stay first — see WindowKind comment above
    struct wl_list             link;
    Compositor*                  comp;
    struct wlr_xdg_toplevel*    xdg_toplevel;
    struct wlr_scene_tree*      scene_tree;

    // Focus-color border, drawn as 4 thin rects around the surface.
    struct wlr_scene_rect*      border_top;
    struct wlr_scene_rect*      border_bottom;
    struct wlr_scene_rect*      border_left;
    struct wlr_scene_rect*      border_right;

    struct wl_listener           map;
    struct wl_listener           unmap;
    struct wl_listener           commit;
    struct wl_listener           destroy;
    struct wl_listener           request_move;
    struct wl_listener           request_resize;
    struct wl_listener           request_fullscreen;

    bool             is_fullscreen;
    struct wlr_box  saved_box; // position+size to restore on un-fullscreen

    int workspace; // 0-9, which of the 10 workspaces this window lives on
    bool omnipresent; // shows on every workspace, ignoring 'workspace' above

    // Lets external tools (screenshare picker, taskbars) see this window.
    struct wlr_foreign_toplevel_handle_v1* toplevel_handle;
    struct wl_listener                        toplevel_request_maximize;
    struct wl_listener                        toplevel_request_minimize;
    struct wl_listener                        toplevel_request_activate;
    struct wl_listener                        toplevel_request_fullscreen;
    struct wl_listener                        toplevel_request_close;
};

// An xdg_popup (context menus, dropdowns, tooltips) — separate object type
// from ManagedWindow toplevels, but still needs a scene node + configure ack.
typedef struct {
    struct wlr_xdg_popup* xdg_popup;
    struct wl_listener      commit;
    struct wl_listener      destroy;
} Popup;

// A managed Xwayland (X11) client window — separate from ManagedWindow
// since X11 surfaces have a different lifecycle: the xsurface object exists
// before there's any wl_surface behind it (associate/dissociate track that),
// independently of map/unmap.
typedef struct {
    WindowKind                       kind; // must stay first — see WindowKind comment above
    struct wl_list                 link;
    Compositor*                      comp;
    struct wlr_xwayland_surface*    xsurface;
    struct wlr_scene_tree*          scene_tree;

    // Focus-color border, same visual treatment as native windows.
    struct wlr_scene_rect*          border_top;
    struct wlr_scene_rect*          border_bottom;
    struct wlr_scene_rect*          border_left;
    struct wlr_scene_rect*          border_right;

    struct wl_listener               associate;
    struct wl_listener               dissociate;
    struct wl_listener               map;
    struct wl_listener               unmap;
    struct wl_listener               commit;
    struct wl_listener               destroy;
    struct wl_listener               request_configure;
    struct wl_listener               request_move;
    struct wl_listener               request_resize;
    struct wl_listener               request_fullscreen;

    bool             is_fullscreen;
    struct wlr_box  saved_box;

    int workspace; // 0-9, which of the 10 workspaces this window lives on
    bool omnipresent;

    // Lets external tools (screenshare picker, taskbars) see this window —
    // same mechanism ManagedWindow uses.
    struct wlr_foreign_toplevel_handle_v1* toplevel_handle;
    struct wl_listener                        toplevel_request_maximize;
    struct wl_listener                        toplevel_request_minimize;
    struct wl_listener                        toplevel_request_activate;
    struct wl_listener                        toplevel_request_fullscreen;
    struct wl_listener                        toplevel_request_close;
} XWindow;

int  compositor_init(Compositor* comp);
void compositor_run(Compositor* comp);
void compositor_shutdown(Compositor* comp);

#endif

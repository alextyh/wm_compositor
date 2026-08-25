# wm_compositor

A wayland compositor loosely based on tinywl, using the wlroots library
Has features like: Configuration file to define shortcut binds, and binds related to the compositor's window behaviour (close, toggling workspaces (up to 10), etc, see config below)

(also a pretty big warning, im not a coder, and not a coder in C at that, so this is some genuine ai slop, im only putting this on github so i can use this on other systems better since it works for me, and because of that, some of the comments are not proper, i MAY or may not document this myself properly in the future)

# Building
dependencies: wayland-protocols, wayland-scanner, wlroots, wayland-server, xcb and xkbcommon, and other common building utilities
invoke "make" using the Makefile and run the executable made (make clean cleans up the working directory)

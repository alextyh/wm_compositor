PKG_CONFIG := pkg-config
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CFLAGS += -O2 -flto -march=native -std=c11 -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
          $(shell $(PKG_CONFIG) --cflags wlroots-0.19 wayland-server xkbcommon xcb) \
          -I./include
LIBS := $(shell $(PKG_CONFIG) --libs wlroots-0.19 wayland-server xkbcommon xcb)

all: include/xdg-shell-protocol.h include/pointer-constraints-unstable-v1-protocol.h include/wlr-layer-shell-unstable-v1-protocol.h wm_compositor

include/wlr-layer-shell-unstable-v1-protocol.h: protocol/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) server-header protocol/wlr-layer-shell-unstable-v1.xml $@

include/pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		/usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@

include/xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

src/main.o: src/main.c include/xdg-shell-protocol.h
	$(CC) $(CFLAGS) -c src/main.c -o src/main.o

src/config.o: src/config.c
	$(CC) $(CFLAGS) -c src/config.c -o src/config.o

src/compositor.o: src/compositor.c include/xdg-shell-protocol.h include/config.h include/pointer-constraints-unstable-v1-protocol.h
	$(CC) $(CFLAGS) -c src/compositor.c -o src/compositor.o

wm_compositor: src/main.o src/compositor.o src/config.o
	$(CC) src/main.o src/compositor.o src/config.o $(LIBS) -o wm_compositor

clean:
	rm -f wm_compositor src/*.o include/xdg-shell-protocol.h include/pointer-constraints-unstable-v1-protocol.h include/wlr-layer-shell-unstable-v1-protocol.h

.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
DWLCPPFLAGS = -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND) $(CPPFLAGS)
DWLDEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput json-c $(XLIBS)
DWLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(DWLCPPFLAGS) $(DWLDEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm $(LIBS)

all: dwl
dwl: dwl.c client.h config.h dim.h floating.h focus.h ime.h ipc.h layout.h settings.h util.h workspace.h config.mk cursor-shape-v1-protocol.h \
	ext-image-copy-capture-v1-protocol.h \
	pointer-constraints-unstable-v1-protocol.h wlr-layer-shell-unstable-v1-protocol.h \
	wlr-output-power-management-unstable-v1-protocol.h xdg-shell-protocol.h
	$(CC) dwl.c $(DWLCFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
ext-image-copy-capture-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml $@
pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

config.h:
	cp config.def.h $@

# The tests run dwl on wlroots' headless backend, with tests/client for
# windows, and check what it reports over the IPC.
tests/xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
tests/xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
tests/xdg-activation-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/xdg-activation/xdg-activation-v1.xml $@
tests/xdg-activation-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/xdg-activation/xdg-activation-v1.xml $@
tests/xdg-dialog-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/xdg-dialog/xdg-dialog-v1.xml $@
tests/xdg-dialog-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/xdg-dialog/xdg-dialog-v1.xml $@
tests/content-type-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/content-type/content-type-v1.xml $@
tests/content-type-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/content-type/content-type-v1.xml $@
tests/client: tests/client.c tests/xdg-shell-client-protocol.h tests/xdg-shell-protocol.c \
		tests/xdg-activation-v1-client-protocol.h tests/xdg-activation-v1-protocol.c \
		tests/xdg-dialog-v1-client-protocol.h tests/xdg-dialog-v1-protocol.c \
		tests/content-type-v1-client-protocol.h tests/content-type-v1-protocol.c
	$(CC) tests/client.c tests/xdg-shell-protocol.c tests/xdg-activation-v1-protocol.c \
		tests/xdg-dialog-v1-protocol.c tests/content-type-v1-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
# tests/x11client needs XWayland's xcb libraries. The substitution makes it
# a prerequisite only when XWAYLAND is set, as it is for CI's tests.
tests/x11client: tests/x11client.c
	$(CC) tests/x11client.c `$(PKG_CONFIG) --cflags xcb xcb-icccm` \
		$(DWLDEVCFLAGS) $(CFLAGS) $(LDFLAGS) \
		`$(PKG_CONFIG) --libs xcb xcb-icccm` -o $@
tests/ext-workspace-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-workspace/ext-workspace-v1.xml $@
tests/ext-workspace-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/ext-workspace/ext-workspace-v1.xml $@
tests/workspaces: tests/workspaces.c tests/ext-workspace-v1-client-protocol.h tests/ext-workspace-v1-protocol.c
	$(CC) tests/workspaces.c tests/ext-workspace-v1-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
tests/ext-session-lock-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/staging/ext-session-lock/ext-session-lock-v1.xml $@
tests/ext-session-lock-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/staging/ext-session-lock/ext-session-lock-v1.xml $@
tests/locker: tests/locker.c tests/ext-session-lock-v1-client-protocol.h tests/ext-session-lock-v1-protocol.c
	$(CC) tests/locker.c tests/ext-session-lock-v1-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
tests/wlr-output-power-management-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
tests/wlr-output-power-management-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/wlr-output-power-management-unstable-v1.xml $@
tests/power: tests/power.c tests/wlr-output-power-management-unstable-v1-client-protocol.h \
		tests/wlr-output-power-management-unstable-v1-protocol.c
	$(CC) tests/power.c tests/wlr-output-power-management-unstable-v1-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
tests/input-method-unstable-v2-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/input-method-unstable-v2.xml $@
tests/input-method-unstable-v2-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/input-method-unstable-v2.xml $@
tests/ime: tests/ime.c tests/input-method-unstable-v2-client-protocol.h \
		tests/input-method-unstable-v2-protocol.c
	$(CC) tests/ime.c tests/input-method-unstable-v2-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
tests/wlr-virtual-pointer-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/wlr-virtual-pointer-unstable-v1.xml $@
tests/wlr-virtual-pointer-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/wlr-virtual-pointer-unstable-v1.xml $@
tests/pointer: tests/pointer.c tests/wlr-virtual-pointer-unstable-v1-client-protocol.h \
		tests/wlr-virtual-pointer-unstable-v1-protocol.c
	$(CC) tests/pointer.c tests/wlr-virtual-pointer-unstable-v1-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
tests/virtual-keyboard-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/virtual-keyboard-unstable-v1.xml $@
tests/virtual-keyboard-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/virtual-keyboard-unstable-v1.xml $@
tests/keyboard: tests/keyboard.c tests/virtual-keyboard-unstable-v1-client-protocol.h \
		tests/virtual-keyboard-unstable-v1-protocol.c
	$(CC) tests/keyboard.c tests/virtual-keyboard-unstable-v1-protocol.c -Itests \
		`$(PKG_CONFIG) --cflags wayland-client xkbcommon` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client xkbcommon` -o $@
tests/wlr-layer-shell-unstable-v1-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
tests/wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/wlr-layer-shell-unstable-v1.xml $@
tests/launcher: tests/launcher.c tests/wlr-layer-shell-unstable-v1-client-protocol.h \
		tests/wlr-layer-shell-unstable-v1-protocol.c tests/xdg-shell-client-protocol.h \
		tests/xdg-shell-protocol.c
	$(CC) tests/launcher.c tests/wlr-layer-shell-unstable-v1-protocol.c tests/xdg-shell-protocol.c \
		-Itests `$(PKG_CONFIG) --cflags wayland-client` $(DWLDEVCFLAGS) $(CFLAGS) \
		$(LDFLAGS) `$(PKG_CONFIG) --libs wayland-client` -o $@
test: dwl tests/client tests/workspaces tests/locker tests/power tests/ime tests/pointer tests/launcher \
		tests/keyboard \
		$(XWAYLAND:-DXWAYLAND=tests/x11client)
	python3 -m unittest discover -s tests -v

clean:
	rm -f dwl *.o *-protocol.h tests/client tests/workspaces tests/locker tests/power \
		tests/ime tests/pointer tests/launcher tests/keyboard tests/x11client tests/*-protocol.h \
		tests/*-protocol.c

dist: clean
	mkdir -p dwl-$(VERSION)
	cp -R LICENSE NOTICE Makefile CHANGELOG.md README.md client.h config.def.h \
		config.mk protocols dwl.1 dwl.c dim.h floating.h focus.h ime.h ipc.h layout.h settings.h util.h workspace.h \
		dwl.desktop docs tests \
		dwl-$(VERSION)
	tar -caf dwl-$(VERSION).tar.gz dwl-$(VERSION)
	rm -rf dwl-$(VERSION)

install: dwl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/dwl
	cp -f dwl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dwl
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f dwl.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/dwl.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f dwl.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/dwl.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/dwl.desktop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dwl $(DESTDIR)$(MANDIR)/man1/dwl.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/dwl.desktop

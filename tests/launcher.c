/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' launcher: an overlay layer surface that takes the keyboard,
 * as tide's launcher does, over wlr-layer-shell. It prints "open" once the
 * compositor has it, then a line "close" on stdin closes it, and it
 * prints "closed" once the compositor has had that, and exits.
 *
 * With "bar", it's a bar instead: a top layer surface along the top, 30 px
 * high, that keeps windows out of its space and takes no keys.
 *
 * With "notification", it's a notification with a reply field: a top
 * layer surface in the top right corner, 200x100, that takes the keyboard
 * on demand, when it's clicked. It prints "focused" when it gets the
 * keyboard, "unfocused" when it loses it, and "key" for each key pressed
 * in it.
 *
 * With "center", it's a notification center: the notification, which also
 * grabs focus for itself over hyprland-focus-grab-v1, as Quickshell's
 * HyprlandFocusGrab does. It prints "grabbing" once the compositor has the
 * grab, and "cleared" when the compositor ends it. A line "ping" on stdin
 * has it print "pong" once the compositor has answered, so anything it
 * said before then has been printed.
 *
 * With "menu", it's the center, but its grab names only a menu it opens
 * first, an xdg popup of 50x50 in its top left corner, as a center's
 * context menu might. With "early-menu", it's the menu, but its grab names
 * the menu before the menu maps. A line "close menu" on stdin closes the
 * menu, leaving the center and its grab, and it prints "closed menu" once
 * the compositor has had that.
 *
 * With "pair", it's the center with a second notification under it,
 * which its grab names too. A line "close first" on stdin closes the
 * first, or the center's only one, leaving its grab, and it prints
 * "closed first" once the compositor has had that.
 *
 * A line "ungrab" on stdin ends its grab, leaving it open, and it prints
 * "ungrabbed" once the compositor has had that.
 *
 * With "grab", it's the launcher, which also grabs focus for itself, as
 * the center does, but says nothing of its keyboard. With "early", it's
 * the center, but grabbing before it maps, and saying only "open".
 *
 * Usage: launcher [bar|notification|center|menu|early-menu|pair|grab|early]
 */
#define _GNU_SOURCE /* memfd_create */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "hyprland-focus-grab-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct hyprland_focus_grab_manager_v1 *grabs;
static struct wl_keyboard *keyboard;
static struct wl_surface *surface;
static int configured;
static struct wl_surface *second;
static int second_configured;
static struct xdg_wm_base *wm_base;
static struct wl_surface *menu_surface;
static int menu_configured;

static void
die(const char *msg)
{
	fprintf(stderr, "launcher: %s\n", msg);
	exit(1);
}

static void
draw(struct wl_surface *s, uint32_t width, uint32_t height)
{
	int32_t stride = (int32_t)width * 4, size = stride * (int32_t)height;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int fd = memfd_create("launcher", MFD_CLOEXEC), i;
	if (fd < 0 || ftruncate(fd, size) < 0)
		die("can't make a buffer");
	if ((pixels = mmap(NULL, (size_t)size, PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
		die("can't map the buffer");
	for (i = 0; i < size / 4; i++)
		pixels[i] = 0xff996633;
	munmap(pixels, (size_t)size);
	pool = wl_shm_create_pool(shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(s, buffer, 0, 0);
	wl_surface_damage(s, 0, 0, (int32_t)width, (int32_t)height);
	wl_surface_commit(s);
}

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
		uint32_t width, uint32_t height)
{
	/* data is the second notification's flag, or NULL for the first */
	int *done = data ? data : &configured;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (!*done)
		draw(data ? second : surface, width ? width : 200, height ? height : 100);
	*done = 1;
}

static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	die("the compositor closed the launcher");
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void
say(const char *what)
{
	printf("%s\n", what);
	fflush(stdout);
}

static void
keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
	close(fd);
}

static void
keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		struct wl_array *keys)
{
	say("focused");
}

static void
keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	say("unfocused");
}

static void
keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
		uint32_t state)
{
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		say("key");
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
		uint32_t latched, uint32_t locked, uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void
wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void
menu_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	if (!menu_configured)
		draw(menu_surface, 50, 50);
	menu_configured = 1;
}

static const struct xdg_surface_listener menu_listener = {
	.configure = menu_configure,
};

static void
popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t width,
		int32_t height)
{
}

static void
popup_done(void *data, struct xdg_popup *popup)
{
	die("the compositor closed the menu");
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

static void
grab_cleared(void *data, struct hyprland_focus_grab_v1 *grab)
{
	say("cleared");
}

static const struct hyprland_focus_grab_v1_listener grab_listener = {
	.cleared = grab_cleared,
};

/* Grabs focus for s, and also, if it's there, as Quickshell's
 * HyprlandFocusGrab does */
static struct hyprland_focus_grab_v1 *
start_grab(struct wl_surface *s, struct wl_surface *also)
{
	struct hyprland_focus_grab_v1 *grab = hyprland_focus_grab_manager_v1_create_grab(grabs);
	hyprland_focus_grab_v1_add_listener(grab, &grab_listener, NULL);
	hyprland_focus_grab_v1_add_surface(grab, s);
	if (also)
		hyprland_focus_grab_v1_add_surface(grab, also);
	hyprland_focus_grab_v1_commit(grab);
	return grab;
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		/* On-demand keyboard focus is version 4's */
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
				version < 4 ? version : 4);
	else if (!strcmp(interface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (!strcmp(interface, hyprland_focus_grab_manager_v1_interface.name))
		grabs = wl_registry_bind(registry, name, &hyprland_focus_grab_manager_v1_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct zwlr_layer_surface_v1 *layer, *second_layer = NULL;
	struct hyprland_focus_grab_v1 *grab = NULL;
	struct xdg_surface *menu_xdg = NULL;
	struct xdg_positioner *positioner;
	struct xdg_popup *popup = NULL;
	char line[64];

	int bar = argc == 2 && !strcmp(argv[1], "bar");
	int early_menu = argc == 2 && !strcmp(argv[1], "early-menu");
	int menu = early_menu || (argc == 2 && !strcmp(argv[1], "menu"));
	int pair = argc == 2 && !strcmp(argv[1], "pair");
	int self_grab = argc == 2 && !strcmp(argv[1], "grab");
	int early = argc == 2 && !strcmp(argv[1], "early");
	int center = menu || pair || early || (argc == 2 && !strcmp(argv[1], "center"));
	int notification = center || (argc == 2 && !strcmp(argv[1], "notification"));

	if (argc != 1 && !bar && !notification && !self_grab)
		die("usage: launcher [bar|notification|center|menu|early-menu|pair|grab|early]");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!compositor || !shm || !layer_shell)
		die("the compositor lacks wl_compositor, wl_shm or zwlr_layer_shell_v1");
	if (notification) {
		if (!seat || zwlr_layer_shell_v1_get_version(layer_shell) < 4)
			die("the compositor lacks a seat, or on-demand keyboard focus");
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
	if ((center || self_grab) && !grabs)
		die("the compositor lacks hyprland_focus_grab_manager_v1");
	if (menu && !wm_base)
		die("the compositor lacks xdg_wm_base");
	surface = wl_compositor_create_surface(compositor);
	layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
			bar || notification ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
			: ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			bar ? "bar" : center ? "center" : notification ? "notification" : "launcher");
	zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
	if (bar) {
		zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
				| ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(layer, 0, 30);
		zwlr_layer_surface_v1_set_exclusive_zone(layer, 30);
	} else if (notification) {
		zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
				| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_size(layer, 200, 100);
		zwlr_layer_surface_v1_set_keyboard_interactivity(layer,
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
	} else {
		zwlr_layer_surface_v1_set_size(layer, 200, 100);
		zwlr_layer_surface_v1_set_keyboard_interactivity(layer, 1); /* exclusive */
	}
	wl_surface_commit(surface);
	if (early)
		grab = start_grab(surface, NULL);
	/* The configure, then the buffer it draws, which maps it */
	while (!configured)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	say("open");
	if (menu) {
		menu_surface = wl_compositor_create_surface(compositor);
		menu_xdg = xdg_wm_base_get_xdg_surface(wm_base, menu_surface);
		xdg_surface_add_listener(menu_xdg, &menu_listener, NULL);
		positioner = xdg_wm_base_create_positioner(wm_base);
		xdg_positioner_set_size(positioner, 50, 50);
		xdg_positioner_set_anchor_rect(positioner, 0, 0, 1, 1);
		xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
		xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
		popup = xdg_surface_get_popup(menu_xdg, NULL, positioner);
		xdg_popup_add_listener(popup, &popup_listener, NULL);
		zwlr_layer_surface_v1_get_popup(layer, popup);
		xdg_positioner_destroy(positioner);
		wl_surface_commit(menu_surface);
		if (early_menu)
			grab = start_grab(menu_surface, NULL);
		/* The configure, then the buffer it draws, which maps it */
		while (!menu_configured)
			if (wl_display_dispatch(display) < 0)
				die("lost the compositor");
		if (wl_display_roundtrip(display) < 0)
			die("lost the compositor");
	}
	if (pair) {
		second = wl_compositor_create_surface(compositor);
		second_layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, second, NULL,
				ZWLR_LAYER_SHELL_V1_LAYER_TOP, "center");
		zwlr_layer_surface_v1_add_listener(second_layer, &layer_listener, &second_configured);
		zwlr_layer_surface_v1_set_anchor(second_layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
				| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_margin(second_layer, 110, 0, 0, 0);
		zwlr_layer_surface_v1_set_size(second_layer, 200, 100);
		zwlr_layer_surface_v1_set_keyboard_interactivity(second_layer,
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
		wl_surface_commit(second);
		while (!second_configured)
			if (wl_display_dispatch(display) < 0)
				die("lost the compositor");
		if (wl_display_roundtrip(display) < 0)
			die("lost the compositor");
	}
	if ((center || self_grab) && !early) {
		if (!grab)
			grab = start_grab(menu ? menu_surface : surface, second);
		if (wl_display_roundtrip(display) < 0)
			die("lost the compositor");
		say("grabbing");
	}
	/* Until "close", hearing the compositor too, for the keyboard */
	for (;;) {
		struct pollfd fds[2] = {
			{.fd = wl_display_get_fd(display), .events = POLLIN},
			{.fd = STDIN_FILENO, .events = POLLIN},
		};
		if (wl_display_flush(display) < 0)
			die("lost the compositor");
		if (poll(fds, 2, -1) < 0)
			die("poll failed");
		if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0)
			die("lost the compositor");
		if (!(fds[1].revents & (POLLIN | POLLHUP)))
			continue;
		if (!fgets(line, sizeof(line), stdin) || !strcmp(line, "close\n"))
			break;
		if (!strcmp(line, "ping\n")) {
			if (wl_display_roundtrip(display) < 0)
				die("lost the compositor");
			say("pong");
		} else if (!strcmp(line, "close menu\n") && menu_surface) {
			xdg_popup_destroy(popup);
			xdg_surface_destroy(menu_xdg);
			wl_surface_destroy(menu_surface);
			menu_surface = NULL;
			if (wl_display_roundtrip(display) < 0)
				die("lost the compositor");
			say("closed menu");
		} else if (!strcmp(line, "close first\n") && surface) {
			zwlr_layer_surface_v1_destroy(layer);
			wl_surface_destroy(surface);
			surface = NULL;
			if (wl_display_roundtrip(display) < 0)
				die("lost the compositor");
			say("closed first");
		} else if (!strcmp(line, "ungrab\n") && grab) {
			hyprland_focus_grab_v1_destroy(grab);
			grab = NULL;
			if (wl_display_roundtrip(display) < 0)
				die("lost the compositor");
			say("ungrabbed");
		}
	}
	if (grab)
		hyprland_focus_grab_v1_destroy(grab);
	if (surface) {
		zwlr_layer_surface_v1_destroy(layer);
		wl_surface_destroy(surface);
	}
	if (second) {
		zwlr_layer_surface_v1_destroy(second_layer);
		wl_surface_destroy(second);
	}
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	say("closed");
	return 0;
}

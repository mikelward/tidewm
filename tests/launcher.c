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
 * Usage: launcher [bar|notification]
 */
#define _GNU_SOURCE /* memfd_create */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_surface *surface;
static int configured;

static void
die(const char *msg)
{
	fprintf(stderr, "launcher: %s\n", msg);
	exit(1);
}

static void
draw(uint32_t width, uint32_t height)
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
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, (int32_t)width, (int32_t)height);
	wl_surface_commit(surface);
}

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
		uint32_t width, uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (!configured)
		draw(width ? width : 200, height ? height : 100);
	configured = 1;
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
	struct zwlr_layer_surface_v1 *layer;
	char line[64];

	int bar = argc == 2 && !strcmp(argv[1], "bar");
	int notification = argc == 2 && !strcmp(argv[1], "notification");

	if (argc != 1 && !bar && !notification)
		die("usage: launcher [bar|notification]");
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
	surface = wl_compositor_create_surface(compositor);
	layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
			bar || notification ? ZWLR_LAYER_SHELL_V1_LAYER_TOP
			: ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			bar ? "bar" : notification ? "notification" : "launcher");
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
	/* The configure, then the buffer it draws, which maps it */
	while (!configured)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	say("open");
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
	}
	zwlr_layer_surface_v1_destroy(layer);
	wl_surface_destroy(surface);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	say("closed");
	return 0;
}

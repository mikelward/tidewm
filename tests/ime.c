/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' input method, over input-method-v2: it grabs the keyboard,
 * prints "grabbed" once the compositor has the grab, then "key KEYCODE
 * STATE" for each key the grab gets, STATE 1 for a press, 0 for a release.
 *
 * Usage: ime
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"

static struct zwp_input_method_manager_v2 *manager;
static struct wl_seat *seat;

static void
die(const char *msg)
{
	fprintf(stderr, "ime: %s\n", msg);
	exit(1);
}

static void
grab_keymap(void *data, struct zwp_input_method_keyboard_grab_v2 *grab,
		uint32_t format, int32_t fd, uint32_t size)
{
	close(fd);
}

static void
grab_key(void *data, struct zwp_input_method_keyboard_grab_v2 *grab,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	printf("key %u %u\n", key, state);
	fflush(stdout);
}

static void grab_modifiers(void *data, struct zwp_input_method_keyboard_grab_v2 *grab,
		uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
		uint32_t group) {}
static void grab_repeat_info(void *data, struct zwp_input_method_keyboard_grab_v2 *grab,
		int32_t rate, int32_t delay) {}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	.keymap = grab_keymap,
	.key = grab_key,
	.modifiers = grab_modifiers,
	.repeat_info = grab_repeat_info,
};

static void im_activate(void *data, struct zwp_input_method_v2 *im) {}
static void im_deactivate(void *data, struct zwp_input_method_v2 *im) {}
static void im_surrounding_text(void *data, struct zwp_input_method_v2 *im,
		const char *text, uint32_t cursor, uint32_t anchor) {}
static void im_text_change_cause(void *data, struct zwp_input_method_v2 *im, uint32_t cause) {}
static void im_content_type(void *data, struct zwp_input_method_v2 *im,
		uint32_t hint, uint32_t purpose) {}
static void im_done(void *data, struct zwp_input_method_v2 *im) {}

static void
im_unavailable(void *data, struct zwp_input_method_v2 *im)
{
	die("another input method has the seat");
}

static const struct zwp_input_method_v2_listener im_listener = {
	.activate = im_activate,
	.deactivate = im_deactivate,
	.surrounding_text = im_surrounding_text,
	.text_change_cause = im_text_change_cause,
	.content_type = im_content_type,
	.done = im_done,
	.unavailable = im_unavailable,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, zwp_input_method_manager_v2_interface.name))
		manager = wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1);
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
	struct zwp_input_method_v2 *im;
	struct zwp_input_method_keyboard_grab_v2 *grab;

	if (argc != 1)
		die("usage: ime");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager || !seat)
		die("the compositor lacks zwp_input_method_manager_v2 or a seat");
	im = zwp_input_method_manager_v2_get_input_method(manager, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);
	grab = zwp_input_method_v2_grab_keyboard(im);
	zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	printf("grabbed\n");
	fflush(stdout);
	while (wl_display_dispatch(display) >= 0)
		;
	return 0;
}

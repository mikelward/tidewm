/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' pointer, over wlr-virtual-pointer: it does each action in
 * turn, and exits once the compositor has had them all.
 *
 * Usage: pointer ACTION...
 *   click                     press and release the left button
 *   press BUTTON              press left, middle or right
 *   release BUTTON            release it
 *   move X Y WIDTH HEIGHT     move to X,Y, in a layout WIDTH by HEIGHT
 */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct wl_seat *seat;

static void
die(const char *msg)
{
	fprintf(stderr, "pointer: %s\n", msg);
	exit(1);
}

static void
usage(void)
{
	die("usage: pointer click | press BUTTON | release BUTTON | move X Y WIDTH HEIGHT ...");
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
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

static uint32_t
button(const char *name)
{
	if (!name)
		usage();
	if (!strcmp(name, "left"))
		return BTN_LEFT;
	if (!strcmp(name, "middle"))
		return BTN_MIDDLE;
	if (!strcmp(name, "right"))
		return BTN_RIGHT;
	usage();
	return 0;
}

static uint32_t
number(const char *arg)
{
	char *end;
	unsigned long n;
	if (!arg)
		usage();
	n = strtoul(arg, &end, 10);
	if (*end || end == arg || n > UINT32_MAX)
		usage();
	return n;
}

static void
press(struct zwlr_virtual_pointer_v1 *pointer, uint32_t b, enum wl_pointer_button_state state)
{
	zwlr_virtual_pointer_v1_button(pointer, 0, b, state);
	zwlr_virtual_pointer_v1_frame(pointer);
}

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct zwlr_virtual_pointer_v1 *pointer;
	int i;

	if (argc < 2)
		usage();
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager)
		die("the compositor lacks zwlr_virtual_pointer_manager_v1");
	pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "click")) {
			press(pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
			press(pointer, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
		} else if (!strcmp(argv[i], "press")) {
			press(pointer, button(argv[++i]), WL_POINTER_BUTTON_STATE_PRESSED);
		} else if (!strcmp(argv[i], "release")) {
			press(pointer, button(argv[++i]), WL_POINTER_BUTTON_STATE_RELEASED);
		} else if (!strcmp(argv[i], "move") && i + 4 < argc) {
			uint32_t x = number(argv[i + 1]), y = number(argv[i + 2]);
			uint32_t width = number(argv[i + 3]), height = number(argv[i + 4]);
			i += 4;
			/* Time 0, as a virtual pointer's, warps the cursor there */
			zwlr_virtual_pointer_v1_motion_absolute(pointer, 0, x, y, width, height);
			zwlr_virtual_pointer_v1_frame(pointer);
		} else {
			usage();
		}
	}
	/* The compositor handles requests in order, so it has had them all
	 * by the time it answers */
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	/* Everything freed, so a sanitizer build exits cleanly */
	zwlr_virtual_pointer_v1_destroy(pointer);
	zwlr_virtual_pointer_manager_v1_destroy(manager);
	if (seat)
		wl_seat_destroy(seat);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return 0;
}

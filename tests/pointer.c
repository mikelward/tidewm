/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' pointer, over wlr-virtual-pointer: it clicks the left button
 * wherever the cursor is, and exits once the compositor has had the click.
 *
 * Usage: pointer click
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

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct zwlr_virtual_pointer_v1 *pointer;

	if (argc != 2 || strcmp(argv[1], "click"))
		die("usage: pointer click");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager)
		die("the compositor lacks zwlr_virtual_pointer_manager_v1");
	pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
	zwlr_virtual_pointer_v1_button(pointer, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(pointer);
	zwlr_virtual_pointer_v1_button(pointer, 1, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(pointer);
	/* The compositor handles requests in order, so it has had the click
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

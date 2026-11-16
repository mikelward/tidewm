/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' virtual keyboard, as an on-screen keyboard is, over
 * virtual-keyboard-v1: its keymap has the layouts XKB_LAYOUT names, as
 * "us,de", and it switches to layout LAYOUT, then waits until stdin ends,
 * so the keyboard stays until the test is done with it. It prints
 * "switched" once the compositor has had the switch. With KEY, a key's
 * code, it presses and releases that key after the switch: any code, as a
 * client may send one evdev has no key for. With KEYS, it sends each of
 * its keys, CODE+ a press and CODE- a release, each followed by the
 * modifiers its keymap gives the keys down then, as a keyboard's are
 * after its key: so 125+ 28+ 28- 125- is Super+Return, Super_L a Mod4 key.
 *
 * Usage: XKB_LAYOUT=LAYOUTS [KEY=CODE] [KEYS="CODE+ CODE- ..."] keyboard LAYOUT
 */
#define _GNU_SOURCE /* memfd_create */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;

static void
die(const char *msg)
{
	fprintf(stderr, "keyboard: %s\n", msg);
	exit(1);
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name,
				&zwp_virtual_keyboard_manager_v1_interface, 1);
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

/* The keymap XKB makes of the layouts, in a file the compositor can map;
 * *made, the keymap itself, which the caller unrefs */
static int
keymap_file(const char *layouts, uint32_t *size, struct xkb_keymap **made)
{
	struct xkb_rule_names names = {.layout = layouts};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap;
	char *text;
	int fd;
	if (!context || !(keymap = xkb_keymap_new_from_names(context, &names,
			XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("XKB can't make the keymap");
	if (!(text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1)))
		die("XKB can't write the keymap");
	*size = (uint32_t)strlen(text) + 1;
	if ((fd = memfd_create("tidewm-test-keymap", MFD_CLOEXEC)) < 0
			|| write(fd, text, *size) != (ssize_t)*size)
		die("can't write the keymap's file");
	free(text);
	*made = keymap;
	xkb_context_unref(context);
	return fd;
}

/* KEYS: each CODE+ or CODE-, then the modifiers it leaves */
static void
send_keys(struct zwp_virtual_keyboard_v1 *keyboard, struct xkb_keymap *keymap,
		const char *keys, uint32_t group)
{
	struct xkb_state *state = xkb_state_new(keymap);
	const char *p = keys;
	char *end;
	if (!state)
		die("XKB can't make a state for the keymap");
	xkb_state_update_mask(state, 0, 0, 0, 0, 0, group);
	while (*p) {
		uint32_t code;
		int down;
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		code = (uint32_t)strtoul(p, &end, 10);
		if (end == p || (*end != '+' && *end != '-'))
			die("KEYS: each is a code, then + for a press or - for a release");
		down = *end == '+';
		p = end + 1;
		zwp_virtual_keyboard_v1_key(keyboard, 0, code, down
				? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
		xkb_state_update_key(state, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
		zwp_virtual_keyboard_v1_modifiers(keyboard,
				xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
				xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
				xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
				xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE));
	}
	xkb_state_unref(state);
}

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct wl_registry *registry;
	struct zwp_virtual_keyboard_v1 *keyboard;
	const char *layouts = getenv("XKB_LAYOUT"), *key = getenv("KEY"), *keys = getenv("KEYS");
	struct xkb_keymap *keymap;
	char buf[64];
	uint32_t size;
	int fd;

	if (argc != 2 || !layouts)
		die("usage: XKB_LAYOUT=LAYOUTS keyboard LAYOUT");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager || !seat)
		die("the compositor lacks zwp_virtual_keyboard_manager_v1 or a seat");
	keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
	fd = keymap_file(layouts, &size, &keymap);
	zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	/* No modifier down, and the layout as the group */
	zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, (uint32_t)atoi(argv[1]));
	if (key) {
		uint32_t code = (uint32_t)strtoul(key, NULL, 10);
		zwp_virtual_keyboard_v1_key(keyboard, 0, code, WL_KEYBOARD_KEY_STATE_PRESSED);
		zwp_virtual_keyboard_v1_key(keyboard, 0, code, WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	if (keys)
		send_keys(keyboard, keymap, keys, (uint32_t)atoi(argv[1]));
	xkb_keymap_unref(keymap);
	/* The compositor handles requests in order, so it has had them all
	 * by the time it answers */
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	printf("switched\n");
	fflush(stdout);
	while (read(STDIN_FILENO, buf, sizeof(buf)) > 0)
		;
	zwp_virtual_keyboard_v1_destroy(keyboard);
	zwp_virtual_keyboard_manager_v1_destroy(manager);
	wl_seat_destroy(seat);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	return 0;
}

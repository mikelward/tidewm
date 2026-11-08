/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' window: one xdg-shell toplevel with the app ID and title given
 * on the command line, filled with a solid color. It runs until the
 * compositor closes it or it's killed. Lines on stdin drive it:
 *
 * remap     unmaps the window and maps it again, with the same app ID and
 *           title, which an unmapped toplevel has lost.
 * activate  asks for the window to be activated, over xdg-activation, and
 *           prints "activated" once the compositor has had the request.
 * commit    draws the window again, as it is, and prints "committed" once
 *           the compositor has had the commit.
 * title TEXT
 *           takes the title TEXT.
 * maximize, unmaximize
 *           asks to be maximized, or not.
 * fullscreen, unfullscreen
 *           asks to be fullscreen, or not.
 * dialog [TITLE]
 *           opens a dialog, a second toplevel whose parent is the window,
 *           with the app ID APP_ID.dialog and the title TITLE, or Dialog.
 *           It goes when the compositor closes it.
 * retitled-dialog TITLE
 *           opens one titled Dialog that takes the title TITLE after its
 *           first commit, before it maps.
 * subdialog opens that dialog's own dialog, APP_ID.dialog.dialog.
 * menu      opens a menu, an xdg popup of 50x50 by the window's top left
 *           corner, and prints "menu" once the compositor has it, and
 *           "menu closed" if the compositor closes it.
 * popup menu
 *           opens the menu with an xdg popup grab, as a popup menu takes
 *           the pointer, by the last button pressed on the window, which
 *           SCROLLS has it hear.
 * grab      grabs focus for the window, over hyprland-focus-grab-v1, as
 *           Quickshell's HyprlandFocusGrab does, and prints "grabbing" once
 *           the compositor has the grab, and "cleared" when it ends it.
 * grab menu grabs focus for its menu alone, likewise.
 * ping      prints "pong" once the compositor has answered, so anything
 *           it said before then has been printed.
 *
 * It prints "maximized" or "unmaximized" when the compositor says it's
 * maximized, or no longer, and "fullscreen" or "unfullscreen" likewise.
 *
 * With MAPPED_APP_ID, it changes its app ID to that after its first commit,
 * before it maps. With MIN_SIZE=WxH in its environment, it asks to be at
 * least that big, and with MAX_SIZE=WxH, at most that big. With SIZE=WxH,
 * it's that big until the compositor gives it a size, rather than 100x100.
 * With MAXIMIZED or FULLSCREEN set, it asks to be maximized or fullscreen
 * before it maps, and with MODAL set, to be a modal dialog, over
 * xdg-dialog-v1. With CONTENT_TYPE set to photo, video or game, it says
 * its content is that, over wp-content-type-v1. With SCROLLS set, it
 * prints "pointing" once the compositor has its wl_pointer, which waits
 * for the seat to have a pointer, "scroll AMOUNT" for each vertical
 * scroll it gets, and "button" for each button pressed on it. With
 * GRAB_FIRST set, it grabs focus for itself, as the grab command does,
 * before it first commits, so before it maps. With KEYS set, it prints
 * "typing" once the compositor has its wl_keyboard, and for each key
 * pressed in it, "key " and the key as its keymap reads it, after the
 * modifiers held, as "key ctrl+v". With GESTURES set, it takes the
 * touchpad's gestures over pointer-gestures, prints "pointing" as SCROLLS
 * does, and for each gesture over it, "swipe begin FINGERS",
 * "swipe update DX DY", "swipe end", "pinch begin FINGERS",
 * "pinch update DX DY SCALE ROTATION", "pinch end", "hold begin FINGERS"
 * and "hold end".
 *
 * Usage: client APP_ID TITLE [MAPPED_APP_ID]
 */
#define _GNU_SOURCE /* memfd_create */
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "hyprland-focus-grab-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "content-type-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct xdg_activation_v1 *activation;
static struct xdg_wm_dialog_v1 *wm_dialog;
static struct wp_content_type_manager_v1 *content_types;
static struct hyprland_focus_grab_manager_v1 *grabs;
static struct zwp_pointer_gestures_v1 *gestures;
static struct xdg_surface *window_xdg; /* the window's */
static struct wl_surface *menu_surface;
static int menu_configured;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_state;
static struct xdg_toplevel *window;
static const char *mapped_app_id;
static const char *app_id, *title; /* as it was last given them */
static struct wl_surface *surface;
static struct wl_display *display;
static int32_t width = 100, height = 100;
/* Its states, as last drawn and as configured */
static int maximized, configured_maximized, fullscreen, configured_fullscreen;

static void
die(const char *msg)
{
	fprintf(stderr, "client: %s\n", msg);
	exit(1);
}

static void
paint(struct wl_surface *s, int32_t w, int32_t h)
{
	int32_t stride = w * 4;
	size_t size = (size_t)stride * (size_t)h, i;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int fd = memfd_create("tidewm-test-client", MFD_CLOEXEC);

	if (fd < 0 || ftruncate(fd, (off_t)size) < 0)
		die("can't make a buffer");
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED)
		die("can't map the buffer");
	for (i = 0; i < size / 4; i++)
		pixels[i] = 0xff336699;
	munmap(pixels, size);
	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(s, buffer, 0, 0);
	wl_surface_damage(s, 0, 0, w, h);
	wl_surface_commit(s);
}

static void
draw(void)
{
	paint(surface, width, height);
}

static void
wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xsurface, uint32_t serial)
{
	xdg_surface_ack_configure(xsurface, serial);
	if (configured_maximized != maximized) {
		maximized = configured_maximized;
		printf(maximized ? "maximized\n" : "unmaximized\n");
		fflush(stdout);
	}
	if (configured_fullscreen != fullscreen) {
		fullscreen = configured_fullscreen;
		printf(fullscreen ? "fullscreen\n" : "unfullscreen\n");
		fflush(stdout);
	}
	if (mapped_app_id) {
		xdg_toplevel_set_app_id(window, mapped_app_id);
		app_id = mapped_app_id;
		mapped_app_id = NULL;
	}
	draw();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w,
		int32_t h, struct wl_array *states)
{
	uint32_t *state;
	if (w > 0)
		width = w;
	if (h > 0)
		height = h;
	configured_maximized = configured_fullscreen = 0;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
			configured_maximized = 1;
		else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
			configured_fullscreen = 1;
	}
}

static void
toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	exit(0);
}

static void
toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
		int32_t w, int32_t h)
{
}

static void
toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
		struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

/* Its dialogs: the window's, and that one's own, each with the title to
 * take before it maps, after its first commit */
static struct dialog {
	struct wl_surface *surface;
	struct xdg_surface *xsurface;
	struct xdg_toplevel *toplevel;
	char *retitle;
} dialogs[2];

static void
dialog_surface_configure(void *data, struct xdg_surface *xsurface, uint32_t serial)
{
	struct dialog *d = data;
	xdg_surface_ack_configure(xsurface, serial);
	if (d->retitle) {
		xdg_toplevel_set_title(d->toplevel, d->retitle);
		free(d->retitle);
		d->retitle = NULL;
	}
	paint(d->surface, 100, 100);
}

static const struct xdg_surface_listener dialog_surface_listener = {
	.configure = dialog_surface_configure,
};

static void
dialog_configure(void *data, struct xdg_toplevel *toplevel, int32_t w,
		int32_t h, struct wl_array *states)
{
}

static void
dialog_close(void *data, struct xdg_toplevel *toplevel)
{
	struct dialog *d = data;
	xdg_toplevel_destroy(d->toplevel);
	xdg_surface_destroy(d->xsurface);
	wl_surface_destroy(d->surface);
	free(d->retitle);
	*d = (struct dialog){0};
}

static const struct xdg_toplevel_listener dialog_listener = {
	.configure = dialog_configure,
	.close = dialog_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

/* A dialog, at 100x100, as a file chooser is: the window's, or, as
 * level 1, that dialog's own. It's titled dialog_title, then retitle, if
 * given, before it maps. */
static void
open_dialog(int level, const char *dialog_title, const char *retitle)
{
	struct dialog *d = &dialogs[level];
	char name[256];
	if (d->surface)
		die("there's a dialog already");
	if (level && !dialogs[0].surface)
		die("there's no dialog to open a dialog of");
	if (retitle && !(d->retitle = strdup(retitle)))
		die("can't copy the dialog's title");
	d->surface = wl_compositor_create_surface(compositor);
	d->xsurface = xdg_wm_base_get_xdg_surface(wm_base, d->surface);
	xdg_surface_add_listener(d->xsurface, &dialog_surface_listener, d);
	d->toplevel = xdg_surface_get_toplevel(d->xsurface);
	xdg_toplevel_add_listener(d->toplevel, &dialog_listener, d);
	xdg_toplevel_set_parent(d->toplevel, level ? dialogs[0].toplevel : window);
	snprintf(name, sizeof(name), level ? "%s.dialog.dialog" : "%s.dialog", app_id);
	xdg_toplevel_set_app_id(d->toplevel, name);
	xdg_toplevel_set_title(d->toplevel, dialog_title);
	wl_surface_commit(d->surface);
}

/* Prints data, the word for what the compositor has had by now */
static void
synced(void *data, struct wl_callback *callback, uint32_t serial)
{
	wl_callback_destroy(callback);
	printf("%s\n", (const char *)data);
	fflush(stdout);
}

static const struct wl_callback_listener synced_listener = {
	.done = synced,
};

static void
menu_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	if (menu_configured)
		return;
	menu_configured = 1;
	paint(menu_surface, 50, 50);
	wl_callback_add_listener(wl_display_sync(display), &synced_listener, (void *)"menu");
}

static const struct xdg_surface_listener menu_listener = {
	.configure = menu_configure,
};

static void
popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t w,
		int32_t h)
{
}

static void
popup_done(void *data, struct xdg_popup *popup)
{
	printf("menu closed\n");
	fflush(stdout);
}

static void
popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token)
{
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
	.repositioned = popup_repositioned,
};

static uint32_t button_serial; /* the last button pressed on the window's */

static void
open_menu(int grab)
{
	struct xdg_surface *menu_xdg;
	struct xdg_positioner *positioner;
	struct xdg_popup *popup;
	menu_surface = wl_compositor_create_surface(compositor);
	menu_xdg = xdg_wm_base_get_xdg_surface(wm_base, menu_surface);
	xdg_surface_add_listener(menu_xdg, &menu_listener, NULL);
	positioner = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(positioner, 50, 50);
	xdg_positioner_set_anchor_rect(positioner, 0, 0, 1, 1);
	popup = xdg_surface_get_popup(menu_xdg, window_xdg, positioner);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	if (grab)
		xdg_popup_grab(popup, seat, button_serial);
	xdg_positioner_destroy(positioner);
	wl_surface_commit(menu_surface);
}

static void
grab_cleared(void *data, struct hyprland_focus_grab_v1 *grab)
{
	printf("cleared\n");
	fflush(stdout);
}

static const struct hyprland_focus_grab_v1_listener grab_listener = {
	.cleared = grab_cleared,
};

static void
grab_surface(struct wl_surface *s)
{
	struct hyprland_focus_grab_v1 *grab;
	if (!grabs || !s)
		die("the compositor lacks hyprland_focus_grab_manager_v1, or there's no menu");
	grab = hyprland_focus_grab_manager_v1_create_grab(grabs);
	hyprland_focus_grab_v1_add_listener(grab, &grab_listener, NULL);
	hyprland_focus_grab_v1_add_surface(grab, s);
	hyprland_focus_grab_v1_commit(grab);
	wl_callback_add_listener(wl_display_sync(display), &synced_listener, (void *)"grabbing");
}

static void
pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y)
{
}

static void
pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
}

static void
pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
}

static void
pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state)
{
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	button_serial = serial;
	printf("button\n");
	fflush(stdout);
}

static void
pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	printf("scroll %.2f\n", wl_fixed_to_double(value));
	fflush(stdout);
}

static void
pointer_frame(void *data, struct wl_pointer *p)
{
}

static void
pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void
pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void
said(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static void
swipe_begin(void *data, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t serial,
		uint32_t time, struct wl_surface *s, uint32_t fingers)
{
	said("swipe begin %u", fingers);
}

static void
swipe_update(void *data, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t time,
		wl_fixed_t dx, wl_fixed_t dy)
{
	said("swipe update %.2f %.2f", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

static void
swipe_end(void *data, struct zwp_pointer_gesture_swipe_v1 *g, uint32_t serial,
		uint32_t time, int32_t cancelled)
{
	said("swipe end");
}

static const struct zwp_pointer_gesture_swipe_v1_listener swipe_listener = {
	.begin = swipe_begin,
	.update = swipe_update,
	.end = swipe_end,
};

static void
pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t serial,
		uint32_t time, struct wl_surface *s, uint32_t fingers)
{
	said("pinch begin %u", fingers);
}

static void
pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t time,
		wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation)
{
	said("pinch update %.2f %.2f %.2f %.2f", wl_fixed_to_double(dx), wl_fixed_to_double(dy),
			wl_fixed_to_double(scale), wl_fixed_to_double(rotation));
}

static void
pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t serial,
		uint32_t time, int32_t cancelled)
{
	said("pinch end");
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
	.begin = pinch_begin,
	.update = pinch_update,
	.end = pinch_end,
};

static void
hold_begin(void *data, struct zwp_pointer_gesture_hold_v1 *g, uint32_t serial,
		uint32_t time, struct wl_surface *s, uint32_t fingers)
{
	said("hold begin %u", fingers);
}

static void
hold_end(void *data, struct zwp_pointer_gesture_hold_v1 *g, uint32_t serial,
		uint32_t time, int32_t cancelled)
{
	said("hold end");
}

static const struct zwp_pointer_gesture_hold_v1_listener hold_listener = {
	.begin = hold_begin,
	.end = hold_end,
};

static void
keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
	char *map;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		die("the keymap isn't XKB's");
	if ((map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
		die("can't map the keymap");
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	if (!(keymap = xkb_keymap_new_from_string(xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("can't compile the keymap");
	xkb_state = xkb_state_new(keymap);
	munmap(map, size);
	close(fd);
}

static void
keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		struct wl_array *keys)
{
}

static void
keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
}

static void
keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state)
{
	static const struct { const char *name, *xkb; } mods[] = {
		{ "ctrl", XKB_MOD_NAME_CTRL }, { "shift", XKB_MOD_NAME_SHIFT },
		{ "alt", XKB_MOD_NAME_ALT }, { "super", XKB_MOD_NAME_LOGO },
	};
	char name[64];
	size_t i;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_state)
		return;
	printf("key ");
	for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
		if (xkb_state_mod_name_is_active(xkb_state, mods[i].xkb, XKB_STATE_MODS_EFFECTIVE) > 0)
			printf("%s+", mods[i].name);
	/* An XKB keycode is evdev's plus 8 */
	xkb_keysym_get_name(xkb_state_key_get_one_sym(xkb_state, key + 8), name, sizeof(name));
	printf("%s\n", name);
	fflush(stdout);
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
		uint32_t latched, uint32_t locked, uint32_t group)
{
	if (xkb_state)
		xkb_state_update_mask(xkb_state, depressed, latched, locked, 0, 0, group);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && getenv("KEYS") && !keyboard) {
		xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
		/* The compositor answers in order, so it has the keyboard by then */
		wl_callback_add_listener(wl_display_sync(display), &synced_listener,
				(void *)"typing");
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && (getenv("SCROLLS") || getenv("GESTURES"))
			&& !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		if (getenv("GESTURES")) {
			if (!gestures)
				die("the compositor lacks zwp_pointer_gestures_v1");
			zwp_pointer_gesture_swipe_v1_add_listener(
					zwp_pointer_gestures_v1_get_swipe_gesture(gestures, pointer),
					&swipe_listener, NULL);
			zwp_pointer_gesture_pinch_v1_add_listener(
					zwp_pointer_gestures_v1_get_pinch_gesture(gestures, pointer),
					&pinch_listener, NULL);
			zwp_pointer_gesture_hold_v1_add_listener(
					zwp_pointer_gestures_v1_get_hold_gesture(gestures, pointer),
					&hold_listener, NULL);
		}
		/* The compositor answers in order, so it has the pointer by then */
		wl_callback_add_listener(wl_display_sync(display), &synced_listener,
				(void *)"pointing");
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
token_done(void *data, struct xdg_activation_token_v1 *token, const char *name)
{
	xdg_activation_v1_activate(activation, name, surface);
	xdg_activation_token_v1_destroy(token);
	/* The compositor answers in order, so it has had the request by then */
	wl_callback_add_listener(wl_display_sync(display), &synced_listener, (void *)"activated");
}

static const struct xdg_activation_token_v1_listener token_listener = {
	.done = token_done,
};

/* Asks for a token for this surface, then activates it with that token,
 * as an app does when it's started for something already open. */
static void
activate(void)
{
	struct xdg_activation_token_v1 *token;
	if (!activation)
		die("the compositor lacks xdg_activation_v1");
	token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
	xdg_activation_token_v1_set_surface(token, surface);
	xdg_activation_token_v1_commit(token);
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name))
		/* Version 2 or later, as most apps bind, so a tiled window
		 * hears it's tiled, not maximized */
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
				version < 5 ? version : 5);
	else if (!strcmp(interface, xdg_activation_v1_interface.name))
		activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
	else if (!strcmp(interface, xdg_wm_dialog_v1_interface.name))
		wm_dialog = wl_registry_bind(registry, name, &xdg_wm_dialog_v1_interface, 1);
	else if (!strcmp(interface, wp_content_type_manager_v1_interface.name))
		content_types = wl_registry_bind(registry, name,
				&wp_content_type_manager_v1_interface, 1);
	else if (!strcmp(interface, hyprland_focus_grab_manager_v1_interface.name))
		grabs = wl_registry_bind(registry, name, &hyprland_focus_grab_manager_v1_interface, 1);
	else if (!strcmp(interface, zwp_pointer_gestures_v1_interface.name) && version >= 3)
		/* Version 3, for holds */
		gestures = wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 3);
	else if (!strcmp(interface, wl_seat_interface.name)
			&& (getenv("SCROLLS") || getenv("KEYS") || getenv("GESTURES"))
			&& !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
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

/* One line from stdin */
static void
command(const char *line)
{
	if (!strcmp(line, "remap")) {
		/* Unmap, then commit again, as a new surface's first commit,
		 * so the compositor configures it and it maps again. */
		wl_surface_attach(surface, NULL, 0, 0);
		wl_surface_commit(surface);
		xdg_toplevel_set_app_id(window, app_id);
		xdg_toplevel_set_title(window, title);
		wl_surface_commit(surface);
	} else if (!strcmp(line, "activate")) {
		activate();
	} else if (!strncmp(line, "title ", 6)) {
		xdg_toplevel_set_title(window, line + 6);
		wl_surface_commit(surface);
	} else if (!strcmp(line, "commit")) {
		draw();
		/* The compositor answers in order, so it has had the commit by then */
		wl_callback_add_listener(wl_display_sync(display), &synced_listener, (void *)"committed");
	} else if (!strcmp(line, "maximize")) {
		xdg_toplevel_set_maximized(window);
	} else if (!strcmp(line, "unmaximize")) {
		xdg_toplevel_unset_maximized(window);
	} else if (!strcmp(line, "fullscreen")) {
		xdg_toplevel_set_fullscreen(window, NULL);
	} else if (!strcmp(line, "unfullscreen")) {
		xdg_toplevel_unset_fullscreen(window);
	} else if (!strcmp(line, "dialog")) {
		open_dialog(0, "Dialog", NULL);
	} else if (!strncmp(line, "dialog ", 7)) {
		open_dialog(0, line + 7, NULL);
	} else if (!strncmp(line, "retitled-dialog ", 16)) {
		open_dialog(0, "Dialog", line + 16);
	} else if (!strcmp(line, "subdialog")) {
		open_dialog(1, "Dialog", NULL);
	} else if (!strcmp(line, "menu")) {
		open_menu(0);
	} else if (!strcmp(line, "popup menu")) {
		open_menu(1);
	} else if (!strcmp(line, "grab")) {
		grab_surface(surface);
	} else if (!strcmp(line, "grab menu")) {
		grab_surface(menu_surface);
	} else if (!strcmp(line, "ping")) {
		wl_callback_add_listener(wl_display_sync(display), &synced_listener, (void *)"pong");
	}
}

int
main(int argc, char *argv[])
{
	const char *min = getenv("MIN_SIZE"), *max = getenv("MAX_SIZE"), *size = getenv("SIZE");
	const char *content;
	int hint_width, hint_height;
	char buf[256];
	size_t buflen = 0;

	if (argc != 3 && argc != 4)
		die("usage: client APP_ID TITLE [MAPPED_APP_ID]");
	mapped_app_id = argv[3]; /* argv[argc] is NULL */
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!compositor || !shm || !wm_base)
		die("the compositor lacks wl_compositor, wl_shm or xdg_wm_base");
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	window_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(window_xdg, &xdg_surface_listener, NULL);
	window = xdg_surface_get_toplevel(window_xdg);
	xdg_toplevel_add_listener(window, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(window, app_id = argv[1]);
	xdg_toplevel_set_title(window, title = argv[2]);
	if (min && sscanf(min, "%dx%d", &hint_width, &hint_height) == 2)
		xdg_toplevel_set_min_size(window, hint_width, hint_height);
	if (max && sscanf(max, "%dx%d", &hint_width, &hint_height) == 2)
		xdg_toplevel_set_max_size(window, hint_width, hint_height);
	if (size && sscanf(size, "%dx%d", &hint_width, &hint_height) == 2) {
		width = hint_width;
		height = hint_height;
	}
	if (getenv("MODAL")) {
		if (!wm_dialog)
			die("the compositor lacks xdg_wm_dialog_v1");
		xdg_dialog_v1_set_modal(xdg_wm_dialog_v1_get_xdg_dialog(wm_dialog, window));
	}
	if ((content = getenv("CONTENT_TYPE"))) {
		uint32_t type = !strcmp(content, "photo") ? WP_CONTENT_TYPE_V1_TYPE_PHOTO
				: !strcmp(content, "video") ? WP_CONTENT_TYPE_V1_TYPE_VIDEO
				: !strcmp(content, "game") ? WP_CONTENT_TYPE_V1_TYPE_GAME
				: (die("CONTENT_TYPE is photo, video or game"), 0);
		if (!content_types)
			die("the compositor lacks wp_content_type_manager_v1");
		wp_content_type_v1_set_content_type(
				wp_content_type_manager_v1_get_surface_content_type(content_types, surface),
				type);
	}
	if (getenv("MAXIMIZED"))
		xdg_toplevel_set_maximized(window);
	if (getenv("FULLSCREEN"))
		xdg_toplevel_set_fullscreen(window, NULL);
	if (getenv("GRAB_FIRST"))
		grab_surface(surface);
	wl_surface_commit(surface);

	for (int input = STDIN_FILENO;;) {
		struct pollfd fds[2] = {
			{.fd = wl_display_get_fd(display), .events = POLLIN},
			{.fd = input, .events = POLLIN}, /* poll skips -1 */
		};
		char *line, *nl;
		ssize_t n;
		if (wl_display_flush(display) < 0)
			die("lost the compositor");
		if (poll(fds, 2, -1) < 0)
			die("poll failed");
		if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0)
			return 0; /* the compositor went away */
		if (fds[0].revents & (POLLHUP | POLLERR))
			return 0;
		if (!(fds[1].revents & (POLLIN | POLLHUP)))
			continue;
		if ((n = read(input, buf + buflen, sizeof(buf) - 1 - buflen)) <= 0) {
			input = -1; /* stdin closed: just stay mapped */
			continue;
		}
		buflen += (size_t)n;
		buf[buflen] = '\0';
		/* Each whole line, in order: one read may bring several */
		for (line = buf; (nl = strchr(line, '\n')); line = nl + 1) {
			*nl = '\0';
			command(line);
		}
		buflen = strlen(line);
		memmove(buf, line, buflen + 1);
		if (buflen == sizeof(buf) - 1)
			buflen = 0; /* a line too long to be a command */
	}
}

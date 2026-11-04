/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' screen share, over ext-image-copy-capture, as
 * xdg-desktop-portal-wlr's is: it starts a capture session of the first
 * output, or of the window with APP_ID, found over ext-foreign-toplevel-list,
 * or of just the pointer's cursor over the first output, as a stream beside
 * an output's, and prints "capturing" once the compositor has it. A cursor
 * needs the seat to have a pointer. A line "stop" on stdin, or its end,
 * stops the session; it prints "stopped" once the compositor has had that,
 * and exits.
 *
 * Usage: capture output | capture window APP_ID | capture cursor
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_output *output;
static struct wl_seat *seat;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_output_image_capture_source_manager_v1 *output_sources;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_sources;
static struct ext_image_copy_capture_manager_v1 *copy_capture;
static const char *wanted; /* the app ID of the window to capture */
static struct ext_foreign_toplevel_handle_v1 *found;

static void
die(const char *msg)
{
	fprintf(stderr, "capture: %s\n", msg);
	exit(1);
}

static void
handle_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
}

static void
handle_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle)
{
}

static void
handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *title)
{
}

static void
handle_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle, const char *app_id)
{
	if (wanted && !found && !strcmp(app_id, wanted))
		found = handle;
}

static void
handle_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *identifier)
{
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	.closed = handle_closed,
	.done = handle_done,
	.title = handle_title,
	.app_id = handle_app_id,
	.identifier = handle_identifier,
};

static void
list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l,
		struct ext_foreign_toplevel_handle_v1 *handle)
{
	ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, NULL);
}

static void
list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l)
{
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_output_interface.name) && !output)
		output = wl_registry_bind(registry, name, &wl_output_interface, 1);
	else if (!strcmp(interface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name)) {
		/* It sends the windows at once */
		list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, NULL);
	}
	else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name))
		output_sources = wl_registry_bind(registry, name,
				&ext_output_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(interface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name))
		toplevel_sources = wl_registry_bind(registry, name,
				&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name))
		copy_capture = wl_registry_bind(registry, name,
				&ext_image_copy_capture_manager_v1_interface, 1);
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
	struct ext_image_capture_source_v1 *source;
	struct ext_image_copy_capture_session_v1 *session;
	struct ext_image_copy_capture_cursor_session_v1 *cursor = NULL;
	struct wl_pointer *pointer = NULL;
	char line[64];
	int of_cursor = argc == 2 && !strcmp(argv[1], "cursor");

	if (argc == 3 && !strcmp(argv[1], "window"))
		wanted = argv[2];
	else if (argc != 2 || (strcmp(argv[1], "output") && !of_cursor))
		die("usage: capture output | capture window APP_ID | capture cursor");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!copy_capture || !output || !seat || !list || !output_sources || !toplevel_sources)
		die("the compositor lacks an output, a seat, ext-foreign-toplevel-list or image capture");
	/* What each window is called */
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (wanted && !found)
		die("no window has that app ID");
	source = wanted
			? ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
				toplevel_sources, found)
			: ext_output_image_capture_source_manager_v1_create_source(output_sources, output);
	if (of_cursor) {
		pointer = wl_seat_get_pointer(seat);
		cursor = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(copy_capture,
				source, pointer);
		session = ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor);
	} else {
		session = ext_image_copy_capture_manager_v1_create_session(copy_capture, source, 0);
	}
	/* The compositor handles requests in order, so it has the session by
	 * the time it answers */
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	printf("capturing\n");
	fflush(stdout);
	while (fgets(line, sizeof(line), stdin) && strcmp(line, "stop\n"))
		;
	ext_image_copy_capture_session_v1_destroy(session);
	if (cursor)
		ext_image_copy_capture_cursor_session_v1_destroy(cursor);
	if (pointer)
		wl_pointer_destroy(pointer);
	ext_image_capture_source_v1_destroy(source);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	printf("stopped\n");
	fflush(stdout);
	return 0;
}

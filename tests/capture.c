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
 * capture shot APP_ID instead takes one frame of the window, as grim -T
 * does, into shared memory, and prints its size and the color of its
 * middle pixel as "WIDTHxHEIGHT RRGGBB", or "failed REASON". capture
 * pixels X,Y... takes one frame of the first output, as grim does, and
 * prints the color of each pixel named, in the output's own pixels, as
 * "RRGGBB ...", "none" for one outside it.
 *
 * Usage: capture output | capture window APP_ID | capture cursor |
 *        capture shot APP_ID | capture pixels X,Y...
 */
#define _GNU_SOURCE /* memfd_create */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_output *output;
static struct wl_seat *seat;
static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_output_image_capture_source_manager_v1 *output_sources;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_sources;
static struct ext_image_copy_capture_manager_v1 *copy_capture;
static const char *wanted; /* the app ID of the window to capture */
static char **points; /* capture pixels' X,Y, NULL-terminated */
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
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
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

/* A shot: the session's constraints, then the frame's end */
static uint32_t shot_width, shot_height;
static int shot_xrgb = -1; /* WL_SHM_FORMAT_XRGB8888 or ARGB8888, if offered */
static int shot_constrained, shot_over;
static const char *shot_failure;

static void
session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t width, uint32_t height)
{
	shot_width = width;
	shot_height = height;
}

static void
session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t format)
{
	if (format == WL_SHM_FORMAT_XRGB8888 || (format == WL_SHM_FORMAT_ARGB8888 && shot_xrgb < 0))
		shot_xrgb = (int)format;
}

static void
session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
		struct wl_array *device)
{
}

static void
session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t format, struct wl_array *modifiers)
{
}

static void
session_done(void *data, struct ext_image_copy_capture_session_v1 *s)
{
	shot_constrained = 1;
}

static void
session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s)
{
	shot_failure = "stopped";
	shot_over = 1;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_buffer_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

static void
frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t transform)
{
}

static void
frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
}

static void
frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *f,
		uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
}

static void
frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f)
{
	shot_over = 1;
}

static void
frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason)
{
	shot_failure = reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED ? "stopped"
			: reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS
			? "buffer-constraints" : "unknown";
	shot_over = 1;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_presentation_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

/* Takes one frame of the session's source and prints it */
static void
shot(struct wl_display *display, struct ext_image_copy_capture_session_v1 *session)
{
	struct ext_image_copy_capture_frame_v1 *frame;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels, middle;
	size_t stride, size;
	int fd;

	ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
	while (!shot_constrained && !shot_over)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	if (shot_over) {
		printf("failed %s\n", shot_failure);
		return;
	}
	if (shot_xrgb < 0 || !shot_width || !shot_height)
		die("the compositor offers no 32-bit RGB shared memory buffer");
	stride = (size_t)shot_width * 4;
	size = stride * shot_height;
	if ((fd = memfd_create("tidewm-test-capture", MFD_CLOEXEC)) < 0
			|| ftruncate(fd, (off_t)size) < 0)
		die("can't make shared memory");
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED)
		die("can't map shared memory");
	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)shot_width, (int32_t)shot_height,
			(int32_t)stride, (uint32_t)shot_xrgb);
	wl_shm_pool_destroy(pool);
	close(fd);

	frame = ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0,
			(int32_t)shot_width, (int32_t)shot_height);
	ext_image_copy_capture_frame_v1_capture(frame);
	while (!shot_over)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	if (shot_failure) {
		printf("failed %s\n", shot_failure);
	} else if (points) {
		char **p;
		for (p = points; *p; p++) {
			unsigned int x, y;
			if (sscanf(*p, "%u,%u", &x, &y) != 2)
				die("a pixel is X,Y");
			if (x < shot_width && y < shot_height)
				printf("%s%06x", p == points ? "" : " ",
						pixels[y * shot_width + x] & 0xffffff);
			else
				printf("%snone", p == points ? "" : " ");
		}
		printf("\n");
	} else {
		middle = pixels[(shot_height / 2) * shot_width + shot_width / 2];
		printf("%ux%u %06x\n", shot_width, shot_height, middle & 0xffffff);
	}
	ext_image_copy_capture_frame_v1_destroy(frame);
	wl_buffer_destroy(buffer);
	munmap(pixels, size);
}

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
	int a_shot = argc == 3 && !strcmp(argv[1], "shot");

	if (argc >= 3 && !strcmp(argv[1], "pixels")) {
		points = argv + 2;
		a_shot = 1;
	} else if (argc == 3 && (!strcmp(argv[1], "window") || a_shot)) {
		wanted = argv[2];
	} else if (argc != 2 || (strcmp(argv[1], "output") && !of_cursor)) {
		die("usage: capture output | capture window APP_ID | capture cursor | "
				"capture shot APP_ID | capture pixels X,Y...");
	}
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!copy_capture || !output || !seat || !shm || !list || !output_sources || !toplevel_sources)
		die("the compositor lacks an output, a seat, shared memory, "
				"ext-foreign-toplevel-list or image capture");
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
	if (a_shot) {
		shot(display, session);
		fflush(stdout);
		ext_image_copy_capture_session_v1_destroy(session);
		ext_image_capture_source_v1_destroy(source);
		wl_display_roundtrip(display);
		return 0;
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

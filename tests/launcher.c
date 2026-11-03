/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' launcher: an overlay layer surface that takes the keyboard,
 * as tide's launcher does, over wlr-layer-shell. It prints "open" once the
 * compositor has it, then a line "close" on stdin closes it, and it
 * prints "closed" once the compositor has had that, and exits.
 *
 * Usage: launcher
 */
#define _GNU_SOURCE /* memfd_create */
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
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
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

	if (argc != 1)
		die("usage: launcher");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!compositor || !shm || !layer_shell)
		die("the compositor lacks wl_compositor, wl_shm or zwlr_layer_shell_v1");
	surface = wl_compositor_create_surface(compositor);
	layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
			ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "launcher");
	zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer, 200, 100);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer, 1); /* exclusive */
	wl_surface_commit(surface);
	/* The configure, then the buffer it draws, which maps it */
	while (!configured)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	printf("open\n");
	fflush(stdout);
	while (fgets(line, sizeof(line), stdin) && strcmp(line, "close\n"))
		;
	zwlr_layer_surface_v1_destroy(layer);
	wl_surface_destroy(surface);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	printf("closed\n");
	fflush(stdout);
	return 0;
}

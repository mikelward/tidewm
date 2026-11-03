/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' window: one xdg-shell toplevel with the app ID and title given
 * on the command line, filled with a solid color. It runs until the
 * compositor closes it or it's killed. A line "remap" on stdin unmaps the
 * window and maps it again.
 *
 * Usage: client APP_ID TITLE
 */
#define _GNU_SOURCE /* memfd_create */
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static int32_t width = 100, height = 100;

static void
die(const char *msg)
{
	fprintf(stderr, "client: %s\n", msg);
	exit(1);
}

static void
draw(void)
{
	int32_t stride = width * 4;
	size_t size = (size_t)stride * (size_t)height, i;
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
	buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
			WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);
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
	draw();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void
toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w,
		int32_t h, struct wl_array *states)
{
	if (w > 0)
		width = w;
	if (h > 0)
		height = h;
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

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name))
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
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
	struct xdg_surface *xsurface;
	struct xdg_toplevel *toplevel;

	if (argc != 3)
		die("usage: client APP_ID TITLE");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!compositor || !shm || !wm_base)
		die("the compositor lacks wl_compositor, wl_shm or xdg_wm_base");
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	surface = wl_compositor_create_surface(compositor);
	xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xsurface, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xsurface);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, argv[1]);
	xdg_toplevel_set_title(toplevel, argv[2]);
	wl_surface_commit(surface);

	for (int input = STDIN_FILENO;;) {
		struct pollfd fds[2] = {
			{.fd = wl_display_get_fd(display), .events = POLLIN},
			{.fd = input, .events = POLLIN}, /* poll skips -1 */
		};
		char line[64];
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
		if (!fgets(line, sizeof(line), stdin)) {
			input = -1; /* stdin closed: just stay mapped */
			continue;
		}
		if (!strcmp(line, "remap\n")) {
			/* Unmap, then commit again, as a new surface's first commit,
			 * so the compositor configures it and it maps again. */
			wl_surface_attach(surface, NULL, 0, 0);
			wl_surface_commit(surface);
			wl_surface_commit(surface);
		}
	}
}

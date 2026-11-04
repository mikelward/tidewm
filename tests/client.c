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
 * xdg-dialog-v1.
 *
 * Usage: client APP_ID TITLE [MAPPED_APP_ID]
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

#include "xdg-activation-v1-client-protocol.h"
#include "xdg-dialog-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct xdg_activation_v1 *activation;
static struct xdg_wm_dialog_v1 *wm_dialog;
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
	}
}

int
main(int argc, char *argv[])
{
	struct xdg_surface *xsurface;
	const char *min = getenv("MIN_SIZE"), *max = getenv("MAX_SIZE"), *size = getenv("SIZE");
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
	xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xsurface, &xdg_surface_listener, NULL);
	window = xdg_surface_get_toplevel(xsurface);
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
	if (getenv("MAXIMIZED"))
		xdg_toplevel_set_maximized(window);
	if (getenv("FULLSCREEN"))
		xdg_toplevel_set_fullscreen(window, NULL);
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

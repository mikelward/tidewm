/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' X11 window: one window with the class and title given on the
 * command line. It changes its title once before it maps, which the
 * compositor may hear of while the window has no Wayland surface yet. It
 * runs until it's killed or the X server goes.
 *
 * Usage: x11client CLASS TITLE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static void
die(const char *msg)
{
	fprintf(stderr, "x11client: %s\n", msg);
	exit(1);
}

/* Waits until the X server has handled every request so far. */
static void
sync_server(xcb_connection_t *conn)
{
	free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
}

int
main(int argc, char *argv[])
{
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	xcb_window_t win;
	char class[256];
	size_t len;
	xcb_generic_event_t *event;

	if (argc != 3)
		die("usage: x11client CLASS TITLE");
	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn))
		die("can't connect to the X server");
	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	win = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
			0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual, XCB_CW_BACK_PIXEL, &screen->white_pixel);
	/* WM_CLASS is the instance, then the class, each NUL-terminated */
	len = (size_t)snprintf(class, sizeof(class), "%s%c%s", argv[1], '\0', argv[1]) + 1;
	xcb_icccm_set_wm_class(conn, win, (uint32_t)len, class);
	xcb_icccm_set_wm_name(conn, win, XCB_ATOM_STRING, 8, 7, "unnamed");
	sync_server(conn);
	/* A title change while unmapped */
	xcb_icccm_set_wm_name(conn, win, XCB_ATOM_STRING, 8,
			(uint32_t)strlen(argv[2]), argv[2]);
	sync_server(conn);
	xcb_map_window(conn, win);
	xcb_flush(conn);
	while ((event = xcb_wait_for_event(conn)))
		free(event);
	return 0;
}

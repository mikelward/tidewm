/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' X11 window: one window with the class and title given on the
 * command line. It changes its title once before it maps, which the
 * compositor may hear of while the window has no Wayland surface yet. It
 * runs until it's killed or the X server goes.
 *
 * It waits for the window manager first. Xwayland starts for the first X
 * client, and wlroots' window manager doesn't take on a window that was
 * there before it watched the root, so a window made in between would
 * never be managed.
 *
 * A line "activate" on stdin asks for the window to be activated, with a
 * _NET_ACTIVE_WINDOW message to the root, as an application does.
 *
 * Usage: x11client CLASS TITLE
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

static void
die(const char *msg)
{
	fprintf(stderr, "x11client: %s\n", msg);
	exit(1);
}

/* Waits until the window manager is watching the root: it selects
 * substructure redirect there, then sets _NET_SUPPORTING_WM_CHECK, on one
 * connection, so the X server has done the first once the second is set. */
static void
wait_for_wm(xcb_connection_t *conn, xcb_window_t root)
{
	static const char name[] = "_NET_SUPPORTING_WM_CHECK";
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_intern_atom_reply_t *atom;
	xcb_get_property_reply_t *prop;
	xcb_generic_event_t *event;
	int there;

	if (!(atom = xcb_intern_atom_reply(conn,
			xcb_intern_atom(conn, 0, sizeof(name) - 1, name), NULL)))
		die("can't intern _NET_SUPPORTING_WM_CHECK");
	/* Watch first, then look, so a change in between isn't missed */
	xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, &mask);
	for (;;) {
		prop = xcb_get_property_reply(conn, xcb_get_property(conn, 0, root,
				atom->atom, XCB_ATOM_WINDOW, 0, 1), NULL);
		there = prop && xcb_get_property_value_length(prop) > 0;
		free(prop);
		if (there)
			break;
		/* Any property change on the root may be it */
		do {
			if (!(event = xcb_wait_for_event(conn)))
				die("the X server went before a window manager came");
			there = (event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY;
			free(event);
		} while (!there);
	}
	free(atom);
	mask = 0;
	xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, &mask);
}

/* Asks the window manager to activate win, as an application does: source
 * 1, at no particular time */
static void
activate(xcb_connection_t *conn, xcb_window_t root, xcb_window_t win)
{
	static const char name[] = "_NET_ACTIVE_WINDOW";
	xcb_intern_atom_reply_t *atom;
	xcb_client_message_event_t msg = {0};
	if (!(atom = xcb_intern_atom_reply(conn,
			xcb_intern_atom(conn, 0, sizeof(name) - 1, name), NULL)))
		die("can't intern _NET_ACTIVE_WINDOW");
	msg.response_type = XCB_CLIENT_MESSAGE;
	msg.format = 32;
	msg.window = win;
	msg.type = atom->atom;
	msg.data.data32[0] = 1;
	msg.data.data32[1] = XCB_CURRENT_TIME;
	free(atom);
	xcb_send_event(conn, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
			| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&msg);
	xcb_flush(conn);
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
	struct pollfd fds[2];
	char line[64];
	ssize_t n;

	if (argc != 3)
		die("usage: x11client CLASS TITLE");
	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn))
		die("can't connect to the X server");
	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	wait_for_wm(conn, screen->root);
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
	fds[0] = (struct pollfd){.fd = xcb_get_file_descriptor(conn), .events = POLLIN};
	fds[1] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
	for (;;) {
		if (poll(fds, 2, -1) < 0)
			die("can't poll");
		while ((event = xcb_poll_for_event(conn)))
			free(event);
		if (xcb_connection_has_error(conn))
			return 0;
		if (fds[1].revents) {
			/* At the end of stdin, only the X server is left to read */
			if ((n = read(STDIN_FILENO, line, sizeof(line) - 1)) <= 0) {
				fds[1].fd = -1;
				continue;
			}
			line[n] = '\0';
			if (!strcmp(line, "activate\n"))
				activate(conn, screen->root, win);
		}
	}
}

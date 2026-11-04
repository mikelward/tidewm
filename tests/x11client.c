/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' X11 window: one window with the class and title given on the
 * command line. It changes its title once before it maps, which the
 * compositor may hear of while the window has no Wayland surface yet. It
 * runs until it's killed or the X server goes.
 *
 * It waits for the window manager first, as it has since a stall in the
 * tests was put down to a window made before wlroots' window manager
 * watched the root. Xwayland rules that out: it takes no client until the
 * window manager owns WM_S0, which wlroots takes after it watches the
 * root. The wait stays, as every run since has had it (TODO.md, Known
 * bugs).
 *
 * A line "activate" on stdin asks for the window to be activated, with a
 * _NET_ACTIVE_WINDOW message to the root, as an application does. Lines
 * "urgent on" and "urgent off" set and clear its WM_HINTS urgency hint,
 * and "title TEXT" sets its title, which a window manager hears of after
 * any change before it. "configure X Y W H" asks for that place and size,
 * in X11's space.
 *
 * With FULLSCREEN set in its environment, it says it's fullscreen in
 * _NET_WM_STATE before it maps, as an X11 game or video player does, once
 * wlroots' window manager watches the window (wait_for_watch).
 *
 * With FIXED_SIZE=WxH, its WM_NORMAL_HINTS say it's that big, no more
 * and no less, in X11's pixels.
 *
 * With REPORT set, it prints "screen W H", the X screen's size, at the
 * start, then "size W H" each time its window's size changes, "button X Y"
 * for each button pressed in it, at that point in it, and "motion X Y" for
 * each move with a button held, there, all in X11's pixels.
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

static xcb_atom_t
intern(xcb_connection_t *conn, const char *name)
{
	xcb_intern_atom_reply_t *reply;
	xcb_atom_t atom;
	if (!(reply = xcb_intern_atom_reply(conn,
			xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL)))
		die("can't intern an atom");
	atom = reply->atom;
	free(reply);
	return atom;
}

/* Waits until wlroots' window manager watches win's properties, as it does
 * once it has handled win's creation. Until then it misses a change, and
 * mapping win writes its own idea of _NET_WM_STATE over the window's
 * (TODO.md, Known bugs). A _NET_WM_STATE message for a state nothing here
 * uses makes it write that property, and win, watching its own
 * properties, hears it: the window manager handles events in order, so
 * it has handled win's creation by then. */
static void
wait_for_watch(xcb_connection_t *conn, xcb_window_t root, xcb_window_t win)
{
	xcb_atom_t state = intern(conn, "_NET_WM_STATE");
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_client_message_event_t msg = {0};
	xcb_generic_event_t *event;
	int heard = 0;
	xcb_change_window_attributes(conn, win, XCB_CW_EVENT_MASK, &mask);
	msg.response_type = XCB_CLIENT_MESSAGE;
	msg.format = 32;
	msg.window = win;
	msg.type = state;
	msg.data.data32[0] = 1; /* _NET_WM_STATE_ADD */
	msg.data.data32[1] = intern(conn, "_NET_WM_STATE_SKIP_PAGER");
	xcb_send_event(conn, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
			| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&msg);
	xcb_flush(conn);
	while (!heard) {
		if (!(event = xcb_wait_for_event(conn)))
			die("the X server went before the window manager watched the window");
		heard = (event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY
				&& ((xcb_property_notify_event_t *)event)->atom == state;
		free(event);
	}
	mask = 0;
	xcb_change_window_attributes(conn, win, XCB_CW_EVENT_MASK, &mask);
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

/* One line from stdin: activate, urgent on|off, or title TEXT */
static void
command(xcb_connection_t *conn, xcb_window_t root, xcb_window_t win, const char *line)
{
	xcb_icccm_wm_hints_t hints = {0};
	if (!strcmp(line, "activate")) {
		activate(conn, root, win);
	} else if (!strcmp(line, "urgent on") || !strcmp(line, "urgent off")) {
		xcb_icccm_wm_hints_set_urgency(&hints);
		if (!strcmp(line, "urgent off"))
			hints.flags &= ~(uint32_t)XCB_ICCCM_WM_HINT_X_URGENCY;
		xcb_icccm_set_wm_hints(conn, win, &hints);
		xcb_flush(conn);
	} else if (!strncmp(line, "configure ", 10)) {
		int x, y, w, h;
		uint32_t values[4];
		if (sscanf(line + 10, "%d %d %d %d", &x, &y, &w, &h) == 4) {
			values[0] = (uint32_t)x;
			values[1] = (uint32_t)y;
			values[2] = (uint32_t)w;
			values[3] = (uint32_t)h;
			xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
					| XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
			xcb_flush(conn);
		}
	} else if (!strncmp(line, "title ", 6)) {
		xcb_icccm_set_wm_name(conn, win, XCB_ATOM_STRING, 8,
				(uint32_t)strlen(line + 6), line + 6);
		xcb_flush(conn);
	}
}

/* With REPORT, what an event says of the window: its size, as it changes,
 * or a button pressed in it */
static void
report(xcb_generic_event_t *event, xcb_window_t win)
{
	static uint16_t width, height;
	xcb_configure_notify_event_t *configure = (xcb_configure_notify_event_t *)event;
	xcb_button_press_event_t *press = (xcb_button_press_event_t *)event;
	xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)event;
	uint8_t type = event->response_type & ~0x80;
	if (type == XCB_CONFIGURE_NOTIFY && configure->window == win
			&& (configure->width != width || configure->height != height)) {
		width = configure->width;
		height = configure->height;
		printf("size %u %u\n", (unsigned)width, (unsigned)height);
	} else if (type == XCB_BUTTON_PRESS && press->event == win) {
		printf("button %d %d\n", press->event_x, press->event_y);
	} else if (type == XCB_MOTION_NOTIFY && motion->event == win) {
		printf("motion %d %d\n", motion->event_x, motion->event_y);
	}
	fflush(stdout);
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
	char buf[256], *line, *nl;
	size_t buflen = 0;
	ssize_t n;
	int reporting = getenv("REPORT") != NULL, fixed_width, fixed_height;
	const char *fixed = getenv("FIXED_SIZE");
	xcb_size_hints_t size_hints = {0};
	uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS
			| XCB_EVENT_MASK_BUTTON_MOTION;

	if (argc != 3)
		die("usage: x11client CLASS TITLE");
	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn))
		die("can't connect to the X server");
	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	wait_for_wm(conn, screen->root);
	if (reporting) {
		printf("screen %u %u\n", (unsigned)screen->width_in_pixels,
				(unsigned)screen->height_in_pixels);
		fflush(stdout);
	}
	win = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
			0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			screen->root_visual, XCB_CW_BACK_PIXEL, &screen->white_pixel);
	/* WM_CLASS is the instance, then the class, each NUL-terminated */
	len = (size_t)snprintf(class, sizeof(class), "%s%c%s", argv[1], '\0', argv[1]) + 1;
	xcb_icccm_set_wm_class(conn, win, (uint32_t)len, class);
	xcb_icccm_set_wm_name(conn, win, XCB_ATOM_STRING, 8, 7, "unnamed");
	if (fixed && sscanf(fixed, "%dx%d", &fixed_width, &fixed_height) == 2) {
		xcb_icccm_size_hints_set_min_size(&size_hints, fixed_width, fixed_height);
		xcb_icccm_size_hints_set_max_size(&size_hints, fixed_width, fixed_height);
		xcb_icccm_set_wm_normal_hints(conn, win, &size_hints);
	}
	sync_server(conn);
	/* A title change while unmapped */
	xcb_icccm_set_wm_name(conn, win, XCB_ATOM_STRING, 8,
			(uint32_t)strlen(argv[2]), argv[2]);
	if (getenv("FULLSCREEN")) {
		xcb_atom_t fullscreen = intern(conn, "_NET_WM_STATE_FULLSCREEN");
		wait_for_watch(conn, screen->root, win);
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, intern(conn, "_NET_WM_STATE"),
				XCB_ATOM_ATOM, 32, 1, &fullscreen);
	}
	/* After wait_for_watch, which sets the window's events for itself */
	if (reporting)
		xcb_change_window_attributes(conn, win, XCB_CW_EVENT_MASK, &mask);
	sync_server(conn);
	xcb_map_window(conn, win);
	xcb_flush(conn);
	fds[0] = (struct pollfd){.fd = xcb_get_file_descriptor(conn), .events = POLLIN};
	fds[1] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
	for (;;) {
		if (poll(fds, 2, -1) < 0)
			die("can't poll");
		while ((event = xcb_poll_for_event(conn))) {
			if (reporting)
				report(event, win);
			free(event);
		}
		if (xcb_connection_has_error(conn))
			return 0;
		if (fds[1].revents) {
			/* At the end of stdin, only the X server is left to read */
			if ((n = read(STDIN_FILENO, buf + buflen, sizeof(buf) - 1 - buflen)) <= 0) {
				fds[1].fd = -1;
				continue;
			}
			buflen += (size_t)n;
			buf[buflen] = '\0';
			/* Each whole line, in order: one read may bring several */
			for (line = buf; (nl = strchr(line, '\n')); line = nl + 1) {
				*nl = '\0';
				command(conn, screen->root, win, line);
			}
			buflen = strlen(line);
			memmove(buf, line, buflen + 1);
			if (buflen == sizeof(buf) - 1)
				buflen = 0; /* a line too long to be a command */
		}
	}
}

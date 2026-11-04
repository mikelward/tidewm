/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Capture sessions, for the IPC's capture event, tidewm's own: tide's SPEC
 * §12 shows a Sharing pill while the screen or a window is shared. A
 * session over ext-image-copy-capture starts when a client asks for one,
 * such as xdg-desktop-portal-wlr sharing a window or a monitor, or grim
 * taking a screenshot, and stops when the client ends it, goes, or its
 * window closes. A stream of just the pointer's cursor, which a client
 * may capture alongside an output, isn't a capture of its own.
 * wlr-screencopy's captures go unseen: wlroots says nothing of them.
 */

/* A session, and the window it captures: 0 for an output, whose source
 * wlroots keeps to itself, so which output isn't known */
typedef struct {
	struct wl_listener destroy;
	unsigned long con_id;
} CaptureSession;

static void capture_started(struct wl_listener *listener, void *data);
static struct wl_listener capture_new_session = {.notify = capture_started};

static void
capture_stopped(struct wl_listener *listener, void *data)
{
	CaptureSession *s = wl_container_of(listener, s, destroy);
	wl_list_remove(&s->destroy.link);
	ipc_event_capture("stop", s->con_id);
	free(s);
}

static void
capture_started(struct wl_listener *listener, void *data)
{
	struct wlr_ext_image_copy_capture_session_v1 *session = data;
	unsigned long con_id = 0;
	CaptureSession *s;
	Client *c;
	/* A window's source is the one capturerequest() made for it */
	wl_list_for_each(c, &clients, link) {
		if (c->image_capture_source && c->image_capture_source == session->source) {
			con_id = ipc_id(&c->ipc_id);
			break;
		}
	}
	/* Otherwise it's an output's, which can give its pointer's cursor, or
	 * that cursor's own, which can't */
	if (!con_id && !session->source->impl->get_pointer_cursor)
		return;
	s = ecalloc(1, sizeof(*s));
	s->con_id = con_id;
	s->destroy.notify = capture_stopped;
	wl_signal_add(&session->events.destroy, &s->destroy);
	ipc_event_capture("start", s->con_id);
}

static void
capture_init(struct wlr_ext_image_copy_capture_manager_v1 *manager)
{
	wl_signal_add(&manager->events.new_session, &capture_new_session);
}

static void
capture_finish(void)
{
	wl_list_remove(&capture_new_session.link);
}

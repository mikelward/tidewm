/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Touchpad gestures, over pointer-gestures: a pinch, swipe or hold goes to
 * the surface under the pointer, as in sway, so Chrome zooms on a pinch and
 * swipes back and forward. wlroots sends each to the client with pointer
 * focus, if it has asked for that kind. tidewm binds no gesture itself, as
 * sway's bindgesture does. A gesture is input, as a scroll is: the session
 * isn't idle, and a key down meanwhile isn't a tap.
 *
 * The IPC's gesture command, tidewm's own, makes one as if from a touchpad,
 * for a test, as the headless backend has no touchpad and wlr-virtual-pointer
 * no gestures.
 */

#include <wlr/types/wlr_pointer_gestures_v1.h>

static struct wlr_pointer_gestures_v1 *gestures;

/* What any gesture's start does besides being passed on */
static void
gesture_began(void)
{
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	/* Super with a gesture isn't Super tapped, as with a scroll */
	settings_tap_over();
}

static void
gesture_swipe_begin(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_begin_event *event = data;
	gesture_began();
	wlr_pointer_gestures_v1_send_swipe_begin(gestures, seat, event->time_msec,
			event->fingers);
}

static void
gesture_swipe_update(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_update_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wlr_pointer_gestures_v1_send_swipe_update(gestures, seat, event->time_msec,
			event->dx, event->dy);
}

static void
gesture_swipe_end(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_end_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wlr_pointer_gestures_v1_send_swipe_end(gestures, seat, event->time_msec,
			event->cancelled);
}

static void
gesture_pinch_begin(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_begin_event *event = data;
	gesture_began();
	wlr_pointer_gestures_v1_send_pinch_begin(gestures, seat, event->time_msec,
			event->fingers);
}

static void
gesture_pinch_update(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_update_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wlr_pointer_gestures_v1_send_pinch_update(gestures, seat, event->time_msec,
			event->dx, event->dy, event->scale, event->rotation);
}

static void
gesture_pinch_end(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_end_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wlr_pointer_gestures_v1_send_pinch_end(gestures, seat, event->time_msec,
			event->cancelled);
}

static void
gesture_hold_begin(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_hold_begin_event *event = data;
	gesture_began();
	wlr_pointer_gestures_v1_send_hold_begin(gestures, seat, event->time_msec,
			event->fingers);
}

static void
gesture_hold_end(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_hold_end_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wlr_pointer_gestures_v1_send_hold_end(gestures, seat, event->time_msec,
			event->cancelled);
}

static struct wl_listener gesture_listeners[] = {
	{.notify = gesture_swipe_begin}, {.notify = gesture_swipe_update},
	{.notify = gesture_swipe_end}, {.notify = gesture_pinch_begin},
	{.notify = gesture_pinch_update}, {.notify = gesture_pinch_end},
	{.notify = gesture_hold_begin}, {.notify = gesture_hold_end},
};

/* After the cursor is made: its events carry every pointer's gestures */
static void
gesture_init(void)
{
	struct wl_signal *signals[] = {
		&cursor->events.swipe_begin, &cursor->events.swipe_update,
		&cursor->events.swipe_end, &cursor->events.pinch_begin,
		&cursor->events.pinch_update, &cursor->events.pinch_end,
		&cursor->events.hold_begin, &cursor->events.hold_end,
	};
	size_t i;
	gestures = wlr_pointer_gestures_v1_create(dpy);
	for (i = 0; i < LENGTH(gesture_listeners); i++)
		wl_signal_add(signals[i], &gesture_listeners[i]);
}

static void
gesture_finish(void)
{
	size_t i;
	for (i = 0; i < LENGTH(gesture_listeners); i++)
		wl_list_remove(&gesture_listeners[i].link);
}

/* The gesture command: a whole gesture of kind ("swipe", "pinch" or
 * "hold") with fingers fingers, on the cursor's events as a touchpad's
 * come. A swipe moves 10 right, a pinch moves 10 right and zooms in to
 * twice the size, turning 90 degrees. Returns 0 for an unknown kind. */
static int
gesture_run(const char *kind, uint32_t fingers)
{
	struct timespec now;
	uint32_t time;
	clock_gettime(CLOCK_MONOTONIC, &now);
	time = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	if (!strcmp(kind, "swipe")) {
		struct wlr_pointer_swipe_begin_event begin = {.time_msec = time, .fingers = fingers};
		struct wlr_pointer_swipe_update_event update = {.time_msec = time,
				.fingers = fingers, .dx = 10};
		struct wlr_pointer_swipe_end_event end = {.time_msec = time};
		wl_signal_emit_mutable(&cursor->events.swipe_begin, &begin);
		wl_signal_emit_mutable(&cursor->events.swipe_update, &update);
		wl_signal_emit_mutable(&cursor->events.swipe_end, &end);
	} else if (!strcmp(kind, "pinch")) {
		struct wlr_pointer_pinch_begin_event begin = {.time_msec = time, .fingers = fingers};
		struct wlr_pointer_pinch_update_event update = {.time_msec = time,
				.fingers = fingers, .dx = 10, .scale = 2, .rotation = 90};
		struct wlr_pointer_pinch_end_event end = {.time_msec = time};
		wl_signal_emit_mutable(&cursor->events.pinch_begin, &begin);
		wl_signal_emit_mutable(&cursor->events.pinch_update, &update);
		wl_signal_emit_mutable(&cursor->events.pinch_end, &end);
	} else if (!strcmp(kind, "hold")) {
		struct wlr_pointer_hold_begin_event begin = {.time_msec = time, .fingers = fingers};
		struct wlr_pointer_hold_end_event end = {.time_msec = time};
		wl_signal_emit_mutable(&cursor->events.hold_begin, &begin);
		wl_signal_emit_mutable(&cursor->events.hold_end, &end);
	} else {
		return 0;
	}
	return 1;
}

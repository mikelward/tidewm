/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Windows kept open, tidewm's own, for tide's pinned windows (its SPEC
 * §6.5): a shell's keep_open (ipc.h) has kill leave a window open, saying
 * so with a close_refused event, and the config file's --kept-open
 * bindings (settings.h) count only while you're typing in such a window.
 * It's kept open on the workspace it was on when the shell said so, which
 * its IPC id names, as they're never used again: on any other, however it
 * got there, by a command, a key, its parent, a switch it's sticky for or
 * another output, it isn't. Minimized, it's still on that workspace, so
 * it's still kept open.
 */

static void ipc_event_window(Client *c, const char *change);
static unsigned long ipc_id(unsigned long *id);

/* Whether c is kept open, where it is now */
static int
keptopen(Client *c)
{
	return c && c->keptopen_ws && c->ws
		&& ipc_id(&c->ws->ipc_id) == c->keptopen_ws;
}

/* Keeps c open on the workspace it's on, or no longer */
static void
setkeptopen(Client *c, int kept)
{
	c->keptopen_ws = kept && c->ws ? ipc_id(&c->ws->ipc_id) : 0;
}

/* Asks c to close, as kill does, unless it's kept open: then a
 * close_refused event says so instead, for a shell to tell you why.
 * Returns whether it asked. */
static int
closeclient(Client *c)
{
	if (keptopen(c)) {
		ipc_event_window(c, "close_refused");
		return 0;
	}
	client_send_close(c);
	return 1;
}

/* Whether the keyboard is in a window kept open, or a popup of one, so its
 * --kept-open bindings count: not with a launcher or a lock screen in. */
static int
keyboard_kept_open(void)
{
	Client *c = NULL;
	if (toplevel_from_wlr_surface(seat->keyboard_state.focused_surface, &c, NULL) < 0)
		return 0;
	return keptopen(c);
}

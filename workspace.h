/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * tidewm's workspaces, in place of dwl's tags (tide's SPEC.md, section 6.5).
 * A workspace has a name, and spans every monitor: each monitor shows its
 * own part of it, the windows whose monitor it is. One workspace is
 * current, on every monitor at once. A workspace is made the first time
 * it's named, and goes once it's empty and not current, unless it's
 * persistent. dwl.c includes this file, like client.h.
 */
#include <ctype.h>

static unsigned long ipc_id(unsigned long *id);
static void ipc_event_window(Client *c, const char *change);
static void ipc_event_workspace(const char *change, Workspace *cur, Workspace *old);

static struct wl_list workspaces = {&workspaces, &workspaces}; /* in the order made */
static Workspace *curws;
static Workspace *prevws; /* for back_and_forth */
static void ws_view(Workspace *ws);
static void ws_view_to(Workspace *ws, Client *c);
/* layout.h, included after this file */
static void lt_view(Workspace *old, Workspace *ws);
static void lt_announce(void);
static void lt_forget(Workspace *ws);

/* ext-workspace: one group, on every output, as a workspace spans them,
 * holding every workspace. A client may ask to activate one. */
static struct wlr_ext_workspace_manager_v1 *ws_manager;
static struct wlr_ext_workspace_group_handle_v1 *ws_group;

static void
ws_protocol_commit(struct wl_listener *listener, void *data)
{
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		/* Only activate is offered; a destroyed workspace is NULL */
		if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE && req->activate.workspace)
			ws_view(req->activate.workspace->data);
	}
}

static struct wl_listener ws_protocol_commit_listener = {.notify = ws_protocol_commit};

static void
ws_protocol_init(void)
{
	ws_manager = wlr_ext_workspace_manager_v1_create(dpy, 1);
	ws_group = wlr_ext_workspace_group_handle_v1_create(ws_manager, 0);
	wl_signal_add(&ws_manager->events.commit, &ws_protocol_commit_listener);
}

/* wlroots wants every listener gone before the display goes */
static void
ws_protocol_finish(void)
{
	wl_list_remove(&ws_protocol_commit_listener.link);
}

static void
ws_set_active(Workspace *ws, int active)
{
	if (ws && ws->ext)
		wlr_ext_workspace_handle_v1_set_active(ws->ext, active);
}

static Workspace *
ws_find(const char *name)
{
	Workspace *ws;
	wl_list_for_each(ws, &workspaces, link)
		if (!strcmp(ws->name, name))
			return ws;
	return NULL;
}

/* A name is one line of text, not empty; tide's shell sets tighter rules. */
static int
ws_name_ok(const char *name)
{
	const unsigned char *p;
	if (!*name || strlen(name) > 255)
		return 0;
	for (p = (const unsigned char *)name; *p; p++)
		if (*p < 0x20 || *p == 0x7f)
			return 0;
	return 1;
}

static Workspace *
ws_get(const char *name)
{
	Workspace *ws = ws_find(name);
	if (ws || !ws_name_ok(name))
		return ws;
	ws = ecalloc(1, sizeof(*ws));
	ws->name = strdup(name);
	if (!ws->name)
		die("out of memory naming workspace");
	wl_list_insert(workspaces.prev, &ws->link);
	if (ws_manager) {
		/* Its id lasts for the workspace's life, through a rename */
		char id[32];
		snprintf(id, sizeof(id), "tidewm-%lu", ipc_id(&ws->ipc_id));
		ws->ext = wlr_ext_workspace_handle_v1_create(ws_manager, id,
				EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		ws->ext->data = ws;
		wlr_ext_workspace_handle_v1_set_group(ws->ext, ws_group);
		wlr_ext_workspace_handle_v1_set_name(ws->ext, ws->name);
	}
	ipc_event_workspace("init", ws, NULL);
	return ws;
}

static int
ws_has_windows(Workspace *ws)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->ws == ws)
			return 1;
	return 0;
}

static int
ws_urgent(Workspace *ws)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->ws == ws && c->isurgent)
			return 1;
	return 0;
}

/* Says when ws's urgency, whether any window on it is urgent, changes. */
static void
ws_update_urgency(Workspace *ws)
{
	int urgent;
	if (!ws || (urgent = ws_urgent(ws)) == ws->urgent)
		return;
	ws->urgent = urgent;
	if (ws->ext)
		wlr_ext_workspace_handle_v1_set_urgent(ws->ext, urgent);
	ipc_event_workspace("urgent", ws, NULL);
}

/* Removes ws if nothing keeps it: it's current, persistent, or has a
 * window. Returns whether it went. */
static int
ws_prune(Workspace *ws)
{
	if (!ws || ws == curws || ws->persistent || ws_has_windows(ws))
		return 0;
	ipc_event_workspace("empty", ws, NULL);
	if (prevws == ws)
		prevws = NULL;
	if (ws->ext)
		wlr_ext_workspace_handle_v1_destroy(ws->ext);
	/* Its layouts go with it: one made again starts from the defaults */
	lt_forget(ws);
	wl_list_remove(&ws->link);
	free(ws->name);
	free(ws);
	return 1;
}

static void
ws_arrange_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		arrange(m);
}

/* Shows ws on every monitor. Focus stays on the monitor it was on. */
static void
ws_view(Workspace *ws)
{
	ws_view_to(ws, NULL);
}

/* Shows ws on every monitor and focuses c there, a window on it, or,
 * without one, the window to go to on the monitor focus was on: going
 * straight to c, so no other window the focus guard holds is focused on
 * the way and loses its mark */
static void
ws_view_to(Workspace *ws, Client *c)
{
	Workspace *old = curws;
	Client *w;
	if (!ws || ws == curws)
		return;
	prevws = old;
	curws = ws;
	/* A sticky floating window, such as picture-in-picture, comes along,
	 * with its dialogs, and any urgency with it */
	wl_list_for_each(w, &clients, link) {
		if (w->ws == old && stickybelow(w, NULL)) {
			w->ws = ws;
			ipc_event_window(w, "move");
		}
	}
	ws_update_urgency(old);
	ws_update_urgency(ws);
	ws_set_active(old, 0);
	ws_set_active(ws, 1);
	/* Each monitor keeps old's layout and takes ws's */
	lt_view(old, ws);
	ws_arrange_all();
	lt_announce();
	if (c && c->mon)
		selmon = c->mon;
	focusclient(c ? c : focusgo(selmon), NULL, 1);
	printstatus();
	ipc_event_workspace("focus", ws, old);
	ws_prune(old);
}

/* The number a workspace's name is, or -1 for a name that isn't one, as
 * the IPC's num has it */
static int
ws_number(const Workspace *ws)
{
	const char *p = ws->name;
	while (isdigit((unsigned char)*p))
		p++;
	return !*p && p - ws->name < 10 ? atoi(ws->name) : -1;
}

/* Whether a comes before b in sway's order: numbered workspaces by their
 * numbers, then the rest in the order made */
static int
ws_before(const Workspace *a, const Workspace *b)
{
	int na = ws_number(a), nb = ws_number(b);
	return na >= 0 && (nb < 0 || na < nb);
}

/* The workspace step along from from in sway's order, going round, as
 * sway's workspace next and prev go on one output; one spans them all.
 * from itself, with no other. */
static Workspace *
ws_step(Workspace *from, int step)
{
	Workspace *ws, **order;
	size_t n = 0, i, j, at = 0;
	wl_list_for_each(ws, &workspaces, link)
		n++;
	if (!n || !from)
		return from;
	order = ecalloc(n, sizeof(*order));
	n = 0;
	/* Sorted as they come, an equal one staying after, so it's stable */
	wl_list_for_each(ws, &workspaces, link) {
		for (i = n; i > 0 && ws_before(ws, order[i - 1]); i--)
			order[i] = order[i - 1];
		order[i] = ws;
		n++;
	}
	for (j = 0; j < n; j++)
		if (order[j] == from)
			at = j;
	ws = order[(at + n + (size_t)(step % (int)n + (int)n)) % n];
	free(order);
	return ws;
}

/* Moves c to the end of the client list, so it comes last in its part's
 * stack, as a new window does: one moved to another workspace or monitor,
 * or tiled again after floating, counts as new. Nothing for a window not
 * in the list, before it maps or after it unmaps. */
static void
ws_to_end(Client *c)
{
	if (!c->link.next)
		return;
	wl_list_remove(&c->link);
	wl_list_insert(clients.prev, &c->link);
}

/* Moves c into ws's part on c's own monitor. */
static void
ws_move(Client *c, Workspace *ws)
{
	Workspace *old = c->ws;
	if (!ws || ws == old)
		return;
	c->ws = ws;
	ws_to_end(c);
	dialogsfollow(c, old); /* its dialogs go with it */
	if (c->mon)
		arrange(c->mon);
	focusclient(focustop(selmon), NULL, 1);
	printstatus();
	ipc_event_window(c, "move");
	ws_update_urgency(ws);
	if (!ws_prune(old))
		ws_update_urgency(old);
}

static void
ws_rename(Workspace *ws, const char *name)
{
	char *copy;
	if (!(copy = strdup(name)))
		die("out of memory renaming workspace");
	free(ws->name);
	ws->name = copy;
	if (ws->ext)
		wlr_ext_workspace_handle_v1_set_name(ws->ext, ws->name);
	printstatus();
	ipc_event_workspace("rename", ws, NULL);
}

static void
ws_set_persistent(Workspace *ws, int persistent)
{
	ws->persistent = persistent;
	ws_prune(ws);
}

/* Key bindings: arg->v is the workspace's name. */
void
viewws(const Arg *arg)
{
	ws_view(ws_get(arg->v));
}

void
viewprevws(const Arg *arg)
{
	ws_view(prevws);
}

void
movews(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		ws_move(sel, ws_get(arg->v));
}

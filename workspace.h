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

static void ipc_event_window(Client *c, const char *change);
static void ipc_event_workspace(const char *change, Workspace *cur, Workspace *old);

static struct wl_list workspaces = {&workspaces, &workspaces}; /* in the order made */
static Workspace *curws;
static Workspace *prevws; /* for back_and_forth */

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
	Workspace *old = curws;
	if (!ws || ws == curws)
		return;
	prevws = old;
	curws = ws;
	ws_arrange_all();
	focusclient(focustop(selmon), NULL, 1);
	printstatus();
	ipc_event_workspace("focus", ws, old);
	ws_prune(old);
}

/* Moves c into ws's part on c's own monitor. */
static void
ws_move(Client *c, Workspace *ws)
{
	Workspace *old = c->ws;
	if (!ws || ws == old)
		return;
	c->ws = ws;
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

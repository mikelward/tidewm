/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The focus guard, tide's SPEC §14, as tide's hypr/tide/focus.lua has it
 * on Hyprland: nothing takes the keyboard unless you asked for it. With
 * the guard enabled (config.h's focus_guard, or the config file's
 * focus_guard line), a new window takes focus when:
 *
 *   - a grant names its app, or its process descends from the one a grant
 *     names: `tide launch`, a notification's click, or the terminal's
 *     shell, over the grant command;
 *   - it's from the app you're in: a dialog, a second window;
 *   - it's a portal's dialog, such as a file chooser, while a window has
 *     focus, since the portal's process isn't the app's;
 *   - it's a polkit agent's prompt, right after a key press.
 *
 * Any other opens in place, held from focus and marked urgent, until you
 * go to it. A window with no app ID and no title is a status popup, such
 * as Chrome's own notification: it's held from focus, but not marked.
 *
 * A grant is one-shot: the first window that matches uses it, and so does
 * an activation of a window the app already had. Anything you do after it
 * cancels its focus: a key press, a click, or focus moving to another
 * window by any means, a workspace switch among them. It lapses after
 * grant_seconds. A notification's click makes one that, lapsing unused,
 * brings up the app's most recent window.
 *
 * A canceled grant still says where its window opens, as Hyprland's
 * initial_workspace_tracking does (tide's SPEC §14.2): if you've left the
 * workspace it was made on, its window opens there, or on its --workspace,
 * held from focus and marked urgent, and that workspace with it. A
 * wildcard one does only for a window from its --pid's processes, and a
 * notification's not at all.
 *
 * Activation, xdg-activation's or X11's _NET_ACTIVE_WINDOW, marks a window
 * urgent rather than focusing it, unless a grant names it, with the guard
 * or without it.
 */

/* A one-shot grant, as tide's focus guard keeps one */
typedef struct {
	struct wl_list link;
	char **apps; /* normalized ids; "*" alone stands for any app */
	size_t napps;
	pid_t pid; /* the process that asked, whose descendants may use it; 0 for none */
	unsigned long key; /* the granting process's pid, for grant extend; 0 for none */
	char *workspace; /* where its window goes, focus following; NULL for where it opens */
	int or_recent; /* lapsing unused, it brings up the app's most recent window */
	uint64_t at; /* when, in monotonic microseconds */
	unsigned long origin; /* the IPC id of the workspace current as it was made */
	char *origin_name; /* that workspace's name then, to make it again; NULL for none */
	int canceled; /* by you moving on: it gives no focus, only a place */
} FgGrant;

static struct wl_list fg_grants = {&fg_grants, &fg_grants};
static unsigned long fg_active; /* the IPC id of the window focused last; 0 for none */
static unsigned long fg_active_ws; /* the current workspace's IPC id as it was focused */
static pid_t fg_active_pid;
static char *fg_active_app; /* its app, normalized */
static uint64_t fg_last_key; /* when you last pressed a key; 0 for never */
/* Your last key press or click: when, and the window it went to, by IPC
 * id, 0 for none (a launcher's, say), for the shell's polkit agent */
static struct { uint64_t at; int button; unsigned long window; } fg_last_input;
static unsigned long fg_focus_count; /* focuses so far, for Client.focus_seq */
static struct wl_event_source *fg_timer; /* for or-recent grants' lapses */
static int fg_focusing; /* focus that isn't you moving on: the guard's own, or an output change's */
static Client *fg_mapping; /* the window mapping now: focus moving meanwhile isn't you */
static Client *fg_granted; /* a new window that used a grant */
static Client *fg_closing; /* the window with focus that's closing, while it goes */
static unsigned long fg_deferred; /* the IPC id of a granted window whose focus waits */
/* A new window a canceled grant sends to the workspace it was launched
 * from, or to its --workspace: by IPC id, 0 for none, else by name */
static struct { Client *c; unsigned long id; char *name; } fg_place;

static uint64_t
fg_now(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static int
fg_enabled(void)
{
	return settings && settings->focus_guard >= 0 ? settings->focus_guard : focus_guard;
}

/* How long a grant holds, and how recent a key press lets a polkit prompt
 * in, in microseconds */
static uint64_t
fg_grant_us(void)
{
	return (uint64_t)(1e6 * (settings && settings->grant_seconds > 0
			? settings->grant_seconds : grant_seconds));
}

static uint64_t
fg_prompt_us(void)
{
	return (uint64_t)(1e6 * (settings && settings->prompt_seconds > 0
			? settings->prompt_seconds : prompt_seconds));
}

/* An app id as a grant or a window might spell it, normalized: lowercase
 * and without a .desktop suffix. NULL for an empty one. */
static char *
fg_normalize(const char *id)
{
	char *s, *p;
	size_t n;
	if (!id || !*id)
		return NULL;
	if (!(s = strdup(id)))
		die("out of memory normalizing an app id");
	for (p = s; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	if ((n = strlen(s)) >= 8 && !strcmp(s + n - 8, ".desktop"))
		s[n - 8] = '\0';
	if (!*s) {
		free(s);
		return NULL;
	}
	return s;
}

/* Whether two normalized ids name the same app. Qualified ids must match
 * in full, so org.example.chat and com.example.chat stay apart; a bare
 * name matches the last part of a qualified one, so nautilus, a command's
 * name, matches org.gnome.nautilus. */
static int
fg_same_id(const char *a, const char *b)
{
	const char *bare, *qualified, *last;
	if (!a || !b)
		return 0;
	if (!strcmp(a, b))
		return 1;
	if (!strchr(a, '.') == !strchr(b, '.'))
		return 0;
	bare = strchr(a, '.') ? b : a;
	qualified = bare == a ? b : a;
	last = strrchr(qualified, '.') + 1;
	return !strcmp(last, bare);
}

/* A window's app ID or X11 class, and title, as it gave them: NULL for
 * none, where client.h's say "broken" */
static const char *
fg_raw_appid(Client *c)
{
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->class;
#endif
	return c->surface.xdg->toplevel->app_id;
}

static const char *
fg_raw_title(Client *c)
{
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->title;
#endif
	return c->surface.xdg->toplevel->title;
}

/* A status popup, such as Chrome's own notification: a window with
 * neither an app ID nor a title. It floats, and the guard holds it. */
static int
fg_nameless(Client *c)
{
	const char *appid = fg_raw_appid(c), *title = fg_raw_title(c);
	return (!appid || !*appid) && (!title || !*title);
}

static void
fg_free(FgGrant *g)
{
	size_t i;
	wl_list_remove(&g->link);
	for (i = 0; i < g->napps; i++)
		free(g->apps[i]);
	free(g->apps);
	free(g->workspace);
	free(g->origin_name);
	free(g);
}

/* ws has been renamed: a grant made on it keeps its new name, to make it
 * again by if it's pruned before the grant's window opens */
static void
fg_renamed(Workspace *ws)
{
	FgGrant *g;
	char *copy;
	wl_list_for_each(g, &fg_grants, link) {
		if (!g->origin_name || g->origin != ipc_id(&ws->ipc_id))
			continue;
		if (!(copy = strdup(ws->name)))
			die("out of memory renaming workspace");
		free(g->origin_name);
		g->origin_name = copy;
	}
}

/* Whether g has lapsed: one that brings up the app's window waits for
 * fg_lapse to do it, but no window may use it meanwhile */
static int
fg_lapsed(const FgGrant *g, uint64_t now)
{
	return now - g->at > fg_grant_us();
}

/* Drops the grants that have lapsed, but the or-recent ones, which
 * fg_lapse handles */
static void
fg_expire(void)
{
	FgGrant *g, *tmp;
	uint64_t now = fg_now();
	wl_list_for_each_safe(g, tmp, &fg_grants, link)
		if (!g->or_recent && fg_lapsed(g, now))
			fg_free(g);
}

static int
fg_names(const FgGrant *g, const char *app)
{
	size_t i;
	for (i = 0; i < g->napps; i++)
		if (fg_same_id(g->apps[i], app))
			return 1;
	return 0;
}

static int
fg_wildcard(const FgGrant *g)
{
	return g->napps == 1 && !strcmp(g->apps[0], "*");
}

/* Anything you do after a grant cancels every grant: a slow app mustn't
 * take focus from whatever you moved on to. One that can still place its
 * window stays, canceled, until it's used or lapses: not a notification's,
 * nor a wildcard that can't tell its window from any other. */
static void
fg_cancel(void)
{
	FgGrant *g, *tmp;
	wl_list_for_each_safe(g, tmp, &fg_grants, link) {
		if (g->origin_name && (!fg_wildcard(g) || g->pid))
			g->canceled = 1;
		else
			fg_free(g);
	}
}

/* Drops every grant, canceled or not */
static void
fg_drop(void)
{
	FgGrant *g, *tmp;
	wl_list_for_each_safe(g, tmp, &fg_grants, link)
		fg_free(g);
}

/* The grant for c's app, if one holds, canceled or not as asked, taken
 * off the list for the caller to free: one naming it before a wildcard,
 * which any app's window uses, if it isn't canceled */
static FgGrant *
fg_take(Client *c, int canceled)
{
	FgGrant *g;
	char *app = fg_normalize(fg_raw_appid(c));
	uint64_t now = fg_now();
	int wildcard;
	fg_expire();
	for (wildcard = 0; wildcard < 2 - canceled; wildcard++) {
		wl_list_for_each(g, &fg_grants, link) {
			if (!fg_lapsed(g, now) && g->canceled == canceled && fg_wildcard(g) == wildcard
					&& (wildcard || fg_names(g, app))) {
				free(app);
				wl_list_remove(&g->link);
				wl_list_init(&g->link);
				return g;
			}
		}
	}
	free(app);
	return NULL;
}

/* A process's parent, from /proc/PID/stat, or -1 if it can't be read: a
 * process that has exited has no parent to walk. The command name in the
 * second field may hold spaces and parentheses, so the parent comes after
 * the last ')'. */
static pid_t
fg_parent(pid_t pid)
{
	char path[64], line[1024], *close;
	FILE *f;
	int ppid;
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	if (!(f = fopen(path, "r")))
		return -1;
	close = fgets(line, sizeof(line), f) ? strrchr(line, ')') : NULL;
	fclose(f);
	if (!close || sscanf(close + 1, " %*s %d", &ppid) != 1)
		return -1;
	return ppid;
}

/* Whether pid is ancestor or descends from it, walking at most 64
 * parents. A command its shell exec'd carries the shell's own pid. */
static int
fg_descends(pid_t pid, pid_t ancestor)
{
	int i;
	for (i = 0; i < 64 && pid > 1; i++) {
		if (pid == ancestor)
			return 1;
		pid = fg_parent(pid);
	}
	return 0;
}

/* The process-ancestry fallback, for a command whose name matches no
 * window's app, such as a script that opens one: a grant naming the
 * process that asked (the shell a terminal command ran in), for a window
 * whose process descends from it. A launch grant names none, so it never
 * matches this way. */
static FgGrant *
fg_take_by_ancestry(Client *c, int canceled)
{
	FgGrant *g;
	pid_t pid = ipc_client_pid(c);
	fg_expire();
	if (pid <= 0)
		return NULL;
	wl_list_for_each(g, &fg_grants, link) {
		if (g->pid && g->canceled == canceled && fg_descends(pid, g->pid)) {
			wl_list_remove(&g->link);
			wl_list_init(&g->link);
			return g;
		}
	}
	return NULL;
}

static int
fg_class_in(Client *c, const char *const *classes, size_t n)
{
	char *app = fg_normalize(fg_raw_appid(c)), *class;
	size_t i;
	int in = 0;
	for (i = 0; i < n && !in; i++) {
		class = fg_normalize(classes[i]);
		in = fg_same_id(class, app);
		free(class);
	}
	free(app);
	return in;
}

static int
fg_same_app_as_active(Client *c)
{
	pid_t pid = ipc_client_pid(c);
	char *app;
	int same;
	if (!fg_active)
		return 0;
	if (pid > 0 && pid == fg_active_pid)
		return 1;
	app = fg_normalize(fg_raw_appid(c));
	same = fg_same_id(app, fg_active_app);
	free(app);
	return same;
}

/* Whether a polkit prompt opening now follows a key press, which is you
 * running pkexec or pressing a button that asks: the keyboard half of
 * SPEC §14.1's rule */
static int
fg_prompt_may_focus(void)
{
	return fg_last_key && fg_now() - fg_last_key <= fg_prompt_us();
}

/* After the guard has given c focus, as a grant asked: if a launcher, or
 * the lock, holding the keyboard puts it off, c's focus when it goes is
 * still the grant's */
static void
fg_defer(Client *c)
{
	if (c != focused_client && c == focustop(selmon))
		fg_deferred = ipc_id(&c->ipc_id);
}

/* Focuses c as the guard, which isn't you moving on, first taking it to
 * the workspace called workspace, made if it's new, if one's given */
static void
fg_focus(Client *c, const char *workspace)
{
	Workspace *ws;
	fg_focusing++;
	if (workspace && (ws = ws_get(workspace)))
		ws_move(c, ws);
	if (c->ws != curws)
		ws_view_to(c->ws, c);
	if (c->mon)
		selmon = c->mon;
	if (locked) {
		/* focusclient does nothing while the lock holds the keyboard: c
		 * goes atop the focus stack, held no more, to have it as it goes */
		c->held = FgFree;
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
	}
	focusclient(c, NULL, 1);
	/* Under the lock, focusclient says nothing of the output */
	ipc_output_focus();
	fg_focusing--;
	fg_defer(c);
}

/* The window of any of apps focused most recently, or, with none ever
 * focused, the first of them; NULL for none. A site open in two Chrome
 * profiles is two apps, and its notification brings up whichever was used
 * last. */
static Client *
fg_most_recent(char **apps, size_t napps)
{
	Client *c, *best = NULL;
	char *app;
	size_t i;
	int mine;
	wl_list_for_each(c, &clients, link) {
		app = fg_normalize(fg_raw_appid(c));
		for (i = 0, mine = 0; i < napps && !mine; i++)
			mine = fg_same_id(apps[i], app);
		free(app);
		if (mine && (!best || c->focus_seq > best->focus_seq))
			best = c;
	}
	return best;
}

/* Brings up the most recent window of apps, as the guard, or says there's
 * none on stderr, naming them */
static int
fg_bring_up(char **apps, size_t napps, const char *why)
{
	Client *c = fg_most_recent(apps, napps);
	size_t i;
	if (c) {
		fg_focus(c, NULL);
		return 1;
	}
	fprintf(stderr, "tidewm: %s: no window of", why);
	for (i = 0; i < napps; i++)
		fprintf(stderr, " %s", apps[i]);
	fprintf(stderr, " to bring up\n");
	return 0;
}

static int fg_lapse(void *data);

/* Sets the timer for the next or-recent grant to lapse, or stops it */
static void
fg_arm(void)
{
	FgGrant *g;
	uint64_t now = fg_now(), next = 0, due;
	wl_list_for_each(g, &fg_grants, link) {
		due = g->at + fg_grant_us();
		if (g->or_recent && (!next || due < next))
			next = due;
	}
	if (!fg_timer && next && !(fg_timer = wl_event_loop_add_timer(event_loop, fg_lapse, NULL))) {
		/* Without its timer, a notification's grant just lapses */
		fprintf(stderr, "tidewm: can't make the focus guard's timer\n");
		return;
	}
	if (fg_timer)
		wl_event_source_timer_update(fg_timer, !next ? 0
				: next <= now ? 1 : (int)MIN((next - now + 999) / 1000, INT_MAX));
}

/* A notification's grant ran out unused: the app sent no activation and
 * opened no window, and you did nothing else meanwhile. Its most recent
 * window comes up instead, wherever it is. Each that's due, in the order
 * they were made. */
static int
fg_lapse(void *data)
{
	FgGrant *g, *due;
	uint64_t now = fg_now();
	for (;;) {
		due = NULL;
		wl_list_for_each(g, &fg_grants, link) {
			if (g->or_recent && fg_lapsed(g, now)) {
				due = g;
				break;
			}
		}
		if (!due)
			break;
		wl_list_remove(&due->link);
		wl_list_init(&due->link);
		fg_bring_up(due->apps, due->napps, "a notification's grant lapsed");
		fg_free(due);
	}
	fg_arm();
	return 0;
}

/* The focus recent command, for a notification center's entry whose
 * notification is gone: the most recent window of any of the apps, at
 * once, which is you moving on from anything else. Fills err and returns
 * 0 if there's none, or on an error in args, which sets *parse. */
static int
fg_focus_recent(char *args, char *err, size_t errsize, int *parse)
{
	char *word, **apps = NULL;
	size_t napps = 0, i;
	int ok = 0;
	*parse = 1;
	while (*args) {
		if (!(word = settings_arg(&args)) || !(word = fg_normalize(word)) || !strcmp(word, "*")) {
			free(word);
			snprintf(err, errsize, "focus recent: expected app ids");
			goto out;
		}
		apps = realloc(apps, (napps + 1) * sizeof(*apps));
		if (!apps)
			die("out of memory focusing a window");
		apps[napps++] = word;
	}
	if (!napps) {
		snprintf(err, errsize, "focus recent needs an app id");
		goto out;
	}
	*parse = 0;
	if (!fg_most_recent(apps, napps)) {
		snprintf(err, errsize, "No window of %.64s to bring up", apps[0]);
		goto out;
	}
	fg_cancel();
	ok = fg_bring_up(apps, napps, "focus recent");
out:
	for (i = 0; i < napps; i++)
		free(apps[i]);
	free(apps);
	return ok;
}

/* A workspace by its IPC id, or, with none that has it, by its name: one
 * renamed since keeps its id, and one gone since, as you left it empty,
 * may have been made again. NULL for none. */
static Workspace *
fg_find(unsigned long id, const char *name)
{
	Workspace *ws;
	wl_list_for_each(ws, &workspaces, link)
		if (id && ws->ipc_id == id)
			return ws;
	return name ? ws_find(name) : NULL;
}

/* Aims c, a new window, where a canceled grant g says, if you've left the
 * workspace g was made on: on its --workspace, or back on that one, made
 * again if it's gone. Returns whether it did; applyrules puts it there,
 * by fg_placed. */
static int
fg_aim(Client *c, const FgGrant *g)
{
	Workspace *target;
	unsigned long id = g->workspace ? 0 : g->origin;
	const char *name = g->workspace ? g->workspace : g->origin_name;
	if (!curws || fg_find(g->origin, g->origin_name) == curws)
		return 0;
	if ((target = fg_find(id, name)) == curws)
		return 0;
	free(fg_place.name);
	if (!(fg_place.name = strdup(name)))
		die("out of memory placing a window");
	fg_place.c = c;
	fg_place.id = target ? ipc_id(&target->ipc_id) : 0;
	return 1;
}

/* The workspace fg_aim chose for c, made if it's gone, or NULL for the
 * current one: for the rules, whose own workspace comes first */
static Workspace *
fg_placed(Client *c)
{
	Workspace *ws;
	if (c != fg_place.c)
		return NULL;
	return (ws = fg_find(fg_place.id, NULL)) ? ws : ws_get(fg_place.name);
}

static void
fg_unaim(void)
{
	free(fg_place.name);
	fg_place.name = NULL;
	fg_place.c = NULL;
	fg_place.id = 0;
}

/* A new window, before it maps: whether it may take focus. With the
 * guard, one that may not is held from it, and marked urgent unless it's
 * a status popup. A grant is used either way, so it's one-shot, and its
 * workspace taken. Returns that workspace, for fg_opened, or NULL. A
 * canceled grant's window may be aimed at another workspace (fg_aim). */
static char *
fg_open(Client *c)
{
	FgGrant *g;
	char *ws;
	int placed;
	/* A window mapping again is new again: nothing holds it yet */
	c->held = FgFree;
	/* A status popup opens on screen as it is: unfocused even from the
	 * app you're in, unmarked, and taking no grant */
	if (fg_nameless(c)) {
		if (fg_enabled())
			c->held = FgPopup;
		return NULL;
	}
	/* Grants first, so one is used up even by a window of the app you're
	 * in: a second terminal from a terminal */
	if ((g = fg_take(c, 0)) || (g = fg_take_by_ancestry(c, 0))) {
		ws = g->workspace;
		g->workspace = NULL;
		fg_free(g);
		fg_granted = c;
		return ws;
	}
	/* Then a canceled one, for a window that isn't a dialog, which goes
	 * with its parent: it opens where it was launched from, if you've gone
	 * elsewhere, held and marked, with the guard or without it */
	if (!client_get_parent(c) && ((g = fg_take(c, 1)) || (g = fg_take_by_ancestry(c, 1)))) {
		placed = fg_aim(c, g);
		fg_free(g);
		if (placed) {
			c->held = FgWaiting;
			c->isurgent = 1;
			c->urgent_seq = ++urgent_count;
			return NULL;
		}
	}
	if (!fg_enabled())
		return NULL;
	/* Let in, it may take focus; fg_opened sees whether it did */
	if (fg_same_app_as_active(c)
			|| (fg_active && fg_class_in(c, portal_classes, LENGTH(portal_classes)))
			|| (fg_prompt_may_focus() && fg_class_in(c, prompt_classes, LENGTH(prompt_classes))))
		return NULL;
	/* Marked, its border and event coming as it maps: it has no border yet */
	c->held = FgWaiting;
	c->isurgent = 1;
	c->urgent_seq = ++urgent_count;
	return NULL;
}

/* A new window has mapped, with its monitor, workspace and rules: one its
 * grant sends to another workspace goes there with focus, and one the
 * guard holds says it's urgent.
 *
 * A new window that took focus with no grant, one the guard let in, or
 * any with the guard off, is focus moving on: a dialog after a click is
 * the click's only trace. One that opened elsewhere, without focus, moves
 * nothing. A granted one's focus is the grant's, even put off. */
static void
fg_opened(Client *c, char *workspace)
{
	int granted = fg_granted == c;
	fg_granted = NULL;
	fg_unaim();
	if (workspace)
		fg_focus(c, workspace);
	free(workspace);
	if (granted)
		fg_defer(c);
	else if (c == focused_client)
		fg_cancel();
	if (c->held == FgWaiting && c->isurgent) {
		ipc_event_window(c, "urgent");
		ws_update_urgency(c->ws);
	}
}

/* Whether old, a layer surface with the keyboard, keeps it as focus moves
 * to l, or to a window or nothing for no l: one with on-demand focus, such
 * as a notification's reply field, keeps it unless you ask, by a click, a
 * binding or a command, or the lock takes it, or an exclusive one,
 * such as the launcher, opens. A window opening or closing, or the pointer
 * crossing one, doesn't take it, guard or no guard, as tide's SPEC §14.1
 * has it. */
static int
fg_layer_keeps(LayerSurface *old, LayerSurface *l)
{
	return !focus_asked && old->mapped
			&& old->layer_surface->current.keyboard_interactive
				== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
			&& !(l && l->layer_surface->current.keyboard_interactive
				== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
}

/* Focus went to c, or to nothing. However you got there, a held window
 * isn't held now. Focus moving on cancels the grants, unless it's the
 * guard's own, or a window mapping's (its rules may move it away, and
 * focus back), or a granted window's put off by a launcher, or coming back
 * to the window you were in (a launcher closing), or staying on nothing
 * on the same workspace. */
static void
fg_focused(Client *c)
{
	unsigned long before = fg_active, before_ws = fg_active_ws;
	int returned, deferred = c && fg_deferred && ipc_id(&c->ipc_id) == fg_deferred;
	fg_deferred = 0;
	fg_active = c ? ipc_id(&c->ipc_id) : 0;
	fg_active_ws = curws ? ipc_id(&curws->ipc_id) : 0;
	free(fg_active_app);
	fg_active_app = c ? fg_normalize(fg_raw_appid(c)) : NULL;
	fg_active_pid = c ? ipc_client_pid(c) : 0;
	if (c) {
		c->held = FgFree;
		c->focus_seq = ++fg_focus_count;
	}
	returned = (fg_active && fg_active == before)
			|| (!fg_active && !before && fg_active_ws == before_ws);
	if (fg_focusing || returned || fg_mapping || deferred)
		return;
	fg_cancel();
}

/* The window to focus as fg_closing, the window you were in, closes, as
 * tide's SPEC §14.2 and Hyprland's input:focus_on_close cursor have it:
 * the window under the pointer, once the windows left have moved in.
 * Its place is its geometry, which arrange() has set though the window
 * hasn't drawn at its new size yet. Like Hyprland, it looks past layer
 * surfaces, such as a notification, at the windows under them. The
 * pointer stays where it is. Otherwise the most recent window, as dwl
 * has it: when nothing is under the pointer, or the window there is one
 * the guard holds, which waits for you to go to it (TODO.md's *A held
 * window waits for you*), or the one closing is a dialog, whose parent
 * takes focus back. */
static Client *
fg_after_close(void)
{
	static const int lyrs[] = {LyrFS, LyrFloat, LyrTile}; /* topmost first */
	struct wlr_scene_node *node;
	Client *c;
	size_t i;
	if (!fg_closing || client_get_parent(fg_closing))
		return focustop(selmon);
	for (i = 0; i < LENGTH(lyrs); i++) {
		wl_list_for_each_reverse(node, &layers[lyrs[i]]->children, link) {
			if (!node->enabled || node->type != WLR_SCENE_NODE_TREE
					|| !(c = node->data) || c == fg_closing
					|| client_is_unmanaged(c) || !VISIBLEON(c, c->mon)
					|| !wlr_box_contains_point(&c->geom, cursor->x, cursor->y))
				continue;
			return c->held ? focustop(selmon) : c;
		}
	}
	return focustop(selmon);
}

/* A key press, or a click: you moving on. It went to no window until
 * fg_delivered says which, so not if a binding took it. */
static void
fg_key(void)
{
	fg_cancel();
	fg_last_key = fg_now();
	fg_last_input.at = fg_last_key;
	fg_last_input.button = 0;
	fg_last_input.window = 0;
}

static void
fg_click(void)
{
	fg_cancel();
	fg_last_input.at = fg_now();
	fg_last_input.button = 1;
	fg_last_input.window = 0;
}

/* Your key press or click went to s, the keyboard's focus or the
 * pointer's: its window, or none for a menu, which isn't in the tree, a
 * layer surface or the lock screen */
static void
fg_delivered(struct wlr_surface *s)
{
	Client *c = NULL;
	toplevel_from_wlr_surface(s, &c, NULL);
	fg_last_input.window = c && !client_is_unmanaged(c) ? ipc_id(&c->ipc_id) : 0;
}

/* GET_SEATS's last_input: {"type": "key" or "button", "window": its IPC
 * id or null, "msec_ago": how long ago}, or null before any input */
static struct json_object *
fg_json_last_input(void)
{
	struct json_object *obj;
	if (!fg_last_input.at)
		return NULL;
	obj = json_object_new_object();
	json_object_object_add(obj, "type",
			json_object_new_string(fg_last_input.button ? "button" : "key"));
	json_object_object_add(obj, "window", fg_last_input.window
			? json_object_new_uint64(fg_last_input.window) : NULL);
	json_object_object_add(obj, "msec_ago",
			json_object_new_uint64((fg_now() - fg_last_input.at) / 1000));
	return obj;
}

/* An existing window asked to be activated: a grant for its app lets it
 * take focus where it is, even one asked for on another workspace, since
 * moving a window you already had would surprise you. Returns whether it
 * did. An activation before the window maps is left to its opening:
 * Chrome activates a new window before showing it. */
static int
fg_activate(Client *c)
{
	FgGrant *g;
	if (!client_surface(c) || !client_surface(c)->mapped)
		return 0;
	if (!(g = fg_take(c, 0))) {
		/* A canceled one is used too, giving nothing: its app answered
		 * with the window it had, so no new one is coming */
		if ((g = fg_take(c, 1)))
			fg_free(g);
		return 0;
	}
	fg_free(g);
	fg_focus(c, NULL);
	return 1;
}

/* Reads an option's value, --name=VALUE or --name VALUE, if *p starts
 * with --name. Returns 1 for one, 0 for no such option, or -1 for a
 * missing or unclosed value. */
static int
fg_option(char **p, const char *name, char **value)
{
	size_t n = strlen(name);
	if (strncmp(*p, name, n) || ((*p)[n] && (*p)[n] != '=' && !isspace((unsigned char)(*p)[n])))
		return 0;
	*p += n;
	if (**p == '=') {
		(*p)++;
	} else {
		while (isspace((unsigned char)**p))
			(*p)++;
	}
	return **p && (*value = settings_arg(p)) && **value ? 1 : -1;
}

/* A process id or a key, a whole number from 2 up for a pid */
static int
fg_number(const char *s, unsigned long min, unsigned long *n)
{
	char *end;
	errno = 0;
	*n = strtoul(s, &end, 10);
	return !errno && end != s && !*end && isdigit((unsigned char)*s) && *n >= min
			&& *n <= INT_MAX;
}

/* The grant command:
 *
 *   grant [--pid PID] [--key KEY] [--workspace NAME] [APP ...]
 *   grant extend --key KEY [--pid PID] APP ID ...
 *
 * Fills err and returns 0 on an error. */
static int
fg_command(char *args, char *err, size_t errsize)
{
	char *word, *value, *workspace = NULL, **apps = NULL;
	unsigned long pid = 0, key = 0;
	size_t napps = 0, i;
	int extend = 0, or_recent = 0, r, ok = 0;
	FgGrant *g;
	if (!strncmp(args, "extend", 6) && (!args[6] || isspace((unsigned char)args[6]))) {
		extend = 1;
		args += 6;
		while (isspace((unsigned char)*args))
			args++;
	}
	while (!strncmp(args, "--", 2)) {
		if (!extend && !strncmp(args, "--or-recent", 11)
				&& (!args[11] || isspace((unsigned char)args[11]))) {
			or_recent = 1;
			settings_word(&args);
		} else if ((r = fg_option(&args, "--pid", &value))) {
			if (r < 0 || !fg_number(value, 2, &pid)) {
				snprintf(err, errsize, "grant: --pid takes a process id");
				goto out;
			}
		} else if ((r = fg_option(&args, "--key", &value))) {
			if (r < 0 || !fg_number(value, 1, &key)) {
				snprintf(err, errsize, "grant: --key takes a positive whole number");
				goto out;
			}
		} else if (!extend && (r = fg_option(&args, "--workspace", &value))) {
			if (r < 0 || !ws_name_ok(value)) {
				snprintf(err, errsize, "grant: --workspace takes a workspace's name");
				goto out;
			}
			free(workspace);
			workspace = strdup(value);
		} else {
			value = settings_word(&args);
			snprintf(err, errsize, "grant: %.64s is not an option (--pid, --key%s are)", value,
					extend ? "" : ", --workspace, --or-recent");
			goto out;
		}
	}
	while (*args) {
		if (!(word = settings_arg(&args)) || !(value = fg_normalize(word))) {
			snprintf(err, errsize, "grant: expected app ids");
			goto out;
		}
		apps = realloc(apps, (napps + 1) * sizeof(*apps));
		if (!apps)
			die("out of memory granting focus");
		apps[napps++] = value;
	}
	for (i = 0; i < napps; i++) {
		if (!strcmp(apps[i], "*") && (napps > 1 || extend)) {
			snprintf(err, errsize, "grant: * stands alone, for any app's window");
			goto out;
		}
	}
	if (extend) {
		if (!key || napps < 2) {
			snprintf(err, errsize, "grant extend needs --key, the grant's app, then ids to add");
			goto out;
		}
		/* Adds ids to the grant just made for apps[0], while it holds:
		 * one canceled, used or lapsed in between stays gone */
		fg_expire();
		wl_list_for_each(g, &fg_grants, link) {
			if (g->canceled || g->key != key || g->pid != (pid_t)pid || !g->napps
					|| strcmp(g->apps[0], apps[0]))
				continue;
			for (i = 1; i < napps; i++) {
				size_t j;
				for (j = 0; j < g->napps && strcmp(g->apps[j], apps[i]); j++)
					;
				if (j < g->napps)
					continue;
				g->apps = realloc(g->apps, (g->napps + 1) * sizeof(*g->apps));
				if (!g->apps)
					die("out of memory granting focus");
				g->apps[g->napps++] = apps[i];
				apps[i] = NULL;
			}
			break;
		}
		ok = 1;
		goto out;
	}
	if (!napps && !pid) {
		snprintf(err, errsize, "grant needs an app id, *, or --pid");
		goto out;
	}
	if (or_recent && (!napps || !strcmp(apps[0], "*") || pid || key || workspace)) {
		snprintf(err, errsize, "grant --or-recent takes app ids alone, whose window it brings up");
		goto out;
	}
	fg_expire();
	g = ecalloc(1, sizeof(*g));
	g->apps = apps;
	g->napps = napps;
	g->pid = (pid_t)pid;
	g->key = key;
	g->workspace = workspace;
	g->or_recent = or_recent;
	g->at = fg_now();
	if (curws && !or_recent) {
		g->origin = ipc_id(&curws->ipc_id);
		if (!(g->origin_name = strdup(curws->name)))
			die("out of memory granting focus");
	}
	wl_list_insert(fg_grants.prev, &g->link);
	if (or_recent)
		fg_arm();
	return 1;
out:
	for (i = 0; i < napps; i++)
		free(apps[i]);
	free(apps);
	free(workspace);
	return ok;
}

/* Drops the grants, at exit */
static void
fg_finish(void)
{
	if (fg_timer)
		wl_event_source_remove(fg_timer);
	fg_timer = NULL;
	fg_drop();
	fg_unaim();
	free(fg_active_app);
	fg_active_app = NULL;
}

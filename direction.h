/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Focusing and moving a window by direction, sway's focus and move
 * left|right|up|down, by the rule tide's SPEC §6.6 has for its arrow keys,
 * tide for macOS's: of the windows, or monitors, whose center is on that
 * side, the one sharing the most height with where you are (the most
 * width, going up or down), then the one whose center is nearest, then
 * the first in stack order. Past a monitor's edge it goes on to the next
 * monitor that way, to the window at the edge it comes in by, and past the
 * last monitor, nowhere. dwl.c includes this file, like client.h.
 */

/* A box's center, doubled, so it stays whole */
static int
dir_cx(struct wlr_box b)
{
	return 2 * b.x + b.width;
}

static int
dir_cy(struct wlr_box b)
{
	return 2 * b.y + b.height;
}

/* Whether to's center is on dir's side of from's */
static int
dir_beyond(struct wlr_box from, struct wlr_box to, enum wlr_direction dir)
{
	switch (dir) {
	case WLR_DIRECTION_LEFT:
		return dir_cx(to) < dir_cx(from);
	case WLR_DIRECTION_RIGHT:
		return dir_cx(to) > dir_cx(from);
	case WLR_DIRECTION_UP:
		return dir_cy(to) < dir_cy(from);
	default:
		return dir_cy(to) > dir_cy(from);
	}
}

/* How much a and b share across dir: their height, going left or right,
 * or their width, going up or down */
static int
dir_shared(struct wlr_box a, struct wlr_box b, enum wlr_direction dir)
{
	int lo, hi;
	if (dir == WLR_DIRECTION_LEFT || dir == WLR_DIRECTION_RIGHT) {
		lo = MAX(a.y, b.y);
		hi = MIN(a.y + a.height, b.y + b.height);
	} else {
		lo = MAX(a.x, b.x);
		hi = MIN(a.x + a.width, b.x + b.width);
	}
	return MAX(hi - lo, 0);
}

/* The square of the distance between a's and b's centers, doubled */
static long long
dir_distance(struct wlr_box a, struct wlr_box b)
{
	long long dx = dir_cx(a) - dir_cx(b), dy = dir_cy(a) - dir_cy(b);
	return dx * dx + dy * dy;
}

/* Whether to beats best, going dir from from: sharing more, then nearer.
 * A tie keeps best, which came first. */
static int
dir_better(struct wlr_box from, struct wlr_box to, const struct wlr_box *best,
		enum wlr_direction dir)
{
	int shared, best_shared;
	if (!best)
		return 1;
	shared = dir_shared(from, to, dir);
	best_shared = dir_shared(from, *best, dir);
	if (shared != best_shared)
		return shared > best_shared;
	return dir_distance(from, to) < dir_distance(from, *best);
}

/* The next monitor to m going dir, or NULL past the last: no wrapping
 * round, as dwl's dirtomon() does */
static Monitor *
dir_monitor(Monitor *m, enum wlr_direction dir)
{
	Monitor *o, *found = NULL;
	wl_list_for_each(o, &mons, link) {
		if (o == m || !o->wlr_output->enabled || !dir_beyond(m->m, o->m, dir))
			continue;
		if (dir_better(m->m, o->m, found ? &found->m : NULL, dir))
			found = o;
	}
	return found;
}

/* How far b is from the edge of its monitor that a window going dir comes
 * in by, so the least is at that edge: going right, its left edge */
static int
dir_from_edge(struct wlr_box b, enum wlr_direction dir)
{
	switch (dir) {
	case WLR_DIRECTION_LEFT:
		return -(b.x + b.width);
	case WLR_DIRECTION_RIGHT:
		return b.x;
	case WLR_DIRECTION_UP:
		return -(b.y + b.height);
	default:
		return b.y;
	}
}

/* Whether going by direction finds c on m: shown there, tiled if tiled,
 * and not a status popup, which only the pointer focuses */
static int
dir_counts(Client *c, Monitor *m, int tiled)
{
	return VISIBLEON(c, m) && c->held != FgPopup
			&& (!tiled || (!c->isfloating && !c->isfullscreen));
}

/* The fullscreen window shown on m, which covers the others there but
 * its dialogs, or NULL */
static Client *
dir_cover(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->isfullscreen && dir_counts(c, m, 0))
			return c;
	return NULL;
}

/* The window going dir from from finds on m, by the rule, other than
 * skip: one beyond from, if beyond, or else one at the edge going dir
 * comes in by, as onto the next monitor. Tiled ones only, if tiled; or
 * else, with a fullscreen window shown there, only it and its dialogs,
 * since focus on another would end its fullscreen (uncoverfor). */
static Client *
dir_window(Monitor *m, struct wlr_box from, enum wlr_direction dir, Client *skip,
		int beyond, int tiled)
{
	Client *c, *found = NULL, *cover = tiled ? NULL : dir_cover(m);
	int edge = INT_MAX;
#define DIR_SEEN(c) ((c) != skip && dir_counts(c, m, tiled) \
		&& (!cover || (c) == cover || descendsfrom(c, cover)))
	if (!beyond)
		wl_list_for_each(c, &clients, link)
			if (DIR_SEEN(c))
				edge = MIN(edge, dir_from_edge(c->geom, dir));
	wl_list_for_each(c, &clients, link) {
		if (!DIR_SEEN(c) || (beyond
				? !dir_beyond(from, c->geom, dir) : dir_from_edge(c->geom, dir) != edge))
			continue;
		if (dir_better(from, c->geom, found ? &found->geom : NULL, dir))
			found = c;
	}
	return found;
#undef DIR_SEEN
}

/* Swaps a's and b's places in the stack */
static void
dir_swap(Client *a, Client *b)
{
	struct wl_list *before_a, *before_b;
	if (a == b)
		return;
	if (a->link.next == &b->link) {
		wl_list_remove(&a->link);
		wl_list_insert(&b->link, &a->link);
		return;
	}
	if (b->link.next == &a->link) {
		wl_list_remove(&b->link);
		wl_list_insert(&a->link, &b->link);
		return;
	}
	before_a = a->link.prev;
	before_b = b->link.prev;
	wl_list_remove(&a->link);
	wl_list_remove(&b->link);
	wl_list_insert(before_a, &b->link);
	wl_list_insert(before_b, &a->link);
}

/* sway's focus left|right|up|down: the window that way from the focused
 * one on its monitor, or, past the monitor's edge, the nearest at the edge
 * of the next monitor that way, or that monitor, if it shows none. Past
 * the last monitor, nothing. A fullscreen window covers its monitor's
 * others, unless it has a dialog. With only windows the focus guard
 * holds, it goes to one, as focus next does. */
static void
dir_focus(enum wlr_direction dir)
{
	Client *sel = focustop(selmon), *c = NULL;
	Monitor *m;
	if (!selmon)
		return;
	if (!sel && (c = focusgo(selmon))) {
		focusclient(c, NULL, 1);
		return;
	}
	if (sel && !(sel->isfullscreen && !client_has_children(sel)))
		c = dir_window(selmon, sel->geom, dir, sel, 1, 0);
	if (!c) {
		if (!(m = dir_monitor(selmon, dir)))
			return;
		c = dir_window(m, sel ? sel->geom : selmon->m, dir, NULL, 0, 0);
		selmon = m;
	}
	focusclient(c, NULL, 1);
}

/* A floating window's box b moved from window area old to new, as tide's
 * SPEC §6.6 has it: at the same spot relative to the area, its size, but
 * no bigger than the area, nor than its hints allow, and then just inside */
static struct wlr_box
dir_place(Client *c, struct wlr_box b, struct wlr_box old, struct wlr_box new)
{
	struct wlr_box p;
	p.width = MIN(b.width, new.width);
	p.height = MIN(b.height, new.height);
	fitsizehints(c, &p);
	p.x = new.x + (old.width > 0 ? (int)lround((double)(b.x - old.x) * new.width / old.width) : 0);
	p.y = new.y + (old.height > 0 ? (int)lround((double)(b.y - old.y) * new.height / old.height) : 0);
	p.x = MAX(new.x, MIN(p.x, new.x + new.width - p.width));
	p.y = MAX(new.y, MIN(p.y, new.y + new.height - p.height));
	return p;
}

/* sway's move left|right|up|down, for c. A tiled window swaps with the
 * tile that way on its monitor, or, past its edge, joins the next
 * monitor's tiles, taking the place of the one at the edge it comes in by
 * that shares the most with it. A floating one moves px pixels that way,
 * as in sway, or with px negative, goes straight to the next monitor, at
 * the same spot relative to its window area. Past the last monitor,
 * nothing. A window with focus keeps it. Returns an error, or NULL. */
static const char *
dir_move(Client *c, enum wlr_direction dir, int px)
{
	Client *t;
	Monitor *m, *from = c->mon;
	struct wlr_box was = c->geom, box = c->geom;
	int focused = c == focustop(selmon);
	if (!from)
		return "The window isn't on an output";
	if (c->isfullscreen)
		return "A fullscreen window can't move that way";
	/* Which way is which is from where it's shown */
	if (!VISIBLEON(c, from))
		return "The window's workspace isn't shown";
	if (c->isfloating && px >= 0) {
		if (c->ismaximized)
			return "A maximized window can't move by pixels";
		box.x += dir == WLR_DIRECTION_LEFT ? -px : dir == WLR_DIRECTION_RIGHT ? px : 0;
		box.y += dir == WLR_DIRECTION_UP ? -px : dir == WLR_DIRECTION_DOWN ? px : 0;
		resize(c, box, 0);
		return NULL;
	}
	if (!c->isfloating && from->lt[from->sellt]->arrange
			&& (t = dir_window(from, was, dir, c, 1, 1))) {
		dir_swap(c, t);
		arrange(from);
		return NULL;
	}
	if (!(m = dir_monitor(from, dir)))
		return NULL;
	/* A maximized one goes back to its place there once it isn't */
	if (c->isfloating && c->ismaximized && c->unmaximized.width)
		c->unmaximized = dir_place(c, c->unmaximized, from->w, m->w);
	else if (c->isfloating && !c->ismaximized)
		box = dir_place(c, was, from->w, m->w);
	t = c->isfloating ? NULL : dir_window(m, was, dir, NULL, 0, 1);
	/* So setmon() focuses it there */
	if (focused)
		selmon = m;
	/* movemon(), with c placed before its dialogs follow it to its
	 * middle */
	setmon(c, m, c->ws);
	if (t) {
		/* In t's place, which goes one along */
		wl_list_remove(&c->link);
		wl_list_insert(t->link.prev, &c->link);
		arrange(m);
	}
	if (c->isfloating && !c->ismaximized)
		resize(c, box, 0);
	dialogsfollow(c, NULL);
	return NULL;
}

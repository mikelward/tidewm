/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Dragging the master/stack edge, as tide's SPEC §6.6 has it: a plain left
 * press within resize_grab pixels either side of the edge between the
 * masters and the stack starts a drag, and the edge follows the pointer,
 * setting mfact, until a button comes up. The edges are tile's, twocol's,
 * and threecol's with one stack window; with stacks either side of
 * threecol's centered master, either of its sides drags it, and it grows
 * or shrinks both ways. A lone window's left and right sides drag its
 * width, and it stays centered: that width is its part's own, in place of
 * the single_window rules, until single_width reset. A stack's rows are
 * equal, and twocol's masters share their side equally, so there's no
 * edge between them.
 *
 * Anything over the edge takes the press as usual: a floating window, a
 * layer surface, as the bar, a notification or a popup of one, or a
 * window's popup. So does a press on a monitor that isn't focused, under
 * a fullscreen or maximized window, or while a popup grabs the pointer,
 * as a menu does, or a focus grab holds: then a press outside it closes
 * it rather than dragging. The pointer shows a resize cursor over the
 * edge, where the windows don't get it.
 *
 * Super and the right button, a floating_modifier resize, drag a tile's
 * nearest edge the same way, as Hyprland's own resize does, wherever on
 * the tile the press is. A tile with no edge floats, as dwl's does.
 */

enum { EdgeSplit, EdgeCenter, EdgeLone }; /* SplitEdge.kind */

typedef struct {
	int x; /* layout-relative */
	int kind; /* EdgeSplit: mfact is its share of the width from the left;
	           * EdgeCenter: a side of threecol's centered master;
	           * EdgeLone: a side of a lone window */
	int right; /* EdgeCenter, EdgeLone: the right side */
} SplitEdge;

/* The drag on, while cursor_mode is CurEdge */
static struct {
	Monitor *m; /* NULL once its monitor goes */
	Workspace *ws;
	const Layout *lt;
	SplitEdge edge;
	double offset; /* the pointer's from the edge, which it keeps */
	double start; /* the pointer's x at the press */
	int moved; /* the pointer has moved from it: until then nothing's set */
} split_drag;

/* How many pixels either side of an edge a press drags it from: the
 * config file's resize_grab, else config.h's */
static int
split_grab(void)
{
	return settings && settings->resize_grab >= 0 ? settings->resize_grab : resize_grab;
}

/* m's edges as its layout places its tiles now, into out, which holds 2.
 * Returns how many. */
static int
split_edges(Monitor *m, SplitEdge *out)
{
	void (*arrange)(Monitor *) = m->lt[m->sellt]->arrange;
	int n = lt_count(m), nm, i;
	struct wlr_box box, cols[16];
	double w[16];
	if (!arrange || !n)
		return 0;
	if (n == 1) {
		box = lt_single_box(m);
		out[0] = (SplitEdge){box.x, EdgeLone, 0};
		out[1] = (SplitEdge){box.x + box.width, EdgeLone, 1};
		return 2;
	}
	if (arrange == tile) {
		if (m->nmaster <= 0 || n <= m->nmaster)
			return 0;
		/* As tile() rounds the masters' width */
		out[0] = (SplitEdge){m->w.x + (int)roundf((float)m->w.width * m->mfact), EdgeSplit, 0};
		return 1;
	}
	if (arrange == threecol) {
		nm = MIN(MAX(m->nmaster, 1), n);
		if (n - nm == 0)
			return 0;
		if (n - nm == 1) {
			lt_columns(m->w, (double[]){m->mfact, 1 - m->mfact}, 2, cols);
			out[0] = (SplitEdge){cols[1].x, EdgeSplit, 0};
			return 1;
		}
		lt_columns(m->w, (double[]){(1 - m->mfact) / 2, m->mfact, (1 - m->mfact) / 2}, 3, cols);
		out[0] = (SplitEdge){cols[1].x, EdgeCenter, 0};
		out[1] = (SplitEdge){cols[2].x, EdgeCenter, 1};
		return 2;
	}
	if (arrange == twocol) {
		nm = MIN(MAX(m->nmaster, 1), MIN(n, 15));
		if (nm == n)
			return 0;
		/* twocol's own weights, so the edge rounds as its columns do */
		for (i = 0; i < nm; i++)
			w[i] = m->mfact / nm;
		w[nm] = 1 - m->mfact;
		lt_columns(m->w, w, nm + 1, cols);
		out[0] = (SplitEdge){cols[nm].x, EdgeSplit, 0};
		return 1;
	}
	return 0;
}

/* Whether m shows a fullscreen or maximized window, which covers its edges */
static int
split_covered(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && (c->isfullscreen || c->ismaximized))
			return 1;
	return 0;
}

/* The monitor whose edge a plain press at x, y would drag, with the edge
 * in *found, or NULL if the press goes on as usual */
static Monitor *
split_edge_at(double x, double y, SplitEdge *found)
{
	Monitor *m = xytomon(x, y);
	SplitEdge edges[2];
	struct wlr_surface *surface = NULL;
	Client *c = NULL, *f;
	LayerSurface *l = NULL;
	int grab = split_grab(), n, i;
	/* A popup's pointer grab, a menu's, or a focus grab: a press outside
	 * it closes it */
	if (grab <= 0 || locked || !m || m != selmon || !m->lt_settled || seat->drag
			|| active_constraint || grab_active
			|| seat->pointer_state.grab != seat->pointer_state.default_grab)
		return NULL;
	if (y < m->w.y || y >= m->w.y + m->w.height || split_covered(m))
		return NULL;
	n = split_edges(m, edges);
	for (i = 0; i < n; i++)
		if (x >= edges[i].x - grab && x < edges[i].x + grab)
			break;
	if (i == n)
		return NULL;
	/* What's over the tiles there takes the press: a layer surface or its
	 * popup, a floating or unmanaged window, or a window's popup */
	xytonode(x, y, &surface, &c, &l, NULL, NULL);
	if (l || (c && (c->isfloating || client_is_unmanaged(c)
			|| wlr_surface_get_root_surface(surface) != client_surface(c))))
		return NULL;
	/* A floating window's border, which isn't a surface */
	wl_list_for_each(f, &clients, link)
		if (VISIBLEON(f, m) && f->isfloating && x >= f->geom.x && y >= f->geom.y
				&& x < f->geom.x + f->geom.width && y < f->geom.y + f->geom.height)
			return NULL;
	*found = edges[i];
	return m;
}

/* Starts dragging m's edge e from the pointer at x */
static void
split_start(Monitor *m, const SplitEdge *e, double x)
{
	split_drag.m = m;
	split_drag.ws = curws;
	split_drag.lt = m->lt[m->sellt];
	split_drag.edge = *e;
	split_drag.offset = x - e->x;
	split_drag.start = x;
	split_drag.moved = 0;
	cursor_mode = CurEdge;
	/* The windows don't see the drag */
	wlr_seat_pointer_clear_focus(seat);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "ew-resize");
}

/* Super and the right button on tiled c, at x: drags its monitor's edge
 * nearest the pointer. Returns whether it has one. */
static int
split_start_tile(Client *c, double x)
{
	SplitEdge edges[2];
	int n, i, best = 0;
	if (!c->mon || c->isfloating || c->isfullscreen || c->ismaximized
			|| !VISIBLEON(c, c->mon) || split_covered(c->mon)
			|| !(n = split_edges(c->mon, edges)))
		return 0;
	for (i = 1; i < n; i++)
		if (fabs(edges[i].x - x) < fabs(edges[best].x - x))
			best = i;
	split_start(c->mon, &edges[best], x);
	return 1;
}

/* The pointer at x, during a drag: the mfact, or the lone window's width,
 * that puts the edge there, keeping its offset. While the workspace or the
 * layout isn't the one dragged, nothing changes. */
static void
split_follow(double x)
{
	Monitor *m = split_drag.m;
	double v;
	float f;
	if (x != split_drag.start)
		split_drag.moved = 1;
	if (!m || !split_drag.moved || curws != split_drag.ws || !m->lt_settled
			|| m->lt[m->sellt] != split_drag.lt || m->w.width <= 0)
		return;
	x -= split_drag.offset;
	if (split_drag.edge.kind == EdgeSplit) {
		v = (x - m->w.x) / m->w.width;
	} else {
		/* A centered master's side, or a lone window's: the edge's
		 * distance from the center is half its width. Past the center,
		 * it stays at its narrowest. */
		v = 2 * (x - (m->w.x + m->w.width / 2.0)) / m->w.width;
		if (!split_drag.edge.right)
			v = -v;
	}
	if (split_drag.edge.kind == EdgeLone) {
		v = MAX(0.1, MIN(v, 1));
		if (v == m->single)
			return;
		m->single = v;
	} else {
		f = (float)MAX(0.1, MIN(v, 0.9));
		if (f == m->mfact)
			return;
		m->mfact = f;
	}
	arrange(m);
}

/* m goes: a drag of its edge does nothing more */
static void
split_unplug(Monitor *m)
{
	if (split_drag.m == m)
		split_drag.m = NULL;
}

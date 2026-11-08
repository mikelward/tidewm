/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * tidewm's layouts beside dwl's tile and monocle, from tide's SPEC §6.1:
 * three-column, with the master centered and stacks either side, and two
 * columns + a stack. Their geometry is tide's (hypr/tide/geometry.lua):
 * boxes on whole pixels, neighbors sharing an edge, covering the window
 * area with no gap and no overlap.
 *
 * Each monitor keeps mfact and nmaster per layout, so moving between
 * layouts doesn't carry one layout's split into another.
 */

/* A layout's own mfact and nmaster, where config.h's monitor rule doesn't
 * give them: tide's defaults */
static const struct {
	void (*arrange)(Monitor *);
	float mfact;
	int nmaster;
} lt_defaults[] = {
	{ threecol, 0.5f, 1 },
	{ twocol, 0.72f, 2 },
};

/* Cuts [start, start + len) into n pieces weighted by w, into pos and
 * size. Each edge is rounded, not each size, so the pieces meet and add up
 * to len. */
static void
lt_spans(int start, int len, const double *w, int n, int *pos, int *size)
{
	double total = 0, acc = 0;
	int i, prev = start, edge;
	for (i = 0; i < n; i++)
		total += w[i];
	for (i = 0; i < n; i++) {
		acc += w[i];
		edge = (int)floor(start + len * acc / total + 0.5);
		pos[i] = prev;
		size[i] = edge - prev;
		prev = edge;
	}
}

/* n columns across area, weighted by w, into out */
static void
lt_columns(struct wlr_box area, const double *w, int n, struct wlr_box *out)
{
	int pos[16], size[16], i;
	lt_spans(area.x, area.width, w, n, pos, size);
	for (i = 0; i < n; i++)
		out[i] = (struct wlr_box){pos[i], area.y, size[i], area.height};
}

/* n equal rows down area, into out */
static void
lt_rows(struct wlr_box area, int n, struct wlr_box *out)
{
	int i, prev = area.y, edge;
	for (i = 0; i < n; i++) {
		edge = (int)floor(area.y + area.height * (i + 1) / (double)n + 0.5);
		out[i] = (struct wlr_box){area.x, prev, area.width, edge - prev};
		prev = edge;
	}
}

/* How many tiled windows m shows: not floating, not fullscreen */
static int
lt_count(Monitor *m)
{
	Client *c;
	int n = 0;
	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	return n;
}

/* Whether boxes a and b are in the same column */
static int
lt_same_column(const struct wlr_box *a, const struct wlr_box *b)
{
	return a->x == b->x && a->width == b->width;
}

/* Gives m's n tiled windows the boxes, in stack order. A window's size
 * hints can make it taller or shorter than its box: as dwl's tile() goes
 * on from each window's real height, the rows after it in its column start
 * at its bottom, and share what's left of the column evenly. */
static void
lt_place(Monitor *m, struct wlr_box *boxes, int n)
{
	Client *c;
	int i = 0, j, k, count, top, bottom, end, left;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, boxes[i], 0);
		if (c->geom.height != boxes[i].height) {
			/* The later rows in this column, and where it ends */
			end = boxes[i].y + boxes[i].height;
			bottom = end;
			for (j = i + 1, count = 0; j < n; j++) {
				if (lt_same_column(&boxes[j], &boxes[i]) && boxes[j].y >= end) {
					count++;
					bottom = boxes[j].y + boxes[j].height;
				}
			}
			top = boxes[i].y + c->geom.height;
			left = MAX(bottom - top, 0);
			/* Each is checked before it's moved, so the later ones still are */
			for (j = i + 1, k = 0; j < n && k < count; j++) {
				if (!lt_same_column(&boxes[j], &boxes[i]) || boxes[j].y < end)
					continue;
				boxes[j].y = top + (int)floor(left * k / (double)count + 0.5);
				boxes[j].height = top + (int)floor(left * (k + 1) / (double)count + 0.5)
					- boxes[j].y;
				k++;
			}
		}
		i++;
	}
}

/* The width, as a fraction, for m's lone window: its part's own, which a
 * drag of its side sets (split.h), else the config file's single_window
 * rules, or config.h's, whichever the window area's aspect reaches with
 * the largest min_aspect, and the whole width for none */
static double
lt_single_width(Monitor *m)
{
	const SingleRule *r = settings && settings->nsingles ? settings->singles : single_window;
	size_t n = settings && settings->nsingles ? settings->nsingles : LENGTH(single_window), i;
	double aspect = m->w.height > 0 ? m->w.width / (double)m->w.height : 0, best = -1;
	double width = 1;
	if (m->single > 0)
		return MAX(0.1, MIN(m->single, 1));
	for (i = 0; i < n; i++) {
		if (aspect >= r[i].min_aspect && r[i].min_aspect > best) {
			best = r[i].min_aspect;
			width = r[i].width;
		}
	}
	return MAX(0.1, MIN(width, 1));
}

/* m's lone window's box: centered at its single-window width */
static struct wlr_box
lt_single_box(Monitor *m)
{
	struct wlr_box cols[3];
	double f = lt_single_width(m), side = (1 - f) / 2;
	if (f >= 1)
		return m->w;
	lt_columns(m->w, (double[]){side, f, side}, 3, cols);
	return cols[1];
}

/* Places m's tiled window, if it's alone, centered at its single-window
 * width, in any layout, as tide's geometry does (tide's SPEC §1). Returns
 * whether it was alone, so the layout needn't place it. */
static int
lt_single(Monitor *m)
{
	struct wlr_box box;
	if (lt_count(m) != 1)
		return 0;
	box = lt_single_box(m);
	lt_place(m, &box, 1);
	/* As monocle counts its windows */
	if (m->lt[m->sellt]->arrange == monocle)
		snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[1]");
	return 1;
}

/* The master centered, with a stack either side. With one stack window
 * there's nothing to balance, so it's the master and one column. Stack
 * windows go right, left, right..., so adding one never moves another to
 * the other side. */
void
threecol(Monitor *m)
{
	int n = lt_count(m), nm, s, nright, r = 0, l = 0, k;
	struct wlr_box *boxes, cols[3], *right, *left;
	double two[2] = {m->mfact, 1 - m->mfact};
	double three[3] = {(1 - m->mfact) / 2, m->mfact, (1 - m->mfact) / 2};
	if (!n)
		return;
	boxes = ecalloc((size_t)n, sizeof(*boxes));
	nm = MIN(MAX(m->nmaster, 1), n);
	s = n - nm;
	if (s == 0) {
		lt_rows(m->w, n, boxes);
	} else if (s == 1) {
		lt_columns(m->w, two, 2, cols);
		lt_rows(cols[0], nm, boxes);
		boxes[nm] = cols[1];
	} else {
		lt_columns(m->w, three, 3, cols);
		nright = (s + 1) / 2;
		right = ecalloc((size_t)nright, sizeof(*right));
		left = ecalloc((size_t)(s - nright), sizeof(*left));
		lt_rows(cols[2], nright, right);
		lt_rows(cols[0], s - nright, left);
		lt_rows(cols[1], nm, boxes);
		for (k = 0; k < s; k++)
			boxes[nm + k] = k % 2 == 0 ? right[r++] : left[l++];
		free(right);
		free(left);
	}
	lt_place(m, boxes, n);
	free(boxes);
}

/* The masters as full-height columns side by side, sharing mfact of the
 * width, and the rest stacked on the right. With no stack the masters
 * share the whole width. */
void
twocol(Monitor *m)
{
	int n = lt_count(m), nm, i;
	struct wlr_box *boxes, cols[16];
	double w[16];
	if (!n)
		return;
	boxes = ecalloc((size_t)n, sizeof(*boxes));
	/* At most 15 master columns, which fit lt_columns' arrays */
	nm = MIN(MAX(m->nmaster, 1), MIN(n, 15));
	for (i = 0; i < nm; i++)
		w[i] = nm == n ? 1 : m->mfact / nm;
	w[nm] = 1 - m->mfact;
	lt_columns(m->w, w, nm == n ? nm : nm + 1, cols);
	for (i = 0; i < nm; i++)
		boxes[i] = cols[i];
	if (nm < n)
		lt_rows(cols[nm], n - nm, boxes + nm);
	lt_place(m, boxes, n);
	free(boxes);
}

/* The layout step along m's in the cycle, the config file's layout_cycle,
 * else tide's: tile, threecol, twocol, going round. From monocle, it goes
 * from the layout before, as tide's does. From one the cycle hasn't, it
 * goes to the cycle's first, or its last going back. */
static void
(*lt_cycle(Monitor *m, int step))(Monitor *)
{
	static void (*const tides[])(Monitor *) = {tile, threecol, twocol};
	void (*const *cycle)(Monitor *) = settings && settings->ncycle ? settings->cycle : tides;
	size_t n = settings && settings->ncycle ? settings->ncycle : LENGTH(tides), i, at = n;
	const Layout *from = m->lt[m->sellt];
	if (from && from->arrange == monocle)
		from = m->lt[m->sellt ^ 1];
	for (i = 0; i < n && at == n; i++)
		if (from && from->arrange == cycle[i])
			at = i;
	if (at == n)
		return cycle[step > 0 ? 0 : n - 1];
	return cycle[(at + n + (size_t)(step % (int)n + (int)n)) % n];
}

/* lt's mfact and nmaster on the output called output, before they're
 * changed: the config file's layout_defaults, then tide's, for the layouts
 * that have them, then config.h's monitor rule's */
static void
lt_mode_defaults(const char *output, const Layout *lt, float *mfact, int *nmaster)
{
	const MonitorRule *r;
	size_t i;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(output, r->name)) {
			*mfact = r->mfact;
			*nmaster = r->nmaster;
			break;
		}
	}
	for (i = 0; i < LENGTH(lt_defaults); i++) {
		if (lt->arrange == lt_defaults[i].arrange) {
			*mfact = lt_defaults[i].mfact;
			*nmaster = lt_defaults[i].nmaster;
		}
	}
	for (i = 0; settings && i < LENGTH(settings_lt_modes); i++) {
		if (lt->arrange != settings_lt_modes[i].arrange)
			continue;
		if (settings->layout_defaults[i].has_mfact)
			*mfact = settings->layout_defaults[i].mfact;
		if (settings->layout_defaults[i].has_nmaster)
			*nmaster = settings->layout_defaults[i].nmaster;
	}
}

/* Keeps old's mfact and nmaster on m, and gives m those it had for lt, or
 * lt's defaults. */
static void
lt_switch(Monitor *m, const Layout *old, const Layout *lt)
{
	size_t i;
	if (old == lt)
		return;
	for (i = 0; i < LENGTH(m->modes); i++) {
		if (m->modes[i].lt == old || !m->modes[i].lt) {
			m->modes[i].lt = old;
			m->modes[i].mfact = m->mfact;
			m->modes[i].nmaster = m->nmaster;
			break;
		}
	}
	for (i = 0; i < LENGTH(m->modes) && m->modes[i].lt; i++) {
		if (m->modes[i].lt == lt) {
			m->mfact = m->modes[i].mfact;
			m->nmaster = m->modes[i].nmaster;
			return;
		}
	}
	lt_mode_defaults(m->wlr_output->name, lt, &m->mfact, &m->nmaster);
}

/* A workspace's part on a monitor, while another workspace is current:
 * its layouts, mfact and nmaster, each layout's own as well. It's kept by
 * output name, so a monitor plugged in again finds it. The current
 * workspace's parts are the monitors' own fields, once they've settled. */
typedef struct {
	struct wl_list link;
	Workspace *ws;
	char *output;
	const Layout *lt[2];
	unsigned int sellt;
	float mfact;
	int nmaster;
	double single; /* its lone window's width of its own; 0: the rules' */
	LayoutMode modes[LENGTH(((Monitor *)0)->modes)];
} LtPart;

static struct wl_list lt_parts = {&lt_parts, &lt_parts};

/* Where an ultrawide monitor begins: the config file's ultrawide_aspect,
 * else tide's SPEC §6.1's */
static double
lt_ultrawide(void)
{
	return settings && settings->ultrawide_aspect > 0 ? settings->ultrawide_aspect : 2.1;
}

static LtPart *
lt_part(Workspace *ws, Monitor *m, int make)
{
	LtPart *p;
	wl_list_for_each(p, &lt_parts, link)
		if (p->ws == ws && !strcmp(p->output, m->wlr_output->name))
			return p;
	if (!make)
		return NULL;
	p = ecalloc(1, sizeof(*p));
	p->ws = ws;
	p->output = strdup(m->wlr_output->name);
	wl_list_insert(&lt_parts, &p->link);
	return p;
}

/* The layout a new part on m starts in: the config file's default_layout
 * for the monitor's aspect, else three-column on an ultrawide, else the
 * monitor rule's, as dwl's, if config.h's layouts have it */
static const Layout *
lt_first_layout(Monitor *m)
{
	const Layout *lt = m->rule_lt ? m->rule_lt : &layouts[0];
	int wide = m->m.height > 0 && m->m.width >= lt_ultrawide() * m->m.height;
	void (*want)(Monitor *) = wide ? threecol : NULL;
	size_t i;
	if (settings && settings->first_layout[wide])
		want = settings->first_layout[wide];
	for (i = 0; want && i < LENGTH(layouts); i++)
		if (layouts[i].arrange == want)
			lt = &layouts[i];
	return lt;
}

/* A new part's layouts: lt_first_layout's, with the other layout to
 * toggle to; each with its own mfact and nmaster. */
static void
lt_defaults_for(Monitor *m)
{
	const Layout *lt = lt_first_layout(m);
	memset(m->modes, 0, sizeof(m->modes));
	m->lt[0] = lt;
	m->lt[1] = &layouts[LENGTH(layouts) > 1 && lt != &layouts[1]];
	m->sellt = 0;
	m->single = 0;
	/* Not the layout before it, which lt_switch would keep: fresh values */
	lt_mode_defaults(m->wlr_output->name, lt, &m->mfact, &m->nmaster);
	strncpy(m->ltsymbol, lt->symbol, LENGTH(m->ltsymbol));
}

/* Whether t tiles in the same part as c: its monitor's share of its
 * workspace, not floating, fullscreen or minimized */
static int
lt_beside(const Client *t, const Client *c)
{
	return t != c && t->mon == c->mon && t->ws == c->ws
		&& !t->isfloating && !t->isfullscreen && !t->isminimized;
}

/* How many tiled windows come first as masters in ws's part on m, as its
 * layout arranges them: monocle's first counting as one, threecol's at
 * least one, twocol's one to 15. A part not made yet, of a workspace not
 * shown there since it began, counts as it will start. */
static int
lt_masters(Monitor *m, Workspace *ws)
{
	const Layout *lt = m->lt[m->sellt];
	int nmaster = m->nmaster;
	float mfact;
	LtPart *p;
	if (ws != curws || !m->lt_settled) {
		if ((p = lt_part(ws, m, 0))) {
			lt = p->lt[p->sellt];
			nmaster = p->nmaster;
		} else {
			lt = lt_first_layout(m);
			lt_mode_defaults(m->wlr_output->name, lt, &mfact, &nmaster);
		}
	}
	if (lt->arrange == monocle)
		return 1;
	if (lt->arrange == threecol)
		return MAX(nmaster, 1);
	if (lt->arrange == twocol)
		return MIN(MAX(nmaster, 1), 15);
	return nmaster;
}

/* Puts new window c, once the rules have given it its monitor and
 * workspace, where the config file's new_window says in the clients'
 * order, which is the tiled order, as tide's layout does: at the end of
 * the stack, where it already is, so the master stays put; at its top,
 * just after the masters; next after focused, the window that had focus
 * as it opened, if that tiles beside it; or as the master. Where there's
 * nothing to go after, it stays at the end. */
static void
lt_place_new(Client *c, Client *focused)
{
	int rule = settings && settings->new_window >= 0 ? settings->new_window : NewEnd;
	Client *t, *after = NULL, *first = NULL;
	int masters, n = 0;
	if (rule == NewEnd || !c->mon || c->isfloating || c->isfullscreen)
		return;
	wl_list_remove(&c->link);
	if (rule == NewMaster) {
		wl_list_insert(&clients, &c->link);
	} else if (rule == NewTop) {
		masters = lt_masters(c->mon, c->ws);
		wl_list_for_each(t, &clients, link) {
			if (!lt_beside(t, c))
				continue;
			if (!first)
				first = t;
			if (n++ == masters)
				break;
			after = t;
		}
		/* Before the stack's first window, or with no stack, last */
		wl_list_insert(n <= masters ? clients.prev : after ? &after->link : first->link.prev,
				&c->link);
	} else if (focused && lt_beside(focused, c)) {
		wl_list_insert(&focused->link, &c->link);
	} else {
		wl_list_insert(clients.prev, &c->link);
	}
	arrange(c->mon);
}

/* Every part's mfact and nmaster, each layout's, from the defaults again,
 * once a load changes them, each part keeping its layouts */
static void
lt_reset_modes(void)
{
	Monitor *m;
	LtPart *p;
	wl_list_for_each(p, &lt_parts, link) {
		memset(p->modes, 0, sizeof(p->modes));
		lt_mode_defaults(p->output, p->lt[p->sellt], &p->mfact, &p->nmaster);
	}
	wl_list_for_each(m, &mons, link) {
		if (!m->lt_settled)
			continue;
		memset(m->modes, 0, sizeof(m->modes));
		lt_mode_defaults(m->wlr_output->name, m->lt[m->sellt], &m->mfact, &m->nmaster);
		arrange(m);
	}
}

/* Keeps m's layouts as ws's part there */
static void
lt_store(Monitor *m, Workspace *ws)
{
	LtPart *p = lt_part(ws, m, 1);
	memcpy(p->lt, m->lt, sizeof(p->lt));
	p->sellt = m->sellt;
	p->mfact = m->mfact;
	p->nmaster = m->nmaster;
	p->single = m->single;
	memcpy(p->modes, m->modes, sizeof(p->modes));
}

/* Gives m ws's part there, or a new one from the defaults */
static void
lt_restore(Monitor *m, Workspace *ws)
{
	LtPart *p = lt_part(ws, m, 0);
	if (!p) {
		lt_defaults_for(m);
		lt_store(m, ws);
		return;
	}
	memcpy(m->lt, p->lt, sizeof(m->lt));
	m->sellt = p->sellt;
	m->mfact = p->mfact;
	m->nmaster = p->nmaster;
	m->single = p->single;
	memcpy(m->modes, p->modes, sizeof(m->modes));
	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
}

/* The current workspace goes from old to ws: each monitor keeps its part
 * of old, if its fields hold it, and takes its part of ws once it has
 * geometry to choose defaults by: lt_settle gives a monitor without any,
 * a disabled one, its part when it's enabled. lt_announce says which
 * layouts they are, once the monitors are arranged. */
static void
lt_view(Workspace *old, Workspace *ws)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (old && m->lt_settled)
			lt_store(m, old);
		m->lt_settled = 0;
		if (m->m.width > 0 && m->m.height > 0) {
			lt_restore(m, ws);
			m->lt_settled = 1;
		}
	}
}

/* m's layout event, once it's arranged, which sets monocle's [n] symbol.
 * A monitor asleep, powered off with its geometry kept, can't be arranged,
 * so its event waits for lt_flush, once it wakes. */
static void
lt_event(Monitor *m)
{
	if (m->wlr_output->enabled)
		ipc_event_layout(m);
	else
		m->lt_pending = 1;
}

/* The layout event for each monitor holding its part of the current
 * workspace, after a switch has arranged them */
static void
lt_announce(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		if (m->lt_settled)
			lt_event(m);
}

/* Once m has its geometry, its part of the current workspace, if its
 * fields don't hold it yet: the one kept by its output's name, for a
 * monitor plugged in again, or the defaults, by its aspect. Its layout
 * event waits for lt_flush, once updatemons has placed every window. */
static void
lt_settle(Monitor *m)
{
	if (m->lt_settled || !curws)
		return;
	lt_restore(m, curws);
	m->lt_settled = m->lt_pending = 1;
}

/* The layout events held back, by lt_settle or for a monitor asleep:
 * after updatemons has arranged the monitors and given windows left
 * without one a monitor, so monocle's symbol counts them. One still off
 * keeps its event until it's on. */
static void
lt_flush(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		if (m->lt_pending && m->lt_settled)
			ipc_event_layout(m);
		m->lt_pending = 0;
	}
}

/* Keeps m's part of the current workspace, as m goes */
static void
lt_unplug(Monitor *m)
{
	if (m->lt_settled && curws)
		lt_store(m, curws);
	m->lt_settled = 0;
}

/* Drops a workspace's parts, as it goes */
static void
lt_forget(Workspace *ws)
{
	LtPart *p, *tmp;
	wl_list_for_each_safe(p, tmp, &lt_parts, link) {
		if (p->ws != ws)
			continue;
		wl_list_remove(&p->link);
		free(p->output);
		free(p);
	}
}

/* The layout ws's part on m is in, for the IPC: m's own for the current
 * workspace, if its fields hold it, else the kept one's, or NULL for a
 * part not made yet */
static const Layout *
lt_part_layout(Workspace *ws, Monitor *m)
{
	LtPart *p;
	if (!m)
		return NULL;
	if (ws == curws && m->lt_settled)
		return m->lt[m->sellt];
	return (p = lt_part(ws, m, 0)) ? p->lt[p->sellt] : NULL;
}

/* Drops every part, at exit */
static void
lt_finish(void)
{
	LtPart *p, *tmp;
	wl_list_for_each_safe(p, tmp, &lt_parts, link) {
		wl_list_remove(&p->link);
		free(p->output);
		free(p);
	}
}

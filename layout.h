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

/* Keeps old's mfact and nmaster on m, and gives m those it had for lt, or
 * lt's defaults: its own, else config.h's monitor rule's. */
static void
lt_switch(Monitor *m, const Layout *old, const Layout *lt)
{
	size_t i;
	int found = 0;
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
	for (i = 0; i < LENGTH(lt_defaults); i++) {
		if (lt->arrange == lt_defaults[i].arrange) {
			m->mfact = lt_defaults[i].mfact;
			m->nmaster = lt_defaults[i].nmaster;
			found = 1;
		}
	}
	if (!found) {
		m->mfact = m->rule_mfact;
		m->nmaster = m->rule_nmaster;
	}
}

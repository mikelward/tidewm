/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * `zoom next` and `zoom prev`, tide's Alt+Tab (its SPEC §6.6): bound to a
 * key with modifiers, the first press notes the focused window, each
 * further press while the modifiers are held moves focus to the next, or
 * the last, tiled window on the focused monitor, in stack order, and
 * letting go of the modifiers but Shift makes the focused window the
 * master, as `zoom` does, unless it's the master already. So a tap is
 * `zoom`. Escape while the modifiers are held
 * puts focus back where it was and zooms nothing. Run with no modifiers
 * held, as over the IPC, it's `zoom` at once. dwl.c includes this file,
 * like client.h.
 */

static struct {
	int armed;
	uint32_t mods; /* the modifiers whose letting go ends it */
	KeyboardGroup *group; /* the keyboard they're held on */
	Client *start; /* where focus was as it began */
	int stepped; /* whether focus moved since */
} zoomcycle;

/* The modifiers of the key press whose binding runs now, and its
 * keyboard, set by keypress() before it runs one, so `zoom next` knows
 * which to wait on, and where */
static uint32_t cycle_press_mods;
static KeyboardGroup *cycle_press_group;

/* Whether c is a window it steps to: tiled, on m, never a status popup,
 * which only the pointer focuses, as zoom() has them */
static int
cycle_takes(Client *c, Monitor *m)
{
	return VISIBLEON(c, m) && !c->isfloating && c->held != FgPopup;
}

/* The window after sel, or before it, on sel's monitor, in stack order,
 * round from the end to the start; sel with none other */
static Client *
cycle_step(Client *sel, int dir)
{
	Client *c, *first = NULL, *last = NULL, *before = NULL, *after = NULL;
	int passed = 0;
	wl_list_for_each(c, &clients, link) {
		if (!cycle_takes(c, sel->mon))
			continue;
		if (!first)
			first = c;
		last = c;
		if (c == sel)
			passed = 1;
		else if (!passed)
			before = c;
		else if (!after)
			after = c;
	}
	if (dir > 0)
		return after ? after : first;
	return before ? before : last;
}

static int
cycle_alive(Client *c)
{
	Client *other;
	wl_list_for_each(other, &clients, link)
		if (other == c)
			return 1;
	return 0;
}

/* `zoom next` (dir 1) or `zoom prev` (dir -1) */
static void
cycle_zoom(int dir)
{
	Client *sel = focustop(selmon);
	/* Over the IPC, say, even while a keyboard's is under way */
	if (!cycle_press_mods) {
		zoom(NULL);
		return;
	}
	if (!zoomcycle.armed) {
		zoomcycle.armed = 1;
		/* Shift, which turns Alt+Tab back, isn't one it waits on */
		zoomcycle.mods = cycle_press_mods & ~WLR_MODIFIER_SHIFT;
		if (!zoomcycle.mods)
			zoomcycle.mods = cycle_press_mods;
		zoomcycle.group = cycle_press_group;
		zoomcycle.start = sel;
		zoomcycle.stepped = 0;
		return;
	}
	if (sel && !sel->isfloating && cycle_takes(sel, sel->mon)) {
		Client *next = cycle_step(sel, dir);
		if (next && next != sel) {
			focusclient(next, NULL, 1);
			zoomcycle.stepped = 1;
		}
	} else {
		/* From a floating window, or none, to the first tiled one */
		Client *c;
		wl_list_for_each(c, &clients, link) {
			if (selmon && cycle_takes(c, selmon)) {
				focusclient(c, NULL, 1);
				zoomcycle.stepped = 1;
				break;
			}
		}
	}
}

/* Whether c is its monitor's master: the first window it steps to */
static int
cycle_is_master(Client *c)
{
	Client *first;
	wl_list_for_each(first, &clients, link)
		if (cycle_takes(first, c->mon))
			return first == c;
	return 0;
}

/* group's modifiers are now mods: one of those it waits on up, on the
 * keyboard they're held on, ends it, making the focused window the master. A tap is `zoom`, which
 * swaps the master with the next window; a window chosen that is the
 * master already stays it. */
static void
cycle_modifiers(KeyboardGroup *group, uint32_t mods)
{
	Client *sel;
	if (!zoomcycle.armed || group != zoomcycle.group
			|| (mods & zoomcycle.mods) == zoomcycle.mods)
		return;
	zoomcycle.armed = 0;
	zoomcycle.group = NULL;
	zoomcycle.start = NULL;
	sel = focustop(selmon);
	if (zoomcycle.stepped && sel && cycle_is_master(sel))
		return;
	/* Asked for, as its binding was, so it ends a focus grab and leaves
	 * a layer surface, as zoom from a binding does */
	focus_asked++;
	zoom(NULL);
	focus_asked--;
}

/* A key pressed while it's under way: Escape ends it with focus back
 * where it was, and is taken; any other goes on as it would */
static int
cycle_key(xkb_keysym_t sym)
{
	if (!zoomcycle.armed || sym != XKB_KEY_Escape)
		return 0;
	zoomcycle.armed = 0;
	zoomcycle.group = NULL;
	/* Only to a window still shown: on a workspace left since, it stays */
	if (zoomcycle.start && cycle_alive(zoomcycle.start)
			&& VISIBLEON(zoomcycle.start, zoomcycle.start->mon)) {
		focus_asked++; /* as a binding's is */
		focusclient(zoomcycle.start, NULL, 1);
		focus_asked--;
	}
	zoomcycle.start = NULL;
	return 1;
}

/* c is going: it's no longer where focus goes back to */
static void
cycle_unmapped(Client *c)
{
	if (zoomcycle.start == c)
		zoomcycle.start = NULL;
}

/* group is going, as a virtual keyboard does, with the modifiers held:
 * their letting go will never come, so it ends, focus where it is and no
 * window made the master */
static void
cycle_keyboard_gone(KeyboardGroup *group)
{
	if (!zoomcycle.armed || zoomcycle.group != group)
		return;
	zoomcycle.armed = 0;
	zoomcycle.group = NULL;
	zoomcycle.start = NULL;
}

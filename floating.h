/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Floating windows remember their place per app for the session, as
 * tide's SPEC §6.4 has it: wherever one of an app's floating windows last
 * opened, moved or was resized to, size and position, floating a window of
 * that app by hand puts it. A redraw where a window already is doesn't
 * count. The place is kept relative to its monitor's window area, so it
 * comes back on another monitor, kept inside that one's.
 *
 * A dialog, a fixed-size window and picture-in-picture place themselves
 * (placefloating), so theirs isn't kept, and they don't take the app's.
 * A window that floats as it opens, by a rule or its type, still goes in
 * the middle; one a drag floats stays where the drag takes it.
 */

/* An app's last floating window's place */
typedef struct {
	struct wl_list link;
	char *app_id;
	struct wlr_box box; /* relative to its monitor's window area */
} FlPlace;

static struct wl_list fl_places = {&fl_places, &fl_places};

/* Whether c's floating place is its app's: not a dialog, a fixed-size
 * window or picture-in-picture, which place themselves, nor a window with
 * no app ID */
static int
fl_apps(Client *c)
{
	const char *app_id = fg_raw_appid(c);
	return app_id && *app_id && !client_is_unmanaged(c) && !client_get_parent(c)
			&& !client_is_float_type(c) && !client_is_pip(c);
}

static FlPlace *
fl_place(Client *c)
{
	const char *app_id = fg_raw_appid(c);
	FlPlace *f;
	wl_list_for_each(f, &fl_places, link)
		if (!strcmp(f->app_id, app_id))
			return f;
	return NULL;
}

/* Keeps c's place for its app, if it's floating there: maximized or
 * fullscreen, it isn't in its own place, and before it maps it has none */
static void
fl_remember(Client *c)
{
	FlPlace *f;
	if (!c->mon || !client_surface(c)->mapped || !c->isfloating || c->isfullscreen
			|| c->ismaximized || !fl_apps(c))
		return;
	if (!(f = fl_place(c))) {
		f = ecalloc(1, sizeof(*f));
		if (!(f->app_id = strdup(fg_raw_appid(c))))
			die("strdup:");
		wl_list_insert(&fl_places, &f->link);
	}
	f->box = c->geom;
	f->box.x -= c->mon->w.x;
	f->box.y -= c->mon->w.y;
}

/* Floats c, or tiles it, as a key or a command asks. Floated, it goes to
 * its app's place, kept in its monitor's window area, or stays where it
 * was tiled, which becomes the app's place. */
static void
fl_by_hand(Client *c, int floating)
{
	struct wlr_box area, box;
	FlPlace *f;
	setfloating(c, floating);
	if (!floating || !c->mon || !fl_apps(c))
		return;
	if (!(f = fl_place(c))) {
		fl_remember(c);
		return;
	}
	area = c->mon->w;
	box = f->box;
	box.width = MIN(box.width, area.width);
	box.height = MIN(box.height, area.height);
	/* At the size it gets, so it's kept in the area at that size */
	fitsizehints(c, &box);
	box.x = MAX(area.x, MIN(area.x + box.x, area.x + area.width - box.width));
	box.y = MAX(area.y, MIN(area.y + box.y, area.y + area.height - box.height));
	/* A maximized one takes it once it isn't */
	if (c->ismaximized)
		c->unmaximized = box;
	else
		resize(c, box, 0);
}

static void
fl_finish(void)
{
	FlPlace *f, *tmp;
	wl_list_for_each_safe(f, tmp, &fl_places, link) {
		wl_list_remove(&f->link);
		free(f->app_id);
		free(f);
	}
}

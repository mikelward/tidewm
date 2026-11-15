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
 *
 * A floating window's shadow is where a drag resizes it, as tide's SPEC
 * §6.4 has it: a plain left press just outside its edge or a corner,
 * as far out as the shadow reaches, drags that edge or corner until a
 * button comes up, and the pointer shows a resize cursor there. With
 * shadows off, or a shadow_blur_radius of 0, that's FL_GRAB pixels. A
 * window that casts no shadow now, as an unfocused one with shadows
 * active, has the same zone. A press inside the window is the app's, and
 * one in the zone never reaches what's under it. What's over the zone
 * takes the press: another floating window, a bar or a notification, a
 * popup, or a fullscreen window or its dialog.
 */

#define FL_GRAB 8 /* the zone with no shadow, in pixels */

enum { FlLeft = 1, FlRight = 2, FlTop = 4, FlBottom = 8 }; /* the sides a drag moves */

/* The resize on, of grabc, while cursor_mode is CurBorder */
static struct {
	int sides;
	struct wlr_box box; /* the window's at the press */
	double x, y; /* the pointer's at the press */
} fl_drag;

static int shadow_mode(void);
static int shadow_range(void);

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

/* How far outside a floating window a press resizes it: as far as a
 * shadow reaches, or FL_GRAB with none */
static int
fl_reach(void)
{
	int r = shadow_mode() != ShadowsNone ? shadow_range() : 0;
	return r > 0 ? r : FL_GRAB;
}

static int
fl_inside(const struct wlr_box *b, double x, double y, int grown)
{
	return x >= b->x - grown && y >= b->y - grown && x < b->x + b->width + grown
			&& y < b->y + b->height + grown;
}

/* The floating window whose resize zone holds x, y, with the sides a
 * press there drags in *sides, or NULL if the press goes on as usual */
static Client *
fl_zone_at(double x, double y, int *sides)
{
	static const int lyrs[] = {LyrFS, LyrFloat, LyrTile}; /* topmost first */
	struct wlr_scene_node *node;
	struct wlr_surface *surface = NULL;
	Client *c, *found = NULL, *over = NULL;
	LayerSurface *l = NULL;
	const struct wlr_box *g;
	int r = fl_reach(), corner, s = 0;
	size_t i;
	/* A popup's pointer grab, a menu's, or a focus grab: a press outside
	 * it closes it */
	if (locked || seat->drag || active_constraint || grab_active
			|| seat->pointer_state.grab != seat->pointer_state.default_grab)
		return NULL;
	/* The topmost window there: one the point is inside takes the press,
	 * unless it's a tile, which is under every floating window. A
	 * fullscreen window's dialog is up with it, over the floating ones. */
	for (i = 0; !found && i < LENGTH(lyrs); i++) {
		wl_list_for_each_reverse(node, &layers[lyrs[i]]->children, link) {
			wl_list_for_each(c, &clients, link)
				if (&c->scene->node == node)
					break;
			if (&c->link == &clients || !node->enabled || !c->mon
					|| !VISIBLEON(c, c->mon))
				continue;
			if (fl_inside(&c->geom, x, y, 0)) {
				if (lyrs[i] == LyrFS || c->isfloating
						|| !c->mon->lt[c->mon->sellt]->arrange)
					return NULL;
				continue;
			}
			if (c->isfloating && !c->isfullscreen && !c->ismaximized
					&& fl_inside(&c->geom, x, y, r)) {
				found = c;
				break;
			}
		}
	}
	if (!found)
		return NULL;
	/* What's over the floating windows there: a bar, a notification or
	 * another top or overlay layer, an unmanaged or fullscreen window,
	 * or a window's popup */
	xytonode(x, y, &surface, &over, &l, NULL, NULL);
	if (l && l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		return NULL;
	if (over && (client_is_unmanaged(over)
			|| (over->isfullscreen && found->scene->node.parent != layers[LyrFS])
			|| wlr_surface_get_root_surface(surface) != client_surface(over)))
		return NULL;
	g = &found->geom;
	if (x < g->x)
		s |= FlLeft;
	else if (x >= g->x + g->width)
		s |= FlRight;
	if (y < g->y)
		s |= FlTop;
	else if (y >= g->y + g->height)
		s |= FlBottom;
	/* Beside the end of an edge, its corner */
	if (!(s & (FlLeft | FlRight))) {
		corner = MIN(MIN(r, 2 * FL_GRAB), g->width / 3);
		s |= x < g->x + corner ? FlLeft : x >= g->x + g->width - corner ? FlRight : 0;
	} else if (!(s & (FlTop | FlBottom))) {
		corner = MIN(MIN(r, 2 * FL_GRAB), g->height / 3);
		s |= y < g->y + corner ? FlTop : y >= g->y + g->height - corner ? FlBottom : 0;
	}
	*sides = s;
	return found;
}

/* The cursor that says which way sides resize */
static const char *
fl_cursor(int sides)
{
	switch (sides) {
	case FlLeft | FlTop: return "nw-resize";
	case FlRight | FlTop: return "ne-resize";
	case FlLeft | FlBottom: return "sw-resize";
	case FlRight | FlBottom: return "se-resize";
	case FlTop: case FlBottom: return "ns-resize";
	default: return "ew-resize";
	}
}

/* Starts resizing c by sides from the pointer at x, y */
static void
fl_start(Client *c, int sides, double x, double y)
{
	grabc = c;
	fl_drag.sides = sides;
	fl_drag.box = c->geom;
	fl_drag.x = x;
	fl_drag.y = y;
	cursor_mode = CurBorder;
	/* The windows don't see the drag */
	wlr_seat_pointer_clear_focus(seat);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, fl_cursor(sides));
}

/* The pointer at x, y, during a resize: the sides dragged keep their
 * distance from it, and the others stay put, at the sizes the window
 * takes */
static void
fl_follow(double x, double y)
{
	Client *c = grabc;
	struct wlr_box box = fl_drag.box;
	int dx = (int)round(x - fl_drag.x), dy = (int)round(y - fl_drag.y);
	int bw;
	if (!c)
		return;
	bw = 2 * (int)c->bw;
	if (fl_drag.sides & (FlLeft | FlRight))
		box.width += fl_drag.sides & FlLeft ? -dx : dx;
	if (fl_drag.sides & (FlTop | FlBottom))
		box.height += fl_drag.sides & FlTop ? -dy : dy;
	box.width = MAX(box.width, 1 + bw);
	box.height = MAX(box.height, 1 + bw);
	fitsizehints(c, &box);
	if (fl_drag.sides & FlLeft)
		box.x = fl_drag.box.x + fl_drag.box.width - box.width;
	if (fl_drag.sides & FlTop)
		box.y = fl_drag.box.y + fl_drag.box.height - box.height;
	resize(c, box, 1);
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

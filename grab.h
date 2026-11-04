/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Focus grabs, over hyprland-focus-grab-v1, which Quickshell's
 * HyprlandFocusGrab speaks: tide's notification center closes on a click
 * outside it, but not when the pointer crosses away, as tide's SPEC §9 has
 * it. While a grab holds, the keyboard stays on its surfaces and the
 * pointer enters only them. A click anywhere else ends it and goes no
 * further, as a menu's does. So does focus you move, by a binding or a
 * command, the launcher opening, or the lock; a window opening doesn't
 * take the keyboard meanwhile. A second grab starting ends the first, and
 * one can't start under the lock.
 */

typedef struct FocusGrab FocusGrab;

/* A surface a grab names: pending until the client commits it */
typedef struct {
	struct wl_list link;
	FocusGrab *grab;
	struct wlr_surface *surface;
	struct wl_listener destroy, map, unmap;
	int pending, current;
} GrabSurface;

struct FocusGrab {
	struct wl_resource *resource;
	struct wl_list surfaces; /* GrabSurface.link */
};

static FocusGrab *grab_active; /* the grab that holds now, if any */
static int grab_focusing; /* the grab moving the keyboard to its own surface */
static uint32_t grab_ate; /* the button of the click that ended a grab: its release goes nowhere too */

static int
grab_names(FocusGrab *g, struct wlr_surface *surface)
{
	GrabSurface *gs;
	wl_list_for_each(gs, &g->surfaces, link)
		if (gs->current && gs->surface == surface)
			return 1;
	return 0;
}

static int
grab_any(FocusGrab *g)
{
	GrabSurface *gs;
	wl_list_for_each(gs, &g->surfaces, link)
		if (gs->current)
			return 1;
	return 0;
}

/* Whether g has surface: it, its root, or the window or layer surface it
 * belongs to, as a menu does, is one of g's */
static int
grab_covers(FocusGrab *g, struct wlr_surface *surface)
{
	Client *c = NULL;
	LayerSurface *l = NULL;
	if (!surface)
		return 0;
	if (grab_names(g, surface) || grab_names(g, wlr_surface_get_root_surface(surface)))
		return 1;
	toplevel_from_wlr_surface(surface, &c, &l);
	return (c && grab_names(g, client_surface(c)))
			|| (l && grab_names(g, l->layer_surface->surface));
}

/* Whether surface goes when gone does: it's gone itself, a subsurface of
 * it, or a menu of the window or layer surface gone is */
static int
grab_goes_with(struct wlr_surface *surface, struct wlr_surface *gone)
{
	Client *c = NULL;
	LayerSurface *l = NULL;
	if (!surface || !gone)
		return 0;
	if (surface == gone || wlr_surface_get_root_surface(surface) == gone)
		return 1;
	toplevel_from_wlr_surface(surface, &c, &l);
	return (c && client_surface(c) == gone) || (l && l->layer_surface->surface == gone);
}

/* Whether one of g's surfaces, and not one going with gone, is mapped: a
 * grab holds input only while one is */
static int
grab_live(FocusGrab *g, struct wlr_surface *gone)
{
	GrabSurface *gs;
	wl_list_for_each(gs, &g->surfaces, link)
		if (gs->current && gs->surface->mapped && !grab_goes_with(gs->surface, gone))
			return 1;
	return 0;
}

/* Whether input may go to surface now: always, unless a grab holds and
 * surface isn't one of its */
static int
grab_accepts(struct wlr_surface *surface)
{
	return !grab_active || !grab_live(grab_active, NULL) || grab_covers(grab_active, surface);
}

/* Whether a grab holds and has the keyboard on one of its surfaces, where
 * a click inside the grab leaves it */
static int
grab_has_keyboard(void)
{
	return grab_active && grab_live(grab_active, NULL)
			&& grab_covers(grab_active, seat->keyboard_state.focused_surface);
}

static void
grab_surface_free(GrabSurface *gs)
{
	wl_list_remove(&gs->link);
	wl_list_remove(&gs->destroy.link);
	wl_list_remove(&gs->map.link);
	wl_list_remove(&gs->unmap.link);
	free(gs);
}

/* The compositor ends the grab that holds: its surfaces are dropped, and
 * its client hears */
static void
grab_clear(void)
{
	FocusGrab *g = grab_active;
	GrabSurface *gs, *tmp;
	if (!g)
		return;
	grab_active = NULL;
	wl_list_for_each_safe(gs, tmp, &g->surfaces, link)
		grab_surface_free(gs);
	hyprland_focus_grab_v1_send_cleared(g->resource);
	/* The pointer may enter what's under it again */
	motionnotify(0, NULL, 0, 0, 0, 0);
}

/* Gives the keyboard to one of the grab's surfaces, unless one has it
 * that isn't going with gone: to its window or layer surface, or, if the
 * grab names only a menu of it, to the menu itself. A surface not yet
 * mapped waits: it takes focus when it maps, as the grab names it. An
 * exclusive layer with the keyboard, such as the launcher, keeps it, and
 * the grab ends rather than hold beside it. */
static void
grab_focus(FocusGrab *g, struct wlr_surface *gone)
{
	struct wlr_surface *focused = seat->keyboard_state.focused_surface;
	GrabSurface *gs;
	Client *c, *old_c = NULL;
	LayerSurface *l, *old_l = NULL;
	if (grab_covers(g, focused) && !grab_goes_with(focused, gone))
		return;
	toplevel_from_wlr_surface(focused, &old_c, &old_l);
	if (old_l && !grab_goes_with(focused, gone)
			&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP
			&& old_l->layer_surface->current.keyboard_interactive
				== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		if (grab_live(g, gone))
			grab_clear();
		return;
	}
	wl_list_for_each(gs, &g->surfaces, link) {
		c = NULL;
		l = NULL;
		if (!gs->current || !gs->surface->mapped || grab_goes_with(gs->surface, gone)
				|| toplevel_from_wlr_surface(gs->surface, &c, &l) < 0)
			continue;
		focus_asked++;
		grab_focusing++;
		if (grab_names(g, c ? client_surface(c) : l->layer_surface->surface)) {
			focusclient(c, l, c != NULL);
		} else {
			/* Focus leaves another window or layer surface first, but not
			 * the menu's own, which would close the menu */
			if ((c && c != old_c) || (l && l != old_l))
				focusclient(NULL, NULL, 0);
			input_method_relay_set_focus(input_method_relay, gs->surface);
			client_notify_enter(gs->surface, wlr_seat_get_keyboard(seat));
		}
		grab_focusing--;
		focus_asked--;
		return;
	}
}

/* A surface maps: if the grab names it, and has no keyboard yet, it may
 * have it now */
static void
grab_mapped(struct wlr_surface *surface)
{
	if (!grab_active || !grab_names(grab_active, surface))
		return;
	grab_focus(grab_active, NULL);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

/* A click outside the grab's surfaces ends it, and goes no further.
 * Returns whether it was such a click. */
static int
grab_click(struct wlr_surface *surface, uint32_t button)
{
	if (grab_accepts(surface))
		return 0;
	grab_clear();
	grab_ate = button;
	return 1;
}

/* Whether a button's release is of the click that ended a grab */
static int
grab_release(uint32_t button)
{
	if (!grab_ate || grab_ate != button)
		return 0;
	grab_ate = 0;
	return 1;
}

/* Whether the grab keeps the keyboard as focus moves to c or l, or to
 * nothing: not to one of its surfaces, nor if you asked, or an exclusive
 * layer, such as the launcher, opens, which end the grab, nor while none
 * of its surfaces is mapped */
static int
grab_keeps(Client *c, LayerSurface *l)
{
	struct wlr_surface *target = c ? client_surface(c) : l ? l->layer_surface->surface : NULL;
	if (!grab_active || grab_focusing || grab_covers(grab_active, target))
		return 0;
	if (focus_asked || (l && l->layer_surface->current.keyboard_interactive
			== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)) {
		grab_clear();
		return 0;
	}
	return grab_live(grab_active, NULL);
}

/* A menu maps: grab_mapped, as for windows and layer surfaces, which wait
 * for mapnotify and commitlayersurfacenotify to place them first */
static void
grab_surface_mapped(struct wl_listener *listener, void *data)
{
	GrabSurface *gs = wl_container_of(listener, gs, map);
	if (wlr_xdg_popup_try_from_wlr_surface(gs->surface))
		grab_mapped(gs->surface);
}

/* A surface the grab names unmaps, or goes: if the keyboard goes with it,
 * another of the grab's surfaces has it */
static void
grab_surface_unmapped(struct wl_listener *listener, void *data)
{
	GrabSurface *gs = wl_container_of(listener, gs, unmap);
	if (gs->grab == grab_active && gs->current)
		grab_focus(gs->grab, gs->surface);
}

/* A surface the grab names goes. If the keyboard is still on it, as on a
 * menu's, which nothing gives back on unmapping as dwl does a window's or
 * a layer surface's, it goes where it would without a grab. */
static void
grab_surface_destroyed(struct wl_listener *listener, void *data)
{
	GrabSurface *gs = wl_container_of(listener, gs, destroy);
	FocusGrab *g = gs->grab;
	int keyboard = seat->keyboard_state.focused_surface == gs->surface;
	grab_surface_free(gs);
	if (g != grab_active)
		return;
	if (!grab_any(g)) {
		grab_active = NULL;
		motionnotify(0, NULL, 0, 0, 0, 0);
	}
	if (keyboard && (!grab_active || !grab_live(grab_active, NULL)))
		focusclient(focustop(selmon), NULL, 1);
}

static GrabSurface *
grab_find(FocusGrab *g, struct wlr_surface *surface)
{
	GrabSurface *gs;
	wl_list_for_each(gs, &g->surfaces, link)
		if (gs->surface == surface)
			return gs;
	return NULL;
}

static void
grab_handle_add_surface(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *surface_resource)
{
	FocusGrab *g = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	GrabSurface *gs = grab_find(g, surface);
	if (!gs) {
		gs = ecalloc(1, sizeof(*gs));
		gs->grab = g;
		gs->surface = surface;
		gs->destroy.notify = grab_surface_destroyed;
		wl_signal_add(&surface->events.destroy, &gs->destroy);
		gs->map.notify = grab_surface_mapped;
		wl_signal_add(&surface->events.map, &gs->map);
		gs->unmap.notify = grab_surface_unmapped;
		wl_signal_add(&surface->events.unmap, &gs->unmap);
		wl_list_insert(g->surfaces.prev, &gs->link);
	}
	gs->pending = 1;
}

static void
grab_handle_remove_surface(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *surface_resource)
{
	FocusGrab *g = wl_resource_get_user_data(resource);
	GrabSurface *gs = grab_find(g, wlr_surface_from_resource(surface_resource));
	if (gs)
		gs->pending = 0;
}

static void
grab_handle_commit(struct wl_client *client, struct wl_resource *resource)
{
	FocusGrab *g = wl_resource_get_user_data(resource);
	GrabSurface *gs, *tmp;
	wl_list_for_each_safe(gs, tmp, &g->surfaces, link)
		if (!(gs->current = gs->pending))
			grab_surface_free(gs);
	if (!grab_any(g)) {
		if (grab_active == g) {
			grab_active = NULL;
			motionnotify(0, NULL, 0, 0, 0, 0);
		}
		return;
	}
	if (grab_active != g)
		grab_clear();
	grab_active = g;
	/* The lock has the keyboard and pointer: a grab can't start under it */
	if (locked) {
		grab_clear();
		return;
	}
	grab_focus(g, NULL);
	/* The pointer leaves what it may no longer enter */
	motionnotify(0, NULL, 0, 0, 0, 0);
}

static void
grab_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
grab_resource_destroy(struct wl_resource *resource)
{
	FocusGrab *g = wl_resource_get_user_data(resource);
	GrabSurface *gs, *tmp;
	int held = grab_active == g;
	if (held)
		grab_active = NULL;
	wl_list_for_each_safe(gs, tmp, &g->surfaces, link)
		grab_surface_free(gs);
	free(g);
	/* The pointer may enter what's under it again */
	if (held)
		motionnotify(0, NULL, 0, 0, 0, 0);
}

static const struct hyprland_focus_grab_v1_interface grab_impl = {
	.add_surface = grab_handle_add_surface,
	.remove_surface = grab_handle_remove_surface,
	.commit = grab_handle_commit,
	.destroy = grab_handle_destroy,
};

static void
grab_manager_create(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	FocusGrab *g = ecalloc(1, sizeof(*g));
	if (!(g->resource = wl_resource_create(client, &hyprland_focus_grab_v1_interface,
			wl_resource_get_version(resource), id))) {
		free(g);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&g->surfaces);
	wl_resource_set_implementation(g->resource, &grab_impl, g, grab_resource_destroy);
}

static void
grab_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct hyprland_focus_grab_manager_v1_interface grab_manager_impl = {
	.create_grab = grab_manager_create,
	.destroy = grab_manager_destroy,
};

static void
grab_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&hyprland_focus_grab_manager_v1_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &grab_manager_impl, NULL, NULL);
}

/* tidewm is going: its clients' grabs go with them, and nothing is left
 * for the pointer to enter */
static void
grab_finish(void)
{
	grab_active = NULL;
}

static void
grab_init(void)
{
	if (!wl_global_create(dpy, &hyprland_focus_grab_manager_v1_interface, 1, NULL, grab_bind))
		die("can't make the focus grab global");
}

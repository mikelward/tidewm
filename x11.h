/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * X11 windows sharp at a fractional scale, as tide's SPEC §17 has it.
 * With xwayland_scaling disable, as Hyprland's xwayland:force_zero_scaling,
 * X11's pixels are the outputs' own: an X11 window is configured in its
 * output's pixels, and the scene shows its buffer at that size over the
 * output's scale, one pixel for one, so it's sharp. An app that scales
 * itself, by GDK_SCALE, QT_SCALE_FACTOR or Xft.dpi, is then the size it
 * would be scaled; one that doesn't is small. By default X11 windows are
 * scaled, as in sway, and blurry at a fractional scale.
 *
 * X11 has one space for every output, so each output's box in it is its
 * place in the layout spread by the largest scale, so that none overlap,
 * and its size in its own pixels. Xwayland hears of those boxes over a
 * zxdg_output_manager_v1 of tidewm's own, which only Xwayland sees, in
 * place of wlroots' one, so its screen and RandR match. Xwayland's shell
 * global is for Xwayland alone too, as wlroots asks.
 */

static int x11_unscaled; /* xwayland_scaling disable: X11's pixels are the outputs' */
static struct wlr_xdg_output_manager_v1 *x11_wlr_xdg_outputs; /* wlroots', for everyone else */

#ifdef XWAYLAND
/* A zxdg_output_v1 of Xwayland's, for an output, which may have gone */
typedef struct {
	struct wl_resource *resource;
	struct wlr_output *output;
	struct wl_listener output_destroy;
	struct wl_list link; /* x11_xdg_outputs */
} X11Output;

static struct wl_global *x11_xdg_output_manager; /* Xwayland's zxdg_output_manager_v1 */
static struct wl_list x11_xdg_outputs = {&x11_xdg_outputs, &x11_xdg_outputs};

/* The scale X11 has on m: its own, unscaled, or 1 */
static float
x11_scale(Monitor *m)
{
	return x11_unscaled && m && m->wlr_output->enabled ? m->wlr_output->scale : 1;
}

/* How far X11's space spreads the layout: by the largest scale, so the
 * outputs' boxes, each its own pixels, don't overlap */
static float
x11_spread(void)
{
	Monitor *m;
	float spread = 1;
	wl_list_for_each(m, &mons, link)
		if (x11_scale(m) > spread)
			spread = x11_scale(m);
	return spread;
}

/* m's box in X11's space */
static struct wlr_box
x11_box(Monitor *m)
{
	struct wlr_box b = m->m;
	float spread;
	if (!x11_unscaled || !m->wlr_output->enabled)
		return b;
	spread = x11_spread();
	b.x = (int)lroundf((float)m->m.x * spread);
	b.y = (int)lroundf((float)m->m.y * spread);
	wlr_output_transformed_resolution(m->wlr_output, &b.width, &b.height);
	return b;
}

/* The monitor whose box in X11's space has x, y, or else the selected one */
static Monitor *
x11_monitor_at(int x, int y)
{
	Monitor *m;
	struct wlr_box b;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		b = x11_box(m);
		if (wlr_box_contains_point(&b, x, y))
			return m;
	}
	return selmon;
}

/* x, y in X11's space from lx, ly in the layout, on m */
static void
x11_from_layout(Monitor *m, int lx, int ly, int *x, int *y)
{
	struct wlr_box b;
	float s = x11_scale(m);
	if (!x11_unscaled || !m) {
		*x = lx;
		*y = ly;
		return;
	}
	b = x11_box(m);
	*x = b.x + (int)lroundf((float)(lx - m->m.x) * s);
	*y = b.y + (int)lroundf((float)(ly - m->m.y) * s);
}

/* lx, ly in the layout from x, y in X11's space, on the monitor that has
 * them, which it returns */
static Monitor *
x11_to_layout(int x, int y, int *lx, int *ly)
{
	Monitor *m = x11_monitor_at(x, y);
	struct wlr_box b;
	float s = x11_scale(m);
	if (!x11_unscaled || !m) {
		*lx = x;
		*ly = y;
		return m;
	}
	b = x11_box(m);
	*lx = m->m.x + (int)lroundf((float)(x - b.x) / s);
	*ly = m->m.y + (int)lroundf((float)(y - b.y) / s);
	return m;
}

/* c's X11 window's geometry, from X11's space into the layout: its size
 * there over the scale of the monitor it's on */
static void
x11_geometry(Client *c, struct wlr_box *geom)
{
	struct wlr_xwayland_surface *xs = c->surface.xwayland;
	c->xscale = x11_scale(x11_to_layout(xs->x, xs->y, &geom->x, &geom->y));
	geom->width = (int)lroundf((float)xs->width / c->xscale);
	geom->height = (int)lroundf((float)xs->height / c->xscale);
}

/* Configures c's X11 window at its place in the layout, width by height
 * there, in its monitor's pixels */
static void
x11_configure(Client *c, uint32_t width, uint32_t height)
{
	int x, y;
	c->xscale = x11_scale(c->mon);
	x11_from_layout(c->mon, c->geom.x + (int)c->bw, c->geom.y + (int)c->bw, &x, &y);
	wlr_xwayland_surface_configure(c->surface.xwayland, (int16_t)x, (int16_t)y,
			(uint16_t)lroundf((float)width * c->xscale),
			(uint16_t)lroundf((float)height * c->xscale));
}

/* Whether a point on an X11 window's buffer, in the layout's units from
 * its corner, takes input, and where that is in the window's own pixels */
static bool
x11_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
	struct wlr_xwayland_surface *xs;
	Client *c;
	if (!ss)
		return false;
	/* Its clip starts at its corner, so adds nothing (client_get_clip) */
	if ((xs = wlr_xwayland_surface_try_from_wlr_surface(ss->surface)) && (c = xs->data)) {
		*sx *= c->xscale;
		*sy *= c->xscale;
	}
	return wlr_surface_point_accepts_input(ss->surface, *sx, *sy);
}

/* Shows buffer, of c's X11 window, at its size over c's scale: as wlroots'
 * scene works it out, its own size clipped, which for a window a monitor
 * has is resize's clip from its corner (client_get_clip). Its opaque
 * region, in its own pixels, needs nothing: rootless Xwayland sets none,
 * and the scene keeps one within the buffer's size. */
static void
x11_scale_buffer(Client *c, struct wlr_scene_buffer *buffer)
{
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
	int width, height;
	if (!ss)
		return;
	buffer->point_accepts_input = x11_accepts_input;
	/* At 1 too, as a window that was scaled may be no longer */
	width = ss->surface->current.width;
	height = ss->surface->current.height;
	if (c->mon) {
		width = MIN(width, (int)lroundf((float)(c->geom.width - (int)c->bw) * c->xscale));
		height = MIN(height, (int)lroundf((float)(c->geom.height - (int)c->bw) * c->xscale));
	}
	if (width <= 0 || height <= 0)
		return;
	wlr_scene_buffer_set_dest_size(buffer, (int)lroundf((float)width / c->xscale),
			(int)lroundf((float)height / c->xscale));
}

static void
x11_scale_node(Client *c, struct wlr_scene_node *node)
{
	struct wlr_scene_node *child;
	if (node->type == WLR_SCENE_NODE_BUFFER)
		x11_scale_buffer(c, wlr_scene_buffer_from_node(node));
	else if (node->type == WLR_SCENE_NODE_TREE)
		wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
			x11_scale_node(c, child);
}

/* c's scene shows its X11 window at its size in the layout, shown or not:
 * at map, and after wlroots' scene sizes it again, as the window commits
 * or is clipped */
static void
x11_scale_scene(Client *c)
{
	if (c->type == X11 && c->scene_surface)
		x11_scale_node(c, &c->scene_surface->node);
}

static void
x11_committed(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, x11_commit);
	x11_scale_scene(c);
}

/* Xwayland's xdg-output: an output's box in X11's space */
static void
x11_output_send(X11Output *o)
{
	Monitor *m = o->output ? o->output->data : NULL;
	struct wlr_box b;
	if (!m)
		return;
	b = x11_box(m);
	zxdg_output_v1_send_logical_position(o->resource, b.x, b.y);
	zxdg_output_v1_send_logical_size(o->resource, b.width, b.height);
	if (wl_resource_get_version(o->resource) >= 3)
		wlr_output_schedule_done(o->output);
	else
		zxdg_output_v1_send_done(o->resource);
}

static void
x11_output_gone(struct wl_listener *listener, void *data)
{
	X11Output *o = wl_container_of(listener, o, output_destroy);
	wl_list_remove(&o->output_destroy.link);
	wl_list_init(&o->output_destroy.link);
	o->output = NULL;
}

static void
x11_output_resource_destroy(struct wl_resource *resource)
{
	X11Output *o = wl_resource_get_user_data(resource);
	wl_list_remove(&o->output_destroy.link);
	wl_list_remove(&o->link);
	free(o);
}

static void
x11_output_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface x11_output_impl = {
	.destroy = x11_output_handle_destroy,
};

static void
x11_manager_get_output(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *output_resource)
{
	X11Output *o = ecalloc(1, sizeof(*o));
	if (!(o->resource = wl_resource_create(client, &zxdg_output_v1_interface,
			wl_resource_get_version(resource), id))) {
		free(o);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(o->resource, &x11_output_impl, o, x11_output_resource_destroy);
	wl_list_insert(&x11_xdg_outputs, &o->link);
	wl_list_init(&o->output_destroy.link);
	o->output_destroy.notify = x11_output_gone;
	/* An output already gone leaves it inert */
	if (!(o->output = wlr_output_from_resource(output_resource)))
		return;
	wl_signal_add(&o->output->events.destroy, &o->output_destroy);
	if (wl_resource_get_version(o->resource) >= 2) {
		zxdg_output_v1_send_name(o->resource, o->output->name);
		if (o->output->description)
			zxdg_output_v1_send_description(o->resource, o->output->description);
	}
	x11_output_send(o);
}

static void
x11_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface x11_manager_impl = {
	.destroy = x11_manager_destroy,
	.get_xdg_output = x11_manager_get_output,
};

static void
x11_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&zxdg_output_manager_v1_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &x11_manager_impl, NULL, NULL);
}

/* Xwayland sees tidewm's xdg-output, in X11's space, rather than
 * wlroots', and its own shell, which no one else sees */
static bool
x11_filter(const struct wl_client *client, const struct wl_global *global, void *data)
{
	const struct wl_client *x = xwayland && xwayland->server ? xwayland->server->client : NULL;
	if (global == x11_xdg_output_manager
			|| (xwayland && xwayland->shell_v1 && global == xwayland->shell_v1->global))
		return client == x;
	return !(x && client == x && x11_wlr_xdg_outputs && global == x11_wlr_xdg_outputs->global);
}

static void
x11_init(void)
{
	if (!(x11_xdg_output_manager = wl_global_create(dpy, &zxdg_output_manager_v1_interface,
			3, NULL, x11_manager_bind)))
		die("can't make Xwayland's xdg-output global");
	wl_display_set_global_filter(dpy, x11_filter, NULL);
}

/* The outputs, or how X11 scales, changed: Xwayland hears their boxes
 * again, and X11 windows are configured and shown again */
static void
x11_update(void)
{
	X11Output *o;
	Client *c;
	struct wlr_scene_node *node;
	wl_list_for_each(o, &x11_xdg_outputs, link)
		x11_output_send(o);
	wl_list_for_each(c, &clients, link)
		if (c->type == X11 && c->mon && c->scene_surface)
			resize(c, c->geom, 0);
	/* And its menus, in no list but their layer's, from where they are */
	wl_list_for_each(node, &layers[LyrUnmanaged]->children, link) {
		if (!(c = node->data) || c->type != X11)
			continue;
		x11_geometry(c, &c->geom);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		x11_scale_scene(c);
	}
}
#endif

/* xwayland_scaling, from the config: whether X11 windows are scaled */
static void
x11_set_scaling(int scaled)
{
	if (x11_unscaled == !scaled)
		return;
	x11_unscaled = !scaled;
#ifdef XWAYLAND
	x11_update();
#endif
}

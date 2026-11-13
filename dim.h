/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Dimming, tide's SPEC §6.2: every window but the focused one is dimmed,
 * by a translucent black layer over it at the dim_inactive strength, and
 * clicks go through the layer to the window. Where there's nothing to tell
 * apart, a monitor showing one window or a fullscreen one, nothing dims;
 * nor does a window that would look broken dimmed: picture-in-picture,
 * video by its content type, and one the dim command or a for_window rule
 * exempts. A window's menus and tooltips stay over the layer. As focus
 * moves, the dim fades (effects.h), unless the window wasn't shown before.
 */

/* The layer's one black pixel, which each window's scene buffer stretches */
typedef struct {
	struct wlr_buffer base;
	uint32_t pixel;
} DimPixel;

static DimPixel *dim_pixel;
static struct wlr_content_type_manager_v1 *dim_content_types;

static void
dim_pixel_destroy(struct wlr_buffer *buffer)
{
	DimPixel *p = wl_container_of(buffer, p, base);
	wlr_buffer_finish(buffer);
	free(p);
}

static bool
dim_pixel_access(struct wlr_buffer *buffer, uint32_t flags, void **data,
		uint32_t *format, size_t *stride)
{
	DimPixel *p = wl_container_of(buffer, p, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = &p->pixel;
	*format = DRM_FORMAT_ARGB8888;
	*stride = sizeof(p->pixel);
	return true;
}

static void
dim_pixel_end(struct wlr_buffer *buffer)
{
}

static const struct wlr_buffer_impl dim_pixel_impl = {
	.destroy = dim_pixel_destroy,
	.begin_data_ptr_access = dim_pixel_access,
	.end_data_ptr_access = dim_pixel_end,
};

/* Clicks and the pointer go through the layer to the window under it */
static bool
dim_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static void
dim_init(void)
{
	dim_pixel = ecalloc(1, sizeof(*dim_pixel));
	wlr_buffer_init(&dim_pixel->base, &dim_pixel_impl, 1, 1);
	dim_pixel->pixel = 0xff000000;
	dim_content_types = wlr_content_type_manager_v1_create(dpy, 1);
}

static void
dim_finish(void)
{
	/* Freed once the last window's layer lets it go */
	if (dim_pixel)
		wlr_buffer_drop(&dim_pixel->base);
	dim_pixel = NULL;
}

/* The strength: the config file's, or config.h's without one */
static float
dim_strength(void)
{
	return settings && settings->dim_inactive >= 0 ? (float)settings->dim_inactive
			: dim_inactive;
}

/* Gives c its layer, off, above its surface and borders but under the
 * menus it opens later */
static void
dim_attach(Client *c)
{
	c->dim = wlr_scene_buffer_create(c->scene, &dim_pixel->base);
	c->dim->point_accepts_input = dim_no_input;
	wlr_scene_node_set_enabled(&c->dim->node, 0);
}

/* How many windows m shows: a fullscreen one on top hides the rest */
static int
dim_shown(Monitor *m)
{
	Client *c = focustop(m);
	int n = 0;
	if (c && c->isfullscreen)
		return 1;
	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !client_is_unmanaged(c))
			n++;
	return n;
}

static int
dim_video(Client *c)
{
#ifdef XWAYLAND
	if (client_is_x11(c))
		return 0;
#endif
	return wlr_surface_get_content_type_v1(dim_content_types, client_surface(c))
			== WP_CONTENT_TYPE_V1_TYPE_VIDEO;
}

static int
dim_wanted(Client *c, float strength)
{
	return strength > 0 && c != focused_client && c->mon && VISIBLEON(c, c->mon)
			&& !c->nodim && !client_is_pip(c) && !dim_video(c)
			&& dim_shown(c->mon) > 1;
}

/* Shows c's dim at the strength it has reached, as far in as c is while
 * it fades in as it opens (effects.h) */
static void
dim_apply(Client *c)
{
	if (!c->dim)
		return;
	wlr_scene_node_set_enabled(&c->dim->node, c->dim_now > 0);
	if (c->dim_now > 0)
		wlr_scene_buffer_set_opacity(c->dim, c->fading_in ? c->dim_now * c->fade_now : c->dim_now);
}

/* Dims c, or not, as it is now: fading there, if it was shown and is
 * still, or at once */
static void
dim_update_one(Client *c)
{
	float strength = dim_strength(), to;
	int shown;
	if (!c->dim)
		return;
	to = dim_wanted(c, strength) ? strength : 0;
	shown = c->mon && VISIBLEON(c, c->mon);
	if (!shown || !c->dim_seen || fx_fade_duration() <= 0) {
		c->dim_now = c->dim_from = c->dim_to = to;
		dim_apply(c);
	} else if (to != c->dim_to) {
		c->dim_from = c->dim_now;
		c->dim_to = to;
		c->dim_start = fx_now();
		wlr_output_schedule_frame(c->mon->wlr_output);
	}
	c->dim_seen = shown;
}

/* Steps c's dim's fade as its monitor draws a frame (effects.h) */
static void
dim_step(Client *c, int64_t now, int *more)
{
	double p;
	if (!c->dim || c->dim_now == c->dim_to)
		return;
	p = fx_eased(c->dim_start, now);
	if (p >= 1.0) {
		c->dim_now = c->dim_to;
	} else {
		c->dim_now = c->dim_from + (c->dim_to - c->dim_from) * (float)p;
		*more = 1;
	}
	dim_apply(c);
}

/* Dims every window, or not, as it is now: after focus moves, the
 * windows shown change, or the settings do */
static void
dim_update(void)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		dim_update_one(c);
}

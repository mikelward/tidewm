/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * Effects, tidewm's own, as tide's Hyprland config has them: a window
 * fades in as it opens, the dim (dim.h) fades as focus moves, and a
 * floating window casts a soft shadow. A fade takes the config file's
 * fade_duration, config.h's without the line, easing out; 0 makes it
 * instant. A window that opens dimmed, as one the focus guard holds, has
 * its dim fade in with it. Fades step as each frame is drawn, so an app's commit, which
 * sets its surfaces' opacity again, can't undo one. A window's border
 * shows at once.
 *
 * The shadow is one image, cut in nine: its corners drawn as they are,
 * and its edges stretched along the window's sides. Clicks go through it.
 */

#define SHADOW_RANGE 8 /* pixels past the window's edge, as Hyprland's range */
#define SHADOW_ALPHA 0x44 /* its darkest, at the edge, out of 255 */

/* The shadow's image: black, premultiplied, round a window one pixel wide */
typedef struct {
	struct wlr_buffer base;
	uint32_t pixels[(2 * SHADOW_RANGE + 1) * (2 * SHADOW_RANGE + 1)];
} ShadowImage;

static ShadowImage *shadow_image;

static void dim_step(Client *c, int64_t now, int *more);
static void dim_apply(Client *c);

static void
shadow_image_destroy(struct wlr_buffer *buffer)
{
	ShadowImage *s = wl_container_of(buffer, s, base);
	wlr_buffer_finish(buffer);
	free(s);
}

static bool
shadow_image_access(struct wlr_buffer *buffer, uint32_t flags, void **data,
		uint32_t *format, size_t *stride)
{
	ShadowImage *s = wl_container_of(buffer, s, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = s->pixels;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (2 * SHADOW_RANGE + 1) * sizeof(uint32_t);
	return true;
}

static void
shadow_image_end(struct wlr_buffer *buffer)
{
}

static const struct wlr_buffer_impl shadow_image_impl = {
	.destroy = shadow_image_destroy,
	.begin_data_ptr_access = shadow_image_access,
	.end_data_ptr_access = shadow_image_end,
};

/* Clicks and the pointer go through the shadow to what's under it */
static bool
fx_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

/* Draws the shadow's image: each pixel as dark as it's near the window,
 * fading as the square of its distance's share of the range, to nothing
 * at the range, as Hyprland's render_power 2 */
static void
fx_init(void)
{
	const int r = SHADOW_RANGE, size = 2 * SHADOW_RANGE + 1;
	int x, y;
	shadow_image = ecalloc(1, sizeof(*shadow_image));
	wlr_buffer_init(&shadow_image->base, &shadow_image_impl, size, size);
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			/* From the window's edge to the pixel's middle */
			double dx = MAX(0.0, abs(x - r) - 0.5), dy = MAX(0.0, abs(y - r) - 0.5);
			double t = 1.0 - sqrt(dx * dx + dy * dy) / r;
			uint32_t a = x == r && y == r ? 0 : t > 0 ? (uint32_t)lround(SHADOW_ALPHA * t * t) : 0;
			shadow_image->pixels[y * size + x] = a << 24;
		}
	}
}

static void
fx_finish(void)
{
	/* Freed once the last window's shadow lets it go */
	if (shadow_image)
		wlr_buffer_drop(&shadow_image->base);
	shadow_image = NULL;
}

/* Now, in milliseconds, for the fades */
static int64_t
fx_now(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* How long a fade takes: the config file's fade_duration, or config.h's */
static int
fx_fade_duration(void)
{
	return settings && settings->fade_duration >= 0 ? settings->fade_duration : fade_duration;
}

/* How far a fade that started at start is by now, from 0 to 1, easing
 * out: quick at first, slowing to its end */
static double
fx_eased(int64_t start, int64_t now)
{
	int duration = fx_fade_duration();
	double p = duration > 0 ? (double)(now - start) / duration : 1.0;
	if (p >= 1.0)
		return 1.0;
	if (p <= 0.0)
		return 0.0;
	return 1.0 - (1.0 - p) * (1.0 - p) * (1.0 - p);
}

/* Shows node's buffers, but c's dim, at opacity a: a surface's as its
 * app's alpha-modifier-v1 has it, times a. Hidden ones too, so none is
 * left part faded once it shows. */
static void
fx_opacity(struct wlr_scene_node *node, Client *c, float a)
{
	struct wlr_scene_node *child;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
		const struct wlr_alpha_modifier_surface_v1_state *app = surface
				? wlr_alpha_modifier_v1_get_surface_state(surface->surface) : NULL;
		if (buffer != c->dim)
			wlr_scene_buffer_set_opacity(buffer, app ? (float)app->multiplier * a : a);
		return;
	}
	if (node->type != WLR_SCENE_NODE_TREE)
		return;
	wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
		fx_opacity(child, c, a);
}

/* c, just mapped, fades in, unless fades are instant */
static void
fade_in(Client *c)
{
	if (fx_fade_duration() <= 0 || !c->mon)
		return;
	c->fade_start = fx_now();
	c->fading_in = 1;
	c->fade_now = 0;
	fx_opacity(&c->scene->node, c, 0);
	dim_apply(c);
	wlr_output_schedule_frame(c->mon->wlr_output);
}

static void
fade_step(Client *c, int64_t now, int *more)
{
	double p;
	if (!c->fading_in)
		return;
	p = fx_eased(c->fade_start, now);
	if (p >= 1.0)
		c->fading_in = 0;
	else
		*more = 1;
	c->fade_now = (float)p;
	fx_opacity(&c->scene->node, c, (float)p);
	dim_apply(c);
}

/* Steps every window's fades as m draws a frame, by the time, so another
 * monitor's frame changes nothing, and asks a window's monitor for another
 * while one goes on: one moved to another monitor meanwhile goes on there */
static void
fx_frame(Monitor *m)
{
	int64_t now = fx_now();
	Client *c;
	wl_list_for_each(c, &clients, link) {
		int more = 0;
		dim_step(c, now, &more);
		fade_step(c, now, &more);
		if (more && c->mon)
			wlr_output_schedule_frame(c->mon->wlr_output);
	}
}

/* Gives c its shadow, off, under its surface and borders */
static void
shadow_attach(Client *c)
{
	int i;
	c->shadow = wlr_scene_tree_create(c->scene);
	wlr_scene_node_lower_to_bottom(&c->shadow->node);
	for (i = 0; i < 8; i++) {
		c->shadow_part[i] = wlr_scene_buffer_create(c->shadow, &shadow_image->base);
		c->shadow_part[i]->point_accepts_input = fx_no_input;
		/* Stretched, an edge's one pixel is its own, not blended with
		 * the corners beside it */
		wlr_scene_buffer_set_filter_mode(c->shadow_part[i], WLR_SCALE_FILTER_NEAREST);
	}
	wlr_scene_node_set_enabled(&c->shadow->node, 0);
}

/* Whether c casts a shadow: floating, over what's under it, but not
 * covering its monitor */
static int
shadow_wanted(Client *c)
{
	int on = settings && settings->shadows >= 0 ? settings->shadows : shadows;
	return on && c->isfloating && !c->isfullscreen && !c->ismaximized;
}

/* Shows c's shadow, or not, round it as it is now */
static void
shadow_update(Client *c)
{
	const int r = SHADOW_RANGE, w = c->geom.width, h = c->geom.height;
	/* Each part: where it is in the image, then round the window */
	const struct { int sx, sy, sw, sh, x, y, w, h; } parts[8] = {
		{ 0, 0, r, r, -r, -r, r, r },             /* top left */
		{ r, 0, 1, r, 0, -r, w, r },              /* top */
		{ r + 1, 0, r, r, w, -r, r, r },          /* top right */
		{ 0, r, r, 1, -r, 0, r, h },              /* left */
		{ r + 1, r, r, 1, w, 0, r, h },           /* right */
		{ 0, r + 1, r, r, -r, h, r, r },          /* bottom left */
		{ r, r + 1, 1, r, 0, h, w, r },           /* bottom */
		{ r + 1, r + 1, r, r, w, h, r, r },       /* bottom right */
	};
	int i, on;
	if (!c->shadow)
		return;
	/* A size of 0 would be the image's own */
	on = shadow_wanted(c) && w > 0 && h > 0;
	wlr_scene_node_set_enabled(&c->shadow->node, on);
	if (!on)
		return;
	for (i = 0; i < 8; i++) {
		struct wlr_fbox src = { parts[i].sx, parts[i].sy, parts[i].sw, parts[i].sh };
		wlr_scene_buffer_set_source_box(c->shadow_part[i], &src);
		wlr_scene_buffer_set_dest_size(c->shadow_part[i], parts[i].w, parts[i].h);
		wlr_scene_node_set_position(&c->shadow_part[i]->node, parts[i].x, parts[i].y);
	}
}

/* Every window's shadow, after the settings change */
static void
shadow_update_all(void)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		shadow_update(c);
}

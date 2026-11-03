/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' power manager, over wlr-output-power-management, as an idle
 * daemon would use it: it turns the output called OUTPUT on or off, and
 * prints "on" or "off" once the compositor says it is.
 *
 * Usage: power OUTPUT on|off
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-output-power-management-unstable-v1-client-protocol.h"

struct output {
	struct wl_output *output;
	char *name;
};

static struct zwlr_output_power_manager_v1 *manager;
static struct output outputs[8];
static int noutputs;
static int mode = -1, failed;

static void
die(const char *msg)
{
	fprintf(stderr, "power: %s\n", msg);
	exit(1);
}

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
		int32_t transform) {}
static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
		int32_t h, int32_t refresh) {}
static void output_done(void *data, struct wl_output *o) {}
static void output_scale(void *data, struct wl_output *o, int32_t factor) {}
static void output_description(void *data, struct wl_output *o, const char *d) {}

static void
output_name(void *data, struct wl_output *o, const char *name)
{
	struct output *out = data;
	free(out->name);
	out->name = strdup(name);
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode, .done = output_done,
	.scale = output_scale, .name = output_name, .description = output_description,
};

static void
power_mode(void *data, struct zwlr_output_power_v1 *power, uint32_t m)
{
	mode = m;
}

static void
power_failed(void *data, struct zwlr_output_power_v1 *power)
{
	failed = 1;
}

static const struct zwlr_output_power_v1_listener power_listener = {
	.mode = power_mode,
	.failed = power_failed,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, zwlr_output_power_manager_v1_interface.name)) {
		manager = wl_registry_bind(registry, name, &zwlr_output_power_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_output_interface.name) && version >= 4
			&& noutputs < (int)(sizeof(outputs) / sizeof(outputs[0]))) {
		outputs[noutputs].output = wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(outputs[noutputs].output, &output_listener, &outputs[noutputs]);
		noutputs++;
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	struct zwlr_output_power_v1 *power;
	struct output *out = NULL;
	int i, want;

	if (argc != 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off")))
		die("usage: power OUTPUT on|off");
	want = !strcmp(argv[2], "on") ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF;
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	/* One roundtrip for the globals, one for the outputs' names */
	if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager)
		die("the compositor lacks zwlr_output_power_manager_v1");
	for (i = 0; i < noutputs; i++)
		if (outputs[i].name && !strcmp(outputs[i].name, argv[1]))
			out = &outputs[i];
	if (!out)
		die("no output has that name");
	power = zwlr_output_power_manager_v1_get_output_power(manager, out->output);
	zwlr_output_power_v1_add_listener(power, &power_listener, NULL);
	zwlr_output_power_v1_set_mode(power, want);
	while (!failed && mode != want)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	if (failed)
		die("the compositor refused");
	printf("%s\n", argv[2]);
	return 0;
}

/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' ext-workspace client. It prints each workspace the compositor
 * announces, in order, as "NAME STATE GROUPOUTPUTS", where STATE is
 * active or inactive, plus ",urgent" when it's urgent, and GROUPOUTPUTS is
 * how many outputs its group is on. With --activate NAME it then asks for
 * that workspace and commits.
 *
 * Usage: workspaces [--activate NAME]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "ext-workspace-v1-client-protocol.h"

struct handle {
	struct ext_workspace_handle_v1 *handle;
	struct ext_workspace_group_handle_v1 *group;
	char *name;
	uint32_t state;
	int removed;
};

struct group {
	struct ext_workspace_group_handle_v1 *group;
	int outputs;
};

static struct ext_workspace_manager_v1 *manager;
static struct handle handles[64];
static int nhandles;
static struct group groups[8];
static int ngroups;
static int done;

static void
die(const char *msg)
{
	fprintf(stderr, "workspaces: %s\n", msg);
	exit(1);
}

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id) {}
static void ws_coordinates(void *data, struct ext_workspace_handle_v1 *h, struct wl_array *c) {}
static void ws_capabilities(void *data, struct ext_workspace_handle_v1 *h, uint32_t caps) {}

static void
ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name)
{
	struct handle *w = data;
	free(w->name);
	w->name = strdup(name);
}

static void
ws_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t state)
{
	((struct handle *)data)->state = state;
}

static void
ws_removed(void *data, struct ext_workspace_handle_v1 *h)
{
	((struct handle *)data)->removed = 1;
}

static const struct ext_workspace_handle_v1_listener ws_listener = {
	.id = ws_id, .name = ws_name, .coordinates = ws_coordinates,
	.state = ws_state, .capabilities = ws_capabilities, .removed = ws_removed,
};

static void group_capabilities(void *data, struct ext_workspace_group_handle_v1 *g, uint32_t caps) {}
static void group_output_leave(void *data, struct ext_workspace_group_handle_v1 *g, struct wl_output *o)
{
	((struct group *)data)->outputs--;
}
static void group_output_enter(void *data, struct ext_workspace_group_handle_v1 *g, struct wl_output *o)
{
	((struct group *)data)->outputs++;
}
static void group_removed(void *data, struct ext_workspace_group_handle_v1 *g) {}

static void
group_workspace_enter(void *data, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *h)
{
	((struct handle *)ext_workspace_handle_v1_get_user_data(h))->group = g;
}

static void
group_workspace_leave(void *data, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *h)
{
	((struct handle *)ext_workspace_handle_v1_get_user_data(h))->group = NULL;
}

static const struct ext_workspace_group_handle_v1_listener group_listener = {
	.capabilities = group_capabilities, .output_enter = group_output_enter,
	.output_leave = group_output_leave, .workspace_enter = group_workspace_enter,
	.workspace_leave = group_workspace_leave, .removed = group_removed,
};

static void
manager_workspace_group(void *data, struct ext_workspace_manager_v1 *m,
		struct ext_workspace_group_handle_v1 *g)
{
	if (ngroups == (int)(sizeof(groups) / sizeof(groups[0])))
		die("too many groups");
	groups[ngroups].group = g;
	ext_workspace_group_handle_v1_add_listener(g, &group_listener, &groups[ngroups++]);
}

static void
manager_workspace(void *data, struct ext_workspace_manager_v1 *m,
		struct ext_workspace_handle_v1 *h)
{
	if (nhandles == (int)(sizeof(handles) / sizeof(handles[0])))
		die("too many workspaces");
	handles[nhandles].handle = h;
	ext_workspace_handle_v1_add_listener(h, &ws_listener, &handles[nhandles++]);
}

static void
manager_done(void *data, struct ext_workspace_manager_v1 *m)
{
	done = 1;
}

static void manager_finished(void *data, struct ext_workspace_manager_v1 *m) {}

static const struct ext_workspace_manager_v1_listener manager_listener = {
	.workspace_group = manager_workspace_group, .workspace = manager_workspace,
	.done = manager_done, .finished = manager_finished,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, ext_workspace_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name, &ext_workspace_manager_v1_interface, 1);
	else if (!strcmp(interface, wl_output_interface.name))
		wl_registry_bind(registry, name, &wl_output_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *r, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global, .global_remove = registry_global_remove,
};

static int
group_outputs(struct ext_workspace_group_handle_v1 *g)
{
	int i;
	for (i = 0; i < ngroups; i++)
		if (groups[i].group == g)
			return groups[i].outputs;
	return 0;
}

int
main(int argc, char *argv[])
{
	struct wl_display *display;
	const char *activate = NULL;
	int i;

	if (argc == 3 && !strcmp(argv[1], "--activate"))
		activate = argv[2];
	else if (argc != 1)
		die("usage: workspaces [--activate NAME]");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager)
		die("the compositor has no ext_workspace_manager_v1");
	ext_workspace_manager_v1_add_listener(manager, &manager_listener, NULL);
	while (!done)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	/* The outputs' events for the group come in the same burst */
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");

	for (i = 0; i < nhandles; i++) {
		struct handle *w = &handles[i];
		if (w->removed)
			continue;
		printf("%s %s%s %d\n", w->name ? w->name : "",
				w->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE ? "active" : "inactive",
				w->state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT ? ",urgent" : "",
				group_outputs(w->group));
		if (activate && w->name && !strcmp(w->name, activate)) {
			ext_workspace_handle_v1_activate(w->handle);
			ext_workspace_manager_v1_commit(manager);
			activate = NULL;
		}
	}
	if (activate)
		die("no such workspace to activate");
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	return 0;
}

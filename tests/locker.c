/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * The tests' screen locker, over ext-session-lock: it locks the session
 * and prints "locked" once the compositor says it is. A line "unlock" on
 * stdin unlocks it and ends. It makes no lock surfaces, which tidewm, as
 * dwl, doesn't wait for.
 *
 * Usage: locker
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

static struct ext_session_lock_manager_v1 *manager;
static int finished;

static void
die(const char *msg)
{
	fprintf(stderr, "locker: %s\n", msg);
	exit(1);
}

static void
lock_locked(void *data, struct ext_session_lock_v1 *lock)
{
	printf("locked\n");
	fflush(stdout);
}

static void
lock_finished(void *data, struct ext_session_lock_v1 *lock)
{
	finished = 1;
}

static const struct ext_session_lock_v1_listener lock_listener = {
	.locked = lock_locked,
	.finished = lock_finished,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	if (!strcmp(interface, ext_session_lock_manager_v1_interface.name))
		manager = wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
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
	struct ext_session_lock_v1 *lock;
	char line[64];

	if (argc != 1)
		die("usage: locker");
	if (!(display = wl_display_connect(NULL)))
		die("can't connect to the compositor");
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (!manager)
		die("the compositor lacks ext_session_lock_manager_v1");
	lock = ext_session_lock_manager_v1_lock(manager);
	ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);

	for (;;) {
		struct pollfd fds[2] = {
			{.fd = wl_display_get_fd(display), .events = POLLIN},
			{.fd = STDIN_FILENO, .events = POLLIN},
		};
		if (wl_display_flush(display) < 0)
			die("lost the compositor");
		if (poll(fds, 2, -1) < 0)
			die("poll failed");
		if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0)
			die("lost the compositor");
		if (finished)
			die("the compositor refused the lock");
		if (!(fds[1].revents & (POLLIN | POLLHUP)))
			continue;
		if (!fgets(line, sizeof(line), stdin) || !strcmp(line, "unlock\n"))
			break;
	}
	ext_session_lock_v1_unlock_and_destroy(lock);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	return 0;
}

/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * tidewm's IPC: sway's i3-ipc protocol, the subset tide needs, on a socket
 * named by SWAYSOCK. docs/ipc.md says what it answers. dwl.c includes this
 * file, like client.h, so it reads dwl's state directly.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>

#define IPC_MAGIC "i3-ipc"
#define IPC_MAGIC_LEN 6
#define IPC_HEADER_LEN (IPC_MAGIC_LEN + 2 * sizeof(uint32_t))
/* A request bigger than this is a broken client, not a real one. */
#define IPC_MAX_PAYLOAD (4u << 20)
/* A client this far behind isn't reading its events: drop it, not grow. */
#define IPC_MAX_PENDING (16u << 20)
#define IPC_EVENT_BIT 0x80000000u

enum { IpcCommand = 0, IpcGetWorkspaces = 1, IpcSubscribe = 2,
	IpcGetOutputs = 3, IpcGetTree = 4, IpcGetVersion = 7, IpcSendTick = 10 };
/* Event numbers, as sway sends them with IPC_EVENT_BIT set */
enum { IpcEvWorkspace = 0, IpcEvOutput = 1, IpcEvWindow = 3,
	IpcEvShutdown = 6, IpcEvTick = 7 };

typedef struct {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
	uint32_t events; /* bit n set: subscribed to event n */
	int dead; /* failed while its own request ran: drop it after */
	char *in;
	size_t inlen, incap;
	char *out;
	size_t outlen, outcap;
} IpcClient;

static int ipc_socket = -1;
static struct wl_event_source *ipc_source;
static struct wl_list ipc_clients = {&ipc_clients, &ipc_clients};
static char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static unsigned long ipc_next_id = 2; /* 1 is the root */
static int ipc_shutdown_sent;
/* The client whose request is running, which only its own handler drops */
static IpcClient *ipc_current;

static void ipc_client_destroy(IpcClient *cl);

static unsigned long
ipc_id(unsigned long *id)
{
	if (!*id)
		*id = ipc_next_id++;
	return *id;
}

static int
ipc_grow(char **buf, size_t *cap, size_t need)
{
	size_t newcap = *cap ? *cap : 4096;
	char *p;
	while (newcap < need)
		newcap *= 2;
	if (newcap == *cap)
		return 0;
	if (!(p = realloc(*buf, newcap)))
		return -1;
	*buf = p;
	*cap = newcap;
	return 0;
}

/* Writes what it can of the client's pending output, and asks to be told
 * when the rest can go. Returns -1 when the client must be dropped. */
static int
ipc_flush(IpcClient *cl)
{
	while (cl->outlen) {
		ssize_t n = write(cl->fd, cl->out, cl->outlen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wl_event_source_fd_update(cl->source,
					WL_EVENT_READABLE | WL_EVENT_WRITABLE);
			return 0;
		}
		if (n < 0) {
			fprintf(stderr, "ipc: write to client %d: %s\n", cl->fd, strerror(errno));
			return -1;
		}
		memmove(cl->out, cl->out + n, cl->outlen - (size_t)n);
		cl->outlen -= (size_t)n;
	}
	wl_event_source_fd_update(cl->source, WL_EVENT_READABLE);
	return 0;
}

static int
ipc_send(IpcClient *cl, uint32_t type, const char *payload)
{
	uint32_t len = (uint32_t)strlen(payload);
	size_t need = cl->outlen + IPC_HEADER_LEN + len;
	char *p;
	if (need > IPC_MAX_PENDING) {
		fprintf(stderr, "ipc: client %d has %zu bytes unread; dropping it\n",
				cl->fd, cl->outlen);
		return -1;
	}
	if (ipc_grow(&cl->out, &cl->outcap, need) < 0) {
		fprintf(stderr, "ipc: out of memory queueing for client %d\n", cl->fd);
		return -1;
	}
	p = cl->out + cl->outlen;
	memcpy(p, IPC_MAGIC, IPC_MAGIC_LEN);
	memcpy(p + IPC_MAGIC_LEN, &len, sizeof(len));
	memcpy(p + IPC_MAGIC_LEN + sizeof(len), &type, sizeof(type));
	memcpy(p + IPC_HEADER_LEN, payload, len);
	cl->outlen = need;
	return ipc_flush(cl);
}

static int
ipc_send_json(IpcClient *cl, uint32_t type, struct json_object *obj)
{
	int r = ipc_send(cl, type, json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN));
	json_object_put(obj);
	return r;
}

/* Sends an event to every client subscribed to it. It takes obj. */
static void
ipc_broadcast(int event, struct json_object *obj)
{
	IpcClient *cl, *tmp;
	const char *payload = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
	wl_list_for_each_safe(cl, tmp, &ipc_clients, link) {
		if (!(cl->events & (1u << event)) || cl->dead
				|| ipc_send(cl, IPC_EVENT_BIT | (uint32_t)event, payload) >= 0)
			continue;
		if (cl == ipc_current)
			cl->dead = 1;
		else
			ipc_client_destroy(cl);
	}
	json_object_put(obj);
}

static int
ipc_subscribed(int event)
{
	IpcClient *cl;
	wl_list_for_each(cl, &ipc_clients, link)
		if (cl->events & (1u << event))
			return 1;
	return 0;
}

static struct json_object *
ipc_json_success(int success, const char *error)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "success", json_object_new_boolean(success));
	if (error)
		json_object_object_add(obj, "error", json_object_new_string(error));
	return obj;
}

static struct json_object *
ipc_json_string(const char *s)
{
	return s ? json_object_new_string(s) : NULL;
}

static struct json_object *
ipc_json_box(struct wlr_box box)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "x", json_object_new_int(box.x));
	json_object_object_add(obj, "y", json_object_new_int(box.y));
	json_object_object_add(obj, "width", json_object_new_int(box.width));
	json_object_object_add(obj, "height", json_object_new_int(box.height));
	return obj;
}

static const char *
ipc_transform_name(enum wl_output_transform t)
{
	static const char *const names[] = {
		[WL_OUTPUT_TRANSFORM_NORMAL] = "normal",
		[WL_OUTPUT_TRANSFORM_90] = "90",
		[WL_OUTPUT_TRANSFORM_180] = "180",
		[WL_OUTPUT_TRANSFORM_270] = "270",
		[WL_OUTPUT_TRANSFORM_FLIPPED] = "flipped",
		[WL_OUTPUT_TRANSFORM_FLIPPED_90] = "flipped-90",
		[WL_OUTPUT_TRANSFORM_FLIPPED_180] = "flipped-180",
		[WL_OUTPUT_TRANSFORM_FLIPPED_270] = "flipped-270",
	};
	return (unsigned)t < LENGTH(names) && names[t] ? names[t] : "normal";
}

static struct json_object *
ipc_json_output(Monitor *m)
{
	struct wlr_output *o = m->wlr_output;
	struct json_object *obj = json_object_new_object(), *mode;
	json_object_object_add(obj, "id", json_object_new_uint64(ipc_id(&m->ipc_id)));
	json_object_object_add(obj, "type", json_object_new_string("output"));
	json_object_object_add(obj, "name", json_object_new_string(o->name));
	json_object_object_add(obj, "make", json_object_new_string(o->make ? o->make : ""));
	json_object_object_add(obj, "model", json_object_new_string(o->model ? o->model : ""));
	json_object_object_add(obj, "serial", json_object_new_string(o->serial ? o->serial : ""));
	json_object_object_add(obj, "active", json_object_new_boolean(o->enabled));
	json_object_object_add(obj, "power", json_object_new_boolean(o->enabled && !m->asleep));
	json_object_object_add(obj, "primary", json_object_new_boolean(0));
	json_object_object_add(obj, "scale", json_object_new_double(o->scale));
	json_object_object_add(obj, "transform", json_object_new_string(ipc_transform_name(o->transform)));
	json_object_object_add(obj, "focused", json_object_new_boolean(m == selmon));
	/* A workspace spans every monitor, so each shows the current one. */
	json_object_object_add(obj, "current_workspace", curws ? json_object_new_string(curws->name) : NULL);
	json_object_object_add(obj, "rect", ipc_json_box(m->m));
	mode = json_object_new_object();
	json_object_object_add(mode, "width", json_object_new_int(o->width));
	json_object_object_add(mode, "height", json_object_new_int(o->height));
	json_object_object_add(mode, "refresh", json_object_new_int(o->refresh));
	json_object_object_add(obj, "current_mode", mode);
	return obj;
}

static pid_t
ipc_client_pid(Client *c)
{
	pid_t pid = 0;
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->pid;
#endif
	wl_client_get_credentials(wl_resource_get_client(c->surface.xdg->resource),
			&pid, NULL, NULL);
	return pid;
}

static struct json_object *
ipc_json_window(Client *c)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "id", json_object_new_uint64(ipc_id(&c->ipc_id)));
	json_object_object_add(obj, "type", json_object_new_string(
			c->isfloating ? "floating_con" : "con"));
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		struct json_object *props = json_object_new_object();
		json_object_object_add(obj, "name", ipc_json_string(c->surface.xwayland->title));
		json_object_object_add(obj, "app_id", NULL);
		json_object_object_add(obj, "shell", json_object_new_string("xwayland"));
		json_object_object_add(obj, "window", json_object_new_uint64(c->surface.xwayland->window_id));
		json_object_object_add(props, "class", ipc_json_string(c->surface.xwayland->class));
		json_object_object_add(props, "instance", ipc_json_string(c->surface.xwayland->instance));
		json_object_object_add(props, "title", ipc_json_string(c->surface.xwayland->title));
		json_object_object_add(obj, "window_properties", props);
	} else
#endif
	{
		json_object_object_add(obj, "name", ipc_json_string(c->surface.xdg->toplevel->title));
		json_object_object_add(obj, "app_id", ipc_json_string(c->surface.xdg->toplevel->app_id));
		json_object_object_add(obj, "shell", json_object_new_string("xdg_shell"));
	}
	json_object_object_add(obj, "pid", json_object_new_int(ipc_client_pid(c)));
	json_object_object_add(obj, "focused", json_object_new_boolean(c == focused_client));
	json_object_object_add(obj, "urgent", json_object_new_boolean(c->isurgent));
	json_object_object_add(obj, "visible", json_object_new_boolean(c->mon && VISIBLEON(c, c->mon)));
	json_object_object_add(obj, "fullscreen_mode", json_object_new_int(c->isfullscreen));
	json_object_object_add(obj, "rect", ipc_json_box(c->geom));
	json_object_object_add(obj, "workspace", c->ws ? json_object_new_string(c->ws->name) : NULL);
	json_object_object_add(obj, "output", c->mon ? json_object_new_string(c->mon->wlr_output->name) : NULL);
	json_object_object_add(obj, "foreign_toplevel_identifier", c->foreign_toplevel_handle
			? json_object_new_string(c->foreign_toplevel_handle->identifier) : NULL);
	json_object_object_add(obj, "nodes", json_object_new_array());
	json_object_object_add(obj, "floating_nodes", json_object_new_array());
	return obj;
}

/* A workspace, as GET_WORKSPACES and events see it. It spans every
 * monitor, so its output is the focused one, where it has focus. */
static struct json_object *
ipc_json_workspace(Workspace *ws, Monitor *m)
{
	struct json_object *obj = json_object_new_object();
	const char *p = ws->name;
	int num = -1;
	while (isdigit((unsigned char)*p))
		p++;
	if (!*p && p - ws->name < 10)
		num = atoi(ws->name);
	json_object_object_add(obj, "id", json_object_new_uint64(ipc_id(&ws->ipc_id)));
	json_object_object_add(obj, "type", json_object_new_string("workspace"));
	json_object_object_add(obj, "name", json_object_new_string(ws->name));
	json_object_object_add(obj, "num", json_object_new_int(num));
	json_object_object_add(obj, "focused", json_object_new_boolean(ws == curws));
	json_object_object_add(obj, "visible", json_object_new_boolean(ws == curws));
	json_object_object_add(obj, "urgent", json_object_new_boolean(ws_urgent(ws)));
	json_object_object_add(obj, "persistent", json_object_new_boolean(ws->persistent));
	json_object_object_add(obj, "output", m ? json_object_new_string(m->wlr_output->name) : NULL);
	json_object_object_add(obj, "rect", ipc_json_box(m ? m->w : (struct wlr_box){0}));
	return obj;
}

/* root > outputs > workspaces > windows. Every workspace is under every
 * output, holding that monitor's part of it. */
static struct json_object *
ipc_json_tree(void)
{
	struct json_object *root = json_object_new_object(), *outputs = json_object_new_array();
	struct wlr_box all = {0};
	Monitor *m;
	Client *c;

	wl_list_for_each(m, &mons, link) {
		struct json_object *out = ipc_json_output(m), *parts = json_object_new_array();
		Workspace *ws;
		wl_list_for_each(ws, &workspaces, link) {
			struct json_object *part = ipc_json_workspace(ws, m);
			struct json_object *tiled = json_object_new_array(), *floating = json_object_new_array();
			wl_list_for_each(c, &clients, link) {
				if (c->mon == m && c->ws == ws)
					json_object_array_add(c->isfloating ? floating : tiled, ipc_json_window(c));
			}
			json_object_object_add(part, "nodes", tiled);
			json_object_object_add(part, "floating_nodes", floating);
			json_object_array_add(parts, part);
		}
		json_object_object_add(out, "nodes", parts);
		json_object_object_add(out, "floating_nodes", json_object_new_array());
		json_object_array_add(outputs, out);
	}
	wlr_output_layout_get_box(output_layout, NULL, &all);
	json_object_object_add(root, "id", json_object_new_int(1));
	json_object_object_add(root, "type", json_object_new_string("root"));
	json_object_object_add(root, "name", json_object_new_string("root"));
	json_object_object_add(root, "focused", json_object_new_boolean(0));
	json_object_object_add(root, "rect", ipc_json_box(all));
	json_object_object_add(root, "nodes", outputs);
	json_object_object_add(root, "floating_nodes", json_object_new_array());
	return root;
}

static struct json_object *
ipc_json_version(void)
{
	struct json_object *obj = json_object_new_object();
	const char *v = VERSION;
	int major = 0, minor = 0, patch = 0;
	while (*v && !isdigit((unsigned char)*v))
		v++;
	sscanf(v, "%d.%d.%d", &major, &minor, &patch);
	json_object_object_add(obj, "human_readable", json_object_new_string("tidewm " VERSION));
	json_object_object_add(obj, "variant", json_object_new_string("tidewm"));
	json_object_object_add(obj, "major", json_object_new_int(major));
	json_object_object_add(obj, "minor", json_object_new_int(minor));
	json_object_object_add(obj, "patch", json_object_new_int(patch));
	json_object_object_add(obj, "loaded_config_file_name", json_object_new_string(""));
	return obj;
}

static void
ipc_send_shutdown(void)
{
	struct json_object *obj;
	if (ipc_shutdown_sent)
		return;
	ipc_shutdown_sent = 1;
	obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string("exit"));
	ipc_broadcast(IpcEvShutdown, obj);
}

/* If *p starts with word, in any case, then a space or the end, moves *p
 * past them both and returns 1. */
static int
ipc_take(char **p, const char *word)
{
	size_t n = strlen(word);
	if (strncasecmp(*p, word, n) || ((*p)[n] && !isspace((unsigned char)(*p)[n])))
		return 0;
	*p += n;
	while (isspace((unsigned char)**p))
		(*p)++;
	return 1;
}

static Client *
ipc_find_window(unsigned long id)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->ipc_id == id)
			return c;
	return NULL;
}

/* An output named, or left, right, up or down of the focused one. */
static Monitor *
ipc_find_output(const char *arg)
{
	static const struct { const char *name; enum wlr_direction dir; } dirs[] = {
		{"left", WLR_DIRECTION_LEFT}, {"right", WLR_DIRECTION_RIGHT},
		{"up", WLR_DIRECTION_UP}, {"down", WLR_DIRECTION_DOWN},
	};
	Monitor *m;
	size_t i;
	for (i = 0; i < LENGTH(dirs); i++)
		if (!strcasecmp(arg, dirs[i].name))
			return selmon ? dirtomon(dirs[i].dir) : NULL;
	wl_list_for_each(m, &mons, link)
		if (!strcmp(m->wlr_output->name, arg) && m->wlr_output->enabled)
			return m;
	return NULL;
}

/* The workspace a command names: "number N", "back_and_forth" or a name,
 * made if it's new. */
static Workspace *
ipc_command_workspace(char *arg, const char **error)
{
	Workspace *ws;
	if (ipc_take(&arg, "back_and_forth")) {
		if (!prevws)
			*error = "No previous workspace";
		return prevws;
	}
	ipc_take(&arg, "number");
	if (!(ws = ws_get(arg)))
		*error = "Invalid workspace name";
	return ws;
}

/* Runs one command, as a RUN_COMMAND payload holds several separated by
 * semicolons, and returns its result. Criteria in brackets, [con_id=N],
 * pick the window a command acts on; without them it's the focused one. */
static struct json_object *
ipc_run_command(char *cmd, int *quit_after)
{
	const char *error = NULL;
	struct json_object *result;
	Client *target = NULL;
	Workspace *ws;
	Monitor *m;
	char *end, *to, *orig, buf[160];
	int has_criteria = 0, persist;

	while (isspace((unsigned char)*cmd))
		cmd++;
	end = cmd + strlen(cmd);
	while (end > cmd && isspace((unsigned char)end[-1]))
		*--end = '\0';
	orig = cmd;

	if (*cmd == '[') {
		unsigned long id;
		char *close = strchr(cmd, ']'), *tail;
		if (!close || sscanf(cmd, "[con_id=%lu", &id) != 1) {
			error = "Unsupported criteria: only [con_id=N]";
			goto parse_error;
		}
		has_criteria = 1;
		if (!(target = ipc_find_window(id)))
			return ipc_json_success(0, "No window matches the criteria");
		tail = close + 1;
		while (isspace((unsigned char)*tail))
			tail++;
		cmd = tail;
	} else {
		target = focustop(selmon);
	}

	if (!has_criteria && ipc_take(&cmd, "nop"))
		return ipc_json_success(1, NULL);
	if (!has_criteria && !strcasecmp(cmd, "exit")) {
		*quit_after = 1;
		return ipc_json_success(1, NULL);
	}
	if (!has_criteria && ipc_take(&cmd, "workspace")) {
		if (!(ws = ipc_command_workspace(cmd, &error)))
			return ipc_json_success(0, error);
		ws_view(ws);
		return ipc_json_success(1, NULL);
	}
	if (ipc_take(&cmd, "move")) {
		if (!ipc_take(&cmd, "container"))
			ipc_take(&cmd, "window");
		ipc_take(&cmd, "to");
		if (ipc_take(&cmd, "workspace")) {
			if (!target)
				return ipc_json_success(0, "No window to move");
			if (!(ws = ipc_command_workspace(cmd, &error)))
				return ipc_json_success(0, error);
			ws_move(target, ws);
			return ipc_json_success(1, NULL);
		}
		if (ipc_take(&cmd, "output")) {
			if (!target)
				return ipc_json_success(0, "No window to move");
			if (!(m = ipc_find_output(cmd)))
				return ipc_json_success(0, "No such output");
			setmon(target, m, target->ws); /* which says it moved */
			return ipc_json_success(1, NULL);
		}
		error = "Expected move [container] [to] workspace|output ...";
		goto parse_error;
	}
	if (ipc_take(&cmd, "focus")) {
		if (!has_criteria && ipc_take(&cmd, "output")) {
			if (!(m = ipc_find_output(cmd)))
				return ipc_json_success(0, "No such output");
			selmon = m;
			focusclient(focustop(selmon), NULL, 1);
			return ipc_json_success(1, NULL);
		}
		if (*cmd || !has_criteria) {
			error = "Expected [con_id=N] focus, or focus output ...";
			goto parse_error;
		}
		ws_view(target->ws);
		selmon = target->mon;
		focusclient(target, NULL, 1);
		return ipc_json_success(1, NULL);
	}
	if (ipc_take(&cmd, "kill")) {
		if (*cmd) {
			error = "kill takes no arguments";
			goto parse_error;
		}
		if (!target)
			return ipc_json_success(0, "No window to close");
		client_send_close(target);
		return ipc_json_success(1, NULL);
	}
	if (!has_criteria && ipc_take(&cmd, "rename") && ipc_take(&cmd, "workspace")) {
		Workspace *from = curws;
		if (ipc_take(&cmd, "to")) {
			to = cmd;
		} else if ((to = strstr(cmd, " to "))) {
			*to = '\0';
			to += 4;
			while (isspace((unsigned char)*to))
				to++;
			if (!(from = ws_find(cmd)))
				return ipc_json_success(0, "No such workspace");
		} else {
			error = "Expected rename workspace [OLD] to NEW";
			goto parse_error;
		}
		if (!ws_name_ok(to))
			return ipc_json_success(0, "Invalid workspace name");
		if (ws_find(to))
			return ipc_json_success(0, "A workspace has that name already");
		ws_rename(from, to);
		return ipc_json_success(1, NULL);
	}
	/* tidewm's own: a persistent workspace stays when it's empty */
	persist = has_criteria ? -1 : ipc_take(&cmd, "persist") ? 1
		: ipc_take(&cmd, "unpersist") ? 0 : -1;
	if (persist >= 0) {
		if (!ipc_take(&cmd, "workspace")) {
			error = "Expected persist|unpersist workspace NAME";
			goto parse_error;
		}
		if (!(ws = persist ? ws_get(cmd) : ws_find(cmd)))
			return ipc_json_success(0, persist ? "Invalid workspace name" : "No such workspace");
		ws_set_persistent(ws, persist);
		return ipc_json_success(1, NULL);
	}
	error = NULL;
parse_error:
	snprintf(buf, sizeof(buf), "%s%.100s", error ? error : "Unknown command: ",
			error ? "" : orig);
	result = ipc_json_success(0, buf);
	json_object_object_add(result, "parse_error", json_object_new_boolean(1));
	return result;
}

static int
ipc_handle_command(IpcClient *cl, char *payload)
{
	struct json_object *results = json_object_new_array();
	char *saveptr = NULL, *cmd;
	int quit_after = 0, r;
	for (cmd = strtok_r(payload, ";", &saveptr); cmd; cmd = strtok_r(NULL, ";", &saveptr))
		json_object_array_add(results, ipc_run_command(cmd, &quit_after));
	if (!json_object_array_length(results))
		json_object_array_add(results, ipc_run_command(payload, &quit_after));
	r = ipc_send_json(cl, IpcCommand, results);
	if (quit_after) {
		/* Subscribers hear of the exit before the socket goes away. */
		ipc_send_shutdown();
		wl_display_terminate(dpy);
	}
	return r;
}

static int
ipc_handle_subscribe(IpcClient *cl, const char *payload)
{
	static const struct { const char *name; int event; } names[] = {
		{"workspace", IpcEvWorkspace}, {"output", IpcEvOutput},
		{"window", IpcEvWindow}, {"shutdown", IpcEvShutdown}, {"tick", IpcEvTick},
	};
	struct json_object *req = json_tokener_parse(payload), *tick;
	uint32_t events = 0;
	size_t i, j, n;
	int found;

	if (!req || !json_object_is_type(req, json_type_array)) {
		json_object_put(req);
		return ipc_send_json(cl, IpcSubscribe, ipc_json_success(0, "expected a JSON array of event names"));
	}
	n = json_object_array_length(req);
	for (i = 0; i < n; i++) {
		const char *name = json_object_get_string(json_object_array_get_idx(req, i));
		found = 0;
		for (j = 0; name && j < LENGTH(names); j++) {
			if (!strcmp(name, names[j].name)) {
				events |= 1u << names[j].event;
				found = 1;
			}
		}
		if (!found) {
			json_object_put(req);
			return ipc_send_json(cl, IpcSubscribe, ipc_json_success(0, "unknown event name"));
		}
	}
	json_object_put(req);
	cl->events |= events;
	if (ipc_send_json(cl, IpcSubscribe, ipc_json_success(1, NULL)) < 0)
		return -1;
	if (!(events & (1u << IpcEvTick)))
		return 0;
	/* As in i3 and sway, a new tick subscriber gets a first tick at once. */
	tick = json_object_new_object();
	json_object_object_add(tick, "first", json_object_new_boolean(1));
	json_object_object_add(tick, "payload", json_object_new_string(""));
	return ipc_send_json(cl, IPC_EVENT_BIT | IpcEvTick, tick);
}

static int
ipc_handle_message(IpcClient *cl, uint32_t type, char *payload)
{
	struct json_object *tick;
	switch (type) {
	case IpcCommand:
		return ipc_handle_command(cl, payload);
	case IpcGetWorkspaces: {
		struct json_object *list = json_object_new_array();
		Workspace *ws;
		wl_list_for_each(ws, &workspaces, link)
			json_object_array_add(list, ipc_json_workspace(ws, selmon));
		return ipc_send_json(cl, type, list);
	}
	case IpcSubscribe:
		return ipc_handle_subscribe(cl, payload);
	case IpcGetOutputs: {
		struct json_object *outputs = json_object_new_array();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			json_object_array_add(outputs, ipc_json_output(m));
		return ipc_send_json(cl, type, outputs);
	}
	case IpcGetTree:
		return ipc_send_json(cl, type, ipc_json_tree());
	case IpcGetVersion:
		return ipc_send_json(cl, type, ipc_json_version());
	case IpcSendTick:
		tick = json_object_new_object();
		json_object_object_add(tick, "first", json_object_new_boolean(0));
		json_object_object_add(tick, "payload", json_object_new_string(payload));
		ipc_broadcast(IpcEvTick, tick);
		return ipc_send_json(cl, type, ipc_json_success(1, NULL));
	default:
		return ipc_send_json(cl, type, ipc_json_success(0, "unsupported message type"));
	}
}

/* Handles every complete message in the client's input. Returns -1 when
 * the client must be dropped. */
static int
ipc_handle_input(IpcClient *cl)
{
	uint32_t len, type;
	char *payload;
	int r;
	while (cl->inlen >= IPC_HEADER_LEN) {
		if (memcmp(cl->in, IPC_MAGIC, IPC_MAGIC_LEN)) {
			fprintf(stderr, "ipc: client %d sent a message without the i3-ipc magic; dropping it\n", cl->fd);
			return -1;
		}
		memcpy(&len, cl->in + IPC_MAGIC_LEN, sizeof(len));
		memcpy(&type, cl->in + IPC_MAGIC_LEN + sizeof(len), sizeof(type));
		if (len > IPC_MAX_PAYLOAD) {
			fprintf(stderr, "ipc: client %d sent a %u-byte message; dropping it\n", cl->fd, len);
			return -1;
		}
		if (cl->inlen < IPC_HEADER_LEN + len)
			return 0;
		if (!(payload = malloc(len + 1))) {
			fprintf(stderr, "ipc: out of memory reading from client %d\n", cl->fd);
			return -1;
		}
		memcpy(payload, cl->in + IPC_HEADER_LEN, len);
		payload[len] = '\0';
		memmove(cl->in, cl->in + IPC_HEADER_LEN + len, cl->inlen - IPC_HEADER_LEN - len);
		cl->inlen -= IPC_HEADER_LEN + len;
		r = ipc_handle_message(cl, type, payload);
		free(payload);
		if (r < 0 || cl->dead)
			return -1;
	}
	return 0;
}

static int
ipc_client_event(int fd, uint32_t mask, void *data)
{
	IpcClient *cl = data;
	ipc_current = cl;
	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			ssize_t n;
			if (ipc_grow(&cl->in, &cl->incap, cl->inlen + 4096) < 0) {
				fprintf(stderr, "ipc: out of memory reading from client %d\n", fd);
				goto drop;
			}
			n = read(fd, cl->in + cl->inlen, cl->incap - cl->inlen);
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (n < 0) {
				fprintf(stderr, "ipc: read from client %d: %s\n", fd, strerror(errno));
				goto drop;
			}
			if (n == 0)
				goto drop; /* the client hung up */
			cl->inlen += (size_t)n;
			if (ipc_handle_input(cl) < 0)
				goto drop;
		}
	}
	if ((mask & WL_EVENT_WRITABLE) && ipc_flush(cl) < 0)
		goto drop;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		goto drop;
	ipc_current = NULL;
	return 0;
drop:
	ipc_current = NULL;
	ipc_client_destroy(cl);
	return 0;
}

static void
ipc_client_destroy(IpcClient *cl)
{
	wl_event_source_remove(cl->source);
	close(cl->fd);
	wl_list_remove(&cl->link);
	free(cl->in);
	free(cl->out);
	free(cl);
}

/* Makes fd non-blocking and closed across exec, as sockets made with
 * plain POSIX calls aren't. */
static int
ipc_set_fd_flags(int fd)
{
	int fl = fcntl(fd, F_GETFL);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
		return -1;
	if ((fl = fcntl(fd, F_GETFD)) < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
		return -1;
	return 0;
}

static int
ipc_accept(int fd, uint32_t mask, void *data)
{
	IpcClient *cl;
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		fprintf(stderr, "ipc: accept: %s\n", strerror(errno));
		return 0;
	}
	if (ipc_set_fd_flags(cfd) < 0) {
		fprintf(stderr, "ipc: set flags on client %d: %s\n", cfd, strerror(errno));
		close(cfd);
		return 0;
	}
	if (!(cl = calloc(1, sizeof(*cl)))) {
		fprintf(stderr, "ipc: out of memory for a new client\n");
		close(cfd);
		return 0;
	}
	cl->fd = cfd;
	cl->source = wl_event_loop_add_fd(event_loop, cfd, WL_EVENT_READABLE,
			ipc_client_event, cl);
	if (!cl->source) {
		fprintf(stderr, "ipc: can't watch client %d\n", cfd);
		close(cfd);
		free(cl);
		return 0;
	}
	wl_list_insert(&ipc_clients, &cl->link);
	return 0;
}

/* Opens the socket and names it in SWAYSOCK, for the startup command and
 * everything it starts. Without it tidewm still runs, so a failure is
 * reported, not fatal. */
static void
ipc_init(void)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int n;

	unsetenv("SWAYSOCK");
	if (!dir) {
		fprintf(stderr, "ipc: XDG_RUNTIME_DIR is unset, so there's no IPC socket\n");
		return;
	}
	n = snprintf(ipc_path, sizeof(ipc_path), "%s/tidewm-ipc.%u.%d.sock",
			dir, (unsigned)getuid(), (int)getpid());
	if (n < 0 || (size_t)n >= sizeof(ipc_path)) {
		fprintf(stderr, "ipc: socket path under %s is too long\n", dir);
		ipc_path[0] = '\0';
		return;
	}
	if ((ipc_socket = socket(AF_UNIX, SOCK_STREAM, 0)) < 0
			|| ipc_set_fd_flags(ipc_socket) < 0) {
		fprintf(stderr, "ipc: socket: %s\n", strerror(errno));
		goto fail;
	}
	strcpy(addr.sun_path, ipc_path);
	unlink(ipc_path);
	if (bind(ipc_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0
			|| listen(ipc_socket, 16) < 0) {
		fprintf(stderr, "ipc: listen on %s: %s\n", ipc_path, strerror(errno));
		goto fail;
	}
	if (!(ipc_source = wl_event_loop_add_fd(event_loop, ipc_socket,
			WL_EVENT_READABLE, ipc_accept, NULL))) {
		fprintf(stderr, "ipc: can't watch %s\n", ipc_path);
		goto fail;
	}
	setenv("SWAYSOCK", ipc_path, 1);
	return;
fail:
	if (ipc_socket >= 0) {
		close(ipc_socket);
		ipc_socket = -1;
		unlink(ipc_path);
	}
	ipc_path[0] = '\0';
}

static void
ipc_finish(void)
{
	IpcClient *cl, *tmp;
	ipc_send_shutdown();
	wl_list_for_each_safe(cl, tmp, &ipc_clients, link)
		ipc_client_destroy(cl);
	if (ipc_source)
		wl_event_source_remove(ipc_source);
	ipc_source = NULL;
	if (ipc_socket >= 0) {
		close(ipc_socket);
		unlink(ipc_path);
	}
	ipc_socket = -1;
}

/* A window event: new, close, focus, title, fullscreen_mode, floating,
 * urgent or move. Unmanaged X11 windows (menus, tooltips) have none. */
static void
ipc_event_window(Client *c, const char *change)
{
	struct json_object *obj;
	if (!c || client_is_unmanaged(c))
		return;
	/* A window's first event is "new", sent once it's placed. */
	if (!strcmp(change, "new"))
		c->ipc_announced = 1;
	else if (!c->ipc_announced)
		return;
	/* A window that maps again is new again, and waits for its "new" */
	if (!strcmp(change, "close"))
		c->ipc_announced = 0;
	if (!ipc_subscribed(IpcEvWindow))
		return;
	obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(change));
	json_object_object_add(obj, "container", ipc_json_window(c));
	ipc_broadcast(IpcEvWindow, obj);
}

/* A workspace event: init, empty, focus (with the old one), rename or
 * urgent. */
static void
ipc_event_workspace(const char *change, Workspace *cur, Workspace *old)
{
	struct json_object *obj;
	if (!ipc_subscribed(IpcEvWorkspace))
		return;
	obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(change));
	json_object_object_add(obj, "current", cur ? ipc_json_workspace(cur, selmon) : NULL);
	json_object_object_add(obj, "old", old ? ipc_json_workspace(old, selmon) : NULL);
	ipc_broadcast(IpcEvWorkspace, obj);
}

static void
ipc_event_output(void)
{
	struct json_object *obj;
	if (!ipc_subscribed(IpcEvOutput))
		return;
	obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string("unspecified"));
	ipc_broadcast(IpcEvOutput, obj);
}

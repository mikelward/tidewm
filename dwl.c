/*
 * See LICENSE and NOTICE for copyright and license details.
 */
#include <getopt.h>
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_output_swapchain_manager.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/interfaces/wlr_switch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <wlr/xwayland/shell.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

#include "xdg-output-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"
#include "hyprland-focus-grab-v1-protocol.h"
#include "util.h"

/* macros */
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
/* Whether a binding took key K's press: never a code past evdev's keys, as
 * a client's virtual keyboard can send, which consumed[] has no place for */
#define CONSUMED(K)             ((K) <= KEY_MAX && consumed[(K)])
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && (C)->ws == curws && !(C)->isminimized)
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof(*_l)); _l->notify = (H); wl_signal_add((E), _l); } while (0)

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize, CurEdge }; /* cursor; CurEdge: split.h's drag */
enum { XDGShell, LayerShell, X11 }; /* client types */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrUnmanaged, LyrOverlay,
	LyrIMPopup, LyrBlock, NUM_LAYERS }; /* scene layers */
enum { AxisUp, AxisRight, AxisDown, AxisLeft };
enum { FgFree, FgWaiting, FgPopup }; /* how the focus guard holds a window (focus.h) */

typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int dir;
	void (*func)(const Arg *);
	const Arg arg;
} Axis;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;

/* A layout's mfact and nmaster, kept while another is in force */
typedef struct {
	const void *lt;
	float mfact;
	int nmaster;
} LayoutMode;

/* A workspace spans every monitor; see workspace.h */
typedef struct {
	struct wl_list link;
	char *name;
	int persistent;
	int urgent; /* a window on it is urgent, as last announced */
	unsigned long ipc_id; /* the IPC's workspace id, set on first use */
	struct wlr_ext_workspace_handle_v1 *ext; /* its ext-workspace handle */
} Workspace;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom; /* layout-relative, includes border */
	struct wlr_box prev; /* layout-relative, includes border */
	struct wlr_box bounds; /* only width and height are used */
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wlr_ext_foreign_toplevel_handle_v1 *foreign_toplevel_handle;
	struct wlr_scene *image_capture_scene;
	struct wlr_ext_image_capture_source_v1 *image_capture_source;
	union {
		struct wlr_scene_tree *image_capture_tree;
		struct wlr_scene_surface *image_capture_scene_surface;
	} capture;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_geometry;
	struct wl_listener set_hints;
	struct wl_listener x11_commit; /* x11.h's, after the scene's own */
#endif
	unsigned int bw;
	float xscale; /* an X11 window's pixels to the layout's, on its monitor (x11.h) */
	Workspace *ws;
	int isfloating, isurgent, isfullscreen;
	int ismaximized; /* covers its monitor's window area, above the other tiles */
	int isminimized; /* hidden, on its workspace still, until it's focused (setminimized) */
	unsigned long keptopen_ws; /* the workspace it's kept open on, by IPC id, or 0 (keepopen.h) */
	int issticky; /* floating, it's on every workspace, as sway's sticky */
	int recenter; /* it followed its parent: to its middle once laid out */
	struct wlr_scene_buffer *dim; /* dim.h's layer over it */
	int nodim; /* the dim command's or a rule's: it never dims */
	int wantsfullscreen; /* asked for fullscreen while held, so waits for focus */
	struct wlr_box unmaximized; /* its place before it was maximized; none if it opened so */
	int held; /* how the focus guard holds it from focus: FgFree, FgWaiting, FgPopup */
	unsigned long focus_seq; /* when it last had focus, in the guard's count; 0 for never */
	unsigned long urgent_seq; /* when it was last marked urgent, in marks so far */
	int hint_urgent; /* an X11 window's urgency hint, as it last was */
	unsigned long ipc_id; /* the IPC's container id, set on first use */
	int ipc_announced; /* its "new" event is out, so others may follow */
} Client;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wlr_keyboard_group *wlr_group;
	struct wlr_keyboard *virtual_keyboard;

	xkb_keysym_t keysyms[2];
	uint32_t mods;
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
	xkb_layout_index_t layout; /* the active one, as its input event last said */
} KeyboardGroup;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener commit_lock_surface;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; /* monitor area, layout-relative */
	struct wlr_box w; /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	const Layout *lt[2];
	unsigned int sellt;
	float mfact;
	int gamma_lut_changed;
	int nmaster;
	double single; /* a lone window's width of its part's own (layout.h); 0: the rules' */
	char ltsymbol[16];
	int asleep;
	unsigned long ipc_id; /* the IPC's output id, set on first use */
	int want_place, want_x, want_y; /* where it goes in the layout: settings.h's Place */
	int head_moved; /* an output management request moves it */
	int file_off; /* off by its line in the config file, not a command since */
	int want_mode, want_width, want_height, want_refresh, want_custom; /* its mode once enabled, if given while off */
	int want_has_adaptive_sync, want_adaptive_sync; /* its adaptive sync once on, if given while off */
	LayoutMode modes[16]; /* each layout's, but the current one's */
	const Layout *rule_lt; /* config.h's monitor rule's */
	int lt_settled; /* its layout fields hold the current workspace's part */
	int lt_pending; /* its layout event waits until it's arranged */
};

typedef struct {
	const char *name;
	float mfact;
	int nmaster;
	float scale;
	const Layout *lt;
	enum wl_output_transform rr;
	int x, y;
} MonitorRule;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	const char *id;
	const char *title;
	const char *workspace; /* NULL: the current one */
	int isfloating;
	int monitor;
} Rule;

/* A lone tiled window's width, as a fraction, from a window area this
 * wide for its height (layout.h's lt_single) */
typedef struct {
	double min_aspect;
	double width;
} SingleRule;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyrules(Client *c);
static void arrange(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static void buttonpress(struct wl_listener *listener, void *data);
static void capturerequest(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static int vtkey(uint32_t mods, xkb_keysym_t sym);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);
static void cleanupmon(struct wl_listener *listener, void *data);
static void cleanuplisteners(void);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitlocksurface(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static KeyboardGroup *createkeyboardgroup(void);
static void createswitch(struct wlr_switch *wlr_switch);
static void switchset(int type, int on);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void focusclient(Client *c, LayerSurface *l, int lift);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Client *focustop(Monitor *m);
static Client *focusgo(Monitor *m);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);
static void handlesig(int signo);
static void incnmaster(const Arg *arg);
static void inputdevice(struct wl_listener *listener, void *data);
static const Key *keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
static void killclient(const Arg *arg);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static Monitor *matchrules(Client *c, const char **wsname);
static void maximizenotify(struct wl_listener *listener, void *data);
static void monocle(Monitor *m);
static void threecol(Monitor *m);
static void twocol(Monitor *m);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, LayerSurface* l, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void printstatus(void);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void quit(const Arg *arg);
static int runbinding(uint32_t mods, xkb_keysym_t sym);
static void rendermon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
static int client_is_pip(Client *c);
static void placefloating(Client *c, struct wlr_box asked);
static void fitsizehints(Client *c, struct wlr_box *box);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static int fullscreenwanted(Client *c);
static int dialogdepth(Client *d, Client *c);
static int descendsfrom(Client *d, Client *c);
static int stickybelow(Client *c, Client *top);
static void movemon(Client *c, Monitor *m, Workspace *ws);
static void dialogsfollow(Client *c, Workspace *from);
static int laidout(Client *c);
static void raisewithdialogs(Client *c);
static void setmaximized(Client *c, int maximized);
static void setminimized(Client *c, int minimized);
static void uncoverfor(Client *c);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setmon(Client *c, Monitor *m, Workspace *ws);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void spawn(const Arg *arg);
static void startdrag(struct wl_listener *listener, void *data);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static int unlockdead(void);
static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
static void requestactivate(Client *c);
static void seturgent(Client *c, int urgent);
static void viewws(const Arg *arg);
static void viewprevws(const Arg *arg);
static void movews(const Arg *arg);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
static void zoom(const Arg *arg);

/* variables */
static pid_t child_pid = -1;
static int locked;
static unsigned long urgent_count; /* windows marked urgent so far, for Client.urgent_seq */
static Client *focused_client;
/* Focus moving now was asked for, by a click, a key, button or scroll
 * binding, a command or the lock: only then does an on-demand layer give
 * up the keyboard */
static int focus_asked;
/* tidewm's own switch device, made the first time the switch command
 * moves it, as if a real switch had: for a test, which has no lid, or a
 * lid the hardware doesn't report */
static struct wlr_switch *ownswitch;
static struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_scene_tree *drag_icon;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;
static struct wlr_session *session;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;
static struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
static struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *ext_foreign_toplevel_image_capture_source_manager_v1;

static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_scene_rect *root_bg;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;

static struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;
static int consumed[KEY_MAX + 1];

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};
static struct wl_listener new_foreign_toplevel_capture_request = {.notify = capturerequest};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void setgeometryx11(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

/* configuration, allows nested code to access above variables */
#include "config.h"

#include "x11.h"

/* attempt to encapsulate suck into one file */
#include "client.h"

#include "ime.h"
#include "keepopen.h"

#include "workspace.h"
#include "ipc.h"
#include "settings.h"
#include "layout.h"
#include "focus.h"
#include "grab.h"
#include "split.h"
#include "floating.h"
#include "dim.h"
#include "capture.h"
#include "gesture.h"
#include "direction.h"

/* function implementations */
void
applybounds(Client *c, struct wlr_box *bbox)
{
	/* set minimum client size to 1 */
	c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
	c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

	if (!c->isfullscreen) {
		struct wlr_box min = {0}, max = {0};
		client_get_size_hints(c, &max, &min);
		c->geom.width = MAX(min.width + 2 * (int)c->bw, c->geom.width);
		c->geom.height = MAX(min.height + 2 * (int)c->bw, c->geom.height);
		/* Some clients set their max size to INT_MAX, which does not violate the
		 * protocol but it's unnecesary, as they can set their max size to zero. */
		if (max.width > 0 && 2 * (int)c->bw <= INT_MAX - max.width) /* Checks for overflow */
			c->geom.width = MIN(max.width + 2 * (int)c->bw, c->geom.width);
		if (max.height > 0 && 2 * (int)c->bw <= INT_MAX - max.height) /* Checks for overflow */
			c->geom.height = MIN(max.height + 2 * (int)c->bw, c->geom.height);
	}

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

void
applyrules(Client *c)
{
	const char *wsname;
	Monitor *mon = matchrules(c, &wsname);
	/* Only the last rule's workspace is made: an earlier one would be left
	 * empty. Without one, a window opens where it was launched from, if
	 * the focus guard says so (focus.h). */
	setmon(c, mon, wsname ? ws_get(wsname) : fg_placed(c));
}

void
arrange(Monitor *m)
{
	Client *c, *p;
	int recentered, fullscreen = 0;

	if (!m->wlr_output->enabled)
		return;

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));
			client_set_suspended(c, !VISIBLEON(c, m)
					&& (!c->image_capture_source
					|| wl_list_empty(&c->image_capture_source->resources)));
		}
	}

	/* The black behind a fullscreen window, while one is shown here,
	 * whatever has focus: its dialog, or another window not yet focused
	 * over one brought back. Another window here taking focus ends it. */
	wl_list_for_each(c, &clients, link)
		fullscreen |= VISIBLEON(c, m) && c->isfullscreen;
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, fullscreen);

	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));

	/* We move all clients (except fullscreen and unmanaged) to LyrTile while
	 * in floating layout to avoid "real" floating clients be always on top */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->scene->node.parent == layers[LyrFS])
			continue;

		wlr_scene_node_reparent(&c->scene->node,
				(!m->lt[m->sellt]->arrange && c->isfloating)
						? layers[LyrTile]
						: (m->lt[m->sellt]->arrange && c->isfloating)
								? layers[LyrFloat]
								: c->scene->node.parent);
	}

	/* A lone window has the single-window width, in any layout */
	if (m->lt[m->sellt]->arrange && !lt_single(m))
		m->lt[m->sellt]->arrange(m);
	/* A maximized window covers the window area, above the other tiles
	 * but under its own dialogs. Floating, it stacks by focus, as the
	 * other floating windows do. */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && c->ismaximized && !c->isfullscreen) {
			resize(c, m->w, 0);
			if (!c->isfloating)
				raisewithdialogs(c);
		}
	}
	/* A dialog that followed its parent goes to its middle, now that the
	 * parent is laid out here: each after its own parent */
	do {
		recentered = 0;
		wl_list_for_each(c, &clients, link) {
			if (!c->recenter || !VISIBLEON(c, m) || ((p = client_get_parent(c))
					&& p->recenter && VISIBLEON(p, m)))
				continue;
			c->recenter = 0;
			recentered = 1;
			if (c->isfloating && !c->isfullscreen)
				placefloating(c, c->geom);
		}
	} while (recentered);
	dim_update();
	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *l;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (!layer_surface->initialized)
			continue;

		if (exclusive != (layer_surface->current.exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	LayerSurface *l;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (!wlr_box_equal(&usable_area, &m->w)) {
		m->w = usable_area;
		arrange(m);
	}

	/* Arrange non-exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	/* Find topmost keyboard interactive layer that has indicated it wants
	 * exclusive access to the keyboard, if such a layer exists */
	for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
			if (locked ||
					l->layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE ||
					!l->mapped)
				continue;
			focusclient(NULL, l, 0);
			return;
		}
	}
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	const Axis *a;
	unsigned int adir;
	LayerSurface *l = NULL;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	double factor;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	/* Super with a scroll isn't Super tapped */
	settings_tap_over();

	/* A scroll's end, with no delta, went no way, so runs no binding */
	if (!locked && event->delta != 0) {
		if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL)
			adir = event->delta > 0 ? AxisDown : AxisUp;
		else
			adir = event->delta > 0 ? AxisRight : AxisLeft;
		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		/* The config file's, as sway's button4 to button7, over a
		 * window or none; then config.h's, unless the file has its own */
		xytonode(cursor->x, cursor->y, NULL, NULL, &l, NULL, NULL);
		if (!l && settings_run_button(mods, adir == AxisUp ? SettingsScrollUp
				: adir == AxisDown ? SettingsScrollDown
				: adir == AxisLeft ? SettingsScrollLeft : SettingsScrollRight))
			return;
		for (a = axes; !settings_mouse() && a < END(axes); a++) {
			if (CLEANMASK(mods) == CLEANMASK(a->mod) && adir == a->dir && a->func) {
				focus_asked++;
				a->func(&a->arg);
				focus_asked--;
				return;
			}
		}
	}

	/* Notify the client with pointer focus of the axis event, at the
	 * device's scroll factor (sway's scroll_factor) */
	factor = settings_scroll_factor(&event->pointer->base);
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta * factor,
			(int32_t)round(event->delta_discrete * factor), event->source,
			event->relative_direction);
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	const Button *b;
	unsigned int drag;
	SplitEdge edge;
	Monitor *edge_mon;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		/* A second button while the edge is dragged goes nowhere */
		if (cursor_mode == CurEdge)
			return;
		/* A click is you moving on, from whatever a grant was for */
		fg_click();
		/* Super+click and Super+drag aren't Super tapped */
		settings_tap_over();
		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		/* The master/stack edge, on the monitor focused before the press */
		edge_mon = event->button == BTN_LEFT && !CLEANMASK(mods)
				? split_edge_at(cursor->x, cursor->y, &edge) : NULL;
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		ipc_output_focus();
		if (locked)
			break;
		if (edge_mon) {
			split_start(edge_mon, &edge, cursor->x);
			return;
		}

		/* Change focus if the button was _pressed_ over a client
		   or a layer surface with on-demand keyboard interactivity */
		xytonode(cursor->x, cursor->y, &surface, &c, &l, NULL, NULL);
		/* A click outside a focus grab ends it, and goes no further */
		if (grab_click(surface, event->button))
			return;
		focus_asked++;
		/* Inside a focus grab, the keyboard stays where the grab has it */
		if (!grab_has_keyboard()) {
			if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
				focusclient(c, NULL, 1);
			else if (l && l->layer_surface->current.keyboard_interactive)
				focusclient(NULL, l, 0);
		}
		focus_asked--;

		/* The config file's: its --whole-window bindings, over a window
		 * or none, as sway's, then its floating_modifier's drags */
		if (!l && settings_run_button(mods, event->button))
			return;
		if (!l && (drag = settings_floating_drag(mods, event->button)) != CurNormal) {
			focus_asked++;
			moveresize(&(const Arg){.ui = drag});
			focus_asked--;
			return;
		}
		/* config.h's, unless the file says what the mouse does */
		for (b = buttons; !settings_mouse() && b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				focus_asked++;
				b->func(&b->arg);
				focus_asked--;
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		if (grab_release(event->button)) {
			cursor_mode = CurNormal;
			return;
		}
		/* A button up ends a drag of the edge, and the windows never saw
		 * its press */
		if (cursor_mode == CurEdge) {
			cursor_mode = CurNormal;
			wlr_seat_pointer_clear_focus(seat);
			motionnotify(0, NULL, 0, 0, 0, 0);
			return;
		}
		/* If you released any buttons, we exit interactive move/resize mode. */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			cursor_mode = CurNormal;
			/* Update the cursor */
			wlr_seat_pointer_clear_focus(seat);
			motionnotify(0, NULL, 0, 0, 0, 0);
			/* Drop the window off on its new monitor */
			selmon = xytomon(cursor->x, cursor->y);
			movemon(grabc, selmon, 0);
			ipc_output_focus();
			grabc = NULL;
			return;
		}
		cursor_mode = CurNormal;
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred. A held button keeps
	 * that focus, so a second button goes where the first did; the shell's
	 * polkit agent asks which window that was. */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
		fg_delivered(seat->pointer_state.focused_surface);
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
capturerequest(struct wl_listener *listener, void *data)
{
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
	Client *view = request->toplevel_handle->data;

	if (!view->image_capture_source) {
		view->image_capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
				&view->image_capture_scene->tree.node, event_loop, alloc, drw);
		if (!view->image_capture_source)
			return;
	}

	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
		request, view->image_capture_source);

	client_set_suspended(view, 0);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

/* Whether these keys are Ctrl+Alt and F1…F12, switching to that VT if so:
 * built in, as in sway, where a session has VTs */
int
vtkey(uint32_t mods, xkb_keysym_t sym)
{
	if (CLEANMASK(mods) != (WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT)
			|| sym < XKB_KEY_F1 || sym > XKB_KEY_F12)
		return 0;
	if (session)
		wlr_session_change_vt(session, sym - XKB_KEY_F1 + 1);
	return 1;
}

void
checkidleinhibitor(struct wlr_surface *exclude)
{
	int inhibited = 0, unused_lx, unused_ly;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		if (exclude != surface && (bypass_surface_visibility || (!tree
				|| wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

void
cleanup(void)
{
	ipc_finish();
	fg_finish();
	grab_finish();
	fl_finish();
	if (ownswitch) {
		wlr_switch_finish(ownswitch);
		free(ownswitch);
		ownswitch = NULL;
	}
	dim_finish();
	settings_finish();
	lt_finish();
	ws_protocol_finish();
	cleanuplisteners();
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
	xwayland = NULL;
#endif
	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(-child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	input_method_relay_finish(input_method_relay);
	input_method_relay = NULL;

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	wlr_backend_destroy(backend);

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	size_t i;

	/* Its part of the current workspace, for when it's plugged in again */
	lt_unplug(m);
	split_unplug(m);
	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_ext_workspace_group_handle_v1_output_leave(ws_group, m->wlr_output);
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	closemon(m);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);
	settings_output_gone();
	ipc_event_output();
}

void
cleanuplisteners(void)
{
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_activate.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
	wl_list_remove(&new_foreign_toplevel_capture_request.link);
	capture_finish();
	gesture_finish();
#ifdef XWAYLAND
	/* Added only if XWayland started: it can't without an Xwayland binary */
	if (xwayland) {
		wl_list_remove(&new_xwayland_surface.link);
		wl_list_remove(&xwayland_ready.link);
	}
#endif
}

void
closemon(Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c, *tmp, *last = wl_container_of(clients.prev, last, link);
	int i = 0, nmons = wl_list_length(&mons);
	if (!nmons) {
		selmon = NULL;
	} else if (m == selmon) {
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);

		if (!selmon->wlr_output->enabled)
			selmon = NULL;
	}

	/* setmon() moves each window to the end of the list, as a new one, so
	 * go safely, and stop after the one that was last. The focus moving
	 * with them isn't you moving on, for the focus guard. */
	fg_focusing++;
	wl_list_for_each_safe(c, tmp, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
					.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->ws);
		if (c == last)
			break;
	}
	focusclient(focustop(selmon), NULL, 1);
	fg_focusing--;
	printstatus();
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->current.layer]];
	struct wlr_layer_surface_v1_state old_state;
	int mapping;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		return;
	}

	if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
		return;
	mapping = !l->mapped && layer_surface->surface->mapped;
	l->mapped = layer_surface->surface->mapped;

	if (scene_layer != l->scene->node.parent) {
		wlr_scene_node_reparent(&l->scene->node, scene_layer);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
		wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
				< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer));
	}

	arrangelayers(l->mon);
	/* A focus grab that names it may give it the keyboard now (grab.h) */
	if (mapping)
		grab_mapped(layer_surface->surface);
}

void
commitlocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, commit_lock_surface);

	if (!m->lock_surface->surface->mapped)
		return;

	wl_list_remove(&m->commit_lock_surface.link);
	wl_list_init(&m->commit_lock_surface.link);

	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);
	const char *wsname;

	if (c->surface.xdg->initial_commit) {
		/*
		 * Get the monitor this client will be rendered on
		 * Note that if the user set a rule in which the client is placed on
		 * a different monitor based on its title, this will likely select
		 * a wrong monitor. Its workspace waits for mapnotify(), which
		 * matches the rules again: one made now would be left empty if
		 * the window changes its app ID or title first.
		 */
		setmon(c, matchrules(c, &wsname), NULL);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
				WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
				| WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
		/* Maximized before it maps if it asked, as its first configure
		 * says, and not if it's mapping again maximized no more */
		c->ismaximized = c->surface.xdg->toplevel->requested.maximized;
		client_set_maximized(c, c->ismaximized);
		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
		if (!c->isfloating)
			client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
					WLR_EDGE_RIGHT);
		return;
	}

	resize(c, c->geom, (c->isfloating && !c->isfullscreen));
	/* Its content type comes with a commit */
	dim_update_one(c);
}

void
commitpopup(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
	LayerSurface *l = NULL;
	Client *c = NULL;
	struct wlr_box box;
	int type = -1;

	if (!popup->base->initial_commit)
		return;

	type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
	if (!popup->parent || type < 0)
		return;
	popup->base->surface->data = wlr_scene_xdg_surface_create(
			popup->parent->data, popup->base);
	if ((l && !l->mon) || (c && !c->mon)) {
		wlr_xdg_popup_destroy(popup);
		return;
	}
	box = type == LayerShell ? l->mon->m : c->mon->w;
	box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x - c->surface.xdg->geometry.x);
	box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y - c->surface.xdg->geometry.y);
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
	wl_list_remove(&listener->link);
	free(listener);
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Into the group for its settings: kb_group, unless the config file's
	 * input lines give it others */
	settings_keyboard_added(keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(settings_xkb_flags);
	if (!(keymap = xkb_keymap_new_from_names(context, &xkb_rules,
				XKB_KEYMAP_COMPILE_NO_FLAGS))
			&& !(keymap = settings_keymap_without_environment()))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = data;
	LayerSurface *l;
	struct wlr_surface *surface = layer_surface->surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output
			&& !(layer_surface->output = selmon ? selmon->wlr_output : NULL)) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
	LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
			< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
	wlr_surface_send_enter(surface, layer_surface->output);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data
			= wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);
	LISTEN(&lock_surface->surface->events.commit, &m->commit_lock_surface, commitlocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	struct wlr_output_state state;
	Monitor *m;
	OutputConfig oc, file;
	int configured;

	if (!wlr_output_init_render(wlr_output, alloc, drw))
		return;

	m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);

	wlr_output_state_init(&state);
	/* Initialize monitor state using configured rules */
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
			m->lt[0] = m->rule_lt = r->lt;
			m->lt[1] = &layouts[LENGTH(layouts) > 1 && r->lt != &layouts[1]];
			strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
			wlr_output_state_set_scale(&state, r->scale);
			wlr_output_state_set_transform(&state, r->rr);
			break;
		}
	}

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

	/* config.h's monitor rule, then the config file's output lines over
	 * it, and the preferred mode unless they give one, as on reload */
	settings_output_defaults(m, &oc);
	if ((configured = settings_output_config(wlr_output, &file)))
		settings_merge_output(&oc, &file);
	settings_preferred_mode(wlr_output, &oc);
	settings_output_state(m, &oc, &state);
	if (!wlr_output_commit_state(wlr_output, &state))
		settings_output_fallback(wlr_output, configured);
	wlr_output_state_finish(&state);
	settings_output_set(m, &oc, 1);

	wl_list_insert(&mons, &m->link);
	printstatus();

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	wlr_ext_workspace_group_handle_v1_output_enter(ws_group, wlr_output);
	if (!wlr_output->enabled)
		/* Disabled by its config, it changes no layout, so nothing else
		 * would tell the output manager it's there to be enabled */
		updatemons(NULL, NULL);
	else
		settings_output_place(m);
	ipc_event_output();
	settings_keep_output_on(m);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client creates a new toplevel (application window). */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = borderpx;

	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void
createpointer(struct wlr_pointer *pointer)
{
	/* config.h's libinput settings, then the config file's input lines */
	settings_pointer_added(pointer);
	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
createpopup(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client (either xdg-shell or layer-shell)
	 * creates a new popup. */
	struct wlr_xdg_popup *popup = data;
	LISTEN_STATIC(&popup->base->surface->events.commit, commitpopup);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	if (active_constraint == constraint)
		return;

	if (active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);

	active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		/* An X11 window's hint is in its own pixels (x11.h) */
		double scale = c->type == X11 ? c->xscale : 1;
		wlr_cursor_warp(cursor, NULL, sx / scale + c->geom.x + c->bw,
				sy / scale + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy_decoration);
	c->decoration = NULL;

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), NULL, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(wlr_surface_get_root_surface(data));
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_commit.link);
	wlr_scene_node_destroy(&l->scene->node);
	wlr_scene_node_destroy(&l->popups->node);
	free(l);
}

void
destroylock(SessionLock *lock, int unlock)
{
	wlr_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	focusclient(focustop(selmon), NULL, 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);
	wl_list_remove(&m->commit_lock_surface.link);

	if (lock_surface->surface != seat->keyboard_state.focused_surface)
		return;

	if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
		surface = wl_container_of(cur_lock->surfaces.next, surface, link);
		client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
	} else if (!locked) {
		focusclient(focustop(selmon), NULL, 1);
	} else {
		wlr_seat_keyboard_clear_focus(seat);
	}
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->activate.link);
		wl_list_remove(&c->associate.link);
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->dissociate.link);
		wl_list_remove(&c->set_geometry.link);
		wl_list_remove(&c->set_hints.link);
	} else
#endif
	{
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
		wl_list_remove(&c->maximize.link);
		if (c->decoration) {
			wl_list_remove(&c->set_decoration_mode.link);
			wl_list_remove(&c->destroy_decoration.link);
		}
	}
	free(c);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

Monitor *
dirtomon(enum wlr_direction dir)
{
	struct wlr_output *next;
	if (!wlr_output_layout_get(output_layout, selmon->wlr_output))
		return selmon;
	if ((next = wlr_output_layout_adjacent_output(output_layout,
			dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(output_layout,
			dir ^ (WLR_DIRECTION_LEFT | WLR_DIRECTION_RIGHT |
					WLR_DIRECTION_UP | WLR_DIRECTION_DOWN),
			selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	return selmon;
}

void
focusclient(Client *c, LayerSurface *l, int lift)
{
	struct wlr_surface *surface, *old = seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	/* Focus brings a minimized window back first, whatever asks for it: a
	 * command, a grant, or the lock going with it atop the focus stack */
	if (c && c->isminimized)
		setminimized(c, 0);

	if (locked)
		return;

	/* Raise client in stacking order if requested, its dialogs above it */
	if (c && lift)
		raisewithdialogs(c);

	if (c && client_surface(c) == old)
		return;

	/* A focus grab that keeps the keyboard, unless you ask, keeps its
	 * windows' menus open, and the window it holds the selected one, though
	 * a new one went atop the focus stack as it mapped (grab.h) */
	old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l);
	if (grab_keeps(c, l)) {
		if (old_c && !client_is_unmanaged(old_c)) {
			wl_list_remove(&old_c->flink);
			wl_list_insert(&fstack, &old_c->flink);
		}
		return;
	}
	if (old_client_type == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->held = FgFree;
		uncoverfor(c);
		/* Held no more, it goes fullscreen if it asked to while held */
		if (c->wantsfullscreen)
			setfullscreen(c, 1);
		seturgent(c, 0);

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!(old_l && old_l->layer_surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
				&& !(old_c && client_wants_focus(old_c))
				&& !seat->drag)
			client_set_border_color(c, focuscolor);
	}

	/* If an overlay is focused, don't focus or activate the client,
	 * but only update its position in fstack to render its border with focuscolor
	 * and focus it after the overlay is closed. */
	if (!l && old && (!c || client_surface(c) != old)
			&& old_client_type == LayerShell && wlr_scene_node_coords(
				&old_l->scene->node, &unused_lx, &unused_ly)
			&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP
			&& old_l->layer_surface->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
		return;
	/* Nor one with on-demand focus you're typing in, unless you ask */
	if (old_client_type == LayerShell && fg_layer_keeps(old_l, l))
		return;

	if (focused_client && focused_client != c && !(c && client_is_unmanaged(c))) {
		struct wlr_surface *s = client_surface(focused_client);
		if (c)
			client_set_border_color(focused_client, bordercolor);
		if (s && s->mapped)
			client_activate_surface(s, 0);
		focused_client = NULL;
	}
	printstatus();
	dim_update();

	if (!c && !l) {
		/* With no client, all we have left is to clear focus */
		input_method_relay_set_focus(input_method_relay, NULL);
		wlr_seat_keyboard_notify_clear_focus(seat);
		fg_focused(NULL);
		return;
	}

	surface = c ? client_surface(c) : l->layer_surface->surface;
	input_method_relay_set_focus(input_method_relay, surface);
	client_notify_enter(surface, wlr_seat_get_keyboard(seat));

	if (l)
		return;

	client_activate_surface(surface, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);

	if (!client_is_unmanaged(c)) {
		focused_client = c;
		/* Undimmed before the event says so */
		dim_update();
		ipc_event_window(c, "focus");
		fg_focused(c);
	}
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons) {
		do /* don't switch to disabled mons */
			selmon = dirtomon(arg->i);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	}
	focusclient(focusgo(selmon), NULL, 1);
	ipc_output_focus();
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(selmon);
	if (!sel) {
		/* Only windows the focus guard holds, if any: go to one */
		if ((c = focusgo(selmon)))
			focusclient(c, NULL, 1);
		return;
	}
	if (sel->isfullscreen && !client_has_children(sel))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon) && c->held != FgPopup)
				break; /* found it, never a status popup */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon) && c->held != FgPopup)
				break; /* found it, never a status popup */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, NULL, 1);
}

/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *
focustop(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &fstack, flink) {
		if (VISIBLEON(c, m) && !c->held)
			return c;
	}
	return NULL;
}

/* The window to focus when you go to m, by a workspace switch or focus
 * output: its top one, or one the focus guard is holding, waiting for
 * you to go there, if that's all it has. Never a status popup. */
Client *
focusgo(Monitor *m)
{
	Client *c = focustop(m);
	if (c)
		return c;
	wl_list_for_each(c, &fstack, flink) {
		if (VISIBLEON(c, m) && c->held == FgWaiting)
			return c;
	}
	return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void
gpureset(struct wl_listener *listener, void *data)
{
	struct wlr_renderer *old_drw = drw;
	struct wlr_allocator *old_alloc = alloc;
	struct Monitor *m;
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't recreate renderer");

	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&drw->events.lost, &gpu_reset);

	wlr_compositor_set_renderer(compositor, drw);

	wl_list_for_each(m, &mons, link) {
		wlr_output_init_render(m->wlr_output, alloc, drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}

void
handlesig(int signo)
{
	if (signo == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0);
	else if (signo == SIGINT || signo == SIGTERM)
		quit(NULL);
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

/* A switch device, such as the lid's, whose toggles run the config file's
 * bindswitch lines */
typedef struct {
	struct wl_listener toggle;
	struct wl_listener destroy;
} Switch;

static void
switchtoggled(struct wl_listener *listener, void *data)
{
	struct wlr_switch_toggle_event *event = data;
	settings_run_switch(event->switch_type, event->switch_state == WLR_SWITCH_STATE_ON);
}

static void
switchdestroyed(struct wl_listener *listener, void *data)
{
	Switch *sw = wl_container_of(listener, sw, destroy);
	wl_list_remove(&sw->toggle.link);
	wl_list_remove(&sw->destroy.link);
	free(sw);
}

void
createswitch(struct wlr_switch *wlr_switch)
{
	Switch *sw = ecalloc(1, sizeof(*sw));
	LISTEN(&wlr_switch->events.toggle, &sw->toggle, switchtoggled);
	LISTEN(&wlr_switch->base.events.destroy, &sw->destroy, switchdestroyed);
}

static const struct wlr_switch_impl ownswitch_impl = {.name = "tidewm-switch"};

void
switchset(int type, int on)
{
	struct timespec now;
	struct wlr_switch_toggle_event event = {
		.switch_type = (enum wlr_switch_type)type,
		.switch_state = on ? WLR_SWITCH_STATE_ON : WLR_SWITCH_STATE_OFF,
	};
	clock_gettime(CLOCK_MONOTONIC, &now);
	event.time_msec = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	if (!ownswitch) {
		ownswitch = ecalloc(1, sizeof(*ownswitch));
		wlr_switch_init(ownswitch, &ownswitch_impl, "tidewm-switch");
		createswitch(ownswitch);
	}
	wl_signal_emit_mutable(&ownswitch->events.toggle, &event);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		createswitch(wlr_switch_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (settings_has_keyboard())
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

const Key *
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	const Key *k;
	for (k = keys; k < END(keys); k++) {
		if (CLEANMASK(mods) == CLEANMASK(k->mod) && sym == k->keysym && k->func)
			return k;
	}
	return NULL;
}

void
keypress(struct wl_listener *listener, void *data)
{
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	struct wlr_keyboard *kb = &group->wlr_group->keyboard;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
	int handled = 0;

	// Get the keysyms for level 0 (normal) and level 1 (shifted)
	// Only one keysym for each level:
	// multiple keysyms per keycode don't really exist in the real world
	for (int i = 0; i < 2; i++)
		group->keysyms[i] = keymap_get_one_sym_by_level(kb->keymap, keycode, layout, i);
	group->mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	/* On _press_, attempt to process a compositor keybinding: on the lock
	 * screen, only the config file's --locked ones. A release binding runs
	 * when its key comes up, and the client sees both, as in sway. */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* You moving on, before a binding can grant anything */
		fg_key();
		settings_key_pressed(event->keycode, group->mods, group->keysyms);
		for (int i = 0; i < 2; i++) {
			if (runbinding(group->mods, group->keysyms[i])) {
				if (event->keycode <= KEY_MAX)
					consumed[event->keycode] = 1;
				handled = 1;
				break;
			}
		}
		/* Ctrl+Alt+F1…F12 switch VT even where the config file binds
		 * nothing to them, locked or not: a file that lost its keys
		 * (see settings_init) can't leave you with no way out. A
		 * binding of the file's on these keys came first, above, and
		 * a release binding one of them armed keeps it. */
		if (!handled && !(settings_tap.armed && settings_tap.keycode == event->keycode)
				&& vtkey(group->mods, group->keysyms[0])) {
			if (event->keycode <= KEY_MAX)
				consumed[event->keycode] = 1;
			handled = 1;
		}
	} else {
		settings_key_released(event->keycode);
	}

	/* A binding that moved the keyboard to another group took its key out
	 * of this one, whose timer the release would never stop */
	if (handled && group->wlr_group->keyboard.repeat_info.delay > 0
			&& settings_key_held(&group->wlr_group->keyboard, event->keycode))
		wl_event_source_timer_update(group->key_repeat_source,
				group->wlr_group->keyboard.repeat_info.delay);
	else
		wl_event_source_timer_update(group->key_repeat_source, 0);

	if (handled)
		return;
	/* The window a key press goes to, straight or through an input
	 * method, which types into it, for the shell's polkit agent */
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && !CONSUMED(event->keycode))
		fg_delivered(seat->keyboard_state.focused_surface);
	if (input_method_keyboard_grab_forward_key(group, event))
		return;

	if (CONSUMED(event->keycode)) {
		if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED)
			consumed[event->keycode] = 0;
		return; /* Don't pass to the client the release event of a handled key-press */
	}
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	/* A key, xkb_switch_layout or a client may have switched the layout */
	settings_layout_check(group);
	if (input_method_keyboard_grab_forward_modifiers(group))
		return;

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	/* runbinding() knows which bindings run on the lock screen */
	if (group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (int i = 0; i < 2; i++)
		if (runbinding(group->mods, group->keysyms[i]))
			break;

	return 0;
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		closeclient(sel); /* keepopen.h */
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	wlr_scene_node_set_enabled(&locked_bg->node, 1);
	if (cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	focus_asked++;
	focusclient(NULL, NULL, 0);
	focus_asked--;

	lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;

	/* Cancel any interactive move/resize and take the pointer focus away
	 * from the clients below the lock screen */
	cursor_mode = CurNormal;
	grabc = NULL;
	motionnotify(0, NULL, 0, 0, 0, 0);

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p = NULL, *was_focused;
	Client *w, *c = wl_container_of(listener, c, map);
	Monitor *m;
	char *grant_ws;
	struct wlr_box asked;
	int i;
#ifdef XWAYLAND
	int asked_width = 0, asked_height = 0; /* an X11 window's, in its own pixels */
#endif

	struct wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state = {
		.app_id = client_get_appid(c),
		.title = client_get_title(c),
	};

	/* Create scene tree for this client and its border */
	c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
	/* Enabled later by a call to arrange() */
	wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	client_get_geometry(c, &c->geom);
#ifdef XWAYLAND
	/* An X11 window is shown at its size in the layout, as it commits too */
	if (client_is_x11(c)) {
		LISTEN(&client_surface(c)->events.commit, &c->x11_commit, x11_committed);
		x11_scale_scene(c);
	}
#endif

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node, layers[LyrUnmanaged]);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		if (client_wants_focus(c))
			focusclient(c, NULL, 1);
		goto unset_fullscreen;
	}

#ifdef XWAYLAND
	/* An X11 window says it's fullscreen in _NET_WM_STATE before it maps,
	 * which wlroots reads but doesn't announce */
	if (client_is_x11(c))
		c->isfullscreen = client_wants_fullscreen(c);
#endif
	/* The focus guard's say, before its border shows whether it's urgent */
	grant_ws = fg_open(c);
	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
				c->isurgent ? urgentcolor : bordercolor);
		c->border[i]->node.data = c;
	}
	dim_attach(c);

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT |
			WLR_EDGE_RIGHT);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;
	asked = c->geom; /* for placefloating, after the rules and arrange */
#ifdef XWAYLAND
	/* Before its monitor configures it so */
	if (client_is_x11(c)) {
		asked_width = c->surface.xwayland->width;
		asked_height = c->surface.xwayland->height;
	}
#endif
	if (client_is_x11(c)) {
		int x = c->geom.x - (int)c->bw, y = c->geom.y - (int)c->bw;
		c->geom.x = c->geom.x < sgeom.x ? x : MAX(x, sgeom.x);
		c->geom.y = c->geom.y < sgeom.y ? y : MAX(y, sgeom.y);
	}

	/* Insert this client into client lists: at the end of the stack, so
	 * the master stays put, unless new_window says otherwise once the
	 * rules have placed it. The window it may go next after is the one
	 * that had focus as it opened. */
	was_focused = focustop(selmon);
	wl_list_insert(clients.prev, &c->link);
	wl_list_insert(&fstack, &c->flink);

	c->foreign_toplevel_handle = wlr_ext_foreign_toplevel_handle_v1_create(
			foreign_toplevel_list, &foreign_toplevel_state);
	c->foreign_toplevel_handle->data = c;
	c->image_capture_scene = wlr_scene_create();
	if (c->type == XDGShell)
		c->capture.image_capture_tree = wlr_scene_xdg_surface_create(&c->image_capture_scene->tree, c->surface.xdg);
#ifdef XWAYLAND
	else
		c->capture.image_capture_scene_surface = wlr_scene_surface_create(&c->image_capture_scene->tree, c->surface.xwayland->surface);
#endif


	/* Set initial monitor, workspace, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same workspace and monitor as its parent.
	 * If there is no parent, apply rules */
	/* Its focusing as it maps isn't you moving on */
	fg_mapping = c;
	if ((p = client_get_parent(c))) {
		c->isfloating = 1;
		/* The rules don't see a dialog: picture-in-picture is sticky here */
		c->issticky = client_is_pip(c);
		setmon(c, p->mon, p->ws);
	} else {
		applyrules(c);
	}
	/* The config file's for_window rules, whose events wait for the new one */
	settings_apply_rules(c);
	fg_mapping = NULL;
	lt_place_new(c, was_focused);
	if (c->isfloating && !c->isfullscreen && c->mon) {
#ifdef XWAYLAND
		/* An X11 window's size, at the scale of the monitor it has now,
		 * not the one its first place is on (x11.h) */
		if (client_is_x11(c)) {
			asked.width = (int)lroundf((float)asked_width / x11_scale(c->mon)) + 2 * (int)c->bw;
			asked.height = (int)lroundf((float)asked_height / x11_scale(c->mon)) + 2 * (int)c->bw;
		}
#endif
		placefloating(c, asked);
	}
	printstatus();
	/* Now it has a monitor, workspace and floating state; its focus waited for this */
	ipc_event_window(c, "new");
	if (c == focused_client)
		ipc_event_window(c, "focus");
	fg_opened(c, grant_ws);
	/* A focus grab that names it may give it the keyboard now (grab.h) */
	grab_mapped(client_surface(c));

unset_fullscreen:
	/* One the focus guard holds leaves the window you're in as it is */
	if (c->held)
		return;
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	/* A minimized one covers nothing, so it keeps it for coming back */
	wl_list_for_each(w, &clients, link) {
		if (w != c && w != p && w->isfullscreen && !w->isminimized
				&& m == w->mon && w->ws == c->ws)
			setfullscreen(w, 0);
	}
}

/* Matches c against the rules: sets whether it floats, and returns the
 * monitor they give it, with the name of its workspace in *wsname, or
 * NULL for the current one. */
Monitor *
matchrules(Client *c, const char **wsname)
{
	const char *appid, *title;
	int i;
	const Rule *r;
	Monitor *mon = selmon, *m;

	*wsname = NULL;
	appid = client_get_appid(c);
	title = client_get_title(c);
	/* Decided afresh each time, at the first commit and at the map, as
	 * the app ID and title may have come in between */
	c->isfloating = 0;
	/* Picture-in-picture floats on every workspace (tide's SPEC §6.4) */
	c->issticky = client_is_pip(c);

	/* Once a config file is loaded, its for_window rules replace these */
	for (r = rules; !settings && r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			if (r->workspace)
				*wsname = r->workspace;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	c->isfloating |= client_is_float_type(c) || fg_nameless(c) || c->issticky;
	return mon;
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. tidewm maximizes, as tide's SPEC §6.3 has
	 * it, and the configure that says so answers the request. Before the
	 * first commit, the initial configure answers it (commitnotify). */
	Client *c = wl_container_of(listener, c, maximize);
	if (c->surface.xdg->initialized)
		setmaximized(c, c->surface.xdg->toplevel->requested.maximized);
}

/* Whether a layout places c: it's tiled, and its own workspace's part of
 * its monitor, showing or not, has an arranging layout, or will have one,
 * for a part not made yet, which takes the monitor's defaults */
static int
laidout(Client *c)
{
	const Layout *lt;
	if (c->isfloating || !c->mon)
		return 0;
	lt = lt_part_layout(c->ws, c->mon);
	return lt ? lt->arrange != NULL : c->mon->lt[c->mon->sellt]->arrange != NULL;
}

/* How many parents d goes up to reach c: 1 for c's own dialog, 2 for its
 * dialog's, and 0 for a window that's neither */
int
dialogdepth(Client *d, Client *c)
{
	Client *p;
	int n = 1;
	for (p = client_get_parent(d); p; p = client_get_parent(p), n++)
		if (p == c)
			return n;
	return 0;
}

/* Whether d is c's dialog, or its dialog's, and so on */
int
descendsfrom(Client *d, Client *c)
{
	return dialogdepth(d, c) > 0;
}

/* Whether c, or a parent of it short of top, is sticky and floating: on
 * every workspace, where its dialogs stay with it */
int
stickybelow(Client *c, Client *top)
{
	for (; c && c != top; c = client_get_parent(c))
		if (c->issticky && c->isfloating)
			return 1;
	return 0;
}

/* Moves c to m, on ws, as a command, a key or a drag moves a window: its
 * dialogs go with it */
void
movemon(Client *c, Monitor *m, Workspace *ws)
{
	setmon(c, m, ws);
	dialogsfollow(c, NULL);
}

/* c moved, from the workspace from, which its caller tidies: its dialogs
 * go with it, to its workspace and monitor, and to the middle of it again
 * once it's laid out there (tide's SPEC §6.4). A sticky one stays, with its own
 * dialogs: it's on every workspace. A workspace a dialog leaves goes if
 * nothing else keeps it, and says whether it's still urgent, as c's says
 * whether it now is. */
void
dialogsfollow(Client *c, Workspace *from)
{
	Client *d, **follow;
	Workspace **left, *was;
	size_t n = 0, i, j, nleft = 0;
	int depth, deepest = 0;
	wl_list_for_each(d, &fstack, flink)
		n++;
	follow = ecalloc(n ? n : 1, sizeof(*follow));
	left = ecalloc(n ? n : 1, sizeof(*left));
	/* Gathered first: moving one changes the lists */
	n = 0;
	wl_list_for_each(d, &fstack, flink) {
		if (d != c && descendsfrom(d, c) && !stickybelow(d, c)
				&& (d->mon != c->mon || d->ws != c->ws)) {
			follow[n++] = d;
			deepest = MAX(deepest, dialogdepth(d, c));
		}
	}
	/* Each dialog after its parent, so it's centered where its parent is */
	for (depth = 1; depth <= deepest; depth++) for (i = 0; i < n; i++) {
		d = follow[i];
		if (dialogdepth(d, c) != depth)
			continue;
		if ((was = d->ws) && was != c->ws && was != from) {
			for (j = 0; j < nleft && left[j] != was; j++)
				;
			if (j == nleft)
				left[nleft++] = was;
		}
		/* To c's middle, or its own parent's, once they're laid out
		 * where they're shown: arrange() sees to it */
		d->recenter = 1;
		if (d->mon != c->mon) {
			setmon(d, c->mon, c->ws); /* which says it moved, and arranges */
		} else {
			d->ws = c->ws;
			ipc_event_window(d, "move");
		}
	}
	/* One that changed workspace alone shows, or goes, once it's arranged,
	 * and brings any urgency to c's workspace */
	if (n && c->mon)
		arrange(c->mon);
	if (n)
		ws_update_urgency(c->ws);
	for (i = 0; i < nleft; i++)
		if (!ws_prune(left[i]))
			ws_update_urgency(left[i]);
	free(left);
	free(follow);
}

/* Raises c to the top of its layer, then its dialogs over it, the one
 * focused last on top: in the floating layout, they share its layer */
static void
raisewithdialogs(Client *c)
{
	Client *d;
	int depth, deeper = 1, k;
	wlr_scene_node_raise_to_top(&c->scene->node);
	/* Each dialog over its parent, and among dialogs of one depth, the one
	 * focused last on top */
	for (depth = 1; deeper; depth++) {
		deeper = 0;
		wl_list_for_each_reverse(d, &fstack, flink) {
			if ((k = dialogdepth(d, c)) == depth)
				wlr_scene_node_raise_to_top(&d->scene->node);
			else if (k > depth)
				deeper = 1;
		}
	}
}

/* Maximizes c, over its monitor's window area with the bar showing, or
 * gives it its place back, telling the client either way. Another window
 * on its workspace taking focus ends it (uncoverfor). */
void
setmaximized(Client *c, int maximized)
{
	int changed = !c->ismaximized != !maximized;
	/* Its place, which it takes back if it's floating by then */
	if (maximized && changed)
		c->unmaximized = c->isfullscreen ? c->prev : c->geom;
	c->ismaximized = maximized;
	client_set_maximized(c, maximized);
	if (!changed || !c->mon || !client_surface(c) || !client_surface(c)->mapped)
		return;
	/* Placed by hand, floating or in the floating layout, where no layout
	 * places it, on its own workspace, it takes its place back, as it
	 * leaves fullscreen if it's fullscreen now. A floating one that opened
	 * maximized takes the place it would have opened in (placefloating);
	 * one floated only later has no place, and keeps the area's size. */
	if (!maximized && !laidout(c) && c->unmaximized.width) {
		if (c->isfullscreen)
			c->prev = c->unmaximized;
		else
			resize(c, c->unmaximized, 0);
	}
	arrange(c->mon);
	ipc_event_window(c, "maximize");
}

/* Minimizes c, or brings it back, as tide's SPEC §6.7 has it. A minimized
 * window keeps its workspace, monitor and states, so a workspace with one
 * stays while it waits, but it isn't shown, laid out or focused: focus on
 * it goes to the top window left on the focused monitor. Brought back, it
 * joins the end of its part's stack, as a window that arrives does.
 * Focusing it brings it back (focusclient). An app's own minimize request
 * does nothing. */
void
setminimized(Client *c, int minimized)
{
	if (!c->isminimized == !minimized)
		return;
	c->isminimized = minimized;
	if (!minimized) {
		ws_to_end(c);
		/* Fullscreen, it covers its output as it is now: one changed
		 * meanwhile resized only the fullscreen window it showed */
		if (c->isfullscreen && c->mon)
			resize(c, c->mon->m, 0);
	}
	if (c->mon)
		arrange(c->mon);
	if (minimized && c == focused_client)
		focusclient(focustop(selmon), NULL, 1);
	printstatus();
	ipc_event_window(c, "minimize");
}

/* Focus went to c: no other window on its workspace there covers it, so
 * one maximized or fullscreen is so no more, unless c is its dialog. Focus
 * a command or a notification gives a window, even one the focus guard
 * held, is never on a window you can't see. */
static void
uncoverfor(Client *c)
{
	Client *w;
	wl_list_for_each(w, &clients, link) {
		if (w == c || (!w->ismaximized && !w->isfullscreen) || w->isminimized
				|| w->mon != c->mon || w->ws != c->ws || descendsfrom(c, w))
			continue;
		if (w->isfullscreen)
			setfullscreen(w, 0);
		if (w->ismaximized)
			setmaximized(w, 0);
	}
}

void
monocle(Monitor *m)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, m->w, 0);
		n++;
	}
	if (n)
		snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	int nx, ny;
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL, *focusedl = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint;
	SplitEdge edge;

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, &l, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &focusedl) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (focusedl ? focusedl->scene->node.x : w->geom.x);
		sy = cursor->y - (focusedl ? focusedl->scene->node.y : w->geom.y);
		/* An X11 window's, from inside its border, in its own pixels (x11.h) */
		if (w && w->type == X11) {
			sx = (sx - w->bw) * w->xscale;
			sy = (sy - w->bw) * w->xscale;
		}
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				/* In the window's own pixels, an X11 one's its own (x11.h) */
				double scale = c->type == X11 ? c->xscale : 1;
				sx = (cursor->x - c->geom.x - c->bw) * scale;
				sy = (cursor->y - c->geom.y - c->bw) * scale;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx * scale, sy + dy * scale, &sx_confined, &sy_confined)) {
					dx = (sx_confined - sx) / scale;
					dy = (sy_confined - sy) / scale;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		wlr_cursor_move(cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
		ipc_output_focus();
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		nx = (int)round(cursor->x) - grabcx;
		ny = (int)round(cursor->y) - grabcy;
		if (abs(selmon->w.x - nx) < (int)snap)
			nx = selmon->w.x;
		else if (abs((selmon->w.x + selmon->w.width) - (nx + grabc->geom.width)) < (int)snap)
			nx = selmon->w.x + selmon->w.width - grabc->geom.width;
		if (abs(selmon->w.y - ny) < (int)snap)
			ny = selmon->w.y;
		else if (abs((selmon->w.y + selmon->w.height) - (ny + grabc->geom.height)) < (int)snap)
			ny = selmon->w.y + selmon->w.height - grabc->geom.height;
		resize(grabc, (struct wlr_box){.x = nx, .y = ny,
			.width = grabc->geom.width, .height = grabc->geom.height}, 1);
		return;
	} else if (cursor_mode == CurResize) {
		resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
			.width = (int)round(cursor->x) - grabc->geom.x, .height = (int)round(cursor->y) - grabc->geom.y}, 1);
		return;
	} else if (cursor_mode == CurEdge) {
		split_follow(cursor->x);
		return;
	}

	/* Over the master/stack edge, the pointer is the edge's, not a
	 * window's, and shows that it resizes */
	if (cursor_mode == CurNormal && split_edge_at(cursor->x, cursor->y, &edge)) {
		wlr_seat_pointer_clear_focus(seat);
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "ew-resize");
		return;
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	pointerfocus(c, l, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
		return;
	/* A tile's resize drags its master/stack edge, if it has one */
	if (arg->ui == CurResize && split_start_tile(grabc, cursor->x)) {
		grabc = NULL;
		return;
	}

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	switch (cursor_mode = arg->ui) {
	case CurMove:
		grabcx = (int)round(cursor->x) - grabc->geom.x;
		grabcy = (int)round(cursor->y) - grabc->geom.y;
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
		break;
	case CurResize:
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(cursor, NULL,
				grabc->geom.x + grabc->geom.width,
				grabc->geom.y + grabc->geom.height);
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
		break;
	}
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	size_t states_len, i;
	struct wlr_output_swapchain_manager swapchain_manager;
	struct wlr_backend_output_state *states =
		wlr_output_configuration_v1_build_state(config, &states_len);
	int ok = 0;

	if (!states) {
		wlr_output_configuration_v1_send_failed(config);
		return;
	}

	wlr_output_swapchain_manager_init(&swapchain_manager, backend);

	ok = wlr_output_swapchain_manager_prepare(&swapchain_manager, states, states_len);
	if (!ok || test)
		goto out;

	for (i = 0; i < states_len; i++) {
		struct wlr_swapchain *swapchain = wlr_output_swapchain_manager_get_swapchain(
				&swapchain_manager, states[i].output);
		if (swapchain && !states[i].output->enabled)
			wlr_output_state_set_buffer(&states[i].base, wlr_swapchain_acquire(swapchain));
	}

	/* Which outputs the request moves, from where they are now: the
	 * commit, and each move after it, places the others beside them
	 * again, which isn't the client moving those */
	wl_list_for_each(config_head, &config->heads, link) {
		Monitor *m = config_head->state.output->data;
		m->head_moved = config_head->state.enabled
				&& (m->m.x != config_head->state.x || m->m.y != config_head->state.y
					|| !wlr_output_layout_get(output_layout, config_head->state.output));
	}

	if (!(ok = wlr_backend_commit(backend, states, states_len)))
		goto out;

	wlr_output_swapchain_manager_apply(&swapchain_manager);

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;

		/* Ensure displays previously disabled by wlr-output-power-management-v1
		 * are properly handled*/
		m->asleep = 0;
		/* On, it's the client's; left off, it's still its line's */
		if (wlr_output->enabled)
			m->file_off = 0;

		/* Don't move monitors if position wouldn't change. This avoids
		 * wlroots marking the output as manually configured, and keeps
		 * it beside the others if that's where it was placed. One moved
		 * stays where it was put, as a position its line gives would.
		 * wlr_output_layout_add does not like disabled outputs */
		if (m->head_moved) {
			m->want_place = PlaceAt;
			m->want_x = config_head->state.x;
			m->want_y = config_head->state.y;
			wlr_output_layout_add(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);
		}
	}

out:
	wlr_output_swapchain_manager_finish(&swapchain_manager);
	for (i = 0; i < states_len; i++)
		wlr_output_state_finish(&states[i].base);

	free(states);

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* https://codeberg.org/dwl/dwl/issues/577 */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
pointerfocus(Client *c, LayerSurface *l, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;

	/* While a focus grab holds, the pointer enters only its surfaces */
	if (!grab_accepts(surface)) {
		c = NULL;
		l = NULL;
		surface = NULL;
	}

	/* An on-demand layer, such as a notification's reply field, takes the
	 * keyboard only when it's clicked, as tide's SPEC §14.1 has it */
	if (surface != seat->pointer_state.focused_surface && sloppyfocus && time) {
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c))) {
			focusclient(c, NULL, 0);
		} else if (l && l->layer_surface->current.keyboard_interactive
				== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
			focusclient(NULL, l, 0);
		}
	}

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
printstatus(void)
{
	Monitor *m = NULL;
	Client *c;

	wl_list_for_each(m, &mons, link) {
		if ((c = focustop(m))) {
			printf("%s title %s\n", m->wlr_output->name, client_get_title(c));
			printf("%s appid %s\n", m->wlr_output->name, client_get_appid(c));
			printf("%s fullscreen %d\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %d\n", m->wlr_output->name, c->isfloating);
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == selmon);
		printf("%s workspace %s\n", m->wlr_output->name, curws ? curws->name : "");
		printf("%s layout %s\n", m->wlr_output->name, m->ltsymbol);
	}
	fflush(stdout);
	ipc_output_focus();
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state = {0};
	Monitor *m = event->output->data;

	if (!m)
		return;

	m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the output */
	wlr_output_state_set_enabled(&state, event->mode);
	if (event->mode && m->want_has_adaptive_sync)
		settings_adaptive_sync(m, m->want_adaptive_sync, &state);
	if (wlr_output_commit_state(m->wlr_output, &state) && event->mode)
		m->want_has_adaptive_sync = 0;
	wlr_output_state_finish(&state);

	m->asleep = !event->mode;
	updatemons(NULL, NULL);
}

void
quit(const Arg *arg)
{
	wl_display_terminate(dpy);
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	Monitor *m = wl_container_of(listener, m, frame);
	struct timespec now;

	if (!wlr_scene_output_needs_frame(m->scene_output)) {
		return;
	}

	wlr_scene_output_commit(m->scene_output, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	if (c->surface.xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;
	struct wlr_box clip, was = c->geom;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);
	/* A move or a resize is its app's place; a redraw where it is, as
	 * every commit makes, isn't another window's to take */
	if (c->geom.x != was.x || c->geom.y != was.y || c->geom.width != was.width
			|| c->geom.height != was.height)
		fl_remember(c);

	/* Update scene-graph, including borders */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);
	if (c->dim)
		wlr_scene_buffer_set_dest_size(c->dim, c->geom.width, c->geom.height);

	client_set_size(c, c->geom.width - 2 * c->bw, c->geom.height - 2 * c->bw);
	client_get_clip(c, &clip);
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
#ifdef XWAYLAND
	/* The clip sizes it again */
	x11_scale_scene(c);
#endif
}

void
run(char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);
	ipc_init();

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");
	settings_outputs_ready();

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing dwl to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* tidewm: the pointer starts in the middle of the leftmost enabled
	 * output (the top one of those level with it), where focus starts, not
	 * by its top left corner. mons is newest first, and keeps outputs a
	 * config disables, so its order says nothing here. */
	{
		Monitor *m, *start = NULL;
		wl_list_for_each(m, &mons, link) {
			if (!m->wlr_output->enabled)
				continue;
			if (!start || m->m.x < start->m.x
					|| (m->m.x == start->m.x && m->m.y < start->m.y))
				start = m;
		}
		if (start)
			wlr_cursor_warp_closest(cursor, NULL, start->m.x + start->m.width / 2.0,
					start->m.y + start->m.height / 2.0);
		else
			wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	}
	selmon = xytomon(cursor->x, cursor->y);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}

/* Runs the binding for these keys: the config file's, once one is loaded,
 * or config.h's. Returns whether there was one. */
int
runbinding(uint32_t mods, xkb_keysym_t sym)
{
	const Key *key;
	if (settings_keys())
		return settings_run_binding(mods, sym, 0);
	/* config.h's keys are all off on the lock screen */
	if (locked || !(key = keybinding(mods, sym)))
		return 0;
	focus_asked++;
	key->func(&key->arg);
	focus_asked--;
	return 1;
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr,
				wlr_cursor_shape_v1_name(event->shape));
}

/* Whether c is a browser's picture-in-picture window, by its title, as
 * tide's Hyprland rule has it: ^Picture[- ]in[- ][Pp]icture$ */
int
client_is_pip(Client *c)
{
	const char *t = fg_raw_title(c);
	return t && strlen(t) == 18 && !strncmp(t, "Picture", 7)
			&& (t[7] == '-' || t[7] == ' ') && !strncmp(t + 8, "in", 2)
			&& (t[10] == '-' || t[10] == ' ') && (t[11] == 'P' || t[11] == 'p')
			&& !strcmp(t + 12, "icture");
}

/* A window that floats as it opens, a dialog or one a rule floats, goes
 * in the middle of its parent, or of its monitor's window area without
 * one, as big as it asked, border and all, but no more than 80% of its
 * monitor, and kept in that area (tide's SPEC §6.4). Picture-in-picture
 * is a quarter of the monitor each way, in the window area's bottom right
 * corner, 16 px in. */
void
placefloating(Client *c, struct wlr_box asked)
{
	Client *p = client_get_parent(c);
	struct wlr_box area = c->mon->w, box = asked;
	struct wlr_box around = p && p->mon == c->mon ? p->geom : area;
	int pip = client_is_pip(c);
	if (pip) {
		box.width = c->mon->m.width / 4;
		box.height = c->mon->m.height / 4;
	}
	box.width = MIN(box.width, c->mon->m.width * 4 / 5);
	box.height = MIN(box.height, c->mon->m.height * 4 / 5);
	/* Its minimum wins over the cap, so it's centered at that size */
	fitsizehints(c, &box);
	box.x = pip ? area.x + area.width - box.width - 16
			: around.x + (around.width - box.width) / 2;
	box.y = pip ? area.y + area.height - box.height - 16
			: around.y + (around.height - box.height) / 2;
	box.x = MAX(area.x, MIN(box.x, area.x + area.width - box.width));
	box.y = MAX(area.y, MIN(box.y, area.y + area.height - box.height));
	/* One that opened maximized takes this place once it isn't */
	if (c->ismaximized)
		c->unmaximized = box;
	else
		resize(c, box, 0);
}

/* Gives box the size applybounds() will give c, border and all: at least
 * its minimum, and at most its maximum, so it's placed at that size */
void
fitsizehints(Client *c, struct wlr_box *box)
{
	struct wlr_box min = {0}, max = {0};
	int bw = 2 * (int)c->bw;
	client_get_size_hints(c, &max, &min);
	box->width = MAX(box->width, min.width + bw);
	box->height = MAX(box->height, min.height + bw);
	if (max.width > 0 && bw <= INT_MAX - max.width)
		box->width = MIN(box->width, max.width + bw);
	if (max.height > 0 && bw <= INT_MAX - max.height)
		box->height = MIN(box->height, max.height + bw);
}

void
setfloating(Client *c, int floating)
{
	Client *p = client_get_parent(c);
	int changed = c->isfloating != floating;
	c->isfloating = floating;
	if (changed && c->mon && client_surface(c)->mapped)
		ipc_event_window(c, "floating");
	/* Tiled again, it joins the end of the stack, as a new window does */
	if (changed && !floating)
		ws_to_end(c);
	/* If in floating layout do not change the client's layer */
	if (!c->mon || !client_surface(c)->mapped || !c->mon->lt[c->mon->sellt]->arrange)
		return;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
			(p && p->isfullscreen) ? LyrFS
			: c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
	printstatus();
}

/* Whether c is fullscreen, or asked to be and will be once it has focus */
int
fullscreenwanted(Client *c)
{
	return c->isfullscreen || c->wantsfullscreen;
}

void
setfullscreen(Client *c, int fullscreen)
{
	int changed;
	/* One the focus guard holds goes fullscreen once it has focus
	 * (focusclient), rather than cover the window you're in. Before it
	 * maps, nothing holds it yet. */
	c->wantsfullscreen = fullscreen && c->held && c->mon && client_surface(c)->mapped;
	if (c->wantsfullscreen) {
		c->isfullscreen = 0;
		client_set_fullscreen(c, 0); /* the request's answer, for now */
		return;
	}
	changed = c->isfullscreen != fullscreen;
	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else if (changed) {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated.
		 * One that wasn't fullscreen, as one whose request a cancel took
		 * back while the focus guard held it, stays where it is. */
		resize(c, c->prev, 0);
	}
	arrange(c->mon);
	printstatus();
	if (changed)
		ipc_event_window(c, "fullscreen_mode");
}

void
setlayout(const Arg *arg)
{
	const Layout *old;
	if (!selmon)
		return;
	old = selmon->lt[selmon->sellt];
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = (Layout *)arg->v;
	/* Each layout keeps its own mfact and nmaster */
	lt_switch(selmon, old, selmon->lt[selmon->sellt]);
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, LENGTH(selmon->ltsymbol));
	arrange(selmon);
	printstatus();
	lt_event(selmon);
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0f ? arg->f + selmon->mfact : arg->f - 1.0f;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
setmon(Client *c, Monitor *m, Workspace *ws)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		arrange(oldmon);
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		/* Its app's place is now relative to this monitor, though it
		 * may not have moved */
		fl_remember(c);
		c->ws = ws ? ws : curws; /* the current workspace, unless told */
		/* This will call arrange(c->mon), and keeps a request waiting */
		setfullscreen(c, fullscreenwanted(c));
		setfloating(c, c->isfloating);
		/* An X11 window can be urgent before it maps */
		ws_update_urgency(c->ws);
	}
	/* Not for a window's first placement, nor its unmapping; moved, it
	 * joins the end of the stack there, as a new window does */
	if (oldmon && m) {
		ws_to_end(c);
		arrange(m);
		ipc_event_window(c, "move");
	}
	/* The window you were in closing, the one under the pointer (focus.h) */
	focusclient(!m && c == fg_closing ? fg_after_close() : focustop(selmon), NULL, 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	wlr_log_init(log_level, NULL);

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	dpy = wl_display_create();
	event_loop = wl_display_get_event_loop(dpy);

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(backend = wlr_backend_autocreate(event_loop, &session)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, rootcolor);
	for (i = 0; i < NUM_LAYERS; i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	drag_icon = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't create renderer");
	wl_signal_add(&drw->events.lost, &gpu_reset);

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is to call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create the linux_dmabuf interface manually to integrate it
	 * with wlr_scene. */
	wlr_renderer_init_wl_shm(drw, dpy);

	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene,
				wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}

	if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
			&& backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, 6, drw);
	wlr_subcompositor_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	capture_init(wlr_ext_image_copy_capture_manager_v1_create(dpy, 1));
	grab_init();
	wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);
	wlr_data_control_manager_v1_create(dpy);
	wlr_ext_data_control_manager_v1_create(dpy, 1);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	dim_init();
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_alpha_modifier_v1_create(dpy);

	/* Modal dialogs, which float (client_is_float_type) */
	wlr_xdg_wm_dialog_v1_create(dpy, 1);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

	power_mgr = wlr_output_power_manager_v1_create(dpy);
	wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

	/* Creates an output layout, which is a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);

	x11_wlr_xdg_outputs = wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&mons);
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	ws_protocol_init();
	curws = ws_get("1");
	ws_set_active(curws, 1);
	wl_list_init(&fstack);

	xdg_shell = wlr_xdg_shell_create(dpy, 6);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	layer_shell = wlr_layer_shell_v1_create(dpy, 4);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
	locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
			(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(dpy,1);
	ext_foreign_toplevel_image_capture_source_manager_v1 =
            wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(dpy, 1);
	wl_signal_add(&ext_foreign_toplevel_image_capture_source_manager_v1->events.new_request,
			&new_foreign_toplevel_capture_request);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	/* tidewm: the theme and size XCURSOR_THEME and XCURSOR_SIZE name, as
	 * clients read them, so the pointer looks the same over both; without
	 * them, the default theme at 24 */
	{
		const char *size_env = getenv("XCURSOR_SIZE");
		long size = size_env ? strtol(size_env, NULL, 10) : 0;
		if (size <= 0 || size > 512)
			size = 24;
		cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), (uint32_t)size);
		if (!size_env || strtol(size_env, NULL, 10) != size) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%ld", size);
			setenv("XCURSOR_SIZE", buf, 1);
		}
	}

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);
	gesture_init();

	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_signal_add(&backend->events.new_input, &new_input_device);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	kb_group = createkeyboardgroup();
	wl_list_init(&kb_group->destroy.link);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	input_method_manager = wlr_input_method_manager_v2_create(dpy);
	text_input_manager = wlr_text_input_manager_v3_create(dpy);
	input_method_relay = input_method_relay_create();

	/* Make sure XWayland clients don't connect to the parent X server,
	 * e.g when running in the x11 backend or the wayland backend and the
	 * compositor has Xwayland support */
	unsetenv("DISPLAY");
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		fprintf(stderr, "failed to setup XWayland X server, continuing without it\n");
	}
	x11_init();
#endif
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("dwl: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
tagmon(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		movemon(sel, dirtomon(arg->i), sel->ws);
}

void
tile(Monitor *m)
{
	unsigned int mw, my, ty;
	int i, n = 0;
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? (int)roundf(m->w.width * m->mfact) : 0;
	else
		mw = m->w.width;
	i = my = ty = 0;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (i < m->nmaster) {
			resize(c, (struct wlr_box){.x = m->w.x, .y = m->w.y + my, .width = mw,
				.height = (m->w.height - my) / (MIN(n, m->nmaster) - i)}, 0);
			my += c->geom.height;
		} else {
			resize(c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
				.width = m->w.width - mw, .height = (m->w.height - ty) / (n - i)}, 0);
			ty += c->geom.height;
		}
		i++;
	}
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
		fl_by_hand(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

/* Drops a lock whose locker has gone, as after it crashed: the way back
 * in, from a TTY, that keeps the session. A lock its locker still holds
 * only the locker may end. */
int
unlockdead(void)
{
	if (!locked || cur_lock)
		return 0;
	locked = 0;
	wlr_scene_node_set_enabled(&locked_bg->node, 0);
	focusclient(focustop(selmon), NULL, 0);
	motionnotify(0, NULL, 0, 0, 0, 0);
	return 1;
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	l->mapped = 0;
	wlr_scene_node_set_enabled(&l->scene->node, 0);
	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);
	if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
		focusclient(focustop(selmon), NULL, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	Workspace *ws;
	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}
	ipc_event_window(c, "close");
	if (c == focused_client) {
		focused_client = NULL;
		fg_closing = c; /* for setmon(), below */
	}
	/* Mapping again, it's maximized only if it asks again, and shown,
	 * and kept open only if a shell says so again: it's a new window */
	c->ismaximized = 0;
	c->isminimized = 0;
	c->keptopen_ws = 0;
	c->wantsfullscreen = 0;
	c->unmaximized = (struct wlr_box){0};

	if (!client_is_unmanaged(c)) {
		wl_list_remove(&c->link);
		setmon(c, NULL, NULL);
		wl_list_remove(&c->flink);
		/* The workspace may go, and the window gets a new one if it maps again */
		ws = c->ws;
		c->ws = NULL;
		if (!ws_prune(ws))
			ws_update_urgency(ws);
	} else if (client_wants_focus(c)) {
		focusclient(focustop(selmon), NULL, 1);
	}
	fg_closing = NULL;

	if (c->foreign_toplevel_handle) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(c->foreign_toplevel_handle);
		c->foreign_toplevel_handle = NULL;
	}
#ifdef XWAYLAND
	if (c->type != XDGShell && c->capture.image_capture_scene_surface) {
		wlr_scene_node_destroy(&c->capture.image_capture_scene_surface->buffer->node);
		c->capture.image_capture_scene_surface = NULL;
	}
	if (client_is_x11(c))
		wl_list_remove(&c->x11_commit.link);
#endif

	wlr_scene_node_destroy(&c->image_capture_scene->tree.node);
	c->image_capture_scene = NULL;
	c->image_capture_source = NULL;
	wlr_scene_node_destroy(&c->scene->node);
	c->dim = NULL; /* it went with the tree */
	client_surface(c)->data = NULL;
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config;
	Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;

	/* Outputs being placed beside each other: the call that's placing
	 * them carries on once they're placed */
	if (settings_placing)
		return;
	config = wlr_output_configuration_v1_create();

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		if (m->asleep)
			continue;

		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to, and place those that go beside the
	 * others */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(output_layout, m->wlr_output))
			settings_output_place(m);
	}
	settings_place_outputs();

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

	/* Make sure the clients are hidden when dwl is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		/* The current workspace's part here, kept or from the defaults */
		lt_settle(m);
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);
		/* make sure fullscreen clients have the right size */
		if ((c = focustop(m)) && c->isfullscreen)
			resize(c, m->m, 0);

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon) {
			selmon = m;
		}
	}

	if (selmon && selmon->wlr_output->enabled) {
		/* Nor is focus moving as outputs change, for the focus guard */
		fg_focusing++;
		wl_list_for_each(c, &clients, link) {
			if (!c->mon && client_surface(c)->mapped)
				setmon(c, selmon, c->ws);
		}
		focusclient(focustop(selmon), NULL, 1);
		fg_focusing--;
		if (selmon->lock_surface) {
			client_notify_enter(selmon->lock_surface->surface,
					wlr_seat_get_keyboard(seat));
			client_activate_surface(selmon->lock_surface->surface, 1);
		}
	}
	/* The layout of each monitor that took its part, now its windows are placed */
	lt_flush();
#ifdef XWAYLAND
	/* Xwayland's outputs, and X11 windows in their pixels */
	x11_update();
#endif

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what it's
	 * at the wrong position after all. */
	wlr_cursor_move(cursor, NULL, 0, 0);

	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();
	/* An X11 window can set a title before it has a surface */
	if (client_surface(c) && client_surface(c)->mapped)
		ipc_event_window(c, "title");

	if (c->foreign_toplevel_handle) {
		struct wlr_ext_foreign_toplevel_handle_v1_state foreign_toplevel_state = {
			.app_id = client_get_appid(c),
			.title = client_get_title(c),
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(c->foreign_toplevel_handle,
				&foreign_toplevel_state);
	}
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (c)
		requestactivate(c);
}

/* A window asked to be activated, over xdg-activation or X11's
 * _NET_ACTIVE_WINDOW: a grant for its app lets it take focus, and
 * otherwise it's marked urgent */
void
requestactivate(Client *c)
{
	/* A grant is used even by the window you're in, so it's one-shot */
	if (fg_activate(c) || c == focustop(selmon))
		return;
	seturgent(c, 1);
}

/* Marks c urgent, making it the most recent mark even if it was already,
 * or takes its mark away: its flag and border, its urgent event, and its
 * workspace's urgency. A window not mapped yet, or with no surface yet,
 * gets its border and event as it maps; an unmanaged one, a menu, has no
 * border to show it. */
void
seturgent(Client *c, int urgent)
{
	struct wlr_surface *surface = client_surface(c);
	if (!urgent && !c->isurgent)
		return;
	c->isurgent = urgent;
	if (urgent)
		c->urgent_seq = ++urgent_count;
	printstatus();
	if (!surface || !surface->mapped)
		return;
	if (c->border[0])
		client_set_border_color(c, urgent ? urgentcolor
				: c == focused_client ? focuscolor : bordercolor);
	ipc_event_window(c, "urgent");
	ws_update_urgency(c->ws);
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	group->virtual_keyboard = &kb->keyboard;
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
	wlr_seat_set_capabilities(seat, seat->capabilities | WL_SEAT_CAPABILITY_KEYBOARD);
	settings_virtual_added(&kb->keyboard.base);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
	wlr_seat_set_capabilities(seat, seat->capabilities | WL_SEAT_CAPABILITY_POINTER);
	settings_virtual_added(device);
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node;
	struct wlr_surface *surface = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && layer >= (locked ? LyrBlock : 0); layer--) {
		if (layer == LyrIMPopup ||
				!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER)
			surface = wlr_scene_surface_try_from_buffer(
					wlr_scene_buffer_from_node(node))->surface;
	}

	if (psurface) *psurface = surface;
	toplevel_from_wlr_surface(surface, pc, pl);
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = focustop(selmon);

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way; never a status popup, which only
	 * the pointer focuses */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon) && !c->isfloating && c->held != FgPopup) {
			if (c != sel)
				break;
			sel = NULL;
		}
	}

	/* Return if no other tiled window was found */
	if (&c->link == &clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, NULL, 1);
	arrange(selmon);
}

#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);

	/* Only "managed" windows can be activated, as a Wayland one is */
	if (!client_is_unmanaged(c))
		requestactivate(c);
}

void
associatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	int x, y;
	float scale;
	if (!client_surface(c) || !client_surface(c)->mapped
			|| client_is_unmanaged(c)) {
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if ((c->isfloating && c != grabc) || !c->mon->lt[c->mon->sellt]->arrange) {
		/* From X11's space, and in the pixels of the monitor that
		 * configures it, its own, wherever it asks to be */
		x11_to_layout(event->x, event->y, &x, &y);
		scale = x11_scale(c->mon);
		resize(c, (struct wlr_box){.x = x - (int)c->bw, .y = y - (int)c->bw,
				.width = (int)lroundf((float)event->width / scale) + (int)c->bw * 2,
				.height = (int)lroundf((float)event->height / scale) + (int)c->bw * 2}, 0);
	} else {
		arrange(c->mon);
	}
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	Client *c;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	c->xscale = 1;
	c->bw = client_is_unmanaged(c) ? 0 : borderpx;

	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xsurface->events.set_geometry, &c->set_geometry, setgeometryx11);
	LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

void
setgeometryx11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_geometry);

	if (!client_is_unmanaged(c) || !client_surface(c)
			|| !client_surface(c)->mapped)
		return;

	client_get_geometry(c, &c->geom);
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	x11_scale_scene(c);
}

void
sethints(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hints);
	int urgent;
	if (!c->surface.xwayland->hints)
		return;

	/* A mark when the hint comes on, even on a window marked another way,
	 * but not each time the hints change; it goes as the hint does. The
	 * focused window isn't marked, but its hint is still followed. */
	urgent = !!xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
	if (urgent != c->hint_urgent && c != focustop(selmon))
		seturgent(c, urgent);
	c->hint_urgent = urgent;
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of dwl. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland, wlr_xcursor_image_get_buffer(xcursor->images[0]),
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}
#endif

int
main(int argc, char *argv[])
{
	char *startup_cmd = NULL, *config = NULL;
	int c;

	while ((c = getopt(argc, argv, "c:s:hdv")) != -1) {
		if (c == 'c')
			config = optarg;
		else if (c == 's')
			startup_cmd = optarg;
		else if (c == 'd')
			log_level = WLR_DEBUG;
		else if (c == 'v')
			die("dwl " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	settings_init(config);
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-c config file] [-s startup command]", argv[0]);
}

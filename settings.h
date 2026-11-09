/*
 * See LICENSE and NOTICE for copyright and license details.
 *
 * tidewm's runtime config: a file of lines in sway's syntax, read at
 * startup and again by the reload command. A binding's action is a list
 * of IPC commands, which ipc.h runs. Once a file is loaded, its bindings
 * replace config.h's keys; with no file, config.h's keys stay. A file
 * with any error is rejected whole, and what was there before stays.
 * docs/config.md says what a file can hold. dwl.c includes this file,
 * after ipc.h.
 */
#include <glob.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#define SETTINGS_MAX_SIZE (1u << 20)
/* How deep include lines may nest: deeper is a file that includes itself */
#define SETTINGS_MAX_DEPTH 10

typedef struct {
	uint32_t mods;
	xkb_keysym_t sym;
	int release; /* runs when the key comes up, if it was tapped */
	int locked; /* runs while the screen is locked, too */
	int kept_open; /* counts only while the keyboard's in a window kept open */
	uint32_t button; /* a --whole-window binding's button, or scroll; 0 for a key */
	char *keys; /* as written, for messages */
	char *command;
	size_t named; /* 1 + its NamedBinding's index in Settings, or 0 */
} Binding;

/* A bindsym line's flags, which a named binding's every key shares */
typedef struct {
	int release; /* runs when the key comes up, if it was tapped */
	int locked; /* runs while the screen is locked, too */
	int kept_open; /* counts only while the keyboard's in a window kept open */
	int whole_window; /* binds a button, or scroll, rather than a key */
} BindingFlags;

/* A named binding, bindsym --name's: one action, whose keys are its
 * lines', and a rebind line can give it others, for a shell's keyboard
 * shortcuts (GET_NAMED_BINDINGS) */
typedef struct {
	char *name;
	size_t mode; /* in Settings' modes */
	char *command;
	BindingFlags flags;
	char **defaults; /* its bindsym lines' keys, as written */
	size_t ndefaults;
} NamedBinding;

/* Scrolls, as sway's button4 to button7 name them, beside the buttons'
 * own codes */
enum { SettingsScrollUp = KEY_MAX + 1, SettingsScrollDown, SettingsScrollLeft,
	SettingsScrollRight };

/* The buttons a --whole-window binding can name, as sway names them: X11's
 * numbers, then evdev's names; a code's first name is the binding event's
 * symbol */
static const struct {
	const char *name;
	uint32_t code;
} settings_buttons[] = {
	{"button1", BTN_LEFT}, {"button2", BTN_MIDDLE}, {"button3", BTN_RIGHT},
	{"button4", SettingsScrollUp}, {"button5", SettingsScrollDown},
	{"button6", SettingsScrollLeft}, {"button7", SettingsScrollRight},
	{"button8", BTN_SIDE}, {"button9", BTN_EXTRA},
	{"BTN_LEFT", BTN_LEFT}, {"BTN_MIDDLE", BTN_MIDDLE}, {"BTN_RIGHT", BTN_RIGHT},
	{"BTN_SIDE", BTN_SIDE}, {"BTN_EXTRA", BTN_EXTRA}, {"BTN_FORWARD", BTN_FORWARD},
	{"BTN_BACK", BTN_BACK}, {"BTN_TASK", BTN_TASK},
};

/* A bindswitch line: a command run when a switch, such as the lid, turns
 * on, off, or either */
enum { SwOff, SwOn, SwToggle };
typedef struct {
	int type; /* WLR_SWITCH_TYPE_LID or WLR_SWITCH_TYPE_TABLET_MODE */
	int state; /* SwOff, SwOn or SwToggle */
	int locked; /* runs while the screen is locked, too */
	char *command;
} SwitchBinding;

/* A submap: bindings in force while it's the current mode */
typedef struct {
	char *name;
	Binding *bindings;
	size_t nbindings;
} Mode;

/* What a for_window rule's criteria can match, as sway names them */
enum { CritAppId, CritClass, CritInstance, CritTitle, CritShell, CritLast };
static const char *const settings_crit_names[CritLast] = {
	"app_id", "class", "instance", "title", "shell",
};

/* A for_window rule: commands run on each new window its criteria match,
 * each criterion a PCRE2 pattern, as sway's are */
typedef struct {
	pcre2_code *re[CritLast];
	int has[CritLast];
	char *command;
} WindowRule;

/* Where an output goes in the layout: at x, y, or beside the others, as
 * Hyprland's monitor rules' auto, auto-left, auto-up and auto-down put it
 * (settings_place_outputs()). Without a position it goes to the right. */
enum { PlaceRight, PlaceLeft, PlaceUp, PlaceDown, PlaceAt };

/* An output line: what to set on the outputs it names, "*" for all */
typedef struct {
	char *name;
	int has_mode, width, height, refresh; /* refresh in mHz; 0 for the best */
	int custom; /* mode --custom: that mode, even if the output lists none like it */
	int has_scale;
	float scale;
	int has_position, place, x, y;
	int has_transform;
	enum wl_output_transform transform;
	int enabled; /* 1 or 0, or -1 to leave it */
	int has_adaptive_sync, adaptive_sync;
	int power; /* the command's power or dpms: 1 or 0, or -1 to leave it */
} OutputConfig;

/* What an input line can set, as sway names it: XKB's names, then the
 * rest, which take a word or a number */
enum { InXkbRules, InXkbModel, InXkbLayout, InXkbVariant, InXkbOptions, InXkbLast };
static const char *const settings_xkb_names[InXkbLast] = {
	"xkb_rules", "xkb_model", "xkb_layout", "xkb_variant", "xkb_options",
};
enum { InTap, InTapButtonMap, InDrag, InDragLock, InNaturalScroll, InDwt, InLeftHanded,
	InMiddleEmulation, InScrollMethod, InClickMethod, InAccelProfile, InEvents,
	InRepeatRate, InRepeatDelay, InLast };
/* Each setting's words and their libinput values; a number has none */
static const struct {
	const char *name;
	const char *words[4];
	int values[4];
} settings_input_settings[InLast] = {
	[InTap] = {"tap", {"enabled", "disabled"}, {1, 0}},
	[InTapButtonMap] = {"tap_button_map", {"lrm", "lmr"},
		{LIBINPUT_CONFIG_TAP_MAP_LRM, LIBINPUT_CONFIG_TAP_MAP_LMR}},
	[InDrag] = {"drag", {"enabled", "disabled"}, {1, 0}},
	[InDragLock] = {"drag_lock", {"enabled", "disabled"}, {1, 0}},
	[InNaturalScroll] = {"natural_scroll", {"enabled", "disabled"}, {1, 0}},
	[InDwt] = {"dwt", {"enabled", "disabled"}, {1, 0}},
	[InLeftHanded] = {"left_handed", {"enabled", "disabled"}, {1, 0}},
	[InMiddleEmulation] = {"middle_emulation", {"enabled", "disabled"}, {1, 0}},
	[InScrollMethod] = {"scroll_method", {"none", "two_finger", "edge", "on_button_down"},
		{LIBINPUT_CONFIG_SCROLL_NO_SCROLL, LIBINPUT_CONFIG_SCROLL_2FG,
			LIBINPUT_CONFIG_SCROLL_EDGE, LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN}},
	[InClickMethod] = {"click_method", {"none", "button_areas", "clickfinger"},
		{LIBINPUT_CONFIG_CLICK_METHOD_NONE, LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS,
			LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER}},
	[InAccelProfile] = {"accel_profile", {"adaptive", "flat"},
		{LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT}},
	[InEvents] = {"events", {"enabled", "disabled", "disabled_on_external_mouse"},
		{LIBINPUT_CONFIG_SEND_EVENTS_ENABLED, LIBINPUT_CONFIG_SEND_EVENTS_DISABLED,
			LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE}},
	[InRepeatRate] = {"repeat_rate", {NULL}, {0}},
	[InRepeatDelay] = {"repeat_delay", {NULL}, {0}},
};

/* An input line, or block: what to set on the devices it names, "*" for
 * all, "type:keyboard", "type:pointer" or "type:touchpad" for a kind, or
 * a device's identifier */
typedef struct {
	char *name;
	const char *xkb[InXkbLast]; /* NULL: not given */
	int value[InLast]; /* -1: not given */
	int has_accel;
	double accel; /* pointer_accel, -1 to 1 */
	int has_scroll_factor;
	double scroll_factor; /* 0 or more */
	unsigned int xkb_line; /* the line that last gave an XKB name */
	const char *xkb_file; /* the file that line is in; NULL for the config file */
} InputConfig;

/* Where a new tiled window goes among its monitor's, as a new_window line
 * says, as tide's Layouts page has it: the end of the stack, its top just
 * after the masters, next after the focused window, or as the master */
enum { NewEnd, NewTop, NewNext, NewMaster };

/* The layouts a layout_defaults line can name, tide's Layouts page's
 * modes, with the fewest masters each takes */
static const struct {
	const char *name;
	void (*arrange)(Monitor *);
	int min_masters;
} settings_lt_modes[] = {
	{"tile", tile, 0}, {"threecol", threecol, 1}, {"twocol", twocol, 1},
};

/* A layout's own mfact and nmaster, as its layout_defaults lines give them */
typedef struct {
	int has_mfact;
	float mfact;
	int has_nmaster;
	int nmaster;
} LayoutDefaults;

/* layout.h's, which comes after: each layout's mfact and nmaster from the
 * defaults again */
static void lt_reset_modes(void);

/* A file an include line read, for GET_CONFIG */
typedef struct {
	char *path;
	char *text; /* NULL if it was gone by the time it was read */
} SettingsInclude;

typedef struct {
	Mode *modes; /* modes[0] is "default", the file's top level */
	size_t nmodes;
	WindowRule *rules;
	size_t nrules;
	OutputConfig *outputs;
	size_t noutputs;
	InputConfig *inputs;
	size_t ninputs;
	int focus_guard; /* focus_guard enable|disable; -1: not given */
	double dim_inactive; /* dim_inactive STRENGTH; -1: not given */
	int resize_grab; /* resize_grab PIXELS; -1: not given */
	int xwayland_scaling; /* xwayland_scaling enable|disable; -1: not given */
	int floating_mod; /* floating_modifier's modifiers, 0 for none; -1: not given */
	int floating_inverse; /* floating_modifier's inverse: left resizes, right moves */
	SingleRule *singles; /* single_window lines, which replace config.h's */
	size_t nsingles;
	double ultrawide_aspect; /* ultrawide_aspect N; 0: not given */
	void (*first_layout[2])(Monitor *); /* default_layout normal|ultrawide; NULL: not given */
	LayoutDefaults layout_defaults[LENGTH(settings_lt_modes)];
	int new_window; /* new_window end|top|next|master; -1: not given */
	void (*cycle[LENGTH(settings_lt_modes)])(Monitor *); /* layout_cycle's */
	size_t ncycle; /* 0: not given */
	SwitchBinding *switches;
	size_t nswitches;
	double grant_seconds, prompt_seconds; /* 0: not given */
	char *text; /* the file, as read, for GET_CONFIG */
	SettingsInclude *includes; /* the files its include lines read, in order */
	size_t nincludes;
	NamedBinding *named; /* bindsym --name's, in the order they're first named */
	size_t nnamed;
} Settings;

typedef struct {
	char *name; /* "$name" */
	char *value;
} SettingsVar;

static char settings_path[PATH_MAX];
/* The file being read, for its errors: an included one, or NULL for the
 * config file */
static const char *settings_file;
static Settings *settings; /* NULL: config.h's keys */
static struct json_object *settings_errors; /* the last load's, as strings */
static size_t settings_mode; /* the current mode, in settings->modes */
static int settings_apply_outputs(const OutputConfig *only);
static int settings_apply_inputs(const InputConfig *only);
static void fg_arm(void);
static void dim_update(void);
static void settings_group_register(KeyboardGroup *group);
static void settings_free_input(InputConfig *ic);
/* A release binding's key, while it's down and no other key has been */
static struct {
	int armed;
	uint32_t keycode, mods;
	xkb_keysym_t sym;
} settings_tap;

static void
settings_free(Settings *s)
{
	size_t i, j;
	if (!s)
		return;
	for (i = 0; i < s->nmodes; i++) {
		for (j = 0; j < s->modes[i].nbindings; j++) {
			free(s->modes[i].bindings[j].keys);
			free(s->modes[i].bindings[j].command);
		}
		free(s->modes[i].bindings);
		free(s->modes[i].name);
	}
	free(s->modes);
	for (i = 0; i < s->nrules; i++) {
		for (j = 0; j < CritLast; j++)
			if (s->rules[i].has[j])
				pcre2_code_free(s->rules[i].re[j]);
		free(s->rules[i].command);
	}
	free(s->rules);
	for (i = 0; i < s->noutputs; i++)
		free(s->outputs[i].name);
	free(s->outputs);
	for (i = 0; i < s->ninputs; i++)
		settings_free_input(&s->inputs[i]);
	free(s->inputs);
	free(s->singles);
	for (i = 0; i < s->nswitches; i++)
		free(s->switches[i].command);
	free(s->switches);
	for (i = 0; i < s->nincludes; i++) {
		free(s->includes[i].path);
		free(s->includes[i].text);
	}
	free(s->includes);
	for (i = 0; i < s->nnamed; i++) {
		for (j = 0; j < s->named[i].ndefaults; j++)
			free(s->named[i].defaults[j]);
		free(s->named[i].defaults);
		free(s->named[i].name);
		free(s->named[i].command);
	}
	free(s->named);
	free(s->text);
	free(s);
}

static char *
settings_strdup(const char *s, size_t n)
{
	char *copy = malloc(n + 1);
	if (!copy)
		die("out of memory reading the config");
	memcpy(copy, s, n);
	copy[n] = '\0';
	return copy;
}

static void
settings_error(struct json_object *errors, unsigned int line, const char *fmt, ...)
{
	const char *file = settings_file ? settings_file : settings_path;
	char msg[512];
	int n;
	va_list ap;
	n = line ? snprintf(msg, sizeof(msg), "%s:%u: ", file, line)
		: snprintf(msg, sizeof(msg), "%s: ", file);
	if (n < 0 || (size_t)n >= sizeof(msg))
		n = 0;
	va_start(ap, fmt);
	vsnprintf(msg + n, sizeof(msg) - (size_t)n, fmt, ap);
	va_end(ap);
	json_object_array_add(errors, json_object_new_string(msg));
}

/* line with each variable replaced by its value. At a $, the longest
 * variable name that matches wins, as in sway. */
static char *
settings_substitute(const char *line, const SettingsVar *vars, size_t nvars)
{
	size_t cap = strlen(line) + 1, len = 0, i, n, best;
	char *out = malloc(cap);
	const char *p, *value;
	if (!out)
		die("out of memory reading the config");
	for (p = line; *p;) {
		value = NULL;
		best = 0;
		if (*p == '$') {
			for (i = 0; i < nvars; i++) {
				n = strlen(vars[i].name);
				if (n > best && !strncmp(p, vars[i].name, n)) {
					best = n;
					value = vars[i].value;
				}
			}
		}
		n = value ? strlen(value) : 1;
		if (len + n + 1 > cap && !(out = realloc(out, cap = (len + n + 1) * 2)))
			die("out of memory reading the config");
		memcpy(out + len, value ? value : p, n);
		len += n;
		p += value ? best : 1;
	}
	out[len] = '\0';
	return out;
}

/* The modifiers in combo, before its last +, into *mods: returns its last
 * part, the key or button, or NULL with *bad the part that's no modifier */
static char *
settings_parse_mods(char *combo, uint32_t *mods, const char **bad)
{
	static const struct { const char *name; uint32_t mod; } names[] = {
		{"Shift", WLR_MODIFIER_SHIFT}, {"Lock", WLR_MODIFIER_CAPS},
		{"Control", WLR_MODIFIER_CTRL}, {"Ctrl", WLR_MODIFIER_CTRL},
		{"Mod1", WLR_MODIFIER_ALT}, {"Alt", WLR_MODIFIER_ALT},
		{"Mod2", WLR_MODIFIER_MOD2}, {"Mod3", WLR_MODIFIER_MOD3},
		{"Mod4", WLR_MODIFIER_LOGO}, {"Super", WLR_MODIFIER_LOGO},
		{"Logo", WLR_MODIFIER_LOGO}, {"Mod5", WLR_MODIFIER_MOD5},
	};
	char *part, *next;
	size_t i;
	*mods = 0;
	for (part = combo; (next = strchr(part, '+')); part = next + 1) {
		*next = '\0';
		for (i = 0; i < LENGTH(names) && strcasecmp(part, names[i].name); i++)
			;
		*next = '+';
		if (i == LENGTH(names)) {
			*bad = part;
			return NULL;
		}
		*mods |= names[i].mod;
	}
	/* Caps Lock is ignored when matching, so here too, or a binding
	 * naming Lock would never run, nor replace one that doesn't */
	*mods = CLEANMASK(*mods);
	return part;
}

/* A button's code, by its name, or 0 */
static uint32_t
settings_button(const char *name)
{
	size_t i;
	for (i = 0; i < LENGTH(settings_buttons); i++)
		if (strncmp(settings_buttons[i].name, "button", 6)
				? !strcmp(name, settings_buttons[i].name)
				: !strcasecmp(name, settings_buttons[i].name))
			return settings_buttons[i].code;
	return 0;
}

/* Reads "Mod4+Shift+Return" into mods and a keysym. On failure, *bad is
 * the part that's wrong. */
static int
settings_parse_keys(char *combo, uint32_t *mods, xkb_keysym_t *sym, const char **bad)
{
	char *part = settings_parse_mods(combo, mods, bad);
	if (!part)
		return 0;
	*sym = xkb_keysym_from_name(part, XKB_KEYSYM_NO_FLAGS);
	if (*sym == XKB_KEY_NoSymbol)
		*sym = xkb_keysym_from_name(part, XKB_KEYSYM_CASE_INSENSITIVE);
	if (*sym == XKB_KEY_NoSymbol) {
		*bad = part;
		return 0;
	}
	/* A letter is the key's own keysym, as keypress() compares them */
	*sym = xkb_keysym_to_lower(*sym);
	return 1;
}

/* The next word of *p, moving *p past it and the space after. */
static char *
settings_word(char **p)
{
	char *word = *p;
	while (**p && !isspace((unsigned char)**p))
		(*p)++;
	if (**p)
		*(*p)++ = '\0';
	while (isspace((unsigned char)**p))
		(*p)++;
	return word;
}

/* One argument, as sway reads it: a word, or text in double quotes, which
 * may be empty. Moves *p past it. Returns NULL for an unclosed quote, or
 * one with more of the word after it. */
static char *
settings_arg(char **p)
{
	char *arg = *p, *close;
	if (*arg != '"')
		return settings_word(p);
	if (!(close = strchr(++arg, '"')) || (close[1] && !isspace((unsigned char)close[1])))
		return NULL;
	*close = '\0';
	*p = close + 1;
	while (isspace((unsigned char)**p))
		(*p)++;
	return arg;
}

/* A bindswitch line, as sway's: "[--locked] lid|tablet:on|off|toggle
 * COMMAND". A later one for the same switch and state replaces an earlier
 * one. */
static int
settings_add_switch(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	static const char *const states[] = {[SwOff] = "off", [SwOn] = "on", [SwToggle] = "toggle"};
	SwitchBinding b = {0}, *bs;
	char *spec, *colon;
	size_t i;
	while (rest[0] == '-' && rest[1] == '-') {
		char *flag = settings_word(&rest);
		if (!strcmp(flag, "--locked")) {
			b.locked = 1;
		} else {
			settings_error(errors, line, "bindswitch has no flag %s", flag);
			return 0;
		}
	}
	spec = settings_word(&rest);
	if (!*spec || !*rest) {
		settings_error(errors, line, "bindswitch needs a switch and a command");
		return 0;
	}
	b.state = -1;
	if ((colon = strchr(spec, ':'))) {
		*colon = '\0';
		for (i = 0; i < LENGTH(states); i++)
			if (!strcmp(colon + 1, states[i]))
				b.state = (int)i;
	}
	b.type = !strcmp(spec, "lid") ? WLR_SWITCH_TYPE_LID
		: !strcmp(spec, "tablet") ? WLR_SWITCH_TYPE_TABLET_MODE : -1;
	if (colon)
		*colon = ':';
	if (b.type < 0 || b.state < 0) {
		settings_error(errors, line, "bindswitch %s: a switch is lid or tablet, "
				"and its state on, off or toggle", spec);
		return 0;
	}
	for (i = 0; i < s->nswitches; i++)
		if (s->switches[i].type == b.type && s->switches[i].state == b.state)
			break;
	if (i == s->nswitches) {
		if (!(bs = realloc(s->switches, (s->nswitches + 1) * sizeof(*bs))))
			die("out of memory reading the config");
		s->switches = bs;
		s->nswitches++;
	} else {
		free(s->switches[i].command);
	}
	b.command = settings_strdup(rest, strlen(rest));
	s->switches[i] = b;
	return 1;
}

/* Adds a binding of combo to the mode, running command, named if it's
 * not 0, or reports why it can't, as "what combo: why". */
static int
settings_put_binding(Mode *mode, const char *what, char *combo, BindingFlags f,
		const char *command, size_t named, unsigned int line, struct json_object *errors)
{
	char *spec, *last;
	const char *bad = NULL;
	uint32_t mods, button = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	size_t i;
	Binding *b;

	spec = settings_strdup(combo, strlen(combo));
	if (!(last = settings_parse_mods(combo, &mods, &bad))) {
		settings_error(errors, line, "%s %s: no modifier or key is called %s", what, spec, bad);
		free(spec);
		return 0;
	}
	/* A button, as sway's: over a window, or none, with --whole-window,
	 * as tidewm has no title bars, where sway's runs without it */
	if ((button = settings_button(last)) && !f.whole_window) {
		settings_error(errors, line, "%s %s: a button's binding needs --whole-window, "
				"as there are no title bars", what, spec);
		free(spec);
		return 0;
	}
	if (f.whole_window && !button) {
		settings_error(errors, line, "%s --whole-window %s: no button is called %s, "
				"as button1 to button9 or BTN_LEFT", what, spec, last);
		free(spec);
		return 0;
	}
	if (button && (f.release || f.locked || f.kept_open)) {
		settings_error(errors, line, "%s --whole-window %s: a button's binding "
				"takes no --release, --locked or --kept-open", what, spec);
		free(spec);
		return 0;
	}
	if (!button && !settings_parse_keys(combo, &mods, &sym, &bad)) {
		settings_error(errors, line, "%s %s: no modifier or key is called %s", what, spec, bad);
		free(spec);
		return 0;
	}
	/* A later binding of the same keys replaces an earlier one; a release
	 * binding is apart from a press one, and a --kept-open one from one
	 * for every window */
	for (i = 0; i < mode->nbindings; i++) {
		b = &mode->bindings[i];
		if (b->mods == mods && b->sym == sym && b->button == button && b->release == f.release
				&& b->kept_open == f.kept_open) {
			free(b->keys);
			free(b->command);
			break;
		}
	}
	if (i == mode->nbindings) {
		if (!(b = realloc(mode->bindings, (mode->nbindings + 1) * sizeof(*b))))
			die("out of memory reading the config");
		mode->bindings = b;
		b = &mode->bindings[mode->nbindings++];
	}
	b->mods = mods;
	b->sym = sym;
	b->button = button;
	b->release = f.release;
	b->locked = f.locked;
	b->kept_open = f.kept_open;
	b->keys = spec;
	b->command = settings_strdup(command, strlen(command));
	b->named = named;
	return 1;
}

/* A binding's name, as tide's keyboard shortcuts take one: a lower-case
 * letter, then lower-case letters, digits and _ */
static int
settings_name_ok(const char *name)
{
	const char *p;
	if (!(*name >= 'a' && *name <= 'z'))
		return 0;
	for (p = name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
			return 0;
	return 1;
}

/* The index of the named binding called name, or s->nnamed. */
static size_t
settings_find_named(const Settings *s, const char *name)
{
	size_t i;
	for (i = 0; i < s->nnamed; i++)
		if (!strcmp(s->named[i].name, name))
			break;
	return i;
}

/* Registers combo as one of a named binding's keys: a new name, one more
 * line for a name already given the same command, mode and flags, or a
 * line that redefines the name. Returns its 1 + index, or 0 on an error. */
static size_t
settings_add_named(Settings *s, size_t mode, const char *name, const char *combo,
		BindingFlags f, const char *command, unsigned int line, struct json_object *errors)
{
	NamedBinding *n;
	char **d;
	size_t i = settings_find_named(s, name);
	if (!settings_name_ok(name)) {
		settings_error(errors, line, "bindsym --name %s: a name is a lower-case letter, "
				"then lower-case letters, digits and _", name);
		return 0;
	}
	if (i == s->nnamed) {
		if (!(n = realloc(s->named, (s->nnamed + 1) * sizeof(*n))))
			die("out of memory reading the config");
		s->named = n;
		n = &s->named[s->nnamed++];
		memset(n, 0, sizeof(*n));
		n->name = settings_strdup(name, strlen(name));
		n->mode = mode;
		n->command = settings_strdup(command, strlen(command));
		n->flags = f;
	}
	n = &s->named[i];
	/* A line that gives a name another command, flags or mode redefines
	 * it, as a later binding of the same keys replaces an earlier one: a
	 * config that includes tide's and binds the same names to its own
	 * commands wins over tide's. The name's earlier keys go with what
	 * they ran. */
	if (n->mode != mode || strcmp(n->command, command) || memcmp(&n->flags, &f, sizeof(f))) {
		Mode *m = &s->modes[n->mode];
		size_t j, k;
		for (j = k = 0; j < m->nbindings; j++) {
			if (m->bindings[j].named == i + 1) {
				free(m->bindings[j].keys);
				free(m->bindings[j].command);
			} else {
				m->bindings[k++] = m->bindings[j];
			}
		}
		m->nbindings = k;
		for (j = 0; j < n->ndefaults; j++)
			free(n->defaults[j]);
		free(n->defaults);
		n->defaults = NULL;
		n->ndefaults = 0;
		free(n->command);
		n->command = settings_strdup(command, strlen(command));
		n->mode = mode;
		n->flags = f;
	}
	for (i = 0; i < n->ndefaults; i++)
		if (!strcmp(n->defaults[i], combo))
			return (size_t)(n - s->named) + 1;
	if (!(d = realloc(n->defaults, (n->ndefaults + 1) * sizeof(*d))))
		die("out of memory reading the config");
	n->defaults = d;
	n->defaults[n->ndefaults++] = settings_strdup(combo, strlen(combo));
	return (size_t)(n - s->named) + 1;
}

/* A bindsym line: "[FLAGS] KEYS COMMAND", in the mode. */
static int
settings_add_binding(Settings *s, size_t mode, char *rest, unsigned int line,
		struct json_object *errors)
{
	BindingFlags f = {0};
	char *combo, *name = NULL;
	size_t named = 0;

	while (rest[0] == '-' && rest[1] == '-') {
		char *flag = settings_word(&rest);
		if (!strcmp(flag, "--release")) {
			f.release = 1;
		} else if (!strcmp(flag, "--locked")) {
			f.locked = 1;
		} else if (!strcmp(flag, "--kept-open")) {
			f.kept_open = 1;
		} else if (!strcmp(flag, "--whole-window")) {
			f.whole_window = 1;
		} else if (!strncmp(flag, "--name=", 7) && flag[7]) {
			name = flag + 7;
		} else if (!strcmp(flag, "--name") && *rest && strncmp(rest, "--", 2)) {
			name = settings_word(&rest);
		} else if (!strcmp(flag, "--name") || !strcmp(flag, "--name=")) {
			settings_error(errors, line, "bindsym --name needs a name");
			return 0;
		} else {
			settings_error(errors, line, "bindsym has no flag %s", flag);
			return 0;
		}
	}
	combo = settings_word(&rest);
	if (!*combo || !*rest) {
		settings_error(errors, line, "bindsym needs keys and a command");
		return 0;
	}
	if (name && !(named = settings_add_named(s, mode, name, combo, f, rest, line, errors)))
		return 0;
	return settings_put_binding(&s->modes[mode], "bindsym", combo, f, rest, named, line, errors);
}

/* Writes a problem with a line that doesn't stop the file loading to
 * stderr, as "tidewm: FILE:LINE: ...". */
static void
settings_warn(unsigned int line, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "tidewm: %s:%u: ", settings_file ? settings_file : settings_path, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* A rebind line, tidewm's own: "NAME KEYS..." gives the named binding
 * these keys in place of the ones it has, and "NAME none" takes them all
 * away. A name no line has given is reported and skipped, as Hyprland's
 * tide-keys.lua's is, so a shell's file outliving a name in the config
 * doesn't stop it loading. */
static int
settings_rebind(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *name = settings_word(&rest), *combo, what[128];
	NamedBinding *n;
	Mode *mode;
	size_t i, j, idx;
	int ok = 1;
	if (!*name || !*rest) {
		settings_error(errors, line, "rebind needs a binding's name, then its keys, or none");
		return 0;
	}
	if ((idx = settings_find_named(s, name)) == s->nnamed) {
		settings_warn(line, "rebind %s: no binding is called %s, so it's skipped", name, name);
		return 1;
	}
	n = &s->named[idx];
	snprintf(what, sizeof(what), "rebind %s", name);
	if (!strcmp(rest, "none")) {
		rest += 4;
	} else {
		/* Each key is checked before any is taken away */
		char *copy = settings_strdup(rest, strlen(rest)), *p = copy;
		Mode scratch = {0};
		while (*(combo = settings_word(&p))) {
			if (!strcmp(combo, "none")) {
				settings_error(errors, line, "rebind %s: none is alone, for no keys", name);
				ok = 0;
			} else {
				ok &= settings_put_binding(&scratch, what, combo, n->flags, n->command, 0,
						line, errors);
			}
		}
		for (j = 0; j < scratch.nbindings; j++) {
			free(scratch.bindings[j].keys);
			free(scratch.bindings[j].command);
		}
		free(scratch.bindings);
		free(copy);
		if (!ok)
			return 0;
	}
	mode = &s->modes[n->mode];
	for (i = j = 0; i < mode->nbindings; i++) {
		if (mode->bindings[i].named == idx + 1) {
			free(mode->bindings[i].keys);
			free(mode->bindings[i].command);
		} else {
			mode->bindings[j++] = mode->bindings[i];
		}
	}
	mode->nbindings = j;
	while (*(combo = settings_word(&rest)))
		settings_put_binding(mode, what, combo, n->flags, n->command, idx + 1, line, errors);
	return 1;
}

/* A floating_modifier line, as sway's: "MOD [normal|inverse]", or "none".
 * MOD and the left button move a floating window, and MOD and the right
 * one resize it; inverse swaps them. */
static int
settings_floating_modifier(Settings *s, char *rest, unsigned int line,
		struct json_object *errors)
{
	char *mod = settings_word(&rest), *how = settings_word(&rest), *combo;
	const char *bad = NULL;
	uint32_t mods;
	size_t n = strlen(mod);
	if (!*mod || *rest || (*how && strcasecmp(how, "normal") && strcasecmp(how, "inverse"))) {
		settings_error(errors, line, "floating_modifier takes a modifier, "
				"then normal or inverse, or none");
		return 0;
	}
	if (!strcasecmp(mod, "none")) {
		s->floating_mod = 0;
		s->floating_inverse = 0;
		return 1;
	}
	/* Read as a binding's modifiers, before a key */
	combo = malloc(n + 3);
	if (!combo)
		die("out of memory reading the config");
	snprintf(combo, n + 3, "%s+x", mod);
	if (!settings_parse_mods(combo, &mods, &bad) || !mods) {
		settings_error(errors, line, "floating_modifier: no modifier is called %s", mod);
		free(combo);
		return 0;
	}
	free(combo);
	s->floating_mod = (int)mods;
	s->floating_inverse = !strcasecmp(how, "inverse");
	return 1;
}

static int
settings_set(SettingsVar **vars, size_t *nvars, const char *name, const char *value,
		unsigned int line, struct json_object *errors)
{
	const char *p;
	SettingsVar *v;
	size_t i;
	for (p = name + 1; *p && (isalnum((unsigned char)*p) || *p == '_'); p++)
		;
	if (name[0] != '$' || p == name + 1 || *p) {
		settings_error(errors, line, "set needs a $name, of letters, digits and _, then its value");
		return 0;
	}
	for (i = 0; i < *nvars && strcmp((*vars)[i].name, name); i++)
		;
	if (i == *nvars) {
		if (!(v = realloc(*vars, (*nvars + 1) * sizeof(*v))))
			die("out of memory reading the config");
		*vars = v;
		(*vars)[(*nvars)++].name = settings_strdup(name, strlen(name));
	} else {
		free((*vars)[i].value);
	}
	(*vars)[i].value = settings_strdup(value, strlen(value));
	return 1;
}

/* Whether s[i] follows an odd run of backslashes, which escapes it, as
 * an exec's shell will read it. */
static int
settings_escaped(const char *s, size_t i)
{
	size_t n = 0;
	while (i > n && s[i - n - 1] == '\\')
		n++;
	return n % 2;
}

/* "on" or "off" as 1 or 0, else -1 */
static int
settings_on_off(const char *word)
{
	return !strcmp(word, "on") ? 1 : !strcmp(word, "off") ? 0 : -1;
}

/* Parses an output line's settings, rest, for the outputs name names,
 * into oc, as the config file, its output blocks and the output command
 * share it. On failure, err says why. */
static int
settings_parse_output_settings(OutputConfig *oc, char *name, char *rest, char *err, size_t errsize)
{
	static const struct { const char *name; enum wl_output_transform t; } transforms[] = {
		{"normal", WL_OUTPUT_TRANSFORM_NORMAL}, {"90", WL_OUTPUT_TRANSFORM_90},
		{"180", WL_OUTPUT_TRANSFORM_180}, {"270", WL_OUTPUT_TRANSFORM_270},
		{"flipped", WL_OUTPUT_TRANSFORM_FLIPPED},
		{"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90},
		{"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
		{"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
	};
	static const struct { const char *name; int place; } places[] = {
		{"auto", PlaceRight}, {"auto-right", PlaceRight}, {"auto-left", PlaceLeft},
		{"auto-up", PlaceUp}, {"auto-down", PlaceDown},
	};
	char *word, *arg, *end, extra;
	size_t i, len;
	float hz;
	int used, good;

	memset(oc, 0, sizeof(*oc));
	oc->enabled = -1;
	oc->power = -1;
	if (!name || !*name) {
		snprintf(err, errsize, "output needs a name, or *, then settings");
		return 0;
	}
	oc->name = name;
	if (!*rest) {
		snprintf(err, errsize, "output %s needs a setting", oc->name);
		return 0;
	}
	while (*(word = settings_word(&rest))) {
		if (!strcmp(word, "mode") || !strcmp(word, "resolution") || !strcmp(word, "res")) {
			/* WIDTHxHEIGHT, then @HZ, Hz or not, if it's given */
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			if ((oc->custom = !strcmp(arg, "--custom")) && !(arg = settings_arg(&rest)))
				goto unclosed;
			len = strlen(arg);
			if (len > 2 && !strcasecmp(arg + len - 2, "hz"))
				arg[len - 2] = '\0';
			hz = 0;
			used = 0;
			good = sscanf(arg, "%dx%d%n", &oc->width, &oc->height, &used) == 2
				&& oc->width > 0 && oc->height > 0;
			if (good && arg[used] == '@') {
				hz = strtof(arg + used + 1, &end);
				/* @0, as nwg-displays writes for an output with
				 * no rate, is any rate, as sway takes it */
				good = end != arg + used + 1 && !*end && isfinite(hz) && hz >= 0 && hz <= 1000;
			} else if (good && arg[used]) {
				good = 0;
			}
			if (!good) {
				snprintf(err, errsize, "output %s: mode %s isn't WIDTHxHEIGHT[@HZ]", oc->name, arg);
				return 0;
			}
			oc->refresh = (int)roundf(hz * 1000);
			oc->has_mode = 1;
		} else if (!strcmp(word, "scale")) {
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			oc->scale = strtof(arg, &end);
			if (end == arg || *end || !isfinite(oc->scale) || oc->scale <= 0 || oc->scale > 10) {
				snprintf(err, errsize, "output %s: scale %s isn't a number from above 0 to 10", oc->name, arg);
				return 0;
			}
			oc->has_scale = 1;
		} else if (!strcmp(word, "position") || !strcmp(word, "pos")) {
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			for (i = 0; i < LENGTH(places) && strcmp(arg, places[i].name); i++)
				;
			if (i < LENGTH(places)) {
				oc->place = places[i].place;
			} else if (strchr(arg, ',') ? sscanf(arg, "%d,%d%c", &oc->x, &oc->y, &extra) != 2
					: (sscanf(arg, "%d%c", &oc->x, &extra) != 1
					|| !(word = settings_arg(&rest))
					|| sscanf(word, "%d%c", &oc->y, &extra) != 1)) {
				snprintf(err, errsize, "output %s: position needs X Y, or auto, "
						"auto-right, auto-left, auto-up or auto-down", oc->name);
				return 0;
			} else {
				oc->place = PlaceAt;
			}
			oc->has_position = 1;
		} else if (!strcmp(word, "transform")) {
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			for (i = 0; i < LENGTH(transforms) && strcmp(arg, transforms[i].name); i++)
				;
			if (i == LENGTH(transforms)) {
				snprintf(err, errsize, "output %s: no transform is called %s", oc->name, arg);
				return 0;
			}
			oc->transform = transforms[i].t;
			oc->has_transform = 1;
		} else if (!strcmp(word, "enable") || !strcmp(word, "disable")) {
			oc->enabled = word[0] == 'e';
		} else if (!strcmp(word, "adaptive_sync")) {
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			if ((oc->adaptive_sync = settings_on_off(arg)) < 0) {
				snprintf(err, errsize, "output %s: adaptive_sync takes on or off", oc->name);
				return 0;
			}
			oc->has_adaptive_sync = 1;
		} else if (!strcmp(word, "power") || !strcmp(word, "dpms")) {
			/* As sway's: dpms is power's old name */
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			if ((oc->power = settings_on_off(arg)) < 0) {
				snprintf(err, errsize, "output %s: %s takes on or off", oc->name, word);
				return 0;
			}
		} else if (!strcmp(word, "scale_filter")) {
			/* Taken, as sway's, but tidewm always scales linearly */
			if (!(arg = settings_arg(&rest)))
				goto unclosed;
			if (strcmp(arg, "linear") && strcmp(arg, "nearest") && strcmp(arg, "smart")) {
				snprintf(err, errsize, "output %s: scale_filter takes linear, nearest or smart",
						oc->name);
				return 0;
			}
		} else {
			snprintf(err, errsize, "output %s: no setting is called %s", oc->name, word);
			return 0;
		}
	}
	return 1;
unclosed:
	snprintf(err, errsize, "output %s: a value's \" has no closing \"", oc->name);
	return 0;
}

/* Parses an output line's rest, "NAME SETTING...", into oc, as the config
 * file and the output command share it. On failure, err says why. */
static int
settings_parse_output(OutputConfig *oc, char *rest, char *err, size_t errsize)
{
	char *name = settings_arg(&rest);
	return settings_parse_output_settings(oc, name, rest, err, errsize);
}

/* Puts what from sets into into, as a later output line for the same
 * name adds to and overrides an earlier one. */
static void
settings_merge_output(OutputConfig *into, const OutputConfig *from)
{
	if (from->has_mode) {
		into->has_mode = 1;
		into->custom = from->custom;
		into->width = from->width;
		into->height = from->height;
		into->refresh = from->refresh;
	}
	if (from->has_scale) {
		into->has_scale = 1;
		into->scale = from->scale;
	}
	if (from->has_position) {
		into->has_position = 1;
		into->place = from->place;
		into->x = from->x;
		into->y = from->y;
	}
	if (from->has_transform) {
		into->has_transform = 1;
		into->transform = from->transform;
	}
	if (from->enabled >= 0)
		into->enabled = from->enabled;
	if (from->has_adaptive_sync) {
		into->has_adaptive_sync = 1;
		into->adaptive_sync = from->adaptive_sync;
	}
	if (from->power >= 0)
		into->power = from->power;
}

/* An output line, or a line of an output block, for the outputs name
 * names: rest, its settings */
static int
settings_add_output(Settings *s, char *name, char *rest, unsigned int line,
		struct json_object *errors)
{
	OutputConfig oc, *grown;
	char err[256];
	size_t i;
	if (!settings_parse_output_settings(&oc, name, rest, err, sizeof(err))) {
		settings_error(errors, line, "%s", err);
		return 0;
	}
	for (i = 0; i < s->noutputs; i++) {
		if (!strcmp(s->outputs[i].name, oc.name)) {
			settings_merge_output(&s->outputs[i], &oc);
			return 1;
		}
	}
	if (!(grown = realloc(s->outputs, (s->noutputs + 1) * sizeof(*grown))))
		die("out of memory reading the config");
	s->outputs = grown;
	oc.name = settings_strdup(oc.name, strlen(oc.name));
	s->outputs[s->noutputs++] = oc;
	return 1;
}

static void
settings_free_input(InputConfig *ic)
{
	size_t i;
	free(ic->name);
	for (i = 0; i < InXkbLast; i++)
		free((char *)ic->xkb[i]);
}

/* An input config that gives nothing, for name, which it doesn't own */
static void
settings_input_init(InputConfig *ic, char *name)
{
	size_t i;
	memset(ic, 0, sizeof(*ic));
	ic->name = name;
	for (i = 0; i < InLast; i++)
		ic->value[i] = -1;
}

/* Whether name can name devices: "*", a known type, or an identifier */
static int
settings_input_name_ok(const char *name, char *err, size_t errsize)
{
	static const char *const types[] = {"keyboard", "pointer", "touchpad"};
	size_t i;
	if (!*name) {
		snprintf(err, errsize, "input needs a device's identifier, a type:, or *, then a setting");
		return 0;
	}
	if (strncmp(name, "type:", 5))
		return 1;
	for (i = 0; i < LENGTH(types); i++)
		if (!strcmp(name + 5, types[i]))
			return 1;
	snprintf(err, errsize, "input %s: no device type is called %s", name, name + 5);
	return 0;
}

/* Reads one input setting, "SETTING VALUE", into ic, whose name says what
 * it's for. An XKB name it gives is a copy ic owns. */
static int
settings_parse_input_setting(InputConfig *ic, char *rest, char *err, size_t errsize)
{
	char *word = settings_word(&rest), *arg, *end;
	size_t i, j;
	long n;
	double d;
	int quoted = *rest == '"';
	if (!*word) {
		snprintf(err, errsize, "input %s needs a setting", ic->name);
		return 0;
	}
	/* A value may be in quotes, and an XKB name empty, as "" */
	if (!(arg = settings_arg(&rest))) {
		snprintf(err, errsize, "input %s: %s's \" has no closing \"", ic->name, word);
		return 0;
	}
	if ((!*arg && !quoted) || *rest) {
		snprintf(err, errsize, "input %s: %s needs one value", ic->name, word);
		return 0;
	}
	for (i = 0; i < InXkbLast; i++) {
		if (!strcmp(word, settings_xkb_names[i])) {
			free((char *)ic->xkb[i]);
			ic->xkb[i] = settings_strdup(arg, strlen(arg));
			return 1;
		}
	}
	if (!strcmp(word, "pointer_accel")) {
		errno = 0;
		d = strtod(arg, &end);
		if (end == arg || *end || errno || !isfinite(d) || d < -1 || d > 1) {
			snprintf(err, errsize, "input %s: pointer_accel %s isn't a number from -1 to 1",
					ic->name, arg);
			return 0;
		}
		ic->has_accel = 1;
		ic->accel = d;
		return 1;
	}
	if (!strcmp(word, "scroll_factor")) {
		errno = 0;
		d = strtod(arg, &end);
		if (end == arg || *end || errno || !isfinite(d) || d < 0) {
			snprintf(err, errsize, "input %s: scroll_factor %s isn't a number, 0 or more",
					ic->name, arg);
			return 0;
		}
		ic->has_scroll_factor = 1;
		ic->scroll_factor = d;
		return 1;
	}
	for (i = 0; i < InLast; i++) {
		if (strcmp(word, settings_input_settings[i].name))
			continue;
		if (!settings_input_settings[i].words[0]) {
			errno = 0;
			n = strtol(arg, &end, 10);
			if (end == arg || *end || errno || n < 0 || n > 100000) {
				snprintf(err, errsize, "input %s: %s %s isn't a whole number from 0 to 100000",
						ic->name, word, arg);
				return 0;
			}
			ic->value[i] = (int)n;
			return 1;
		}
		for (j = 0; j < LENGTH(settings_input_settings[i].words)
				&& settings_input_settings[i].words[j]; j++) {
			if (!strcmp(arg, settings_input_settings[i].words[j])) {
				ic->value[i] = settings_input_settings[i].values[j];
				return 1;
			}
		}
		snprintf(err, errsize, "input %s: %s can't be %s", ic->name, word, arg);
		return 0;
	}
	snprintf(err, errsize, "input %s: no setting is called %s", ic->name, word);
	return 0;
}

/* Puts what src gives over dst. dst borrows src's XKB names. */
static void
settings_merge_input(InputConfig *dst, const InputConfig *src)
{
	size_t i;
	for (i = 0; i < InXkbLast; i++)
		if (src->xkb[i])
			dst->xkb[i] = src->xkb[i];
	for (i = 0; i < InLast; i++)
		if (src->value[i] >= 0)
			dst->value[i] = src->value[i];
	if (src->has_accel) {
		dst->has_accel = 1;
		dst->accel = src->accel;
	}
	if (src->has_scroll_factor) {
		dst->has_scroll_factor = 1;
		dst->scroll_factor = src->scroll_factor;
	}
}

/* Adds one setting for the devices name names, from the file's line. A
 * name's settings, from every line for it, are one InputConfig. */
static int
settings_add_input(Settings *s, char *name, char *setting, unsigned int line,
		struct json_object *errors)
{
	InputConfig *ic = NULL, *grown;
	char err[256];
	size_t i;
	if (!settings_input_name_ok(name, err, sizeof(err))) {
		settings_error(errors, line, "%s", err);
		return 0;
	}
	for (i = 0; i < s->ninputs; i++)
		if (!strcmp(s->inputs[i].name, name))
			ic = &s->inputs[i];
	if (!ic) {
		if (!(grown = realloc(s->inputs, (s->ninputs + 1) * sizeof(*grown))))
			die("out of memory reading the config");
		s->inputs = grown;
		ic = &s->inputs[s->ninputs++];
		settings_input_init(ic, settings_strdup(name, strlen(name)));
	}
	if (!settings_parse_input_setting(ic, setting, err, sizeof(err))) {
		settings_error(errors, line, "%s", err);
		return 0;
	}
	for (i = 0; i < InXkbLast; i++) {
		if (!strncmp(setting, settings_xkb_names[i], strlen(settings_xkb_names[i]))) {
			ic->xkb_line = line;
			ic->xkb_file = settings_file;
		}
	}
	return 1;
}

/* Reads for_window's criteria, [key=value ...], and its commands into a
 * new rule. A value is a word or text in double quotes, where \" is a
 * quote, and is a PCRE2 pattern, as sway's criteria are, in UTF-8 with
 * Unicode's classes, matching anywhere unless anchored. */
static int
settings_add_rule(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	WindowRule rule = {0}, *grown;
	char *p = rest, *key, *value, *out, msg[160];
	size_t i, n;
	int err, ok = 1, any = 0;
	PCRE2_SIZE at;

	if (*p++ != '[') {
		settings_error(errors, line, "for_window needs [criteria], then commands");
		return 0;
	}
	for (;;) {
		while (isspace((unsigned char)*p))
			p++;
		if (*p == ']' || !*p)
			break;
		key = p;
		while (isalnum((unsigned char)*p) || *p == '_')
			p++;
		n = (size_t)(p - key);
		if (*p != '=' || !n) {
			settings_error(errors, line, "for_window's criteria are key=value, in [ ]");
			ok = 0;
			break;
		}
		*p++ = '\0';
		for (i = 0; i < CritLast && strcmp(key, settings_crit_names[i]); i++)
			;
		/* The value, unquoted in place */
		value = out = p;
		if (*p == '"') {
			for (value = out = ++p; *p && *p != '"'; p++) {
				if (*p == '\\' && p[1] == '"')
					p++;
				*out++ = *p;
			}
			if (*p != '"') {
				settings_error(errors, line, "for_window: %s's value has no closing \"", key);
				ok = 0;
				break;
			}
			p++;
		} else {
			while (*p && *p != ']' && !isspace((unsigned char)*p))
				p++;
			out = p;
		}
		if (i == CritLast) {
			settings_error(errors, line, "for_window: no criterion is called %s", key);
			ok = 0;
			break;
		}
		if (rule.has[i]) {
			settings_error(errors, line, "for_window: %s is there twice", key);
			ok = 0;
			break;
		}
		{
			char saved = *out;
			*out = '\0';
			rule.re[i] = pcre2_compile((PCRE2_SPTR)value, PCRE2_ZERO_TERMINATED,
					PCRE2_UTF | PCRE2_UCP, &err, &at, NULL);
			if (!rule.re[i]) {
				pcre2_get_error_message(err, (PCRE2_UCHAR *)msg, sizeof(msg));
				settings_error(errors, line, "for_window: %s=%s: %s at character %zu",
						key, value, msg, (size_t)at);
				*out = saved;
				ok = 0;
				break;
			}
			*out = saved;
		}
		rule.has[i] = any = 1;
	}
	if (ok && *p != ']') {
		settings_error(errors, line, "for_window's criteria have no ]");
		ok = 0;
	}
	if (ok) {
		for (p++; isspace((unsigned char)*p); p++)
			;
		if (!any || !*p) {
			settings_error(errors, line, "for_window needs [criteria], then commands");
			ok = 0;
		}
	}
	if (!ok) {
		for (i = 0; i < CritLast; i++)
			if (rule.has[i])
				pcre2_code_free(rule.re[i]);
		return 0;
	}
	rule.command = settings_strdup(p, strlen(p));
	if (!(grown = realloc(s->rules, (s->nrules + 1) * sizeof(*grown))))
		die("out of memory reading the config");
	s->rules = grown;
	s->rules[s->nrules++] = rule;
	return 1;
}

/* A focus_guard line: enable or disable, or grant_seconds or
 * prompt_seconds and a number of seconds */
static int
settings_focus_guard(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *word = settings_arg(&rest), *end;
	double *seconds = NULL, v;
	if (word && (!strcmp(word, "enable") || !strcmp(word, "disable")) && !*rest) {
		s->focus_guard = !strcmp(word, "enable");
		return 1;
	}
	if (word && !strcmp(word, "grant_seconds"))
		seconds = &s->grant_seconds;
	else if (word && !strcmp(word, "prompt_seconds"))
		seconds = &s->prompt_seconds;
	if (!seconds) {
		settings_error(errors, line, "focus_guard takes enable, disable, grant_seconds "
				"or prompt_seconds");
		return 0;
	}
	errno = 0;
	v = strtod(rest, &end);
	/* NaN fails v > 0, and an infinite or huge one would never lapse */
	if (!*rest || *end || errno || !(v > 0 && v <= 86400)) {
		settings_error(errors, line, "focus_guard %s takes a number of seconds, "
				"more than 0 and at most a day", word);
		return 0;
	}
	*seconds = v;
	return 1;
}

/* A single_window line, "MIN_ASPECT WIDTH", as tide's layout takes them:
 * an aspect from 0 to 100 and a width from 0.1 to 1 */
static int
settings_single(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	SingleRule r;
	char *end;
	errno = 0;
	r.min_aspect = strtod(rest, &end);
	if (end != rest && isspace((unsigned char)*end)) {
		rest = end;
		r.width = strtod(rest, &end);
	} else {
		end = rest;
	}
	if (end == rest || *end || errno || !(r.min_aspect >= 0 && r.min_aspect <= 100)
			|| !(r.width >= 0.1 && r.width <= 1)) {
		settings_error(errors, line, "single_window takes an aspect from 0 to 100 "
				"and a width from 0.1 to 1");
		return 0;
	}
	s->singles = realloc(s->singles, (s->nsingles + 1) * sizeof(*s->singles));
	if (!s->singles)
		die("realloc:");
	s->singles[s->nsingles++] = r;
	return 1;
}

/* An ultrawide_aspect line: how wide for its height a monitor's window
 * area must be for its new parts to start in default_layout ultrawide's
 * layout, from 0.1 to 100 */
static int
settings_ultrawide(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *end;
	double v;
	errno = 0;
	v = strtod(rest, &end);
	if (end == rest || *end || errno || !(v >= 0.1 && v <= 100)) {
		settings_error(errors, line, "ultrawide_aspect takes a number from 0.1 to 100");
		return 0;
	}
	s->ultrawide_aspect = v;
	return 1;
}

/* A default_layout line, "normal|ultrawide LAYOUT": the layout a
 * workspace's new part on a monitor starts in, below ultrawide_aspect or
 * from it */
static int
settings_first_layout(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *which = settings_word(&rest), *name = settings_word(&rest);
	int wide = !strcmp(which, "ultrawide");
	size_t i;
	if ((wide || !strcmp(which, "normal")) && !*rest) {
		for (i = 0; i < LENGTH(ipc_layouts); i++) {
			if (ipc_layouts[i].arrange && !strcmp(name, ipc_layouts[i].name)) {
				s->first_layout[wide] = ipc_layouts[i].arrange;
				return 1;
			}
		}
	}
	settings_error(errors, line, "default_layout takes normal or ultrawide, "
			"then tile, threecol, twocol or monocle");
	return 0;
}

/* A layout_defaults line, "LAYOUT [mfact F] [nmaster N]": the mfact, from
 * 0.1 to 0.9, and nmaster, up to 100, the layout starts with. A later line
 * for a layout adds to an earlier one. */
static int
settings_layout_defaults(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *name = settings_word(&rest), *word, *arg, *end;
	LayoutDefaults d;
	size_t i;
	double v;
	long n;
	for (i = 0; i < LENGTH(settings_lt_modes) && strcmp(name, settings_lt_modes[i].name); i++)
		;
	if (i == LENGTH(settings_lt_modes) || !*rest) {
		settings_error(errors, line, "layout_defaults takes tile, threecol or twocol, "
				"then mfact, nmaster or both");
		return 0;
	}
	d = s->layout_defaults[i];
	while (*(word = settings_word(&rest))) {
		arg = settings_word(&rest);
		errno = 0;
		if (!strcmp(word, "mfact")) {
			v = strtod(arg, &end);
			if (end == arg || *end || errno || !(v >= 0.1 && v <= 0.9)) {
				settings_error(errors, line, "layout_defaults %s: mfact takes "
						"a number from 0.1 to 0.9", name);
				return 0;
			}
			d.has_mfact = 1;
			d.mfact = (float)v;
		} else if (!strcmp(word, "nmaster")) {
			n = strtol(arg, &end, 10);
			if (end == arg || *end || errno || n < settings_lt_modes[i].min_masters || n > 100) {
				settings_error(errors, line, "layout_defaults %s: nmaster takes "
						"a whole number from %d to 100", name, settings_lt_modes[i].min_masters);
				return 0;
			}
			d.has_nmaster = 1;
			d.nmaster = (int)n;
		} else {
			settings_error(errors, line, "layout_defaults %s: no setting is called %s", name, word);
			return 0;
		}
	}
	s->layout_defaults[i] = d;
	return 1;
}

/* A layout_cycle line: the layouts layout next and prev go through, in
 * order, each of tile, threecol and twocol once, as tide's cycle */
static int
settings_layout_cycle(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *name;
	size_t i, j, n = 0;
	while (*(name = settings_word(&rest))) {
		for (i = 0; i < LENGTH(settings_lt_modes) && strcmp(name, settings_lt_modes[i].name); i++)
			;
		if (i == LENGTH(settings_lt_modes)) {
			settings_error(errors, line, "layout_cycle takes tile, threecol and twocol, "
					"not %s", name);
			return 0;
		}
		for (j = 0; j < n; j++) {
			if (s->cycle[j] == settings_lt_modes[i].arrange) {
				settings_error(errors, line, "layout_cycle has %s twice", name);
				return 0;
			}
		}
		s->cycle[n++] = settings_lt_modes[i].arrange;
	}
	if (!n) {
		settings_error(errors, line, "layout_cycle needs a layout");
		return 0;
	}
	s->ncycle = n;
	return 1;
}

/* A new_window line: end, top, next or master */
static int
settings_new_window(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	static const char *const names[] = {
		[NewEnd] = "end", [NewTop] = "top", [NewNext] = "next", [NewMaster] = "master",
	};
	size_t i;
	for (i = 0; i < LENGTH(names); i++) {
		if (!strcmp(rest, names[i])) {
			s->new_window = (int)i;
			return 1;
		}
	}
	settings_error(errors, line, "new_window takes end, top, next or master");
	return 0;
}

/* An xwayland_scaling line: enable, as by default, or disable, for X11
 * windows in their outputs' pixels (x11.h) */
static int
settings_xwayland_scaling(Settings *s, char *rest, unsigned int line,
		struct json_object *errors)
{
	char *word = settings_arg(&rest);
	if (!word || (strcmp(word, "enable") && strcmp(word, "disable")) || *rest) {
		settings_error(errors, line, "xwayland_scaling takes enable or disable");
		return 0;
	}
	s->xwayland_scaling = !strcmp(word, "enable");
	return 1;
}

/* A dim_inactive line: how dark the dim over every window but the focused
 * one is, from 0, none, to 1, black */
static int
settings_dim(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *end;
	double v;
	errno = 0;
	v = strtod(rest, &end);
	/* NaN fails both comparisons */
	if (!*rest || *end || errno || !(v >= 0 && v <= 1)) {
		settings_error(errors, line, "dim_inactive takes a strength from 0 to 1");
		return 0;
	}
	s->dim_inactive = v;
	return 1;
}

/* A resize_grab line: how many pixels either side of the master/stack
 * edge a press drags it from, from 0, none, to 32 */
static int
settings_resize_grab(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *end;
	long v;
	errno = 0;
	v = strtol(rest, &end, 10);
	if (!*rest || *end || errno || v < 0 || v > 32) {
		settings_error(errors, line, "resize_grab takes a whole number of pixels from 0 to 32");
		return 0;
	}
	s->resize_grab = (int)v;
	return 1;
}

/* A mode's or input selector's name: a word, or text in double quotes,
 * as sway takes it. Moves *p past it. Returns NULL for an empty name or an
 * unclosed quote. */
static char *
settings_name(char **p)
{
	char *name = settings_arg(p);
	return name && *name ? name : NULL;
}

/* The index of the mode called name, made if it's new. */
static size_t
settings_add_mode(Settings *s, const char *name)
{
	Mode *m;
	size_t i;
	for (i = 0; i < s->nmodes; i++)
		if (!strcmp(s->modes[i].name, name))
			return i;
	if (!(m = realloc(s->modes, (s->nmodes + 1) * sizeof(*m))))
		die("out of memory reading the config");
	s->modes = m;
	memset(&s->modes[s->nmodes], 0, sizeof(*m));
	s->modes[s->nmodes].name = settings_strdup(name, strlen(name));
	return s->nmodes++;
}

static struct xkb_context *settings_xkb;

/* config.h's XKB names, which an input line's go over */
static void
settings_xkb_base(const char *names[InXkbLast])
{
	names[InXkbRules] = xkb_rules.rules;
	names[InXkbModel] = xkb_rules.model;
	names[InXkbLayout] = xkb_rules.layout;
	names[InXkbVariant] = xkb_rules.variant;
	names[InXkbOptions] = xkb_rules.options;
}

/* A keymap of these XKB names, or NULL if XKB can't make one; quiet
 * says not to write XKB's own messages to stderr. */
static struct xkb_keymap *
settings_keymap(const char *const names[InXkbLast], int quiet)
{
	struct xkb_rule_names rn = {
		.rules = names[InXkbRules], .model = names[InXkbModel],
		.layout = names[InXkbLayout], .variant = names[InXkbVariant],
		.options = names[InXkbOptions],
	};
	struct xkb_keymap *keymap;
	if (!settings_xkb && !(settings_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
		return NULL;
	xkb_context_set_log_level(settings_xkb, quiet ? XKB_LOG_LEVEL_CRITICAL : XKB_LOG_LEVEL_ERROR);
	keymap = xkb_keymap_new_from_names(settings_xkb, &rn, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_set_log_level(settings_xkb, XKB_LOG_LEVEL_ERROR);
	return keymap;
}

/* Checks that XKB can make the keymap a keyboard gets from lines, each
 * over the last, over config.h's names; if not, it's an error at blame's
 * last xkb_ line. */
static int
settings_check_keymap(const InputConfig *const lines[3], const InputConfig *blame,
		struct json_object *errors)
{
	const char *names[InXkbLast];
	struct xkb_keymap *keymap;
	size_t i, j;
	settings_xkb_base(names);
	for (i = 0; i < 3; i++)
		for (j = 0; lines[i] && j < InXkbLast; j++)
			if (lines[i]->xkb[j])
				names[j] = lines[i]->xkb[j];
	if ((keymap = settings_keymap(names, 1))) {
		xkb_keymap_unref(keymap);
		return 1;
	}
	/* The error is at that line, in whichever file it's in */
	settings_file = blame->xkb_file;
	settings_error(errors, blame->xkb_line, "input %s: XKB can't make a keymap of its xkb_ settings",
			blame->name);
	settings_file = NULL;
	return 0;
}

/* Checks that XKB can make a keymap of what each keyboard would get from
 * the file: *'s names, then type:keyboard's, then, for one with xkb_
 * lines of its own, its identifier's. So a layout that isn't there, or a
 * variant its layout lacks, is an error in the file. A pointer's xkb_
 * lines are never used. */
static int
settings_check_keymaps(Settings *s, struct json_object *errors)
{
	const InputConfig *lines[3] = {NULL, NULL, NULL};
	size_t i;
	int ok = 1;
	for (i = 0; i < s->ninputs; i++) {
		if (!strcmp(s->inputs[i].name, "*"))
			lines[0] = &s->inputs[i];
		else if (!strcmp(s->inputs[i].name, "type:keyboard"))
			lines[1] = &s->inputs[i];
	}
	if (lines[1] && lines[1]->xkb_line)
		ok &= settings_check_keymap(lines, lines[1], errors);
	else if (lines[0] && lines[0]->xkb_line)
		ok &= settings_check_keymap(lines, lines[0], errors);
	for (i = 0; i < s->ninputs; i++) {
		if (!s->inputs[i].xkb_line || !strcmp(s->inputs[i].name, "*")
				|| !strncmp(s->inputs[i].name, "type:", 5))
			continue;
		lines[2] = &s->inputs[i];
		ok &= settings_check_keymap(lines, lines[2], errors);
	}
	return ok;
}

static int settings_include(Settings *s, char *pattern, unsigned int line,
		SettingsVar **vars, size_t *nvars, int depth, struct json_object *errors);

/* Parses one file's text into s, adding each error to errors: the config
 * file, at depth 0, or one an include line read, at the top level, so a
 * mode or input block it opens must close in it. Variables carry from file
 * to file, in the order they're read. Returns whether it had no errors. */
static int
settings_parse_text(Settings *s, char *text, SettingsVar **vars, size_t *nvars, int depth,
		struct json_object *errors)
{
	size_t len, mode = 0;
	unsigned int line = 0, mode_line = 0, input_line = 0, output_line = 0;
	char *start, *end, *raw, *expanded, *p, *word, *name, *input = NULL, *output = NULL;
	int ok = 1;

	for (start = text; *start; start = *end ? end + 1 : end) {
		line++;
		end = strchr(start, '\n');
		if (!end)
			end = start + strlen(start);
		raw = settings_strdup(start, (size_t)(end - start));
		for (p = raw; isspace((unsigned char)*p); p++)
			;
		len = strlen(p);
		while (len && isspace((unsigned char)p[len - 1]) && !settings_escaped(p, len - 1))
			p[--len] = '\0';
		if (!*p || *p == '#') {
			free(raw);
			continue;
		}
		/* set's own name isn't substituted, only its value */
		if (!strncasecmp(p, "set", 3) && isspace((unsigned char)p[3])) {
			p += 3;
			while (isspace((unsigned char)*p))
				p++;
			name = settings_word(&p);
			expanded = settings_substitute(p, *vars, *nvars);
			ok &= settings_set(vars, nvars, name, expanded, line, errors);
			free(expanded);
			free(raw);
			continue;
		}
		expanded = settings_substitute(p, *vars, *nvars);
		p = expanded;
		/* In an input block, each line is a setting for its devices */
		if (input && strcmp(p, "}")) {
			ok &= settings_add_input(s, input, p, line, errors);
			free(expanded);
			free(raw);
			continue;
		}
		/* In an output block, as sway's, each line is a setting for its outputs */
		if (output && strcmp(p, "}")) {
			ok &= settings_add_output(s, output, p, line, errors);
			free(expanded);
			free(raw);
			continue;
		}
		word = settings_word(&p);
		if (input) {
			free(input);
			input = NULL;
		} else if (output) {
			free(output);
			output = NULL;
		} else if (!strcasecmp(word, "bindsym")) {
			ok &= settings_add_binding(s, mode, p, line, errors);
		} else if (!strcasecmp(word, "rebind")) {
			if (mode) {
				settings_error(errors, line, "rebind can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_rebind(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "bindswitch")) {
			if (mode) {
				settings_error(errors, line, "bindswitch can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_add_switch(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "output")) {
			if (mode) {
				settings_error(errors, line, "output can't be in a mode");
				ok = 0;
			} else if ((name = settings_arg(&p)) && *name && !strcmp(p, "{")) {
				output = settings_strdup(name, strlen(name));
				output_line = line;
			} else {
				ok &= settings_add_output(s, name, p, line, errors);
			}
		} else if (!strcasecmp(word, "input")) {
			if (!(name = settings_name(&p)))
				name = "";
			if (mode) {
				settings_error(errors, line, "input can't be in a mode");
				ok = 0;
			} else if (!strcmp(p, "{")) {
				char err[256];
				if (settings_input_name_ok(name, err, sizeof(err))) {
					input = settings_strdup(name, strlen(name));
					input_line = line;
				} else {
					settings_error(errors, line, "%s", err);
					ok = 0;
				}
			} else if (!*name) {
				settings_error(errors, line, "input needs a device's identifier, "
						"a type:, or *, then a setting");
				ok = 0;
			} else {
				ok &= settings_add_input(s, name, p, line, errors);
			}
		} else if (!strcasecmp(word, "single_window")) {
			if (mode) {
				settings_error(errors, line, "single_window can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_single(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "ultrawide_aspect") || !strcasecmp(word, "default_layout")
				|| !strcasecmp(word, "layout_defaults") || !strcasecmp(word, "new_window")
				|| !strcasecmp(word, "layout_cycle")) {
			if (mode) {
				settings_error(errors, line, "%s can't be in a mode", word);
				ok = 0;
			} else if (!strcasecmp(word, "ultrawide_aspect")) {
				ok &= settings_ultrawide(s, p, line, errors);
			} else if (!strcasecmp(word, "default_layout")) {
				ok &= settings_first_layout(s, p, line, errors);
			} else if (!strcasecmp(word, "layout_defaults")) {
				ok &= settings_layout_defaults(s, p, line, errors);
			} else if (!strcasecmp(word, "layout_cycle")) {
				ok &= settings_layout_cycle(s, p, line, errors);
			} else {
				ok &= settings_new_window(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "dim_inactive")) {
			if (mode) {
				settings_error(errors, line, "dim_inactive can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_dim(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "resize_grab")) {
			if (mode) {
				settings_error(errors, line, "resize_grab can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_resize_grab(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "xwayland_scaling")) {
			if (mode) {
				settings_error(errors, line, "xwayland_scaling can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_xwayland_scaling(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "floating_modifier")) {
			if (mode) {
				settings_error(errors, line, "floating_modifier can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_floating_modifier(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "focus_guard")) {
			if (mode) {
				settings_error(errors, line, "focus_guard can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_focus_guard(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "include")) {
			if (mode) {
				settings_error(errors, line, "include can't be in a mode");
				ok = 0;
			} else if (!(name = settings_name(&p)) || *p) {
				settings_error(errors, line, "include needs a file, or a pattern, and nothing after");
				ok = 0;
			} else {
				ok &= settings_include(s, name, line, vars, nvars, depth, errors);
			}
		} else if (!strcasecmp(word, "for_window")) {
			if (mode) {
				settings_error(errors, line, "for_window can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_add_rule(s, p, line, errors);
			}
		} else if (!strcasecmp(word, "mode")) {
			if (mode) {
				settings_error(errors, line, "mode %s is still open: modes don't nest",
						s->modes[mode].name);
				ok = 0;
			} else if (!(name = settings_name(&p)) || strcmp(p, "{")) {
				settings_error(errors, line, "mode needs a name, then {");
				ok = 0;
			} else if (!strcmp(name, "default")) {
				settings_error(errors, line, "default is the top level, not a mode to open");
				ok = 0;
			} else if (strchr(name, ';')) {
				/* The mode command couldn't name it: ; ends a command */
				settings_error(errors, line, "mode %s: a mode's name can't hold ;", name);
				ok = 0;
			} else {
				mode = settings_add_mode(s, name);
				mode_line = line;
			}
		} else if (!strcmp(word, "}")) {
			if (!mode) {
				settings_error(errors, line, "} closes no mode, input or output block");
				ok = 0;
			}
			mode = 0;
		} else {
			settings_error(errors, line, "no setting is called %s", word);
			ok = 0;
		}
		free(expanded);
		free(raw);
	}
	if (mode) {
		settings_error(errors, mode_line, "mode %s has no }", s->modes[mode].name);
		ok = 0;
	}
	if (input) {
		settings_error(errors, input_line, "input %s has no }", input);
		free(input);
		ok = 0;
	}
	if (output) {
		settings_error(errors, output_line, "output %s has no }", output);
		free(output);
		ok = 0;
	}
	return ok;
}

static int settings_read(const char *path, char **text, struct json_object *errors);

/* src into dst with glob's special characters escaped, so a directory is
 * taken as it is, not as a pattern. Returns 0 if it doesn't fit. */
static int
settings_glob_quote(char *dst, size_t size, const char *src)
{
	size_t n = 0;
	for (; *src; src++) {
		if (n + 2 >= size)
			return 0;
		if (strchr("*?[]\\", *src))
			dst[n++] = '\\';
		dst[n++] = *src;
	}
	dst[n] = '\0';
	return 1;
}

/* include PATTERN: reads each file PATTERN matches, in order, as if its
 * lines were here, as sway's does. ~/ is $HOME's, and a relative pattern
 * is from the directory of the file it's in. A pattern nothing matches,
 * such as a file that isn't written yet, includes nothing. */
static int
settings_include(Settings *s, char *pattern, unsigned int line, SettingsVar **vars,
		size_t *nvars, int depth, struct json_object *errors)
{
	const char *home = getenv("HOME"), *outer = settings_file;
	char path[PATH_MAX], dir[PATH_MAX], quoted[PATH_MAX], *slash, *text;
	glob_t found;
	size_t i;
	int n = -1, r, ok = 1;

	if (depth >= SETTINGS_MAX_DEPTH) {
		settings_error(errors, line, "include %s: includes nest over %d deep, "
				"as when a file includes itself", pattern, SETTINGS_MAX_DEPTH);
		return 0;
	}
	/* Only the line's own pattern is one: the directory it's from is
	 * escaped, whatever its name holds */
	if (!strncmp(pattern, "~/", 2)) {
		if (settings_glob_quote(quoted, sizeof(quoted), home ? home : ""))
			n = snprintf(path, sizeof(path), "%s/%s", quoted, pattern + 2);
	} else if (*pattern == '/') {
		n = snprintf(path, sizeof(path), "%s", pattern);
	} else {
		snprintf(dir, sizeof(dir), "%s", outer ? outer : settings_path);
		if ((slash = strrchr(dir, '/')))
			*slash = '\0';
		else
			snprintf(dir, sizeof(dir), ".");
		if (settings_glob_quote(quoted, sizeof(quoted), dir))
			n = snprintf(path, sizeof(path), "%s/%s", quoted, pattern);
	}
	if (n < 0 || (size_t)n >= sizeof(path)) {
		settings_error(errors, line, "include %s: the path is too long", pattern);
		return 0;
	}
	if ((r = glob(path, 0, NULL, &found)) == GLOB_NOMATCH)
		return 1;
	if (r) {
		settings_error(errors, line, "include %s: can't list what it matches", pattern);
		return 0;
	}
	for (i = 0; i < found.gl_pathc; i++) {
		SettingsInclude *inc;
		s->includes = realloc(s->includes, (s->nincludes + 1) * sizeof(*s->includes));
		if (!s->includes)
			die("out of memory reading the config");
		inc = &s->includes[s->nincludes++];
		inc->path = settings_strdup(found.gl_pathv[i], strlen(found.gl_pathv[i]));
		inc->text = NULL;
		/* Its errors name it; inc may move as it includes others */
		settings_file = inc->path;
		if ((r = settings_read(settings_file, &text, errors)) > 0) {
			inc->text = text;
			ok &= settings_parse_text(s, text, vars, nvars, depth + 1, errors);
		} else if (r < 0) {
			ok = 0;
		}
		/* r == 0: gone since it was listed, so it includes nothing */
		settings_file = outer;
	}
	globfree(&found);
	return ok;
}

/* Parses text, which it keeps, and the files its include lines name, into
 * new settings, or returns NULL and adds each error to errors. With
 * partial, a line with an error is left out and the rest kept, unless a
 * keymap is bad, which would leave the keyboard typing nothing. */
static Settings *
settings_parse(char *text, struct json_object *errors, int partial)
{
	int keymaps_ok;
	Settings *s = ecalloc(1, sizeof(*s));
	SettingsVar *vars = NULL;
	size_t nvars = 0, i;
	int ok;

	s->text = text;
	s->focus_guard = -1;
	s->dim_inactive = -1;
	s->resize_grab = -1;
	s->new_window = -1;
	s->xwayland_scaling = -1;
	s->floating_mod = -1;
	settings_add_mode(s, "default");
	settings_file = NULL;
	ok = settings_parse_text(s, text, &vars, &nvars, 0, errors);
	keymaps_ok = settings_check_keymaps(s, errors);
	ok &= keymaps_ok;
	for (i = 0; i < nvars; i++) {
		free(vars[i].name);
		free(vars[i].value);
	}
	free(vars);
	if (!ok && !(partial && keymaps_ok)) {
		settings_free(s);
		return NULL;
	}
	return s;
}

/* Reads path into *text. Returns 1, 0 if there's no such file, or -1 on
 * an error, which it adds to errors. */
static int
settings_read(const char *path, char **text, struct json_object *errors)
{
	FILE *f = fopen(path, "r");
	size_t len;
	if (!f) {
		if (errno == ENOENT)
			return 0;
		settings_error(errors, 0, "can't open it: %s", strerror(errno));
		return -1;
	}
	*text = ecalloc(1, SETTINGS_MAX_SIZE + 1);
	len = fread(*text, 1, SETTINGS_MAX_SIZE + 1, f);
	if (ferror(f) || len > SETTINGS_MAX_SIZE) {
		settings_error(errors, 0, ferror(f) ? "can't read it" : "it's over 1 MiB");
		fclose(f);
		free(*text);
		return -1;
	}
	fclose(f);
	(*text)[len] = '\0';
	return 1;
}

/* Loads the file at settings_path in place of the current settings, or
 * keeps those and returns 0 if it has errors. No file means config.h's
 * keys. It returns -1 if it loaded, but an output refused its settings.
 * With partial, a file with errors in some lines loads without them, and
 * settings_errors says which. settings_errors gets this load's errors. */
static int
settings_load(int partial)
{
	static const LayoutDefaults no_defaults[LENGTH(settings_lt_modes)];
	struct json_object *errors = json_object_new_array();
	Settings *loaded = NULL;
	char *text;
	int refused, reset, r = settings_read(settings_path, &text, errors);
	if (r > 0)
		loaded = settings_parse(text, errors, partial);
	json_object_put(settings_errors);
	settings_errors = errors;
	if (r < 0 || (r > 0 && !loaded))
		return 0;
	/* Each layout's mfact and nmaster start again from new defaults, as a
	 * reload of tide's layouts has them on Hyprland; a load that keeps them,
	 * for another setting, leaves them as they are */
	reset = memcmp(settings ? settings->layout_defaults : no_defaults,
			loaded ? loaded->layout_defaults : no_defaults, sizeof(no_defaults)) != 0;
	settings_free(settings);
	settings = loaded;
	if (reset)
		lt_reset_modes();
	/* A notification's grant lapses by the new grant_seconds */
	fg_arm();
	/* Windows dim at the new strength */
	dim_update();
	/* X11 windows scale, or not, at once; with no file, they scale */
	x11_set_scaling(!settings || settings->xwayland_scaling != 0);
	/* Outputs and inputs take the new lines at once; at startup, there are
	 * none yet. One that refuses them is an error, but the rest stands */
	refused = settings_apply_outputs(NULL) < 0;
	refused |= settings_apply_inputs(NULL) < 0;
	/* As sway does, a load starts at the top level */
	if (settings_mode) {
		settings_mode = 0;
		ipc_event_mode("default");
	}
	return refused ? -1 : 1;
}

/* Finds the file: the -c argument, or $XDG_CONFIG_HOME/tidewm/config, or
 * ~/.config/tidewm/config. A line with an error is reported and left out,
 * and the rest of the file loads: a session that starts with config.h's
 * keys in place of yours, and without the shell your file starts, is no
 * way to find the line. A file that can't load at all, as one with a bad
 * keymap, is left out, and tidewm starts with config.h's keys. */
static void
settings_init(const char *arg)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	size_t i, nerrors;
	int n, loaded;
	if (arg)
		n = snprintf(settings_path, sizeof(settings_path), "%s", arg);
	else if (xdg && *xdg)
		n = snprintf(settings_path, sizeof(settings_path), "%s/tidewm/config", xdg);
	else
		n = snprintf(settings_path, sizeof(settings_path), "%s/.config/tidewm/config",
				home ? home : "");
	if (n < 0 || (size_t)n >= sizeof(settings_path))
		die("the config file's path is too long");
	settings_group_register(kb_group);
	loaded = settings_load(1);
	nerrors = json_object_array_length(settings_errors);
	for (i = 0; i < nerrors; i++)
		fprintf(stderr, "tidewm: %s\n",
				json_object_get_string(json_object_array_get_idx(settings_errors, i)));
	if (!loaded)
		fprintf(stderr, "tidewm: not using %s: config.h's keys work instead\n", settings_path);
	else if (nerrors)
		fprintf(stderr, "tidewm: left out the lines above from %s; the rest is loaded\n",
				settings_path);
}

/* The reload command: rereads the file. On errors, keeps what was loaded,
 * and puts the first few in buf. An output that refused its settings
 * fails it too, though the rest of the file is loaded. */
static int
settings_reload(char *buf, size_t size)
{
	size_t i, n, len = 0;
	int w, r;
	if ((r = settings_load(0)) > 0)
		return 1;
	n = json_object_array_length(settings_errors);
	buf[0] = '\0';
	for (i = 0; i < n && i < 3; i++) {
		w = snprintf(buf + len, size - len, "%s%s", i ? "; " : "",
				json_object_get_string(json_object_array_get_idx(settings_errors, i)));
		if (w < 0 || (size_t)w >= size - len)
			break;
		len += (size_t)w;
	}
	if (n > 3) {
		w = snprintf(buf + len, size - len, "; and %zu more", n - 3);
		len += w > 0 && (size_t)w < size - len ? (size_t)w : 0;
	}
	if (r < 0)
		snprintf(buf + len, size - len, "; the rest of the file is loaded");
	return 0;
}

/* GET_CONFIG's reply: sway's config, the loaded file's text, plus
 * tidewm's path and errors, from the last load. */
static struct json_object *
settings_json(void)
{
	struct json_object *obj = json_object_new_object();
	struct json_object *included = json_object_new_array();
	size_t i;
	json_object_object_add(obj, "config", json_object_new_string(settings ? settings->text : ""));
	/* As sway's: each file an include line read, as read */
	for (i = 0; settings && i < settings->nincludes; i++) {
		struct json_object *inc = json_object_new_object();
		json_object_object_add(inc, "path", json_object_new_string(settings->includes[i].path));
		json_object_object_add(inc, "raw_config", json_object_new_string(
				settings->includes[i].text ? settings->includes[i].text : ""));
		json_object_array_add(included, inc);
	}
	json_object_object_add(obj, "included_configs", included);
	json_object_object_add(obj, "path", json_object_new_string(settings_path));
	json_object_object_add(obj, "errors", json_object_get(settings_errors));
	return obj;
}

/* The loaded file's path, or "" with config.h's keys, for GET_VERSION. */
static const char *
settings_loaded_path(void)
{
	return settings ? settings_path : "";
}

/* The current mode's press or release binding for these keys, or NULL.
 * While the screen is locked, only a --locked one counts. A --kept-open
 * one counts only while the keyboard's in a window kept open, and then
 * before one for every window; otherwise the key goes to the window. */
static const Binding *
settings_find(uint32_t mods, xkb_keysym_t sym, int release)
{
	const Binding *found = NULL;
	const Mode *mode;
	size_t i;
	int kept = -1; /* not asked yet */
	if (!settings)
		return NULL;
	mode = &settings->modes[settings_mode];
	for (i = 0; i < mode->nbindings; i++) {
		const Binding *b = &mode->bindings[i];
		if (b->button || b->mods != CLEANMASK(mods) || b->sym != sym
				|| b->release != release || (locked && !b->locked))
			continue;
		if (!b->kept_open) {
			found = found ? found : b;
			continue;
		}
		if (kept < 0)
			kept = keyboard_kept_open();
		if (kept)
			return b;
	}
	return found;
}

/* Runs a binding's or a rule's commands, on window if it's given, and
 * writes each that fails to stderr, saying what it was for. The commands
 * may reload the settings, so they run from a copy. */
static void
settings_run(const char *commands, const char *what, Client *window)
{
	struct json_object *results, *result, *error;
	char *copy = settings_strdup(commands, strlen(commands));
	size_t i;
	int quit = 0;
	results = ipc_run_commands(copy, &quit, window);
	for (i = 0; i < json_object_array_length(results); i++) {
		result = json_object_array_get_idx(results, i);
		if (!json_object_get_boolean(json_object_object_get(result, "success"))
				&& json_object_object_get_ex(result, "error", &error))
			fprintf(stderr, "tidewm: %s: %s\n", what, json_object_get_string(error));
	}
	json_object_put(results);
	free(copy);
	if (quit)
		ipc_quit();
}

/* Runs the loaded file's press or release binding for these keys.
 * Returns whether it has one. */
static int
settings_run_binding(uint32_t mods, xkb_keysym_t sym, int release)
{
	const Binding *b = settings_find(mods, sym, release);
	char what[300];
	if (!b)
		return 0;
	snprintf(what, sizeof(what), "binding %s", b->keys);
	/* Said first, as the command may reload the file b is in */
	ipc_event_binding(b->command, b->mods, b->sym, NULL, "keyboard");
	focus_asked++;
	settings_run(b->command, what, NULL);
	focus_asked--;
	return 1;
}

/* Whether the loaded file says what the mouse does, by a floating_modifier
 * line or a --whole-window binding in any mode: then config.h's buttons
 * and scrolls are left out, as its keys are, and the file's are all there
 * are, as in sway. */
static int
settings_mouse(void)
{
	size_t i, j;
	if (!settings)
		return 0;
	if (settings->floating_mod >= 0)
		return 1;
	for (i = 0; i < settings->nmodes; i++)
		for (j = 0; j < settings->modes[i].nbindings; j++)
			if (settings->modes[i].bindings[j].button)
				return 1;
	/* A button's binding a rebind took away still said what the mouse does */
	for (i = 0; i < settings->nnamed; i++)
		if (settings->named[i].flags.whole_window)
			return 1;
	return 0;
}

/* Runs the current mode's --whole-window binding for a press of button, or
 * a scroll, with mods held. Returns whether it has one. Never while
 * locked, as no pointer event reaches a window then. */
static int
settings_run_button(uint32_t mods, uint32_t button)
{
	const Mode *mode;
	const Binding *b = NULL;
	char what[300], *command;
	const char *symbol = NULL;
	size_t i;
	if (!settings || locked)
		return 0;
	mode = &settings->modes[settings_mode];
	for (i = 0; i < mode->nbindings && !b; i++)
		if (mode->bindings[i].button == button && mode->bindings[i].mods == CLEANMASK(mods))
			b = &mode->bindings[i];
	if (!b)
		return 0;
	for (i = 0; i < LENGTH(settings_buttons) && !symbol; i++)
		if (settings_buttons[i].code == button)
			symbol = settings_buttons[i].name;
	snprintf(what, sizeof(what), "binding %s", b->keys);
	/* Copied first, as the command may reload the file b is in */
	command = settings_strdup(b->command, strlen(b->command));
	ipc_event_binding(command, b->mods, XKB_KEY_NoSymbol, symbol, "mouse");
	focus_asked++;
	settings_run(command, what, NULL);
	focus_asked--;
	free(command);
	return 1;
}

/* What a press of button with mods held does by the file's
 * floating_modifier: CurMove or CurResize, as dwl's buttons have them,
 * or CurNormal for nothing. Its modifiers count if they're all held, with
 * others or not, as sway's one does. */
static unsigned int
settings_floating_drag(uint32_t mods, uint32_t button)
{
	if (!settings || settings->floating_mod <= 0
			|| (CLEANMASK(mods) & (uint32_t)settings->floating_mod)
				!= (uint32_t)settings->floating_mod)
		return CurNormal;
	if (button == BTN_LEFT)
		return settings->floating_inverse ? CurResize : CurMove;
	if (button == BTN_RIGHT)
		return settings->floating_inverse ? CurMove : CurResize;
	return CurNormal;
}

/* A switch, such as the lid, turned on or off: runs the file's bindswitch
 * commands for it, each for its state or for either, and on the lock
 * screen only --locked ones */
static void
settings_run_switch(int type, int on)
{
	const Settings *loaded = settings;
	char *command, what[64];
	size_t i;
	for (i = 0; settings && settings == loaded && i < settings->nswitches; i++) {
		const SwitchBinding *b = &settings->switches[i];
		if (b->type != type || (b->state != SwToggle && b->state != (on ? SwOn : SwOff))
				|| (locked && !b->locked))
			continue;
		snprintf(what, sizeof(what), "bindswitch %s", type == WLR_SWITCH_TYPE_LID ? "lid" : "tablet");
		/* Copied first, as the command may reload the file b is in */
		command = settings_strdup(b->command, strlen(b->command));
		ipc_event_binding(command, 0, XKB_KEY_NoSymbol, NULL, "switch");
		settings_run(command, what, NULL);
		free(command);
	}
}

/* A window's value for a criterion, or NULL where it has none, which no
 * pattern matches: an X11 window has no app_id, a Wayland one no class. */
static const char *
settings_crit_value(Client *c, int crit)
{
#ifdef XWAYLAND
	if (client_is_x11(c)) {
		switch (crit) {
		case CritClass: return c->surface.xwayland->class;
		case CritInstance: return c->surface.xwayland->instance;
		case CritTitle: return c->surface.xwayland->title;
		case CritShell: return "xwayland";
		default: return NULL;
		}
	}
#endif
	switch (crit) {
	case CritAppId: return c->surface.xdg->toplevel->app_id;
	case CritTitle: return c->surface.xdg->toplevel->title;
	case CritShell: return "xdg_shell";
	default: return NULL;
	}
}

/* Whether PCRE2 pattern re, rule's criterion crit, matches anywhere in
 * value. A match PCRE2 can't finish (a value that isn't valid UTF-8, or a
 * limit reached) is no match, and is logged, naming the rule, so a rule
 * that never runs says why. */
static int
settings_matches(const pcre2_code *re, const char *value, size_t rule, int crit)
{
	pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
	char msg[160];
	int rc;
	if (!md)
		die("out of memory matching a for_window rule");
	rc = pcre2_match(re, (PCRE2_SPTR)value, PCRE2_ZERO_TERMINATED, 0, 0, md, NULL);
	pcre2_match_data_free(md);
	if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
		pcre2_get_error_message(rc, (PCRE2_UCHAR *)msg, sizeof(msg));
		fprintf(stderr, "tidewm: for_window rule %zu: couldn't match its %s: %s; its commands don't run\n",
				rule + 1, settings_crit_names[crit], msg);
	}
	return rc >= 0;
}

/* Runs each for_window rule whose criteria all match the new window c, in
 * the file's order, its commands acting on c. Before c's new event, so
 * that says where the rules put it. */
static void
settings_apply_rules(Client *c)
{
	const Settings *loaded = settings;
	const char *value;
	char what[64];
	size_t i;
	int crit;
	for (i = 0; settings && settings == loaded && i < settings->nrules; i++) {
		const WindowRule *r = &settings->rules[i];
		for (crit = 0; crit < CritLast; crit++)
			if (r->has[crit] && (!(value = settings_crit_value(c, crit))
					|| !settings_matches(r->re[crit], value, i, crit)))
				break;
		if (crit < CritLast)
			continue;
		snprintf(what, sizeof(what), "for_window rule %zu", i + 1);
		settings_run(r->command, what, c);
	}
}

/* Whether name names output o other than by its connector: as sway's
 * identifier does, "MAKE MODEL SERIAL" with Unknown for what it doesn't
 * say, or as Hyprland's description does, with nothing for that, trimmed
 * and without commas, which tide's settings use. */
static int
settings_output_described(const char *name, struct wlr_output *o)
{
	const char *parts[] = {o->make, o->model, o->serial};
	char id[512], desc[512], *from, *to;
	size_t start, end;
	snprintf(id, sizeof(id), "%s %s %s", parts[0] ? parts[0] : "Unknown",
			parts[1] ? parts[1] : "Unknown", parts[2] ? parts[2] : "Unknown");
	if (!strcmp(name, id))
		return 1;
	snprintf(desc, sizeof(desc), "%s %s %s", parts[0] ? parts[0] : "",
			parts[1] ? parts[1] : "", parts[2] ? parts[2] : "");
	for (start = 0; isspace((unsigned char)desc[start]); start++)
		;
	for (end = strlen(desc); end > start && isspace((unsigned char)desc[end - 1]); end--)
		;
	desc[end] = '\0';
	for (from = to = desc + start; *from; from++)
		if (*from != ',')
			*to++ = *from;
	*to = '\0';
	return desc[start] && !strcmp(name, desc + start);
}

/* Whether an output line or command naming name sets output o: "*", its
 * connector, or what it is. */
static int
settings_output_named(const char *name, struct wlr_output *o)
{
	return !strcmp(name, "*") || !strcmp(name, o->name) || settings_output_described(name, o);
}

/* The file's settings for output o: its "*" line, then the one naming its
 * connector, then one naming what it is, as sway lets each override the
 * one before. Returns 0 if none is there. */
static int
settings_output_config(struct wlr_output *o, OutputConfig *out)
{
	size_t i;
	int pass, found = 0;
	const char *name;
	memset(out, 0, sizeof(*out));
	out->enabled = -1;
	out->power = -1;
	for (pass = 0; settings && pass < 3; pass++) {
		for (i = 0; i < settings->noutputs; i++) {
			name = settings->outputs[i].name;
			if (pass == 0 ? strcmp(name, "*") : pass == 1 ? strcmp(name, o->name)
					: !strcmp(name, "*") || !strcmp(name, o->name)
					|| !settings_output_described(name, o))
				continue;
			settings_merge_output(out, &settings->outputs[i]);
			found = 1;
		}
	}
	return found;
}

/* Gives oc o's preferred mode, unless it has a mode. */
static void
settings_preferred_mode(struct wlr_output *o, OutputConfig *oc)
{
	struct wlr_output_mode *preferred = wlr_output_preferred_mode(o);
	if (oc->has_mode || !preferred)
		return;
	oc->has_mode = 1;
	oc->width = preferred->width;
	oc->height = preferred->height;
	oc->refresh = preferred->refresh;
}

/* Puts oc's mode, scale, transform, adaptive sync and enabling in state,
 * for monitor m. A mode its output lists is used if there is one, unless
 * it's --custom: with a rate, the closest within 1 Hz, so 59.94 Hz stays
 * apart from 60; without, the fastest. Otherwise it's a custom mode.
 * wlroots won't give an output that's off a mode, so settings_output_set()
 * keeps one for when it's enabled; one enabled without a mode takes that,
 * or keeps the one it had, which wlroots remembers, or, if it never had
 * one, its preferred. Adaptive sync the output can't have stays off, as
 * sway leaves it. */
/* Puts adaptive sync in state for m's output, if it changes it: one the
 * output can't have, or a backend that takes no adaptive sync at all, as
 * a nested session's, leaves it as it is */
static void
settings_adaptive_sync(Monitor *m, int enabled, struct wlr_output_state *state)
{
	struct wlr_output *o = m->wlr_output;
	if (!enabled == (o->adaptive_sync_status != WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED))
		return;
	if (enabled && !o->adaptive_sync_supported)
		return;
	wlr_output_state_set_adaptive_sync_enabled(state, enabled);
	if (!wlr_output_test_state(o, state))
		state->committed &= ~WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
}

static void
settings_output_state(Monitor *m, const OutputConfig *oc, struct wlr_output_state *state)
{
	struct wlr_output *o = m->wlr_output;
	struct wlr_output_mode *mode, *best = NULL;
	OutputConfig want = *oc;
	int on = oc->enabled >= 0 ? oc->enabled : o->enabled;
	if (!on) {
		want.has_mode = 0;
	} else if (!want.has_mode && !o->enabled) {
		if ((want.has_mode = m->want_mode)) {
			want.width = m->want_width;
			want.height = m->want_height;
			want.refresh = m->want_refresh;
			want.custom = m->want_custom;
		} else if (!o->width || !o->height) {
			settings_preferred_mode(o, &want);
		}
	}
	if (want.has_mode && !want.custom) {
		wl_list_for_each(mode, &o->modes, link) {
			if (mode->width != want.width || mode->height != want.height
					|| (want.refresh && abs(mode->refresh - want.refresh) > 1000))
				continue;
			if (!best || (want.refresh
					? abs(mode->refresh - want.refresh) < abs(best->refresh - want.refresh)
					: mode->refresh > best->refresh))
				best = mode;
		}
	}
	if (best)
		wlr_output_state_set_mode(state, best);
	else if (want.has_mode)
		wlr_output_state_set_custom_mode(state, want.width, want.height, want.refresh);
	if (oc->has_scale)
		wlr_output_state_set_scale(state, oc->scale);
	if (oc->has_transform)
		wlr_output_state_set_transform(state, oc->transform);
	if (oc->enabled >= 0)
		wlr_output_state_set_enabled(state, oc->enabled);
	if (oc->has_adaptive_sync && on)
		settings_adaptive_sync(m, oc->adaptive_sync, state);
	else if (on && m->want_has_adaptive_sync)
		settings_adaptive_sync(m, m->want_adaptive_sync, state);
}

/* Once m's output has taken oc's settings: keeps the mode and position oc
 * gives for when it's enabled, if it's off, and where it goes in the
 * layout. With reset, as on reload, one oc doesn't give is forgotten. An
 * output enabled or disabled as asked is no longer asleep, as an idle
 * daemon's power management leaves it, so updatemons() treats it as off. */
static void
settings_output_set(Monitor *m, const OutputConfig *oc, int reset)
{
	if (oc->enabled >= 0)
		m->asleep = 0;
	if (m->wlr_output->enabled) {
		m->want_mode = 0;
	} else if (oc->has_mode || reset) {
		m->want_mode = oc->has_mode;
		m->want_width = oc->width;
		m->want_height = oc->height;
		m->want_refresh = oc->refresh;
		m->want_custom = oc->custom;
	}
	if (m->wlr_output->enabled) {
		m->want_has_adaptive_sync = 0;
	} else if (oc->has_adaptive_sync || reset) {
		m->want_has_adaptive_sync = oc->has_adaptive_sync;
		m->want_adaptive_sync = oc->adaptive_sync;
	}
	if (oc->has_position) {
		m->want_place = oc->place;
		m->want_x = oc->x;
		m->want_y = oc->y;
	} else if (reset) {
		m->want_place = PlaceRight;
	}
}

/* Puts m's output in the layout: where it's put, or, if it isn't there
 * already, where wlroots would until settings_place_outputs() puts it
 * beside the others. */
static void
settings_output_place(Monitor *m)
{
	if (m->want_place == PlaceAt)
		wlr_output_layout_add(output_layout, m->wlr_output, m->want_x, m->want_y);
	else if (!wlr_output_layout_get(output_layout, m->wlr_output))
		wlr_output_layout_add_auto(output_layout, m->wlr_output);
}

/* Set while settings_place_outputs() moves outputs, so the layout's change
 * events wait for the updatemons() that called it. */
static int settings_placing;

/* Places each output in the layout that isn't put at a position, as
 * Hyprland arranges monitors (src/state/MonitorPositionController.cpp):
 * in the order they were connected, each beside the outputs put at
 * positions and those it placed before, and the origin, on the side it
 * asks for, at 0 across. Another output's size changing, or one going,
 * moves those beside it. */
static void
settings_place_outputs(void)
{
	struct wlr_output_layout_output *lo;
	struct wlr_box b;
	int left = 0, right = 0, top = 0, bottom = 0, x, y, pass;
	Monitor *m;
	settings_placing = 1;
	for (pass = 0; pass < 2; pass++) {
		wl_list_for_each_reverse(m, &mons, link) {
			if ((m->want_place == PlaceAt) != !pass
					|| !(lo = wlr_output_layout_get(output_layout, m->wlr_output)))
				continue;
			wlr_output_layout_get_box(output_layout, m->wlr_output, &b);
			if (pass) {
				x = m->want_place == PlaceLeft ? left - b.width
					: m->want_place == PlaceRight ? right : 0;
				y = m->want_place == PlaceUp ? top - b.height
					: m->want_place == PlaceDown ? bottom : 0;
				/* One that's asleep, which is off, stays put */
				if (m->wlr_output->enabled && (lo->auto_configured || lo->x != x || lo->y != y))
					wlr_output_layout_add(output_layout, m->wlr_output, x, y);
				b.x = lo->x;
				b.y = lo->y;
			}
			left = MIN(left, b.x);
			right = MAX(right, b.x + b.width);
			top = MIN(top, b.y);
			bottom = MAX(bottom, b.y + b.height);
		}
	}
	settings_placing = 0;
}

/* An output's settings before the file's: config.h's monitor rule's scale,
 * transform and position, and enabled. */
static void
settings_output_defaults(Monitor *m, OutputConfig *oc)
{
	const MonitorRule *r;
	memset(oc, 0, sizeof(*oc));
	oc->enabled = 1;
	oc->power = -1;
	oc->has_adaptive_sync = 1; /* off, as sway's */
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(m->wlr_output->name, r->name)) {
			oc->has_scale = oc->has_transform = 1;
			oc->scale = r->scale;
			oc->transform = r->rr;
			if ((oc->has_position = r->x != -1 || r->y != -1)) {
				oc->place = PlaceAt;
				oc->x = r->x;
				oc->y = r->y;
			}
			break;
		}
	}
}

/* Turns m's output off, as an idle daemon does, keeping its windows and
 * place, or on again. Returns whether it took it. */
static int
settings_monitor_power(Monitor *m, int on)
{
	struct wlr_output_state state;
	int ok;
	if (on != m->asleep)
		return 1;
	wlr_output_state_init(&state);
	m->gamma_lut_changed = 1; /* reapplied once it's on again */
	wlr_output_state_set_enabled(&state, on);
	ok = wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);
	if (ok)
		m->asleep = !on;
	return ok;
}

/* Sets only on the outputs it names, as the output command does. Without
 * it, as on reload, each output starts again from its defaults and its
 * preferred mode, then takes the file's lines, so nothing the command set
 * outlives a reload. An output enabled goes where it was last placed, if
 * it was. The command's power or dpms then turns an output that's on, or
 * asleep, off or on; the file's are taken but left, as an output starts
 * on. Returns how many outputs took their settings, or -1 if any refused
 * them, which it says on stderr, and, on reload, in settings_errors. */
static int
settings_apply_outputs(const OutputConfig *only)
{
	struct wlr_output_state state;
	OutputConfig oc, file;
	Monitor *m;
	int set = 0, refused = 0;
	wl_list_for_each(m, &mons, link) {
		if (only) {
			if (!settings_output_named(only->name, m->wlr_output))
				continue;
			oc = *only;
		} else {
			settings_output_defaults(m, &oc);
			if (settings_output_config(m->wlr_output, &file))
				settings_merge_output(&oc, &file);
			settings_preferred_mode(m->wlr_output, &oc);
		}
		wlr_output_state_init(&state);
		/* Powered on again, as power on wakes it, it takes the rest too */
		if (only && oc.power == 1 && m->asleep && oc.enabled < 0) {
			oc.enabled = 1;
			m->gamma_lut_changed = 1;
		}
		settings_output_state(m, &oc, &state);
		if (wlr_output_commit_state(m->wlr_output, &state)) {
			set++;
			settings_output_set(m, &oc, !only);
			if (m->wlr_output->enabled)
				settings_output_place(m);
			if (only && oc.power == 0 && m->wlr_output->enabled && !settings_monitor_power(m, 0))
				refused = 1;
		} else {
			fprintf(stderr, "tidewm: output %s refused its settings\n", m->wlr_output->name);
			if (!only)
				settings_error(settings_errors, 0, "output %s refused its settings, "
						"so it keeps what it had", m->wlr_output->name);
			refused = 1;
		}
		wlr_output_state_finish(&state);
	}
	if (set) {
		updatemons(NULL, NULL);
		ipc_event_output();
	}
	return refused ? -1 : set;
}

/* The output command's "NAME power on|off|toggle", alone, as sway's, or
 * dpms, its old name: turns the outputs NAME names off, as an idle daemon
 * does over wlr-output-power-management, keeping their windows and
 * places, or on again. An output its settings disable stays off. Returns -1 for an
 * output command of any other kind, else as settings_output_command. */
static int
settings_output_power(const char *args, char *err, size_t errsize, int *parse)
{
	char buf[1024], *rest = buf, *name, *key, *word;
	int on, named = 0, found = 0, refused = 0;
	Monitor *m;
	if (strlen(args) >= sizeof(buf))
		return -1;
	strcpy(buf, args);
	if (!(name = settings_arg(&rest)) || !*name)
		return -1;
	key = settings_word(&rest);
	if (strcmp(key, "power") && strcmp(key, "dpms"))
		return -1;
	word = settings_word(&rest);
	/* With other settings, it's one of them, as settings_apply_outputs()
	 * takes it */
	if (*rest && settings_on_off(word) >= 0)
		return -1;
	if (*rest || (strcmp(word, "on") && strcmp(word, "off") && strcmp(word, "toggle"))) {
		snprintf(err, errsize, "output %s %s takes on, off or toggle", name, key);
		*parse = 1;
		return 0;
	}
	*parse = 0;
	wl_list_for_each(m, &mons, link) {
		if (!settings_output_named(name, m->wlr_output))
			continue;
		named = 1;
		if (!m->wlr_output->enabled && !m->asleep)
			continue;
		found = 1;
		on = !strcmp(word, "toggle") ? m->asleep : !strcmp(word, "on");
		if (!settings_monitor_power(m, on))
			refused = 1;
	}
	if (found) {
		updatemons(NULL, NULL);
		ipc_event_output();
	}
	if (found && !refused)
		return 1;
	if (found)
		snprintf(err, errsize, "An output refused to power %s", word);
	else if (named)
		snprintf(err, errsize, "Output %s is disabled, so it has no power to turn on or off", name);
	else
		snprintf(err, errsize, "No output is called %s", name);
	return 0;
}

/* The output command, "NAME SETTING...": sets them now, on the outputs
 * NAME names, "*" for all, without keeping them past a reload; or
 * settings_output_power's "NAME power on|off|toggle". Returns 1, or 0 with
 * err saying why; *parse says it didn't parse. */
static int
settings_output_command(char *args, char *err, size_t errsize, int *parse)
{
	OutputConfig oc;
	int set;
	if ((set = settings_output_power(args, err, errsize, parse)) >= 0)
		return set;
	*parse = !settings_parse_output(&oc, args, err, errsize);
	if (*parse)
		return 0;
	if ((set = settings_apply_outputs(&oc)) > 0)
		return 1;
	snprintf(err, errsize, set < 0 ? "An output refused those settings"
			: "No output is called %s", oc.name);
	return 0;
}

/* An input device tidewm knows: a keyboard or pointer, its own or a
 * client's virtual one */
typedef struct {
	struct wl_list link;
	struct wlr_input_device *dev;
	char *identifier; /* VENDOR:PRODUCT:NAME, as sway makes it */
	char *hypr_name; /* its name as Hyprland gives it, which tide's settings use */
	const char *type; /* "keyboard", "pointer" or "touchpad" */
	int is_virtual; /* a client's: it keeps its own keymap, and has no libinput */
	KeyboardGroup *group; /* a keyboard's, unless it's virtual */
	const char *names[InXkbLast]; /* a keyboard's XKB names now, its own copies */
	int rate, delay;
	double scroll_factor; /* a pointer's scroll times this */
	struct wl_listener destroy;
} SettingsInput;

/* Whether the name of an input line or command, other than "*" or a type:,
 * names in: its identifier, or its name as Hyprland gives it */
static int
settings_input_is(const SettingsInput *in, const char *name)
{
	return !strcmp(name, in->identifier) || !strcmp(name, in->hypr_name);
}

/* A keyboard group for keyboards with the same keymap and repeat info, as
 * sway's smart grouping makes them: kb_group, config.h's, is one, which
 * stays; tidewm makes the others as keyboards need them. */
typedef struct {
	struct wl_list link;
	KeyboardGroup *group;
} SettingsGroup;

static struct wl_list settings_inputs = {&settings_inputs, &settings_inputs};
static struct wl_list settings_groups = {&settings_groups, &settings_groups};

/* The word setting i's value is, as GET_INPUTS says it */
static const char *
settings_input_word(int i, int value)
{
	size_t j;
	for (j = 0; j < LENGTH(settings_input_settings[i].words)
			&& settings_input_settings[i].words[j]; j++)
		if (settings_input_settings[i].values[j] == value)
			return settings_input_settings[i].words[j];
	return "unknown";
}

static struct libinput_device *
settings_libinput_device(const SettingsInput *in)
{
	return !in->is_virtual && wlr_input_device_is_libinput(in->dev)
		? wlr_libinput_get_device_handle(in->dev) : NULL;
}

/* kb's active layout, as XKB has it */
static xkb_layout_index_t
settings_active_layout(const struct wlr_keyboard *kb)
{
	return kb->xkb_state ? xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE)
			: 0;
}

/* An input in GET_INPUTS and the input event, with sway's fields */
static struct json_object *
settings_json_input(const SettingsInput *in)
{
	struct json_object *obj = json_object_new_object(), *names, *li;
	struct libinput_device *d = settings_libinput_device(in);
	struct wlr_keyboard *kb;
	xkb_layout_index_t i, n, active;
	const char *name;
	json_object_object_add(obj, "identifier", json_object_new_string(in->identifier));
	json_object_object_add(obj, "hyprland_name", json_object_new_string(in->hypr_name));
	json_object_object_add(obj, "name", json_object_new_string(in->dev->name ? in->dev->name : ""));
	json_object_object_add(obj, "vendor", json_object_new_int(d ? (int)libinput_device_get_id_vendor(d) : 0));
	json_object_object_add(obj, "product", json_object_new_int(d ? (int)libinput_device_get_id_product(d) : 0));
	json_object_object_add(obj, "type", json_object_new_string(in->type));
	/* sway's, for a pointer or touchpad */
	if (strcmp(in->type, "keyboard"))
		json_object_object_add(obj, "scroll_factor", json_object_new_double(in->scroll_factor));
	if (!strcmp(in->type, "keyboard")) {
		kb = in->group ? &in->group->wlr_group->keyboard : wlr_keyboard_from_input_device(in->dev);
		n = kb->keymap ? xkb_keymap_num_layouts(kb->keymap) : 0;
		active = settings_active_layout(kb);
		names = json_object_new_array();
		for (i = 0; i < n; i++) {
			name = xkb_keymap_layout_get_name(kb->keymap, i);
			json_object_array_add(names, json_object_new_string(name ? name : ""));
		}
		json_object_object_add(obj, "xkb_layout_names", names);
		json_object_object_add(obj, "xkb_active_layout_index", json_object_new_int((int)active));
		name = active < n ? xkb_keymap_layout_get_name(kb->keymap, active) : NULL;
		json_object_object_add(obj, "xkb_active_layout_name", name ? json_object_new_string(name) : NULL);
		json_object_object_add(obj, "repeat_delay", json_object_new_int(kb->repeat_info.delay));
		json_object_object_add(obj, "repeat_rate", json_object_new_int(kb->repeat_info.rate));
	}
	if (!d)
		return obj;
	li = json_object_new_object();
	json_object_object_add(li, "send_events", json_object_new_string(
			settings_input_word(InEvents, (int)libinput_device_config_send_events_get_mode(d))));
	if (libinput_device_config_tap_get_finger_count(d)) {
		json_object_object_add(li, "tap", json_object_new_string(settings_input_word(InTap,
				libinput_device_config_tap_get_enabled(d))));
		json_object_object_add(li, "tap_button_map", json_object_new_string(settings_input_word(
				InTapButtonMap, libinput_device_config_tap_get_button_map(d))));
		json_object_object_add(li, "tap_drag", json_object_new_string(settings_input_word(InDrag,
				libinput_device_config_tap_get_drag_enabled(d))));
		json_object_object_add(li, "tap_drag_lock", json_object_new_string(settings_input_word(
				InDragLock, libinput_device_config_tap_get_drag_lock_enabled(d))));
	}
	if (libinput_device_config_accel_is_available(d)) {
		json_object_object_add(li, "accel_speed",
				json_object_new_double(libinput_device_config_accel_get_speed(d)));
		json_object_object_add(li, "accel_profile", json_object_new_string(settings_input_word(
				InAccelProfile, libinput_device_config_accel_get_profile(d))));
	}
	if (libinput_device_config_scroll_has_natural_scroll(d))
		json_object_object_add(li, "natural_scroll", json_object_new_string(settings_input_word(
				InNaturalScroll, libinput_device_config_scroll_get_natural_scroll_enabled(d))));
	if (libinput_device_config_left_handed_is_available(d))
		json_object_object_add(li, "left_handed", json_object_new_string(settings_input_word(
				InLeftHanded, libinput_device_config_left_handed_get(d))));
	if (libinput_device_config_click_get_methods(d) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
		json_object_object_add(li, "click_method", json_object_new_string(settings_input_word(
				InClickMethod, libinput_device_config_click_get_method(d))));
	if (libinput_device_config_middle_emulation_is_available(d))
		json_object_object_add(li, "middle_emulation", json_object_new_string(settings_input_word(
				InMiddleEmulation, libinput_device_config_middle_emulation_get_enabled(d))));
	if (libinput_device_config_scroll_get_methods(d) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
		json_object_object_add(li, "scroll_method", json_object_new_string(settings_input_word(
				InScrollMethod, libinput_device_config_scroll_get_method(d))));
	if (libinput_device_config_dwt_is_available(d))
		json_object_object_add(li, "dwt", json_object_new_string(settings_input_word(InDwt,
				libinput_device_config_dwt_get_enabled(d))));
	json_object_object_add(obj, "libinput", li);
	return obj;
}

/* GET_INPUTS's reply: each input device, in the order they came */
static struct json_object *
settings_json_inputs(void)
{
	struct json_object *arr = json_object_new_array();
	SettingsInput *in;
	wl_list_for_each(in, &settings_inputs, link)
		json_object_array_add(arr, settings_json_input(in));
	return arr;
}

/* sway's input event: change is added, removed, xkb_keymap or
 * libinput_config */
static void
settings_event_input(const char *change, const SettingsInput *in)
{
	struct json_object *obj;
	if (!ipc_subscribed(IpcEvInput))
		return;
	obj = json_object_new_object();
	json_object_object_add(obj, "change", json_object_new_string(change));
	json_object_object_add(obj, "input", settings_json_input(in));
	ipc_broadcast(IpcEvInput, obj);
}

/* A device's identifier, as sway makes it: VENDOR:PRODUCT:NAME, the
 * numbers 0 for a device without libinput, and in the name, which loses
 * space at either end, _ for each space or unprintable character. */
static char *
settings_identifier(struct wlr_input_device *dev)
{
	struct libinput_device *d = wlr_input_device_is_libinput(dev)
		? wlr_libinput_get_device_handle(dev) : NULL;
	const char *name = dev->name ? dev->name : "";
	unsigned int vendor = d ? libinput_device_get_id_vendor(d) : 0;
	unsigned int product = d ? libinput_device_get_id_product(d) : 0;
	size_t len;
	char *id, *p;
	int n;
	while (isspace((unsigned char)*name))
		name++;
	len = strlen(name);
	while (len && isspace((unsigned char)name[len - 1]))
		len--;
	n = snprintf(NULL, 0, "%u:%u:%.*s", vendor, product, (int)len, name);
	id = ecalloc(1, (size_t)n + 1);
	snprintf(id, (size_t)n + 1, "%u:%u:%.*s", vendor, product, (int)len, name);
	for (p = id + (size_t)n - len; *p; p++)
		if (*p == ' ' || !isprint((unsigned char)*p))
			*p = '_';
	return id;
}

/* The file's settings for in: "*"'s, then its type's, then its own,
 * each over the last, over config.h's. out borrows their XKB names. */
static void
settings_input_config(const SettingsInput *in, InputConfig *out)
{
	char type[32];
	size_t i;
	int pass;
	settings_input_init(out, NULL);
	settings_xkb_base(out->xkb);
	out->value[InRepeatRate] = repeat_rate;
	out->value[InRepeatDelay] = repeat_delay;
	out->value[InTap] = tap_to_click;
	out->value[InTapButtonMap] = (int)button_map;
	out->value[InDrag] = tap_and_drag;
	out->value[InDragLock] = drag_lock;
	out->value[InNaturalScroll] = natural_scrolling;
	out->value[InDwt] = disable_while_typing;
	out->value[InLeftHanded] = left_handed;
	out->value[InMiddleEmulation] = middle_button_emulation;
	out->value[InScrollMethod] = (int)scroll_method;
	out->value[InClickMethod] = (int)click_method;
	out->value[InAccelProfile] = (int)accel_profile;
	out->value[InEvents] = (int)send_events_mode;
	out->has_accel = 1;
	out->accel = accel_speed;
	out->has_scroll_factor = 1;
	out->scroll_factor = 1;
	snprintf(type, sizeof(type), "type:%s", in->type);
	for (pass = 0; settings && pass < 3; pass++)
		for (i = 0; i < settings->ninputs; i++)
			if (pass == 0 ? !strcmp(settings->inputs[i].name, "*")
					: pass == 1 ? !strcmp(settings->inputs[i].name, type)
					: settings_input_is(in, settings->inputs[i].name))
				settings_merge_input(out, &settings->inputs[i]);
}

/* Puts each setting ic gives on in's libinput device, where the device
 * has it. Returns 0 if in has no libinput device. */
static int
settings_libinput(const SettingsInput *in, const InputConfig *ic)
{
	struct libinput_device *d = settings_libinput_device(in);
	const int *v = ic->value;
	if (!d)
		return 0;
	if (libinput_device_config_tap_get_finger_count(d)) {
		if (v[InTap] >= 0)
			libinput_device_config_tap_set_enabled(d, v[InTap]);
		if (v[InDrag] >= 0)
			libinput_device_config_tap_set_drag_enabled(d, v[InDrag]);
		if (v[InDragLock] >= 0)
			libinput_device_config_tap_set_drag_lock_enabled(d, v[InDragLock]);
		if (v[InTapButtonMap] >= 0)
			libinput_device_config_tap_set_button_map(d, v[InTapButtonMap]);
	}
	if (v[InNaturalScroll] >= 0 && libinput_device_config_scroll_has_natural_scroll(d))
		libinput_device_config_scroll_set_natural_scroll_enabled(d, v[InNaturalScroll]);
	if (v[InDwt] >= 0 && libinput_device_config_dwt_is_available(d))
		libinput_device_config_dwt_set_enabled(d, v[InDwt]);
	if (v[InLeftHanded] >= 0 && libinput_device_config_left_handed_is_available(d))
		libinput_device_config_left_handed_set(d, v[InLeftHanded]);
	if (v[InMiddleEmulation] >= 0 && libinput_device_config_middle_emulation_is_available(d))
		libinput_device_config_middle_emulation_set_enabled(d, v[InMiddleEmulation]);
	if (v[InScrollMethod] >= 0
			&& libinput_device_config_scroll_get_methods(d) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
		libinput_device_config_scroll_set_method(d, v[InScrollMethod]);
	if (v[InClickMethod] >= 0
			&& libinput_device_config_click_get_methods(d) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
		libinput_device_config_click_set_method(d, v[InClickMethod]);
	if (v[InEvents] >= 0 && libinput_device_config_send_events_get_modes(d))
		libinput_device_config_send_events_set_mode(d, (uint32_t)v[InEvents]);
	if (libinput_device_config_accel_is_available(d)) {
		if (v[InAccelProfile] >= 0)
			libinput_device_config_accel_set_profile(d, v[InAccelProfile]);
		if (ic->has_accel)
			libinput_device_config_accel_set_speed(d, ic->accel);
	}
	return 1;
}

static void
settings_group_register(KeyboardGroup *group)
{
	SettingsGroup *g = ecalloc(1, sizeof(*g));
	g->group = group;
	wl_list_insert(settings_groups.prev, &g->link);
}

static struct wl_event_source *settings_release_source;

/* Destroys each group no keyboard is in, but config.h's. The seat's
 * keyboard, if that went with one, becomes a group still in use. */
static void
settings_release_groups(void *data)
{
	SettingsGroup *g, *tmp;
	settings_release_source = NULL;
	wl_list_for_each_safe(g, tmp, &settings_groups, link) {
		if (g->group == kb_group || !wl_list_empty(&g->group->wlr_group->devices))
			continue;
		wl_list_remove(&g->link);
		destroykeyboardgroup(&g->group->destroy, NULL);
		free(g);
	}
	if (seat->keyboard_state.keyboard)
		return;
	wl_list_for_each(g, &settings_groups, link) {
		if (!wl_list_empty(&g->group->wlr_group->devices)) {
			wlr_seat_set_keyboard(seat, &g->group->wlr_group->keyboard);
			return;
		}
	}
	wlr_seat_set_keyboard(seat, &kb_group->wlr_group->keyboard);
}

/* Destroys group once no keyboard is in it, unless it's config.h's, when
 * tidewm is next idle: a key binding can empty the group whose key
 * keypress() is still handling. */
static void
settings_group_release(KeyboardGroup *group)
{
	if (!group || group == kb_group || !wl_list_empty(&group->wlr_group->devices)
			|| settings_release_source)
		return;
	settings_release_source = wl_event_loop_add_idle(event_loop, settings_release_groups, NULL);
}

/* Puts keyboard in into the group for these XKB names and repeat info,
 * making one if no keyboard has them yet, and keeps them as in's own.
 * Returns 0, leaving it as it was, if XKB can't make their keymap. */
static int
settings_keyboard_set(SettingsInput *in, const char *const names[InXkbLast], int rate, int delay)
{
	struct wlr_keyboard *kb = wlr_keyboard_from_input_device(in->dev), *gk;
	struct xkb_keymap *keymap = settings_keymap(names, 0);
	KeyboardGroup *group = NULL, *old = in->group;
	SettingsGroup *g;
	const char *copy[InXkbLast];
	size_t i;
	if (!keymap)
		return 0;
	wl_list_for_each(g, &settings_groups, link) {
		gk = &g->group->wlr_group->keyboard;
		if (wlr_keyboard_keymaps_match(gk->keymap, keymap)
				&& gk->repeat_info.rate == rate && gk->repeat_info.delay == delay) {
			group = g->group;
			break;
		}
	}
	if (!group) {
		group = createkeyboardgroup();
		wl_list_init(&group->destroy.link);
		wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
		wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, rate, delay);
		settings_group_register(group);
	}
	xkb_keymap_unref(keymap);
	/* names may be in's own: copy them before letting those go */
	for (i = 0; i < InXkbLast; i++)
		copy[i] = names[i] ? settings_strdup(names[i], strlen(names[i])) : NULL;
	for (i = 0; i < InXkbLast; i++) {
		free((char *)in->names[i]);
		in->names[i] = copy[i];
	}
	in->rate = rate;
	in->delay = delay;
	if (group == old)
		return 1;
	if (old && kb->group)
		wlr_keyboard_group_remove_keyboard(old->wlr_group, kb);
	wlr_keyboard_set_keymap(kb, group->wlr_group->keyboard.keymap);
	wlr_keyboard_set_repeat_info(kb, rate, delay);
	wlr_keyboard_group_add_keyboard(group->wlr_group, kb);
	in->group = group;
	settings_group_release(old);
	/* The focused window gets the keymap now, not at the next key */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	if (old)
		settings_event_input("xkb_keymap", in);
	return 1;
}

static void
settings_input_destroyed(struct wl_listener *listener, void *data)
{
	SettingsInput *in = wl_container_of(listener, in, destroy);
	struct wlr_keyboard *kb;
	size_t i;
	settings_event_input("removed", in);
	if (in->group) {
		kb = wlr_keyboard_from_input_device(in->dev);
		if (kb->group)
			wlr_keyboard_group_remove_keyboard(kb->group, kb);
		settings_group_release(in->group);
	}
	wl_list_remove(&in->destroy.link);
	wl_list_remove(&in->link);
	for (i = 0; i < InXkbLast; i++)
		free((char *)in->names[i]);
	free(in->identifier);
	free(in->hypr_name);
	free(in);
}

/* dev's name as Hyprland gives a device one
 * (src/helpers/MiscFunctions.cpp's deviceNameToInternalString and
 * CInputManager::getNameForNewDevice): its name in lower case, with a
 * space, newline or comma a -, or unknown-device for none, then -1, -2 and
 * so on after it for each device already here with the same, as tide's
 * input settings name a device. */
static char *
settings_hypr_name(struct wlr_input_device *dev)
{
	const char *name = dev->name ? dev->name : "";
	size_t len = strlen(name), i;
	char *base = ecalloc(1, len + sizeof("unknown-device")), *out;
	SettingsInput *in;
	int n = 0, taken;
	for (i = 0; i < len; i++)
		base[i] = name[i] == ' ' || name[i] == '\n' || name[i] == ','
			? '-' : (char)tolower((unsigned char)name[i]);
	if (!len)
		strcpy(base, "unknown-device");
	out = ecalloc(1, strlen(base) + 16);
	do {
		if (n)
			sprintf(out, "%s-%d", base, n);
		else
			strcpy(out, base);
		taken = 0;
		wl_list_for_each(in, &settings_inputs, link)
			taken |= !strcmp(in->hypr_name, out);
		n++;
	} while (taken);
	free(base);
	return out;
}

static SettingsInput *
settings_track(struct wlr_input_device *dev, const char *type, int is_virtual)
{
	SettingsInput *in = ecalloc(1, sizeof(*in));
	in->dev = dev;
	in->identifier = settings_identifier(dev);
	in->hypr_name = settings_hypr_name(dev);
	in->type = type;
	in->is_virtual = is_virtual;
	in->scroll_factor = 1;
	LISTEN(&dev->events.destroy, &in->destroy, settings_input_destroyed);
	wl_list_insert(settings_inputs.prev, &in->link);
	return in;
}

/* A new keyboard: into the group for its settings, config.h's and the
 * file's input lines, or config.h's alone if XKB can't make their
 * keymap, which stderr says. */
static void
settings_keyboard_added(struct wlr_keyboard *keyboard)
{
	SettingsInput *in = settings_track(&keyboard->base, "keyboard", 0);
	InputConfig ic;
	const char *names[InXkbLast];
	settings_input_config(in, &ic);
	if (!settings_keyboard_set(in, ic.xkb, ic.value[InRepeatRate], ic.value[InRepeatDelay])) {
		fprintf(stderr, "tidewm: XKB can't make a keymap of %s's settings; "
				"it has config.h's\n", in->identifier);
		settings_xkb_base(names);
		settings_keyboard_set(in, names, repeat_rate, repeat_delay);
	}
	/* A libinput keyboard has events, as a pointer does */
	settings_libinput(in, &ic);
	settings_event_input("added", in);
}

/* A new pointer: its libinput settings, config.h's and the file's. A
 * libinput device that can tap is a touchpad. */
static void
settings_pointer_added(struct wlr_pointer *pointer)
{
	struct wlr_input_device *dev = &pointer->base;
	struct libinput_device *d = wlr_input_device_is_libinput(dev)
		? wlr_libinput_get_device_handle(dev) : NULL;
	SettingsInput *in = settings_track(dev, d && libinput_device_config_tap_get_finger_count(d)
			? "touchpad" : "pointer", 0);
	InputConfig ic;
	settings_input_config(in, &ic);
	in->scroll_factor = ic.scroll_factor;
	settings_libinput(in, &ic);
	settings_event_input("added", in);
}

/* A client's virtual keyboard or pointer, which GET_INPUTS lists, but
 * which keeps its own keymap and has no libinput settings. A pointer's
 * scroll factor is tidewm's, so it has that. */
static void
settings_virtual_added(struct wlr_input_device *dev)
{
	SettingsInput *in = settings_track(dev,
			dev->type == WLR_INPUT_DEVICE_KEYBOARD ? "keyboard" : "pointer", 1);
	InputConfig ic;
	settings_input_config(in, &ic);
	in->scroll_factor = ic.scroll_factor;
	settings_event_input("added", in);
}

/* How much dev's scrolls count, 1 unless its input settings say */
static double
settings_scroll_factor(struct wlr_input_device *dev)
{
	SettingsInput *in;
	wl_list_for_each(in, &settings_inputs, link)
		if (in->dev == dev)
			return in->scroll_factor;
	return 1;
}

/* Whether keycode is down on kb. A group's key is until its keyboard
 * leaves the group, as a binding can make it, when wlroots lets it go. */
static int
settings_key_held(const struct wlr_keyboard *kb, uint32_t keycode)
{
	size_t i;
	for (i = 0; i < kb->num_keycodes; i++)
		if (kb->keycodes[i] == keycode)
			return 1;
	return 0;
}

/* Whether a keyboard of tidewm's own is there, for the seat's
 * capabilities */
static int
settings_has_keyboard(void)
{
	SettingsInput *in;
	wl_list_for_each(in, &settings_inputs, link)
		if (in->group)
			return 1;
	return 0;
}

/* Sets on the devices only names, as the input command does, its settings
 * over what they have. Without it, as on reload, each device starts again
 * from config.h's and takes the file's. Returns how many devices it named,
 * or -1 if XKB couldn't make a keymap for a keyboard, which keeps the one
 * it had, as stderr and, on reload, settings_errors say. */
static int
settings_apply_inputs(const InputConfig *only)
{
	SettingsInput *in;
	InputConfig ic;
	const char *names[InXkbLast];
	char type[32];
	size_t i;
	int n = 0, refused = 0, rate, delay;
	wl_list_for_each(in, &settings_inputs, link) {
		if (only) {
			snprintf(type, sizeof(type), "type:%s", in->type);
			if (strcmp(only->name, "*") && strcmp(only->name, type)
					&& !settings_input_is(in, only->name))
				continue;
			ic = *only;
		} else {
			settings_input_config(in, &ic);
		}
		n++;
		if (in->group) {
			for (i = 0; i < InXkbLast; i++)
				names[i] = only && !ic.xkb[i] ? in->names[i] : ic.xkb[i];
			rate = ic.value[InRepeatRate] >= 0 ? ic.value[InRepeatRate] : in->rate;
			delay = ic.value[InRepeatDelay] >= 0 ? ic.value[InRepeatDelay] : in->delay;
			if (!settings_keyboard_set(in, names, rate, delay)) {
				fprintf(stderr, "tidewm: XKB can't make a keymap of %s's settings\n",
						in->identifier);
				if (!only)
					settings_error(settings_errors, 0, "input %s: XKB can't make a keymap "
							"of its settings, so it keeps the one it had", in->identifier);
				refused = 1;
			}
		}
		if (ic.has_scroll_factor)
			in->scroll_factor = ic.scroll_factor;
		if (settings_libinput(in, &ic))
			settings_event_input("libinput_config", in);
	}
	return refused ? -1 : n;
}

/* group's modifiers changed. If its active layout did too, each of its
 * keyboards' input event says so (xkb_layout), as sway's does, for a
 * layout indicator. A client's virtual keyboard, as an on-screen one, has
 * a group of its own, which its device doesn't name, as it takes no
 * settings: it's the group's virtual keyboard. */
static void
settings_layout_check(KeyboardGroup *group)
{
	SettingsInput *in;
	xkb_layout_index_t layout = settings_active_layout(&group->wlr_group->keyboard);
	if (layout == group->layout)
		return;
	group->layout = layout;
	wl_list_for_each(in, &settings_inputs, link)
		if (in->group == group || (group->virtual_keyboard
				&& in->dev == &group->virtual_keyboard->base))
			settings_event_input("xkb_layout", in);
}

/* Whether in is one of the devices the input command's name names */
static int
settings_input_named(const SettingsInput *in, const char *name)
{
	char type[32];
	snprintf(type, sizeof(type), "type:%s", in->type);
	return !strcmp(name, "*") || !strcmp(name, type) || settings_input_is(in, name);
}

/* sway's "input NAME xkb_switch_layout next|prev|INDEX": switches the
 * active layout of the keyboards NAME names now. It's a keyboard's in
 * its group, so one keyboard switches each group, and wlroots takes the
 * rest of the group along. Returns 1, or 0 with err saying why. */
static int
settings_switch_layout(const char *name, char *arg, char *err, size_t errsize)
{
	SettingsInput *in, *other;
	struct wlr_keyboard *kb, *member;
	char *end;
	long index = -1;
	int step = 0, n = 0, seen;
	xkb_layout_index_t nlayouts, layout;
	arg += strspn(arg, " \t");
	if (!strcmp(arg, "next")) {
		step = 1;
	} else if (!strcmp(arg, "prev")) {
		step = -1;
	} else {
		errno = 0;
		index = strtol(arg, &end, 10);
		if (!*arg || *end || errno || index < 0) {
			snprintf(err, errsize, "input %s: xkb_switch_layout takes next, prev or a "
					"layout's index", name);
			return 0;
		}
	}
	wl_list_for_each(in, &settings_inputs, link) {
		if (!in->group || !settings_input_named(in, name))
			continue;
		/* One switch a group */
		seen = 0;
		wl_list_for_each(other, &settings_inputs, link) {
			if (other == in)
				break;
			seen |= other->group == in->group && settings_input_named(other, name);
		}
		if (seen)
			continue;
		kb = &in->group->wlr_group->keyboard;
		nlayouts = kb->keymap ? xkb_keymap_num_layouts(kb->keymap) : 0;
		if (!nlayouts)
			continue;
		if (step)
			layout = (xkb_layout_index_t)(((long)settings_active_layout(kb) + step
					+ (long)nlayouts) % (long)nlayouts);
		else if ((unsigned long)index < nlayouts)
			layout = (xkb_layout_index_t)index;
		else {
			snprintf(err, errsize, "input %s: xkb_switch_layout %ld: its keymap has %u nlayouts",
					name, index, nlayouts);
			return 0;
		}
		member = wlr_keyboard_from_input_device(in->dev);
		wlr_keyboard_notify_modifiers(member, member->modifiers.depressed,
				member->modifiers.latched, member->modifiers.locked, layout);
		n++;
	}
	if (!n)
		snprintf(err, errsize, "No keyboard matches %s", name);
	return n > 0;
}

/* The input command, "NAME SETTING VALUE": sets it now, on the devices
 * NAME names, until a reload. Returns 1, or 0 with err saying why; *parse
 * says it didn't parse. */
static int
settings_input_command(char *args, char *err, size_t errsize, int *parse)
{
	InputConfig ic;
	char *name = settings_name(&args);
	size_t i;
	int n = 0;
	*parse = 1;
	if (!settings_input_name_ok(name ? name : "", err, errsize))
		return 0;
	/* A switch now, not a setting */
	if (!strncmp(args, "xkb_switch_layout", 17) && (!args[17] || isspace((unsigned char)args[17]))) {
		*parse = 0;
		return settings_switch_layout(name, args + 17, err, errsize);
	}
	settings_input_init(&ic, name);
	if (settings_parse_input_setting(&ic, args, err, errsize)) {
		*parse = 0;
		if ((n = settings_apply_inputs(&ic)) <= 0)
			snprintf(err, errsize, n < 0 ? "XKB can't make a keymap of those settings"
					: "No input device matches %s", name);
	}
	for (i = 0; i < InXkbLast; i++)
		free((char *)ic.xkb[i]);
	return !*parse && n > 0;
}

/* A key went down, with these modifiers and keysyms. Another key's press
 * means a tap is over, so it disarms a release binding; then this key
 * arms its own, which runs when it comes up. The modifiers are as they
 * were before this key, so a binding of Super_L alone matches it. */
static void
settings_key_pressed(uint32_t keycode, uint32_t mods, const xkb_keysym_t syms[2])
{
	int i;
	settings_tap.armed = 0;
	for (i = 0; i < 2; i++) {
		if (settings_find(mods, syms[i], 1)) {
			settings_tap.armed = 1;
			settings_tap.keycode = keycode;
			settings_tap.mods = mods;
			settings_tap.sym = syms[i];
			return;
		}
	}
}

/* A pointer button went down, or the pointer scrolled, or a touchpad
 * gesture began, while a release binding's key was down: that key is a
 * modifier now, not a tap */
static void
settings_tap_over(void)
{
	settings_tap.armed = 0;
}

/* A key came up: runs the release binding it armed, if it's still there.
 * Returns whether one ran. */
static int
settings_key_released(uint32_t keycode)
{
	if (!settings_tap.armed || settings_tap.keycode != keycode)
		return 0;
	settings_tap.armed = 0;
	return settings_run_binding(settings_tap.mods, settings_tap.sym, 1);
}

/* The mode command: makes name's bindings the ones in force. Returns 0 if
 * there's no such mode. "default" is always there, as the top level. */
static int
settings_enter_mode(char *name)
{
	size_t i;
	char *p = name;
	if (*p == '"' && (name = settings_name(&p)) && *p)
		return 0; /* something after the quoted name */
	if (!name)
		return 0;
	for (i = 0; settings && i < settings->nmodes; i++)
		if (!strcmp(settings->modes[i].name, name))
			break;
	if (strcmp(name, "default") && (!settings || i == settings->nmodes))
		return 0;
	if (!settings)
		return 1; /* config.h's keys have only the top level */
	if (i != settings_mode) {
		settings_mode = i;
		ipc_event_mode(settings->modes[i].name);
	}
	return 1;
}

static const char *
settings_mode_now(void)
{
	return settings ? settings->modes[settings_mode].name : "default";
}

/* GET_BINDING_MODES's reply: every mode's name, default first. */
static struct json_object *
settings_json_modes(void)
{
	struct json_object *list = json_object_new_array();
	size_t i;
	if (!settings)
		json_object_array_add(list, json_object_new_string("default"));
	for (i = 0; settings && i < settings->nmodes; i++)
		json_object_array_add(list, json_object_new_string(settings->modes[i].name));
	return list;
}

/* The config file's bindings, for GET_BINDINGS: each mode's bindsym
 * lines, the top level's first, with their keys as written, after its
 * variables, then the bindswitch lines, each as sway's identifier for its
 * switch and state. config.h's keys, which have no commands to show, are
 * left out: without a file, there are none. */
static struct json_object *
settings_json_bindings(void)
{
	static const char *const switches[] = {"off", "on", "toggle"};
	struct json_object *list = json_object_new_array(), *b;
	const Binding *k;
	const SwitchBinding *sw;
	char name[32];
	size_t i, j;
	for (i = 0; settings && i < settings->nmodes; i++) {
		for (j = 0; j < settings->modes[i].nbindings; j++) {
			k = &settings->modes[i].bindings[j];
			b = json_object_new_object();
			json_object_object_add(b, "type", json_object_new_string(k->button ? "mouse" : "key"));
			json_object_object_add(b, "keys", json_object_new_string(k->keys));
			json_object_object_add(b, "command", json_object_new_string(k->command));
			json_object_object_add(b, "mode", json_object_new_string(settings->modes[i].name));
			json_object_object_add(b, "release", json_object_new_boolean(k->release));
			json_object_object_add(b, "locked", json_object_new_boolean(k->locked));
			json_object_object_add(b, "kept_open", json_object_new_boolean(k->kept_open));
			json_object_object_add(b, "name", json_object_new_string(
					k->named ? settings->named[k->named - 1].name : ""));
			json_object_array_add(list, b);
		}
	}
	for (i = 0; settings && i < settings->nswitches; i++) {
		sw = &settings->switches[i];
		snprintf(name, sizeof(name), "%s:%s", sw->type == WLR_SWITCH_TYPE_LID ? "lid" : "tablet",
				switches[sw->state]);
		b = json_object_new_object();
		json_object_object_add(b, "type", json_object_new_string("switch"));
		json_object_object_add(b, "keys", json_object_new_string(name));
		json_object_object_add(b, "command", json_object_new_string(sw->command));
		json_object_object_add(b, "mode", json_object_new_string("default"));
		json_object_object_add(b, "release", json_object_new_boolean(0));
		json_object_object_add(b, "locked", json_object_new_boolean(sw->locked));
		json_object_object_add(b, "kept_open", json_object_new_boolean(0));
		json_object_object_add(b, "name", json_object_new_string(""));
		json_object_array_add(list, b);
	}
	return list;
}

/* GET_NAMED_BINDINGS's reply: each named binding, in the order the file
 * first names it, with the keys its bindsym lines gave it and the keys it
 * has now, after any rebind line, for a shell to show and change. */
static struct json_object *
settings_json_named(void)
{
	struct json_object *list = json_object_new_array(), *b, *keys, *defaults;
	const NamedBinding *n;
	const Mode *mode;
	size_t i, j;
	for (i = 0; settings && i < settings->nnamed; i++) {
		n = &settings->named[i];
		mode = &settings->modes[n->mode];
		keys = json_object_new_array();
		for (j = 0; j < mode->nbindings; j++)
			if (mode->bindings[j].named == i + 1)
				json_object_array_add(keys, json_object_new_string(mode->bindings[j].keys));
		defaults = json_object_new_array();
		for (j = 0; j < n->ndefaults; j++)
			json_object_array_add(defaults, json_object_new_string(n->defaults[j]));
		b = json_object_new_object();
		json_object_object_add(b, "name", json_object_new_string(n->name));
		json_object_object_add(b, "type", json_object_new_string(n->flags.whole_window ? "mouse" : "key"));
		json_object_object_add(b, "keys", keys);
		json_object_object_add(b, "default_keys", defaults);
		json_object_object_add(b, "command", json_object_new_string(n->command));
		json_object_object_add(b, "mode", json_object_new_string(mode->name));
		json_object_object_add(b, "release", json_object_new_boolean(n->flags.release));
		json_object_object_add(b, "locked", json_object_new_boolean(n->flags.locked));
		json_object_object_add(b, "kept_open", json_object_new_boolean(n->flags.kept_open));
		json_object_array_add(list, b);
	}
	return list;
}

static void
settings_finish(void)
{
	SettingsInput *in, *itmp;
	SettingsGroup *g, *gtmp;
	size_t i;
	if (settings_release_source)
		wl_event_source_remove(settings_release_source);
	settings_release_source = NULL;
	wl_list_for_each_safe(in, itmp, &settings_inputs, link) {
		wl_list_remove(&in->destroy.link);
		wl_list_remove(&in->link);
		for (i = 0; i < InXkbLast; i++)
			free((char *)in->names[i]);
		free(in->identifier);
		free(in->hypr_name);
		free(in);
	}
	wl_list_for_each_safe(g, gtmp, &settings_groups, link) {
		if (g->group != kb_group)
			destroykeyboardgroup(&g->group->destroy, NULL);
		wl_list_remove(&g->link);
		free(g);
	}
	if (settings_xkb)
		xkb_context_unref(settings_xkb);
	settings_xkb = NULL;
	settings_free(settings);
	settings = NULL;
	settings_mode = 0;
	json_object_put(settings_errors);
	settings_errors = NULL;
}

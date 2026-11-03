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
#include <regex.h>

#define SETTINGS_MAX_SIZE (1u << 20)

typedef struct {
	uint32_t mods;
	xkb_keysym_t sym;
	int release; /* runs when the key comes up, if it was tapped */
	int locked; /* runs while the screen is locked, too */
	char *keys; /* as written, for messages */
	char *command;
} Binding;

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

/* A for_window rule: commands run on each new window its criteria match */
typedef struct {
	regex_t re[CritLast];
	int has[CritLast];
	char *command;
} WindowRule;

typedef struct {
	Mode *modes; /* modes[0] is "default", the file's top level */
	size_t nmodes;
	WindowRule *rules;
	size_t nrules;
	char *text; /* the file, as read, for GET_CONFIG */
} Settings;

typedef struct {
	char *name; /* "$name" */
	char *value;
} SettingsVar;

static char settings_path[PATH_MAX];
static Settings *settings; /* NULL: config.h's keys */
static struct json_object *settings_errors; /* the last load's, as strings */
static size_t settings_mode; /* the current mode, in settings->modes */
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
				regfree(&s->rules[i].re[j]);
		free(s->rules[i].command);
	}
	free(s->rules);
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
	char msg[512];
	int n;
	va_list ap;
	n = line ? snprintf(msg, sizeof(msg), "%s:%u: ", settings_path, line)
		: snprintf(msg, sizeof(msg), "%s: ", settings_path);
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

/* Reads "Mod4+Shift+Return" into mods and a keysym. On failure, *bad is
 * the part that's wrong. */
static int
settings_parse_keys(char *combo, uint32_t *mods, xkb_keysym_t *sym, const char **bad)
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
			return 0;
		}
		*mods |= names[i].mod;
	}
	/* Caps Lock is ignored when matching, so here too, or a binding
	 * naming Lock would never run, nor replace one that doesn't */
	*mods = CLEANMASK(*mods);
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

static int
settings_add_binding(Mode *mode, char *rest, unsigned int line, struct json_object *errors)
{
	char *combo, *spec;
	const char *bad = NULL;
	uint32_t mods;
	xkb_keysym_t sym;
	size_t i;
	int release = 0, locked_too = 0;
	Binding *b;

	while (rest[0] == '-' && rest[1] == '-') {
		char *flag = settings_word(&rest);
		if (!strcmp(flag, "--release")) {
			release = 1;
		} else if (!strcmp(flag, "--locked")) {
			locked_too = 1;
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
	spec = settings_strdup(combo, strlen(combo));
	if (!settings_parse_keys(combo, &mods, &sym, &bad)) {
		settings_error(errors, line, "bindsym %s: no modifier or key is called %s", spec, bad);
		free(spec);
		return 0;
	}
	/* A later binding of the same keys replaces an earlier one; a release
	 * binding is apart from a press one */
	for (i = 0; i < mode->nbindings; i++) {
		b = &mode->bindings[i];
		if (b->mods == mods && b->sym == sym && b->release == release) {
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
	b->release = release;
	b->locked = locked_too;
	b->keys = spec;
	b->command = settings_strdup(rest, strlen(rest));
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

/* Reads for_window's criteria, [key=value ...], and its commands into a
 * new rule. A value is a word or text in double quotes, where \" is a
 * quote, and is a POSIX extended regular expression. */
static int
settings_add_rule(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	WindowRule rule = {0}, *grown;
	char *p = rest, *key, *value, *out, msg[160];
	size_t i, n;
	int err, ok = 1, any = 0;

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
			err = regcomp(&rule.re[i], value, REG_EXTENDED | REG_NOSUB);
			if (err) {
				regerror(err, &rule.re[i], msg, sizeof(msg));
				settings_error(errors, line, "for_window: %s=%s: %s", key, value, msg);
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
				regfree(&rule.re[i]);
		return 0;
	}
	rule.command = settings_strdup(p, strlen(p));
	if (!(grown = realloc(s->rules, (s->nrules + 1) * sizeof(*grown))))
		die("out of memory reading the config");
	s->rules = grown;
	s->rules[s->nrules++] = rule;
	return 1;
}

/* A mode's name: a word, or text in double quotes, as sway takes it.
 * Moves *p past it. Returns NULL for an empty name or an unclosed quote. */
static char *
settings_mode_name(char **p)
{
	char *name = *p, *close;
	if (*name != '"')
		return *(name = settings_word(p)) ? name : NULL;
	if (!(close = strchr(++name, '"')) || close == name)
		return NULL;
	*close = '\0';
	*p = close + 1;
	while (isspace((unsigned char)**p))
		(*p)++;
	return name;
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

/* Parses text, which it keeps, into new settings, or returns NULL and
 * adds each error to errors. */
static Settings *
settings_parse(char *text, struct json_object *errors)
{
	Settings *s = ecalloc(1, sizeof(*s));
	SettingsVar *vars = NULL;
	size_t nvars = 0, i, len, mode = 0;
	unsigned int line = 0, mode_line = 0;
	char *start, *end, *raw, *expanded, *p, *word, *name;
	int ok = 1;

	s->text = text;
	settings_add_mode(s, "default");
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
			expanded = settings_substitute(p, vars, nvars);
			ok &= settings_set(&vars, &nvars, name, expanded, line, errors);
			free(expanded);
			free(raw);
			continue;
		}
		expanded = settings_substitute(p, vars, nvars);
		p = expanded;
		word = settings_word(&p);
		if (!strcasecmp(word, "bindsym")) {
			ok &= settings_add_binding(&s->modes[mode], p, line, errors);
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
			} else if (!(name = settings_mode_name(&p)) || strcmp(p, "{")) {
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
				settings_error(errors, line, "} closes no mode");
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
	for (i = 0; i < nvars; i++) {
		free(vars[i].name);
		free(vars[i].value);
	}
	free(vars);
	if (!ok) {
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
 * keys. settings_errors gets this load's errors. */
static int
settings_load(void)
{
	struct json_object *errors = json_object_new_array();
	Settings *loaded = NULL;
	char *text;
	int r = settings_read(settings_path, &text, errors);
	if (r > 0)
		loaded = settings_parse(text, errors);
	json_object_put(settings_errors);
	settings_errors = errors;
	if (r < 0 || (r > 0 && !loaded))
		return 0;
	settings_free(settings);
	settings = loaded;
	/* As sway does, a load starts at the top level */
	if (settings_mode) {
		settings_mode = 0;
		ipc_event_mode("default");
	}
	return 1;
}

/* Finds the file: the -c argument, or $XDG_CONFIG_HOME/tidewm/config, or
 * ~/.config/tidewm/config. A file with errors is reported and left out:
 * tidewm starts with config.h's keys, so it can still be fixed. */
static void
settings_init(const char *arg)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	size_t i;
	int n;
	if (arg)
		n = snprintf(settings_path, sizeof(settings_path), "%s", arg);
	else if (xdg && *xdg)
		n = snprintf(settings_path, sizeof(settings_path), "%s/tidewm/config", xdg);
	else
		n = snprintf(settings_path, sizeof(settings_path), "%s/.config/tidewm/config",
				home ? home : "");
	if (n < 0 || (size_t)n >= sizeof(settings_path))
		die("the config file's path is too long");
	if (settings_load())
		return;
	for (i = 0; i < json_object_array_length(settings_errors); i++)
		fprintf(stderr, "tidewm: %s\n",
				json_object_get_string(json_object_array_get_idx(settings_errors, i)));
	fprintf(stderr, "tidewm: not using %s: config.h's keys work instead\n", settings_path);
}

/* The reload command: rereads the file. On errors, keeps what was loaded,
 * and puts the first few in buf. */
static int
settings_reload(char *buf, size_t size)
{
	size_t i, n, len = 0;
	int w;
	if (settings_load())
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
	if (n > 3)
		snprintf(buf + len, size - len, "; and %zu more", n - 3);
	return 0;
}

/* GET_CONFIG's reply: sway's config, the loaded file's text, plus
 * tidewm's path and errors, from the last load. */
static struct json_object *
settings_json(void)
{
	struct json_object *obj = json_object_new_object();
	json_object_object_add(obj, "config", json_object_new_string(settings ? settings->text : ""));
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
 * While the screen is locked, only a --locked one counts. */
static const Binding *
settings_find(uint32_t mods, xkb_keysym_t sym, int release)
{
	const Mode *mode;
	size_t i;
	if (!settings)
		return NULL;
	mode = &settings->modes[settings_mode];
	for (i = 0; i < mode->nbindings; i++) {
		const Binding *b = &mode->bindings[i];
		if (b->mods == CLEANMASK(mods) && b->sym == sym && b->release == release
				&& (!locked || b->locked))
			return b;
	}
	return NULL;
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
	settings_run(b->command, what, NULL);
	return 1;
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
					|| regexec(&r->re[crit], value, 0, NULL, 0)))
				break;
		if (crit < CritLast)
			continue;
		snprintf(what, sizeof(what), "for_window rule %zu", i + 1);
		settings_run(r->command, what, c);
	}
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
	if (*p == '"' && (name = settings_mode_name(&p)) && *p)
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

static void
settings_finish(void)
{
	settings_free(settings);
	settings = NULL;
	settings_mode = 0;
	json_object_put(settings_errors);
	settings_errors = NULL;
}

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

/* An output line: what to set on the outputs it names, "*" for all */
typedef struct {
	char *name;
	int has_mode, width, height, refresh; /* refresh in mHz; 0 for the best */
	int has_scale;
	float scale;
	int has_position, x, y;
	int has_transform;
	enum wl_output_transform transform;
	int enabled; /* 1 or 0, or -1 to leave it */
} OutputConfig;

typedef struct {
	Mode *modes; /* modes[0] is "default", the file's top level */
	size_t nmodes;
	WindowRule *rules;
	size_t nrules;
	OutputConfig *outputs;
	size_t noutputs;
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
static int settings_apply_outputs(const OutputConfig *only);
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
	for (i = 0; i < s->noutputs; i++)
		free(s->outputs[i].name);
	free(s->outputs);
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

/* Parses an output line's rest, "NAME SETTING...", into oc, as the config
 * file and the output command share it. On failure, err says why. */
static int
settings_parse_output(OutputConfig *oc, char *rest, char *err, size_t errsize)
{
	static const struct { const char *name; enum wl_output_transform t; } transforms[] = {
		{"normal", WL_OUTPUT_TRANSFORM_NORMAL}, {"90", WL_OUTPUT_TRANSFORM_90},
		{"180", WL_OUTPUT_TRANSFORM_180}, {"270", WL_OUTPUT_TRANSFORM_270},
		{"flipped", WL_OUTPUT_TRANSFORM_FLIPPED},
		{"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90},
		{"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
		{"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
	};
	char *word, *arg, *end, extra;
	size_t i, len;
	float hz;
	int used, good;

	memset(oc, 0, sizeof(*oc));
	oc->enabled = -1;
	if (!*(word = settings_word(&rest))) {
		snprintf(err, errsize, "output needs a name, or *, then settings");
		return 0;
	}
	oc->name = word;
	if (!*rest) {
		snprintf(err, errsize, "output %s needs a setting", oc->name);
		return 0;
	}
	while (*(word = settings_word(&rest))) {
		if (!strcmp(word, "mode") || !strcmp(word, "resolution") || !strcmp(word, "res")) {
			/* WIDTHxHEIGHT, then @HZ, Hz or not, if it's given */
			arg = settings_word(&rest);
			len = strlen(arg);
			if (len > 2 && !strcasecmp(arg + len - 2, "hz"))
				arg[len - 2] = '\0';
			hz = 0;
			used = 0;
			good = sscanf(arg, "%dx%d%n", &oc->width, &oc->height, &used) == 2
				&& oc->width > 0 && oc->height > 0;
			if (good && arg[used] == '@') {
				hz = strtof(arg + used + 1, &end);
				good = end != arg + used + 1 && !*end && isfinite(hz) && hz > 0 && hz <= 1000;
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
			arg = settings_word(&rest);
			oc->scale = strtof(arg, &end);
			if (end == arg || *end || !isfinite(oc->scale) || oc->scale <= 0 || oc->scale > 10) {
				snprintf(err, errsize, "output %s: scale %s isn't a number from above 0 to 10", oc->name, arg);
				return 0;
			}
			oc->has_scale = 1;
		} else if (!strcmp(word, "position") || !strcmp(word, "pos")) {
			arg = settings_word(&rest);
			if (strchr(arg, ',') ? sscanf(arg, "%d,%d%c", &oc->x, &oc->y, &extra) != 2
					: (sscanf(arg, "%d%c", &oc->x, &extra) != 1
					|| sscanf(settings_word(&rest), "%d%c", &oc->y, &extra) != 1)) {
				snprintf(err, errsize, "output %s: position needs X Y", oc->name);
				return 0;
			}
			oc->has_position = 1;
		} else if (!strcmp(word, "transform")) {
			arg = settings_word(&rest);
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
		} else {
			snprintf(err, errsize, "output %s: no setting is called %s", oc->name, word);
			return 0;
		}
	}
	return 1;
}

/* Puts what from sets into into, as a later output line for the same
 * name adds to and overrides an earlier one. */
static void
settings_merge_output(OutputConfig *into, const OutputConfig *from)
{
	if (from->has_mode) {
		into->has_mode = 1;
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
		into->x = from->x;
		into->y = from->y;
	}
	if (from->has_transform) {
		into->has_transform = 1;
		into->transform = from->transform;
	}
	if (from->enabled >= 0)
		into->enabled = from->enabled;
}

static int
settings_add_output(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	OutputConfig oc, *grown;
	char err[256];
	size_t i;
	if (!settings_parse_output(&oc, rest, err, sizeof(err))) {
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
		} else if (!strcasecmp(word, "output")) {
			if (mode) {
				settings_error(errors, line, "output can't be in a mode");
				ok = 0;
			} else {
				ok &= settings_add_output(s, p, line, errors);
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
 * keys. It returns -1 if it loaded, but an output refused its settings.
 * settings_errors gets this load's errors. */
static int
settings_load(void)
{
	struct json_object *errors = json_object_new_array();
	Settings *loaded = NULL;
	char *text;
	int refused, r = settings_read(settings_path, &text, errors);
	if (r > 0)
		loaded = settings_parse(text, errors);
	json_object_put(settings_errors);
	settings_errors = errors;
	if (r < 0 || (r > 0 && !loaded))
		return 0;
	settings_free(settings);
	settings = loaded;
	/* Outputs take the new lines at once; at startup, there are none yet.
	 * One that refuses them is an error, but the rest of the file stands */
	refused = settings_apply_outputs(NULL) < 0;
	/* As sway does, a load starts at the top level */
	if (settings_mode) {
		settings_mode = 0;
		ipc_event_mode("default");
	}
	return refused ? -1 : 1;
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
 * and puts the first few in buf. An output that refused its settings
 * fails it too, though the rest of the file is loaded. */
static int
settings_reload(char *buf, size_t size)
{
	size_t i, n, len = 0;
	int w, r;
	if ((r = settings_load()) > 0)
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

/* The file's settings for the output called name: its "*" line, then its
 * own, which overrides it. Returns 0 if neither is there. */
static int
settings_output_config(const char *name, OutputConfig *out)
{
	size_t i;
	int pass, found = 0;
	memset(out, 0, sizeof(*out));
	out->enabled = -1;
	for (pass = 0; settings && pass < 2; pass++) {
		for (i = 0; i < settings->noutputs; i++) {
			if (strcmp(settings->outputs[i].name, pass ? name : "*"))
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

/* Puts oc's mode, scale, transform and enabling in state, for monitor m.
 * A mode its output lists is used if there is one: with a rate, the
 * closest within 1 Hz, so 59.94 Hz stays apart from 60; without, the
 * fastest. Otherwise it's a custom mode. wlroots won't give an output
 * that's off a mode, so settings_output_set() keeps one for when it's
 * enabled; one enabled without a mode takes that, or keeps the one it
 * had, which wlroots remembers, or, if it never had one, its preferred. */
static void
settings_output_state(Monitor *m, const OutputConfig *oc, struct wlr_output_state *state)
{
	struct wlr_output *o = m->wlr_output;
	struct wlr_output_mode *mode, *best = NULL;
	OutputConfig want = *oc;
	if (!(oc->enabled >= 0 ? oc->enabled : o->enabled)) {
		want.has_mode = 0;
	} else if (!want.has_mode && !o->enabled) {
		if ((want.has_mode = m->want_mode)) {
			want.width = m->want_width;
			want.height = m->want_height;
			want.refresh = m->want_refresh;
		} else if (!o->width || !o->height) {
			settings_preferred_mode(o, &want);
		}
	}
	if (want.has_mode) {
		wl_list_for_each(mode, &o->modes, link) {
			if (mode->width != want.width || mode->height != want.height
					|| (want.refresh && abs(mode->refresh - want.refresh) > 1000))
				continue;
			if (!best || (want.refresh
					? abs(mode->refresh - want.refresh) < abs(best->refresh - want.refresh)
					: mode->refresh > best->refresh))
				best = mode;
		}
		if (best)
			wlr_output_state_set_mode(state, best);
		else
			wlr_output_state_set_custom_mode(state, want.width, want.height, want.refresh);
	}
	if (oc->has_scale)
		wlr_output_state_set_scale(state, oc->scale);
	if (oc->has_transform)
		wlr_output_state_set_transform(state, oc->transform);
	if (oc->enabled >= 0)
		wlr_output_state_set_enabled(state, oc->enabled);
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
	}
	if (oc->has_position) {
		m->want_pos = 1;
		m->want_x = oc->x;
		m->want_y = oc->y;
	} else if (reset) {
		m->want_pos = 0;
	}
}

/* An output's settings before the file's: config.h's monitor rule's scale,
 * transform and position, and enabled. */
static void
settings_output_defaults(Monitor *m, OutputConfig *oc)
{
	const MonitorRule *r;
	memset(oc, 0, sizeof(*oc));
	oc->enabled = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(m->wlr_output->name, r->name)) {
			oc->has_scale = oc->has_transform = 1;
			oc->scale = r->scale;
			oc->transform = r->rr;
			if ((oc->has_position = r->x != -1 || r->y != -1)) {
				oc->x = r->x;
				oc->y = r->y;
			}
			break;
		}
	}
}

/* Sets only on the outputs it names, as the output command does. Without
 * it, as on reload, each output starts again from its defaults and its
 * preferred mode, then takes the file's lines, so nothing the command set
 * outlives a reload. An output enabled goes where it was last placed, if
 * it was. Returns how many outputs took their settings, or -1 if any
 * refused them, which it says on stderr, and, on reload, in
 * settings_errors. */
static int
settings_apply_outputs(const OutputConfig *only)
{
	struct wlr_output_state state;
	OutputConfig oc, file;
	Monitor *m;
	int set = 0, refused = 0;
	wl_list_for_each(m, &mons, link) {
		if (only) {
			if (strcmp(only->name, "*") && strcmp(only->name, m->wlr_output->name))
				continue;
			oc = *only;
		} else {
			settings_output_defaults(m, &oc);
			if (settings_output_config(m->wlr_output->name, &file))
				settings_merge_output(&oc, &file);
			settings_preferred_mode(m->wlr_output, &oc);
		}
		wlr_output_state_init(&state);
		settings_output_state(m, &oc, &state);
		if (wlr_output_commit_state(m->wlr_output, &state)) {
			set++;
			settings_output_set(m, &oc, !only);
			if (m->wlr_output->enabled && m->want_pos)
				wlr_output_layout_add(output_layout, m->wlr_output, m->want_x, m->want_y);
			else if (m->wlr_output->enabled && !only)
				wlr_output_layout_add_auto(output_layout, m->wlr_output);
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

/* The output command, "NAME SETTING...": sets them now, on the outputs
 * NAME names, "*" for all, without keeping them past a reload. Returns
 * 1, or 0 with err saying why; *parse says it didn't parse. */
static int
settings_output_command(char *args, char *err, size_t errsize, int *parse)
{
	OutputConfig oc;
	int set;
	*parse = !settings_parse_output(&oc, args, err, errsize);
	if (*parse)
		return 0;
	if ((set = settings_apply_outputs(&oc)) > 0)
		return 1;
	snprintf(err, errsize, set < 0 ? "An output refused those settings"
			: "No output is called %s", oc.name);
	return 0;
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

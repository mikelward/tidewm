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

#define SETTINGS_MAX_SIZE (1u << 20)

typedef struct {
	uint32_t mods;
	xkb_keysym_t sym;
	char *keys; /* as written, for messages */
	char *command;
} Binding;

typedef struct {
	Binding *bindings;
	size_t nbindings;
	char *text; /* the file, as read, for GET_CONFIG */
} Settings;

typedef struct {
	char *name; /* "$name" */
	char *value;
} SettingsVar;

static char settings_path[PATH_MAX];
static Settings *settings; /* NULL: config.h's keys */
static struct json_object *settings_errors; /* the last load's, as strings */

static void
settings_free(Settings *s)
{
	size_t i;
	if (!s)
		return;
	for (i = 0; i < s->nbindings; i++) {
		free(s->bindings[i].keys);
		free(s->bindings[i].command);
	}
	free(s->bindings);
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
settings_add_binding(Settings *s, char *rest, unsigned int line, struct json_object *errors)
{
	char *combo, *spec;
	const char *bad = NULL;
	uint32_t mods;
	xkb_keysym_t sym;
	size_t i;
	Binding *b;

	while (rest[0] == '-' && rest[1] == '-') {
		char *flag = settings_word(&rest);
		if (!strcmp(flag, "--release") || !strcmp(flag, "--locked"))
			settings_error(errors, line, "bindsym %s isn't supported yet", flag);
		else
			settings_error(errors, line, "bindsym has no flag %s", flag);
		return 0;
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
	/* A later binding of the same keys replaces an earlier one */
	for (i = 0; i < s->nbindings; i++) {
		b = &s->bindings[i];
		if (b->mods == mods && b->sym == sym) {
			free(b->keys);
			free(b->command);
			break;
		}
	}
	if (i == s->nbindings) {
		if (!(b = realloc(s->bindings, (s->nbindings + 1) * sizeof(*b))))
			die("out of memory reading the config");
		s->bindings = b;
		b = &s->bindings[s->nbindings++];
	}
	b->mods = mods;
	b->sym = sym;
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

/* Parses text, which it keeps, into new settings, or returns NULL and
 * adds each error to errors. */
static Settings *
settings_parse(char *text, struct json_object *errors)
{
	Settings *s = ecalloc(1, sizeof(*s));
	SettingsVar *vars = NULL;
	size_t nvars = 0, i, len;
	unsigned int line = 0;
	char *start, *end, *raw, *expanded, *p, *word;
	int ok = 1;

	s->text = text;
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
			char *name;
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
		if (!strcasecmp(word, "bindsym"))
			ok &= settings_add_binding(s, p, line, errors);
		else {
			settings_error(errors, line, "no setting is called %s", word);
			ok = 0;
		}
		free(expanded);
		free(raw);
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

/* Runs the loaded file's binding for these keys. Returns whether it has
 * one. */
static int
settings_run_binding(uint32_t mods, xkb_keysym_t sym)
{
	struct json_object *results, *result, *error;
	char *command, *combo;
	size_t i;
	int quit = 0;
	for (i = 0; i < settings->nbindings; i++)
		if (settings->bindings[i].mods == CLEANMASK(mods) && settings->bindings[i].sym == sym)
			break;
	if (i == settings->nbindings)
		return 0;
	/* Copies, since the commands may reload the settings */
	command = settings_strdup(settings->bindings[i].command, strlen(settings->bindings[i].command));
	combo = settings_strdup(settings->bindings[i].keys, strlen(settings->bindings[i].keys));
	results = ipc_run_commands(command, &quit);
	for (i = 0; i < json_object_array_length(results); i++) {
		result = json_object_array_get_idx(results, i);
		if (!json_object_get_boolean(json_object_object_get(result, "success"))
				&& json_object_object_get_ex(result, "error", &error))
			fprintf(stderr, "tidewm: binding %s: %s\n", combo, json_object_get_string(error));
	}
	json_object_put(results);
	free(command);
	free(combo);
	if (quit)
		ipc_quit();
	return 1;
}

static void
settings_finish(void)
{
	settings_free(settings);
	settings = NULL;
	json_object_put(settings_errors);
	settings_errors = NULL;
}

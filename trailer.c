#include "git-compat-util.h"
#include "config.h"
#include "environment.h"
#include "gettext.h"
#include "string-list.h"
#include "run-command.h"
#include "commit.h"
#include "trailer.h"
#include "list.h"
/*
 * Copyright (c) 2013, 2014 Christian Couder <chriscool@tuxfamily.org>
 */

struct trailer_block {
	/*
	 * True if there is a blank line before the location pointed to by
	 * "start".
	 */
	int blank_line_before_trailer;

	/*
	 * The locations of the start and end positions of the trailer block
	 * found, as offsets from the beginning of the source text from which
	 * this trailer block was parsed. If no trailer block is found, these
	 * are both set to 0.
	 */
	size_t start, end;

	/*
	 * Array of trailers found.
	 */
	char **trailers;
	size_t trailer_nr;
};

struct trailer_conf {
	char *name;
	char *key;
	char *command;
	char *cmd;
	enum trailer_where where;
	enum trailer_if_exists if_exists;
	enum trailer_if_missing if_missing;
};

static struct trailer_conf default_trailer_conf;

struct trailer_item {
	struct list_head list;
	/*
	 * If this is not a trailer line, the line is stored in value
	 * (excluding the terminating newline) and token is NULL.
	 */
	char *token;
	char *value;
};

struct trailer_template {
	struct list_head list;
	char *token;
	char *value;
	struct trailer_conf conf;
};

static LIST_HEAD(templates_from_conf);

static char *separators = ":";

const char *trailer_default_separators(void)
{
	return separators;
}

static int configured;

#define TRAILER_ARG_STRING "$ARG"

static const char *git_generated_prefixes[] = {
	"Signed-off-by: ",
	"(cherry picked from commit ",
	NULL
};

/* Iterate over the elements of the list. */
#define list_for_each_dir(pos, head, is_reverse) \
	for (pos = is_reverse ? (head)->prev : (head)->next; \
		pos != (head); \
		pos = is_reverse ? pos->prev : pos->next)

static int after_or_end(enum trailer_where where)
{
	return (where == WHERE_AFTER) || (where == WHERE_END);
}

/*
 * Return the length of the string not including any final
 * punctuation. E.g., the input "Signed-off-by:" would return
 * 13, stripping the trailing punctuation but retaining
 * internal punctuation.
 */
static size_t token_len_without_separator(const char *token, size_t len)
{
	while (len > 0 && !isalnum(token[len - 1]))
		len--;
	return len;
}

static int same_token(struct trailer_item *a, struct trailer_template *b)
{
	size_t a_len, b_len, min_len;

	if (!a->token)
		return 0;

	a_len = token_len_without_separator(a->token, strlen(a->token));
	b_len = token_len_without_separator(b->token, strlen(b->token));
	min_len = (a_len > b_len) ? b_len : a_len;

	return !strncasecmp(a->token, b->token, min_len);
}

static int same_value(struct trailer_item *a, struct trailer_template *b)
{
	return !strcasecmp(a->value, b->value);
}

static int same_trailer(struct trailer_item *a, struct trailer_template *b)
{
	return same_token(a, b) && same_value(a, b);
}

static inline int is_blank_line(const char *str)
{
	const char *s = str;
	while (*s && *s != '\n' && isspace(*s))
		s++;
	return !*s || *s == '\n';
}

static inline void strbuf_replace(struct strbuf *sb, const char *a, const char *b)
{
	const char *ptr = strstr(sb->buf, a);
	if (ptr)
		strbuf_splice(sb, ptr - sb->buf, strlen(a), b, strlen(b));
}

static void free_trailer_item(struct trailer_item *item)
{
	free(item->token);
	free(item->value);
	free(item);
}

static void free_template(struct trailer_template *template)
{
	free(template->conf.name);
	free(template->conf.key);
	free(template->conf.command);
	free(template->conf.cmd);
	free(template->token);
	free(template->value);
	free(template);
}

static char last_non_space_char(const char *s)
{
	int i;
	for (i = strlen(s) - 1; i >= 0; i--)
		if (!isspace(s[i]))
			return s[i];
	return '\0';
}

static struct trailer_item *trailer_from(struct trailer_template *template)
{
	struct trailer_item *new_item = xcalloc(1, sizeof(*new_item));
	new_item->token = template->token;
	new_item->value = template->value;
	template->token = template->value = NULL;
	free_template(template);
	return new_item;
}

static void apply_template_to_trailers(struct trailer_template *template,
				       struct trailer_item *on_tok)
{
	int aoe = after_or_end(template->conf.where);
	struct trailer_item *to_add = trailer_from(template);
	if (aoe)
		list_add(&to_add->list, &on_tok->list);
	else
		list_add_tail(&to_add->list, &on_tok->list);
}

static int check_if_different(struct trailer_template *template,
			      struct trailer_item *in_tok,
			      struct list_head *head,
			      int check_all)
{
	enum trailer_where where = template->conf.where;
	struct list_head *next_head;
	do {
		if (same_trailer(in_tok, template))
			return 0;
		/*
		 * if we want to add a trailer after another one,
		 * we have to check those before this one
		 */
		next_head = after_or_end(where) ? in_tok->list.prev
						: in_tok->list.next;
		if (next_head == head)
			break;
		in_tok = list_entry(next_head, struct trailer_item, list);
	} while (check_all);
	return 1;
}

static char *run_command_from_template(struct trailer_conf *conf,
				       const char *arg)
{
	struct strbuf cmd = STRBUF_INIT;
	struct strbuf buf = STRBUF_INIT;
	struct child_process cp = CHILD_PROCESS_INIT;
	char *result;

	if (conf->cmd) {
		strbuf_addstr(&cmd, conf->cmd);
		strvec_push(&cp.args, cmd.buf);
		if (arg)
			strvec_push(&cp.args, arg);
	} else if (conf->command) {
		strbuf_addstr(&cmd, conf->command);
		if (arg)
			strbuf_replace(&cmd, TRAILER_ARG_STRING, arg);
		strvec_push(&cp.args, cmd.buf);
	}
	strvec_pushv(&cp.env, (const char **)local_repo_env);
	cp.no_stdin = 1;
	cp.use_shell = 1;

	if (capture_command(&cp, &buf, 1024)) {
		error(_("running trailer command '%s' failed"), cmd.buf);
		strbuf_release(&buf);
		result = xstrdup("");
	} else {
		strbuf_trim(&buf);
		result = strbuf_detach(&buf, NULL);
	}

	strbuf_release(&cmd);
	return result;
}

/*
 * Prepare the template by running the command (if any) requested by the
 * template in order to populate the template's value field.
 */
static void populate_template_value(struct trailer_template *template,
				    struct trailer_item *in_tok)
{
	if (template->conf.command || template->conf.cmd) {
		/*
		 * Determine argument to pass into the command.
		 */
		const char *arg;
		if (template->value && template->value[0]) {
			arg = template->value;
		} else {
			if (in_tok && in_tok->value)
				arg = xstrdup(in_tok->value);
			else
				arg = xstrdup("");
		}
		template->value = run_command_from_template(&template->conf,
							    arg);
		free((char *)arg);
	}
}

static void maybe_add_if_exists(struct trailer_template *template,
				struct trailer_item *in_tok,
				struct trailer_item *on_tok,
				struct list_head *trailers)
{
	switch (template->conf.if_exists) {
	case EXISTS_DO_NOTHING:
		free_template(template);
		break;
	case EXISTS_REPLACE:
		populate_template_value(template, in_tok);
		apply_template_to_trailers(template, on_tok);
		list_del(&in_tok->list);
		free_trailer_item(in_tok);
		break;
	case EXISTS_ADD:
		populate_template_value(template, in_tok);
		apply_template_to_trailers(template, on_tok);
		break;
	case EXISTS_ADD_IF_DIFFERENT:
		populate_template_value(template, in_tok);
		if (check_if_different(template, in_tok, trailers, 1))
			apply_template_to_trailers(template, on_tok);
		else
			free_template(template);
		break;
	case EXISTS_ADD_IF_DIFFERENT_NEIGHBOR:
		populate_template_value(template, in_tok);
		if (check_if_different(template, on_tok, trailers, 0))
			apply_template_to_trailers(template, on_tok);
		else
			free_template(template);
		break;
	default:
		BUG("trailer.c: unhandled value %d",
		    template->conf.if_exists);
	}
}

static void maybe_add_if_missing(struct trailer_template *template,
				 struct list_head *trailers)
{
	enum trailer_where where;
	struct trailer_item *to_add;

	switch (template->conf.if_missing) {
	case MISSING_DO_NOTHING:
		free_template(template);
		break;
	case MISSING_ADD:
		where = template->conf.where;
		populate_template_value(template, NULL);
		to_add = trailer_from(template);
		if (after_or_end(where))
			list_add_tail(&to_add->list, trailers);
		else
			list_add(&to_add->list, trailers);
		break;
	default:
		BUG("trailer.c: unhandled value %d",
		    template->conf.if_missing);
	}
}

static int find_same_and_apply_arg(struct trailer_template *template,
				   struct list_head *trailers)
{
	struct list_head *pos;
	struct trailer_item *in_tok;
	struct trailer_item *on_tok;

	enum trailer_where where = template->conf.where;
	int middle = (where == WHERE_AFTER) || (where == WHERE_BEFORE);
	int backwards = after_or_end(where);
	struct trailer_item *start_tok;

	if (list_empty(trailers))
		return 0;

	start_tok = list_entry(backwards ? trailers->prev : trailers->next,
			       struct trailer_item,
			       list);

	list_for_each_dir(pos, trailers, backwards) {
		in_tok = list_entry(pos, struct trailer_item, list);
		if (!same_token(in_tok, template))
			continue;
		on_tok = middle ? in_tok : start_tok;
		maybe_add_if_exists(template, in_tok, on_tok, trailers);
		return 1;
	}
	return 0;
}

void apply_trailer_templates(struct list_head *templates,
			     struct list_head *trailers)
{
	struct list_head *pos, *p;
	struct trailer_template *template;

	list_for_each_safe(pos, p, templates) {
		int applied = 0;
		template = list_entry(pos, struct trailer_template, list);

		list_del(pos);

		applied = find_same_and_apply_arg(template, trailers);

		if (!applied)
			maybe_add_if_missing(template, trailers);
	}
}

int trailer_set_where(const char *value, enum trailer_where *item)
{
	if (!value)
		*item = WHERE_DEFAULT;
	else if (!strcasecmp("after", value))
		*item = WHERE_AFTER;
	else if (!strcasecmp("before", value))
		*item = WHERE_BEFORE;
	else if (!strcasecmp("end", value))
		*item = WHERE_END;
	else if (!strcasecmp("start", value))
		*item = WHERE_START;
	else
		return -1;
	return 0;
}

int trailer_set_if_exists(const char *value, enum trailer_if_exists *item)
{
	if (!value)
		*item = EXISTS_DEFAULT;
	else if (!strcasecmp("addIfDifferent", value))
		*item = EXISTS_ADD_IF_DIFFERENT;
	else if (!strcasecmp("addIfDifferentNeighbor", value))
		*item = EXISTS_ADD_IF_DIFFERENT_NEIGHBOR;
	else if (!strcasecmp("add", value))
		*item = EXISTS_ADD;
	else if (!strcasecmp("replace", value))
		*item = EXISTS_REPLACE;
	else if (!strcasecmp("doNothing", value))
		*item = EXISTS_DO_NOTHING;
	else
		return -1;
	return 0;
}

int trailer_set_if_missing(const char *value, enum trailer_if_missing *item)
{
	if (!value)
		*item = MISSING_DEFAULT;
	else if (!strcasecmp("doNothing", value))
		*item = MISSING_DO_NOTHING;
	else if (!strcasecmp("add", value))
		*item = MISSING_ADD;
	else
		return -1;
	return 0;
}

void trailer_set_conf_where(enum trailer_where where,
			    struct trailer_conf *conf)
{
	conf->where = where;
}

void trailer_set_conf_if_exists(enum trailer_if_exists if_exists,
				struct trailer_conf *conf)
{
	conf->if_exists = if_exists;
}

void trailer_set_conf_if_missing(enum trailer_if_missing if_missing,
				 struct trailer_conf *conf)
{
	conf->if_missing = if_missing;
}

struct trailer_conf *new_trailer_conf(void)
{
	 return xcalloc(1, sizeof(struct trailer_conf));
}

void duplicate_trailer_conf(struct trailer_conf *dst,
			    const struct trailer_conf *src)
{
	*dst = *src;
	dst->name = xstrdup_or_null(src->name);
	dst->key = xstrdup_or_null(src->key);
	dst->command = xstrdup_or_null(src->command);
	dst->cmd = xstrdup_or_null(src->cmd);
}

void free_trailer_conf(struct trailer_conf *conf)
{
	free(conf->name);
	free(conf->key);
	free(conf->command);
	free(conf->cmd);
	free(conf);
}

static struct trailer_template *get_or_add_template_by(const char *name)
{
	struct list_head *pos;
	struct trailer_template *template;

	/* Look up template with same name. */
	list_for_each(pos, &templates_from_conf) {
		template = list_entry(pos, struct trailer_template, list);
		if (!strcasecmp(template->conf.name, name))
			return template;
	}

	/* Template does not already exist; create it. */
	CALLOC_ARRAY(template, 1);
	duplicate_trailer_conf(&template->conf, &default_trailer_conf);
	template->conf.name = xstrdup(name);

	list_add_tail(&template->list, &templates_from_conf);

	return template;
}

enum trailer_info_type { TRAILER_KEY, TRAILER_COMMAND, TRAILER_CMD,
			TRAILER_WHERE, TRAILER_IF_EXISTS, TRAILER_IF_MISSING };

static struct {
	const char *name;
	enum trailer_info_type type;
} trailer_config_items[] = {
	{ "key", TRAILER_KEY },
	{ "command", TRAILER_COMMAND },
	{ "cmd", TRAILER_CMD },
	{ "where", TRAILER_WHERE },
	{ "ifexists", TRAILER_IF_EXISTS },
	{ "ifmissing", TRAILER_IF_MISSING }
};

static int git_trailer_default_config(const char *conf_key, const char *value,
				      const struct config_context *ctx UNUSED,
				      void *cb UNUSED)
{
	const char *trailer_item, *variable_name;

	if (!skip_prefix(conf_key, "trailer.", &trailer_item))
		return 0;

	variable_name = strrchr(trailer_item, '.');
	if (!variable_name) {
		if (!strcmp(trailer_item, "where")) {
			if (trailer_set_where(value,
					      &default_trailer_conf.where) < 0)
				warning(_("unknown value '%s' for key '%s'"),
					value, conf_key);
		} else if (!strcmp(trailer_item, "ifexists")) {
			if (trailer_set_if_exists(value,
						  &default_trailer_conf.if_exists) < 0)
				warning(_("unknown value '%s' for key '%s'"),
					value, conf_key);
		} else if (!strcmp(trailer_item, "ifmissing")) {
			if (trailer_set_if_missing(value,
						   &default_trailer_conf.if_missing) < 0)
				warning(_("unknown value '%s' for key '%s'"),
					value, conf_key);
		} else if (!strcmp(trailer_item, "separators")) {
			if (!value)
				return config_error_nonbool(conf_key);
			separators = xstrdup(value);
		}
	}
	return 0;
}

static int git_trailer_config(const char *conf_key, const char *value,
			      const struct config_context *ctx UNUSED,
			      void *cb UNUSED)
{
	const char *trailer_item, *variable_name;
	struct trailer_template *template;
	struct trailer_conf *conf;
	char *name = NULL;
	enum trailer_info_type type;
	int i;

	if (!skip_prefix(conf_key, "trailer.", &trailer_item))
		return 0;

	variable_name = strrchr(trailer_item, '.');
	if (!variable_name)
		return 0;

	variable_name++;
	for (i = 0; i < ARRAY_SIZE(trailer_config_items); i++) {
		if (strcmp(trailer_config_items[i].name, variable_name))
			continue;
		name = xstrndup(trailer_item,  variable_name - trailer_item - 1);
		type = trailer_config_items[i].type;
		break;
	}

	if (!name)
		return 0;

	template = get_or_add_template_by(name);
	conf = &template->conf;
	free(name);

	switch (type) {
	case TRAILER_KEY:
		if (conf->key)
			warning(_("more than one %s"), conf_key);
		if (!value)
			return config_error_nonbool(conf_key);
		conf->key = xstrdup(value);
		break;
	case TRAILER_COMMAND:
		if (conf->command)
			warning(_("more than one %s"), conf_key);
		if (!value)
			return config_error_nonbool(conf_key);
		conf->command = xstrdup(value);
		break;
	case TRAILER_CMD:
		if (conf->cmd)
			warning(_("more than one %s"), conf_key);
		if (!value)
			return config_error_nonbool(conf_key);
		conf->cmd = xstrdup(value);
		break;
	case TRAILER_WHERE:
		if (trailer_set_where(value, &conf->where))
			warning(_("unknown value '%s' for key '%s'"), value, conf_key);
		break;
	case TRAILER_IF_EXISTS:
		if (trailer_set_if_exists(value, &conf->if_exists))
			warning(_("unknown value '%s' for key '%s'"), value, conf_key);
		break;
	case TRAILER_IF_MISSING:
		if (trailer_set_if_missing(value, &conf->if_missing))
			warning(_("unknown value '%s' for key '%s'"), value, conf_key);
		break;
	default:
		BUG("trailer.c: unhandled type %d", type);
	}
	return 0;
}

void trailer_config_init(void)
{
	if (configured)
		return;

	/* Default config must be setup first */
	default_trailer_conf.where = WHERE_END;
	default_trailer_conf.if_exists = EXISTS_ADD_IF_DIFFERENT_NEIGHBOR;
	default_trailer_conf.if_missing = MISSING_ADD;
	git_config(git_trailer_default_config, NULL);
	git_config(git_trailer_config, NULL);
	configured = 1;
}

static const char *token_from_template(struct trailer_template *template, char *tok)
{
	if (template->conf.key)
		return template->conf.key;
	if (tok)
		return tok;
	return template->conf.name;
}

static int token_matches_template(const char *tok,
				  struct trailer_template *template,
				  size_t tok_len)
{
	if (!strncasecmp(tok, template->conf.name, tok_len))
		return 1;
	return template->conf.key ? !strncasecmp(tok, template->conf.key, tok_len) : 0;
}

/*
 * If the given line is of the form
 * "<token><optional whitespace><separator>..." or "<separator>...", return the
 * location of the separator. Otherwise, return -1.  The optional whitespace
 * is allowed there primarily to allow things like "Bug #43" where <token> is
 * "Bug" and <separator> is "#".
 *
 * The separator-starts-line case (in which this function returns 0) is
 * distinguished from the non-well-formed-line case (in which this function
 * returns -1) because some callers of this function need such a distinction.
 */
ssize_t find_separator(const char *line, const char *separators)
{
	int whitespace_found = 0;
	const char *c;
	for (c = line; *c; c++) {
		if (strchr(separators, *c))
			return c - line;
		if (!whitespace_found && (isalnum(*c) || *c == '-'))
			continue;
		if (c != line && (*c == ' ' || *c == '\t')) {
			whitespace_found = 1;
			continue;
		}
		break;
	}
	return -1;
}

/*
 * Obtain the token, value, and conf from the given trailer.
 *
 * separator_pos must not be 0, since the token cannot be an empty string.
 *
 * If separator_pos is -1, interpret the whole trailer as a token.
 */
void parse_trailer(const char *line, ssize_t separator_pos,
		   struct strbuf *tok, struct strbuf *val,
		   const struct trailer_conf **conf)
{
	struct trailer_template *template;
	size_t tok_len;
	struct list_head *pos;

	if (separator_pos != -1) {
		strbuf_add(tok, line, separator_pos);
		strbuf_trim(tok);
		strbuf_addstr(val, line + separator_pos + 1);
		strbuf_trim(val);
	} else {
		strbuf_addstr(tok, line);
		strbuf_trim(tok);
	}

	/* Lookup if the token matches something in the config */
	tok_len = token_len_without_separator(tok->buf, tok->len);
	if (conf)
		*conf = &default_trailer_conf;
	list_for_each(pos, &templates_from_conf) {
		template = list_entry(pos, struct trailer_template, list);
		if (token_matches_template(tok->buf, template, tok_len)) {
			char *tok_buf = strbuf_detach(tok, NULL);
			if (conf)
				*conf = &template->conf;
			strbuf_addstr(tok, token_from_template(template, tok_buf));
			free(tok_buf);
			break;
		}
	}
}

static struct trailer_item *add_trailer_item(struct list_head *head, char *tok,
					     char *val)
{
	struct trailer_item *new_item = xcalloc(1, sizeof(*new_item));
	new_item->token = tok;
	new_item->value = val;
	list_add_tail(&new_item->list, head);
	return new_item;
}

void add_trailer_template(char *tok, char *val, const struct trailer_conf *conf,
			  struct list_head *templates)
{
	struct trailer_template *template = xcalloc(1, sizeof(*template));
	template->token = tok;
	template->value = val;
	duplicate_trailer_conf(&template->conf, conf);
	list_add_tail(&template->list, templates);
}

void parse_trailer_templates_from_config(struct list_head *config_head)
{
	struct trailer_template *template;
	struct list_head *pos;

	/*
	 * Get configured templates with a ".command" option.
	 *
	 * NEEDSWORK: If the interpret-trailers builtin sees a
	 * "trailer.foo.command = ..." setting, then the "foo" trailer will
	 * always be inserted, even if "--trailer foo" is not provided.
	 * Considering how ".command" is deprecated, it is a bit strange to see
	 * it getting special treatment like this over ".cmd". Instead, we
	 * should add a new option that explicitly lets the user decide if the
	 * configured trailer should always be added automatically, or if it
	 * should only be added if "--trailer foo" is provided (default).
	 * Then we can collect configured trailers that have either ".command"
	 * or ".cmd" below, instead of just ".command".
	 */
	list_for_each(pos, &templates_from_conf) {
		template = list_entry(pos, struct trailer_template, list);
		if (template->conf.command)
			add_trailer_template(xstrdup(token_from_template(template,
									 NULL)),
					     xstrdup(""), &template->conf, config_head);
	}
}

static const char *next_line(const char *str)
{
	const char *nl = strchrnul(str, '\n');
	return nl + !!*nl;
}

/*
 * Return the position of the start of the last line. If len is 0, return -1.
 */
static ssize_t last_line(const char *buf, size_t len)
{
	ssize_t i;
	if (len == 0)
		return -1;
	if (len == 1)
		return 0;
	/*
	 * Skip the last character (in addition to the null terminator),
	 * because if the last character is a newline, it is considered as part
	 * of the last line anyway.
	 */
	i = len - 2;

	for (; i >= 0; i--) {
		if (buf[i] == '\n')
			return i + 1;
	}
	return 0;
}

/*
 * Find the end of the log message as an offset from the start of the input
 * (where callers of this function are interested in looking for a trailers
 * block in the same input). We have to consider two categories of content that
 * can come at the end of the input which we want to ignore (because they don't
 * belong in the log message):
 *
 * (1) the "patch part" which begins with a "---" divider and has patch
 * information (like the output of git-format-patch), and
 *
 * (2) any trailing comment lines, blank lines like in the output of "git
 * commit -v", or stuff below the "cut" (scissor) line.
 *
 * As a formula, the situation looks like this:
 *
 *     INPUT = LOG MESSAGE + IGNORED
 *
 * where IGNORED can be either of the two categories described above. It may be
 * that there is nothing to ignore. Now it may be the case that the LOG MESSAGE
 * contains a trailer block, but that's not the concern of this function.
 */
static size_t find_end_of_log_message(const char *input, int no_divider)
{
	size_t end;
	const char *s;

	/* Assume the naive end of the input is already what we want. */
	end = strlen(input);

	if (no_divider)
		return end;

	/* Optionally skip over any patch part ("---" line and below). */
	for (s = input; *s; s = next_line(s)) {
		const char *v;

		if (skip_prefix(s, "---", &v) && isspace(*v)) {
			end = s - input;
			break;
		}
	}

	/* Skip over other ignorable bits. */
	return end - ignored_log_message_bytes(input, end);
}

/*
 * Return the position of the first trailer line or len if there are no
 * trailers.
 */
static size_t find_trailer_block_start(const char *buf, size_t len)
{
	const char *s;
	ssize_t end_of_title, l;
	int only_spaces = 1;
	int recognized_prefix = 0, trailer_lines = 0, non_trailer_lines = 0;
	/*
	 * Number of possible continuation lines encountered. This will be
	 * reset to 0 if we encounter a trailer (since those lines are to be
	 * considered continuations of that trailer), and added to
	 * non_trailer_lines if we encounter a non-trailer (since those lines
	 * are to be considered non-trailers).
	 */
	int possible_continuation_lines = 0;

	/* The first paragraph is the title and cannot be trailers */
	for (s = buf; s < buf + len; s = next_line(s)) {
		if (s[0] == comment_line_char)
			continue;
		if (is_blank_line(s))
			break;
	}
	end_of_title = s - buf;

	/*
	 * Get the start of the trailers by looking starting from the end for a
	 * blank line before a set of non-blank lines that (i) are all
	 * trailers, or (ii) contains at least one Git-generated trailer and
	 * consists of at least 25% trailers.
	 */
	for (l = last_line(buf, len);
	     l >= end_of_title;
	     l = last_line(buf, l)) {
		const char *bol = buf + l;
		const char **p;
		ssize_t separator_pos;

		if (bol[0] == comment_line_char) {
			non_trailer_lines += possible_continuation_lines;
			possible_continuation_lines = 0;
			continue;
		}
		if (is_blank_line(bol)) {
			if (only_spaces)
				continue;
			non_trailer_lines += possible_continuation_lines;
			if (recognized_prefix &&
			    trailer_lines * 3 >= non_trailer_lines)
				return next_line(bol) - buf;
			else if (trailer_lines && !non_trailer_lines)
				return next_line(bol) - buf;
			return len;
		}
		only_spaces = 0;

		for (p = git_generated_prefixes; *p; p++) {
			if (starts_with(bol, *p)) {
				trailer_lines++;
				possible_continuation_lines = 0;
				recognized_prefix = 1;
				goto continue_outer_loop;
			}
		}

		separator_pos = find_separator(bol, separators);
		if (separator_pos >= 1 && !isspace(bol[0])) {
			struct list_head *pos;

			trailer_lines++;
			possible_continuation_lines = 0;
			if (recognized_prefix)
				continue;
			/*
			 * The templates here are not used for actually
			 * adding trailers anywhere, but instead to help us
			 * identify trailer lines by comparing their keys with
			 * those found in configured templates.
			 */
			list_for_each(pos, &templates_from_conf) {
				struct trailer_template *template;
				template = list_entry(pos, struct trailer_template, list);
				if (token_matches_template(bol, template,
						       separator_pos)) {
					recognized_prefix = 1;
					break;
				}
			}
		} else if (isspace(bol[0]))
			possible_continuation_lines++;
		else {
			non_trailer_lines++;
			non_trailer_lines += possible_continuation_lines;
			possible_continuation_lines = 0;
		}
continue_outer_loop:
		;
	}

	return len;
}

static int ends_with_blank_line(const char *buf, size_t len)
{
	ssize_t ll = last_line(buf, len);
	if (ll < 0)
		return 0;
	return is_blank_line(buf + ll);
}

static void unfold_value(struct strbuf *val)
{
	struct strbuf out = STRBUF_INIT;
	size_t i;

	strbuf_grow(&out, val->len);
	i = 0;
	while (i < val->len) {
		char c = val->buf[i++];
		if (c == '\n') {
			/* Collapse continuation down to a single space. */
			while (i < val->len && isspace(val->buf[i]))
				i++;
			strbuf_addch(&out, ' ');
		} else {
			strbuf_addch(&out, c);
		}
	}

	/* Empty lines may have left us with whitespace cruft at the edges */
	strbuf_trim(&out);

	/* output goes back to val as if we modified it in-place */
	strbuf_swap(&out, val);
	strbuf_release(&out);
}

static struct trailer_block *trailer_block_new(void)
{
	struct trailer_block *trailer_block = xcalloc(1, sizeof(*trailer_block));
	return trailer_block;
}

static struct trailer_block *trailer_block_get(const struct process_trailer_options *opts,
					       const char *str)
{
	struct trailer_block *trailer_block = trailer_block_new();
	size_t end_of_log_message = 0, trailer_block_start = 0;
	struct strbuf **trailer_lines, **ptr;
	char **trailer_strings = NULL;
	size_t nr = 0, alloc = 0;
	char **last = NULL;

	trailer_config_init();

	end_of_log_message = find_end_of_log_message(str, opts->no_divider);
	trailer_block_start = find_trailer_block_start(str, end_of_log_message);

	trailer_lines = strbuf_split_buf(str + trailer_block_start,
					 end_of_log_message - trailer_block_start,
					 '\n',
					 0);
	for (ptr = trailer_lines; *ptr; ptr++) {
		if (last && isspace((*ptr)->buf[0])) {
			struct strbuf sb = STRBUF_INIT;
			strbuf_attach(&sb, *last, strlen(*last), strlen(*last));
			strbuf_addbuf(&sb, *ptr);
			*last = strbuf_detach(&sb, NULL);
			continue;
		}
		ALLOC_GROW(trailer_strings, nr + 1, alloc);
		trailer_strings[nr] = strbuf_detach(*ptr, NULL);
		last = find_separator(trailer_strings[nr], separators) >= 1
			? &trailer_strings[nr]
			: NULL;
		nr++;
	}
	strbuf_list_free(trailer_lines);

	trailer_block->blank_line_before_trailer = ends_with_blank_line(str,
									trailer_block_start);
	trailer_block->start = trailer_block_start;
	trailer_block->end = end_of_log_message;
	trailer_block->trailers = trailer_strings;
	trailer_block->trailer_nr = nr;

	return trailer_block;
}

/*
 * Parse trailers in "str", populating the trailer_block info and "head" linked
 * list structure.
 */
struct trailer_block *parse_trailers(const struct process_trailer_options *opts,
				     const char *str,
				     struct list_head *head)
{
	struct trailer_block *trailer_block;
	struct strbuf tok = STRBUF_INIT;
	struct strbuf val = STRBUF_INIT;
	size_t i;

	trailer_block = trailer_block_get(opts, str);

	for (i = 0; i < trailer_block->trailer_nr; i++) {
		int separator_pos;
		char *line = trailer_block->trailers[i];
		if (line[0] == comment_line_char)
			continue;
		separator_pos = find_separator(line, separators);
		if (separator_pos >= 1) {
			parse_trailer(line, separator_pos, &tok, &val, NULL);
			if (opts->unfold)
				unfold_value(&val);
			add_trailer_item(head,
					 strbuf_detach(&tok, NULL),
					 strbuf_detach(&val, NULL));
		} else if (!opts->only_trailers) {
			strbuf_addstr(&val, line);
			strbuf_strip_suffix(&val, "\n");
			add_trailer_item(head,
					 NULL,
					 strbuf_detach(&val, NULL));
		}
	}

	return trailer_block;
}

void free_trailers(struct list_head *trailers)
{
	struct list_head *pos, *p;
	list_for_each_safe(pos, p, trailers) {
		list_del(pos);
		free_trailer_item(list_entry(pos, struct trailer_item, list));
	}
}

void free_trailer_templates(struct list_head *trailer_templates)
{
	struct list_head *pos, *p;

	list_for_each_safe(pos, p, trailer_templates) {
		list_del(pos);
		free_template(list_entry(pos, struct trailer_template, list));
	}
}

size_t trailer_block_start(struct trailer_block *trailer_block)
{
	return trailer_block->start;
}

size_t trailer_block_end(struct trailer_block *trailer_block)
{
	return trailer_block->end;
}

int blank_line_before_trailer_block(struct trailer_block *trailer_block)
{
	return trailer_block->blank_line_before_trailer;
}

void trailer_block_release(struct trailer_block *trailer_block)
{
	size_t i;
	for (i = 0; i < trailer_block->trailer_nr; i++)
		free(trailer_block->trailers[i]);
	free(trailer_block->trailers);
	free(trailer_block);
}

void format_trailers(const struct process_trailer_options *opts,
		     struct list_head *trailers,
		     struct strbuf *out)
{
	size_t origlen = out->len;
	struct list_head *pos;
	struct trailer_item *item;

	list_for_each(pos, trailers) {
		item = list_entry(pos, struct trailer_item, list);
		if (item->token) {
			struct strbuf tok = STRBUF_INIT;
			struct strbuf val = STRBUF_INIT;
			strbuf_addstr(&tok, item->token);
			strbuf_addstr(&val, item->value);

			/*
			 * Skip key/value pairs where the value was empty. This
			 * can happen from trailers specified without a
			 * separator, like `--trailer "Reviewed-by"` (no
			 * corresponding value).
			 */
			if (opts->trim_empty && !strlen(item->value))
				continue;

			if (!opts->filter || opts->filter(&tok, opts->filter_data)) {
				if (opts->separator && out->len != origlen)
					strbuf_addbuf(out, opts->separator);
				if (!opts->value_only)
					strbuf_addbuf(out, &tok);
				if (!opts->key_only && !opts->value_only) {
					if (opts->key_value_separator)
						strbuf_addbuf(out, opts->key_value_separator);
					else {
						char c = last_non_space_char(tok.buf);
						if (c && !strchr(separators, c))
							strbuf_addf(out, "%c ", separators[0]);
					}
				}
				if (!opts->key_only)
					strbuf_addbuf(out, &val);
				if (!opts->separator)
					strbuf_addch(out, '\n');
			}
			strbuf_release(&tok);
			strbuf_release(&val);

		} else if (!opts->only_trailers) {
			if (opts->separator && out->len != origlen) {
				strbuf_addbuf(out, opts->separator);
			}
			strbuf_addstr(out, item->value);
			if (opts->separator)
				strbuf_rtrim(out);
			else
				strbuf_addch(out, '\n');
		}
	}
}

void format_trailers_from_commit(const struct process_trailer_options *opts,
				 const char *msg,
				 struct strbuf *out)
{
	LIST_HEAD(trailers);
	struct trailer_block *trailer_block = parse_trailers(opts, msg, &trailers);

	/* If we want the whole block untouched, we can take the fast path. */
	if (!opts->only_trailers && !opts->unfold && !opts->filter &&
	    !opts->separator && !opts->key_only && !opts->value_only &&
	    !opts->key_value_separator) {
		strbuf_add(out, msg + trailer_block->start,
			   trailer_block->end - trailer_block->start);
	} else
		format_trailers(opts, &trailers, out);

	free_trailers(&trailers);
	trailer_block_release(trailer_block);
}

void trailer_iterator_init(struct trailer_iterator *iter, const char *msg)
{
	struct process_trailer_options opts = PROCESS_TRAILER_OPTIONS_INIT;
	strbuf_init(&iter->key, 0);
	strbuf_init(&iter->val, 0);
	opts.no_divider = 1;
	iter->internal.trailer_block = trailer_block_get(&opts, msg);
	iter->internal.cur = 0;
}

int trailer_iterator_advance(struct trailer_iterator *iter)
{
	if (iter->internal.cur < iter->internal.trailer_block->trailer_nr) {
		char *line = iter->internal.trailer_block->trailers[iter->internal.cur++];
		int separator_pos = find_separator(line, separators);

		iter->raw = line;
		strbuf_reset(&iter->key);
		strbuf_reset(&iter->val);
		parse_trailer(line, separator_pos, &iter->key, &iter->val, NULL);
		/* Always unfold values during iteration. */
		unfold_value(&iter->val);
		return 1;
	}
	return 0;
}

void trailer_iterator_release(struct trailer_iterator *iter)
{
	trailer_block_release(iter->internal.trailer_block);
	strbuf_release(&iter->val);
	strbuf_release(&iter->key);
}

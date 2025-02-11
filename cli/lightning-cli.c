/*
 * Helper to submit via JSON-RPC and get back response.
 */
#include "config.h"
#include <assert.h>
#include <ccan/asort/asort.h>
#include <ccan/err/err.h>
#include <ccan/opt/opt.h>
#include <ccan/read_write_all/read_write_all.h>
#include <ccan/str/str.h>
#include <ccan/tal/str/str.h>
#include <common/configdir.h>
#include <common/json.h>
#include <common/json_command.h>
#include <common/json_escaped.h>
#include <common/memleak.h>
#include <common/utils.h>
#include <common/version.h>
#include <libgen.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define NO_ERROR 0
#define ERROR_FROM_LIGHTNINGD 1
#define ERROR_TALKING_TO_LIGHTNINGD 2
#define ERROR_USAGE 3

/* Tal wrappers for opt. */
static void *opt_allocfn(size_t size)
{
	return tal_arr_label(NULL, char, size, TAL_LABEL("opt_allocfn", ""));
}

static void *tal_reallocfn(void *ptr, size_t size)
{
	if (!ptr)
		return opt_allocfn(size);
	tal_resize_(&ptr, 1, size, false);
	return ptr;
}

static void tal_freefn(void *ptr)
{
	tal_free(ptr);
}

struct netaddr;

/* Returns number of tokens digested */
static size_t human_readable(const char *buffer, const jsmntok_t *t, char term)
{
	size_t i, n;

	switch (t->type) {
	case JSMN_PRIMITIVE:
	case JSMN_STRING:
		for (i = t->start; i < t->end; i++) {
			/* We only translate \n and \t. */
			if (buffer[i] == '\\' && i + 1 < t->end) {
				if (buffer[i+1] == 'n') {
					fputc('\n', stdout);
					i++;
					continue;
				} else if (buffer[i+1] == 't') {
					fputc('\t', stdout);
					i++;
					continue;
				}
			}
			fputc(buffer[i], stdout);
		}
		fputc(term, stdout);
		return 1;
	case JSMN_ARRAY:
		n = 1;
		for (i = 0; i < t->size; i++)
			n += human_readable(buffer, t + n, '\n');
		return n;
	case JSMN_OBJECT:
		/* Elide single-field objects */
		if (t->size == 1)
			return human_readable(buffer, t + 2, '\n') + 3;
		n = 1;
		for (i = 0; i < t->size; i++) {
			n += human_readable(buffer, t + n, '=');
			n += human_readable(buffer, t + n, '\n');
		}
		return n;
	case JSMN_UNDEFINED:
		break;
	}
	abort();
}

static int compare_tok(const jsmntok_t *a, const jsmntok_t *b,
		       const char *buffer)
{
	int a_len = a->end - a->start, b_len = b->end - b->start, min_len, cmp;

	if (a_len > b_len)
		min_len = b_len;
	else
		min_len = a_len;

	cmp = memcmp(buffer + a->start, buffer + b->start, min_len);
	if (cmp != 0)
		return cmp;
	/* If equal, shorter one wins. */
	return a_len - b_len;
}

static int compare_help(const jsmntok_t *const *a,
			const jsmntok_t *const *b,
			char *buffer)
{
	const jsmntok_t *cat_a, *cat_b;
	bool a_is_developer, b_is_developer;
	int cmp;

	cat_a = json_get_member(buffer, *a, "category");
	cat_b = json_get_member(buffer, *b, "category");

	/* Just in case it's an older lightningd! */
	if (!cat_a)
		goto same_category;

	/* We always tweak "developer" category to last. */
	a_is_developer = json_tok_streq(buffer, cat_a, "developer");
	b_is_developer = json_tok_streq(buffer, cat_b, "developer");

	if (a_is_developer && b_is_developer)
		cmp = 0;
	else if (a_is_developer)
		cmp = 1;
	else if (b_is_developer)
		cmp = -1;
	else
		/* Otherwise we order category alphabetically. */
		cmp = compare_tok(cat_a, cat_b, buffer);

	if (cmp != 0)
		return cmp;

	/* After category, we order by name */
same_category:
	return compare_tok(json_get_member(buffer, *a, "command"),
			   json_get_member(buffer, *b, "command"),
			   buffer);
}

static void human_help(char *buffer, const jsmntok_t *result, bool has_command) {
	unsigned int i;
	/* `curr` is used as a temporary token */
	const jsmntok_t *curr;
	const char *prev_cat;
	/* Contains all commands objects, which have the following structure :
	 * {
	 *     "command": "The command name and usage",
	 *     "category": "The command category",
	 *     "description": "The command's description",
	 *     "verbose": "The command's detailed description"
	 * }
	 */
	const jsmntok_t * help_array = json_get_member(buffer, result, "help");
	const jsmntok_t **help = tal_arr(NULL, const jsmntok_t *,
					 help_array->size);

	/* Populate an array for easy sorting with asort */
	json_for_each_arr(i, curr, help_array)
		help[i] = curr;

	asort(help, tal_count(help), compare_help, buffer);

	prev_cat = "";
	for (i = 0; i < tal_count(help); i++) {
		const jsmntok_t *category, *command, *desc;

		category = json_get_member(buffer, help[i], "category");
		if (category && !json_tok_streq(buffer, category, prev_cat)) {
			prev_cat = json_strdup(help, buffer, category);
			if (!has_command)
				printf("=== %s ===\n\n", prev_cat);
		}

		command = json_get_member(buffer, help[i], "command");
		desc = json_get_member(buffer, help[i], "description");
		printf("%.*s\n",
		       command->end - command->start, buffer + command->start);
		printf("    %.*s\n\n",
		       desc->end - desc->start, buffer + desc->start);
	}
	tal_free(help);

	if (!has_command)
		printf("---\nrun `lightning-cli help <command>` for more information on a specific command\n");
}

enum format {
	JSON,
	HUMAN,
	DEFAULT_FORMAT,
	RAW
};

static char *opt_set_human(enum format *format)
{
	*format = HUMAN;
	return NULL;
}

static char *opt_set_json(enum format *format)
{
	*format = JSON;
	return NULL;
}

static char *opt_set_raw(enum format *format)
{
	*format = RAW;
	return NULL;
}

enum input {
	KEYWORDS,
	ORDERED,
	DEFAULT_INPUT
};

static char *opt_set_keywords(enum input *input)
{
	*input = KEYWORDS;
	return NULL;
}

static char *opt_set_ordered(enum input *input)
{
	*input = ORDERED;
	return NULL;
}

static bool is_literal(const char *arg)
{
	size_t arglen = strlen(arg);
	if (arglen == 0) {
		return false;
	}
	return strspn(arg, "0123456789") == arglen
		|| streq(arg, "true")
		|| streq(arg, "false")
		|| streq(arg, "null")
		|| (arg[0] == '{' && arg[arglen - 1] == '}')
		|| (arg[0] == '[' && arg[arglen - 1] == ']')
		|| (arg[0] == '"' && arg[arglen - 1] == '"');
}

static void add_input(char **cmd, const char *input,
		      int i, int argc)
{
	/* Numbers, bools, objects and arrays are left unquoted,
	 * and quoted things left alone. */
	if (is_literal(input))
		tal_append_fmt(cmd, "%s", input);
	else
		tal_append_fmt(cmd, "\"%s\"", json_escape(*cmd, input)->s);
	if (i != argc - 1)
		tal_append_fmt(cmd, ", ");
}

static void
try_exec_man (const char *page, char *relative_to) {
	int status;

	switch (fork()) {
	case -1:
		err(1, "Forking man command");
	case 0:
		/* child, run man command. */
		if (relative_to != NULL) {
			page = tal_fmt(page, "%s/../doc/%s.7", relative_to, page);
			execlp("man", "man", "-l", page, (char *)NULL);
		}
		else {
			execlp("man", "man", page, (char *)NULL);
		}

		err(1, "Running man command");
	default:
		break;
	}

	wait(&status);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		exit(0);
}

static void print_json(const char *str, const jsmntok_t *tok, const char *indent)
{
	size_t i;
	const jsmntok_t *t;
	bool first;
	char next_indent[strlen(indent) + 3 + 1];

	memset(next_indent, ' ', sizeof(next_indent)-1);
	next_indent[sizeof(next_indent)-1] = '\0';

	switch (tok->type) {
	case JSMN_PRIMITIVE:
	case JSMN_STRING:
		printf("%.*s", json_tok_full_len(tok), json_tok_full(str, tok));
		return;

	case JSMN_ARRAY:
		first = true;
		json_for_each_arr(i, t, tok) {
			if (first)
				printf("[\n%s", next_indent);
			else
				printf(",\n%s", next_indent);
			print_json(str, t, next_indent);
			first = false;
		}
		if (first)
			printf("[]");
		else
			printf("\n%s]", indent);
		return;

	case JSMN_OBJECT:
		first = true;
		json_for_each_obj(i, t, tok) {
			if (first)
				printf("{\n%s", next_indent);
			else
				printf(",\n%s", next_indent);
			print_json(str, t, next_indent);
			printf(" : ");
			print_json(str, t + 1, next_indent);
			first = false;
		}
		if (first)
			printf("{}");
		else
			printf("\n%s}", indent);
		return;
	case JSMN_UNDEFINED:
		break;
	}
	abort();
}

/* Always returns a positive number < len.  len must be > 0! */
static size_t read_nofail(int fd, void *buf, size_t len)
{
	ssize_t i;
	assert(len > 0);

	i = read(fd, buf, len);
	if (i == 0)
		errx(ERROR_TALKING_TO_LIGHTNINGD,
		     "reading response: socket closed");
	else if (i < 0)
		err(ERROR_TALKING_TO_LIGHTNINGD, "reading response");
	return i;
}

/* We rely on the fact that lightningd terminates all JSON RPC responses with
 * "\n\n", so we can stream even if we can't parse. */
static void oom_dump(int fd, char *resp, size_t resp_len, size_t off)
{
	warnx("Out of memory: sending raw output");

	/* Note: resp does not already end in '\n\n', and resp_len is > 0 */
	do {
		/* Keep last char, to avoid splitting \n\n */
		write_all(STDOUT_FILENO, resp, off-1);
		resp[0] = resp[off-1];
		off = 1 + read_nofail(fd, resp + 1, resp_len - 1);
	} while (resp[off-2] != '\n' || resp[off-1] != '\n');
	write_all(STDOUT_FILENO, resp, off-1);
	/* We assume giant answer means "success" */
	exit(0);
}

int main(int argc, char *argv[])
{
	setup_locale();

	int fd, i;
	size_t off;
	const char *method;
	char *cmd, *resp, *idstr, *rpc_filename;
	struct sockaddr_un addr;
	jsmntok_t *toks;
	const jsmntok_t *result, *error, *id;
	char *lightning_dir;
	const tal_t *ctx = tal(NULL, char);
	jsmn_parser parser;
	int parserr;
	enum format format = DEFAULT_FORMAT;
	enum input input = DEFAULT_INPUT;
	char *command = NULL;
	size_t resp_len, num_toks;

	err_set_progname(argv[0]);
	jsmn_init(&parser);

	opt_set_alloc(opt_allocfn, tal_reallocfn, tal_freefn);
	configdir_register_opts(ctx, &lightning_dir, &rpc_filename);

	opt_register_noarg("--help|-h", opt_usage_and_exit,
			   "<command> [<params>...]", "Show this message. Use the command help (without hyphens -- \"lightning-cli help\") to get a list of all RPC commands");
	opt_register_noarg("-H|--human-readable", opt_set_human, &format,
			   "Human-readable output (default for 'help')");
	opt_register_noarg("-J|--json", opt_set_json, &format,
			   "JSON output (default unless 'help')");
	opt_register_noarg("-R|--raw", opt_set_raw, &format,
			   "Raw, unformatted JSON output");
	opt_register_noarg("-k|--keywords", opt_set_keywords, &input,
			   "Use format key=value for <params>");
	opt_register_noarg("-o|--order", opt_set_ordered, &input,
			   "Use params in order for <params>");

	opt_register_version();

	opt_early_parse(argc, argv, opt_log_stderr_exit);
	opt_parse(&argc, argv, opt_log_stderr_exit);

	method = argv[1];
	if (!method) {
		char *usage = opt_usage(argv[0], NULL);
		printf("%s\n", usage);
		tal_free(usage);
		printf("Querying lightningd for available RPC commands (\"lightning-cli help\"):\n\n");
		method = "help";
	}

	if (format == DEFAULT_FORMAT) {
		if (streq(method, "help"))
			format = HUMAN;
		else
			format = JSON;
	}

	/* Launch a manpage if we have a help command with an argument. We do
	 * not need to have lightningd running in this case. */
	if (streq(method, "help") && format == HUMAN && argc >= 3) {
		command = argv[2];
		char *page = tal_fmt(ctx, "lightning-%s", command);

		try_exec_man(page, NULL);

		/* Try to find the page relative to this executable.
		 * This handles the common scenario where lightning-cli
		 * was built from source and hasn't been installed yet */
		try_exec_man(page, dirname(argv[0]));

		tal_free(page);
	}

	if (chdir(lightning_dir) != 0)
		err(ERROR_TALKING_TO_LIGHTNINGD, "Moving into '%s'",
		    lightning_dir);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (strlen(rpc_filename) + 1 > sizeof(addr.sun_path))
		errx(ERROR_USAGE, "rpc filename '%s' too long", rpc_filename);
	strcpy(addr.sun_path, rpc_filename);
	addr.sun_family = AF_UNIX;

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
		err(ERROR_TALKING_TO_LIGHTNINGD,
		    "Connecting to '%s'", rpc_filename);

	idstr = tal_fmt(ctx, "lightning-cli-%i", getpid());
	cmd = tal_fmt(ctx,
		      "{ \"jsonrpc\" : \"2.0\", \"method\" : \"%s\", \"id\" : \"%s\", \"params\" :",
		      json_escape(ctx, method)->s, idstr);

	if (input == DEFAULT_INPUT) {
		/* Hacky autodetect; only matters if more than single arg */
		if (argc > 2 && strchr(argv[2], '='))
			input = KEYWORDS;
		else
			input = ORDERED;
	}

	if (input == KEYWORDS) {
		tal_append_fmt(&cmd, "{ ");
		for (i = 2; i < argc; i++) {
			const char *eq = strchr(argv[i], '=');

			if (!eq)
				err(ERROR_USAGE, "Expected key=value in '%s'",
				    argv[i]);

			tal_append_fmt(&cmd, "\"%.*s\" : ",
				       (int)(eq - argv[i]), argv[i]);

			add_input(&cmd, eq + 1, i, argc);
		}
		tal_append_fmt(&cmd, "} }");
	} else {
		tal_append_fmt(&cmd, "[ ");
		for (i = 2; i < argc; i++)
			add_input(&cmd, argv[i], i, argc);
		tal_append_fmt(&cmd, "] }");
	}

	if (!write_all(fd, cmd, strlen(cmd)))
		err(ERROR_TALKING_TO_LIGHTNINGD, "Writing command");

	/* Start with 1000 characters, 100 tokens. */
	resp_len = 1000;
	resp = malloc(resp_len);
	num_toks = 100;
	toks = malloc(sizeof(jsmntok_t) * num_toks);

	off = 0;
	parserr = 0;
	while (parserr <= 0) {
		/* Read more if parser says, or we have 0 tokens. */
		if (parserr == 0 || parserr == JSMN_ERROR_PART) {
			ssize_t i = read(fd, resp + off, resp_len - 1 - off);
			if (i == 0)
				errx(ERROR_TALKING_TO_LIGHTNINGD,
				     "reading response: socket closed");
			else if (i < 0)
				err(ERROR_TALKING_TO_LIGHTNINGD,
				    "reading response");
			off += i;
			/* NUL terminate */
			resp[off] = '\0';
		}

		/* (Continue) parsing */
		parserr = jsmn_parse(&parser, resp, off, toks, num_toks);

		switch (parserr) {
		case JSMN_ERROR_INVAL:
			errx(ERROR_TALKING_TO_LIGHTNINGD,
			     "Malformed response '%s'", resp);
		case JSMN_ERROR_NOMEM: {
			/* Need more tokens, double it */
			jsmntok_t *newtoks = realloc(toks,
						     sizeof(jsmntok_t)
						     * num_toks * 2);
			if (!newtoks)
				oom_dump(fd, resp, resp_len, off);
			toks = newtoks;
			num_toks *= 2;
			break;
		}
		case JSMN_ERROR_PART:
			/* Need more data: make room if necessary */
			if (off == resp_len - 1) {
				char *newresp = realloc(resp, resp_len * 2);
				if (!newresp)
					oom_dump(fd, resp, resp_len, off);
				resp = newresp;
				resp_len *= 2;
			}
			break;
		}
	}

	if (toks->type != JSMN_OBJECT)
		errx(ERROR_TALKING_TO_LIGHTNINGD,
		     "Non-object response '%s'", resp);

	result = json_get_member(resp, toks, "result");
	error = json_get_member(resp, toks, "error");
	if (!error && !result)
		errx(ERROR_TALKING_TO_LIGHTNINGD,
		     "Either 'result' or 'error' must be returned in response '%s'", resp);
	id = json_get_member(resp, toks, "id");
	if (!id)
		errx(ERROR_TALKING_TO_LIGHTNINGD,
		     "Missing 'id' in response '%s'", resp);
	if (!json_tok_streq(resp, id, idstr))
		errx(ERROR_TALKING_TO_LIGHTNINGD,
		     "Incorrect 'id' in response: %.*s",
		     json_tok_full_len(id), json_tok_full(resp, id));

	if (!error || json_tok_is_null(resp, error)) {
		// if we have specific help command
		if (format == HUMAN)
			if (streq(method, "help") && command == NULL)
				human_help(resp, result, false);
			else
				human_readable(resp, result, '\n');
		else if (format == RAW)
			printf("%.*s\n",
			       json_tok_full_len(result),
			       json_tok_full(resp, result));
		else {
			print_json(resp, result, "");
			printf("\n");
		}
		tal_free(lightning_dir);
		tal_free(rpc_filename);
		tal_free(ctx);
		opt_free_table();
		free(resp);
		free(toks);
		return 0;
	}

	if (format == RAW)
		printf("%.*s\n",
		       json_tok_full_len(error), json_tok_full(resp, error));
	else {
		print_json(resp, error, "");
		printf("\n");
	}
	tal_free(lightning_dir);
	tal_free(rpc_filename);
	tal_free(ctx);
	opt_free_table();
	free(resp);
	free(toks);
	return 1;
}

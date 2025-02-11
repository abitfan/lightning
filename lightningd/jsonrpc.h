#ifndef LIGHTNING_LIGHTNINGD_JSONRPC_H
#define LIGHTNING_LIGHTNINGD_JSONRPC_H
#include "config.h"
#include <bitcoin/chainparams.h>
#include <ccan/autodata/autodata.h>
#include <ccan/list/list.h>
#include <common/json.h>
#include <lightningd/json_stream.h>
#include <stdarg.h>

struct jsonrpc;

/* The command mode tells param() how to process. */
enum command_mode {
	/* Normal command processing */
	CMD_NORMAL,
	/* Create command usage string, nothing else. */
	CMD_USAGE,
	/* Check parameters, nothing else. */
	CMD_CHECK
};

/* Context for a command (from JSON, but might outlive the connection!). */
/* FIXME: move definition into jsonrpc.c */
struct command {
	/* Off json_cmd->commands */
	struct list_node list;
	/* The global state */
	struct lightningd *ld;
	/* The 'id' which we need to include in the response. */
	const char *id;
	/* What command we're running (for logging) */
	const struct json_command *json_cmd;
	/* The connection, or NULL if it closed. */
	struct json_connection *jcon;
	/* Have we been marked by command_still_pending?  For debugging... */
	bool pending;
	/* Tell param() how to process the command */
	enum command_mode mode;
	/* Have we started a json stream already?  For debugging. */
	bool have_json_stream;
};

/**
 * Dummy structure to make sure you call one of
 * command_success / command_failed / command_still_pending.
 */
struct command_result;

struct json_command {
	const char *name;
	const char *category;
	struct command_result *(*dispatch)(struct command *,
					   const char *buffer,
					   const jsmntok_t *obj,
					   const jsmntok_t *params);
	const char *description;
	bool deprecated;
	const char *verbose;
};

struct jsonrpc_notification {
	/* The topic that this notification is for. Internally this
	 * will be serialized as "method", hence the different name
	 * here */
	const char *method;
	struct json_stream *stream;
};

struct jsonrpc_request {
	u64 id;
	const char *method;
	struct json_stream *stream;
	void (*response_cb)(const char *buffer, const jsmntok_t *toks,
			    const jsmntok_t *idtok, void *);
	void *response_cb_arg;
};

/**
 * json_stream_success - start streaming a successful json result.
 * @cmd: the command we're running.
 *
 * The returned value should go to command_success() when done.
 * json_add_* will be placed into the 'result' field of the JSON reply.
 */
struct json_stream *json_stream_success(struct command *cmd);

/**
 * json_stream_fail - start streaming a failed json result.
 * @cmd: the command we're running.
 * @code: the error code from common/jsonrpc_errors.h
 * @errmsg: the error string.
 *
 * The returned value should go to command_failed() when done;
 * json_add_* will be placed into the 'data' field of the 'error' JSON reply.
 */
struct json_stream *json_stream_fail(struct command *cmd,
				     int code,
				     const char *errmsg);

/**
 * json_stream_fail_nodata - start streaming a failed json result.
 * @cmd: the command we're running.
 * @code: the error code from common/jsonrpc_errors.h
 * @errmsg: the error string.
 *
 * This is used by command_fail(), which doesn't add any JSON data.
 */
struct json_stream *json_stream_fail_nodata(struct command *cmd,
					    int code,
					    const char *errmsg);

struct json_stream *null_response(struct command *cmd);

/* These returned values are never NULL. */
struct command_result *command_success(struct command *cmd,
				       struct json_stream *response)
	 WARN_UNUSED_RESULT;
struct command_result *command_failed(struct command *cmd,
				      struct json_stream *result)
	 WARN_UNUSED_RESULT;

/* Mainly for documentation, that we plan to close this later. */
struct command_result *command_still_pending(struct command *cmd)
	 WARN_UNUSED_RESULT;

/* For low-level JSON stream access: */
struct json_stream *json_stream_raw_for_cmd(struct command *cmd);
void json_stream_log_suppress_for_cmd(struct json_stream *js,
					    const struct command *cmd);
struct command_result *command_raw_complete(struct command *cmd,
					    struct json_stream *result);

/* To return if param() fails. */
extern struct command_result *command_param_failed(void)
	 WARN_UNUSED_RESULT;

/* Wrapper for pending commands (ignores return) */
static inline void was_pending(const struct command_result *res)
{
	assert(res);
}

/* Transition for ignoring command */
static inline void fixme_ignore(const struct command_result *res)
{
}

/* FIXME: For the few cases where return value is indeterminate */
struct command_result *command_its_complicated(const char *why);

/**
 * Create a new jsonrpc to wrap all related information.
 *
 * This doesn't setup the listener yet, see `jsonrpc_listen` for
 * that. This just creates the container for all jsonrpc-related
 * information so we can start gathering it before actually starting.
 *
 * It initializes ld->jsonrpc.
 */
void jsonrpc_setup(struct lightningd *ld);


/**
 * Start listeing on ld->rpc_filename.
 *
 * Sets up the listener effectively starting the RPC interface.
 */
void jsonrpc_listen(struct jsonrpc *rpc, struct lightningd *ld);

/**
 * Add a new command/method to the JSON-RPC interface.
 *
 * Returns true if the command was added correctly, false if adding
 * this would clobber a command name.
 *
 * Free @command to remove it.
 */
bool jsonrpc_command_add(struct jsonrpc *rpc, struct json_command *command,
			 const char *usage TAKES);

/**
 * Begin a JSON-RPC notification with the specified topic.
 *
 * Automatically starts the `params` object, hence only key-value
 * based params are supported at the moment.
 */
struct jsonrpc_notification *jsonrpc_notification_start(const tal_t *ctx, const char *topic);

/**
 * Counterpart to jsonrpc_notification_start.
 */
void jsonrpc_notification_end(struct jsonrpc_notification *n);

#define jsonrpc_request_start(ctx, method, log, response_cb, response_cb_arg) \
	jsonrpc_request_start_(					\
		(ctx), (method), (log),					\
	    typesafe_cb_preargs(void, void *, (response_cb), (response_cb_arg),	\
				const char *buffer,		\
				const jsmntok_t *toks,		\
				const jsmntok_t *idtok),	\
	    (response_cb_arg))

struct jsonrpc_request *jsonrpc_request_start_(
    const tal_t *ctx, const char *method, struct log *log,
    void (*response_cb)(const char *buffer, const jsmntok_t *toks,
			const jsmntok_t *idtok, void *),
    void *response_cb_arg);

void jsonrpc_request_end(struct jsonrpc_request *request);

AUTODATA_TYPE(json_command, struct json_command);

#if DEVELOPER
struct htable;
struct jsonrpc;

void jsonrpc_remove_memleak(struct htable *memtable,
			    const struct jsonrpc *jsonrpc);
#endif /* DEVELOPER */

#endif /* LIGHTNING_LIGHTNINGD_JSONRPC_H */

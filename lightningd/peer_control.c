#include "lightningd.h"
#include "peer_control.h"
#include "subd.h"
#include <arpa/inet.h>
#include <bitcoin/feerate.h>
#include <bitcoin/script.h>
#include <bitcoin/tx.h>
#include <ccan/array_size/array_size.h>
#include <ccan/io/io.h>
#include <ccan/noerr/noerr.h>
#include <ccan/str/str.h>
#include <ccan/take/take.h>
#include <ccan/tal/str/str.h>
#include <channeld/gen_channel_wire.h>
#include <common/dev_disconnect.h>
#include <common/features.h>
#include <common/initial_commit_tx.h>
#include <common/json_command.h>
#include <common/json_helpers.h>
#include <common/jsonrpc_errors.h>
#include <common/key_derive.h>
#include <common/param.h>
#include <common/per_peer_state.h>
#include <common/status.h>
#include <common/timeout.h>
#include <common/version.h>
#include <common/wire_error.h>
#include <connectd/gen_connect_wire.h>
#include <errno.h>
#include <fcntl.h>
#include <hsmd/gen_hsm_wire.h>
#include <inttypes.h>
#include <lightningd/bitcoind.h>
#include <lightningd/chaintopology.h>
#include <lightningd/channel_control.h>
#include <lightningd/closing_control.h>
#include <lightningd/connect_control.h>
#include <lightningd/hsm_control.h>
#include <lightningd/json.h>
#include <lightningd/jsonrpc.h>
#include <lightningd/log.h>
#include <lightningd/memdump.h>
#include <lightningd/notification.h>
#include <lightningd/onchain_control.h>
#include <lightningd/opening_control.h>
#include <lightningd/options.h>
#include <lightningd/peer_htlcs.h>
#include <lightningd/plugin_hook.h>
#include <unistd.h>
#include <wally_bip32.h>
#include <wire/gen_onion_wire.h>
#include <wire/wire_sync.h>

struct close_command {
	/* Inside struct lightningd close_commands. */
	struct list_node list;
	/* Command structure. This is the parent of the close command. */
	struct command *cmd;
	/* Channel being closed. */
	struct channel *channel;
	/* Should we force the close on timeout? */
	bool force;
};

static void destroy_peer(struct peer *peer)
{
	list_del_from(&peer->ld->peers, &peer->list);
}

/* We copy per-peer entries above --log-level into the main log. */
static void copy_to_parent_log(const char *prefix,
			       enum log_level level,
			       bool continued,
			       const struct timeabs *time UNUSED,
			       const char *str,
			       const u8 *io, size_t io_len,
			       struct log *parent_log)
{
	if (level == LOG_IO_IN || level == LOG_IO_OUT)
		log_io(parent_log, level, prefix, io, io_len);
	else if (continued)
		log_add(parent_log, "%s ... %s", prefix, str);
	else
		log_(parent_log, level, false, "%s %s", prefix, str);
}

static void peer_update_features(struct peer *peer,
				 const u8 *globalfeatures TAKES,
				 const u8 *localfeatures TAKES)
{
	tal_free(peer->globalfeatures);
	tal_free(peer->localfeatures);
	peer->globalfeatures = tal_dup_arr(peer, u8,
					   globalfeatures,
					   tal_count(globalfeatures), 0);
	peer->localfeatures = tal_dup_arr(peer, u8,
					  localfeatures,
					  tal_count(localfeatures), 0);
}

struct peer *new_peer(struct lightningd *ld, u64 dbid,
		      const struct node_id *id,
		      const struct wireaddr_internal *addr)
{
	/* We are owned by our channels, and freed manually by destroy_channel */
	struct peer *peer = tal(NULL, struct peer);

	peer->ld = ld;
	peer->dbid = dbid;
	peer->id = *id;
	peer->uncommitted_channel = NULL;
	peer->addr = *addr;
	peer->globalfeatures = peer->localfeatures = NULL;
	list_head_init(&peer->channels);
	peer->direction = node_id_idx(&peer->ld->id, &peer->id);

#if DEVELOPER
	peer->ignore_htlcs = false;
#endif

	/* Max 128k per peer. */
	peer->log_book = new_log_book(peer->ld, 128*1024, get_log_level(ld->log_book));
	set_log_outfn(peer->log_book, copy_to_parent_log, ld->log);
	list_add_tail(&ld->peers, &peer->list);
	tal_add_destructor(peer, destroy_peer);
	return peer;
}

static void delete_peer(struct peer *peer)
{
	assert(list_empty(&peer->channels));
	assert(!peer->uncommitted_channel);
	/* If it only ever existed because of uncommitted channel, it won't
	 * be in the database */
	if (peer->dbid != 0)
		wallet_peer_delete(peer->ld->wallet, peer->dbid);
	tal_free(peer);
}

/* Last one out deletes peer. */
void maybe_delete_peer(struct peer *peer)
{
	if (!list_empty(&peer->channels))
		return;
	if (peer->uncommitted_channel) {
		/* This isn't sufficient to keep it in db! */
		if (peer->dbid != 0) {
			wallet_peer_delete(peer->ld->wallet, peer->dbid);
			peer->dbid = 0;
		}
		return;
	}
	delete_peer(peer);
}

struct peer *find_peer_by_dbid(struct lightningd *ld, u64 dbid)
{
	struct peer *p;

	list_for_each(&ld->peers, p, list)
		if (p->dbid == dbid)
			return p;
	return NULL;
}

struct peer *peer_by_id(struct lightningd *ld, const struct node_id *id)
{
	struct peer *p;

	list_for_each(&ld->peers, p, list)
		if (node_id_eq(&p->id, id))
			return p;
	return NULL;
}

struct peer *peer_from_json(struct lightningd *ld,
			    const char *buffer,
			    const jsmntok_t *peeridtok)
{
	struct node_id peerid;

	if (!json_to_node_id(buffer, peeridtok, &peerid))
		return NULL;

	return peer_by_id(ld, &peerid);
}

u8 *p2wpkh_for_keyidx(const tal_t *ctx, struct lightningd *ld, u64 keyidx)
{
	struct pubkey shutdownkey;

	if (!bip32_pubkey(ld->wallet->bip32_base, &shutdownkey, keyidx))
		return NULL;

	return scriptpubkey_p2wpkh(ctx, &shutdownkey);
}

static void sign_last_tx(struct channel *channel)
{
	struct lightningd *ld = channel->peer->ld;
	struct bitcoin_signature sig;
	u8 *msg, **witness;

	assert(!channel->last_tx->wtx->inputs[0].witness);

	msg = towire_hsm_sign_commitment_tx(tmpctx,
					    &channel->peer->id,
					    channel->dbid,
					    channel->last_tx,
					    &channel->channel_info
					    .remote_fundingkey,
					    channel->funding);

	if (!wire_sync_write(ld->hsm_fd, take(msg)))
		fatal("Could not write to HSM: %s", strerror(errno));

	msg = wire_sync_read(tmpctx, ld->hsm_fd);
	if (!fromwire_hsm_sign_commitment_tx_reply(msg, &sig))
		fatal("HSM gave bad sign_commitment_tx_reply %s",
		      tal_hex(tmpctx, msg));

	witness =
	    bitcoin_witness_2of2(channel->last_tx, &channel->last_sig,
				 &sig, &channel->channel_info.remote_fundingkey,
				 &channel->local_funding_pubkey);

	bitcoin_tx_input_set_witness(channel->last_tx, 0, witness);
}

static void remove_sig(struct bitcoin_tx *signed_tx)
{
	bitcoin_tx_input_set_witness(signed_tx, 0, NULL);
}

/* Resolve a single close command. */
static void
resolve_one_close_command(struct close_command *cc, bool cooperative)
{
	struct json_stream *result = json_stream_success(cc->cmd);
	struct bitcoin_txid txid;

	bitcoin_txid(cc->channel->last_tx, &txid);

	json_object_start(result, NULL);
	json_add_tx(result, "tx", cc->channel->last_tx);
	json_add_txid(result, "txid", &txid);
	if (cooperative)
		json_add_string(result, "type", "mutual");
	else
		json_add_string(result, "type", "unilateral");
	json_object_end(result);

	was_pending(command_success(cc->cmd, result));
}

/* Resolve a close command for a channel that will be closed soon. */
static void
resolve_close_command(struct lightningd *ld, struct channel *channel,
		      bool cooperative)
{
	struct close_command *cc;
	struct close_command *n;

	list_for_each_safe (&ld->close_commands, cc, n, list) {
		if (cc->channel != channel)
			continue;
		resolve_one_close_command(cc, cooperative);
	}
}

/* Destroy the close command structure in reaction to the
 * channel being destroyed. */
static void
destroy_close_command_on_channel_destroy(struct channel *_ UNUSED,
					 struct close_command *cc)
{
	/* The cc has the command as parent, so resolving the
	 * command destroys the cc and triggers destroy_close_command.
	 * Clear the cc->channel first so that we will not try to
	 * remove a destructor. */
	cc->channel = NULL;
	was_pending(command_fail(cc->cmd, LIGHTNINGD,
				 "Channel forgotten before proper close."));
}

/* Destroy the close command structure. */
static void
destroy_close_command(struct close_command *cc)
{
	list_del(&cc->list);
	/* If destroy_close_command_on_channel_destroy was
	 * triggered beforehand, it will have cleared
	 * the channel field, preventing us from removing it
	 * from an already-destroyed channel. */
	if (!cc->channel)
		return;
	tal_del_destructor2(cc->channel,
			    &destroy_close_command_on_channel_destroy,
			    cc);
}

/* Handle timeout. */
static void
close_command_timeout(struct close_command *cc)
{
	if (cc->force)
		/* This will trigger drop_to_chain, which will trigger
		 * resolution of the command and destruction of the
		 * close_command. */
		channel_fail_permanent(cc->channel,
				       "Forcibly closed by 'close' command timeout");
	else
		/* Fail the command directly, which will resolve the
		 * command and destroy the close_command. */
		was_pending(command_fail(cc->cmd, LIGHTNINGD,
					 "Channel close negotiation not finished "
					 "before timeout"));
}

/* Construct a close command structure and add to ld. */
static void
register_close_command(struct lightningd *ld,
		       struct command *cmd,
		       struct channel *channel,
		       unsigned int timeout,
		       bool force)
{
	struct close_command *cc;
	assert(channel);

	cc = tal(cmd, struct close_command);
	list_add_tail(&ld->close_commands, &cc->list);
	cc->cmd = cmd;
	cc->channel = channel;
	cc->force = force;
	tal_add_destructor(cc, &destroy_close_command);
	tal_add_destructor2(channel,
			    &destroy_close_command_on_channel_destroy,
			    cc);
	new_reltimer(&ld->timers, cc, time_from_sec(timeout),
		     &close_command_timeout, cc);
}

void drop_to_chain(struct lightningd *ld, struct channel *channel,
		   bool cooperative)
{
	struct bitcoin_txid txid;
	/* BOLT #2:
	 *
	 * - if `next_remote_revocation_number` is greater than expected
	 *   above, AND `your_last_per_commitment_secret` is correct for that
	 *   `next_remote_revocation_number` minus 1:
	 *      - MUST NOT broadcast its commitment transaction.
	 */
	if (channel->future_per_commitment_point && !cooperative) {
		log_broken(channel->log,
			   "Cannot broadcast our commitment tx:"
			   " they have a future one");
	} else {
		sign_last_tx(channel);
		bitcoin_txid(channel->last_tx, &txid);
		wallet_transaction_add(ld->wallet, channel->last_tx, 0, 0);
		wallet_transaction_annotate(ld->wallet, &txid, channel->last_tx_type, channel->dbid);

		/* Keep broadcasting until we say stop (can fail due to dup,
		 * if they beat us to the broadcast). */
		broadcast_tx(ld->topology, channel, channel->last_tx, NULL);

		remove_sig(channel->last_tx);
	}

	resolve_close_command(ld, channel, cooperative);
}

void channel_errmsg(struct channel *channel,
		    struct per_peer_state *pps,
		    const struct channel_id *channel_id UNUSED,
		    const char *desc,
		    const u8 *err_for_them)
{
	/* No per_peer_state means a subd crash or disconnection. */
	if (!pps) {
		channel_fail_transient(channel, "%s: %s",
				       channel->owner->name, desc);
		return;
	}

	/* Do we have an error to send? */
	if (err_for_them && !channel->error)
		channel->error = tal_dup_arr(channel, u8,
					     err_for_them,
					     tal_count(err_for_them), 0);

	notify_disconnect(channel->peer->ld, &channel->peer->id);

	/* BOLT #1:
	 *
	 * A sending node:
	 *...
	 *   - when `channel_id` is 0:
	 *    - MUST fail all channels with the receiving node.
	 *    - MUST close the connection.
	 */
	/* FIXME: Close if it's an all-channels error sent or rcvd */

	/* BOLT #1:
	 *
	 * A sending node:
	 *  - when sending `error`:
	 *    - MUST fail the channel referred to by the error message.
	 *...
	 * The receiving node:
	 *  - upon receiving `error`:
	 *    - MUST fail the channel referred to by the error message,
	 *      if that channel is with the sending node.
	 */
	channel_fail_permanent(channel, "%s: %s ERROR %s",
			       channel->owner->name,
			       err_for_them ? "sent" : "received", desc);
}

struct peer_connected_hook_payload {
	struct lightningd *ld;
	struct channel *channel;
	struct wireaddr_internal addr;
	struct peer *peer;
	struct per_peer_state *pps;
};

static void json_add_htlcs(struct lightningd *ld,
			   struct json_stream *response,
			   const struct channel *channel)
{
	/* FIXME: make per-channel htlc maps! */
	const struct htlc_in *hin;
	struct htlc_in_map_iter ini;
	const struct htlc_out *hout;
	struct htlc_out_map_iter outi;

	/* FIXME: Add more fields. */
	json_array_start(response, "htlcs");
	for (hin = htlc_in_map_first(&ld->htlcs_in, &ini);
	     hin;
	     hin = htlc_in_map_next(&ld->htlcs_in, &ini)) {
		if (hin->key.channel != channel)
			continue;

		json_object_start(response, NULL);
		json_add_string(response, "direction", "in");
		json_add_u64(response, "id", hin->key.id);
		json_add_amount_msat_compat(response, hin->msat,
					    "msatoshi", "amount_msat");
		json_add_u64(response, "expiry", hin->cltv_expiry);
		json_add_hex(response, "payment_hash",
			     &hin->payment_hash, sizeof(hin->payment_hash));
		json_add_string(response, "state",
				htlc_state_name(hin->hstate));
		json_object_end(response);
	}

	for (hout = htlc_out_map_first(&ld->htlcs_out, &outi);
	     hout;
	     hout = htlc_out_map_next(&ld->htlcs_out, &outi)) {
		if (hout->key.channel != channel)
			continue;

		json_object_start(response, NULL);
		json_add_string(response, "direction", "out");
		json_add_u64(response, "id", hout->key.id);
		json_add_amount_msat_compat(response, hout->msat,
					    "msatoshi", "amount_msat");
		json_add_u64(response, "expiry", hout->cltv_expiry);
		json_add_hex(response, "payment_hash",
			     &hout->payment_hash, sizeof(hout->payment_hash));
		json_add_string(response, "state",
				htlc_state_name(hout->hstate));
		json_object_end(response);
	}
	json_array_end(response);
}

/* We do this replication manually because it's an array. */
static void json_add_sat_only(struct json_stream *result,
			      const char *fieldname,
			      struct amount_sat sat)
{
	struct amount_msat msat;

	if (amount_sat_to_msat(&msat, sat))
		json_add_member(result, fieldname, "\"%s\"",
				type_to_string(tmpctx, struct amount_msat, &msat));
}

static void json_add_channel(struct lightningd *ld,
			     struct json_stream *response, const char *key,
			     const struct channel *channel)
{
	struct channel_id cid;
	struct channel_stats channel_stats;
	struct amount_msat spendable, funding_msat;
	struct peer *p = channel->peer;

	json_object_start(response, key);
	json_add_string(response, "state", channel_state_name(channel));
	if (channel->last_tx) {
		struct bitcoin_txid txid;
		bitcoin_txid(channel->last_tx, &txid);

		json_add_txid(response, "scratch_txid", &txid);
	}
	if (channel->owner)
		json_add_string(response, "owner", channel->owner->name);

	if (channel->scid) {
		json_add_short_channel_id(response, "short_channel_id",
					  channel->scid);
		json_add_num(response, "direction",
			     node_id_idx(&ld->id, &channel->peer->id));
	}

	derive_channel_id(&cid, &channel->funding_txid,
			  channel->funding_outnum);
	json_add_string(response, "channel_id",
			type_to_string(tmpctx, struct channel_id, &cid));
	json_add_txid(response, "funding_txid", &channel->funding_txid);
	json_add_bool(
	    response, "private",
	    !(channel->channel_flags & CHANNEL_FLAGS_ANNOUNCE_CHANNEL));

	// FIXME @conscott : Modify this when dual-funded channels
	// are implemented
	json_object_start(response, "funding_allocation_msat");
	if (channel->funder == LOCAL) {
		json_add_u64(response, node_id_to_hexstr(tmpctx, &p->id), 0);
		json_add_u64(response, node_id_to_hexstr(tmpctx, &ld->id),
			     channel->funding.satoshis * 1000); /* Raw: raw JSON field */
	} else {
		json_add_u64(response, node_id_to_hexstr(tmpctx, &ld->id), 0);
		json_add_u64(response, node_id_to_hexstr(tmpctx, &p->id),
			     channel->funding.satoshis * 1000); /* Raw: raw JSON field */
	}
	json_object_end(response);

	json_object_start(response, "funding_msat");
	if (channel->funder == LOCAL) {
		json_add_sat_only(response,
				  node_id_to_hexstr(tmpctx, &p->id),
				  AMOUNT_SAT(0));
		json_add_sat_only(response,
				  node_id_to_hexstr(tmpctx, &ld->id),
				  channel->funding);
	} else {
		json_add_sat_only(response,
				  node_id_to_hexstr(tmpctx, &ld->id),
				  AMOUNT_SAT(0));
		json_add_sat_only(response,
				  node_id_to_hexstr(tmpctx, &p->id),
				  channel->funding);
	}
	json_object_end(response);

	if (!amount_sat_to_msat(&funding_msat, channel->funding)) {
		log_broken(channel->log,
			   "Overflow converting funding %s",
			   type_to_string(tmpctx, struct amount_sat,
					  &channel->funding));
		funding_msat = AMOUNT_MSAT(0);
	}
	json_add_amount_msat_compat(response, channel->our_msat,
				    "msatoshi_to_us", "to_us_msat");
	json_add_amount_msat_compat(response, channel->msat_to_us_min,
				    "msatoshi_to_us_min", "min_to_us_msat");
	json_add_amount_msat_compat(response, channel->msat_to_us_max,
				    "msatoshi_to_us_max", "max_to_us_msat");
	json_add_amount_msat_compat(response, funding_msat,
				    "msatoshi_total", "total_msat");

	/* channel config */
	json_add_amount_sat_compat(response,
				   channel->our_config.dust_limit,
				   "dust_limit_satoshis", "dust_limit_msat");
	json_add_amount_msat_compat(response,
				    channel->our_config.max_htlc_value_in_flight,
				    "max_htlc_value_in_flight_msat",
				    "max_total_htlc_in_msat");

	/* The `channel_reserve_satoshis` is imposed on
	 * the *other* side (see `channel_reserve_msat`
	 * function in, it uses `!side` to flip sides).
	 * So our configuration `channel_reserve_satoshis`
	 * is imposed on their side, while their
	 * configuration `channel_reserve_satoshis` is
	 * imposed on ours. */
	json_add_amount_sat_compat(response,
				   channel->our_config.channel_reserve,
				   "their_channel_reserve_satoshis",
				   "their_reserve_msat");
	json_add_amount_sat_compat(response,
				   channel->channel_info.their_config.channel_reserve,
				   "our_channel_reserve_satoshis",
				   "our_reserve_msat");
	/* Compute how much we can send via this channel. */
	if (!amount_msat_sub_sat(&spendable,
				 channel->our_msat,
				 channel->channel_info.their_config.channel_reserve))
		spendable = AMOUNT_MSAT(0);

	json_add_amount_msat_compat(response, spendable,
				    "spendable_msatoshi", "spendable_msat");
	json_add_amount_msat_compat(response,
				    channel->our_config.htlc_minimum,
				    "htlc_minimum_msat",
				    "minimum_htlc_in_msat");

	/* The `to_self_delay` is imposed on the *other*
	 * side, so our configuration `to_self_delay` is
	 * imposed on their side, while their configuration
	 * `to_self_delay` is imposed on ours. */
	json_add_num(response, "their_to_self_delay",
		     channel->our_config.to_self_delay);
	json_add_num(response, "our_to_self_delay",
		     channel->channel_info.their_config.to_self_delay);
	json_add_num(response, "max_accepted_htlcs",
		     channel->our_config.max_accepted_htlcs);

	json_array_start(response, "status");
	for (size_t i = 0; i < ARRAY_SIZE(channel->billboard.permanent); i++) {
		if (!channel->billboard.permanent[i])
			continue;
		json_add_string(response, NULL,
				channel->billboard.permanent[i]);
	}
	if (channel->billboard.transient)
		json_add_string(response, NULL, channel->billboard.transient);
	json_array_end(response);

	/* Provide channel statistics */
	wallet_channel_stats_load(ld->wallet, channel->dbid, &channel_stats);
	json_add_u64(response, "in_payments_offered",
		     channel_stats.in_payments_offered);
	json_add_amount_msat_compat(response,
				    channel_stats.in_msatoshi_offered,
				    "in_msatoshi_offered",
				    "in_offered_msat");
	json_add_u64(response, "in_payments_fulfilled",
		     channel_stats.in_payments_fulfilled);
	json_add_amount_msat_compat(response,
				    channel_stats.in_msatoshi_fulfilled,
				    "in_msatoshi_fulfilled",
				    "in_fulfilled_msat");
	json_add_u64(response, "out_payments_offered",
		     channel_stats.out_payments_offered);
	json_add_amount_msat_compat(response,
				    channel_stats.out_msatoshi_offered,
				    "out_msatoshi_offered",
				    "out_offered_msat");
	json_add_u64(response, "out_payments_fulfilled",
		     channel_stats.out_payments_fulfilled);
	json_add_amount_msat_compat(response,
				    channel_stats.out_msatoshi_fulfilled,
				    "out_msatoshi_fulfilled",
				    "out_fulfilled_msat");

	json_add_htlcs(ld, response, channel);
	json_object_end(response);
}

static void
peer_connected_serialize(struct peer_connected_hook_payload *payload,
			 struct json_stream *stream)
{
	const struct peer *p = payload->peer;
	json_object_start(stream, "peer");
	json_add_node_id(stream, "id", &p->id);
	json_add_string(
	    stream, "addr",
	    type_to_string(stream, struct wireaddr_internal, &payload->addr));
	json_add_hex_talarr(stream, "globalfeatures", p->globalfeatures);
	json_add_hex_talarr(stream, "localfeatures", p->localfeatures);
	json_object_end(stream); /* .peer */
}

static void
peer_connected_hook_cb(struct peer_connected_hook_payload *payload,
		       const char *buffer,
		       const jsmntok_t *toks)
{
	struct lightningd *ld = payload->ld;
	struct channel *channel = payload->channel;
	struct wireaddr_internal addr = payload->addr;
	struct peer *peer = payload->peer;
	u8 *error;

	/* If we had a hook, interpret result. */
	if (buffer) {
		const jsmntok_t *resulttok;

		resulttok = json_get_member(buffer, toks, "result");
		if (!resulttok) {
			fatal("Plugin returned an invalid response to the connected "
			      "hook: %s", buffer);
		}

		if (json_tok_streq(buffer, resulttok, "disconnect")) {
			const jsmntok_t *m = json_get_member(buffer, toks,
							     "error_message");
			if (m) {
				error = towire_errorfmt(tmpctx, NULL,
							"%.*s",
							m->end - m->start,
							buffer + m->start);
				goto send_error;
			}
			tal_free(payload);
			return;
		} else if (!json_tok_streq(buffer, resulttok, "continue"))
			fatal("Plugin returned an invalid response to the connected "
			      "hook: %s", buffer);
	}

	if (channel) {
		log_debug(channel->log, "Peer has reconnected, state %s",
			  channel_state_name(channel));

		/* If we have a canned error, deliver it now. */
		if (channel->error) {
			error = channel->error;
			goto send_error;
		}

#if DEVELOPER
		if (dev_disconnect_permanent(ld)) {
			channel_internal_error(channel,
					       "dev_disconnect permfail");
			error = channel->error;
			goto send_error;
		}
#endif

		switch (channel->state) {
		case ONCHAIN:
		case FUNDING_SPEND_SEEN:
		case CLOSINGD_COMPLETE:
			/* Channel is supposed to be active! */
			abort();

		/* We consider this "active" but we only send an error */
		case AWAITING_UNILATERAL: {
			struct channel_id cid;
			derive_channel_id(&cid,
					  &channel->funding_txid,
					  channel->funding_outnum);
			/* channel->error is not saved in db, so this can
			 * happen if we restart. */
			error = towire_errorfmt(tmpctx, &cid,
						"Awaiting unilateral close");
			goto send_error;
		}

		case CHANNELD_AWAITING_LOCKIN:
		case CHANNELD_NORMAL:
		case CHANNELD_SHUTTING_DOWN:
			assert(!channel->owner);

			channel->peer->addr = addr;
			peer_start_channeld(channel, payload->pps, NULL,
					    true);
			tal_free(payload);
			return;

		case CLOSINGD_SIGEXCHANGE:
			assert(!channel->owner);

			channel->peer->addr = addr;
			peer_start_closingd(channel, payload->pps,
					    true, NULL);
			tal_free(payload);
			return;
		}
		abort();
	}

	notify_connect(ld, &peer->id, &addr);

	/* No err, all good. */
	error = NULL;

send_error:
	peer_start_openingd(peer, payload->pps, error);
	tal_free(payload);
}

REGISTER_PLUGIN_HOOK(peer_connected, peer_connected_hook_cb,
		     struct peer_connected_hook_payload *,
		     peer_connected_serialize,
		     struct peer_connected_hook_payload *);

/* Connectd tells us a peer has connected: it never hands us duplicates, since
 * it holds them until we say peer_died. */
void peer_connected(struct lightningd *ld, const u8 *msg,
		    int peer_fd, int gossip_fd, int gossip_store_fd)
{
	struct node_id id;
	u8 *globalfeatures, *localfeatures;
	struct peer *peer;
	struct peer_connected_hook_payload *hook_payload;

	hook_payload = tal(NULL, struct peer_connected_hook_payload);
	hook_payload->ld = ld;
	if (!fromwire_connect_peer_connected(hook_payload, msg,
					     &id, &hook_payload->addr,
					     &hook_payload->pps,
					     &globalfeatures, &localfeatures))
		fatal("Connectd gave bad CONNECT_PEER_CONNECTED message %s",
		      tal_hex(msg, msg));

#if DEVELOPER
	/* Override broaedcast interval from our config */
	hook_payload->pps->dev_gossip_broadcast_msec
		= ld->config.broadcast_interval_msec;
#endif

	per_peer_state_set_fds(hook_payload->pps,
			       peer_fd, gossip_fd, gossip_store_fd);

	/* Complete any outstanding connect commands. */
	connect_succeeded(ld, &id);

	/* If we're already dealing with this peer, hand off to correct
	 * subdaemon.  Otherwise, we'll hand to openingd to wait there. */
	peer = peer_by_id(ld, &id);
	if (!peer)
		peer = new_peer(ld, 0, &id, &hook_payload->addr);

	tal_steal(peer, hook_payload);
	hook_payload->peer = peer;

	peer_update_features(peer, globalfeatures, localfeatures);

	/* Can't be opening, since we wouldn't have sent peer_disconnected. */
	assert(!peer->uncommitted_channel);
	hook_payload->channel = peer_active_channel(peer);

	plugin_hook_call_peer_connected(ld, hook_payload, hook_payload);
}

static enum watch_result funding_depth_cb(struct lightningd *ld,
					   struct channel *channel,
					   const struct bitcoin_txid *txid,
					   unsigned int depth)
{
	const char *txidstr;
	struct short_channel_id scid;

	txidstr = type_to_string(tmpctx, struct bitcoin_txid, txid);
	log_debug(channel->log, "Funding tx %s depth %u of %u",
		  txidstr, depth, channel->minimum_depth);
	tal_free(txidstr);

	bool min_depth_reached = depth >= channel->minimum_depth;

	/* Reorg can change scid, so always update/save scid when possible (depth=0
	 * means the stale block with our funding tx was removed) */
	if ((min_depth_reached && !channel->scid) || (depth && channel->scid)) {
		struct txlocator *loc;

		wallet_transaction_annotate(ld->wallet, txid,
					    TX_CHANNEL_FUNDING, channel->dbid);
		loc = wallet_transaction_locate(tmpctx, ld->wallet, txid);
		if (!mk_short_channel_id(&scid,
					 loc->blkheight, loc->index,
					 channel->funding_outnum)) {
			channel_fail_permanent(channel, "Invalid funding scid %u:%u:%u",
					       loc->blkheight, loc->index,
					       channel->funding_outnum);
			return DELETE_WATCH;
		}

		/* If we restart, we could already have peer->scid from database */
		if (!channel->scid) {
			channel->scid = tal(channel, struct short_channel_id);
			*channel->scid = scid;
			wallet_channel_save(ld->wallet, channel);

		} else if (!short_channel_id_eq(channel->scid, &scid)) {
			/* This normally restarts channeld, initialized with updated scid
			 * and also adds it (at least our halve_chan) to rtable. */
			channel_fail_transient(channel,
					        "short_channel_id changed to %s (was %s)",
					        short_channel_id_to_str(tmpctx, &scid),
					        short_channel_id_to_str(tmpctx, channel->scid));

			*channel->scid = scid;
			wallet_channel_save(ld->wallet, channel);
			return KEEP_WATCHING;
		}
	}

	/* Try to tell subdaemon */
	if (!channel_tell_depth(ld, channel, txid, depth))
		return KEEP_WATCHING;

	if (!min_depth_reached)
		return KEEP_WATCHING;

	/* We keep telling it depth/scid until we get to announce depth. */
	if (depth < ANNOUNCE_MIN_DEPTH)
		return KEEP_WATCHING;

	return DELETE_WATCH;
}

static enum watch_result funding_spent(struct channel *channel,
				       const struct bitcoin_tx *tx,
				       size_t inputnum UNUSED,
				       const struct block *block)
{
	struct bitcoin_txid txid;
	bitcoin_txid(tx, &txid);

	wallet_channeltxs_add(channel->peer->ld->wallet, channel,
			      WIRE_ONCHAIN_INIT, &txid, 0, block->height);
	return onchaind_funding_spent(channel, tx, block->height);
}

void channel_watch_funding(struct lightningd *ld, struct channel *channel)
{
	/* FIXME: Remove arg from cb? */
	watch_txid(channel, ld->topology, channel,
		   &channel->funding_txid, funding_depth_cb);
	watch_txo(channel, ld->topology, channel,
		  &channel->funding_txid, channel->funding_outnum,
		  funding_spent);
}

static void json_add_peer(struct lightningd *ld,
			  struct json_stream *response,
			  struct peer *p,
			  const enum log_level *ll)
{
	bool connected;
	struct channel *channel;

	json_object_start(response, NULL);
	json_add_node_id(response, "id", &p->id);

	/* Channel is also connected if uncommitted channel */
	if (p->uncommitted_channel)
		connected = true;
	else {
		channel = peer_active_channel(p);
		connected = channel && channel->connected;
	}
	json_add_bool(response, "connected", connected);

	/* If it's not connected, features are unreliable: we don't
	 * store them in the database, and they would only reflect
	 * their features *last* time they connected. */
	if (connected) {
		json_array_start(response, "netaddr");
		json_add_string(response, NULL,
				type_to_string(response,
					       struct wireaddr_internal,
					       &p->addr));
		json_array_end(response);
		json_add_hex_talarr(response, "globalfeatures",
				    p->globalfeatures);
		json_add_hex_talarr(response, "localfeatures",
				    p->localfeatures);
	}

	json_array_start(response, "channels");
	json_add_uncommitted_channel(response, p->uncommitted_channel);

	list_for_each(&p->channels, channel, list)
		json_add_channel(ld, response, NULL, channel);
	json_array_end(response);

	if (ll)
		json_add_log(response, p->log_book, *ll);
	json_object_end(response);
}

static struct command_result *json_listpeers(struct command *cmd,
					     const char *buffer,
					     const jsmntok_t *obj UNNEEDED,
					     const jsmntok_t *params)
{
	enum log_level *ll;
	struct node_id *specific_id;
	struct peer *peer;
	struct json_stream *response;

	if (!param(cmd, buffer, params,
		   p_opt("id", param_node_id, &specific_id),
		   p_opt("level", param_loglevel, &ll),
		   NULL))
		return command_param_failed();

	response = json_stream_success(cmd);
	json_object_start(response, NULL);
	json_array_start(response, "peers");
	if (specific_id) {
		peer = peer_by_id(cmd->ld, specific_id);
		if (peer)
			json_add_peer(cmd->ld, response, peer, ll);
	} else {
		list_for_each(&cmd->ld->peers, peer, list)
			json_add_peer(cmd->ld, response, peer, ll);
	}
	json_array_end(response);
	json_object_end(response);
	return command_success(cmd, response);
}

static const struct json_command listpeers_command = {
	"listpeers",
	"network",
	json_listpeers,
	"Show current peers, if {level} is set, include logs for {id}"
};
AUTODATA(json_command, &listpeers_command);

static struct command_result *
command_find_channel(struct command *cmd,
		     const char *buffer, const jsmntok_t *tok,
		     struct channel **channel)
{
	struct lightningd *ld = cmd->ld;
	struct channel_id cid;
	struct channel_id channel_cid;
	struct short_channel_id scid;
	struct peer *peer;

	if (json_tok_channel_id(buffer, tok, &cid)) {
		list_for_each(&ld->peers, peer, list) {
			*channel = peer_active_channel(peer);
			if (!*channel)
				continue;
			derive_channel_id(&channel_cid,
					  &(*channel)->funding_txid,
					  (*channel)->funding_outnum);
			if (channel_id_eq(&channel_cid, &cid))
				return NULL;
		}
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Channel ID not found: '%.*s'",
				    tok->end - tok->start,
				    buffer + tok->start);
	} else if (json_to_short_channel_id(buffer, tok, &scid,
					    deprecated_apis)) {
		list_for_each(&ld->peers, peer, list) {
			*channel = peer_active_channel(peer);
			if (!*channel)
				continue;
			if ((*channel)->scid
			    && (*channel)->scid->u64 == scid.u64)
				return NULL;
		}
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Short channel ID not found: '%.*s'",
				    tok->end - tok->start,
				    buffer + tok->start);
	} else {
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Given id is not a channel ID or "
				    "short channel ID: '%.*s'",
				    json_tok_full_len(tok),
				    json_tok_full(buffer, tok));
	}
}

static struct command_result *json_close(struct command *cmd,
					 const char *buffer,
					 const jsmntok_t *obj UNNEEDED,
					 const jsmntok_t *params)
{
	const jsmntok_t *idtok;
	struct peer *peer;
	/* FIXME: gcc 7.3.0 thinks this might not be initialized. */
	struct channel *channel = NULL;
	unsigned int *timeout;
	bool *force;

	if (!param(cmd, buffer, params,
		   p_req("id", param_tok, &idtok),
		   p_opt_def("force", param_bool, &force, false),
		   p_opt_def("timeout", param_number, &timeout, 30),
		   NULL))
		return command_param_failed();

	peer = peer_from_json(cmd->ld, buffer, idtok);
	if (peer)
		channel = peer_active_channel(peer);
	else {
		struct command_result *res;
		res = command_find_channel(cmd, buffer, idtok, &channel);
		if (res)
			return res;
	}

	if (!channel && peer) {
		struct uncommitted_channel *uc = peer->uncommitted_channel;
		if (uc) {
			/* Easy case: peer can simply be forgotten. */
			kill_uncommitted_channel(uc, "close command called");

			return command_success(cmd, null_response(cmd));
		}
		return command_fail(cmd, LIGHTNINGD,
				    "Peer has no active channel");
	}

	/* Normal case.
	 * We allow states shutting down and sigexchange; a previous
	 * close command may have timed out, and this current command
	 * will continue waiting for the effects of the previous
	 * close command. */
	if (channel->state != CHANNELD_NORMAL &&
	    channel->state != CHANNELD_AWAITING_LOCKIN &&
	    channel->state != CHANNELD_SHUTTING_DOWN &&
	    channel->state != CLOSINGD_SIGEXCHANGE) {
		return command_fail(cmd, LIGHTNINGD, "Channel is in state %s",
				    channel_state_name(channel));
	}

	/* If normal or locking in, transition to shutting down
	 * state.
	 * (if already shutting down or sigexchange, just keep
	 * waiting) */
	if (channel->state == CHANNELD_NORMAL || channel->state == CHANNELD_AWAITING_LOCKIN) {
		channel_set_state(channel,
				  channel->state, CHANNELD_SHUTTING_DOWN);

		if (channel->owner)
			subd_send_msg(channel->owner,
				      take(towire_channel_send_shutdown(channel)));
	}

	/* Register this command for later handling. */
	register_close_command(cmd->ld, cmd, channel, *timeout, *force);

	/* Wait until close drops down to chain. */
	return command_still_pending(cmd);
}

static const struct json_command close_command = {
	"close",
	"channels",
	json_close,
	"Close the channel with {id} "
	"(either peer ID, channel ID, or short channel ID). "
	"If {force} (default false) is true, force a unilateral close "
	"after {timeout} seconds (default 30), "
	"otherwise just schedule a mutual close later and fail after "
	"timing out."
};
AUTODATA(json_command, &close_command);

static void activate_peer(struct peer *peer)
{
	u8 *msg;
	struct channel *channel;
	struct lightningd *ld = peer->ld;

	/* We can only have one active channel: make sure connectd
	 * knows to try reconnecting. */
	channel = peer_active_channel(peer);
	if (channel && ld->reconnect) {
		msg = towire_connectctl_connect_to_peer(NULL, &peer->id, 0,
							&peer->addr);
		subd_send_msg(ld->connectd, take(msg));
		channel_set_billboard(channel, false, "Attempting to reconnect");
	}

	list_for_each(&peer->channels, channel, list) {
		/* Watching lockin may be unnecessary, but it's harmless. */
		channel_watch_funding(ld, channel);
	}
}

void activate_peers(struct lightningd *ld)
{
	struct peer *p;

	list_for_each(&ld->peers, p, list)
		activate_peer(p);
}

/* Pull peers, channels and HTLCs from db, and wire them up. */
void load_channels_from_wallet(struct lightningd *ld)
{
	struct peer *peer;

	/* Load peers from database */
	if (!wallet_channels_load_active(ld->wallet))
		fatal("Could not load channels from the database");

	/* This is a poor-man's db join :( */
	list_for_each(&ld->peers, peer, list) {
		struct channel *channel;

		list_for_each(&peer->channels, channel, list) {
			if (!wallet_htlcs_load_for_channel(ld->wallet,
							   channel,
							   &ld->htlcs_in,
							   &ld->htlcs_out)) {
				fatal("could not load htlcs for channel");
			}
		}
	}

	/* Now connect HTLC pointers together */
	htlcs_reconnect(ld, &ld->htlcs_in, &ld->htlcs_out);
}

static struct command_result *json_disconnect(struct command *cmd,
					      const char *buffer,
					      const jsmntok_t *obj UNNEEDED,
					      const jsmntok_t *params)
{
	struct node_id *id;
	struct peer *peer;
	struct channel *channel;
	bool *force;

	if (!param(cmd, buffer, params,
		   p_req("id", param_node_id, &id),
		   p_opt_def("force", param_bool, &force, false),
		   NULL))
		return command_param_failed();

	peer = peer_by_id(cmd->ld, id);
	if (!peer) {
		return command_fail(cmd, LIGHTNINGD, "Peer not connected");
	}
	channel = peer_active_channel(peer);
	if (channel) {
		if (*force) {
			channel_fail_transient(channel,
					       "disconnect command force=true");
			return command_success(cmd, null_response(cmd));
		}
		return command_fail(cmd, LIGHTNINGD, "Peer is in state %s",
				    channel_state_name(channel));
	}
	if (!peer->uncommitted_channel) {
		return command_fail(cmd, LIGHTNINGD, "Peer not connected");
	}
	kill_uncommitted_channel(peer->uncommitted_channel,
				 "disconnect command");
	return command_success(cmd, null_response(cmd));
}

static const struct json_command disconnect_command = {
	"disconnect",
	"network",
	json_disconnect,
	"Disconnect from {id} that has previously been connected to using connect; with {force} set, even if it has a current channel"
};
AUTODATA(json_command, &disconnect_command);

static struct command_result *json_getinfo(struct command *cmd,
					   const char *buffer,
					   const jsmntok_t *obj UNNEEDED,
					   const jsmntok_t *params)
{
    struct json_stream *response;
    struct peer *peer;
    struct channel *channel;
    unsigned int pending_channels = 0, active_channels = 0,
            inactive_channels = 0, num_peers = 0;

    if (!param(cmd, buffer, params, NULL))
        return command_param_failed();

    response = json_stream_success(cmd);
    json_object_start(response, NULL);
    json_add_node_id(response, "id", &cmd->ld->id);
    json_add_string(response, "alias", (const char *)cmd->ld->alias);
    json_add_hex_talarr(response, "color", cmd->ld->rgb);

    /* Add some peer and channel stats */
    list_for_each(&cmd->ld->peers, peer, list) {
        num_peers++;

        list_for_each(&peer->channels, channel, list) {
            if (channel->state == CHANNELD_AWAITING_LOCKIN) {
                pending_channels++;
            } else if (channel_active(channel)) {
                active_channels++;
            } else {
                inactive_channels++;
            }
        }
    }
    json_add_num(response, "num_peers", num_peers);
    json_add_num(response, "num_pending_channels", pending_channels);
    json_add_num(response, "num_active_channels", active_channels);
    json_add_num(response, "num_inactive_channels", inactive_channels);

    /* Add network info */
    if (cmd->ld->listen) {
        /* These are the addresses we're announcing */
        json_array_start(response, "address");
        for (size_t i = 0; i < tal_count(cmd->ld->announcable); i++)
            json_add_address(response, NULL, cmd->ld->announcable+i);
        json_array_end(response);

        /* This is what we're actually bound to. */
        json_array_start(response, "binding");
        for (size_t i = 0; i < tal_count(cmd->ld->binding); i++)
            json_add_address_internal(response, NULL,
                          cmd->ld->binding+i);
        json_array_end(response);
    }
    json_add_string(response, "version", version());
    json_add_num(response, "blockheight", get_block_height(cmd->ld->topology));
    json_add_string(response, "network", get_chainparams(cmd->ld)->network_name);
    json_add_amount_msat_compat(response,
				wallet_total_forward_fees(cmd->ld->wallet),
				"msatoshi_fees_collected",
				"fees_collected_msat");
    json_object_end(response);
    return command_success(cmd, response);
}

static const struct json_command getinfo_command = {
    "getinfo",
	"utility",
    json_getinfo,
    "Show information about this node"
};
AUTODATA(json_command, &getinfo_command);

static struct command_result *param_channel_or_all(struct command *cmd,
					     const char *name,
					     const char *buffer,
					     const jsmntok_t *tok,
					     struct channel **channel)
{
	struct command_result *res;
	struct peer *peer;

	/* early return the easy case */
	if (json_tok_streq(buffer, tok, "all")) {
		*channel = NULL;
		return NULL;
	}

	/* Find channel by peer_id */
	peer = peer_from_json(cmd->ld, buffer, tok);
	if (peer) {
		*channel = peer_active_channel(peer);
		if (!*channel)
			return command_fail(cmd, LIGHTNINGD,
					"Could not find active channel of peer with that id");
		return NULL;

	/* Find channel by id or scid */
	} else {
		res = command_find_channel(cmd, buffer, tok, channel);
		if (res)
			return res;
		/* check channel is found and in valid state */
		if (!*channel)
			return command_fail(cmd, LIGHTNINGD,
					"Could not find channel with that id");
		return NULL;
	}
}

/* Fee base is a u32, but it's convenient to let them specify it using
 * msat etc. suffix. */
static struct command_result *param_msat_u32(struct command *cmd,
					     const char *name,
					     const char *buffer,
					     const jsmntok_t *tok,
					     u32 **num)
{
	struct amount_msat *msat;
	struct command_result *res;

	/* Parse just like an msat. */
	res = param_msat(cmd, name, buffer, tok, &msat);
	if (res)
		return res;

	*num = tal(cmd, u32);
	if (!amount_msat_to_u32(*msat, *num)) {
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "'%s' value '%s' exceeds u32 max",
				    name,
				    type_to_string(tmpctx, struct amount_msat,
						   msat));
	}

	return NULL;
}

static void set_channel_fees(struct command *cmd, struct channel *channel,
		u32 base, u32 ppm, struct json_stream *response)
{
	struct channel_id cid;

	/* set new values */
	channel->feerate_base = base;
	channel->feerate_ppm = ppm;

	/* tell channeld to make a send_channel_update */
	if (channel->owner && streq(channel->owner->name, "lightning_channeld"))
		subd_send_msg(channel->owner,
				take(towire_channel_specific_feerates(NULL, base, ppm)));

	/* save values to database */
	wallet_channel_save(cmd->ld->wallet, channel);

	/* write JSON response entry */
	derive_channel_id(&cid, &channel->funding_txid, channel->funding_outnum);
	json_object_start(response, NULL);
	json_add_node_id(response, "peer_id", &channel->peer->id);
	json_add_string(response, "channel_id",
			type_to_string(tmpctx, struct channel_id, &cid));
	if (channel->scid)
		json_add_short_channel_id(response, "short_channel_id", channel->scid);
	json_object_end(response);
}

static struct command_result *json_setchannelfee(struct command *cmd,
					 const char *buffer,
					 const jsmntok_t *obj UNNEEDED,
					 const jsmntok_t *params)
{
	struct json_stream *response;
	struct peer *peer;
	struct channel *channel;
	u32 *base, *ppm;

	/* Parse the JSON command */
	if (!param(cmd, buffer, params,
		   p_req("id", param_channel_or_all, &channel),
		   p_opt_def("base", param_msat_u32,
			     &base, cmd->ld->config.fee_base),
		   p_opt_def("ppm", param_number, &ppm,
			     cmd->ld->config.fee_per_satoshi),
		   NULL))
		return command_param_failed();

	/* Open JSON response object for later iteration */
	response = json_stream_success(cmd);
	json_object_start(response, NULL);
	json_add_num(response, "base", *base);
	json_add_num(response, "ppm", *ppm);
	json_array_start(response, "channels");

	/* If the users requested 'all' channels we need to iterate */
	if (channel == NULL) {
		list_for_each(&cmd->ld->peers, peer, list) {
			list_for_each(&peer->channels, channel, list) {
				channel = peer_active_channel(peer);
				if (!channel)
					continue;
				if (channel->state != CHANNELD_NORMAL &&
					channel->state != CHANNELD_AWAITING_LOCKIN)
					continue;
				set_channel_fees(cmd, channel, *base, *ppm, response);
			}
		}

	/* single channel should be updated */
	} else {
		if (channel->state != CHANNELD_NORMAL &&
			channel->state != CHANNELD_AWAITING_LOCKIN)
			return command_fail(cmd, LIGHTNINGD,
					"Channel is in state %s", channel_state_name(channel));
		set_channel_fees(cmd, channel, *base, *ppm, response);
	}

	/* Close and return response */
	json_array_end(response);
	json_object_end(response);
	return command_success(cmd, response);
}

static const struct json_command setchannelfee_command = {
	"setchannelfee",
	"channels",
	json_setchannelfee,
	"Sets specific routing fees for channel with {id} "
	"(either peer ID, channel ID, short channel ID or 'all'). "
	"Routing fees are defined by a fixed {base} (msat) "
	"and a {ppm} (proportional per millionth) value. "
	"If values for {base} or {ppm} are left out, defaults will be used. "
	"{base} can also be defined in other units, for example '1sat'. "
	"If {id} is 'all', the fees will be applied for all channels. "
};
AUTODATA(json_command, &setchannelfee_command);

#if DEVELOPER
static struct command_result *json_sign_last_tx(struct command *cmd,
						const char *buffer,
						const jsmntok_t *obj UNNEEDED,
						const jsmntok_t *params)
{
	struct node_id *peerid;
	struct peer *peer;
	struct json_stream *response;
	struct channel *channel;

	if (!param(cmd, buffer, params,
		   p_req("id", param_node_id, &peerid),
		   NULL))
		return command_param_failed();

	peer = peer_by_id(cmd->ld, peerid);
	if (!peer) {
		return command_fail(cmd, LIGHTNINGD,
				    "Could not find peer with that id");
	}
	channel = peer_active_channel(peer);
	if (!channel) {
		return command_fail(cmd, LIGHTNINGD,
				    "Could not find active channel");
	}

	response = json_stream_success(cmd);
	log_debug(channel->log, "dev-sign-last-tx: signing tx with %zu outputs",
		  channel->last_tx->wtx->num_outputs);

	sign_last_tx(channel);
	json_object_start(response, NULL);
	json_add_tx(response, "tx", channel->last_tx);
	json_object_end(response);
	remove_sig(channel->last_tx);

	return command_success(cmd, response);
}

static const struct json_command dev_sign_last_tx = {
	"dev-sign-last-tx",
	"developer",
	json_sign_last_tx,
	"Sign and show the last commitment transaction with peer {id}"
};
AUTODATA(json_command, &dev_sign_last_tx);

static struct command_result *json_dev_fail(struct command *cmd,
					    const char *buffer,
					    const jsmntok_t *obj UNNEEDED,
					    const jsmntok_t *params)
{
	struct node_id *peerid;
	struct peer *peer;
	struct channel *channel;

	if (!param(cmd, buffer, params,
		   p_req("id", param_node_id, &peerid),
		   NULL))
		return command_param_failed();

	peer = peer_by_id(cmd->ld, peerid);
	if (!peer) {
		return command_fail(cmd, LIGHTNINGD,
				    "Could not find peer with that id");
	}

	channel = peer_active_channel(peer);
	if (!channel) {
		return command_fail(cmd, LIGHTNINGD,
				    "Could not find active channel with peer");
	}

	channel_internal_error(channel, "Failing due to dev-fail command");
	return command_success(cmd, null_response(cmd));
}

static const struct json_command dev_fail_command = {
	"dev-fail",
	"developer",
	json_dev_fail,
	"Fail with peer {id}"
};
AUTODATA(json_command, &dev_fail_command);

static void dev_reenable_commit_finished(struct subd *channeld UNUSED,
					 const u8 *resp UNUSED,
					 const int *fds UNUSED,
					 struct command *cmd)
{
	was_pending(command_success(cmd, null_response(cmd)));
}

static struct command_result *json_dev_reenable_commit(struct command *cmd,
						       const char *buffer,
						       const jsmntok_t *obj UNNEEDED,
						       const jsmntok_t *params)
{
	struct node_id *peerid;
	struct peer *peer;
	u8 *msg;
	struct channel *channel;

	if (!param(cmd, buffer, params,
		   p_req("id", param_node_id, &peerid),
		   NULL))
		return command_param_failed();

	peer = peer_by_id(cmd->ld, peerid);
	if (!peer) {
		return command_fail(cmd, LIGHTNINGD,
				    "Could not find peer with that id");
	}

	channel = peer_active_channel(peer);
	if (!channel) {
		return command_fail(cmd, LIGHTNINGD,
				    "Peer has no active channel");
	}
	if (!channel->owner) {
		return command_fail(cmd, LIGHTNINGD,
				    "Peer has no owner");
	}

	if (!streq(channel->owner->name, "lightning_channeld")) {
		return command_fail(cmd, LIGHTNINGD,
				    "Peer owned by %s", channel->owner->name);
	}

	msg = towire_channel_dev_reenable_commit(channel);
	subd_req(peer, channel->owner, take(msg), -1, 0,
		 dev_reenable_commit_finished, cmd);
	return command_still_pending(cmd);
}

static const struct json_command dev_reenable_commit = {
	"dev-reenable-commit",
	"developer",
	json_dev_reenable_commit,
	"Re-enable the commit timer on peer {id}"
};
AUTODATA(json_command, &dev_reenable_commit);

struct dev_forget_channel_cmd {
	struct short_channel_id scid;
	struct channel *channel;
	bool force;
	struct command *cmd;
};

static void process_dev_forget_channel(struct bitcoind *bitcoind UNUSED,
				       const struct bitcoin_tx_output *txout,
				       void *arg)
{
	struct json_stream *response;
	struct dev_forget_channel_cmd *forget = arg;
	if (txout != NULL && !forget->force) {
		was_pending(command_fail(forget->cmd, LIGHTNINGD,
			     "Cowardly refusing to forget channel with an "
			     "unspent funding output, if you know what "
			     "you're doing you can override with "
			     "`force=true`, otherwise consider `close` or "
			     "`dev-fail`! If you force and the channel "
			     "confirms we will not track the funds in the "
			     "channel"));
		return;
	}
	response = json_stream_success(forget->cmd);
	json_object_start(response, NULL);
	json_add_bool(response, "forced", forget->force);
	json_add_bool(response, "funding_unspent", txout != NULL);
	json_add_txid(response, "funding_txid", &forget->channel->funding_txid);
	json_object_end(response);

	/* Set error so we don't try to reconnect. */
	forget->channel->error = towire_errorfmt(forget->channel, NULL,
						 "dev_forget_channel");
	delete_channel(forget->channel);

	was_pending(command_success(forget->cmd, response));
}

static struct command_result *json_dev_forget_channel(struct command *cmd,
						      const char *buffer,
						      const jsmntok_t *obj UNNEEDED,
						      const jsmntok_t *params)
{
	struct node_id *peerid;
	struct peer *peer;
	struct channel *channel;
	struct short_channel_id *scid;
	struct dev_forget_channel_cmd *forget = tal(cmd, struct dev_forget_channel_cmd);
	forget->cmd = cmd;

	bool *force;
	if (!param(cmd, buffer, params,
		   p_req("id", param_node_id, &peerid),
		   p_opt("short_channel_id", param_short_channel_id, &scid),
		   p_opt_def("force", param_bool, &force, false),
		   NULL))
		return command_param_failed();

	forget->force = *force;
	peer = peer_by_id(cmd->ld, peerid);
	if (!peer) {
		return command_fail(cmd, LIGHTNINGD,
				    "Could not find channel with that peer");
	}

	forget->channel = NULL;
	list_for_each(&peer->channels, channel, list) {
		if (scid) {
			if (!channel->scid)
				continue;
			if (!short_channel_id_eq(channel->scid, scid))
				continue;
		}
		if (forget->channel) {
			return command_fail(cmd, LIGHTNINGD,
					    "Multiple channels:"
					    " please specify short_channel_id");
		}
		forget->channel = channel;
	}
	if (!forget->channel) {
		return command_fail(cmd, LIGHTNINGD,
				    "No channels matching that peer_id%s",
					scid ? " and that short_channel_id" : "");
	}

	if (channel_has_htlc_out(forget->channel) ||
	    channel_has_htlc_in(forget->channel)) {
		return command_fail(cmd, LIGHTNINGD,
				    "This channel has HTLCs attached and it is "
				    "not safe to forget it. Please use `close` "
				    "or `dev-fail` instead.");
	}

	bitcoind_gettxout(cmd->ld->topology->bitcoind,
			  &forget->channel->funding_txid,
			  forget->channel->funding_outnum,
			  process_dev_forget_channel, forget);
	return command_still_pending(cmd);
}

static const struct json_command dev_forget_channel_command = {
	"dev-forget-channel",
	"developer",
	json_dev_forget_channel,
	"Forget the channel with peer {id}, ignore UTXO check with {force}='true'.", false,
	"Forget the channel with peer {id}. Checks if the channel is still active by checking its funding transaction. Check can be ignored by setting {force} to 'true'"
};
AUTODATA(json_command, &dev_forget_channel_command);

static void subd_died_forget_memleak(struct subd *openingd, struct command *cmd)
{
	/* FIXME: We ignore the remaining per-peer daemons in this case. */
	peer_memleak_done(cmd, NULL);
}

/* Mutual recursion */
static void peer_memleak_req_next(struct command *cmd, struct channel *prev);
static void peer_memleak_req_done(struct subd *subd, bool found_leak,
				  struct command *cmd)
{
	struct channel *c = subd->channel;

	if (found_leak)
		peer_memleak_done(cmd, subd);
	else
		peer_memleak_req_next(cmd, c);
}

static void channeld_memleak_req_done(struct subd *channeld,
				      const u8 *msg, const int *fds UNUSED,
				      struct command *cmd)
{
	bool found_leak;

	tal_del_destructor2(channeld, subd_died_forget_memleak, cmd);
	if (!fromwire_channel_dev_memleak_reply(msg, &found_leak)) {
		was_pending(command_fail(cmd, LIGHTNINGD,
					 "Bad channel_dev_memleak"));
		return;
	}
	peer_memleak_req_done(channeld, found_leak, cmd);
}

static void onchaind_memleak_req_done(struct subd *onchaind,
				      const u8 *msg, const int *fds UNUSED,
				      struct command *cmd)
{
	bool found_leak;

	tal_del_destructor2(onchaind, subd_died_forget_memleak, cmd);
	if (!fromwire_onchain_dev_memleak_reply(msg, &found_leak)) {
		was_pending(command_fail(cmd, LIGHTNINGD,
					 "Bad onchain_dev_memleak"));
		return;
	}
	peer_memleak_req_done(onchaind, found_leak, cmd);
}

static void peer_memleak_req_next(struct command *cmd, struct channel *prev)
{
	struct peer *p;

	list_for_each(&cmd->ld->peers, p, list) {
		struct channel *c;

		list_for_each(&p->channels, c, list) {
			if (c == prev) {
				prev = NULL;
				continue;
			}

			if (!c->owner)
				continue;

			if (prev != NULL)
				continue;

			/* Note: closingd does its own checking automatically */
			if (streq(c->owner->name, "lightning_channeld")) {
				subd_req(c, c->owner,
					 take(towire_channel_dev_memleak(NULL)),
					 -1, 0, channeld_memleak_req_done, cmd);
				tal_add_destructor2(c->owner,
						    subd_died_forget_memleak,
						    cmd);
				return;
			}
			if (streq(c->owner->name, "lightning_onchaind")) {
				subd_req(c, c->owner,
					 take(towire_onchain_dev_memleak(NULL)),
					 -1, 0, onchaind_memleak_req_done, cmd);
				tal_add_destructor2(c->owner,
						    subd_died_forget_memleak,
						    cmd);
				return;
			}
		}
	}
	peer_memleak_done(cmd, NULL);
}

void peer_dev_memleak(struct command *cmd)
{
	peer_memleak_req_next(cmd, NULL);
}
#endif /* DEVELOPER */


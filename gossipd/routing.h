#ifndef LIGHTNING_GOSSIPD_ROUTING_H
#define LIGHTNING_GOSSIPD_ROUTING_H
#include "config.h"
#include <bitcoin/pubkey.h>
#include <ccan/crypto/siphash24/siphash24.h>
#include <ccan/htable/htable_type.h>
#include <ccan/intmap/intmap.h>
#include <ccan/time/time.h>
#include <common/amount.h>
#include <common/node_id.h>
#include <gossipd/broadcast.h>
#include <gossipd/gossip_constants.h>
#include <gossipd/gossip_store.h>
#include <wire/gen_onion_wire.h>
#include <wire/wire.h>

struct routing_state;

struct half_chan {
	/* millisatoshi. */
	u32 base_fee;
	/* millionths */
	u32 proportional_fee;

	/* Delay for HTLC in blocks.*/
	u32 delay;

	/* Timestamp and index into store file */
	struct broadcastable bcast;

	/* Flags as specified by the `channel_update`s, among other
	 * things indicated direction wrt the `channel_id` */
	u8 channel_flags;

	/* Flags as specified by the `channel_update`s, indicates
	 * optional fields.  */
	u8 message_flags;

	/* Minimum and maximum number of msatoshi in an HTLC */
	struct amount_msat htlc_minimum, htlc_maximum;
};

struct chan {
	struct short_channel_id scid;

	/*
	 * half[0]->src == nodes[0] half[0]->dst == nodes[1]
	 * half[1]->src == nodes[1] half[1]->dst == nodes[0]
	 */
	struct half_chan half[2];
	/* node[0].id < node[1].id */
	struct node *nodes[2];

	/* Timestamp and index into store file */
	struct broadcastable bcast;

	struct amount_sat sat;
};

/* Use this instead of tal_free(chan)! */
void free_chan(struct routing_state *rstate, struct chan *chan);

/* A local channel can exist which isn't announced: we abuse timestamp
 * to indicate this. */
static inline bool is_chan_public(const struct chan *chan)
{
	return chan->bcast.timestamp != 0;
}

static inline bool is_halfchan_defined(const struct half_chan *hc)
{
	return hc->bcast.index != 0;
}

static inline bool is_halfchan_enabled(const struct half_chan *hc)
{
	return is_halfchan_defined(hc) && !(hc->channel_flags & ROUTING_FLAGS_DISABLED);
}

/* Container for per-node channel pointers.  Better cache performance
* than uintmap, and we don't need ordering. */
static inline const struct short_channel_id *chan_map_scid(const struct chan *c)
{
	return &c->scid;
}

static inline size_t hash_scid(const struct short_channel_id *scid)
{
	/* scids cost money to generate, so simple hash works here */
	return (scid->u64 >> 32) ^ (scid->u64 >> 16) ^ scid->u64;
}

static inline bool chan_eq_scid(const struct chan *c,
				const struct short_channel_id *scid)
{
	return short_channel_id_eq(scid, &c->scid);
}

HTABLE_DEFINE_TYPE(struct chan, chan_map_scid, hash_scid, chan_eq_scid, chan_map);

/* For a small number of channels (by far the most common) we use a simple
 * array, with empty buckets NULL.  For larger, we use a proper hash table,
 * with the extra allocation that implies. */
#define NUM_IMMEDIATE_CHANS (sizeof(struct chan_map) / sizeof(struct chan *) - 1)

struct node {
	struct node_id id;

	/* Timestamp and index into store file */
	struct broadcastable bcast;

	/* Channels connecting us to other nodes */
	union {
		struct chan_map map;
		struct chan *arr[NUM_IMMEDIATE_CHANS+1];
	} chans;

	/* Temporary data for routefinding. */
	struct {
		/* Total to get to here from target. */
		struct amount_msat total;
		/* Total risk premium of this route. */
		struct amount_msat risk;
	} dijkstra;
};

const struct node_id *node_map_keyof_node(const struct node *n);
size_t node_map_hash_key(const struct node_id *pc);
bool node_map_node_eq(const struct node *n, const struct node_id *pc);
HTABLE_DEFINE_TYPE(struct node, node_map_keyof_node, node_map_hash_key, node_map_node_eq, node_map);

/* We've unpacked and checked its signatures, now we wait for master to tell
 * us the txout to check */
struct pending_cannouncement {
	/* Unpacked fields here */

	/* also the key in routing_state->pending_cannouncements */
	struct short_channel_id short_channel_id;
	struct node_id node_id_1;
	struct node_id node_id_2;
	struct pubkey bitcoin_key_1;
	struct pubkey bitcoin_key_2;

	/* The raw bits */
	const u8 *announce;

	/* Deferred updates, if we received them while waiting for
	 * this (one for each direction) */
	const u8 *updates[2];

	/* Only ever replace with newer updates */
	u32 update_timestamps[2];
};

static inline const struct short_channel_id *panding_cannouncement_map_scid(
				const struct pending_cannouncement *pending_ann)
{
	return &pending_ann->short_channel_id;
}

static inline size_t hash_pending_cannouncement_scid(
				const struct short_channel_id *scid)
{
	/* like hash_scid() for struct chan above */
	return (scid->u64 >> 32) ^ (scid->u64 >> 16) ^ scid->u64;
}

static inline bool pending_cannouncement_eq_scid(
				const struct pending_cannouncement *pending_ann,
				const struct short_channel_id *scid)
{
	return short_channel_id_eq(scid, &pending_ann->short_channel_id);
}

HTABLE_DEFINE_TYPE(struct pending_cannouncement, panding_cannouncement_map_scid,
		   hash_pending_cannouncement_scid, pending_cannouncement_eq_scid,
		   pending_cannouncement_map);

struct pending_node_map;
struct unupdated_channel;

/* Fast versions: if you know n is one end of the channel */
static inline struct node *other_node(const struct node *n,
				      const struct chan *chan)
{
	int idx = (chan->nodes[1] == n);

	assert(chan->nodes[0] == n || chan->nodes[1] == n);
	return chan->nodes[!idx];
}

/* If you know n is one end of the channel, get connection src == n */
static inline struct half_chan *half_chan_from(const struct node *n,
					       struct chan *chan)
{
	int idx = (chan->nodes[1] == n);

	assert(chan->nodes[0] == n || chan->nodes[1] == n);
	return &chan->half[idx];
}

/* If you know n is one end of the channel, get index dst == n */
static inline int half_chan_to(const struct node *n, const struct chan *chan)
{
	int idx = (chan->nodes[1] == n);

	assert(chan->nodes[0] == n || chan->nodes[1] == n);
	return !idx;
}

struct routing_state {
	/* Which chain we're on */
	const struct chainparams *chainparams;

	/* All known nodes. */
	struct node_map *nodes;

	/* node_announcements which are waiting on pending_cannouncement */
	struct pending_node_map *pending_node_map;

	/* channel_announcement which are pending short_channel_id lookup */
	struct pending_cannouncement_map pending_cannouncements;

	/* Gossip store */
	struct gossip_store *gs;

	/* Our own ID so we can identify local channels */
	struct node_id local_id;

	/* How old does a channel have to be before we prune it? */
	u32 prune_timeout;

        /* A map of channels indexed by short_channel_ids */
	UINTMAP(struct chan *) chanmap;

        /* A map of channel_announcements indexed by short_channel_ids:
	 * we haven't got a channel_update for these yet. */
	UINTMAP(struct unupdated_channel *) unupdated_chanmap;

	/* Has one of our own channels been announced? */
	bool local_channel_announced;

	/* Cache for txout queries that failed. Allows us to skip failed
	 * checks if we get another announcement for the same scid. */
	UINTMAP(bool) txout_failures;

        /* A map of (local) disabled channels by short_channel_ids */
	struct chan_map local_disabled_map;

#if DEVELOPER
	/* Override local time for gossip messages */
	struct timeabs *gossip_time;
#endif
};

static inline struct chan *
get_channel(const struct routing_state *rstate,
	    const struct short_channel_id *scid)
{
	return uintmap_get(&rstate->chanmap, scid->u64);
}

struct route_hop {
	struct short_channel_id channel_id;
	int direction;
	struct node_id nodeid;
	struct amount_msat amount;
	u32 delay;
};

struct routing_state *new_routing_state(const tal_t *ctx,
					const struct chainparams *chainparams,
					const struct node_id *local_id,
					u32 prune_timeout,
					struct list_head *peers,
					const u32 *dev_gossip_time);

/**
 * Add a new bidirectional channel from id1 to id2 with the given
 * short_channel_id and capacity to the local network view. The channel may not
 * already exist, and might create the node entries for the two endpoints, if
 * they do not exist yet.
 */
struct chan *new_chan(struct routing_state *rstate,
		      const struct short_channel_id *scid,
		      const struct node_id *id1,
		      const struct node_id *id2,
		      struct amount_sat sat);

/* Handlers for incoming messages */

/**
 * handle_channel_announcement -- Check channel announcement is valid
 *
 * Returns error message if we should fail channel.  Make *scid non-NULL
 * (for checking) if we extracted a short_channel_id, otherwise ignore.
 */
u8 *handle_channel_announcement(struct routing_state *rstate,
				const u8 *announce TAKES,
				const struct short_channel_id **scid);

/**
 * handle_pending_cannouncement -- handle channel_announce once we've
 * completed short_channel_id lookup.
 */
void handle_pending_cannouncement(struct routing_state *rstate,
				  const struct short_channel_id *scid,
				  const struct amount_sat sat,
				  const u8 *txscript);

/* Iterate through channels in a node */
struct chan *first_chan(const struct node *node, struct chan_map_iter *i);
struct chan *next_chan(const struct node *node, struct chan_map_iter *i);

/* Returns NULL if all OK, otherwise an error for the peer which sent. */
u8 *handle_channel_update(struct routing_state *rstate, const u8 *update TAKES,
			  const char *source);

/* Returns NULL if all OK, otherwise an error for the peer which sent. */
u8 *handle_node_announcement(struct routing_state *rstate, const u8 *node);

/* Get a node: use this instead of node_map_get() */
struct node *get_node(struct routing_state *rstate,
		      const struct node_id *id);

/* Compute a route to a destination, for a given amount and riskfactor. */
struct route_hop *get_route(const tal_t *ctx, struct routing_state *rstate,
			    const struct node_id *source,
			    const struct node_id *destination,
			    const struct amount_msat msat, double riskfactor,
			    u32 final_cltv,
			    double fuzz,
			    u64 seed,
			    const struct short_channel_id_dir *excluded,
			    size_t max_hops);
/* Disable channel(s) based on the given routing failure. */
void routing_failure(struct routing_state *rstate,
		     const struct node_id *erring_node,
		     const struct short_channel_id *erring_channel,
		     int erring_direction,
		     enum onion_type failcode,
		     const u8 *channel_update);

void route_prune(struct routing_state *rstate);

/**
 * Add a channel_announcement to the network view without checking it
 *
 * Directly add the channel to the local network, without checking it first. Use
 * this only for messages from trusted sources. Untrusted sources should use the
 * @see{handle_channel_announcement} entrypoint to check before adding.
 *
 * index is usually 0, in which case it's set by insert_broadcast adding it
 * to the store.
 */
bool routing_add_channel_announcement(struct routing_state *rstate,
				      const u8 *msg TAKES,
				      struct amount_sat sat,
				      u32 index);

/**
 * Add a channel_update without checking for errors
 *
 * Used to actually insert the information in the channel update into the local
 * network view. Only use this for messages that are known to be good. For
 * untrusted source, requiring verification please use
 * @see{handle_channel_update}
 */
bool routing_add_channel_update(struct routing_state *rstate,
				const u8 *update TAKES,
				u32 index);

/**
 * Add a node_announcement to the network view without checking it
 *
 * Directly add the node being announced to the network view, without verifying
 * it. This must be from a trusted source, e.g., gossip_store. For untrusted
 * sources (peers) please use @see{handle_node_announcement}.
 */
bool routing_add_node_announcement(struct routing_state *rstate,
				   const u8 *msg TAKES,
				   u32 index);


/**
 * Add a local channel.
 *
 * Entrypoint to add a local channel that was not learned through gossip. This
 * is the case for private channels or channels that have not yet reached
 * `announce_depth`.
 */
bool handle_local_add_channel(struct routing_state *rstate, const u8 *msg,
			      u64 index);

#if DEVELOPER
void memleak_remove_routing_tables(struct htable *memtable,
				   const struct routing_state *rstate);
#endif

/**
 * Get the local time.
 *
 * This gets overridden in dev mode so we can use canned (stale) gossip.
 */
struct timeabs gossip_time_now(const struct routing_state *rstate);

/* Because we can have millions of channels, and we only want a local_disable
 * flag on ones connected to us, we keep a separate hashtable for that flag.
 */
static inline bool is_chan_local_disabled(struct routing_state *rstate,
					  const struct chan *chan)
{
	return chan_map_get(&rstate->local_disabled_map, &chan->scid) != NULL;
}

static inline void local_disable_chan(struct routing_state *rstate,
				      const struct chan *chan)
{
	if (!is_chan_local_disabled(rstate, chan))
		chan_map_add(&rstate->local_disabled_map, chan);
}

static inline void local_enable_chan(struct routing_state *rstate,
				     const struct chan *chan)
{
	chan_map_del(&rstate->local_disabled_map, chan);
}

/* Helper to convert on-wire addresses format to wireaddrs array */
struct wireaddr *read_addresses(const tal_t *ctx, const u8 *ser);

/* Remove channel from store: announcement and any updates. */
void remove_channel_from_store(struct routing_state *rstate,
			       struct chan *chan);

#endif /* LIGHTNING_GOSSIPD_ROUTING_H */

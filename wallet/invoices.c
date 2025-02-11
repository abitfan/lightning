#include "db.h"
#include "invoices.h"
#include "wallet.h"
#include <assert.h>
#include <ccan/list/list.h>
#include <ccan/tal/str/str.h>
#include <ccan/time/time.h>
#include <ccan/timer/timer.h>
#include <common/amount.h>
#include <common/timeout.h>
#include <common/utils.h>
#include <lightningd/invoice.h>
#include <lightningd/log.h>
#include <sodium/randombytes.h>
#include <sqlite3.h>
#include <string.h>

#define INVOICE_TBL_FIELDS "state, payment_key, payment_hash, label, msatoshi, expiry_time, pay_index, msatoshi_received, paid_timestamp, bolt11, description"

struct invoice_waiter {
	/* Is this waiter already triggered? */
	bool triggered;
	/* Is this waiting for any invoice to resolve? */
	bool any;
	/* If !any, the specific invoice this is waiting on */
	u64 id;

	struct list_node list;

	/* The callback to use */
	void (*cb)(const struct invoice *, void*);
	void *cbarg;
};

struct invoices {
	/* The database connection to use. */
	struct db *db;
	/* The log to report to. */
	struct log *log;
	/* The timers object to use for expirations. */
	struct timers *timers;
	/* Waiters waiting for invoices to be paid, expired, or deleted. */
	struct list_head waiters;
	/* Earliest time for some invoice to expire */
	u64 min_expiry_time;
	/* Expiration timer */
	struct oneshot *expiration_timer;
};

static void trigger_invoice_waiter(struct invoice_waiter *w,
				   const struct invoice *invoice)
{
	w->triggered = true;
	w->cb(invoice, w->cbarg);
}

static void trigger_invoice_waiter_resolve(struct invoices *invoices,
					   u64 id,
					   const struct invoice *invoice)
{
	struct invoice_waiter *w;
	struct invoice_waiter *n;

	list_for_each_safe(&invoices->waiters, w, n, list) {
		if (!w->any && w->id != id)
			continue;
		list_del_from(&invoices->waiters, &w->list);
		tal_steal(tmpctx, w);
		trigger_invoice_waiter(w, invoice);
	}
}
static void
trigger_invoice_waiter_expire_or_delete(struct invoices *invoices,
					u64 id,
					const struct invoice *invoice)
{
	struct invoice_waiter *w;
	struct invoice_waiter *n;

	list_for_each_safe(&invoices->waiters, w, n, list) {
		if (w->any || w->id != id)
			continue;
		list_del_from(&invoices->waiters, &w->list);
		tal_steal(tmpctx, w);
		trigger_invoice_waiter(w, invoice);
	}
}

static struct invoice_details *wallet_stmt2invoice_details(const tal_t *ctx,
							   sqlite3_stmt *stmt)
{
	struct invoice_details *dtl = tal(ctx, struct invoice_details);
	dtl->state = sqlite3_column_int(stmt, 0);

	sqlite3_column_preimage(stmt, 1, &dtl->r);

	sqlite3_column_sha256(stmt, 2, &dtl->rhash);

	dtl->label = sqlite3_column_json_escaped(dtl, stmt, 3);

	if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
		dtl->msat = tal(dtl, struct amount_msat);
		*dtl->msat = sqlite3_column_amount_msat(stmt, 4);
	} else {
		dtl->msat = NULL;
	}

	dtl->expiry_time = sqlite3_column_int64(stmt, 5);

	if (dtl->state == PAID) {
		dtl->pay_index = sqlite3_column_int64(stmt, 6);
		dtl->received = sqlite3_column_amount_msat(stmt, 7);
		dtl->paid_timestamp = sqlite3_column_int64(stmt, 8);
	}

	dtl->bolt11 = tal_strndup(dtl, sqlite3_column_blob(stmt, 9),
				  sqlite3_column_bytes(stmt, 9));

	if (sqlite3_column_type(stmt, 10) != SQLITE_NULL)
		dtl->description = tal_strdup(
		    dtl, (const char *)sqlite3_column_text(stmt, 10));
	else
		dtl->description = NULL;

	return dtl;
}

/* Update expirations. */
static void update_db_expirations(struct invoices *invoices, u64 now)
{
	sqlite3_stmt *stmt;
	stmt = db_prepare(invoices->db,
			  "UPDATE invoices"
			  "   SET state = ?"
			  " WHERE state = ?"
			  "   AND expiry_time <= ?;");
	sqlite3_bind_int(stmt, 1, EXPIRED);
	sqlite3_bind_int(stmt, 2, UNPAID);
	sqlite3_bind_int64(stmt, 3, now);
	db_exec_prepared(invoices->db, stmt);
}

static void install_expiration_timer(struct invoices *invoices);

struct invoices *invoices_new(const tal_t *ctx,
			      struct db *db,
			      struct log *log,
			      struct timers *timers)
{
	struct invoices *invs = tal(ctx, struct invoices);

	invs->db = db;
	invs->log = log;
	invs->timers = timers;

	list_head_init(&invs->waiters);

	invs->expiration_timer = NULL;

	update_db_expirations(invs, time_now().ts.tv_sec);
	install_expiration_timer(invs);
	return invs;
}

struct invoice_id_node {
	struct list_node list;
	u64 id;
};

static void trigger_expiration(struct invoices *invoices)
{
	struct list_head idlist;
	struct invoice_id_node *idn;
	u64 now = time_now().ts.tv_sec;
	sqlite3_stmt *stmt;
	struct invoice i;

	/* Free current expiration timer */
	invoices->expiration_timer = tal_free(invoices->expiration_timer);

	/* Acquire all expired invoices and save them in a list */
	list_head_init(&idlist);
	stmt = db_select_prepare(invoices->db,
				 "id"
				 "  FROM invoices"
				 " WHERE state = ?"
				 "   AND expiry_time <= ?;");
	sqlite3_bind_int(stmt, 1, UNPAID);
	sqlite3_bind_int64(stmt, 2, now);
	while (db_select_step(invoices->db, stmt)) {
		idn = tal(tmpctx, struct invoice_id_node);
		list_add_tail(&idlist, &idn->list);
		idn->id = sqlite3_column_int64(stmt, 0);
	}

	/* Expire all those invoices */
	update_db_expirations(invoices, now);

	/* Trigger expirations */
	list_for_each(&idlist, idn, list) {
		/* Trigger expiration */
		i.id = idn->id;
		trigger_invoice_waiter_expire_or_delete(invoices,
							idn->id,
							&i);
	}

	install_expiration_timer(invoices);
}

static void install_expiration_timer(struct invoices *invoices)
{
	bool res;
	sqlite3_stmt *stmt;
	struct timerel rel;
	struct timeabs expiry;
	struct timeabs now = time_now();

	assert(!invoices->expiration_timer);

	/* Find unpaid invoice with nearest expiry time */
	stmt = db_select_prepare(invoices->db,
				 "MIN(expiry_time)"
				 "  FROM invoices"
				 " WHERE state = ?;");
	sqlite3_bind_int(stmt, 1, UNPAID);
	res = db_select_step(invoices->db, stmt);
	assert(res);
	if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) {
		/* Nothing to install */
		db_stmt_done(stmt);
		return;
	} else
		invoices->min_expiry_time = sqlite3_column_int64(stmt, 0);
	db_stmt_done(stmt);

	memset(&expiry, 0, sizeof(expiry));
	expiry.ts.tv_sec = invoices->min_expiry_time;

	/* now > expiry */
	if (time_after(now, expiry))
		expiry = now;

	/* rel = expiry - now */
	rel = time_between(expiry, now);

	/* Have it called at indicated timerel. */
	invoices->expiration_timer = new_reltimer(invoices->timers,
						  invoices,
						  rel,
						  &trigger_expiration,
						  invoices);
}

bool invoices_create(struct invoices *invoices,
		     struct invoice *pinvoice,
		     const struct amount_msat *msat TAKES,
		     const struct json_escaped *label TAKES,
		     u64 expiry,
		     const char *b11enc,
		     const char *description,
		     const struct preimage *r,
		     const struct sha256 *rhash)
{
	sqlite3_stmt *stmt;
	struct invoice dummy;
	u64 expiry_time;
	u64 now = time_now().ts.tv_sec;

	if (invoices_find_by_label(invoices, &dummy, label)) {
		if (taken(msat))
			tal_free(msat);
		if (taken(label))
			tal_free(label);
		return false;
	}

	/* Compute expiration. */
	expiry_time = now + expiry;

	/* Save to database. */
	/* Need to use the lower level API of sqlite3 to bind
	 * label. Otherwise we'd need to implement sanitization of
	 * that string for sql injections... */
	stmt = db_prepare(invoices->db,
			  "INSERT INTO invoices"
			  "            ( payment_hash, payment_key, state"
			  "            , msatoshi, label, expiry_time"
			  "            , pay_index, msatoshi_received"
			  "            , paid_timestamp, bolt11, description)"
			  "     VALUES ( ?, ?, ?"
			  "            , ?, ?, ?"
			  "            , NULL, NULL"
			  "            , NULL, ?, ?);");

	sqlite3_bind_blob(stmt, 1, rhash, sizeof(struct sha256), SQLITE_TRANSIENT);
	sqlite3_bind_blob(stmt, 2, r, sizeof(struct preimage), SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, UNPAID);
	if (msat)
		sqlite3_bind_amount_msat(stmt, 4, *msat);
	else
		sqlite3_bind_null(stmt, 4);
	sqlite3_bind_json_escaped(stmt, 5, label);
	sqlite3_bind_int64(stmt, 6, expiry_time);
	sqlite3_bind_text(stmt, 7, b11enc, strlen(b11enc), SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 8, description, strlen(description), SQLITE_TRANSIENT);

	db_exec_prepared(invoices->db, stmt);

	pinvoice->id = sqlite3_last_insert_rowid(invoices->db->sql);

	/* Install expiration trigger. */
	if (!invoices->expiration_timer ||
	    expiry_time < invoices->min_expiry_time) {
		invoices->expiration_timer
			= tal_free(invoices->expiration_timer);
		install_expiration_timer(invoices);
	}

	if (taken(msat))
		tal_free(msat);
	if (taken(label))
		tal_free(label);
	return true;
}


bool invoices_find_by_label(struct invoices *invoices,
			    struct invoice *pinvoice,
			    const struct json_escaped *label)
{
	sqlite3_stmt *stmt;

	stmt = db_select_prepare(invoices->db,
				 "id"
				 "  FROM invoices"
				 " WHERE label = ?;");
	sqlite3_bind_json_escaped(stmt, 1, label);
	if (!db_select_step(invoices->db, stmt))
		return false;

	pinvoice->id = sqlite3_column_int64(stmt, 0);
	db_stmt_done(stmt);
	return true;
}

bool invoices_find_by_rhash(struct invoices *invoices,
			    struct invoice *pinvoice,
			    const struct sha256 *rhash)
{
	sqlite3_stmt *stmt;

	stmt = db_select_prepare(invoices->db,
				 "id"
				 "  FROM invoices"
				 " WHERE payment_hash = ?;");
	sqlite3_bind_blob(stmt, 1, rhash, sizeof(*rhash), SQLITE_TRANSIENT);
	if (!db_select_step(invoices->db, stmt))
		return false;

	pinvoice->id = sqlite3_column_int64(stmt, 0);
	db_stmt_done(stmt);
	return true;
}

bool invoices_find_unpaid(struct invoices *invoices,
			  struct invoice *pinvoice,
			  const struct sha256 *rhash)
{
	sqlite3_stmt *stmt;

	stmt = db_select_prepare(invoices->db,
				 " id"
				 "  FROM invoices"
				 " WHERE payment_hash = ?"
				 "   AND state = ?;");
	sqlite3_bind_blob(stmt, 1, rhash, sizeof(*rhash), SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, UNPAID);
	if (!db_select_step(invoices->db, stmt))
		return false;

	pinvoice->id = sqlite3_column_int64(stmt, 0);
	db_stmt_done(stmt);
	return true;
}

bool invoices_delete(struct invoices *invoices,
		     struct invoice invoice)
{
	sqlite3_stmt *stmt;

	/* Delete from database. */
	stmt = db_prepare(invoices->db, "DELETE FROM invoices WHERE id=?;");
	sqlite3_bind_int64(stmt, 1, invoice.id);
	db_exec_prepared(invoices->db, stmt);

	if (sqlite3_changes(invoices->db->sql) != 1)
		return false;

	/* Tell all the waiters about the fact that it was deleted. */
	trigger_invoice_waiter_expire_or_delete(invoices,
						invoice.id, NULL);
	return true;
}

void invoices_delete_expired(struct invoices *invoices,
			     u64 max_expiry_time)
{
	sqlite3_stmt *stmt;
	stmt = db_prepare(invoices->db,
			  "DELETE FROM invoices"
			  " WHERE state = ?"
			  "   AND expiry_time <= ?;");
	sqlite3_bind_int(stmt, 1, EXPIRED);
	sqlite3_bind_int64(stmt, 2, max_expiry_time);
	db_exec_prepared(invoices->db, stmt);
}

bool invoices_iterate(struct invoices *invoices,
		      struct invoice_iterator *it)
{
	sqlite3_stmt *stmt;

	if (!it->p) {
		stmt = db_select_prepare(invoices->db,
					 INVOICE_TBL_FIELDS
					 " FROM invoices;");
		it->p = stmt;
	} else
		stmt = it->p;

	if (db_select_step(invoices->db, stmt))
		return true;

	it->p = NULL;
	return false;
}

const struct invoice_details *
invoices_iterator_deref(const tal_t *ctx, struct invoices *invoices UNUSED,
			const struct invoice_iterator *it)
{
	assert(it->p);
	return wallet_stmt2invoice_details(ctx, (sqlite3_stmt*) it->p);
}

static s64 get_next_pay_index(struct db *db)
{
	/* Equivalent to (next_pay_index++) */
	s64 next_pay_index;
	next_pay_index = db_get_intvar(db, "next_pay_index", 0);
	/* Variable should exist. */
	assert(next_pay_index > 0);
	db_set_intvar(db, "next_pay_index", next_pay_index + 1);
	return next_pay_index;
}

static enum invoice_status invoice_get_status(struct invoices *invoices, struct invoice invoice)
{
	sqlite3_stmt *stmt;
	enum invoice_status state;
	bool res;

	stmt = db_select_prepare(invoices->db,
				 "state FROM invoices WHERE id = ?;");
	sqlite3_bind_int64(stmt, 1, invoice.id);
	res = db_select_step(invoices->db, stmt);
	assert(res);
	state = sqlite3_column_int(stmt, 0);
	db_stmt_done(stmt);
	return state;
}

void invoices_resolve(struct invoices *invoices,
		      struct invoice invoice,
		      struct amount_msat received)
{
	sqlite3_stmt *stmt;
	s64 pay_index;
	u64 paid_timestamp;
	enum invoice_status state = invoice_get_status(invoices, invoice);

	assert(state == UNPAID);

	/* Assign a pay-index. */
	pay_index = get_next_pay_index(invoices->db);
	paid_timestamp = time_now().ts.tv_sec;

	/* Update database. */
	stmt = db_prepare(invoices->db,
			  "UPDATE invoices"
			  "   SET state=?"
			  "     , pay_index=?"
			  "     , msatoshi_received=?"
			  "     , paid_timestamp=?"
			  " WHERE id=?;");
	sqlite3_bind_int(stmt, 1, PAID);
	sqlite3_bind_int64(stmt, 2, pay_index);
	sqlite3_bind_amount_msat(stmt, 3, received);
	sqlite3_bind_int64(stmt, 4, paid_timestamp);
	sqlite3_bind_int64(stmt, 5, invoice.id);
	db_exec_prepared(invoices->db, stmt);

	/* Tell all the waiters about the paid invoice. */
	trigger_invoice_waiter_resolve(invoices, invoice.id, &invoice);
}

/* Called when an invoice waiter is destructed. */
static void destroy_invoice_waiter(struct invoice_waiter *w)
{
	/* Already triggered. */
	if (w->triggered)
		return;
	list_del(&w->list);
}

/* Add an invoice waiter to the specified list of invoice waiters. */
static void add_invoice_waiter(const tal_t *ctx,
			       struct list_head *waiters,
			       bool any,
			       u64 id,
			       void (*cb)(const struct invoice *, void*),
			       void* cbarg)
{
	struct invoice_waiter *w = tal(ctx, struct invoice_waiter);
	w->triggered = false;
	w->any = any;
	w->id = id;
	list_add_tail(waiters, &w->list);
	w->cb = cb;
	w->cbarg = cbarg;
	tal_add_destructor(w, &destroy_invoice_waiter);
}


void invoices_waitany(const tal_t *ctx,
		      struct invoices *invoices,
		      u64 lastpay_index,
		      void (*cb)(const struct invoice *, void*),
		      void *cbarg)
{
	sqlite3_stmt *stmt;
	struct invoice invoice;

	/* Look for an already-paid invoice. */
	stmt = db_select_prepare(invoices->db,
				 "id"
				 "  FROM invoices"
				 " WHERE pay_index NOT NULL"
				 "   AND pay_index > ?"
				 " ORDER BY pay_index ASC LIMIT 1;");
	sqlite3_bind_int64(stmt, 1, lastpay_index);

	if (db_select_step(invoices->db, stmt)) {
		invoice.id = sqlite3_column_int64(stmt, 0);
		db_stmt_done(stmt);

		cb(&invoice, cbarg);
		return;
	}

	/* None found. */
	add_invoice_waiter(ctx, &invoices->waiters,
			   true, 0, cb, cbarg);
}


void invoices_waitone(const tal_t *ctx,
		      struct invoices *invoices,
		      struct invoice invoice,
		      void (*cb)(const struct invoice *, void*),
		      void *cbarg)
{
	enum invoice_status state;

	state = invoice_get_status(invoices, invoice);

	if (state == PAID || state == EXPIRED) {
		cb(&invoice, cbarg);
		return;
	}

	/* Not yet paid. */
	add_invoice_waiter(ctx, &invoices->waiters,
			   false, invoice.id, cb, cbarg);
}

const struct invoice_details *invoices_get_details(const tal_t *ctx,
						   struct invoices *invoices,
						   struct invoice invoice)
{
	sqlite3_stmt *stmt;
	bool res;
	struct invoice_details *details;

	stmt = db_select_prepare(invoices->db,
				 INVOICE_TBL_FIELDS
				 " FROM invoices"
				 " WHERE id = ?;");
	sqlite3_bind_int64(stmt, 1, invoice.id);
	res = db_select_step(invoices->db, stmt);
	assert(res);

	details = wallet_stmt2invoice_details(ctx, stmt);
	db_stmt_done(stmt);
	return details;
}

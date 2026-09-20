/*-------------------------------------------------------------------------
 *
 * pg_horizon.c
 *	  Module init, GUCs, and shared helpers
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 * IDENTIFICATION
 *	  pg_horizon.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/multixact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "pg_horizon.h"

PG_MODULE_MAGIC;

int			pg_horizon_min_xmin_age = 0;
bool		pg_horizon_terminate_blockers = false;

void		_PG_init(void);

/*
 * Copy src into dst (size dstsize, including the terminator) without cutting a
 * multibyte character in half. Plain strlcpy() would leave invalid encoding
 * in the result whenever the cut lands inside a character.
 */
void
horizon_copy_clip(char *dst, size_t dstsize, const char *src)
{
	size_t		len;

	if (dstsize == 0)
		return;
	if (src == NULL)
	{
		dst[0] = '\0';
		return;
	}
	len = pg_mbcliplen(src, strlen(src), (int) (dstsize - 1));
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* printf into a fixed buffer, clipped on a character boundary. */
static void pg_attribute_printf(3, 4)
horizon_setf(char *dst, size_t dstsize, const char *fmt,...)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(&buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(&buf, needed);
	}
	horizon_copy_clip(dst, dstsize, buf.data);
	pfree(buf.data);
}

int64
horizon_xid_age(TransactionId xid, TransactionId next_xid)
{
	if (!TransactionIdIsValid(xid) || !TransactionIdIsValid(next_xid))
		return 0;

	return (int64) (uint32) (next_xid - xid);
}

int64
horizon_xid_remaining(TransactionId now, TransactionId limit)
{
	if (!TransactionIdIsValid(now) || !TransactionIdIsValid(limit))
		return 0;
	if (TransactionIdFollowsOrEquals(now, limit))
		return 0;

	return (int64) (uint32) (limit - now);
}

/*
 * Severity is derived from the limits the core XID generator itself uses
 * (xidVacLimit, xidWarnLimit, xidStopLimit) plus the failsafe ages, with one
 * pg_horizon-specific cutoff: fewer than HORIZON_EMERGENCY_HEADROOM XIDs left
 * before the stop limit is an emergency.
 *
 * Every CRITICAL condition is tested before any WARNING one. (Passing the
 * failsafe age implies passing xidVacLimit, so testing the vacuum limit first
 * would make the failsafe check unreachable.)
 *
 * *why receives a palloc'd one-line explanation of the decisive condition.
 */
HorizonSeverity
horizon_compute_severity(const HorizonSnapshot *snap, char **why)
{
	int64		headroom;
	int64		xage;
	int64		mage;
	int			i;

	headroom = horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit);
	xage = horizon_xid_age(snap->oldest_xid, snap->next_xid);
	mage = horizon_xid_age(snap->oldest_mxid, snap->next_mxid);

#define HORIZON_WHY(sev, ...) \
	do { \
		if (why) *why = psprintf(__VA_ARGS__); \
		return (sev); \
	} while (0)

	if (TransactionIdIsValid(snap->xid_stop_limit) &&
		TransactionIdFollowsOrEquals(snap->next_xid, snap->xid_stop_limit))
		HORIZON_WHY(HORIZON_SEV_EMERGENCY,
					"next_xid has reached xid_stop_limit; PostgreSQL refuses new transaction IDs");

	if (headroom > 0 && headroom < HORIZON_EMERGENCY_HEADROOM)
		HORIZON_WHY(HORIZON_SEV_EMERGENCY,
					"only %lld transaction IDs remain before xid_stop_limit",
					(long long) headroom);

	if (TransactionIdIsValid(snap->xid_warn_limit) &&
		TransactionIdFollowsOrEquals(snap->next_xid, snap->xid_warn_limit))
		HORIZON_WHY(HORIZON_SEV_CRITICAL,
					"next_xid has passed xid_warn_limit (the server is already logging wraparound warnings)");

	if (vacuum_failsafe_age > 0 && xage >= vacuum_failsafe_age)
		HORIZON_WHY(HORIZON_SEV_CRITICAL,
					"oldest XID age %lld has reached vacuum_failsafe_age (%d)",
					(long long) xage, vacuum_failsafe_age);

	if (vacuum_multixact_failsafe_age > 0 && mage >= vacuum_multixact_failsafe_age)
		HORIZON_WHY(HORIZON_SEV_CRITICAL,
					"oldest MultiXact age %lld has reached vacuum_multixact_failsafe_age (%d)",
					(long long) mage, vacuum_multixact_failsafe_age);

	if (TransactionIdIsValid(snap->xid_vac_limit) &&
		TransactionIdFollowsOrEquals(snap->next_xid, snap->xid_vac_limit))
		HORIZON_WHY(HORIZON_SEV_WARNING,
					"oldest XID age %lld is past autovacuum_freeze_max_age (%d); anti-wraparound vacuum is due",
					(long long) xage, autovacuum_freeze_max_age);

	if (autovacuum_multixact_freeze_max_age > 0 &&
		mage >= (int64) autovacuum_multixact_freeze_max_age)
		HORIZON_WHY(HORIZON_SEV_WARNING,
					"oldest MultiXact age %lld is past autovacuum_multixact_freeze_max_age (%d); anti-wraparound vacuum is due",
					(long long) mage, autovacuum_multixact_freeze_max_age);

	for (i = 0; i < snap->nblockers; i++)
	{
		const HorizonBlocker *b = snap->blockers[i];

		if (!b->is_horizon_holder || b->is_observer)
			continue;
		if (b->age < HORIZON_STALE_HOLDER_AGE)
			continue;

		if (b->type == HORIZON_BLK_BACKEND && horizon_blocker_is_idle_xact(b))
			HORIZON_WHY(HORIZON_SEV_WARNING,
						"pid %d has been idle in transaction holding the horizon for %lld XIDs",
						b->pid, (long long) b->age);

		if ((b->type == HORIZON_BLK_PHYSICAL_SLOT ||
			 b->type == HORIZON_BLK_LOGICAL_SLOT) && !b->slot_active)
			HORIZON_WHY(HORIZON_SEV_WARNING,
						"inactive replication slot \"%s\" has held the horizon for %lld XIDs",
						b->slot_name, (long long) b->age);

		if (b->type == HORIZON_BLK_PREPARED)
			HORIZON_WHY(HORIZON_SEV_WARNING,
						"prepared transaction \"%s\" has held the horizon for %lld XIDs",
						b->prepared_gid[0] ? b->prepared_gid : "(unknown gid)",
						(long long) b->age);
	}
#undef HORIZON_WHY

	if (why)
		*why = pstrdup("no wraparound pressure and no stale horizon holder");
	return HORIZON_SEV_OK;
}

const char *
horizon_severity_name(HorizonSeverity sev)
{
	switch (sev)
	{
		case HORIZON_SEV_OK:
			return "ok";
		case HORIZON_SEV_WARNING:
			return "warning";
		case HORIZON_SEV_CRITICAL:
			return "critical";
		case HORIZON_SEV_EMERGENCY:
			return "emergency";
	}
	return "ok";
}

const char *
horizon_blocker_type_name(HorizonBlockerType t)
{
	switch (t)
	{
		case HORIZON_BLK_BACKEND:
			return "backend";
		case HORIZON_BLK_PREPARED:
			return "prepared";
		case HORIZON_BLK_PHYSICAL_SLOT:
			return "physical_slot";
		case HORIZON_BLK_LOGICAL_SLOT:
			return "logical_slot";
		case HORIZON_BLK_STANDBY_FEEDBACK:
			return "standby_feedback";
	}
	return "backend";
}

/* Same rule pg_stat_activity uses for the details of another role's session. */
bool
horizon_has_stat_visibility(Oid roleId)
{
	if (superuser())
		return true;
	if (has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
		return true;
	if (OidIsValid(roleId) && roleId == GetUserId())
		return true;
	return false;
}

bool
horizon_blocker_is_idle_xact(const HorizonBlocker *b)
{
	return strcmp(b->state, "idle in transaction") == 0 ||
		strcmp(b->state, "idle in transaction (aborted)") == 0;
}

bool
horizon_blocker_safe_to_terminate(const HorizonBlocker *b, bool force)
{
	if (b->type != HORIZON_BLK_BACKEND)
		return false;
	if (b->pid <= 0 || b->pid == MyProcPid)
		return false;
	if (b->is_observer)
		return false;
	if (b->statusFlags & PROC_VACUUM_FOR_WRAPAROUND)
		return false;

	/*
	 * The holder for hot_standby_feedback is on the standby. Killing the
	 * walsender drops a live replica and does not COMMIT that snapshot.
	 */
	if (strcmp(b->backend_type, "walsender") == 0)
		return false;
	if (strcmp(b->backend_type, "autovacuum worker") == 0)
		return false;
	if (strcmp(b->backend_type, "autovacuum launcher") == 0)
		return false;
	if (horizon_blocker_is_idle_xact(b))
		return true;
	if (force && strcmp(b->state, "active") == 0)
		return true;
	return false;
}

/* Does this holder pass pg_horizon.min_xmin_age? */
bool
horizon_blocker_listed(const HorizonBlocker *b)
{
	if (pg_horizon_min_xmin_age <= 0)
		return true;
	return b->age >= pg_horizon_min_xmin_age;
}

/*
 * Number of non-observer holders that pass pg_horizon.min_xmin_age, optionally
 * only those that pin a horizon. This is the definition behind blocker_count
 * and horizon_holders, and it matches the rows pg_horizon_blockers returns.
 */
int
horizon_count_listed(const HorizonSnapshot *snap, bool holders_only)
{
	int			n = 0;
	int			i;

	for (i = 0; i < snap->nblockers; i++)
	{
		const HorizonBlocker *b = snap->blockers[i];

		if (b->is_observer || !horizon_blocker_listed(b))
			continue;
		if (holders_only && !b->is_horizon_holder)
			continue;
		n++;
	}
	return n;
}

/*
 * The oldest listed holder that actually pins a horizon: any horizon when
 * have_kind is false, else the horizon of the given relation kind. Rows that
 * merely have an xmin but pin nothing are never "dominant".
 */
HorizonBlocker *
horizon_dominant_blocker(const HorizonSnapshot *snap, bool have_kind,
						 HorizonKind kind)
{
	HorizonBlocker *best = NULL;
	int			i;

	if (have_kind && kind == HORIZON_KIND_TEMP)
		return NULL;			/* only this backend can hold a temp horizon */

	for (i = 0; i < snap->nblockers; i++)
	{
		HorizonBlocker *b = snap->blockers[i];
		bool		holds;

		if (b->is_observer || !horizon_blocker_listed(b))
			continue;

		if (!have_kind)
			holds = b->is_horizon_holder;
		else if (kind == HORIZON_KIND_SHARED)
			holds = b->holds_shared;
		else if (kind == HORIZON_KIND_CATALOG)
			holds = b->holds_catalog;
		else
			holds = b->holds_data;

		if (!holds)
			continue;
		if (best == NULL || b->age > best->age)
			best = b;
	}
	return best;
}

/*
 * Mirror of GlobalVisHorizonKindForRel(). The caller supplies the catalog
 * fields so this works from a pg_class scan without opening (locking) the
 * relation.
 */
HorizonKind
horizon_kind_for_class(Oid relid, Oid relnamespace, bool relisshared,
					   char relpersistence, bool user_catalog,
					   const HorizonSnapshot *snap)
{
	if (relisshared || snap->in_recovery)
		return HORIZON_KIND_SHARED;
	if (IsCatalogRelationOid(relid) || user_catalog)
		return HORIZON_KIND_CATALOG;
	if (relpersistence == RELPERSISTENCE_TEMP &&
		(isTempNamespace(relnamespace) || isTempToastNamespace(relnamespace)))
		return HORIZON_KIND_TEMP;
	return HORIZON_KIND_DATA;
}

TransactionId
horizon_xmin_for_kind(const HorizonSnapshot *snap, HorizonKind kind)
{
	switch (kind)
	{
		case HORIZON_KIND_SHARED:
			return snap->shared_xmin;
		case HORIZON_KIND_CATALOG:
			return snap->catalog_xmin;
		case HORIZON_KIND_TEMP:
			return snap->temp_xmin;
		case HORIZON_KIND_DATA:
			break;
	}
	return snap->data_xmin;
}

static const char *
horizon_pin_scope(const HorizonBlocker *b)
{
	if (b->holds_data)
		return "the data horizon of its database";
	if (b->holds_catalog)
		return "the catalog horizon of its database";
	return "the shared-catalog horizon";
}

void
horizon_fill_blocker_reason(HorizonBlocker *b, const HorizonSnapshot *snap)
{
	const char *pin;

	b->reason[0] = '\0';
	b->recommended_sql[0] = '\0';

	/*
	 * Same disclosure rule as pg_stat_activity: what another role's backend
	 * is doing (state, query, wait events) is not for every caller. Replication
	 * slots and prepared transactions are public catalogs, so they stay
	 * visible.
	 */
	if (!b->visible)
	{
		horizon_setf(b->reason, sizeof(b->reason),
					 "Details of this session are not visible to this role (superuser, pg_read_all_stats, or the session's own role is required).");
		return;
	}

	if (b->is_horizon_holder)
		pin = horizon_pin_scope(b);
	else
		pin = NULL;

	switch (b->type)
	{
		case HORIZON_BLK_BACKEND:
			if (horizon_blocker_is_idle_xact(b))
			{
				if (pin)
					horizon_setf(b->reason, sizeof(b->reason),
								 "Session is idle in transaction and pins %s (xid/xmin age %lld). VACUUM cannot remove or freeze anything newer than its snapshot. The real fix is to COMMIT or ROLLBACK the abandoned transaction in the application, or to terminate the backend if the client is gone.",
								 pin, (long long) b->age);
				else
					horizon_setf(b->reason, sizeof(b->reason),
								 "Session is idle in transaction (xid/xmin age %lld) but is not the oldest pin, so it is not what limits VACUUM right now. It will matter once the older holders are gone.",
								 (long long) b->age);
				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT pid, usename, state, xact_start, left(query, 80) AS query FROM pg_stat_activity WHERE pid = %d;\n"
							 "-- Prefer COMMIT/ROLLBACK from the owning application. Only if the client is gone:\n"
							 "-- SELECT pg_terminate_backend(%d);",
							 b->pid, b->pid);
			}
			else if (strcmp(b->state, "active") == 0)
			{
				if (pin)
					horizon_setf(b->reason, sizeof(b->reason),
								 "A still-running statement pins %s (xid/xmin age %lld). Let it finish, or cancel it (SIGINT) if it is not expected to. Terminate only if it ignores cancel. Killing a healthy long query is not a vacuum fix.",
								 pin, (long long) b->age);
				else
					horizon_setf(b->reason, sizeof(b->reason),
								 "A running statement has xid/xmin age %lld but is not the oldest pin, so it is not what limits VACUUM right now.",
								 (long long) b->age);
				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT pid, usename, state, xact_start, left(query, 80) AS query FROM pg_stat_activity WHERE pid = %d;\n"
							 "-- Cancel only if the statement is not expected to finish:\n"
							 "-- SELECT pg_cancel_backend(%d);",
							 b->pid, b->pid);
			}
			else
			{
				horizon_setf(b->reason, sizeof(b->reason),
							 "Backend in state '%s' holds xid/xmin age %lld%s.",
							 b->state[0] ? b->state : "unknown", (long long) b->age,
							 pin ? " and pins a horizon" : " but is not the oldest pin");
				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT pid, usename, state, xact_start, left(query, 80) AS query FROM pg_stat_activity WHERE pid = %d;",
							 b->pid);
			}
			break;

		case HORIZON_BLK_PREPARED:
			horizon_setf(b->reason, sizeof(b->reason),
						 "Prepared transaction xid %u (gid %s) stays in the ProcArray until COMMIT PREPARED or ROLLBACK PREPARED%s. VACUUM cannot advance past it. Raising autovacuum_freeze_max_age does not fix this.",
						 b->xid,
						 b->prepared_gid[0] ? b->prepared_gid : "(unknown)",
						 pin ? "" : " (not the oldest pin right now)");
			if (b->prepared_gid[0])
			{
				char	   *lit = quote_literal_cstr(b->prepared_gid);

				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT gid, prepared, owner, database FROM pg_prepared_xacts WHERE gid = %s;\n"
							 "-- Ask the transaction coordinator first; the outcome may already be decided elsewhere.\n"
							 "-- COMMIT PREPARED %s;\n"
							 "-- ROLLBACK PREPARED %s;",
							 lit, lit, lit);
			}
			else
				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT gid, transaction, prepared, owner, database FROM pg_prepared_xacts WHERE transaction = '%u'::xid;",
							 b->xid);
			break;

		case HORIZON_BLK_PHYSICAL_SLOT:
			{
				char	   *lit = quote_literal_cstr(b->slot_name);

				horizon_setf(b->reason, sizeof(b->reason),
							 "Physical replication slot %s holds xmin age %lld%s. If a replica still uses this slot, dropping it is wrong: fix the replica. If the replica is gone, the slot is an orphan and should be dropped.",
							 b->slot_name, (long long) b->age,
							 pin ? " and pins a horizon" : " (not the oldest pin right now)");
				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT slot_name, active, restart_lsn, xmin, catalog_xmin FROM pg_replication_slots WHERE slot_name = %s;\n"
							 "-- Only if the replica is decommissioned:\n"
							 "-- SELECT pg_drop_replication_slot(%s);",
							 lit, lit);
			}
			break;

		case HORIZON_BLK_LOGICAL_SLOT:
			{
				char	   *lit = quote_literal_cstr(b->slot_name);

				horizon_setf(b->reason, sizeof(b->reason),
							 "Logical slot %s holds catalog_xmin age %lld%s, so catalog rows needed for decoding are not removed or frozen. Consume changes to advance it. Drop the slot only if the consumer is gone. Do not VACUUM FULL.",
							 b->slot_name,
							 (long long) (b->catalog_age >= 0 ? b->catalog_age : b->age),
							 pin ? " and pins a horizon" : " (not the oldest pin right now)");
				horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
							 "SELECT slot_name, active, restart_lsn, confirmed_flush_lsn, xmin, catalog_xmin, plugin FROM pg_replication_slots WHERE slot_name = %s;\n"
							 "-- Advance it by consuming changes, or drop it only if the consumer is decommissioned:\n"
							 "-- SELECT pg_drop_replication_slot(%s);",
							 lit, lit);
			}
			break;

		case HORIZON_BLK_STANDBY_FEEDBACK:
			horizon_setf(b->reason, sizeof(b->reason),
						 "WAL sender pid %d applies hot_standby_feedback xmin age %lld to this primary%s. The holder is on the STANDBY (a long query, an idle-in-transaction session, or a logical slot catalog_xmin). Inspect the standby; do not drop the physical slot of a live replica.",
						 b->pid, (long long) b->age,
						 pin ? "" : " (not the oldest pin right now)");
			horizon_setf(b->recommended_sql, sizeof(b->recommended_sql),
						 "-- On the STANDBY, not this primary:\n"
						 "SELECT * FROM pg_horizon_blockers;\n"
						 "-- A logical slot on the standby can pin catalog_xmin, which hot_standby_feedback then publishes as xmin.");
			break;
	}

	if (b->holds_waited_lock && b->nwaiters > 0)
	{
		size_t		len = strlen(b->reason);

		if (len < sizeof(b->reason) - 1)
			horizon_setf(b->reason + len, sizeof(b->reason) - len,
						 " This session also holds locks that %d other backend(s) are waiting for.",
						 b->nwaiters);
	}

	(void) snap;
}

void
_PG_init(void)
{
	DefineCustomIntVariable("pg_horizon.min_xmin_age",
							"Hide holders whose xmin/xid age is below this XID count.",
							"0 shows every holder. Raise it to ignore noise from short snapshots. Applies to pg_horizon_blockers and to the counts derived from it.",
							&pg_horizon_min_xmin_age,
							0,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_horizon.terminate_blockers",
							 "Allow pg_horizon_terminate_blocker() to signal backends.",
							 "Default off. Enabling it does not drop slots or roll back prepared transactions.",
							 &pg_horizon_terminate_blockers,
							 false,
							 PGC_SUSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_horizon");
}

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

#include "catalog/pg_authid.h"
#include "commands/vacuum.h"
#include "fmgr.h"
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

int64
horizon_blocker_age(const HorizonBlocker *b, TransactionId next_xid)
{
	TransactionId hold;

	hold = TransactionIdOlder(b->xmin, b->xid);
	hold = TransactionIdOlder(hold, b->catalog_xmin);
	return horizon_xid_age(hold, next_xid);
}

HorizonSeverity
horizon_compute_severity(const HorizonSnapshot *snap)
{
	int64		headroom;
	int			i;

	headroom = horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit);

	/*
	 * Use the same cutoffs the core XID generator uses in SetTransactionIdLimit
	 * / GetNewTransactionId — not invented percentages.
	 *
	 * Emergency must be decided before warn→critical: headroom < 10M is
	 * already past xid_warn_limit, so checking warn first made the 10M
	 * emergency path dead.
	 */
	if (TransactionIdIsValid(snap->xid_stop_limit) &&
		TransactionIdFollowsOrEquals(snap->next_xid, snap->xid_stop_limit))
		return HORIZON_SEV_EMERGENCY;

	if (headroom > 0 && headroom < 10000000)
		return HORIZON_SEV_EMERGENCY;

	if (TransactionIdIsValid(snap->xid_warn_limit) &&
		TransactionIdFollowsOrEquals(snap->next_xid, snap->xid_warn_limit))
		return HORIZON_SEV_CRITICAL;

	if (TransactionIdIsValid(snap->xid_vac_limit) &&
		TransactionIdFollowsOrEquals(snap->next_xid, snap->xid_vac_limit))
		return HORIZON_SEV_WARNING;

	if (vacuum_failsafe_age > 0 &&
		horizon_xid_age(snap->oldest_xid, snap->next_xid) >= vacuum_failsafe_age)
		return HORIZON_SEV_CRITICAL;

	if (autovacuum_multixact_freeze_max_age > 0 &&
		horizon_xid_age(snap->oldest_mxid, snap->next_mxid) >=
		(int64) autovacuum_multixact_freeze_max_age)
		return HORIZON_SEV_WARNING;

	for (i = 0; i < snap->nblockers; i++)
	{
		const HorizonBlocker *b = &snap->blockers[i];

		if (!b->is_horizon_holder || b->is_observer)
			continue;

		if (b->type == HORIZON_BLK_BACKEND &&
			(strcmp(b->state, "idle in transaction") == 0 ||
			 strcmp(b->state, "idle in transaction (aborted)") == 0) &&
			horizon_xid_age(TransactionIdOlder(b->xmin, b->xid),
							snap->next_xid) >= 1000000)
			return HORIZON_SEV_WARNING;

		if ((b->type == HORIZON_BLK_PHYSICAL_SLOT ||
			 b->type == HORIZON_BLK_LOGICAL_SLOT) &&
			!b->slot_active &&
			horizon_xid_age(TransactionIdOlder(b->xmin, b->catalog_xmin),
							snap->next_xid) >= 1000000)
			return HORIZON_SEV_WARNING;
	}

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
	if (strcmp(b->state, "idle in transaction") == 0 ||
		strcmp(b->state, "idle in transaction (aborted)") == 0)
		return true;
	if (force && strcmp(b->state, "active") == 0)
		return true;
	return false;
}

void
horizon_fill_blocker_reason(HorizonBlocker *b, const HorizonSnapshot *snap)
{
	int64		age;
	TransactionId hold;

	hold = TransactionIdOlder(b->xmin, b->xid);
	if (b->type == HORIZON_BLK_LOGICAL_SLOT)
		hold = TransactionIdOlder(hold, b->catalog_xmin);
	age = horizon_xid_age(hold, snap->next_xid);

	switch (b->type)
	{
		case HORIZON_BLK_BACKEND:
			if (strcmp(b->state, "idle in transaction") == 0 ||
				strcmp(b->state, "idle in transaction (aborted)") == 0)
			{
				snprintf(b->reason, sizeof(b->reason),
						 "Session is idle in transaction with xmin age %lld. VACUUM cannot freeze or remove dead tuples newer than this snapshot. The real fix is to COMMIT/ROLLBACK the abandoned transaction, or terminate the backend if the client is gone.",
						 (long long) age);
				snprintf(b->recommended_sql, sizeof(b->recommended_sql),
						 "-- Prefer COMMIT or ROLLBACK in that session.\nSELECT pg_terminate_backend(%d); -- only if the client is gone",
						 b->pid);
			}
			else if (strcmp(b->state, "active") == 0)
			{
				snprintf(b->reason, sizeof(b->reason),
						 "A still-running statement holds xmin age %lld. Cancel the statement first (SIGINT). Terminate only if it ignores cancel. Killing a healthy long query is not a vacuum fix.",
						 (long long) age);
				snprintf(b->recommended_sql, sizeof(b->recommended_sql),
						 "SELECT pg_cancel_backend(%d);", b->pid);
			}
			else
			{
				snprintf(b->reason, sizeof(b->reason),
						 "Backend holds xmin age %lld in state '%s'.",
						 (long long) age, b->state);
				snprintf(b->recommended_sql, sizeof(b->recommended_sql),
						 "SELECT pg_cancel_backend(%d);", b->pid);
			}
			break;

		case HORIZON_BLK_PREPARED:
			snprintf(b->reason, sizeof(b->reason),
					 "Prepared transaction xid %u (gid %s) stays in the ProcArray until COMMIT PREPARED or ROLLBACK PREPARED. VACUUM cannot advance past it. This is not fixed by raising autovacuum_freeze_max_age.",
					 b->xid,
					 b->prepared_gid[0] ? b->prepared_gid : "(unknown)");
			if (b->prepared_gid[0])
				snprintf(b->recommended_sql, sizeof(b->recommended_sql),
						 "ROLLBACK PREPARED %s;",
						 quote_literal_cstr(b->prepared_gid));
			else
				snprintf(b->recommended_sql, sizeof(b->recommended_sql),
						 "SELECT gid, transaction, prepared, ownerid, database FROM pg_prepared_xacts WHERE transaction = '%u'::xid;",
						 b->xid);
			break;

		case HORIZON_BLK_PHYSICAL_SLOT:
			snprintf(b->reason, sizeof(b->reason),
					 "Physical replication slot %s holds xmin age %lld and restart_lsn. If a replica still uses this slot, drop is wrong — fix the replica. If the replica is gone, the slot is an orphan and must be dropped.",
					 b->slot_name, (long long) age);
			snprintf(b->recommended_sql, sizeof(b->recommended_sql),
					 "SELECT slot_name, active, restart_lsn, xmin, catalog_xmin FROM pg_replication_slots WHERE slot_name = %s;\n-- Only if the replica is decommissioned:\n-- SELECT pg_drop_replication_slot(%s);",
					 quote_literal_cstr(b->slot_name),
					 quote_literal_cstr(b->slot_name));
			break;

		case HORIZON_BLK_LOGICAL_SLOT:
			snprintf(b->reason, sizeof(b->reason),
					 "Logical slot %s holds catalog_xmin age %lld so catalog rows needed for decoding are not frozen. Consume changes to advance confirmed_flush / catalog_xmin. Drop the slot only if the consumer is gone. Do not VACUUM FULL.",
					 b->slot_name,
					 (long long) horizon_xid_age(b->catalog_xmin, snap->next_xid));
			snprintf(b->recommended_sql, sizeof(b->recommended_sql),
					 "SELECT slot_name, active, restart_lsn, confirmed_flush, xmin, catalog_xmin, plugin FROM pg_replication_slots WHERE slot_name = %s;\n-- Advance by consuming, or drop only if the consumer is decommissioned:\n-- SELECT pg_drop_replication_slot(%s);",
					 quote_literal_cstr(b->slot_name),
					 quote_literal_cstr(b->slot_name));
			break;

		case HORIZON_BLK_STANDBY_FEEDBACK:
			snprintf(b->reason, sizeof(b->reason),
					 "WAL sender pid %d applies hot_standby_feedback xmin age %lld to this primary. The holder is on the STANDBY (long query, idle-in-transaction, or a logical slot catalog_xmin). Inspect the standby; do not drop the physical slot of a live replica.",
					 b->pid, (long long) age);
			snprintf(b->recommended_sql, sizeof(b->recommended_sql),
					 "-- On the STANDBY, not this primary:\nSELECT * FROM pg_horizon_blockers();\n-- A logical slot on the standby can pin catalog_xmin which hot_standby_feedback then publishes as xmin.");
			break;
	}

	if (b->holds_waited_lock && b->nwaiters > 0)
	{
		size_t		len = strlen(b->reason);

		snprintf(b->reason + len, sizeof(b->reason) - len,
				 " This session also holds locks waited on by %d other backend(s).",
				 b->nwaiters);
	}
}

void
_PG_init(void)
{
	DefineCustomIntVariable("pg_horizon.min_xmin_age",
							"Hide blockers whose xmin/xid age is below this XID count.",
							"0 shows every holder. Raise it to ignore noise from short snapshots.",
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

#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("pg_horizon");
#endif
}

/*-------------------------------------------------------------------------
 *
 * horizon_collect.c
 *	  Snapshot of XID horizons and whoever is holding them
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 * IDENTIFICATION
 *	  horizon_collect.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/multixact.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_proc.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "replication/slot.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/wait_event.h"

#include "pg_horizon.h"

#include "catalog/pg_authid.h"

#if PG_VERSION_NUM < 170000
#define HORIZON_XID_CACHE		ShmemVariableCache
#else
#define HORIZON_XID_CACHE		TransamVariables
#endif

typedef struct HorizonAct
{
	int			pid;
	BackendType backend_type;
	BackendState state;
	Oid			userid;
	Oid			databaseid;
	TimestampTz xact_start;
	TimestampTz proc_start;
	char		appname[NAMEDATALEN];
	char		query[HORIZON_QUERY_CHARS];
} HorizonAct;

static TransactionId
horizon_shared_xmin(void)
{
	return GetOldestNonRemovableTransactionId(NULL);
}

static TransactionId
horizon_xmin_for_rel(Relation rel)
{
	return GetOldestNonRemovableTransactionId(rel);
}

static const char *
horizon_backend_state_name(BackendState state)
{
	switch (state)
	{
		case STATE_IDLE:
			return "idle";
		case STATE_RUNNING:
			return "active";
		case STATE_IDLEINTRANSACTION:
			return "idle in transaction";
		case STATE_FASTPATH:
			return "fastpath function call";
		case STATE_IDLEINTRANSACTION_ABORTED:
			return "idle in transaction (aborted)";
		case STATE_DISABLED:
			return "disabled";
		default:
			return "unknown";
	}
}

static HorizonBlocker *
horizon_add_blocker(HorizonSnapshot *snap)
{
	HorizonBlocker *b;

	if (snap->nblockers >= snap->maxblockers)
		elog(ERROR, "pg_horizon blocker array overflow");

	b = &snap->blockers[snap->nblockers++];
	memset(b, 0, sizeof(*b));
	return b;
}

static void
horizon_mark_holders(HorizonSnapshot *snap)
{
	int			i;

	for (i = 0; i < snap->nblockers; i++)
	{
		HorizonBlocker *b = &snap->blockers[i];
		TransactionId hold = TransactionIdOlder(b->xmin, b->xid);

		if (TransactionIdIsValid(hold) &&
			(TransactionIdEquals(hold, snap->data_xmin) ||
			 TransactionIdEquals(hold, snap->shared_xmin) ||
			 TransactionIdEquals(hold, snap->oldest_running_xid)))
			b->is_horizon_holder = true;

		if (TransactionIdIsValid(b->catalog_xmin) &&
			(TransactionIdEquals(b->catalog_xmin, snap->catalog_xmin) ||
			 TransactionIdEquals(b->catalog_xmin, snap->slot_catalog_xmin)))
			b->is_horizon_holder = true;

		if (TransactionIdIsValid(hold) &&
			TransactionIdEquals(hold, snap->slot_xmin))
			b->is_horizon_holder = true;
	}
}

static void
horizon_attach_locks(HorizonSnapshot *snap)
{
	LockData   *lockData;
	int			i,
				j;

	lockData = GetLockStatusData();
	if (lockData == NULL)
		return;

	for (i = 0; i < lockData->nelements; i++)
	{
		LockInstanceData *waiter = &lockData->locks[i];
		LOCKMODE	held;

		if (waiter->waitLockMode == NoLock || waiter->pid <= 0)
			continue;

		for (j = 0; j < lockData->nelements; j++)
		{
			LockInstanceData *holder = &lockData->locks[j];
			int			k;
			bool		conflicts = false;

			if (holder->pid <= 0 || holder->pid == waiter->pid)
				continue;
			if (holder->holdMask == 0)
				continue;
			if (memcmp(&holder->locktag, &waiter->locktag, sizeof(LOCKTAG)) != 0)
				continue;

			for (held = 1; held < MAX_LOCKMODES; held++)
			{
				if ((holder->holdMask & LOCKBIT_ON(held)) &&
					DoLockModesConflict(held, waiter->waitLockMode))
				{
					conflicts = true;
					break;
				}
			}
			if (!conflicts)
				continue;

			for (k = 0; k < snap->nblockers; k++)
			{
				HorizonBlocker *b = &snap->blockers[k];
				int			w;

				if (b->pid != holder->pid)
					continue;

				b->holds_waited_lock = true;
				for (w = 0; w < b->nwaiters; w++)
				{
					if (b->waiter_pids[w] == waiter->pid)
					{
						w = -1;
						break;
					}
				}
				if (w >= 0 && b->nwaiters < HORIZON_MAX_WAITERS)
					b->waiter_pids[b->nwaiters++] = waiter->pid;
			}
		}
	}

	if (lockData->locks)
		pfree(lockData->locks);
	pfree(lockData);
}

static void
horizon_load_activity(HorizonAct **acts_p, int *nacts_p)
{
	int			n;
	int			i;
	int			out = 0;
	HorizonAct *acts;

	n = pgstat_fetch_stat_numbackends();
	if (n <= 0)
	{
		*acts_p = NULL;
		*nacts_p = 0;
		return;
	}

	acts = (HorizonAct *) palloc0(sizeof(HorizonAct) * n);

	for (i = 1; i <= n; i++)
	{
		LocalPgBackendStatus *local;
		PgBackendStatus *st;
		HorizonAct *a;
		char	   *q;

		local = pgstat_get_local_beentry_by_index(i);
		if (local == NULL)
			continue;

		st = &local->backendStatus;
		if (st->st_procpid <= 0)
			continue;

		a = &acts[out++];
		a->pid = st->st_procpid;
		a->backend_type = st->st_backendType;
		a->state = st->st_state;
		a->userid = st->st_userid;
		a->databaseid = st->st_databaseid;
		a->xact_start = st->st_xact_start_timestamp;
		a->proc_start = st->st_proc_start_timestamp;

		if (st->st_appname)
			strlcpy(a->appname, st->st_appname, sizeof(a->appname));

		if (st->st_activity_raw)
		{
			q = pgstat_clip_activity(st->st_activity_raw);
			strlcpy(a->query, q, sizeof(a->query));
			pfree(q);
		}
	}

	*acts_p = acts;
	*nacts_p = out;
}

static HorizonAct *
horizon_find_activity(HorizonAct *acts, int nacts, int pid)
{
	int			i;

	for (i = 0; i < nacts; i++)
	{
		if (acts[i].pid == pid)
			return &acts[i];
	}
	return NULL;
}

static void
horizon_apply_activity(HorizonBlocker *b, HorizonAct *acts, int nacts)
{
	HorizonAct *a;

	if (b->pid <= 0)
		return;

	a = horizon_find_activity(acts, nacts, b->pid);
	if (a == NULL)
		return;

	strlcpy(b->backend_type, GetBackendTypeDesc(a->backend_type),
			sizeof(b->backend_type));
	strlcpy(b->state, horizon_backend_state_name(a->state), sizeof(b->state));
	strlcpy(b->application_name, a->appname, sizeof(b->application_name));
	b->xact_start = a->xact_start;
	b->proc_start = a->proc_start;
	if (!OidIsValid(b->roleId))
		b->roleId = a->userid;
	if (!OidIsValid(b->databaseId))
		b->databaseId = a->databaseid;

	if (horizon_has_stat_visibility(b->roleId))
		strlcpy(b->query, a->query, sizeof(b->query));
	else
		strlcpy(b->query, "<insufficient privilege>", sizeof(b->query));
}

static void
horizon_fill_names(HorizonBlocker *b)
{
	char	   *name;

	if (OidIsValid(b->databaseId))
	{
		name = get_database_name(b->databaseId);
		if (name)
			strlcpy(b->datname, name, sizeof(b->datname));
	}
	if (OidIsValid(b->roleId))
	{
		name = GetUserNameFromId(b->roleId, true);
		if (name)
			strlcpy(b->usename, name, sizeof(b->usename));
	}
}

static void
horizon_load_prepared_gids(HorizonSnapshot *snap)
{
	int			i;

	if (SPI_connect() != SPI_OK_CONNECT)
		return;

	PG_TRY();
	{
		if (SPI_execute("SELECT gid, transaction FROM pg_prepared_xacts", true, 0) == SPI_OK_SELECT &&
			SPI_processed > 0)
		{
			for (i = 0; i < (int) SPI_processed; i++)
			{
				bool		isnull;
				char	   *gid;
				TransactionId xid;
				Datum		gidatum;
				Datum		xidatum;
				int			j;

				gidatum = SPI_getbinval(SPI_tuptable->vals[i],
										SPI_tuptable->tupdesc,
										1, &isnull);
				if (isnull)
					continue;
				gid = TextDatumGetCString(gidatum);

				xidatum = SPI_getbinval(SPI_tuptable->vals[i],
										SPI_tuptable->tupdesc,
										2, &isnull);
				if (isnull)
				{
					pfree(gid);
					continue;
				}
				xid = DatumGetTransactionId(xidatum);

				for (j = 0; j < snap->nblockers; j++)
				{
					if (snap->blockers[j].type == HORIZON_BLK_PREPARED &&
						TransactionIdEquals(snap->blockers[j].xid, xid))
					{
						strlcpy(snap->blockers[j].prepared_gid, gid,
								sizeof(snap->blockers[j].prepared_gid));
					}
				}
				pfree(gid);
			}
		}

		SPI_finish();
	}
	PG_CATCH();
	{
		SPI_finish();
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
horizon_collect_limits(HorizonSnapshot *snap)
{
	LWLockAcquire(XidGenLock, LW_SHARED);
	snap->next_xid = XidFromFullTransactionId(HORIZON_XID_CACHE->nextXid);
	snap->oldest_xid = HORIZON_XID_CACHE->oldestXid;
	snap->oldest_xid_db = HORIZON_XID_CACHE->oldestXidDB;
	snap->xid_vac_limit = HORIZON_XID_CACHE->xidVacLimit;
	snap->xid_warn_limit = HORIZON_XID_CACHE->xidWarnLimit;
	snap->xid_stop_limit = HORIZON_XID_CACHE->xidStopLimit;
	snap->xid_wrap_limit = HORIZON_XID_CACHE->xidWrapLimit;
	LWLockRelease(XidGenLock);

	snap->in_recovery = RecoveryInProgress();
	snap->oldest_mxid = GetOldestMultiXactId();
	snap->next_mxid = ReadNextMultiXactId();
}

static void
horizon_collect_xmin(HorizonSnapshot *snap)
{
	Relation	rel;

	snap->shared_xmin = horizon_shared_xmin();
	snap->oldest_running_xid = GetOldestTransactionIdConsideredRunning();
	GetReplicationHorizons(&snap->slot_xmin, &snap->slot_catalog_xmin);

	/*
	 * pg_class is shared, so GetOldestNonRemovableTransactionId(pg_class)
	 * returns the shared horizon. Catalog freeze uses a non-shared catalog
	 * such as pg_proc (see GlobalVisHorizonKindForRel).
	 */
	rel = table_open(ProcedureRelationId, AccessShareLock);
	snap->catalog_xmin = horizon_xmin_for_rel(rel);
	table_close(rel, AccessShareLock);

	/*
	 * Data horizon is relation-kind specific. Use a non-shared user heap when
	 * one exists; otherwise the shared horizon is the conservative bound.
	 * Skip catalogs and catalog TOAST — those yield the catalog horizon.
	 */
	snap->data_xmin = snap->shared_xmin;
	rel = table_open(RelationRelationId, AccessShareLock);
	{
		TableScanDesc scan;
		HeapTuple	tuple;

		scan = table_beginscan_catalog(rel, 0, NULL);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);
			Relation	urel;

			if (!RELKIND_HAS_TABLE_AM(form->relkind))
				continue;
			if (form->relisshared)
				continue;
			if (IsCatalogRelationOid(form->oid) ||
				IsCatalogNamespace(form->relnamespace))
				continue;
			if (form->relpersistence == RELPERSISTENCE_TEMP)
				continue;

			/*
			 * Never wait for a user-table lock. A wraparound diagnostic that
			 * blocks behind ACCESS EXCLUSIVE is worse than a slightly
			 * conservative data_xmin.
			 */
			if (!ConditionalLockRelationOid(form->oid, AccessShareLock))
				continue;

			urel = try_table_open(form->oid, NoLock);
			if (urel)
			{
				snap->data_xmin = horizon_xmin_for_rel(urel);
				table_close(urel, AccessShareLock);
				break;
			}
			UnlockRelationOid(form->oid, AccessShareLock);
		}
		table_endscan(scan);
	}
	table_close(rel, AccessShareLock);
}

static void
horizon_collect_backends(HorizonSnapshot *snap)
{
	uint32		i;
	uint32		nprocs;

	nprocs = ProcGlobal->allProcCount;

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (i = 0; i < nprocs; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];
		HorizonBlocker *b;
		TransactionId xid;
		TransactionId xmin;
		const char *we;
		const char *wet;

		xid = UINT32_ACCESS_ONCE(proc->xid);
		xmin = UINT32_ACCESS_ONCE(proc->xmin);

		if (proc->pid <= 0 && !TransactionIdIsValid(xid) && !TransactionIdIsValid(xmin))
			continue;

		/* Dummy prepared xacts live in PreparedXactProcs, not here. */
		if (proc->pid <= 0)
			continue;

		if (!TransactionIdIsValid(xid) && !TransactionIdIsValid(xmin))
			continue;

		/* Same skip as ComputeXidHorizons(): vacuum xmin is not a pin. */
		if (proc->statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			continue;

		b = horizon_add_blocker(snap);
		b->pid = proc->pid;
		b->xid = xid;
		b->xmin = xmin;
		b->databaseId = proc->databaseId;
		b->roleId = proc->roleId;
		b->statusFlags = proc->statusFlags;
		b->wait_event_info = proc->wait_event_info;
		b->is_observer = (proc->pid == MyProcPid);
		b->affects_data = true;
		b->affects_catalog = (proc->statusFlags & PROC_AFFECTS_ALL_HORIZONS) != 0;

		if (proc->statusFlags & PROC_AFFECTS_ALL_HORIZONS)
			b->type = HORIZON_BLK_STANDBY_FEEDBACK;
		else
			b->type = HORIZON_BLK_BACKEND;

		we = pgstat_get_wait_event(proc->wait_event_info);
		wet = pgstat_get_wait_event_type(proc->wait_event_info);
		if (we)
			strlcpy(b->wait_event, we, sizeof(b->wait_event));
		if (wet)
			strlcpy(b->wait_event_type, wet, sizeof(b->wait_event_type));
	}
	LWLockRelease(ProcArrayLock);
}

static void
horizon_collect_prepared(HorizonSnapshot *snap)
{
	int			i;

	if (max_prepared_xacts <= 0 || PreparedXactProcs == NULL)
		return;

	LWLockAcquire(ProcArrayLock, LW_SHARED);
	for (i = 0; i < max_prepared_xacts; i++)
	{
		PGPROC	   *proc = &PreparedXactProcs[i];
		HorizonBlocker *b;
		TransactionId xid;
		TransactionId xmin;

		xid = UINT32_ACCESS_ONCE(proc->xid);
		xmin = UINT32_ACCESS_ONCE(proc->xmin);
		if (!TransactionIdIsValid(xid) && !TransactionIdIsValid(xmin))
			continue;

		b = horizon_add_blocker(snap);
		b->type = HORIZON_BLK_PREPARED;
		b->pid = 0;
		b->xid = xid;
		b->xmin = xmin;
		b->databaseId = proc->databaseId;
		b->roleId = proc->roleId;
		b->affects_data = true;
		b->affects_catalog = true;
		strlcpy(b->backend_type, "prepared transaction", sizeof(b->backend_type));
		strlcpy(b->state, "prepared", sizeof(b->state));
	}
	LWLockRelease(ProcArrayLock);
}

static void
horizon_collect_slots(HorizonSnapshot *snap)
{
	int			i;
	XLogRecPtr	insertPtr;

	if (max_replication_slots <= 0 || ReplicationSlotCtl == NULL)
		return;

	insertPtr = RecoveryInProgress() ? GetXLogReplayRecPtr(NULL) : GetXLogInsertRecPtr();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		HorizonBlocker *b;
		TransactionId xmin;
		TransactionId catalog_xmin;
		NameData	name;
		NameData	plugin;
		Oid			dboid;
		pid_t		active_pid;
		XLogRecPtr	restart_lsn;
		bool		logical;

		if (!slot->in_use)
			continue;

		SpinLockAcquire(&slot->mutex);
		xmin = slot->effective_xmin;
		catalog_xmin = slot->effective_catalog_xmin;
		name = slot->data.name;
		plugin = slot->data.plugin;
		dboid = slot->data.database;
		active_pid = slot->active_pid;
		restart_lsn = slot->data.restart_lsn;
		logical = SlotIsLogical(slot);
		SpinLockRelease(&slot->mutex);

		if (!TransactionIdIsValid(xmin) && !TransactionIdIsValid(catalog_xmin))
			continue;

		b = horizon_add_blocker(snap);
		b->type = logical ? HORIZON_BLK_LOGICAL_SLOT : HORIZON_BLK_PHYSICAL_SLOT;
		b->pid = active_pid;
		b->xmin = xmin;
		b->catalog_xmin = catalog_xmin;
		b->databaseId = dboid;
		b->slot_active = (active_pid > 0);
		b->slot_active_pid = active_pid;
		b->restart_lsn = restart_lsn;
		b->affects_data = TransactionIdIsValid(xmin);
		b->affects_catalog = TransactionIdIsValid(catalog_xmin);
		strlcpy(b->slot_name, NameStr(name), sizeof(b->slot_name));
		strlcpy(b->plugin, NameStr(plugin), sizeof(b->plugin));
		strlcpy(b->backend_type, logical ? "logical replication slot" :
				"physical replication slot", sizeof(b->backend_type));
		strlcpy(b->state, active_pid > 0 ? "active" : "inactive", sizeof(b->state));

		if (!XLogRecPtrIsInvalid(restart_lsn) &&
			!XLogRecPtrIsInvalid(insertPtr) &&
			insertPtr > restart_lsn)
			b->wal_retained_bytes = (int64) (insertPtr - restart_lsn);
	}
	LWLockRelease(ReplicationSlotControlLock);
}

HorizonSnapshot *
horizon_snapshot_collect(void)
{
	HorizonSnapshot *snap;
	HorizonAct *acts = NULL;
	int			nacts = 0;
	int			maxb;
	int			i;

	snap = (HorizonSnapshot *) palloc0(sizeof(HorizonSnapshot));
	maxb = (int) ProcGlobal->allProcCount + Max(max_prepared_xacts, 0) +
		Max(max_replication_slots, 0) + 8;
	snap->maxblockers = maxb;
	snap->blockers = (HorizonBlocker *) palloc0(sizeof(HorizonBlocker) * maxb);

	horizon_collect_limits(snap);
	horizon_collect_xmin(snap);
	horizon_collect_backends(snap);
	horizon_collect_prepared(snap);
	horizon_collect_slots(snap);

	horizon_load_activity(&acts, &nacts);
	for (i = 0; i < snap->nblockers; i++)
	{
		horizon_apply_activity(&snap->blockers[i], acts, nacts);
		horizon_fill_names(&snap->blockers[i]);
	}

	horizon_load_prepared_gids(snap);
	horizon_attach_locks(snap);
	horizon_mark_holders(snap);

	for (i = 0; i < snap->nblockers; i++)
		horizon_fill_blocker_reason(&snap->blockers[i], snap);

	if (acts)
		pfree(acts);

	return snap;
}

void
horizon_snapshot_free(HorizonSnapshot *snap)
{
	if (snap == NULL)
		return;
	if (snap->blockers)
		pfree(snap->blockers);
	pfree(snap);
}

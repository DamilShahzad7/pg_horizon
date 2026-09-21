/*-------------------------------------------------------------------------
 *
 * horizon_collect.c
 *	  Snapshot of XID horizons and whoever is holding them
 *
 * Horizon computation
 * -------------------
 * ComputeXidHorizons() is static in procarray.c and its public wrappers
 * (GetOldestNonRemovableTransactionId and friends) include the *calling
 * backend's own* PGPROC. That backend's snapshot xmin is inherited from any
 * running XID in any database, so the wrapper cannot answer "what would a
 * VACUUM started right now, elsewhere, see?".  We therefore copy the relevant
 * PGPROC / prepared-transaction / slot state once under ProcArrayLock and
 * apply the same rules ComputeXidHorizons() applies (PostgreSQL 16-18 share
 * this algorithm), minus the observer:
 *
 *	 initial = latestCompletedXid + 1
 *	 every proc's min(xmin, xid) counts, except PROC_IN_VACUUM and
 *	   PROC_IN_LOGICAL_DECODING procs (which only feed "oldest running")
 *	 shared  = min over all databases
 *	 data    = min over this database's procs, database-0 procs and
 *	           PROC_AFFECTS_ALL_HORIZONS procs (hot_standby_feedback)
 *	 slot xmin applies to shared and data; slot catalog_xmin to shared and
 *	   catalog, and catalog = min(data, slot catalog_xmin)
 *
 * While in recovery the per-database split does not exist (all XIDs live in
 * KnownAssignedXids, which is private to procarray.c), so the core wrapper is
 * used and its value includes the observer; see docs/internals.md.
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 * IDENTIFICATION
 *	  horizon_collect.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/slot.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/wait_event.h"

#include "pg_horizon.h"

#if PG_VERSION_NUM < 170000
#define HORIZON_XID_CACHE		ShmemVariableCache
#else
#define HORIZON_XID_CACHE		TransamVariables
#endif

/*
 * Same definition procarray.c uses; it is not in a public header.
 */
#ifndef UINT32_ACCESS_ONCE
#define UINT32_ACCESS_ONCE(var) ((uint32) (*((volatile uint32 *) &(var))))
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

/* What we copy out of PGPROC while ProcArrayLock is held. */
typedef struct HorizonProcCopy
{
	int			pid;			/* 0 for prepared-transaction dummy procs */
	bool		prepared;
	TransactionId xid;
	TransactionId xmin;
	Oid			databaseId;
	Oid			roleId;
	uint8		statusFlags;
	uint32		wait_event_info;
	char	   *gid;			/* prepared transactions only; palloc'd */
} HorizonProcCopy;

/* What we copy out of a replication slot while its mutex is held. */
typedef struct HorizonSlotCopy
{
	TransactionId xmin;
	TransactionId catalog_xmin;
	NameData	name;
	NameData	plugin;
	Oid			dboid;
	pid_t		active_pid;
	XLogRecPtr	restart_lsn;
	bool		logical;
} HorizonSlotCopy;

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
	{
		snap->maxblockers *= 2;
		snap->blockers = (HorizonBlocker **)
			repalloc(snap->blockers, sizeof(HorizonBlocker *) * snap->maxblockers);
	}

	b = (HorizonBlocker *) palloc0(sizeof(HorizonBlocker));
	snap->blockers[snap->nblockers++] = b;
	return b;
}

/* ------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------ */

static void
horizon_collect_limits(HorizonSnapshot *snap)
{
	FullTransactionId next;

	LWLockAcquire(XidGenLock, LW_SHARED);
	next = HORIZON_XID_CACHE->nextXid;
	snap->oldest_xid = HORIZON_XID_CACHE->oldestXid;
	snap->oldest_xid_db = HORIZON_XID_CACHE->oldestXidDB;
	snap->xid_vac_limit = HORIZON_XID_CACHE->xidVacLimit;
	snap->xid_warn_limit = HORIZON_XID_CACHE->xidWarnLimit;
	snap->xid_stop_limit = HORIZON_XID_CACHE->xidStopLimit;
	snap->xid_wrap_limit = HORIZON_XID_CACHE->xidWrapLimit;
	LWLockRelease(XidGenLock);

	snap->next_xid = XidFromFullTransactionId(next);
	snap->next_xid_epoch = EpochFromFullTransactionId(next);

	/*
	 * The MultiXact wraparound frontier is MultiXactState->oldestMultiXactId,
	 * i.e. min(datminmxid) as set by SetMultiXactIdLimit(). GetOldestMultiXactId()
	 * is a different thing (the oldest MultiXact still *in use*, which VACUUM
	 * uses as its cutoff) and is collected with the horizons.
	 */
	ReadMultiXactIdRange(&snap->oldest_mxid, &snap->next_mxid);
}

/* ------------------------------------------------------------------------
 * Horizons
 * ------------------------------------------------------------------------ */

/*
 * Copy every proc that has an xid or xmin, plus prepared-transaction dummy
 * procs, plus the replication-slot aggregate, all under one ProcArrayLock hold.
 * Nothing is allocated while the lock is held.
 */
static int
horizon_scan_procs(HorizonSnapshot *snap, HorizonProcCopy *copies,
				   TransactionId *initial)
{
	uint32		nprocs = ProcGlobal->allProcCount;
	int			n = 0;
	uint32		i;
	FullTransactionId latest;

	LWLockAcquire(ProcArrayLock, LW_SHARED);

	latest = HORIZON_XID_CACHE->latestCompletedXid;

	/* Shared-acquire of a lock we already hold is fine; this keeps it atomic. */
	ProcArrayGetReplicationSlotXmin(&snap->slot_xmin, &snap->slot_catalog_xmin);

	for (i = 0; i < nprocs; i++)
	{
		PGPROC	   *proc = &ProcGlobal->allProcs[i];
		TransactionId xid;
		TransactionId xmin;

		/* prepared-transaction dummy procs have pid 0 and are read below */
		if (proc->pid <= 0)
			continue;

		xid = UINT32_ACCESS_ONCE(proc->xid);
		xmin = UINT32_ACCESS_ONCE(proc->xmin);
		if (!TransactionIdIsValid(xid) && !TransactionIdIsValid(xmin))
			continue;

		copies[n].pid = proc->pid;
		copies[n].prepared = false;
		copies[n].xid = xid;
		copies[n].xmin = xmin;
		copies[n].databaseId = proc->databaseId;
		copies[n].roleId = proc->roleId;
		copies[n].statusFlags = proc->statusFlags;
		copies[n].wait_event_info = proc->wait_event_info;
		copies[n].gid = NULL;
		n++;
	}

	if (max_prepared_xacts > 0 && PreparedXactProcs != NULL)
	{
		for (i = 0; i < (uint32) max_prepared_xacts; i++)
		{
			PGPROC	   *proc = &PreparedXactProcs[i];
			TransactionId xid = UINT32_ACCESS_ONCE(proc->xid);
			TransactionId xmin = UINT32_ACCESS_ONCE(proc->xmin);

			if (!TransactionIdIsValid(xid) && !TransactionIdIsValid(xmin))
				continue;

			copies[n].pid = 0;
			copies[n].prepared = true;
			copies[n].xid = xid;
			copies[n].xmin = xmin;
			copies[n].databaseId = proc->databaseId;
			copies[n].roleId = proc->roleId;
			copies[n].statusFlags = 0;
			copies[n].wait_event_info = 0;
			copies[n].gid = NULL;
			n++;
		}
	}

	LWLockRelease(ProcArrayLock);

	*initial = XidFromFullTransactionId(latest);
	TransactionIdAdvance(*initial);
	return n;
}

/*
 * A prepared transaction's dummy PGPROC keeps its xid after COMMIT/RESTORE/
 * ROLLBACK PREPARED; only the (opaque) GlobalTransaction's "valid" flag says
 * whether the entry is live. Left alone, every finished prepared transaction
 * would show up as a phantom holder pinning the horizon. pg_prepared_xacts
 * is the public view of exactly the valid entries, so ask it, and fill in the
 * GIDs while we are at it. The name is schema-qualified so a caller's
 * search_path cannot substitute another relation.
 */
static int
horizon_filter_prepared(HorizonProcCopy *copies, int n)
{
	int			i;
	int			out = 0;
	bool		any = false;
	MemoryContext callercxt = CurrentMemoryContext;

	for (i = 0; i < n; i++)
	{
		if (copies[i].prepared)
		{
			any = true;
			break;
		}
	}
	if (!any)
		return n;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "pg_horizon: SPI_connect failed");

	if (SPI_execute("SELECT transaction, gid FROM pg_catalog.pg_prepared_xacts",
					true, 0) != SPI_OK_SELECT)
		elog(ERROR, "pg_horizon: could not read pg_prepared_xacts");

	for (i = 0; i < n; i++)
	{
		bool		keep = true;

		if (copies[i].prepared)
		{
			uint64		r;

			keep = false;
			for (r = 0; r < SPI_processed; r++)
			{
				bool		isnull;
				Datum		xd;
				Datum		gd;

				xd = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc,
								   1, &isnull);
				if (isnull || !TransactionIdEquals(DatumGetTransactionId(xd),
												   copies[i].xid))
					continue;

				gd = SPI_getbinval(SPI_tuptable->vals[r], SPI_tuptable->tupdesc,
								   2, &isnull);
				if (!isnull)
				{
					/* SPI_finish() frees its own context; keep the GID in ours */
					copies[i].gid = MemoryContextStrdup(callercxt,
														TextDatumGetCString(gd));
				}
				keep = true;
				break;
			}
		}

		if (keep)
		{
			if (out != i)
				copies[out] = copies[i];
			out++;
		}
	}

	SPI_finish();
	return out;
}

static bool
horizon_proc_is_observer(const HorizonProcCopy *c)
{
	return !c->prepared && c->pid == MyProcPid;
}

/* Does this proc count towards the data/catalog horizon of our database? */
static bool
horizon_proc_in_scope(const HorizonSnapshot *snap, Oid databaseId, uint8 statusFlags)
{
	return snap->in_recovery ||
		databaseId == MyDatabaseId ||
		MyDatabaseId == InvalidOid ||
		(statusFlags & PROC_AFFECTS_ALL_HORIZONS) != 0;
}

static void
horizon_compute_horizons(HorizonSnapshot *snap, const HorizonProcCopy *copies,
						 int ncopies, TransactionId initial)
{
	TransactionId running = initial;
	TransactionId shared = initial;
	TransactionId data = initial;
	int			i;

	if (snap->in_recovery)
	{
		/* See the file header: no per-database split is possible here. */
		snap->shared_xmin = GetOldestNonRemovableTransactionId(NULL);
		snap->catalog_xmin = snap->shared_xmin;
		snap->data_xmin = snap->shared_xmin;
		snap->temp_xmin = snap->shared_xmin;
		snap->oldest_running_xid = GetOldestTransactionIdConsideredRunning();
		return;
	}

	for (i = 0; i < ncopies; i++)
	{
		const HorizonProcCopy *c = &copies[i];
		TransactionId x;

		if (horizon_proc_is_observer(c))
			continue;

		x = TransactionIdOlder(c->xmin, c->xid);
		if (!TransactionIdIsValid(x))
			continue;

		running = TransactionIdOlder(running, x);

		if (c->statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			continue;

		shared = TransactionIdOlder(shared, x);
		if (horizon_proc_in_scope(snap, c->databaseId, c->statusFlags))
			data = TransactionIdOlder(data, x);
	}

	shared = TransactionIdOlder(shared, snap->slot_xmin);
	data = TransactionIdOlder(data, snap->slot_xmin);
	shared = TransactionIdOlder(shared, snap->slot_catalog_xmin);

	snap->shared_xmin = shared;
	snap->data_xmin = data;
	snap->catalog_xmin = TransactionIdOlder(data, snap->slot_catalog_xmin);

	running = TransactionIdOlder(running, snap->shared_xmin);
	running = TransactionIdOlder(running, snap->catalog_xmin);
	running = TransactionIdOlder(running, snap->data_xmin);
	snap->oldest_running_xid = running;

	/* Only this backend's own changes matter for its temp tables. */
	snap->temp_xmin = TransactionIdIsValid(MyProc->xid) ? MyProc->xid : initial;
}

static void
horizon_collect_horizons(HorizonSnapshot *snap, HorizonProcCopy *copies,
						 int *ncopies)
{
	TransactionId initial;

	*ncopies = horizon_scan_procs(snap, copies, &initial);
	*ncopies = horizon_filter_prepared(copies, *ncopies);
	horizon_compute_horizons(snap, copies, *ncopies, initial);

	/* What VACUUM uses as OldestMxact. */
	snap->mxid_horizon = GetOldestMultiXactId();
}

/* ------------------------------------------------------------------------
 * Blocker rows
 * ------------------------------------------------------------------------ */

static void
horizon_finish_blocker(HorizonSnapshot *snap, HorizonBlocker *b)
{
	TransactionId hold = TransactionIdOlder(b->xmin, b->xid);

	b->is_horizon_holder = b->holds_shared || b->holds_catalog || b->holds_data;
	b->xmin_age = TransactionIdIsValid(hold) ? horizon_xid_age(hold, snap->next_xid) : -1;
	b->catalog_age = TransactionIdIsValid(b->catalog_xmin) ?
		horizon_xid_age(b->catalog_xmin, snap->next_xid) : -1;
	b->age = Max(Max(b->xmin_age, b->catalog_age), 0);
}

/* A backend or prepared-transaction row: does its xid/xmin pin a horizon? */
static void
horizon_mark_proc_holder(HorizonSnapshot *snap, HorizonBlocker *b, bool in_scope)
{
	TransactionId hold = TransactionIdOlder(b->xmin, b->xid);

	b->affects_data = in_scope;
	b->affects_catalog = in_scope;

	if (b->is_observer || !TransactionIdIsValid(hold))
		return;

	b->holds_shared = TransactionIdEquals(hold, snap->shared_xmin);
	if (in_scope)
	{
		b->holds_data = TransactionIdEquals(hold, snap->data_xmin);
		b->holds_catalog = TransactionIdEquals(hold, snap->catalog_xmin);
	}
}

static void
horizon_mark_slot_holder(HorizonSnapshot *snap, HorizonBlocker *b)
{
	if (TransactionIdIsValid(b->xmin))
	{
		b->holds_shared |= TransactionIdEquals(b->xmin, snap->shared_xmin);
		b->holds_data |= TransactionIdEquals(b->xmin, snap->data_xmin);
		b->holds_catalog |= TransactionIdEquals(b->xmin, snap->catalog_xmin);
	}
	if (TransactionIdIsValid(b->catalog_xmin))
	{
		b->holds_shared |= TransactionIdEquals(b->catalog_xmin, snap->shared_xmin);
		b->holds_catalog |= TransactionIdEquals(b->catalog_xmin, snap->catalog_xmin);
	}
}

static void
horizon_build_proc_blockers(HorizonSnapshot *snap, const HorizonProcCopy *copies,
							int ncopies)
{
	int			i;

	for (i = 0; i < ncopies; i++)
	{
		const HorizonProcCopy *c = &copies[i];
		HorizonBlocker *b;
		const char *we;
		const char *wet;

		/* Same skip as ComputeXidHorizons(): a vacuum's xmin is not a pin. */
		if (c->statusFlags & (PROC_IN_VACUUM | PROC_IN_LOGICAL_DECODING))
			continue;

		b = horizon_add_blocker(snap);
		b->pid = c->pid;
		b->xid = c->xid;
		b->xmin = c->xmin;
		b->databaseId = c->databaseId;
		b->roleId = c->roleId;
		b->statusFlags = c->statusFlags;
		b->wait_event_info = c->wait_event_info;
		b->is_observer = horizon_proc_is_observer(c);

		if (c->prepared)
		{
			b->type = HORIZON_BLK_PREPARED;
			horizon_copy_clip(b->backend_type, sizeof(b->backend_type),
							  "prepared transaction");
			horizon_copy_clip(b->state, sizeof(b->state), "prepared");
			horizon_copy_clip(b->prepared_gid, sizeof(b->prepared_gid), c->gid);
			b->visible = true;	/* pg_prepared_xacts is a public view */
		}
		else
		{
			b->type = (c->statusFlags & PROC_AFFECTS_ALL_HORIZONS) ?
				HORIZON_BLK_STANDBY_FEEDBACK : HORIZON_BLK_BACKEND;
			b->visible = horizon_has_stat_visibility(c->roleId);

			we = pgstat_get_wait_event(c->wait_event_info);
			wet = pgstat_get_wait_event_type(c->wait_event_info);
			if (we)
				horizon_copy_clip(b->wait_event, sizeof(b->wait_event), we);
			if (wet)
				horizon_copy_clip(b->wait_event_type, sizeof(b->wait_event_type), wet);
		}

		horizon_mark_proc_holder(snap, b,
								 horizon_proc_in_scope(snap, c->databaseId,
													   c->statusFlags));
		horizon_finish_blocker(snap, b);
	}
}

static void
horizon_collect_slots(HorizonSnapshot *snap)
{
	HorizonSlotCopy *copies;
	int			ncopies = 0;
	int			i;
	XLogRecPtr	insertPtr;

	if (max_replication_slots <= 0 || ReplicationSlotCtl == NULL)
		return;

	copies = (HorizonSlotCopy *) palloc(sizeof(HorizonSlotCopy) * max_replication_slots);
	insertPtr = snap->in_recovery ? GetXLogReplayRecPtr(NULL) : GetXLogInsertRecPtr();

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];
		HorizonSlotCopy *c = &copies[ncopies];
		bool		invalidated;

		if (!slot->in_use)
			continue;

		SpinLockAcquire(&slot->mutex);
		c->xmin = slot->effective_xmin;
		c->catalog_xmin = slot->effective_catalog_xmin;
		c->name = slot->data.name;
		c->plugin = slot->data.plugin;
		c->dboid = slot->data.database;
		c->active_pid = slot->active_pid;
		c->restart_lsn = slot->data.restart_lsn;
		c->logical = SlotIsLogical(slot);
		invalidated = slot->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&slot->mutex);

		/* Core ignores invalidated slots when aggregating xmin; so do we. */
		if (invalidated)
			continue;
		if (!TransactionIdIsValid(c->xmin) && !TransactionIdIsValid(c->catalog_xmin))
			continue;

		ncopies++;
	}
	LWLockRelease(ReplicationSlotControlLock);

	for (i = 0; i < ncopies; i++)
	{
		HorizonSlotCopy *c = &copies[i];
		HorizonBlocker *b = horizon_add_blocker(snap);

		b->type = c->logical ? HORIZON_BLK_LOGICAL_SLOT : HORIZON_BLK_PHYSICAL_SLOT;
		b->pid = c->active_pid;
		b->xmin = c->xmin;
		b->catalog_xmin = c->catalog_xmin;
		b->databaseId = c->dboid;
		b->visible = true;		/* pg_replication_slots is a public view */
		b->slot_active = (c->active_pid > 0);
		b->slot_active_pid = c->active_pid;
		b->restart_lsn = c->restart_lsn;
		b->affects_data = TransactionIdIsValid(c->xmin);
		b->affects_catalog = b->affects_data || TransactionIdIsValid(c->catalog_xmin);
		horizon_copy_clip(b->slot_name, sizeof(b->slot_name), NameStr(c->name));
		horizon_copy_clip(b->plugin, sizeof(b->plugin), NameStr(c->plugin));
		horizon_copy_clip(b->backend_type, sizeof(b->backend_type),
						  c->logical ? "logical replication slot" :
						  "physical replication slot");
		horizon_copy_clip(b->state, sizeof(b->state),
						  c->active_pid > 0 ? "active" : "inactive");

		if (!XLogRecPtrIsInvalid(c->restart_lsn) &&
			!XLogRecPtrIsInvalid(insertPtr) &&
			insertPtr > c->restart_lsn)
			b->wal_retained_bytes = (int64) (insertPtr - c->restart_lsn);

		horizon_mark_slot_holder(snap, b);
		horizon_finish_blocker(snap, b);
	}

	pfree(copies);
}

/* ------------------------------------------------------------------------
 * Activity, names, prepared gids, lock waiters
 * ------------------------------------------------------------------------ */

static void
horizon_load_activity(HorizonAct **acts_p, int *nacts_p)
{
	int			n;
	int			i;
	int			out = 0;
	HorizonAct *acts;

	/*
	 * pg_stat_activity-style data is snapshotted once per transaction. The xmin
	 * values we read are live, so a cached activity snapshot would pair them
	 * with stale state, and pg_horizon_terminate_blocker() could act on a
	 * session that is no longer idle. Always start from a fresh read.
	 */
	pgstat_clear_backend_activity_snapshot();

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
			horizon_copy_clip(a->appname, sizeof(a->appname), st->st_appname);

		if (st->st_activity_raw)
		{
			char	   *q = pgstat_clip_activity(st->st_activity_raw);

			horizon_copy_clip(a->query, sizeof(a->query), q);
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

	/* Slots and prepared transactions carry their own type and state. */
	if (b->type != HORIZON_BLK_BACKEND && b->type != HORIZON_BLK_STANDBY_FEEDBACK)
		return;
	if (b->pid <= 0)
		return;

	a = horizon_find_activity(acts, nacts, b->pid);
	if (a == NULL)
		return;

	horizon_copy_clip(b->backend_type, sizeof(b->backend_type),
					  GetBackendTypeDesc(a->backend_type));
	horizon_copy_clip(b->state, sizeof(b->state), horizon_backend_state_name(a->state));
	horizon_copy_clip(b->application_name, sizeof(b->application_name), a->appname);
	b->xact_start = a->xact_start;
	b->proc_start = a->proc_start;
	if (!OidIsValid(b->roleId))
		b->roleId = a->userid;
	if (!OidIsValid(b->databaseId))
		b->databaseId = a->databaseid;

	if (b->visible)
		horizon_copy_clip(b->query, sizeof(b->query), a->query);
	else
		horizon_copy_clip(b->query, sizeof(b->query), "<insufficient privilege>");
}

static void
horizon_fill_names(HorizonBlocker *b)
{
	char	   *name;

	if (OidIsValid(b->databaseId))
	{
		name = get_database_name(b->databaseId);
		if (name)
			horizon_copy_clip(b->datname, sizeof(b->datname), name);
	}
	if (OidIsValid(b->roleId))
	{
		name = GetUserNameFromId(b->roleId, true);
		if (name)
			horizon_copy_clip(b->usename, sizeof(b->usename), name);
	}
}

static void
horizon_attach_locks(HorizonSnapshot *snap)
{
	LockData   *lockData;
	int			i,
				j,
				k;
	bool		any = false;

	for (k = 0; k < snap->nblockers; k++)
	{
		HorizonBlocker *b = snap->blockers[k];

		if (!b->is_observer && b->pid > 0 &&
			(b->type == HORIZON_BLK_BACKEND || b->type == HORIZON_BLK_STANDBY_FEEDBACK))
		{
			any = true;
			break;
		}
	}
	if (!any)
		return;

	lockData = GetLockStatusData();

	for (i = 0; i < lockData->nelements; i++)
	{
		LockInstanceData *waiter = &lockData->locks[i];

		if (waiter->waitLockMode == NoLock || waiter->pid <= 0)
			continue;

		for (j = 0; j < lockData->nelements; j++)
		{
			LockInstanceData *holder = &lockData->locks[j];
			LOCKMODE	held;
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
				HorizonBlocker *b = snap->blockers[k];
				int			w;

				if (b->pid != holder->pid ||
					(b->type != HORIZON_BLK_BACKEND &&
					 b->type != HORIZON_BLK_STANDBY_FEEDBACK))
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

/* ------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------ */

HorizonSnapshot *
horizon_snapshot_collect(int flags)
{
	HorizonSnapshot *snap;
	HorizonProcCopy *copies = NULL;
	int			ncopies = 0;
	int			i;

	snap = (HorizonSnapshot *) palloc0(sizeof(HorizonSnapshot));
	snap->flags = flags;
	snap->database = MyDatabaseId;
	snap->in_recovery = RecoveryInProgress();
	snap->maxblockers = 16;
	snap->blockers = (HorizonBlocker **)
		palloc(sizeof(HorizonBlocker *) * snap->maxblockers);

	horizon_collect_limits(snap);

	if (!(flags & HORIZON_COLLECT_HORIZONS))
		return snap;

	copies = (HorizonProcCopy *)
		palloc(sizeof(HorizonProcCopy) *
			   ((size_t) ProcGlobal->allProcCount + Max(max_prepared_xacts, 0) + 1));
	horizon_collect_horizons(snap, copies, &ncopies);

	if (flags & HORIZON_COLLECT_BLOCKERS)
	{
		HorizonAct *acts = NULL;
		int			nacts = 0;

		horizon_build_proc_blockers(snap, copies, ncopies);
		horizon_collect_slots(snap);

		horizon_load_activity(&acts, &nacts);
		for (i = 0; i < snap->nblockers; i++)
		{
			horizon_apply_activity(snap->blockers[i], acts, nacts);
			horizon_fill_names(snap->blockers[i]);
		}
		if (acts)
			pfree(acts);

		if (flags & HORIZON_COLLECT_DETAILS)
		{
			horizon_attach_locks(snap);
			for (i = 0; i < snap->nblockers; i++)
				horizon_fill_blocker_reason(snap->blockers[i], snap);
		}
	}

	for (i = 0; i < ncopies; i++)
	{
		if (copies[i].gid)
			pfree(copies[i].gid);
	}
	pfree(copies);
	return snap;
}

void
horizon_snapshot_free(HorizonSnapshot *snap)
{
	int			i;

	if (snap == NULL)
		return;
	for (i = 0; i < snap->nblockers; i++)
		pfree(snap->blockers[i]);
	if (snap->blockers)
		pfree(snap->blockers);
	pfree(snap);
}

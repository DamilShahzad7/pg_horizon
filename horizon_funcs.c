/*-------------------------------------------------------------------------
 *
 * horizon_funcs.c
 *	  SQL-callable diagnostic functions
 *
 * Compatibility note: the C library may be installed before ALTER EXTENSION
 * ... UPDATE has run, so a function must work with the SQL definition of the
 * previous extension version. That is why record-returning functions fill a
 * values[] array sized for the newest definition and let heap_form_tuple() /
 * tuplestore_putvalues() consume only as many leading columns as the
 * installed SQL declares. New OUT columns are therefore only ever appended.
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 * IDENTIFICATION
 *	  horizon_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/proc.h"
#include "utils/array.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "pg_horizon.h"

PG_FUNCTION_INFO_V1(pg_horizon_status);
PG_FUNCTION_INFO_V1(pg_horizon_blockers);
PG_FUNCTION_INFO_V1(pg_horizon_databases);
PG_FUNCTION_INFO_V1(pg_horizon_relations);
PG_FUNCTION_INFO_V1(pg_horizon_explain);
PG_FUNCTION_INFO_V1(pg_horizon_report);
PG_FUNCTION_INFO_V1(pg_horizon_check);
PG_FUNCTION_INFO_V1(pg_horizon_vacuum_sql);

/* Room for the widest record any of our functions returns, now or in a later version. */
#define HORIZON_MAX_COLS		32

static Datum
horizon_text_or_null(const char *s, bool *isnull)
{
	if (s == NULL || s[0] == '\0')
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	return CStringGetTextDatum(s);
}

static void
horizon_set_text(Datum *values, bool *nulls, int idx, const char *s)
{
	values[idx] = horizon_text_or_null(s, &nulls[idx]);
}

static void
horizon_set_xid(Datum *values, bool *nulls, int idx, TransactionId xid)
{
	if (!TransactionIdIsValid(xid))
		nulls[idx] = true;
	else
		values[idx] = TransactionIdGetDatum(xid);
}

static void
horizon_set_age(Datum *values, bool *nulls, int idx, int64 age)
{
	if (age < 0)
		nulls[idx] = true;
	else
		values[idx] = Int64GetDatum(age);
}

static ArrayType *
horizon_int_array(const int *pids, int n)
{
	Datum	   *elems;
	ArrayType  *arr;
	int			i;

	if (n <= 0)
		return NULL;

	elems = (Datum *) palloc(sizeof(Datum) * n);
	for (i = 0; i < n; i++)
		elems[i] = Int32GetDatum(pids[i]);
	arr = construct_array(elems, n, INT4OID, sizeof(int32), true, TYPALIGN_INT);
	pfree(elems);
	return arr;
}

/* "12345" or "(none)" for the text report. */
static const char *
horizon_xid_str(TransactionId xid)
{
	if (!TransactionIdIsValid(xid))
		return "(none)";
	return psprintf("%u", xid);
}

/*
 * Same FreezeLimit arithmetic as vacuum_get_cutoffs(): nextXID -
 * freeze_min_age, freeze_min_age capped at autovacuum_freeze_max_age/2, then
 * never newer than OldestXmin. On a cluster whose XID counter has never
 * wrapped, a limit that would fall below FirstNormalTransactionId is reported
 * as FirstNormalTransactionId (nothing is older) instead of a wrapped value
 * near 2^32 that only looks alarming.
 */
static TransactionId
horizon_freeze_limit(const HorizonSnapshot *snap, TransactionId oldest_xmin)
{
	int			freeze_min_age;
	TransactionId freeze_limit;

	freeze_min_age = vacuum_freeze_min_age;
	if (freeze_min_age < 0)
		freeze_min_age = 0;
	if (autovacuum_freeze_max_age > 1)
		freeze_min_age = Min(freeze_min_age, autovacuum_freeze_max_age / 2);

	if (snap->next_xid_epoch == 0 &&
		(int64) snap->next_xid - (int64) freeze_min_age < (int64) FirstNormalTransactionId)
		freeze_limit = FirstNormalTransactionId;
	else
		freeze_limit = snap->next_xid - (TransactionId) freeze_min_age;

	if (!TransactionIdIsNormal(freeze_limit))
		freeze_limit = FirstNormalTransactionId;
	if (TransactionIdIsNormal(oldest_xmin) &&
		TransactionIdPrecedes(oldest_xmin, freeze_limit))
		freeze_limit = oldest_xmin;
	return freeze_limit;
}

typedef struct HorizonFreezeState
{
	bool		urgent_xid;
	bool		urgent_mxid;
	bool		can_advance_xid;
	bool		can_advance_mxid;
} HorizonFreezeState;

/*
 * A relation is only wraparound-urgent when an age has reached the matching
 * autovacuum_*freeze_max_age. Among urgent dimensions:
 *	 "horizon"    - at least one cannot advance: relfrozenxid / relminmxid is
 *					not older than the cutoff VACUUM would use, so vacuuming
 *					does not help until a holder goes away
 *	 "vacuum_lag" - all of them can advance; autovacuum just has not got there
 * Healthy tables sit at the horizon all the time, which is why non-urgent
 * tables are "ok" rather than "horizon".
 */
static const char *
horizon_freeze_constraint(const HorizonSnapshot *snap, TransactionId relfrozenxid,
						  TransactionId xid_cutoff, int64 xage,
						  MultiXactId relminmxid, int64 mage,
						  HorizonFreezeState *st)
{
	HorizonFreezeState local;

	if (st == NULL)
		st = &local;

	st->urgent_xid = xage >= autovacuum_freeze_max_age;
	st->can_advance_xid = TransactionIdIsNormal(relfrozenxid) &&
		TransactionIdIsNormal(xid_cutoff) &&
		TransactionIdPrecedes(relfrozenxid, xid_cutoff);

	st->urgent_mxid = MultiXactIdIsValid(relminmxid) &&
		autovacuum_multixact_freeze_max_age > 0 &&
		mage >= (int64) autovacuum_multixact_freeze_max_age;
	st->can_advance_mxid = MultiXactIdIsValid(relminmxid) &&
		MultiXactIdIsValid(snap->mxid_horizon) &&
		MultiXactIdPrecedes(relminmxid, snap->mxid_horizon);

	if (!st->urgent_xid && !st->urgent_mxid)
		return "ok";
	if ((st->urgent_xid && !st->can_advance_xid) ||
		(st->urgent_mxid && !st->can_advance_mxid))
		return "horizon";
	return "vacuum_lag";
}

/* CREATE-time reloptions check: is the relation a user_catalog_table? */
static bool
horizon_reloptions_user_catalog(char relkind, Datum reloptions, bool isnull)
{
	List	   *opts;
	ListCell   *lc;

	if (isnull || !XLogLogicalInfoActive())
		return false;
	if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
		return false;

	opts = untransformRelOptions(reloptions);
	foreach(lc, opts)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "user_catalog_table") == 0)
			return defGetBoolean(def);
	}
	return false;
}

static bool
horizon_scan_user_catalog(Relation pgclass, HeapTuple tuple, Form_pg_class form)
{
	bool		isnull;
	Datum		d;

	if (!XLogLogicalInfoActive())
		return false;
	d = heap_getattr(tuple, Anum_pg_class_reloptions, RelationGetDescr(pgclass),
					 &isnull);
	return horizon_reloptions_user_catalog(form->relkind, d, isnull);
}

/* Highest severity rank of the xid and mxid state of one database. */
static const char *
horizon_database_state(int64 xage, int64 mage)
{
	int			rank = 0;

	if (vacuum_failsafe_age > 0 && xage >= vacuum_failsafe_age)
		rank = Max(rank, 3);
	else if (xage >= autovacuum_freeze_max_age)
		rank = Max(rank, 2);
	else if (xage >= autovacuum_freeze_max_age / 2)
		rank = Max(rank, 1);

	if (vacuum_multixact_failsafe_age > 0 && mage >= vacuum_multixact_failsafe_age)
		rank = Max(rank, 3);
	else if (autovacuum_multixact_freeze_max_age > 0 &&
			 mage >= (int64) autovacuum_multixact_freeze_max_age)
		rank = Max(rank, 2);
	else if (autovacuum_multixact_freeze_max_age > 0 &&
			 mage >= (int64) autovacuum_multixact_freeze_max_age / 2)
		rank = Max(rank, 1);

	switch (rank)
	{
		case 3:
			return "failsafe";
		case 2:
			return "aggressive_autovacuum";
		case 1:
			return "aging";
		default:
			return "ok";
	}
}

/*
 * Look a relation up by OID without locking it. Diagnostics must not queue
 * behind an ACCESS EXCLUSIVE lock on the very table being investigated, and
 * everything reported here lives in pg_class.
 */
typedef struct HorizonClassInfo
{
	Oid			relid;
	Oid			relnamespace;
	char		relkind;
	char		relpersistence;
	bool		relisshared;
	bool		user_catalog;
	TransactionId relfrozenxid;
	MultiXactId relminmxid;
	char		relname[NAMEDATALEN];
} HorizonClassInfo;

static void
horizon_lookup_class(Oid relid, HorizonClassInfo *ci)
{
	HeapTuple	tup;
	Form_pg_class form;
	bool		isnull;
	Datum		d;

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", relid)));

	form = (Form_pg_class) GETSTRUCT(tup);
	ci->relid = relid;
	ci->relnamespace = form->relnamespace;
	ci->relkind = form->relkind;
	ci->relpersistence = form->relpersistence;
	ci->relisshared = form->relisshared;
	ci->relfrozenxid = form->relfrozenxid;
	ci->relminmxid = form->relminmxid;
	strlcpy(ci->relname, NameStr(form->relname), sizeof(ci->relname));
	d = SysCacheGetAttr(RELOID, tup, Anum_pg_class_reloptions, &isnull);
	ci->user_catalog = horizon_reloptions_user_catalog(form->relkind, d, isnull);
	ReleaseSysCache(tup);

	if (!RELKIND_HAS_TABLE_AM(ci->relkind))
	{
		if (ci->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a partitioned table, which has no freeze horizon of its own",
							ci->relname),
					 errhint("Run this on the individual partitions.")));
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table, materialized view, or TOAST table",
						ci->relname)));
	}
}

static char *
horizon_vacuum_sql_for(const HorizonClassInfo *ci)
{
	char	   *nsp = get_namespace_name(ci->relnamespace);

	if (nsp == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("could not find schema name for relation OID %u",
						ci->relid)));

	return psprintf("VACUUM (FREEZE, VERBOSE) %s.%s;",
					quote_identifier(nsp), quote_identifier(ci->relname));
}

/* ------------------------------------------------------------------------
 * pg_horizon_status()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_status(PG_FUNCTION_ARGS)
{
	HorizonSnapshot *snap;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[HORIZON_MAX_COLS];
	bool		nulls[HORIZON_MAX_COLS];
	HorizonSeverity sev;
	int64		xid_age;
	int64		mxid_age;
	int64		headroom;
	float8		pct;
	char	   *dbname;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));
	tupdesc = BlessTupleDesc(tupdesc);

	snap = horizon_snapshot_collect(HORIZON_COLLECT_LIMITS |
									HORIZON_COLLECT_HORIZONS |
									HORIZON_COLLECT_BLOCKERS);
	sev = horizon_compute_severity(snap, NULL);
	xid_age = horizon_xid_age(snap->oldest_xid, snap->next_xid);
	mxid_age = horizon_xid_age(snap->oldest_mxid, snap->next_mxid);
	headroom = horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit);

	if (xid_age <= 0)
		pct = 0;
	else
	{
		pct = ((float8) xid_age) * 100.0 / (float8) (MaxTransactionId / 2);
		if (pct > 100.0)
			pct = 100.0;
	}

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	horizon_set_xid(values, nulls, 0, snap->next_xid);
	horizon_set_xid(values, nulls, 1, snap->oldest_xid);
	if (OidIsValid(snap->oldest_xid_db))
		values[2] = ObjectIdGetDatum(snap->oldest_xid_db);
	else
		nulls[2] = true;
	dbname = OidIsValid(snap->oldest_xid_db) ?
		get_database_name(snap->oldest_xid_db) : NULL;
	horizon_set_text(values, nulls, 3, dbname);
	horizon_set_xid(values, nulls, 4, snap->data_xmin);
	horizon_set_xid(values, nulls, 5, snap->shared_xmin);
	horizon_set_xid(values, nulls, 6, snap->catalog_xmin);
	horizon_set_xid(values, nulls, 7, snap->oldest_running_xid);
	horizon_set_xid(values, nulls, 8, snap->slot_xmin);
	horizon_set_xid(values, nulls, 9, snap->slot_catalog_xmin);
	horizon_set_xid(values, nulls, 10, snap->next_mxid);
	horizon_set_xid(values, nulls, 11, snap->oldest_mxid);
	values[12] = Int64GetDatum(xid_age);
	values[13] = Int64GetDatum(mxid_age);
	values[14] = Int64GetDatum(autovacuum_freeze_max_age);
	values[15] = Int64GetDatum(vacuum_failsafe_age);
	values[16] = Int64GetDatum(headroom);
	values[17] = Float8GetDatum(pct);
	values[18] = CStringGetTextDatum(horizon_severity_name(sev));
	values[19] = BoolGetDatum(snap->in_recovery);
	values[20] = Int32GetDatum(autovacuum_max_workers);
	values[21] = Int32GetDatum(horizon_count_listed(snap, false));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	horizon_snapshot_free(snap);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ------------------------------------------------------------------------
 * pg_horizon_blockers()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_blockers(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HorizonSnapshot *snap;
	bool		include_observer = false;
	int			i;

	if (!PG_ARGISNULL(0))
		include_observer = PG_GETARG_BOOL(0);

	InitMaterializedSRF(fcinfo, 0);
	snap = horizon_snapshot_collect(HORIZON_COLLECT_ALL);

	for (i = 0; i < snap->nblockers; i++)
	{
		HorizonBlocker *b = snap->blockers[i];
		Datum		values[31];
		bool		nulls[31];
		ArrayType  *waiters;
		bool		idle_xact;

		if (b->is_observer && !include_observer)
			continue;
		if (!horizon_blocker_listed(b))
			continue;

		/*
		 * pg_stat_activity hides state, wait events, xact_start and the
		 * query of other roles' sessions from callers without the right to
		 * see them; do the same here. The row itself (pid, database, role,
		 * xid/xmin) stays, as it does there.
		 */
		idle_xact = horizon_blocker_is_idle_xact(b);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		values[0] = CStringGetTextDatum(horizon_blocker_type_name(b->type));
		if (b->pid > 0)
			values[1] = Int32GetDatum(b->pid);
		else
			nulls[1] = true;
		horizon_set_text(values, nulls, 2, b->slot_name);
		horizon_set_text(values, nulls, 3, b->prepared_gid);
		if (OidIsValid(b->databaseId))
			values[4] = ObjectIdGetDatum(b->databaseId);
		else
			nulls[4] = true;
		horizon_set_text(values, nulls, 5, b->datname);
		horizon_set_text(values, nulls, 6, b->usename);
		horizon_set_text(values, nulls, 7, b->application_name);
		if (b->visible)
		{
			horizon_set_text(values, nulls, 8, b->backend_type[0] ? b->backend_type : "unknown");
			horizon_set_text(values, nulls, 9, b->state[0] ? b->state : "unknown");
		}
		else
		{
			nulls[8] = true;
			nulls[9] = true;
		}
		horizon_set_xid(values, nulls, 10, b->xid);
		horizon_set_xid(values, nulls, 11, b->xmin);
		horizon_set_age(values, nulls, 12, b->xmin_age);
		horizon_set_xid(values, nulls, 13, b->catalog_xmin);
		horizon_set_age(values, nulls, 14, b->catalog_age);
		if (b->visible && b->xact_start != 0)
			values[15] = TimestampTzGetDatum(b->xact_start);
		else
			nulls[15] = true;
		if (b->visible)
		{
			horizon_set_text(values, nulls, 16, b->wait_event_type);
			horizon_set_text(values, nulls, 17, b->wait_event);
		}
		else
		{
			nulls[16] = true;
			nulls[17] = true;
		}
		horizon_set_text(values, nulls, 18, b->query);
		values[19] = BoolGetDatum(b->holds_waited_lock);
		waiters = horizon_int_array(b->waiter_pids, b->nwaiters);
		if (waiters)
			values[20] = PointerGetDatum(waiters);
		else
			nulls[20] = true;
		if (b->wal_retained_bytes > 0)
			values[21] = Int64GetDatum(b->wal_retained_bytes);
		else
			nulls[21] = true;
		values[22] = BoolGetDatum(b->affects_data);
		values[23] = BoolGetDatum(b->affects_catalog);
		if (b->visible)
			values[24] = BoolGetDatum(idle_xact);
		else
			nulls[24] = true;
		values[25] = BoolGetDatum(b->visible && horizon_blocker_safe_to_terminate(b, false));
		horizon_set_text(values, nulls, 26, b->recommended_sql);
		horizon_set_text(values, nulls, 27, b->reason);
		values[28] = BoolGetDatum(b->is_horizon_holder);
		horizon_set_text(values, nulls, 29, b->plugin);
		if (b->type == HORIZON_BLK_PHYSICAL_SLOT ||
			b->type == HORIZON_BLK_LOGICAL_SLOT)
			values[30] = BoolGetDatum(b->slot_active);
		else
			nulls[30] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	horizon_snapshot_free(snap);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * pg_horizon_databases()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_databases(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HorizonSnapshot *snap;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;

	InitMaterializedSRF(fcinfo, 0);
	snap = horizon_snapshot_collect(HORIZON_COLLECT_LIMITS);

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tuple);
		Datum		values[8];
		bool		nulls[8];
		int64		xage;
		int64		mage;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		values[0] = ObjectIdGetDatum(db->oid);
		values[1] = NameGetDatum(&db->datname);
		horizon_set_xid(values, nulls, 2, db->datfrozenxid);
		xage = horizon_xid_age(db->datfrozenxid, snap->next_xid);
		values[3] = Int64GetDatum(xage);
		horizon_set_xid(values, nulls, 4, db->datminmxid);
		mage = horizon_xid_age(db->datminmxid, snap->next_mxid);
		values[5] = Int64GetDatum(mage);
		values[6] = BoolGetDatum(db->oid == snap->oldest_xid_db);
		values[7] = CStringGetTextDatum(horizon_database_state(xage, mage));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	horizon_snapshot_free(snap);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * pg_horizon_relations()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_relations(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HorizonSnapshot *snap;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;
	int64		min_age = 0;

	if (!PG_ARGISNULL(0))
		min_age = PG_GETARG_INT64(0);

	InitMaterializedSRF(fcinfo, 0);
	snap = horizon_snapshot_collect(HORIZON_COLLECT_LIMITS | HORIZON_COLLECT_HORIZONS);

	rel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class form = (Form_pg_class) GETSTRUCT(tuple);
		Datum		values[16];
		bool		nulls[16];
		int64		xage;
		int64		mage;
		PgStat_StatTabEntry *tabentry;
		HorizonKind kind;
		char		relkind_str[2];

		if (!RELKIND_HAS_TABLE_AM(form->relkind))
			continue;
		if (form->relpersistence == RELPERSISTENCE_TEMP)
			continue;

		xage = horizon_xid_age(form->relfrozenxid, snap->next_xid);
		mage = horizon_xid_age(form->relminmxid, snap->next_mxid);
		if (xage < min_age && mage < min_age)
			continue;

		kind = horizon_kind_for_class(form->oid, form->relnamespace,
									  form->relisshared, form->relpersistence,
									  horizon_scan_user_catalog(rel, tuple, form),
									  snap);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		values[0] = ObjectIdGetDatum(form->oid);
		values[1] = ObjectIdGetDatum(form->relnamespace);
		values[2] = NameGetDatum(&form->relname);
		relkind_str[0] = form->relkind;
		relkind_str[1] = '\0';
		values[3] = CStringGetTextDatum(relkind_str);
		horizon_set_xid(values, nulls, 4, form->relfrozenxid);
		values[5] = Int64GetDatum(xage);
		horizon_set_xid(values, nulls, 6, form->relminmxid);
		values[7] = Int64GetDatum(mage);
		values[8] = Int32GetDatum(form->relpages);
		values[9] = Int32GetDatum(form->relallvisible);

		tabentry = pgstat_fetch_stat_tabentry_ext(form->relisshared, form->oid);
		if (tabentry)
		{
			values[10] = Int64GetDatum(tabentry->live_tuples);
			values[11] = Int64GetDatum(tabentry->dead_tuples);
			if (tabentry->last_vacuum_time > 0)
				values[12] = TimestampTzGetDatum(tabentry->last_vacuum_time);
			else
				nulls[12] = true;
			if (tabentry->last_autovacuum_time > 0)
				values[13] = TimestampTzGetDatum(tabentry->last_autovacuum_time);
			else
				nulls[13] = true;
		}
		else
		{
			nulls[10] = true;
			nulls[11] = true;
			nulls[12] = true;
			nulls[13] = true;
		}

		values[14] = CStringGetTextDatum(
			horizon_freeze_constraint(snap, form->relfrozenxid,
									  horizon_xmin_for_kind(snap, kind), xage,
									  form->relminmxid, mage, NULL));
		values[15] = BoolGetDatum(form->relisshared);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	horizon_snapshot_free(snap);
	return (Datum) 0;
}

/* ------------------------------------------------------------------------
 * pg_horizon_explain()
 * ------------------------------------------------------------------------ */

static const char *
horizon_kind_name(HorizonKind kind)
{
	switch (kind)
	{
		case HORIZON_KIND_SHARED:
			return "shared";
		case HORIZON_KIND_CATALOG:
			return "catalog";
		case HORIZON_KIND_TEMP:
			return "temporary";
		case HORIZON_KIND_DATA:
			break;
	}
	return "data";
}

static const char *
horizon_diagnosis(const char *constraint, const HorizonFreezeState *st)
{
	if (strcmp(constraint, "horizon") == 0)
	{
		if (st->urgent_xid && !st->can_advance_xid && st->urgent_mxid && !st->can_advance_mxid)
			return "Neither relfrozenxid nor relminmxid is older than the cutoff VACUUM would use, "
				"so VACUUM FREEZE cannot advance either until the holder is gone. "
				"See dominant_blocker and pg_horizon_blockers. "
				"Raising autovacuum_freeze_max_age only delays wraparound; it is not a fix.";
		if (st->urgent_xid && !st->can_advance_xid)
			return "relfrozenxid is not older than the xmin horizon VACUUM would use, "
				"so VACUUM FREEZE cannot advance it until the dominant xmin holder is gone. "
				"See dominant_blocker and pg_horizon_blockers. "
				"Raising autovacuum_freeze_max_age only delays wraparound; it is not a fix.";
		return "relminmxid is not older than the oldest MultiXact still in use, "
			"so VACUUM FREEZE cannot advance it until the sessions holding that "
			"MultiXact (row locks shared by several transactions) finish. "
			"Raising autovacuum_multixact_freeze_max_age only delays wraparound; it is not a fix.";
	}
	if (strcmp(constraint, "vacuum_lag") == 0)
		return "The horizon would allow freezing, but this relation has not been vacuumed far enough. "
			"Autovacuum is lagging (cost limit, too many tables, or the table is being skipped). "
			"Run the VACUUM statement in vacuum_sql; do not VACUUM FULL.";
	return "Neither the XID age nor the MultiXact age has reached its autovacuum freeze max age. "
		"No wraparound action is required for this relation right now.";
}

Datum
pg_horizon_explain(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	HorizonClassInfo ci;
	HorizonSnapshot *snap;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[HORIZON_MAX_COLS];
	bool		nulls[HORIZON_MAX_COLS];
	HorizonKind kind;
	HorizonFreezeState st;
	HorizonBlocker *dom;
	TransactionId rel_horizon;
	TransactionId freeze_limit;
	int64		xage;
	int64		mage;
	int64		horizon_age;
	PgStat_StatTabEntry *tabentry;
	const char *constraint;
	StringInfoData summary;
	bool		vacuum_running = false;
	char	   *nspname;
	int			i;
	int			nbackends;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));
	tupdesc = BlessTupleDesc(tupdesc);

	horizon_lookup_class(relid, &ci);
	nspname = get_namespace_name(ci.relnamespace);
	if (nspname == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("could not find schema name for relation OID %u", relid)));

	snap = horizon_snapshot_collect(HORIZON_COLLECT_ALL);
	kind = horizon_kind_for_class(ci.relid, ci.relnamespace, ci.relisshared,
								  ci.relpersistence, ci.user_catalog, snap);
	rel_horizon = horizon_xmin_for_kind(snap, kind);
	xage = horizon_xid_age(ci.relfrozenxid, snap->next_xid);
	mage = horizon_xid_age(ci.relminmxid, snap->next_mxid);
	horizon_age = horizon_xid_age(rel_horizon, snap->next_xid);

	freeze_limit = horizon_freeze_limit(snap, rel_horizon);
	constraint = horizon_freeze_constraint(snap, ci.relfrozenxid, rel_horizon, xage,
										   ci.relminmxid, mage, &st);

	nbackends = pgstat_fetch_stat_numbackends();
	for (i = 1; i <= nbackends; i++)
	{
		LocalPgBackendStatus *local = pgstat_get_local_beentry_by_index(i);

		if (local &&
			local->backendStatus.st_progress_command == PROGRESS_COMMAND_VACUUM &&
			local->backendStatus.st_progress_command_target == relid)
		{
			vacuum_running = true;
			break;
		}
	}

	initStringInfo(&summary);
	appendStringInfo(&summary,
					 "relation %s.%s relkind=%c horizon=%s relfrozenxid age=%lld relation_xmin age=%lld relminmxid age=%lld mxid_horizon=%u freeze_constraint=%s vacuum_running=%s",
					 horizon_printable(nspname), horizon_printable(ci.relname), ci.relkind,
					 horizon_kind_name(kind),
					 (long long) xage, (long long) horizon_age, (long long) mage,
					 snap->mxid_horizon, constraint, vacuum_running ? "yes" : "no");

	dom = horizon_dominant_blocker(snap, true, kind);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	values[0] = ObjectIdGetDatum(relid);
	values[1] = CStringGetTextDatum(summary.data);
	horizon_set_xid(values, nulls, 2, ci.relfrozenxid);
	values[3] = Int64GetDatum(xage);
	horizon_set_xid(values, nulls, 4, rel_horizon);
	values[5] = Int64GetDatum(horizon_age);
	horizon_set_xid(values, nulls, 6, freeze_limit);
	values[7] = CStringGetTextDatum(constraint);
	values[8] = BoolGetDatum(vacuum_running);
	tabentry = pgstat_fetch_stat_tabentry_ext(ci.relisshared, relid);
	if (tabentry)
	{
		values[9] = Int64GetDatum(tabentry->live_tuples);
		values[10] = Int64GetDatum(tabentry->dead_tuples);
	}
	else
	{
		nulls[9] = true;
		nulls[10] = true;
	}
	values[11] = CStringGetTextDatum(horizon_diagnosis(constraint, &st));
	values[12] = CStringGetTextDatum(horizon_vacuum_sql_for(&ci));
	if (dom)
		horizon_set_text(values, nulls, 13, dom->reason);
	else
		nulls[13] = true;

	tuple = heap_form_tuple(tupdesc, values, nulls);
	horizon_snapshot_free(snap);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ------------------------------------------------------------------------
 * pg_horizon_report()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_report(PG_FUNCTION_ARGS)
{
	HorizonSnapshot *snap;
	HorizonSeverity sev;
	StringInfoData buf;
	HorizonBlocker *dom;
	char	   *dbname;
	char	   *why;
	bool	   *used;
	int			shown = 0;
	int			nlisted;
	int			i;

	snap = horizon_snapshot_collect(HORIZON_COLLECT_ALL);
	sev = horizon_compute_severity(snap, &why);
	initStringInfo(&buf);

	appendStringInfoString(&buf, "pg_horizon diagnostic report\n");
	appendStringInfoString(&buf, "============================\n");
	appendStringInfo(&buf, "severity:              %s (%s)\n", horizon_severity_name(sev), why);
	appendStringInfo(&buf, "in_recovery:           %s\n", snap->in_recovery ? "yes" : "no");
	appendStringInfo(&buf, "next_xid:              %u\n", snap->next_xid);
	appendStringInfo(&buf, "oldest_xid:            %u  age=%lld\n",
					 snap->oldest_xid,
					 (long long) horizon_xid_age(snap->oldest_xid, snap->next_xid));
	dbname = OidIsValid(snap->oldest_xid_db) ? get_database_name(snap->oldest_xid_db) : NULL;
	appendStringInfo(&buf, "oldest_xid_database:   %s\n", dbname ? dbname : "(unknown)");
	appendStringInfo(&buf, "xids_until_stop:       %lld\n",
					 (long long) horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit));
	appendStringInfo(&buf, "xid_vac_limit:         %u\n", snap->xid_vac_limit);
	appendStringInfo(&buf, "xid_warn_limit:        %u\n", snap->xid_warn_limit);
	appendStringInfo(&buf, "xid_stop_limit:        %u\n", snap->xid_stop_limit);
	appendStringInfo(&buf, "oldest_mxid:           %u  age=%lld\n",
					 snap->oldest_mxid,
					 (long long) horizon_xid_age(snap->oldest_mxid, snap->next_mxid));
	appendStringInfo(&buf, "autovacuum_freeze_max_age:            %d\n", autovacuum_freeze_max_age);
	appendStringInfo(&buf, "vacuum_failsafe_age:                  %d\n", vacuum_failsafe_age);
	appendStringInfo(&buf, "autovacuum_multixact_freeze_max_age:  %d\n", autovacuum_multixact_freeze_max_age);
	appendStringInfo(&buf, "vacuum_multixact_failsafe_age:        %d\n\n", vacuum_multixact_failsafe_age);

	appendStringInfo(&buf, "Horizons a VACUUM started now would see (this session excluded%s)\n",
					 snap->in_recovery ? "; NOT excluded during recovery" : "");
	appendStringInfoString(&buf, "------------------------------------------------------------\n");
	appendStringInfo(&buf, "shared_xmin:           %s  age=%lld\n",
					 horizon_xid_str(snap->shared_xmin),
					 (long long) horizon_xid_age(snap->shared_xmin, snap->next_xid));
	appendStringInfo(&buf, "data_xmin:             %s  age=%lld   (database oid %u)\n",
					 horizon_xid_str(snap->data_xmin),
					 (long long) horizon_xid_age(snap->data_xmin, snap->next_xid),
					 snap->database);
	appendStringInfo(&buf, "catalog_xmin:          %s  age=%lld\n",
					 horizon_xid_str(snap->catalog_xmin),
					 (long long) horizon_xid_age(snap->catalog_xmin, snap->next_xid));
	appendStringInfo(&buf, "oldest_running_xid:    %s\n", horizon_xid_str(snap->oldest_running_xid));
	appendStringInfo(&buf, "slot_xmin:             %s\n", horizon_xid_str(snap->slot_xmin));
	appendStringInfo(&buf, "slot_catalog_xmin:     %s\n", horizon_xid_str(snap->slot_catalog_xmin));
	appendStringInfo(&buf, "mxid_horizon:          %s\n", horizon_xid_str(snap->mxid_horizon));
	nlisted = horizon_count_listed(snap, false);
	appendStringInfo(&buf, "blockers_listed:       %d  (pg_horizon.min_xmin_age = %d)\n\n",
					 nlisted, pg_horizon_min_xmin_age);

	dom = horizon_dominant_blocker(snap, false, HORIZON_KIND_DATA);
	appendStringInfoString(&buf, "Dominant holder\n");
	appendStringInfoString(&buf, "---------------\n");
	if (dom == NULL)
		appendStringInfoString(&buf, "(no listed holder pins a horizon)\n\n");
	else
	{
		appendStringInfo(&buf, "type: %s  pid: %d  slot: %s\n",
						 horizon_blocker_type_name(dom->type),
						 dom->pid,
						 dom->slot_name[0] ? dom->slot_name : "-");
		appendStringInfo(&buf, "%s\n", dom->reason);
		appendStringInfo(&buf, "recommended:\n%s\n\n", dom->recommended_sql[0] ? dom->recommended_sql : "(none)");
	}

	appendStringInfoString(&buf, "Oldest holders\n");
	appendStringInfoString(&buf, "--------------\n");
	used = (bool *) palloc0(sizeof(bool) * Max(snap->nblockers, 1));
	while (shown < 15)
	{
		int			best = -1;
		HorizonBlocker *b;

		for (i = 0; i < snap->nblockers; i++)
		{
			if (used[i] || snap->blockers[i]->is_observer ||
				!horizon_blocker_listed(snap->blockers[i]))
				continue;
			if (best < 0 || snap->blockers[i]->age > snap->blockers[best]->age)
				best = i;
		}
		if (best < 0)
			break;

		used[best] = true;
		b = snap->blockers[best];
		appendStringInfo(&buf, "  %-18s pid=%-6d age=%-10lld pins_horizon=%s state=%s %s\n",
						 horizon_blocker_type_name(b->type),
						 b->pid,
						 (long long) b->age,
						 b->is_horizon_holder ? "yes" : "no",
						 (b->visible && b->state[0]) ? b->state : "-",
						 b->slot_name[0] ? b->slot_name :
						 (b->visible ? b->application_name : ""));
		shown++;
	}
	pfree(used);
	if (shown == 0)
		appendStringInfoString(&buf, "  none\n");

	appendStringInfoString(&buf,
						   "\nDo not VACUUM FULL. Do not raise autovacuum_freeze_max_age to hide this.\n"
						   "Remove the xmin holder, then let VACUUM (FREEZE) run.\n");

	horizon_snapshot_free(snap);
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

/* ------------------------------------------------------------------------
 * pg_horizon_check()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_check(PG_FUNCTION_ARGS)
{
	HorizonSnapshot *snap;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[HORIZON_MAX_COLS];
	bool		nulls[HORIZON_MAX_COLS];
	HorizonSeverity sev;
	int			code;
	int			holders;
	char	   *why;
	HorizonBlocker *dom;
	StringInfoData msg;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));
	tupdesc = BlessTupleDesc(tupdesc);

	snap = horizon_snapshot_collect(HORIZON_COLLECT_ALL);
	sev = horizon_compute_severity(snap, &why);
	switch (sev)
	{
		case HORIZON_SEV_OK:
			code = 0;
			break;
		case HORIZON_SEV_WARNING:
			code = 1;
			break;
		case HORIZON_SEV_CRITICAL:
		case HORIZON_SEV_EMERGENCY:
		default:
			code = 2;
			break;
	}

	holders = horizon_count_listed(snap, true);
	dom = horizon_dominant_blocker(snap, false, HORIZON_KIND_DATA);

	/*
	 * The message explains the *status*: what made it warning/critical, and
	 * only then who is holding the horizon. At status ok it stays calm.
	 */
	initStringInfo(&msg);
	appendStringInfoString(&msg, why);
	if (sev == HORIZON_SEV_OK)
	{
		if (holders > 0 && dom)
			appendStringInfo(&msg,
							 ". %d holder(s) currently pin a horizon; the oldest is %s%s%s, age %lld, below the warning thresholds",
							 holders,
							 horizon_blocker_type_name(dom->type),
							 dom->pid > 0 ? " pid " : "",
							 dom->pid > 0 ? psprintf("%d", dom->pid) : "",
							 (long long) dom->age);
	}
	else if (dom)
		appendStringInfo(&msg, ". Dominant holder: %s", dom->reason);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	values[0] = CStringGetTextDatum(horizon_severity_name(sev));
	values[1] = Int32GetDatum(code);
	values[2] = Int64GetDatum(horizon_xid_age(snap->oldest_xid, snap->next_xid));
	values[3] = Int64GetDatum(horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit));
	values[4] = Int32GetDatum(holders);
	values[5] = CStringGetTextDatum(msg.data);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	horizon_snapshot_free(snap);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ------------------------------------------------------------------------
 * pg_horizon_vacuum_sql()
 * ------------------------------------------------------------------------ */

Datum
pg_horizon_vacuum_sql(PG_FUNCTION_ARGS)
{
	HorizonClassInfo ci;

	horizon_lookup_class(PG_GETARG_OID(0), &ci);
	PG_RETURN_TEXT_P(cstring_to_text(horizon_vacuum_sql_for(&ci)));
}

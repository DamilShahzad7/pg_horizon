/*-------------------------------------------------------------------------
 *
 * horizon_funcs.c
 *	  SQL-callable diagnostic functions
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
#include "access/table.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/array.h"
#include "postmaster/autovacuum.h"
#include "storage/proc.h"
#include "utils/acl.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#include "pg_horizon.h"

#include "commands/progress.h"

PG_FUNCTION_INFO_V1(pg_horizon_status);
PG_FUNCTION_INFO_V1(pg_horizon_blockers);
PG_FUNCTION_INFO_V1(pg_horizon_databases);
PG_FUNCTION_INFO_V1(pg_horizon_relations);
PG_FUNCTION_INFO_V1(pg_horizon_explain);
PG_FUNCTION_INFO_V1(pg_horizon_report);
PG_FUNCTION_INFO_V1(pg_horizon_check);
PG_FUNCTION_INFO_V1(pg_horizon_vacuum_sql);

static TransactionId
horizon_xmin_for_rel_public(Relation rel)
{
	return GetOldestNonRemovableTransactionId(rel);
}

static text *
horizon_cstring_text(const char *s)
{
	if (s == NULL || s[0] == '\0')
		return NULL;
	return cstring_to_text(s);
}

static void
horizon_xid_value(Datum *values, bool *nulls, int idx,
				  TransactionId xid)
{
	if (!TransactionIdIsValid(xid))
		nulls[idx] = true;
	else
		values[idx] = TransactionIdGetDatum(xid);
}

static ArrayType *
horizon_int_array(const int *pids, int n)
{
	Datum	   *elems;
	int			i;

	if (n <= 0)
		return NULL;

	elems = (Datum *) palloc(sizeof(Datum) * n);
	for (i = 0; i < n; i++)
		elems[i] = Int32GetDatum(pids[i]);
	{
		ArrayType  *arr;

		arr = construct_array(elems, n, INT4OID, sizeof(int32), true, TYPALIGN_INT);
		pfree(elems);
		return arr;
	}
}

/*
 * Same relation-kind split as GlobalVisHorizonKindForRel(), using the
 * horizons already collected. Recovery uses the shared cutoff.
 */
static TransactionId
horizon_cutoff_for_class(Form_pg_class form, const HorizonSnapshot *snap)
{
	if (snap->in_recovery || form->relisshared)
		return snap->shared_xmin;
	if (IsCatalogRelationOid(form->oid) ||
		IsCatalogNamespace(form->relnamespace))
		return snap->catalog_xmin;
	return snap->data_xmin;
}

/*
 * Same FreezeLimit arithmetic as vacuum_get_cutoffs(): nextXID - freeze_min_age,
 * freeze_min_age capped at autovacuum_freeze_max_age/2, then never newer than
 * OldestXmin.
 */
static TransactionId
horizon_freeze_limit(TransactionId oldest_xmin, TransactionId next_xid)
{
	int			freeze_min_age;
	TransactionId freeze_limit;

	freeze_min_age = vacuum_freeze_min_age;
	if (freeze_min_age < 0)
		freeze_min_age = 0;
	if (autovacuum_freeze_max_age > 1)
		freeze_min_age = Min(freeze_min_age, autovacuum_freeze_max_age / 2);

	freeze_limit = next_xid - (TransactionId) freeze_min_age;
	if (!TransactionIdIsNormal(freeze_limit))
		freeze_limit = FirstNormalTransactionId;
	if (TransactionIdIsNormal(oldest_xmin) &&
		TransactionIdPrecedes(oldest_xmin, freeze_limit))
		freeze_limit = oldest_xmin;
	return freeze_limit;
}

/*
 * VACUUM FREEZE uses freeze_min_age 0, so it can advance relfrozenxid up to
 * OldestXmin. Default vacuum_freeze_min_age is 50M; treating "within
 * freeze_min_age of xmin" as horizon-bound marked every healthy table as
 * stuck. Only wraparound-urgent tables are horizon vs vacuum_lag.
 */
static const char *
horizon_freeze_constraint(TransactionId relfrozenxid, TransactionId cutoff,
						  int64 xage)
{
	bool		can_advance;

	can_advance = TransactionIdIsNormal(relfrozenxid) &&
		TransactionIdIsNormal(cutoff) &&
		TransactionIdPrecedes(relfrozenxid, cutoff);

	if (xage < autovacuum_freeze_max_age)
		return "ok";
	if (can_advance)
		return "vacuum_lag";
	return "horizon";
}

static HorizonBlocker *
horizon_dominant_blocker(HorizonSnapshot *snap)
{
	HorizonBlocker *best = NULL;
	int64		best_age = -1;
	int			i;

	for (i = 0; i < snap->nblockers; i++)
	{
		HorizonBlocker *b = &snap->blockers[i];
		TransactionId hold;
		int64		age;

		if (b->is_observer)
			continue;
		hold = TransactionIdOlder(b->xmin, b->xid);
		hold = TransactionIdOlder(hold, b->catalog_xmin);
		age = horizon_xid_age(hold, snap->next_xid);
		if (age > best_age)
		{
			best_age = age;
			best = b;
		}
	}
	return best;
}

Datum
pg_horizon_status(PG_FUNCTION_ARGS)
{
	HorizonSnapshot *snap;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[22];
	bool		nulls[22];
	HorizonSeverity sev;
	int64		xid_age;
	int64		mxid_age;
	int64		headroom;
	float8		pct;
	char	   *dbname;
	int			nvisible = 0;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));
	tupdesc = BlessTupleDesc(tupdesc);

	snap = horizon_snapshot_collect();
	sev = horizon_compute_severity(snap);
	xid_age = horizon_xid_age(snap->oldest_xid, snap->next_xid);
	mxid_age = horizon_xid_age(snap->oldest_mxid, snap->next_mxid);
	headroom = horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit);

	for (i = 0; i < snap->nblockers; i++)
	{
		if (!snap->blockers[i].is_observer)
			nvisible++;
	}

	if (xid_age <= 0)
		pct = 0;
	else
	{
		pct = ((float8) xid_age) * 100.0 / (float8) (MaxTransactionId / 2);
		if (pct > 100.0)
			pct = 100.0;
	}

	memset(nulls, 0, sizeof(nulls));
	horizon_xid_value(values, nulls, 0, snap->next_xid);
	horizon_xid_value(values, nulls, 1, snap->oldest_xid);
	if (OidIsValid(snap->oldest_xid_db))
		values[2] = ObjectIdGetDatum(snap->oldest_xid_db);
	else
		nulls[2] = true;
	dbname = OidIsValid(snap->oldest_xid_db) ?
		get_database_name(snap->oldest_xid_db) : NULL;
	if (dbname)
		values[3] = CStringGetTextDatum(dbname);
	else
		nulls[3] = true;
	horizon_xid_value(values, nulls, 4, snap->data_xmin);
	horizon_xid_value(values, nulls, 5, snap->shared_xmin);
	horizon_xid_value(values, nulls, 6, snap->catalog_xmin);
	horizon_xid_value(values, nulls, 7, snap->oldest_running_xid);
	horizon_xid_value(values, nulls, 8, snap->slot_xmin);
	horizon_xid_value(values, nulls, 9, snap->slot_catalog_xmin);
	horizon_xid_value(values, nulls, 10, snap->next_mxid);
	horizon_xid_value(values, nulls, 11, snap->oldest_mxid);
	values[12] = Int64GetDatum(xid_age);
	values[13] = Int64GetDatum(mxid_age);
	values[14] = Int64GetDatum(autovacuum_freeze_max_age);
	values[15] = Int64GetDatum(vacuum_failsafe_age);
	values[16] = Int64GetDatum(headroom);
	values[17] = Float8GetDatum(pct);
	values[18] = CStringGetTextDatum(horizon_severity_name(sev));
	values[19] = BoolGetDatum(snap->in_recovery);
	values[20] = Int32GetDatum(autovacuum_max_workers);
	values[21] = Int32GetDatum(nvisible);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	horizon_snapshot_free(snap);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

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
	snap = horizon_snapshot_collect();

	for (i = 0; i < snap->nblockers; i++)
	{
		HorizonBlocker *b = &snap->blockers[i];
		Datum		values[31];
		bool		nulls[31];
		TransactionId hold;
		int64		age;
		int64		cage;
		ArrayType  *waiters;
		text	   *t;

		if (b->is_observer && !include_observer)
			continue;

		hold = TransactionIdOlder(b->xmin, b->xid);
		age = horizon_xid_age(hold, snap->next_xid);
		if (pg_horizon_min_xmin_age > 0 &&
			age < pg_horizon_min_xmin_age &&
			horizon_xid_age(b->catalog_xmin, snap->next_xid) < pg_horizon_min_xmin_age)
			continue;

		memset(nulls, 0, sizeof(nulls));
		values[0] = CStringGetTextDatum(horizon_blocker_type_name(b->type));
		if (b->pid > 0)
			values[1] = Int32GetDatum(b->pid);
		else
			nulls[1] = true;
		if (b->slot_name[0])
			values[2] = CStringGetTextDatum(b->slot_name);
		else
			nulls[2] = true;
		if (b->prepared_gid[0])
			values[3] = CStringGetTextDatum(b->prepared_gid);
		else
			nulls[3] = true;
		if (OidIsValid(b->databaseId))
			values[4] = ObjectIdGetDatum(b->databaseId);
		else
			nulls[4] = true;
		if (b->datname[0])
			values[5] = CStringGetTextDatum(b->datname);
		else
			nulls[5] = true;
		if (b->usename[0])
			values[6] = CStringGetTextDatum(b->usename);
		else
			nulls[6] = true;
		if (b->application_name[0])
			values[7] = CStringGetTextDatum(b->application_name);
		else
			nulls[7] = true;
		values[8] = CStringGetTextDatum(b->backend_type[0] ? b->backend_type : "unknown");
		values[9] = CStringGetTextDatum(b->state[0] ? b->state : "unknown");
		horizon_xid_value(values, nulls, 10, b->xid);
		horizon_xid_value(values, nulls, 11, b->xmin);
		values[12] = Int64GetDatum(age);
		horizon_xid_value(values, nulls, 13, b->catalog_xmin);
		cage = horizon_xid_age(b->catalog_xmin, snap->next_xid);
		if (TransactionIdIsValid(b->catalog_xmin))
			values[14] = Int64GetDatum(cage);
		else
			nulls[14] = true;
		if (b->xact_start != 0)
			values[15] = TimestampTzGetDatum(b->xact_start);
		else
			nulls[15] = true;
		if (b->wait_event_type[0])
			values[16] = CStringGetTextDatum(b->wait_event_type);
		else
			nulls[16] = true;
		if (b->wait_event[0])
			values[17] = CStringGetTextDatum(b->wait_event);
		else
			nulls[17] = true;
		t = horizon_cstring_text(b->query);
		if (t)
			values[18] = PointerGetDatum(t);
		else
			nulls[18] = true;
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
		values[24] = BoolGetDatum(strcmp(b->state, "idle in transaction") == 0 ||
								 strcmp(b->state, "idle in transaction (aborted)") == 0);
		values[25] = BoolGetDatum(horizon_blocker_safe_to_terminate(b, false));
		values[26] = CStringGetTextDatum(b->recommended_sql[0] ? b->recommended_sql : "");
		values[27] = CStringGetTextDatum(b->reason[0] ? b->reason : "");
		values[28] = BoolGetDatum(b->is_horizon_holder);
		if (b->plugin[0])
			values[29] = CStringGetTextDatum(b->plugin);
		else
			nulls[29] = true;
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

Datum
pg_horizon_databases(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HorizonSnapshot *snap;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;

	InitMaterializedSRF(fcinfo, 0);
	snap = horizon_snapshot_collect();

	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tuple);
		Datum		values[8];
		bool		nulls[8];
		int64		xage;
		int64		mage;
		const char *constraint;

		memset(nulls, 0, sizeof(nulls));
		values[0] = ObjectIdGetDatum(db->oid);
		values[1] = NameGetDatum(&db->datname);
		horizon_xid_value(values, nulls, 2, db->datfrozenxid);
		xage = horizon_xid_age(db->datfrozenxid, snap->next_xid);
		values[3] = Int64GetDatum(xage);
		horizon_xid_value(values, nulls, 4, db->datminmxid);
		mage = horizon_xid_age(db->datminmxid, snap->next_mxid);
		values[5] = Int64GetDatum(mage);
		values[6] = BoolGetDatum(db->oid == snap->oldest_xid_db);

		if (xage >= vacuum_failsafe_age)
			constraint = "failsafe";
		else if (xage >= autovacuum_freeze_max_age)
			constraint = "aggressive_autovacuum";
		else if (xage >= autovacuum_freeze_max_age / 2)
			constraint = "aging";
		else
			constraint = "ok";
		values[7] = CStringGetTextDatum(constraint);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	horizon_snapshot_free(snap);
	return (Datum) 0;
}

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
	snap = horizon_snapshot_collect();

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
		const char *freeze_constraint;
		char		relkind_str[2];

		if (!RELKIND_HAS_TABLE_AM(form->relkind))
			continue;
		if (form->relpersistence == RELPERSISTENCE_TEMP)
			continue;

		xage = horizon_xid_age(form->relfrozenxid, snap->next_xid);
		mage = horizon_xid_age(form->relminmxid, snap->next_mxid);
		if (xage < min_age && mage < min_age)
			continue;

		memset(nulls, 0, sizeof(nulls));
		values[0] = ObjectIdGetDatum(form->oid);
		values[1] = ObjectIdGetDatum(form->relnamespace);
		values[2] = NameGetDatum(&form->relname);
		relkind_str[0] = form->relkind;
		relkind_str[1] = '\0';
		values[3] = CStringGetTextDatum(relkind_str);
		horizon_xid_value(values, nulls, 4, form->relfrozenxid);
		values[5] = Int64GetDatum(xage);
		horizon_xid_value(values, nulls, 6, form->relminmxid);
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

		freeze_constraint = horizon_freeze_constraint(form->relfrozenxid,
													  horizon_cutoff_for_class(form, snap),
													  xage);
		values[14] = CStringGetTextDatum(freeze_constraint);
		values[15] = BoolGetDatum(form->relisshared);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	horizon_snapshot_free(snap);
	return (Datum) 0;
}

Datum
pg_horizon_explain(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	Form_pg_class form;
	HorizonSnapshot *snap;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[14];
	bool		nulls[14];
	TransactionId rel_horizon;
	TransactionId freeze_limit;
	int64		xage;
	int64		horizon_age;
	PgStat_StatTabEntry *tabentry;
	const char *constraint;
	const char *diagnosis;
	char		buf[2048];
	char		vacuum_sql[256];
	int			i;
	bool		vacuum_running = false;
	char	   *nspname;
	char	   *relname;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));
	tupdesc = BlessTupleDesc(tupdesc);

	rel = relation_open(relid, AccessShareLock);
	form = rel->rd_rel;
	if (!RELKIND_HAS_TABLE_AM(form->relkind))
	{
		char	   *badname = pstrdup(RelationGetRelationName(rel));

		relation_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table, materialized view, or TOAST table",
						badname)));
	}

	snap = horizon_snapshot_collect();
	rel_horizon = horizon_xmin_for_rel_public(rel);
	xage = horizon_xid_age(form->relfrozenxid, snap->next_xid);
	horizon_age = horizon_xid_age(rel_horizon, snap->next_xid);

	freeze_limit = horizon_freeze_limit(rel_horizon, snap->next_xid);
	constraint = horizon_freeze_constraint(form->relfrozenxid, rel_horizon, xage);

	for (i = 1; i <= pgstat_fetch_stat_numbackends(); i++)
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

	nspname = get_namespace_name(form->relnamespace);
	relname = pstrdup(NameStr(form->relname));
	if (nspname == NULL)
	{
		relation_close(rel, AccessShareLock);
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("could not find schema name for relation OID %u", relid)));
	}
	snprintf(vacuum_sql, sizeof(vacuum_sql),
			 "VACUUM (FREEZE, INDEX_CLEANUP ON, PROCESS_TOAST) %s.%s;",
			 quote_identifier(nspname),
			 quote_identifier(relname));

	if (strcmp(constraint, "horizon") == 0)
		diagnosis =
			"relfrozenxid is already at the OldestXmin VACUUM would use. "
			"Running VACUUM FREEZE will not advance frozenxid until the dominant "
			"xmin holder is removed. See pg_horizon_blockers. Raising "
			"autovacuum_freeze_max_age only delays wraparound; it is not a fix.";
	else if (strcmp(constraint, "vacuum_lag") == 0)
		diagnosis =
			"The global xmin would allow freezing, but this relation has not been "
			"vacuumed far enough. Autovacuum is lagging (cost limit, too many tables, "
			"or the table is skipped). Run the VACUUM FREEZE statement; do not VACUUM FULL.";
	else
		diagnosis =
			"Frozenxid age is within autovacuum_freeze_max_age. No wraparound action "
			"is required for this relation right now.";

	snprintf(buf, sizeof(buf),
			 "relation %s.%s relkind=%c relfrozenxid age=%lld relation_xmin age=%lld freeze_constraint=%s vacuum_running=%s",
			 nspname ? nspname : "?",
			 relname,
			 form->relkind,
			 (long long) xage,
			 (long long) horizon_age,
			 constraint,
			 vacuum_running ? "yes" : "no");

	memset(nulls, 0, sizeof(nulls));
	values[0] = ObjectIdGetDatum(relid);
	values[1] = CStringGetTextDatum(buf);
	horizon_xid_value(values, nulls, 2, form->relfrozenxid);
	values[3] = Int64GetDatum(xage);
	horizon_xid_value(values, nulls, 4, rel_horizon);
	values[5] = Int64GetDatum(horizon_age);
	horizon_xid_value(values, nulls, 6, freeze_limit);
	values[7] = CStringGetTextDatum(constraint);
	values[8] = BoolGetDatum(vacuum_running);
	tabentry = pgstat_fetch_stat_tabentry_ext(form->relisshared, relid);
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
	values[11] = CStringGetTextDatum(diagnosis);
	values[12] = CStringGetTextDatum(vacuum_sql);
	{
		HorizonBlocker *dom = horizon_dominant_blocker(snap);

		if (dom && !dom->is_observer)
			values[13] = CStringGetTextDatum(dom->reason);
		else
			nulls[13] = true;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	relation_close(rel, AccessShareLock);
	horizon_snapshot_free(snap);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
pg_horizon_report(PG_FUNCTION_ARGS)
{
	HorizonSnapshot *snap;
	HorizonSeverity sev;
	StringInfoData buf;
	HorizonBlocker *dom;
	char	   *dbname;
	int			shown = 0;
	int			i;

	snap = horizon_snapshot_collect();
	sev = horizon_compute_severity(snap);
	initStringInfo(&buf);

	appendStringInfoString(&buf, "pg_horizon diagnostic report\n");
	appendStringInfoString(&buf, "============================\n");
	appendStringInfo(&buf, "severity:              %s\n", horizon_severity_name(sev));
	appendStringInfo(&buf, "in_recovery:           %s\n", snap->in_recovery ? "yes" : "no");
	appendStringInfo(&buf, "next_xid:              %u\n", snap->next_xid);
	appendStringInfo(&buf, "oldest_xid:            %u  age=%lld\n",
					 snap->oldest_xid,
					 (long long) horizon_xid_age(snap->oldest_xid, snap->next_xid));
	dbname = OidIsValid(snap->oldest_xid_db) ? get_database_name(snap->oldest_xid_db) : NULL;
	appendStringInfo(&buf, "oldest_xid_database:   %s\n", dbname ? dbname : "(unknown)");
	appendStringInfo(&buf, "shared_xmin:           %u  age=%lld\n",
					 snap->shared_xmin,
					 (long long) horizon_xid_age(snap->shared_xmin, snap->next_xid));
	appendStringInfo(&buf, "data_xmin:             %u  age=%lld\n",
					 snap->data_xmin,
					 (long long) horizon_xid_age(snap->data_xmin, snap->next_xid));
	appendStringInfo(&buf, "catalog_xmin:          %u  age=%lld\n",
					 snap->catalog_xmin,
					 (long long) horizon_xid_age(snap->catalog_xmin, snap->next_xid));
	appendStringInfo(&buf, "slot_xmin:             %u\n", snap->slot_xmin);
	appendStringInfo(&buf, "slot_catalog_xmin:     %u\n", snap->slot_catalog_xmin);
	appendStringInfo(&buf, "oldest_mxid:           %u  age=%lld\n",
					 snap->oldest_mxid,
					 (long long) horizon_xid_age(snap->oldest_mxid, snap->next_mxid));
	appendStringInfo(&buf, "xid_vac_limit:         %u\n", snap->xid_vac_limit);
	appendStringInfo(&buf, "xid_warn_limit:        %u\n", snap->xid_warn_limit);
	appendStringInfo(&buf, "xid_stop_limit:        %u\n", snap->xid_stop_limit);
	appendStringInfo(&buf, "xids_until_stop:       %lld\n",
					 (long long) horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit));
	appendStringInfo(&buf, "autovacuum_freeze_max_age: %d\n", autovacuum_freeze_max_age);
	appendStringInfo(&buf, "vacuum_failsafe_age:   %d\n", vacuum_failsafe_age);
	{
		int			nvisible = 0;

		for (i = 0; i < snap->nblockers; i++)
		{
			if (!snap->blockers[i].is_observer)
				nvisible++;
		}
		appendStringInfo(&buf, "blockers_tracked:      %d\n\n", nvisible);
	}

	dom = horizon_dominant_blocker(snap);
	appendStringInfoString(&buf, "Dominant holder\n");
	appendStringInfoString(&buf, "---------------\n");
	if (dom == NULL)
		appendStringInfoString(&buf, "(no non-observer xmin holders)\n\n");
	else
	{
		appendStringInfo(&buf, "type: %s  pid: %d  slot: %s\n",
						 horizon_blocker_type_name(dom->type),
						 dom->pid,
						 dom->slot_name[0] ? dom->slot_name : "-");
		appendStringInfo(&buf, "%s\n", dom->reason);
		appendStringInfo(&buf, "recommended:\n%s\n\n", dom->recommended_sql);
	}

	appendStringInfoString(&buf, "Oldest holders\n");
	appendStringInfoString(&buf, "--------------\n");
	{
		bool	   *used;
		int			nslots;

		nslots = Max(snap->nblockers, 1);
		used = (bool *) palloc0(sizeof(bool) * nslots);

		while (shown < 15)
		{
			int			best = -1;
			int64		best_age = -1;
			HorizonBlocker *b;
			TransactionId hold;
			int64		age;

			for (i = 0; i < snap->nblockers; i++)
			{
				if (used[i] || snap->blockers[i].is_observer)
					continue;
				age = horizon_blocker_age(&snap->blockers[i], snap->next_xid);
				if (best < 0 || age > best_age)
				{
					best = i;
					best_age = age;
				}
			}
			if (best < 0)
				break;

			used[best] = true;
			b = &snap->blockers[best];
			hold = TransactionIdOlder(b->xmin, b->xid);
			age = horizon_xid_age(hold, snap->next_xid);
			appendStringInfo(&buf, "  %-18s pid=%-6d age=%-10lld holder=%s state=%s %s\n",
							 horizon_blocker_type_name(b->type),
							 b->pid,
							 (long long) age,
							 b->is_horizon_holder ? "yes" : "no",
							 b->state,
							 b->slot_name[0] ? b->slot_name : b->application_name);
			shown++;
		}
		pfree(used);
	}
	if (shown == 0)
		appendStringInfoString(&buf, "  none\n");

	appendStringInfoString(&buf,
						   "\nDo not VACUUM FULL. Do not raise autovacuum_freeze_max_age to hide this.\n"
						   "Remove the xmin holder, then let VACUUM (FREEZE) run.\n");

	horizon_snapshot_free(snap);
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

Datum
pg_horizon_check(PG_FUNCTION_ARGS)
{
	HorizonSnapshot *snap;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	Datum		values[6];
	bool		nulls[6];
	HorizonSeverity sev;
	int			code;
	HorizonBlocker *dom;
	int			critical_blockers = 0;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));
	tupdesc = BlessTupleDesc(tupdesc);

	snap = horizon_snapshot_collect();
	sev = horizon_compute_severity(snap);
	switch (sev)
	{
		case HORIZON_SEV_OK:
			code = 0;
			break;
		case HORIZON_SEV_WARNING:
			code = 1;
			break;
		case HORIZON_SEV_CRITICAL:
			code = 2;
			break;
		case HORIZON_SEV_EMERGENCY:
			code = 2;
			break;
		default:
			code = 0;
			break;
	}

	for (i = 0; i < snap->nblockers; i++)
	{
		if (snap->blockers[i].is_horizon_holder && !snap->blockers[i].is_observer)
			critical_blockers++;
	}

	dom = horizon_dominant_blocker(snap);

	memset(nulls, 0, sizeof(nulls));
	values[0] = CStringGetTextDatum(horizon_severity_name(sev));
	values[1] = Int32GetDatum(code);
	values[2] = Int64GetDatum(horizon_xid_age(snap->oldest_xid, snap->next_xid));
	values[3] = Int64GetDatum(horizon_xid_remaining(snap->next_xid, snap->xid_stop_limit));
	values[4] = Int32GetDatum(critical_blockers);
	if (dom)
		values[5] = CStringGetTextDatum(dom->reason);
	else
		values[5] = CStringGetTextDatum("no dominant xmin holder");

	tuple = heap_form_tuple(tupdesc, values, nulls);
	horizon_snapshot_free(snap);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
pg_horizon_vacuum_sql(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	char	   *nsp;
	char		sql[256];

	rel = relation_open(relid, AccessShareLock);
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
	{
		char	   *badname = pstrdup(RelationGetRelationName(rel));

		relation_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a heap that VACUUM can freeze",
						badname)));
	}
	nsp = get_namespace_name(rel->rd_rel->relnamespace);
	if (nsp == NULL)
	{
		relation_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("could not find schema name for relation OID %u", relid)));
	}
	snprintf(sql, sizeof(sql),
			 "VACUUM (FREEZE, INDEX_CLEANUP ON, PROCESS_TOAST) %s.%s;",
			 quote_identifier(nsp),
			 quote_identifier(RelationGetRelationName(rel)));
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(sql));
}

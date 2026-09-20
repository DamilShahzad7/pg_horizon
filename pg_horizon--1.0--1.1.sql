/*-------------------------------------------------------------------------
 *
 * pg_horizon--1.0--1.1.sql
 *	  Upgrade pg_horizon from 1.0 to 1.1
 *
 * What changes at the SQL level:
 *	 pg_horizon_explain()	three OUT columns appended (relminmxid, mxid_age,
 *							mxid_horizon). A function's result row type cannot
 *							be altered, so it is dropped and recreated; nothing
 *							in the extension depends on it.
 *	 pg_horizon				two columns appended (mxid_freeze_max_age,
 *							mxid_failsafe_age)
 *	 pg_horizon_blockers	one column appended (horizon_age)
 *
 * The views are replaced in place (CREATE OR REPLACE VIEW, appended columns
 * only) so objects that users built on top of them keep working.
 *
 * Behaviour changes that need no SQL (the C library implements them): the
 * xmin horizons exclude the calling session, MultiXact age is the real
 * wraparound frontier, xmin_age is NULL rather than 0 when a holder has no
 * xid/xmin, and blocker_count honours pg_horizon.min_xmin_age. See CHANGELOG.
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 *-------------------------------------------------------------------------
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_horizon UPDATE TO '1.1'" to load this file. \quit

DROP FUNCTION pg_horizon_explain(regclass);

CREATE FUNCTION pg_horizon_explain(
    rel regclass,
    OUT relation oid,
    OUT summary text,
    OUT relfrozenxid xid,
    OUT xid_age bigint,
    OUT relation_xmin xid,
    OUT relation_xmin_age bigint,
    OUT freeze_limit xid,
    OUT freeze_constraint text,
    OUT vacuum_running boolean,
    OUT live_tuples bigint,
    OUT dead_tuples bigint,
    OUT diagnosis text,
    OUT vacuum_sql text,
    OUT dominant_blocker text,
    OUT relminmxid xid,
    OUT mxid_age bigint,
    OUT mxid_horizon xid
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_horizon_explain'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

GRANT EXECUTE ON FUNCTION pg_horizon_explain(regclass) TO PUBLIC;

CREATE OR REPLACE VIEW pg_horizon AS
    SELECT s.*,
           current_setting('autovacuum_multixact_freeze_max_age')::bigint AS mxid_freeze_max_age,
           current_setting('vacuum_multixact_failsafe_age')::bigint AS mxid_failsafe_age
    FROM pg_horizon_status() s;

CREATE OR REPLACE VIEW pg_horizon_blockers AS
    SELECT *,
           now() - xact_start AS xact_age,
           GREATEST(xmin_age, catalog_xmin_age) AS horizon_age
    FROM pg_horizon_blockers(false);

COMMENT ON VIEW pg_horizon IS
    'Cluster XID/MultiXact wraparound state, the horizons a VACUUM would see (this session excluded), and severity';
COMMENT ON VIEW pg_horizon_blockers IS
    'Backends, prepared transactions, replication slots and hot_standby_feedback walsenders that hold an xid/xmin; excludes the calling backend';
COMMENT ON VIEW pg_horizon_databases IS
    'Per-database datfrozenxid/datminmxid age and wraparound state';
COMMENT ON VIEW pg_horizon_relations IS
    'Per-relation freeze age and freeze_constraint (ok, horizon, vacuum_lag)';
COMMENT ON VIEW pg_horizon_check IS
    'One monitoring row: status, code (0 ok, 1 warning, 2 critical or emergency), and why';
COMMENT ON FUNCTION pg_horizon_explain(regclass) IS
    'Why one table can or cannot be frozen: horizon it is subject to, freeze constraint, diagnosis, VACUUM statement, dominant blocker';
COMMENT ON FUNCTION pg_horizon_vacuum_sql(regclass) IS
    'VACUUM (FREEZE, VERBOSE) statement for one table, materialized view or TOAST table; never executed';
COMMENT ON FUNCTION pg_horizon_terminate_blocker(integer, boolean) IS
    'Terminate an idle-in-transaction xmin holder; needs pg_horizon.terminate_blockers = on';

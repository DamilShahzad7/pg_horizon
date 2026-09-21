/*-------------------------------------------------------------------------
 *
 * pg_horizon--1.1.sql
 *	  SQL objects for pg_horizon 1.1
 *
 * Keep this file and pg_horizon--1.0--1.1.sql in step: the 004_upgrade
 * regression test compares a fresh 1.1 install with 1.0 updated to 1.1.
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 *-------------------------------------------------------------------------
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_horizon" to load this file. \quit

CREATE FUNCTION pg_horizon_status(
    OUT next_xid xid,
    OUT oldest_xid xid,
    OUT oldest_xid_db oid,
    OUT oldest_xid_db_name text,
    OUT data_xmin xid,
    OUT shared_xmin xid,
    OUT catalog_xmin xid,
    OUT oldest_running_xid xid,
    OUT slot_xmin xid,
    OUT slot_catalog_xmin xid,
    OUT next_mxid xid,
    OUT oldest_mxid xid,
    OUT xid_age bigint,
    OUT mxid_age bigint,
    OUT freeze_max_age bigint,
    OUT failsafe_age bigint,
    OUT xid_headroom bigint,
    OUT wraparound_pct double precision,
    OUT severity text,
    OUT in_recovery boolean,
    OUT autovacuum_max_workers integer,
    OUT blocker_count integer
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_horizon_status'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_blockers(
    include_observer boolean DEFAULT false,
    OUT blocker_type text,
    OUT pid integer,
    OUT slot_name text,
    OUT prepared_gid text,
    OUT datid oid,
    OUT datname text,
    OUT usename text,
    OUT application_name text,
    OUT backend_type text,
    OUT state text,
    OUT xid xid,
    OUT xmin xid,
    OUT xmin_age bigint,
    OUT catalog_xmin xid,
    OUT catalog_xmin_age bigint,
    OUT xact_start timestamptz,
    OUT wait_event_type text,
    OUT wait_event text,
    OUT query text,
    OUT holds_locks boolean,
    OUT waiting_pids integer[],
    OUT wal_retained_bytes bigint,
    OUT affects_data_horizon boolean,
    OUT affects_catalog_horizon boolean,
    OUT is_idle_in_transaction boolean,
    OUT safe_to_terminate boolean,
    OUT recommended_sql text,
    OUT reason text,
    OUT is_horizon_holder boolean,
    OUT plugin text,
    OUT slot_active boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_horizon_blockers'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_databases(
    OUT datid oid,
    OUT datname name,
    OUT datfrozenxid xid,
    OUT xid_age bigint,
    OUT datminmxid xid,
    OUT mxid_age bigint,
    OUT is_oldest boolean,
    OUT freeze_constraint text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_horizon_databases'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_relations(
    min_age bigint DEFAULT 0,
    OUT relid oid,
    OUT relnamespace oid,
    OUT relname name,
    OUT relkind text,
    OUT relfrozenxid xid,
    OUT xid_age bigint,
    OUT relminmxid xid,
    OUT mxid_age bigint,
    OUT relpages integer,
    OUT relallvisible integer,
    OUT live_tuples bigint,
    OUT dead_tuples bigint,
    OUT last_vacuum timestamptz,
    OUT last_autovacuum timestamptz,
    OUT freeze_constraint text,
    OUT relisshared boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_horizon_relations'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

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
    OUT dominant_blocker text
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_horizon_explain'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_report()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_horizon_report'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_check(
    OUT status text,
    OUT code integer,
    OUT oldest_xid_age bigint,
    OUT xid_headroom bigint,
    OUT horizon_holders integer,
    OUT message text
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_horizon_check'
LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_vacuum_sql(rel regclass)
RETURNS text
AS 'MODULE_PATHNAME', 'pg_horizon_vacuum_sql'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;

CREATE FUNCTION pg_horizon_terminate_blocker(pid integer, force boolean DEFAULT false)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_horizon_terminate_blocker'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

CREATE VIEW pg_horizon AS
    SELECT s.*,
           current_setting('autovacuum_multixact_freeze_max_age')::bigint AS mxid_freeze_max_age,
           current_setting('vacuum_multixact_failsafe_age')::bigint AS mxid_failsafe_age
    FROM pg_horizon_status() s;

CREATE VIEW pg_horizon_blockers AS
    SELECT *,
           now() - xact_start AS xact_age,
           GREATEST(xmin_age, catalog_xmin_age) AS horizon_age
    FROM pg_horizon_blockers(false);

CREATE VIEW pg_horizon_databases AS
    SELECT * FROM pg_horizon_databases();

CREATE VIEW pg_horizon_relations AS
    SELECT
        r.*,
        n.nspname AS schemaname
    FROM pg_horizon_relations(0) r
    LEFT JOIN pg_namespace n ON n.oid = r.relnamespace;

CREATE VIEW pg_horizon_check AS
    SELECT * FROM pg_horizon_check();

GRANT SELECT ON pg_horizon TO PUBLIC;
GRANT SELECT ON pg_horizon_blockers TO PUBLIC;
GRANT SELECT ON pg_horizon_databases TO PUBLIC;
GRANT SELECT ON pg_horizon_relations TO PUBLIC;
GRANT SELECT ON pg_horizon_check TO PUBLIC;

GRANT EXECUTE ON FUNCTION pg_horizon_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_blockers(boolean) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_databases() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_relations(bigint) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_explain(regclass) TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_report() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_check() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pg_horizon_vacuum_sql(regclass) TO PUBLIC;

REVOKE ALL ON FUNCTION pg_horizon_terminate_blocker(integer, boolean) FROM PUBLIC;

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

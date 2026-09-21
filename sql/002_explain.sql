\set VERBOSITY terse

CREATE TABLE horizon_explain_demo (id int);
INSERT INTO horizon_explain_demo SELECT generate_series(1, 20);
ANALYZE horizon_explain_demo;

SELECT freeze_constraint = 'ok' AS new_table_ok
FROM pg_horizon_explain('horizon_explain_demo');

SELECT freeze_constraint IN ('ok', 'horizon', 'vacuum_lag') AS constraint_known
FROM pg_horizon_explain('horizon_explain_demo');

SELECT diagnosis IS NOT NULL AND vacuum_sql LIKE 'VACUUM (FREEZE%' AS explain_complete
FROM pg_horizon_explain('horizon_explain_demo');

SELECT summary LIKE 'relation public.horizon_explain_demo relkind=r horizon=data %' AS summary_shape,
       vacuum_sql = 'VACUUM (FREEZE, VERBOSE) public.horizon_explain_demo;' AS vacuum_sql_exact,
       summary ~ ' relminmxid age=[0-9]+ mxid_horizon=[0-9]+ ' AS multixact_in_summary,
       freeze_limit IS NOT NULL AS freeze_limit_present
FROM pg_horizon_explain('horizon_explain_demo');

SELECT pg_horizon_vacuum_sql('horizon_explain_demo') = 'VACUUM (FREEZE, VERBOSE) public.horizon_explain_demo;'
    AS vacuum_sql_shape;

SELECT count(*) = 1 AS relation_listed
FROM pg_horizon_relations
WHERE relname = 'horizon_explain_demo';

-- each relation kind is subject to the horizon core would use for it
SELECT summary LIKE '% horizon=catalog %' AS catalog_table_uses_catalog_horizon
FROM pg_horizon_explain('pg_class');
SELECT summary LIKE '% horizon=shared %' AS shared_catalog_uses_shared_horizon
FROM pg_horizon_explain('pg_database');
CREATE TEMP TABLE horizon_temp_demo (a int);
SELECT summary LIKE '% horizon=temporary %' AS own_temp_table_uses_temp_horizon
FROM pg_horizon_explain('horizon_temp_demo');

-- TOAST tables and materialized views are supported
CREATE TABLE horizon_toast_demo (id int, payload text);
SELECT freeze_constraint IN ('ok', 'horizon', 'vacuum_lag') AS toast_table_ok
FROM pg_horizon_explain((SELECT reltoastrelid FROM pg_class WHERE oid = 'horizon_toast_demo'::regclass));
SELECT pg_horizon_vacuum_sql((SELECT reltoastrelid FROM pg_class WHERE oid = 'horizon_toast_demo'::regclass))
       LIKE 'VACUUM (FREEZE, VERBOSE) pg_toast.pg_toast_%;' AS toast_vacuum_sql;
CREATE MATERIALIZED VIEW horizon_mv_demo AS SELECT 1 AS a;
SELECT summary LIKE '% relkind=m %' AS matview_ok FROM pg_horizon_explain('horizon_mv_demo');

-- errors are ordinary, informative errors
CREATE VIEW horizon_view_demo AS SELECT 1 AS a;
CREATE INDEX horizon_idx_demo ON horizon_explain_demo (id);
CREATE SEQUENCE horizon_seq_demo;
CREATE TABLE horizon_part_demo (id int) PARTITION BY RANGE (id);
SELECT pg_horizon_explain('pg_horizon'::regclass);
SELECT pg_horizon_explain('horizon_view_demo');
SELECT pg_horizon_explain('horizon_idx_demo');
SELECT pg_horizon_explain('horizon_seq_demo');
SELECT pg_horizon_explain('horizon_part_demo');
SELECT pg_horizon_vacuum_sql('horizon_part_demo');
SELECT pg_horizon_explain(99999999::oid::regclass);
SELECT pg_horizon_vacuum_sql(99999999::oid::regclass);
SELECT pg_horizon_explain(NULL) IS NULL AS strict_explain;
SELECT pg_horizon_vacuum_sql(NULL) IS NULL AS strict_vacuum_sql;

-- identifiers are quoted, never truncated
CREATE TABLE "Mixed Case ""Quoted""" (id int);
SELECT pg_horizon_vacuum_sql('"Mixed Case ""Quoted"""') AS quoted;
DO $$
BEGIN
	-- 63 characters, every one a double quote: the worst case for quote_identifier()
	EXECUTE format('CREATE SCHEMA %I', repeat('"', 63));
	EXECUTE format('CREATE TABLE %I.%I (id int)', repeat('"', 63), repeat('"', 63));
END $$;
SELECT length(s) = 283 AND right(s, 1) = ';' AS longest_statement_is_complete
FROM (SELECT pg_horizon_vacuum_sql(format('%I.%I', repeat('"', 63), repeat('"', 63))::regclass) AS s) x;
SELECT vacuum_sql = pg_horizon_vacuum_sql(format('%I.%I', repeat('"', 63), repeat('"', 63))::regclass) AS explain_matches
FROM pg_horizon_explain(format('%I.%I', repeat('"', 63), repeat('"', 63))::regclass);

SET client_min_messages = warning;
DO $$
BEGIN
	EXECUTE format('DROP SCHEMA %I CASCADE', repeat('"', 63));
END $$;
DROP TABLE horizon_explain_demo, horizon_toast_demo, horizon_part_demo, "Mixed Case ""Quoted""" CASCADE;
DROP MATERIALIZED VIEW horizon_mv_demo;
DROP VIEW horizon_view_demo;
DROP SEQUENCE horizon_seq_demo;

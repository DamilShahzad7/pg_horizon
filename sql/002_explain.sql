CREATE TABLE horizon_explain_demo (id int);
INSERT INTO horizon_explain_demo SELECT generate_series(1, 20);
ANALYZE horizon_explain_demo;

SELECT freeze_constraint = 'ok' AS new_table_ok
FROM pg_horizon_explain('horizon_explain_demo');

SELECT freeze_constraint IN ('ok', 'horizon', 'vacuum_lag') AS constraint_known
FROM pg_horizon_explain('horizon_explain_demo');

SELECT diagnosis IS NOT NULL AND vacuum_sql LIKE 'VACUUM (FREEZE%' AS explain_complete
FROM pg_horizon_explain('horizon_explain_demo');

SELECT pg_horizon_vacuum_sql('horizon_explain_demo') LIKE 'VACUUM (FREEZE, INDEX_CLEANUP ON, PROCESS_TOAST)%'
    AS vacuum_sql_shape;

SELECT count(*) = 1 AS relation_listed
FROM pg_horizon_relations
WHERE relname = 'horizon_explain_demo';

SELECT pg_horizon_explain('pg_horizon'::regclass);

DROP TABLE horizon_explain_demo;

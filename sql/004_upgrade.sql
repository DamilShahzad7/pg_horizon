-- 1.0 -> 1.1 must land in exactly the state a fresh 1.1 install has, must keep
-- user objects built on the views, and the 1.1 library must work under the 1.0
-- SQL definitions (the library is installed before ALTER EXTENSION UPDATE runs).
\set VERBOSITY terse
DROP EXTENSION pg_horizon;
CREATE EXTENSION pg_horizon VERSION '1.0';
SELECT extversion FROM pg_extension WHERE extname = 'pg_horizon';

-- objects that depend on the 1.0 views
CREATE VIEW horizon_user_blockers AS SELECT pid, xmin_age, xact_age FROM pg_horizon_blockers;
CREATE VIEW horizon_user_status AS SELECT severity, blocker_count FROM pg_horizon;

-- 1.0 shapes, served by the current library
SELECT count(*) AS status_columns_1_0
FROM pg_attribute WHERE attrelid = 'pg_horizon'::regclass AND attnum > 0;
SELECT count(*) AS explain_columns_1_0
FROM pg_proc p, unnest(p.proargmodes) m
WHERE p.oid = 'pg_horizon_explain(regclass)'::regprocedure AND m IN ('o', 't');
SELECT severity IS NOT NULL AS status_works_under_1_0 FROM pg_horizon;
SELECT count(*) >= 0 AS blockers_work_under_1_0 FROM pg_horizon_blockers;
SELECT freeze_constraint IS NOT NULL AS explain_works_under_1_0 FROM pg_horizon_explain('pg_class');
SELECT status IS NOT NULL AS check_works_under_1_0 FROM pg_horizon_check;
SELECT count(*) >= 1 AS relations_work_under_1_0 FROM pg_horizon_relations;
SELECT pg_horizon_report() IS NOT NULL AS report_works_under_1_0;

ALTER EXTENSION pg_horizon UPDATE TO '1.1';
SELECT extversion FROM pg_extension WHERE extname = 'pg_horizon';

SELECT count(*) AS status_columns_1_1
FROM pg_attribute WHERE attrelid = 'pg_horizon'::regclass AND attnum > 0;
SELECT count(*) AS explain_columns_1_1
FROM pg_proc p, unnest(p.proargmodes) m
WHERE p.oid = 'pg_horizon_explain(regclass)'::regprocedure AND m IN ('o', 't');
SELECT count(*) >= 0 AS user_blockers_view_survives FROM horizon_user_blockers;
SELECT severity IS NOT NULL AS user_status_view_survives FROM horizon_user_status;
SELECT relminmxid IS NOT NULL AS new_explain_columns FROM pg_horizon_explain('pg_class');
SELECT mxid_freeze_max_age > 0 AS new_status_columns FROM pg_horizon;

-- what a catalog comparison sees
CREATE TEMP TABLE horizon_sig_upgraded AS
SELECT 'function' AS kind, p.proname::text AS name,
       pg_get_function_identity_arguments(p.oid) AS args,
       pg_get_function_result(p.oid) AS result,
       concat_ws('/', p.provolatile, p.proparallel, p.proisstrict, p.prosecdef, p.prosrc, p.probin,
                 p.proacl::text, obj_description(p.oid, 'pg_proc')) AS detail
FROM pg_proc p
JOIN pg_depend d ON d.objid = p.oid AND d.classid = 'pg_proc'::regclass AND d.deptype = 'e'
WHERE d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_horizon')
UNION ALL
SELECT 'view', c.relname::text, '',
       (SELECT string_agg(a.attname || ':' || format_type(a.atttypid, a.atttypmod), ',' ORDER BY a.attnum)
        FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped),
       concat_ws('/', pg_get_viewdef(c.oid), c.relacl::text, obj_description(c.oid, 'pg_class'))
FROM pg_class c
JOIN pg_depend d ON d.objid = c.oid AND d.classid = 'pg_class'::regclass AND d.deptype = 'e'
WHERE d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_horizon')
  AND c.relkind = 'v';

DROP VIEW horizon_user_blockers, horizon_user_status;
DROP EXTENSION pg_horizon;
CREATE EXTENSION pg_horizon;
SELECT extversion FROM pg_extension WHERE extname = 'pg_horizon';

CREATE TEMP TABLE horizon_sig_fresh AS
SELECT 'function' AS kind, p.proname::text AS name,
       pg_get_function_identity_arguments(p.oid) AS args,
       pg_get_function_result(p.oid) AS result,
       concat_ws('/', p.provolatile, p.proparallel, p.proisstrict, p.prosecdef, p.prosrc, p.probin,
                 p.proacl::text, obj_description(p.oid, 'pg_proc')) AS detail
FROM pg_proc p
JOIN pg_depend d ON d.objid = p.oid AND d.classid = 'pg_proc'::regclass AND d.deptype = 'e'
WHERE d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_horizon')
UNION ALL
SELECT 'view', c.relname::text, '',
       (SELECT string_agg(a.attname || ':' || format_type(a.atttypid, a.atttypmod), ',' ORDER BY a.attnum)
        FROM pg_attribute a WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped),
       concat_ws('/', pg_get_viewdef(c.oid), c.relacl::text, obj_description(c.oid, 'pg_class'))
FROM pg_class c
JOIN pg_depend d ON d.objid = c.oid AND d.classid = 'pg_class'::regclass AND d.deptype = 'e'
WHERE d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_horizon')
  AND c.relkind = 'v';

SELECT count(*) > 0 AS signatures_recorded FROM horizon_sig_fresh;
SELECT count(*) AS only_in_upgraded FROM (SELECT * FROM horizon_sig_upgraded EXCEPT SELECT * FROM horizon_sig_fresh) x;
SELECT count(*) AS only_in_fresh FROM (SELECT * FROM horizon_sig_fresh EXCEPT SELECT * FROM horizon_sig_upgraded) x;

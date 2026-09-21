CREATE EXTENSION pg_horizon;

SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_horizon';

SELECT severity IN ('ok', 'warning', 'critical', 'emergency') AS severity_known
FROM pg_horizon;

SELECT wraparound_pct >= 0 AND wraparound_pct <= 100 AS wraparound_pct_ok
FROM pg_horizon;

SELECT xid_headroom > 0 AS xid_headroom_positive
FROM pg_horizon;

SELECT freeze_max_age > 0 AND failsafe_age > 0
       AND mxid_freeze_max_age > 0 AND mxid_failsafe_age > 0 AS limits_match_core
FROM pg_horizon;

SELECT pg_horizon_report() LIKE 'pg_horizon diagnostic report%' AS report_header_ok;

SELECT status IN ('ok', 'warning', 'critical', 'emergency') AS check_status_ok,
       code IN (0, 1, 2) AS check_code_ok
FROM pg_horizon_check;

SELECT count(*) >= 1 AS has_databases
FROM pg_horizon_databases;

SELECT count(*) FILTER (WHERE is_oldest) = 1 AS one_oldest_database
FROM pg_horizon_databases;

SELECT bool_and(freeze_constraint IN ('ok', 'aging', 'aggressive_autovacuum', 'failsafe')) AS database_states_known
FROM pg_horizon_databases;

SELECT data_xmin IS NOT NULL
       AND shared_xmin IS NOT NULL
       AND catalog_xmin IS NOT NULL AS xmin_horizons_present
FROM pg_horizon;

-- horizons are ordered: shared <= catalog <= data (older or equal)
SELECT (shared_xmin::text::bigint <= catalog_xmin::text::bigint
        AND catalog_xmin::text::bigint <= data_xmin::text::bigint) AS horizons_ordered
FROM pg_horizon;

-- the MultiXact figure is the real wraparound frontier, never younger than the
-- oldest datminmxid (small slack for multixacts created between the two reads)
SELECT h.mxid_age >= (SELECT max(mxid_age(datminmxid)) FROM pg_database) - 1000 AS mxid_age_is_frontier
FROM pg_horizon h;

SELECT count(*) FILTER (WHERE relname = 'pg_proc') = 1
       AND count(*) FILTER (WHERE relname = 'pg_class') = 1 AS catalogs_listed
FROM pg_horizon_relations;

SELECT freeze_constraint = 'ok' AS healthy_catalog_ok
FROM pg_horizon_relations
WHERE relname = 'pg_class';

SELECT bool_and(freeze_constraint IN ('ok', 'horizon', 'vacuum_lag')) AS relation_states_known
FROM pg_horizon_relations;

-- the observing backend is excluded by default and, when asked for, is
-- never reported as a horizon holder: horizons are what a VACUUM elsewhere sees
SELECT count(*) = 0 AS observer_hidden
FROM pg_horizon_blockers WHERE pid = pg_backend_pid();

SELECT count(*) = 1 AS observer_listed_on_request
FROM pg_horizon_blockers(true) WHERE pid = pg_backend_pid() AND NOT is_horizon_holder;

-- a holder that has neither xid nor xmin (a logical slot) has NULL, not 0, ages
SELECT count(*) = 0 AS no_zero_age_for_missing_xmin
FROM pg_horizon_blockers(true)
WHERE xid IS NULL AND xmin IS NULL AND xmin_age IS NOT NULL;

SELECT count(*) = 0 AS horizon_age_is_the_oldest
FROM pg_horizon_blockers
WHERE horizon_age IS DISTINCT FROM GREATEST(xmin_age, catalog_xmin_age);

-- pg_horizon.min_xmin_age is honoured by the view and by the counts derived from it
SET pg_horizon.min_xmin_age = 2147483647;
SELECT (SELECT count(*) FROM pg_horizon_blockers(true)) AS listed,
       (SELECT blocker_count FROM pg_horizon) AS blocker_count,
       (SELECT horizon_holders FROM pg_horizon_check) AS horizon_holders;
RESET pg_horizon.min_xmin_age;

-- documentation is installed
SELECT obj_description('pg_horizon'::regclass, 'pg_class') IS NOT NULL AS view_commented,
       obj_description('pg_horizon_explain(regclass)'::regprocedure, 'pg_proc') IS NOT NULL AS function_commented;

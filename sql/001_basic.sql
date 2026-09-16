CREATE EXTENSION pg_horizon;

SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_horizon';

SELECT severity IN ('ok', 'warning', 'critical', 'emergency') AS severity_known
FROM pg_horizon;

SELECT wraparound_pct >= 0 AND wraparound_pct <= 100 AS wraparound_pct_ok
FROM pg_horizon;

SELECT xid_headroom > 0 AS xid_headroom_positive
FROM pg_horizon;

SELECT freeze_max_age > 0 AND failsafe_age > 0 AS limits_match_core
FROM pg_horizon;

SELECT pg_horizon_report() LIKE 'pg_horizon diagnostic report%' AS report_header_ok;

SELECT status IN ('ok', 'warning', 'critical', 'emergency') AS check_status_ok,
       code IN (0, 1, 2) AS check_code_ok
FROM pg_horizon_check;

SELECT count(*) >= 1 AS has_databases
FROM pg_horizon_databases;

SELECT count(*) FILTER (WHERE is_oldest) = 1 AS one_oldest_database
FROM pg_horizon_databases;

SELECT data_xmin IS NOT NULL
       AND shared_xmin IS NOT NULL
       AND catalog_xmin IS NOT NULL AS xmin_horizons_present
FROM pg_horizon;

SELECT count(*) FILTER (WHERE relname = 'pg_proc') = 1
       AND count(*) FILTER (WHERE relname = 'pg_class') = 1 AS catalogs_listed
FROM pg_horizon_relations;

SELECT freeze_constraint = 'ok' AS healthy_catalog_ok
FROM pg_horizon_relations
WHERE relname = 'pg_class';

\set VERBOSITY terse

-- the report has every section and says what it measures
SELECT pg_horizon_report() LIKE '%Horizons a VACUUM started now would see (this session excluded)%' AS report_says_what_it_measures,
       pg_horizon_report() LIKE '%Dominant holder%' AS report_has_dominant_section,
       pg_horizon_report() LIKE '%Oldest holders%' AS report_has_holders_section,
       pg_horizon_report() LIKE '%vacuum_multixact_failsafe_age%' AS report_has_multixact_limits;

-- at status ok the check message is calm and says why
SELECT status <> 'ok' OR message LIKE 'no wraparound pressure and no stale horizon holder%' AS ok_message_is_calm
FROM pg_horizon_check;

-- pg_horizon_relations(min_age): a huge threshold hides everything
SELECT count(*) AS relations_over_max_age FROM pg_horizon_relations(2147483647);
SELECT count(*) > 0 AS relations_with_zero_min FROM pg_horizon_relations(0);
SELECT count(*) > 0 AS relations_with_null_min FROM pg_horizon_relations(NULL);

-- include_observer NULL behaves like false
SELECT count(*) = 0 AS null_observer_means_false FROM pg_horizon_blockers(NULL) WHERE pid = pg_backend_pid();

-- direct function calls, not only the views
SELECT count(*) = 1 AS status_function FROM pg_horizon_status();
SELECT count(*) >= 1 AS databases_function FROM pg_horizon_databases();
SELECT status IS NOT NULL AS check_function FROM pg_horizon_check();

-- several calls in one transaction stay consistent and do not accumulate state
BEGIN;
SELECT count(*) FROM pg_horizon_blockers(true) WHERE pid = pg_backend_pid();
SELECT count(*) FROM pg_horizon_blockers(true) WHERE pid = pg_backend_pid();
SELECT severity IS NOT NULL FROM pg_horizon;
COMMIT;

-- lots of calls in one statement (memory contexts, SPI, lock handling)
-- (the argument depends on g, so the function really runs once per row)
SELECT count(*) = 300 AS repeated_status_calls
FROM (SELECT (pg_horizon_status()).blocker_count FROM generate_series(1, 300)) s;
SELECT count(DISTINCT g) = 100 AS repeated_blockers_calls
FROM generate_series(1, 100) g, LATERAL (SELECT count(*) FROM pg_horizon_blockers(g % 2 = 0)) s;

-- horizons are exposed as xids and are comparable with core values
SELECT next_xid::text::bigint >= data_xmin::text::bigint AS next_is_not_behind_data_horizon
FROM pg_horizon;
SELECT oldest_running_xid::text::bigint <= data_xmin::text::bigint AS running_is_not_ahead_of_horizon
FROM pg_horizon;

-- wraparound_pct never exceeds 100 and headroom is never negative
SELECT wraparound_pct BETWEEN 0 AND 100 AND xid_headroom >= 0 AS bounds_ok FROM pg_horizon;

-- pg_horizon.min_xmin_age is a plain integer GUC with sane bounds
SET pg_horizon.min_xmin_age = -1;
SET pg_horizon.min_xmin_age = 2147483648;
SHOW pg_horizon.min_xmin_age;
SELECT name, context FROM pg_settings WHERE name LIKE 'pg_horizon.%' ORDER BY name;

-- the prefix is reserved: unknown pg_horizon.* settings are rejected
SET pg_horizon.no_such_setting = 1;

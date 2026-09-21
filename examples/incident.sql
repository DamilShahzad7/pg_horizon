-- Incident scratchpad. Run as a superuser or pg_read_all_stats, from a
-- separate, short-lived session (the session that runs pg_horizon is left out
-- of the horizons it reports).
CREATE EXTENSION IF NOT EXISTS pg_horizon;

\echo === cluster ===
SELECT severity, xid_age, xid_headroom, wraparound_pct, mxid_age,
       oldest_xid_db_name, in_recovery, blocker_count
FROM pg_horizon;

\echo === why ===
SELECT status, code, message FROM pg_horizon_check;

\echo === databases ===
SELECT datname, xid_age, mxid_age, is_oldest, freeze_constraint
FROM pg_horizon_databases
ORDER BY greatest(xid_age, mxid_age) DESC;

\echo === holders (oldest first; logical slots included) ===
SELECT blocker_type, pid, datname, slot_name, state, horizon_age,
       is_horizon_holder, affects_data_horizon, is_idle_in_transaction,
       safe_to_terminate, left(reason, 160)
FROM pg_horizon_blockers
ORDER BY horizon_age DESC NULLS LAST
LIMIT 20;

\echo === freeze debt ===
SELECT schemaname, relname, xid_age, mxid_age, freeze_constraint, dead_tuples
FROM pg_horizon_relations
WHERE freeze_constraint <> 'ok'
ORDER BY greatest(xid_age, mxid_age) DESC
LIMIT 20;

\echo === report ===
SELECT pg_horizon_report();

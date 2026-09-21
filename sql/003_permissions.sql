-- terminate is denied until the GUC is enabled, and never kills this backend
\set VERBOSITY sqlstate
SELECT pg_horizon_terminate_blocker(pg_backend_pid());

SELECT pg_horizon_terminate_blocker(pg_backend_pid(), true);

SELECT pg_horizon_terminate_blocker(NULL);

\set VERBOSITY terse
-- with the switch on, the current backend and non-holders are still refused
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(pg_backend_pid());
SELECT pg_horizon_terminate_blocker(0);
SELECT pg_horizon_terminate_blocker(2147483647);
RESET pg_horizon.terminate_blockers;

-- everything except terminate is executable by PUBLIC
SELECT p.proname,
       has_function_privilege('public', p.oid, 'execute') AS public_can_execute
FROM pg_proc p
JOIN pg_depend d ON d.objid = p.oid AND d.classid = 'pg_proc'::regclass AND d.deptype = 'e'
WHERE d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_horizon')
ORDER BY p.proname;

-- an unprivileged role can use the diagnostics, but cannot terminate or enable terminate
CREATE ROLE horizon_plain NOLOGIN;
GRANT ALL ON SCHEMA public TO horizon_plain;
SET ROLE horizon_plain;
SELECT count(*) >= 0 AS views_readable FROM pg_horizon_blockers;
SELECT severity IS NOT NULL AS status_readable FROM pg_horizon;
SELECT status IS NOT NULL AS check_readable FROM pg_horizon_check;
SELECT count(*) >= 1 AS databases_readable FROM pg_horizon_databases;
SELECT count(*) >= 1 AS relations_readable FROM pg_horizon_relations;
SELECT pg_horizon_report() IS NOT NULL AS report_readable;
SELECT pg_horizon_terminate_blocker(2147483647);
SET pg_horizon.terminate_blockers = on;
RESET ROLE;
DROP OWNED BY horizon_plain;
DROP ROLE horizon_plain;

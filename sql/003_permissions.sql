-- terminate is denied until the GUC is enabled, and never kills this backend
\set VERBOSITY sqlstate
SELECT pg_horizon_terminate_blocker(pg_backend_pid());

SELECT pg_horizon_terminate_blocker(pg_backend_pid(), true);

SELECT pg_horizon_terminate_blocker(NULL);

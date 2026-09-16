# Security

pg_horizon is a diagnostic loaded into the PostgreSQL backend.

- It does not open network sockets or write outside PostgreSQL data directories.
- `pg_horizon_terminate_blocker` sends `SIGTERM` only after `pg_horizon.terminate_blockers` is on, the pid is an xmin holder, and the caller passes the same privilege checks as `pg_terminate_backend`. Slots and prepared transactions are refused.
- Non-superusers without `pg_read_all_stats` see query text only for their own role, matching `pg_stat_activity`.
- Do not run untrusted extension builds as a superuser. Load only artifacts you compiled or obtained from a channel you trust.

Report vulnerabilities privately to the repository security contact. Do not file public issues that include a working kill-path bypass.

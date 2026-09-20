# Security

pg_horizon is a diagnostic loaded into the PostgreSQL backend.

- It does not open network sockets or write outside PostgreSQL data directories.
- `pg_horizon_terminate_blocker` sends `SIGTERM` only after `pg_horizon.terminate_blockers` is on, the pid is an xmin holder in a state that is re-checked immediately before the signal, and the caller passes the same privilege checks as `pg_terminate_backend`. Slots and prepared transactions are refused.
- What a caller sees about another role's session (state, wait events, `xact_start`, query text, advice) follows `pg_stat_activity`: it requires superuser, `pg_read_all_stats`, or the session's own role. Pid, database, role, application name and xid/xmin are visible to everyone, as in `pg_stat_activity`. Replication slots and prepared transactions are public catalogs and are shown in full.
- The diagnostics take no locks on user relations and read `pg_prepared_xacts` by its schema-qualified name.
- Do not run untrusted extension builds as a superuser. Load only artifacts you compiled or obtained from a channel you trust.

## Reporting a vulnerability

Please report vulnerabilities privately, using GitHub's "Report a vulnerability" (private security advisory) on the repository's Security tab. Do not file public issues that include a working kill-path bypass or a way to see another role's session details.

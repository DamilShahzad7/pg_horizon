# Contributing

pg_horizon is a PostgreSQL C extension. Match contrib style: tabs in C, `ereport` for user-facing errors, `palloc` for memory, no process-global mutable state beyond GUCs.

## Rules for this codebase

- Horizons come from exported backend functions (`GetOldestNonRemovableTransactionId`, `GetReplicationHorizons`, `TransamVariables` / `ShmemVariableCache` limits). Do not reimplement `ComputeXidHorizons()`.
- Do not recommend `VACUUM FULL` or raising `autovacuum_freeze_max_age` as a freeze fix.
- Do not auto-drop replication slots. Do not terminate wraparound autovacuum.
- New SQL functions need GRANT/REVOKE in `pg_horizon--1.0.sql` (or a `--1.0--1.1.sql` upgrade script) and a regression or TAP test.
- Query text visibility must stay aligned with `pg_stat_activity`.

## Build

PostgreSQL 16+ development headers and `pg_config` in `PATH`:

```
make
make install
make installcheck
make prove_installcheck
```

`./scripts/docker-test.sh 18` builds against the official image.

## Pull requests

Include: the user-visible behaviour, the core API you are reading, and which test covers the change.

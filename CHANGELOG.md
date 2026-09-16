# Changelog

## 1.0.0

First public release.

- Cluster XID/MultiXact horizons from `GetOldestNonRemovableTransactionId()` (the VACUUM wrapper on PostgreSQL 16–18)
- Blocker attribution for backends, prepared transactions, physical/logical slots, and hot_standby_feedback
- Per-relation `horizon` vs `vacuum_lag` freeze constraint: only wraparound-urgent tables (`age(relfrozenxid) >= autovacuum_freeze_max_age`). Healthy tables at xmin are `ok`, not false horizon alarms. FreezeLimit matches `vacuum_get_cutoffs()` (`nextXID - freeze_min_age`, capped, then never newer than OldestXmin). Catalog tables use catalog xmin rather than the shared horizon.
- Catalog xmin is read from `pg_proc` (non-shared catalog). `pg_class` is shared and would report the wrong cutoff.
- Guarded terminate (off by default) via `pg_terminate_backend`; never drops slots or prepared xacts
- SQL regression tests and TAP coverage for idle-in-transaction, terminate, logical slots, prepared xacts, and conflicting lock waiters
- PostgreSQL 16–18

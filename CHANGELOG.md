# Changelog

## 1.1.0

Correctness release. Upgrade with `ALTER EXTENSION pg_horizon UPDATE` (see README); the new library works with the 1.0 SQL definitions until you do. The update is purely additive (views replaced with appended columns, comments); no function is recreated, so administrators' `GRANT`/`REVOKE` settings are kept.

### Fixed

- **Sessions in other databases were blamed for blocking VACUUM.** The horizons came from `GetOldestNonRemovableTransactionId()`, which includes the calling backend's own snapshot xmin (inherited from any running XID in any database). A transaction in database B was reported as the data horizon of database A, `freeze_constraint = horizon` was reported for tables `VACUUM FREEZE` could advance, and `is_horizon_holder`, `horizon_holders` and `dominant_blocker` named the wrong session. Horizons are now computed from one copy of the PGPROC / prepared / slot state with the calling session excluded, using the rules of `ComputeXidHorizons()`. A session in another database now pins only the shared horizon.
- **Cluster MultiXact age was effectively always 0.** `oldest_mxid` used `GetOldestMultiXactId()` (the oldest MultiXact still in use). It is now the real wraparound frontier, `min(datminmxid)`, so `mxid_age` and the MultiXact severity conditions work. MultiXact age also feeds `pg_horizon_databases`, `pg_horizon_relations.freeze_constraint` and `pg_horizon_explain`.
- **The failsafe check could never fire.** `critical` for `vacuum_failsafe_age` was tested after the `xid_vac_limit` `warning` return, which is always reached first. All critical conditions are now tested before warnings, and `vacuum_multixact_failsafe_age` is honoured.
- **`pg_horizon_terminate_blocker` could act on stale state.** Activity data was cached for the whole transaction, so a session that had since started a query could be killed without `force`. Activity is read fresh on every collection and re-checked just before the signal. The `pg_horizon terminated pid` log line is now written only after the termination was permitted (it used to be logged before the core permission check, including for denied attempts). Role membership is honoured (`has_privs_of_role`), like `pg_signal_backend`.
- **Finished prepared transactions were reported as holders forever.** The dummy PGPROC keeps its xid after `COMMIT/ROLLBACK PREPARED`. Candidates are now cross-checked against `pg_prepared_xacts`, and the GID is read from the same result.
- **Invalidated replication slots were reported as holders.** Core ignores them; so do we.
- **Slot rows were overwritten by their walsender/backend** (`backend_type`, `state`). Slots and prepared transactions keep their own type and state.
- **`query` could contain invalid UTF-8.** Text is now clipped on a character boundary (`pg_mbcliplen`), for the query, application name, names and gids.
- **`pg_horizon_explain` and `pg_horizon_vacuum_sql` waited behind `ACCESS EXCLUSIVE` locks.** They now read `pg_class` from the syscache without opening or locking the relation.
- **`blocker_count`, `horizon_holders` and the dominant holder ignored `pg_horizon.min_xmin_age`** and counted holders that pin nothing. One definition is used everywhere.
- **Logical slots had `xmin_age = 0`** and sorted last in the documented `ORDER BY xmin_age`. `xmin_age` is `NULL` when a holder has no xid/xmin, and the new `horizon_age` column gives the oldest of `xmin_age` / `catalog_xmin_age`.
- **`affects_data_horizon` / `affects_catalog_horizon`** are now what they say: whether the row is in scope for this database's data / catalog horizon.
- **`slot_xmin` was populated with no slots** (it was a computed horizon). It is now the raw slot aggregate, `NULL` when no slot pins an xmin.
- **`dominant_blocker` picked the oldest row regardless of relevance.** It is now the oldest holder of the horizon of that relation's kind.
- **`vacuum_sql` was silently truncated** at 255 bytes for quote-heavy names.
- **`user_catalog_table` relations** are classified with the catalog horizon, as in core.
- **`pg_horizon_check.message`** now says what decided the status, and is calm at `ok`.
- Friendlier errors: a missing OID, and partitioned tables (with a hint).
- `docker-test.sh` could start its smoke tests against the container's temporary initialisation server.

### Changed

- **Disclosure now follows `pg_stat_activity`.** For sessions of other roles, `state`, `backend_type`, `xact_start`, wait events, `query` and the session-specific advice are hidden from callers that are not superuser / `pg_read_all_stats` / the session's role. Slots and prepared transactions stay public.
- **Recommended SQL is safe to paste.** Destructive statements (`pg_terminate_backend`, `pg_cancel_backend`, `pg_drop_replication_slot`, `COMMIT/ROLLBACK PREPARED`) are commented out, after a read-only lookup. A prepared transaction shows both outcomes and says to ask the coordinator.
- `VACUUM (FREEZE, VERBOSE)` replaces `VACUUM (FREEZE, INDEX_CLEANUP ON, PROCESS_TOAST)`: `VERBOSE` prints the removable cutoff, and forcing index cleanup slowed emergency vacuums for no freezing benefit.
- `freeze_constraint` (relations) and the database state consider MultiXact age as well as XID age.
- `pg_horizon_databases()` collects only limits, and `pg_horizon_relations()` only limits and horizons.
- A collector no longer allocates a struct per backend slot (memory scales with the number of holders, not with `max_connections`).
- `freeze_limit` on a cluster that never wrapped is `FirstNormalTransactionId` rather than a wrapped value near 2^32.

### Added

- Upgrade path `1.0 -> 1.1` and a full `1.1` install script; `default_version` is `1.1`. A regression test checks that the upgrade leaves privileges alone and produces the same catalogs as a fresh install.
- `pg_horizon.mxid_freeze_max_age`, `mxid_failsafe_age` and `pg_horizon_blockers.horizon_age` (appended columns); `pg_horizon_explain().summary` now includes `relminmxid age=` and `mxid_horizon=`.
- Comments on the views and functions.
- Tests: an oracle test that compares the reported horizons with `VACUUM (VERBOSE)`'s removable cutoff (`t/002_horizons.pl`), MultiXact, terminate and stale-state race, encoding, replication and visibility TAP tests, and SQL tests for edge cases and the upgrade. `scripts/docker-test-cassert.sh` runs everything against PostgreSQL built with assertions.

## 1.0.0

First public release.

- Cluster XID/MultiXact horizons
- Blocker attribution for backends, prepared transactions, physical/logical slots, and hot_standby_feedback
- Per-relation `horizon` vs `vacuum_lag` freeze constraint: only wraparound-urgent tables (`age(relfrozenxid) >= autovacuum_freeze_max_age`). Healthy tables at xmin are `ok`, not false horizon alarms.
- Guarded terminate (off by default) via `pg_terminate_backend`; never drops slots or prepared xacts
- SQL regression tests and TAP coverage for idle-in-transaction, terminate, logical slots, prepared xacts, and conflicting lock waiters
- PostgreSQL 16-18

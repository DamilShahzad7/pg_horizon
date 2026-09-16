# pg_horizon

**See what is holding back VACUUM, freezing, and replication — and get the real fix, not a workaround.**

PostgreSQL will stop accepting writes when transaction IDs wrap. Before that happens, dead tuples stop being removable and tables bloat because something is pinning the **xmin horizon**. The holder is almost never “autovacuum is broken”. It is one of:

1. A session **idle in transaction**
2. A **prepared transaction** that was never committed
3. A **replication slot** (often an orphan)
4. **hot_standby_feedback** from a standby that itself has a long snapshot or a logical slot
5. Autovacuum actually lagging, which is a different failure and needs VACUUM, not a killed replica

Amazon RDS/Aurora shipped a proprietary `postgres_get_av_diag()` because this diagnosis is missing from core. PostgresAI documents a four-view SQL recipe that still cannot see `catalog_xmin` vs data xmin the way `ComputeXidHorizons()` does. **pg_horizon is the open, in-database form of that diagnosis**, built as a C extension that calls the same exported backend functions VACUUM uses.

## The gap

| What exists today | What it does not do |
| --- | --- |
| `age(datfrozenxid)` / `age(relfrozenxid)` | Tells you freeze is late, not **who** is pinning OldestXmin |
| `pg_stat_activity.backend_xmin` | Misses prepared xacts, slots, and catalog vs data |
| `pg_replication_slots.xmin` | Easy to drop a **live** replica’s slot if you treat every old xmin as an orphan |
| `VACUUM FULL` / raising `autovacuum_freeze_max_age` | Hides wraparound; `VACUUM FULL` needs an XID you may no longer have |
| RDS `postgres_get_av_diag()` | Closed source, cloud-only |
| SQL scripts (PostgresAI howto) | Approximate, race-prone, no relation-level freeze constraint |

pg_horizon answers three questions that production incidents actually need:

1. **How close is wraparound?** — using `TransamVariables` vac/warn/stop limits, not a guessed percentage
2. **Who holds the horizon?** — backends, prepared xacts, physical/logical slots, standby feedback
3. **Will `VACUUM FREEZE` help this table?** — `freeze_constraint = horizon` means no, remove the holder first; `vacuum_lag` means yes, vacuum is behind

## Requirements

- PostgreSQL **16, 17, or 18**
- A host that can install C extensions (self-managed, CloudNativePG, many on-prem packs). Not RDS/Aurora custom C.
- No `shared_preload_libraries` and no restart

## Install

```sh
make
sudo make install
```

```sql
CREATE EXTENSION pg_horizon;
```

`pg_config` must point at the server you will load the library into.

## 30-second incident loop

```sql
-- 1. How bad is it?
SELECT severity, xid_age, xid_headroom, wraparound_pct, blocker_count
FROM pg_horizon;

-- 2. Who is pinning OldestXmin?
SELECT blocker_type, pid, slot_name, state, xmin_age,
       is_idle_in_transaction, safe_to_terminate, recommended_sql
FROM pg_horizon_blockers
ORDER BY xmin_age DESC NULLS LAST;

-- 3. Why is this table not freezing?
SELECT freeze_constraint, diagnosis, vacuum_sql, dominant_blocker
FROM pg_horizon_explain('public.orders');

-- 4. Human-readable dump for the incident channel
SELECT pg_horizon_report();
```

Nagios / cron / Prometheus textfile:

```sql
SELECT status, code, oldest_xid_age, xid_headroom, message
FROM pg_horizon_check;
```

`code` is 0 (ok), 1 (warning), 2 (critical/emergency).

## What “real fix” means here

pg_horizon will **not** recommend:

- `VACUUM FULL` (takes `ACCESS EXCLUSIVE`, consumes XIDs, does not move OldestXmin)
- Raising `autovacuum_freeze_max_age` to silence wraparound
- Dropping an **active** physical slot that a replica is still using
- Killing autovacuum wraparound workers

It **will** recommend:

- `COMMIT` / `ROLLBACK` / `pg_terminate_backend` for **abandoned idle-in-transaction**
- `pg_cancel_backend` first for a still-running statement
- `COMMIT PREPARED` / `ROLLBACK PREPARED` for two-phase leftovers
- Consume or drop a slot **after** you confirm the consumer is gone
- `VACUUM (FREEZE, INDEX_CLEANUP ON, PROCESS_TOAST)` when freeze is vacuum-lag, not horizon-bound

Terminate is explicit and off by default:

```sql
ALTER SYSTEM SET pg_horizon.terminate_blockers = on;
SELECT pg_reload_conf();
SELECT pg_horizon_terminate_blocker(pid := 12345);
```

That function still refuses slots, prepared xacts, wraparound vacuum, walsenders, and the current backend.

## Views and functions

| Object | Purpose |
| --- | --- |
| `pg_horizon` | Cluster XID/MXID horizons, core wraparound limits, severity |
| `pg_horizon_blockers` | Every xmin/xid/catalog_xmin holder, wait-for locks, recommended SQL |
| `pg_horizon_databases` | Per-database `datfrozenxid` / `datminmxid` |
| `pg_horizon_relations` | Per-table freeze age and `horizon` vs `vacuum_lag` |
| `pg_horizon_explain(regclass)` | One table: the xmin VACUUM would use, freeze limit, diagnosis |
| `pg_horizon_report()` | Text report for tickets |
| `pg_horizon_check` | Monitoring row |
| `pg_horizon_vacuum_sql(regclass)` | The VACUUM FREEZE statement to run |
| `pg_horizon_terminate_blocker(pid, force)` | Guarded SIGTERM |

Query text follows `pg_stat_activity` rules: superuser or `pg_read_all_stats`, otherwise only your own sessions.

## Configuration

| GUC | Default | Meaning |
| --- | --- | --- |
| `pg_horizon.min_xmin_age` | `0` | Hide holders younger than this many XIDs |
| `pg_horizon.terminate_blockers` | `off` | Master switch for `pg_horizon_terminate_blocker` (`SIGHUP` / superuser) |

## How it is implemented

Horizons are **not** recomputed in SQL. The extension calls:

- `GetOldestNonRemovableTransactionId()` (the function VACUUM uses on PostgreSQL 16+)
- `GetOldestTransactionIdConsideredRunning()`
- `GetReplicationHorizons()`
- `TransamVariables` (PostgreSQL 17+) or `ShmemVariableCache` (16) vac/warn/stop/wrap limits under `XidGenLock`
- `GetOldestMultiXactId()` / `ReadNextMultiXactId()`

Holders are copied under `ProcArrayLock`, `ReplicationSlotControlLock`, and the slot mutex, then matched to `pgstat` activity, `GetLockStatusData()`, and `pg_prepared_xacts`. That is the same shared-memory picture VACUUM uses, with names attached.

See [docs/internals.md](docs/internals.md) and [docs/runbook.md](docs/runbook.md).

## Tests

```sh
make installcheck          # SQL regression + TAP
```

Or against official images:

```sh
./scripts/docker-test.sh 16
./scripts/docker-test.sh 17
./scripts/docker-test.sh 18
```

## License

[PostgreSQL License](LICENSE)

## Status

1.0.0 — production diagnostic. It does not change freeze behaviour; it tells you why freeze cannot advance and which core command will.

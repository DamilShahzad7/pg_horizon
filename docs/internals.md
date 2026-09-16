# Internals

pg_horizon is a diagnostic. It does not patch the freeze cutoff, invent a second xmin, or paper over wraparound by changing GUCs. It reads the same state VACUUM already uses and names the holder.

## Why SQL recipes are not enough

A common check unions four sources:

- `pg_stat_activity.backend_xmin`
- `pg_replication_slots.xmin`
- `pg_stat_replication.backend_xmin`
- `pg_prepared_xacts.transaction`

That list is the right *idea* and the wrong *implementation*:

1. **`ComputeXidHorizons()` is static** in `procarray.c`. Extensions cannot call it. They can call the wrapper VACUUM calls: `GetOldestNonRemovableTransactionId()` (PostgreSQL 16+).
2. Data xmin, shared xmin, and catalog xmin are **different**. A logical slot’s `catalog_xmin` must not be treated as a data-horizon pin in the same way as an idle-in-transaction `xmin`. The 2026 pgsql-hackers thread on `hot_standby_feedback` catalog_xmin vs data xmin exists because this confusion causes undiagnosable primary bloat.
3. Vacuum skips `PROC_IN_VACUUM` and `PROC_IN_LOGICAL_DECODING` backends. A SQL scan of `pg_stat_activity` does not.
4. Prepared transactions have `pid = 0` dummy `PGPROC` entries. They are not backends.
5. Slot `effective_xmin` / `effective_catalog_xmin` can differ from the on-disk `data.xmin` until flushed. VACUUM uses the effective values.

## Horizon values

| Field | Source |
| --- | --- |
| `next_xid`, `oldest_xid`, vac/warn/stop/wrap limits | `TransamVariables` under `XidGenLock` (the generator’s own limits) |
| `shared_xmin` | `GetOldestNonRemovableTransactionId(NULL)` |
| `catalog_xmin` | `GetOldestNonRemovableTransactionId(pg_proc)` — **not** `pg_class`, which is shared and returns the shared horizon |
| `data_xmin` | same function on a non-shared, non-catalog user heap (opened with `ConditionalLockRelationOid` so ACCESS EXCLUSIVE cannot stall the diagnostic), else shared |
| `oldest_running_xid` | `GetOldestTransactionIdConsideredRunning()` |
| `slot_xmin`, `slot_catalog_xmin` | `GetReplicationHorizons()` |
| MultiXact | `GetOldestMultiXactId()`, `ReadNextMultiXactId()` |

Severity uses **those core limits**, not a homemade 80% rule:

- `next_xid >= xid_stop_limit` → emergency (PostgreSQL will refuse new XIDs)
- `next_xid >= xid_warn_limit` → critical
- `next_xid >= xid_vac_limit` → warning (aggressive autovacuum should already be running)
- `age(oldest_xid) >= vacuum_failsafe_age` → critical

`xid_stop_limit` is `xidWrapLimit - 3_000_000` in `varsup.c`. `xid_warn_limit` is `xidWrapLimit - 40_000_000`. pg_horizon copies the live values rather than re-deriving them.

## Freeze constraint

`vacuum.c` computes `FreezeLimit` as `nextXID - freeze_min_age` (with `freeze_min_age` capped at `autovacuum_freeze_max_age / 2`), then never newer than the relation’s OldestXmin. `VACUUM FREEZE` uses freeze_min_age 0, so it can advance `relfrozenxid` up to OldestXmin.

`freeze_constraint` is only raised when `age(relfrozenxid)` has reached `autovacuum_freeze_max_age`:

- `horizon` — `relfrozenxid` is already at OldestXmin; VACUUM FREEZE cannot move it until the xmin holder is gone
- `vacuum_lag` — OldestXmin would allow freezing, but this relation has not been vacuumed far enough
- `ok` — not wraparound-urgent yet (healthy tables sit at xmin; that is not an incident)

## Locking

Collection order avoids lock-order inversion with core, and **never waits on user-table locks**:

1. Horizon wrappers (they take/release `ProcArrayLock` themselves). PostgreSQL 16 already exports `GetOldestNonRemovableTransactionId()`; there is no `GetOldestXmin()` on 16–18.
2. `XidGenLock` shared for generator limits
3. `ProcArrayLock` shared to copy backend and prepared dummy `PGPROC`s
4. `ReplicationSlotControlLock` shared plus per-slot mutex
5. `GetLockStatusData()` (its own locks)
6. `pgstat` activity by pid
7. SPI on `pg_prepared_xacts` for GIDs only

`data_xmin` is computed from the first user heap that can be opened with `ConditionalLockRelationOid()`. If every user table is `ACCESS EXCLUSIVE`-locked, `data_xmin` falls back to `shared_xmin` rather than hanging.

Never hold `ProcArrayLock` and `ReplicationSlotControlLock` together.

## Privileges

Query text and other-role details follow `pg_stat_activity`: `superuser` or `pg_read_all_stats`. Terminate follows `pg_signal_backend`. The terminate GUC is `PGC_SUSET` and defaults off so a `SELECT` cannot kill a session.

## What this is not

It is not a vacuum daemon, not a reaper, not an alternative to replication slot monitoring, and not a replacement for `pg_visibility` / `amcheck`. It is the missing attribution layer on top of horizons that already exist in core.

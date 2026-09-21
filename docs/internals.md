# Internals

pg_horizon is a diagnostic. It does not patch the freeze cutoff, invent a second xmin, or paper over wraparound by changing GUCs. It reads the same shared state VACUUM uses, applies the same rules, and names the holder.

## Why SQL recipes are not enough

A common check unions four sources:

- `pg_stat_activity.backend_xmin`
- `pg_replication_slots.xmin`
- `pg_stat_replication.backend_xmin`
- `pg_prepared_xacts.transaction`

That list is the right *idea* and the wrong *implementation*:

1. **`ComputeXidHorizons()` is static** in `procarray.c`, and its public wrappers (`GetOldestNonRemovableTransactionId()` and friends) include the *calling backend's own* PGPROC. See "Why not the core wrapper" below.
2. Data xmin, shared xmin, and catalog xmin are **different**. A logical slot's `catalog_xmin` must not be treated as a data-horizon pin in the same way as an idle-in-transaction `xmin`, and a session in another database must not be treated as a data-horizon pin at all.
3. Vacuum skips `PROC_IN_VACUUM` and `PROC_IN_LOGICAL_DECODING` backends. A SQL scan of `pg_stat_activity` does not.
4. Prepared transactions have `pid = 0` dummy `PGPROC` entries. They are not backends, and the dummy proc keeps its `xid` after `COMMIT/ROLLBACK PREPARED`.
5. Slot `effective_xmin` / `effective_catalog_xmin` can differ from the on-disk `data.xmin` until flushed, and invalidated slots are ignored by core. VACUUM uses the effective values of the valid slots.

## Horizon computation

The rules below are `ComputeXidHorizons()` in PostgreSQL 16, 17 and 18 (the function is identical across them apart from the name of the shared variable cache). pg_horizon applies them to one copy of the state, taken while holding `ProcArrayLock` in shared mode, with the calling backend left out:

```
initial  = latestCompletedXid + 1
for each proc (calling backend excluded):
    x = min(xmin, xid); skip if neither is set
    oldest_running = min(oldest_running, x)
    skip if PROC_IN_VACUUM or PROC_IN_LOGICAL_DECODING
    shared = min(shared, x)
    if proc is in this database, or database 0, or PROC_AFFECTS_ALL_HORIZONS:
        data = min(data, x)
shared  = min(shared, slot_xmin, slot_catalog_xmin)
data    = min(data, slot_xmin)
catalog = min(data, slot_catalog_xmin)
temp    = own xid if assigned, else initial
```

Prepared-transaction dummy procs take part like any other proc, in the database they were prepared in.

| Field | Source |
| --- | --- |
| `next_xid`, `oldest_xid`, vac/warn/stop/wrap limits | `TransamVariables` (`ShmemVariableCache` on 16) under `XidGenLock` (the generator's own limits) |
| `shared_xmin`, `catalog_xmin`, `data_xmin`, `oldest_running_xid` | the computation above, for the current database |
| `slot_xmin`, `slot_catalog_xmin` | `ProcArrayGetReplicationSlotXmin()`: the raw slot aggregate, `NULL` if no slot pins anything |
| `oldest_mxid`, `next_mxid` | `ReadMultiXactIdRange()`: `oldestMultiXactId` is `min(datminmxid)`, the wraparound frontier |
| `mxid_horizon` | `GetOldestMultiXactId()`: the oldest MultiXact still in use, VACUUM's `OldestMxact` |

The per-relation horizon is chosen the way `GlobalVisHorizonKindForRel()` chooses it: shared catalogs (and everything during recovery) use the shared horizon; catalogs (relation OID below `FirstUnpinnedObjectId`) and, when `wal_level = logical`, `user_catalog_table` relations use the catalog horizon; the session's own temporary tables use the temp horizon; everything else uses the data horizon. This is done from `pg_class` fields, so no relation is opened or locked.

### Why not the core wrapper

`GetOldestNonRemovableTransactionId()` runs `ComputeXidHorizons()`, which includes the calling backend's `PGPROC`. A backend that is executing a query has a snapshot, and that snapshot's xmin is the oldest running XID **in any database** at the time. So, called from a query in database A while a transaction in database B holds an XID, the wrapper reports B's XID as A's data horizon, while a `VACUUM` in A (which holds no snapshot) uses a much newer cutoff. Earlier versions of this extension used the wrapper and consequently blamed sessions in other databases for blocking `VACUUM`, and reported `freeze_constraint = horizon` for tables that `VACUUM FREEZE` could in fact advance.

The trade-off is that pg_horizon now reimplements a small, stable algorithm instead of calling core. The TAP test `t/002_horizons.pl` guards that: it compares the reported horizons with the `removable cutoff` that `VACUUM (VERBOSE)` prints (the value core itself computed) in each scenario listed in the README. If a future PostgreSQL changes `ComputeXidHorizons()`, that test fails.

**During recovery** the per-database horizons do not exist (every XID lives in `KnownAssignedXids`, which is private to `procarray.c`), so the core wrapper is used for all kinds and the value includes the calling session. `pg_horizon_report()` says so.

### The observer

The querying session is excluded from every horizon and never marked as a holder. Corollary: a long transaction in the session that runs the diagnostics is invisible to *its own* report. Seen from another session it is an ordinary holder. Run diagnostics from a separate session.

## Holder attribution

Each row gets `holds_shared` / `holds_catalog` / `holds_data` when its contribution (`min(xid, xmin)`, or for slots `xmin` and `catalog_xmin` separately) equals the horizon of that kind and it is in scope for it. `is_horizon_holder` is the OR. Several rows can hold the same horizon (sessions whose snapshots share an xmin). The "dominant" holder for a relation is the oldest listed row that holds the horizon of *that relation's kind*, so a logical slot is named for catalog tables, a session in another database only for shared catalogs, and a plain data table gets `NULL` unless a data-horizon holder exists.

Rows that have an xid or xmin but pin nothing are still listed (they are the next ones to matter), with `is_horizon_holder = false`.

## Freeze constraint

`vacuum.c` computes `FreezeLimit` as `nextXID - freeze_min_age` (with `freeze_min_age` capped at `autovacuum_freeze_max_age / 2`), then never newer than the relation's OldestXmin. `VACUUM FREEZE` uses freeze_min_age 0, so it can advance `relfrozenxid` up to OldestXmin; the `freeze_limit` column is what an ordinary (auto)vacuum would use. On a cluster whose XID counter has never wrapped, a limit that would fall below `FirstNormalTransactionId` is shown as `FirstNormalTransactionId`.

`freeze_constraint` is only raised for a dimension whose age has reached its `autovacuum_*freeze_max_age`. XIDs can advance when `relfrozenxid` precedes the relation's horizon; MultiXacts when `relminmxid` precedes `mxid_horizon`. `horizon` means at least one urgent dimension cannot advance, `vacuum_lag` means all can, `ok` means none is urgent. Healthy tables sit at the horizon all the time, so being at the horizon is not itself a finding.

## Severity

Limits are the core ones: `xid_stop_limit` is `xidWrapLimit - 3_000_000` and `xid_warn_limit` is `xidWrapLimit - 40_000_000` in `varsup.c`; `xid_vac_limit` is `oldest_xid + autovacuum_freeze_max_age`. pg_horizon copies the live values rather than re-deriving them. The one cutoff that is pg_horizon's own is the emergency headroom (10,000,000 XIDs before the stop limit). Every `critical` condition is tested before any `warning` one: passing `vacuum_failsafe_age` implies passing `xid_vac_limit`, so the other order makes the failsafe check unreachable. The decisive condition is returned as text and used in `pg_horizon_check.message` and the report.

## Prepared transactions

The dummy `PGPROC` of a finished prepared transaction keeps its `xid`; only the opaque `GlobalTransaction`'s `valid` flag changes. Scanning `PreparedXactProcs` alone would therefore report every finished prepared transaction as a phantom holder forever (until the slot in the array is reused). pg_horizon cross-checks the candidates against `pg_catalog.pg_prepared_xacts` (the public view of exactly the valid entries, schema-qualified so `search_path` cannot substitute another relation) before computing anything, and takes the GID from the same result.

## Activity data

`pg_stat_activity`-style data is snapshotted once per transaction, while xmins are read live. pg_horizon clears the backend-activity snapshot at the start of every collection so state and xmin are always paired from the same moment. This is what makes `pg_horizon_terminate_blocker()` safe to call after other pg_stat reads in the same transaction. (It also means a later `pg_stat_activity` read in that transaction sees fresh data, which is the same effect as `pg_stat_clear_snapshot()`.)

## Locking

Collection takes only short, shared locks and never waits on user-table locks:

1. `XidGenLock` shared for the generator limits
2. `ProcArrayLock` shared, once, to copy PGPROC, prepared and slot-aggregate state. Nothing is allocated while it is held; the copies are processed afterwards
3. `ReplicationSlotControlLock` shared plus per-slot mutex, copying into a local array
4. SPI on `pg_prepared_xacts`, only when a prepared-transaction candidate exists
5. `GetLockStatusData()` (its own locks), only when there is a session holder, for the lock-waiter attribution
6. backend activity (`pgstat`)

`pg_horizon_explain()` and `pg_horizon_vacuum_sql()` read `pg_class` through the syscache without opening the relation, so they do not queue behind an `ACCESS EXCLUSIVE` lock on it. `pg_horizon_databases()` collects only the limits, and `pg_horizon_relations()` only the limits and horizons; neither builds holder rows.

Never hold `ProcArrayLock` and `ReplicationSlotControlLock` together (the shared re-acquire of `ProcArrayLock` inside `ProcArrayGetReplicationSlotXmin()` is a nested *shared* acquire of the same lock, which is fine).

## Privileges

What a caller may learn about another role's session follows `pg_stat_activity`: `superuser`, `pg_read_all_stats`, or the session's own role see state, wait events, `xact_start` and query; others see `NULL` (query `<insufficient privilege>`), and the reason and advice fields for that row are replaced by a generic sentence. Slots and prepared transactions are shown in full to everyone, as in their catalog views. Terminate follows `pg_signal_backend`. The terminate GUC is `PGC_SUSET` and defaults off so a `SELECT` cannot kill a session; a `SET` of it before the library is loaded in a session is accepted as a placeholder but is re-checked and reverted with a warning when the library loads.

## Compatibility between library and SQL

The shared library is usually installed before `ALTER EXTENSION UPDATE` runs, so a newer library must serve older SQL definitions. Record-returning C functions fill a values array sized for the newest definition and let `heap_form_tuple` / `tuplestore_putvalues` use only as many leading columns as the installed SQL declares. Therefore new OUT columns are only ever **appended**, and existing columns never change type. Changed views use `CREATE OR REPLACE VIEW` (appended columns only) so dependent user objects and their privileges survive. Avoid dropping and recreating a function in an update script: it resets the `EXECUTE` privileges an administrator set on it, and because grants made inside an extension script are recorded as the extension's initial privileges, restoring them from the script would make `pg_dump` omit them and block `DROP ROLE`. Put new information in an appended view column, or in a new function, instead. The regression test `004_upgrade` checks both directions: it runs 1.0 SQL against the current library, and compares a 1.0 → 1.1 upgrade with a fresh 1.1 install catalog by catalog.

## What this is not

It is not a vacuum daemon, not a reaper, not an alternative to replication slot monitoring, and not a replacement for `pg_visibility` / `amcheck`. It is the missing attribution layer on top of horizons that already exist in core.

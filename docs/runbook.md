# Wraparound and freeze runbook

Use this when `datfrozenxid` age is climbing, autovacuum logs "dead but not yet removable", or the log already says wraparound.

## 1. Do not do these

- **`VACUUM FULL`**. It needs a transaction ID, takes `ACCESS EXCLUSIVE`, and does not advance OldestXmin. If you are near `xid_stop_limit` it can make the outage worse.
- **Raise `autovacuum_freeze_max_age`**. That only moves `xid_vac_limit`. It does not unstick a held xmin.
- **Drop an `active` physical slot** because xmin looks old. That is a live replica. Fix the replica.
- **Kill autovacuum** that has `VACUUM_FOR_WRAPAROUND` set.
- **`ROLLBACK PREPARED` without asking.** A prepared transaction is usually waiting for a transaction coordinator; its outcome may already be decided elsewhere.

Run the diagnostics from a **separate, short-lived session**: the session that runs them is excluded from the horizons, so a long transaction in it would not show up in its own report.

## 2. Read the cluster

```sql
CREATE EXTENSION IF NOT EXISTS pg_horizon;

SELECT severity, xid_age, xid_headroom, wraparound_pct,
       mxid_age, oldest_xid_db_name, blocker_count
FROM pg_horizon;

SELECT status, code, message FROM pg_horizon_check;
```

| severity | meaning |
| --- | --- |
| ok | No wraparound pressure and no stale horizon holder |
| warning | XID/MultiXact age past `autovacuum_freeze_max_age` / `autovacuum_multixact_freeze_max_age`, or a horizon holder (idle transaction, inactive slot, prepared transaction) older than 1,000,000 XIDs |
| critical | Past `xid_warn_limit`, or XID/MultiXact age at the failsafe age |
| emergency | At `xid_stop_limit`, or fewer than 10,000,000 XIDs left before it |

`pg_horizon_check.message` says which condition decided the status. `xid_headroom` is XIDs remaining until PostgreSQL refuses new transactions.

`pg_horizon_databases` shows which database is oldest, and its state for both XIDs and MultiXacts.

## 3. Attribute the holder

```sql
SELECT blocker_type, pid, datname, slot_name, prepared_gid, state, horizon_age,
       is_horizon_holder, affects_data_horizon, is_idle_in_transaction,
       holds_locks, waiting_pids, recommended_sql, reason
FROM pg_horizon_blockers
ORDER BY horizon_age DESC NULLS LAST;
```

`horizon_age` is the oldest of `xmin_age` and `catalog_xmin_age`, so logical slots (which only have a `catalog_xmin`) sort correctly.

Act on the **oldest holder that pins the horizon your table sees** first (`is_horizon_holder`, and `affects_data_horizon` for ordinary tables). Killing a younger one does nothing while the oldest snapshot remains. A session in **another database** can pin only the shared-catalog horizon; it does not stop `VACUUM` of your tables (`affects_data_horizon = false`), though it does hold back the shared catalogs and with them `datfrozenxid`.

| type | Real fix |
| --- | --- |
| `backend` idle in transaction | Application COMMIT/ROLLBACK, or `pg_terminate_backend` if the client is gone |
| `backend` active | Let it finish; `pg_cancel_backend` if it should not run |
| `prepared` | Resolve with the transaction coordinator, then `COMMIT PREPARED` or `ROLLBACK PREPARED` on the gid |
| `physical_slot` inactive | Confirm replica is gone, then `pg_drop_replication_slot` |
| `physical_slot` active | Fix the replica; do not drop |
| `logical_slot` | Consume changes; drop only if the consumer is decommissioned |
| `standby_feedback` | Diagnose **on the standby** (`pg_horizon` there). A logical slot on the standby can publish catalog_xmin onto the primary |

The `recommended_sql` column starts with a read-only lookup of the holder and keeps every destructive statement commented out, so it is safe to paste.

Session details (`state`, query, wait events) of *other roles'* sessions are hidden unless you are a superuser or `pg_read_all_stats`; run this as one of those during an incident.

## 4. Per table

```sql
SELECT schemaname, relname, xid_age, mxid_age, freeze_constraint, dead_tuples
FROM pg_horizon_relations
ORDER BY greatest(xid_age, mxid_age) DESC
LIMIT 20;

SELECT * FROM pg_horizon_explain('myschema.mytable');
```

- `freeze_constraint = horizon` -> an age is past its `autovacuum_*freeze_max_age` and VACUUM cannot advance it yet. `diagnosis` says whether it is the XID or the MultiXact side, and `dominant_blocker` names the holder of *this table's* horizon. Go back to step 3.
- `freeze_constraint = vacuum_lag` -> past the limit, but the horizon would allow freezing. Run the `vacuum_sql` from `pg_horizon_explain`. Check `autovacuum_vacuum_cost_limit`, number of workers, and whether the table is skipped.
- `freeze_constraint = ok` -> not wraparound-urgent. A new table sitting at the horizon is normal.

`pg_horizon_explain` does not wait behind locks on the table, so it works while a stuck DDL holds `ACCESS EXCLUSIVE`.

A `MultiXact` `horizon` verdict means sessions are holding row locks (`FOR SHARE`, foreign-key checks) that several transactions share; they must finish. Look at long-running transactions in `pg_stat_activity`.

## 5. After the holder is gone

The horizon should move. Then:

```sql
VACUUM (FREEZE, VERBOSE) myschema.mytable;
```

`VERBOSE` prints `removable cutoff`, the OldestXmin VACUUM used, which you can compare with `pg_horizon.data_xmin`. On a very large table in an emergency you can add `INDEX_CLEANUP OFF` to skip index vacuuming (it does not affect freezing); the default `AUTO` already skips it when the failsafe triggers.

Watch `pg_horizon` `xid_age` and `pg_horizon_databases`. If age does not fall, you removed the wrong holder.

## 6. Monitoring

```sql
SELECT * FROM pg_horizon_check;
```

Alert on `code >= 1`. Page on `code = 2` (critical, or emergency at fewer than 10,000,000 XIDs of headroom).

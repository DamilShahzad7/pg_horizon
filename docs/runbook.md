# Wraparound and freeze runbook

Use this when `datfrozenxid` age is climbing, autovacuum logs “dead but not yet removable”, or the log already says wraparound.

## 1. Do not do these

- **`VACUUM FULL`**. It needs a transaction ID, takes `ACCESS EXCLUSIVE`, and does not advance OldestXmin. If you are near `xid_stop_limit` it can make the outage worse.
- **Raise `autovacuum_freeze_max_age`**. That only moves `xid_vac_limit`. It does not unstick a held xmin.
- **Drop an `active` physical slot** because xmin looks old. That is a live replica. Fix the replica.
- **Kill autovacuum** that has `VACUUM_FOR_WRAPAROUND` set.

## 2. Read the cluster

```sql
CREATE EXTENSION IF NOT EXISTS pg_horizon;

SELECT severity, xid_age, xid_headroom, wraparound_pct,
       oldest_xid_db_name, blocker_count
FROM pg_horizon;
```

| severity | meaning |
| --- | --- |
| ok | Below core vac limit; no abandoned horizon holder worth paging |
| warning | Past `xid_vac_limit` and/or a stuck idle xact / inactive slot |
| critical | Past `xid_warn_limit` or failsafe age |
| emergency | At or past `xid_stop_limit` — writes will fail |

`xid_headroom` is XIDs remaining until PostgreSQL refuses new transactions.

## 3. Attribute the holder

```sql
SELECT blocker_type, pid, slot_name, prepared_gid, state, xmin_age,
       catalog_xmin_age, is_idle_in_transaction, holds_locks,
       waiting_pids, is_horizon_holder, recommended_sql, reason
FROM pg_horizon_blockers
ORDER BY xmin_age DESC NULLS LAST;
```

Act on the **oldest** non-observer holder first. Killing the second-oldest does nothing while the oldest snapshot remains.

| type | Real fix |
| --- | --- |
| `backend` idle in transaction | Application COMMIT/ROLLBACK, or `pg_terminate_backend` if the client is gone |
| `backend` active | `pg_cancel_backend` first |
| `prepared` | `COMMIT PREPARED` / `ROLLBACK PREPARED` on the gid |
| `physical_slot` inactive | Confirm replica is gone, then `pg_drop_replication_slot` |
| `physical_slot` active | Fix the replica; do not drop |
| `logical_slot` | Consume changes; drop only if the consumer is decommissioned |
| `standby_feedback` | Diagnose **on the standby** (`pg_horizon` there). A logical slot on the standby can publish catalog_xmin onto the primary |

## 4. Per table

```sql
SELECT schemaname, relname, xid_age, freeze_constraint, dead_tuples
FROM pg_horizon_relations
ORDER BY xid_age DESC
LIMIT 20;

SELECT * FROM pg_horizon_explain('myschema.mytable');
```

- `freeze_constraint = horizon` → age is past `autovacuum_freeze_max_age` and VACUUM cannot freeze this table yet. Go back to step 3.
- `freeze_constraint = vacuum_lag` → age is past `autovacuum_freeze_max_age` but OldestXmin would allow freezing. Run the `vacuum_sql` from `pg_horizon_explain`. Check `autovacuum_vacuum_cost_limit`, number of workers, and whether the table is skipped.
- `freeze_constraint = ok` → not wraparound-urgent. A new table sitting at xmin is normal.

## 5. After the holder is gone

OldestXmin should move. Then:

```sql
VACUUM (FREEZE, INDEX_CLEANUP ON, PROCESS_TOAST) myschema.mytable;
```

Watch `pg_horizon` `xid_age` and `pg_horizon_databases`. If age does not fall, you removed the wrong holder.

## 6. Monitoring

```sql
SELECT * FROM pg_horizon_check;
```

Alert on `code >= 1`. Page on `code = 2` or `xid_headroom < 20000000`.

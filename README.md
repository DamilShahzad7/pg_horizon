# pg_horizon

pg_horizon is a PostgreSQL extension for inspecting XID and MultiXact freeze horizons. It reports cluster wraparound state and lists backends, prepared transactions, replication slots, and `hot_standby_feedback` WAL senders that are holding those horizons.

The extension does not change freeze behaviour. It does not require `shared_preload_libraries` or a PostgreSQL restart.

See [docs/runbook.md](docs/runbook.md) and [docs/internals.md](docs/internals.md) for operational and implementation detail.

## Features

- Cluster XID/MultiXact horizons and wraparound headroom
- Horizon holders visible to the extension
- Per-database and per-relation freeze age
- Per-relation freeze explanation and a `VACUUM FREEZE` statement
- Optional terminate of a supported backend, off by default

## Requirements

Tested on PostgreSQL 16, 17, and 18. It will not compile on 15 or earlier.

You need PostgreSQL server development files (PGXS) and a `pg_config` that matches the server that will load the library. Managed services that do not allow user C extensions cannot install it.

## Installation

```bash
git clone https://github.com/DamilShahzad7/pg_horizon.git
cd pg_horizon
make
make install
```

If `pg_config` is not the right server:

```bash
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
```

```sql
CREATE EXTENSION pg_horizon;
```

## Quick start

```sql
SELECT severity, xid_age, xid_headroom, oldest_xid_db_name, blocker_count
FROM pg_horizon;

SELECT blocker_type, pid, slot_name, prepared_gid, state, xmin_age,
       recommended_sql, reason
FROM pg_horizon_blockers
ORDER BY xmin_age DESC NULLS LAST;

SELECT schemaname, relname, xid_age, freeze_constraint
FROM pg_horizon_relations
ORDER BY xid_age DESC
LIMIT 20;

SELECT freeze_constraint, relation_xmin, freeze_limit, diagnosis, vacuum_sql
FROM pg_horizon_explain('pg_class');

SELECT status, code, oldest_xid_age, xid_headroom, message
FROM pg_horizon_check;
```

`pg_horizon` is a view. `pg_horizon_blockers` is also a view; it hides the backend running the query. Use `pg_horizon_blockers(true)` to include it.

## Views and functions

Several names are both a function and a view. `SELECT * FROM name` uses the view. `pg_horizon` is a view over `pg_horizon_status()`.

| Object | Type | Description |
| --- | --- | --- |
| `pg_horizon` | view | Cluster horizons, wraparound limits, severity |
| `pg_horizon_blockers` | view | Holders, excluding the calling backend; adds `xact_age` |
| `pg_horizon_blockers(include_observer boolean DEFAULT false)` | function | Same without `xact_age` |
| `pg_horizon_databases` | view | Per-database `datfrozenxid` / `datminmxid` |
| `pg_horizon_relations` | view | Per-relation freeze age and `freeze_constraint`; adds `schemaname` |
| `pg_horizon_relations(min_age bigint DEFAULT 0)` | function | Same without `schemaname` |
| `pg_horizon_explain(rel regclass)` | function | Freeze limit, diagnosis, vacuum SQL, dominant blocker |
| `pg_horizon_vacuum_sql(rel regclass)` | function | `VACUUM (FREEZE, …)` statement for one relation |
| `pg_horizon_report()` | function | Text report |
| `pg_horizon_check` | view | Monitoring row (`status`, `code`, …) |
| `pg_horizon_terminate_blocker(pid integer, force boolean DEFAULT false)` | function | Terminate a supported backend when enabled |

`freeze_constraint` on relations is a text column: `ok`, `horizon`, or `vacuum_lag`.

`pg_horizon_explain` and `pg_horizon_vacuum_sql` are `STRICT` and apply to tables, materialized views, and TOAST tables.

## Configuration

| GUC | Default | Context |
| --- | --- | --- |
| `pg_horizon.min_xmin_age` | `0` | user |
| `pg_horizon.terminate_blockers` | `off` | superuser |

`min_xmin_age` hides holders younger than that many XIDs.

`pg_horizon.terminate_blockers` is only a switch. Turning it on does not terminate anyone.

```sql
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(12345);
```

That calls `pg_terminate_backend` on a backend that holds xmin, which aborts its open transaction. It will not signal slots, prepared transactions, walsenders, autovacuum, or the current backend. `force` also allows an `active` backend; it does not skip the other checks.

## Permissions

Reading the diagnostic views does not require superuser (`GRANT SELECT` / `GRANT EXECUTE` to `PUBLIC`).

Query text is shown for superusers, members of `pg_read_all_stats`, or the backend’s own role, as with `pg_stat_activity`.

`pg_horizon_terminate_blocker` is revoked from `PUBLIC`. The GUC is superuser-only. The function also requires superuser, `pg_signal_backend`, or that the target session belongs to the caller.

## How it works

Each call reads current backend state (XID limits, visibility horizons, MultiXact, slots, `PGPROC`, locks, and freeze ages on `pg_class` / `pg_database`) and attaches a reason to holders it can see. Details are in [docs/internals.md](docs/internals.md).

## Limitations

- Tested on PostgreSQL 16–18.
- Not installable where user C extensions are disallowed.
- Results are a snapshot at call time.
- Suggested SQL is not executed for you.

## Testing

```bash
make installcheck          # SQL tests in sql/
make prove_installcheck    # TAP tests in t/
```

`installcheck` needs a running server with the extension installed. CI uses PostgreSQL 16, 17, and 18.

```bash
./scripts/docker-test.sh 16
./scripts/docker-test.sh 17
./scripts/docker-test.sh 18
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Security

See [SECURITY.md](SECURITY.md).

## License

PostgreSQL License. See [LICENSE](LICENSE).

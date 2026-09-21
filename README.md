# pg_horizon

pg_horizon is a PostgreSQL extension for inspecting XID and MultiXact freeze horizons. It reports cluster wraparound state and lists the backends, prepared transactions, replication slots, and `hot_standby_feedback` WAL senders that are holding those horizons.

The extension does not change freeze behaviour. It does not require `shared_preload_libraries` or a PostgreSQL restart.

See [docs/runbook.md](docs/runbook.md) for what to do during an incident and [docs/internals.md](docs/internals.md) for how the numbers are computed.

## Features

- Cluster XID/MultiXact wraparound state, headroom, and a severity with a stated reason
- The xmin horizons a `VACUUM` started now would see (shared, catalog, and the current database's data horizon)
- Horizon holders, each attributed to the horizon it actually pins
- Per-database and per-relation freeze age, for both XIDs and MultiXacts
- Per-relation freeze explanation and a `VACUUM (FREEZE, VERBOSE)` statement
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

### Upgrading from 1.0

Install the new library, then update each database that has the extension:

```bash
make && make install
```

```sql
ALTER EXTENSION pg_horizon UPDATE;      -- to 1.1
```

The 1.1 library also works with the 1.0 SQL definitions, so there is no broken window between `make install` and `ALTER EXTENSION UPDATE`, and no restart is needed. The update is purely additive: it appends columns to `pg_horizon` (`mxid_freeze_max_age`, `mxid_failsafe_age`) and `pg_horizon_blockers` (`horizon_age`) and adds comments. No function is dropped or recreated, so `GRANT`/`REVOKE` settings you made on the extension's functions and views are kept, and views you built on top of the extension's views keep working. See [CHANGELOG.md](CHANGELOG.md) for behaviour changes.

## Quick start

```sql
SELECT severity, xid_age, xid_headroom, mxid_age, oldest_xid_db_name, blocker_count
FROM pg_horizon;

SELECT blocker_type, pid, slot_name, prepared_gid, state, horizon_age,
       is_horizon_holder, recommended_sql, reason
FROM pg_horizon_blockers
ORDER BY horizon_age DESC NULLS LAST;

SELECT schemaname, relname, xid_age, mxid_age, freeze_constraint
FROM pg_horizon_relations
ORDER BY greatest(xid_age, mxid_age) DESC
LIMIT 20;

SELECT freeze_constraint, relation_xmin, freeze_limit, diagnosis, vacuum_sql
FROM pg_horizon_explain('pg_class');

SELECT status, code, message FROM pg_horizon_check;
```

`pg_horizon` is a view. `pg_horizon_blockers` is also a view; it hides the backend running the query. Use `pg_horizon_blockers(true)` to include it.

Run the diagnostics from a **separate, short-lived session**. The session that runs them is left out of the horizons (see below), so a long transaction in that same session would not appear in its own report.

## Views and functions

Several names are both a function and a view. `SELECT * FROM name` uses the view. `pg_horizon` is a view over `pg_horizon_status()`.

| Object | Type | Description |
| --- | --- | --- |
| `pg_horizon` | view | Cluster limits, horizons, severity; adds `mxid_freeze_max_age`, `mxid_failsafe_age` |
| `pg_horizon_blockers` | view | Holders, excluding the calling backend; adds `xact_age` and `horizon_age` |
| `pg_horizon_blockers(include_observer boolean DEFAULT false)` | function | Same without `xact_age` / `horizon_age` |
| `pg_horizon_databases` | view | Per-database `datfrozenxid` / `datminmxid` and wraparound state |
| `pg_horizon_relations` | view | Per-relation freeze age and `freeze_constraint`; adds `schemaname` |
| `pg_horizon_relations(min_age bigint DEFAULT 0)` | function | Same without `schemaname` |
| `pg_horizon_explain(rel regclass)` | function | Horizon, freeze limit, diagnosis, vacuum SQL, dominant blocker |
| `pg_horizon_vacuum_sql(rel regclass)` | function | `VACUUM (FREEZE, VERBOSE)` statement for one relation |
| `pg_horizon_report()` | function | Text report |
| `pg_horizon_check` | view | Monitoring row (`status`, `code`, `message`, ...) |
| `pg_horizon_terminate_blocker(pid integer, force boolean DEFAULT false)` | function | Terminate a supported backend when enabled |

`pg_horizon_explain` and `pg_horizon_vacuum_sql` are `STRICT` and apply to tables, materialized views, and TOAST tables. They read `pg_class` without locking the relation, so they do not wait behind an `ACCESS EXCLUSIVE` lock on it.

### What the columns mean

**Horizons.** `shared_xmin`, `catalog_xmin` and `data_xmin` are the cutoffs `VACUUM` would use *if it started now, in your current database, from another session*: shared catalogs see every database, ordinary tables see only this database, catalogs see this database plus logical-slot `catalog_xmin`. The querying session is excluded. `data_xmin` and `catalog_xmin` are therefore about the database you are connected to. While in recovery (on a standby) the per-database split does not exist and the values include the querying session; `pg_horizon_report()` says so.

**Holders.** `pg_horizon_blockers` lists every session, prepared transaction and slot that has an xid or xmin, not only the ones pinning something. `is_horizon_holder` says whether the row pins a horizon right now, and `affects_data_horizon` / `affects_catalog_horizon` say whether the row is in scope for this database's horizons at all: a session in another database can pin the shared horizon but never this database's data horizon. `xmin_age` is `NULL` for a holder that has no xid or xmin (a logical slot); `horizon_age` is the oldest of `xmin_age` and `catalog_xmin_age`, so `ORDER BY horizon_age DESC NULLS LAST` puts the worst holder first.

**MultiXacts.** `oldest_mxid` / `mxid_age` in `pg_horizon` is the real wraparound frontier (`min(datminmxid)`). The `mxid_horizon=` value in the `summary` of `pg_horizon_explain` is the oldest MultiXact still in use, which is what `VACUUM` may freeze up to; `pg_horizon_relations` has each table's `relminmxid` and `mxid_age`.

**`freeze_constraint` on relations** is a text column:

| value | meaning |
| --- | --- |
| `ok` | neither the XID age nor the MultiXact age has reached its `autovacuum_*freeze_max_age` |
| `vacuum_lag` | at least one is urgent and every urgent one could be advanced by `VACUUM`; autovacuum has not got there |
| `horizon` | at least one urgent age cannot be advanced: `relfrozenxid` / `relminmxid` is not older than the cutoff `VACUUM` would use, so vacuuming does not help until a holder goes away |

**`freeze_constraint` on databases** is the worst of the XID and MultiXact state: `ok`, `aging` (half of the freeze max age), `aggressive_autovacuum` (past it) or `failsafe` (past the failsafe age).

**Severity** (`pg_horizon.severity`, `pg_horizon_check.status`; `code` is 0 / 1 / 2, with critical and emergency both 2):

| severity | when |
| --- | --- |
| `emergency` | `next_xid` reached `xid_stop_limit`, or fewer than 10,000,000 XIDs remain before it (pg_horizon's own cutoff) |
| `critical` | past `xid_warn_limit`, or XID/MultiXact age at the failsafe age (`vacuum_failsafe_age`, `vacuum_multixact_failsafe_age`) |
| `warning` | XID/MultiXact age past `autovacuum_freeze_max_age` / `autovacuum_multixact_freeze_max_age`, or a horizon holder that is an idle transaction, inactive slot, or prepared transaction older than 1,000,000 XIDs |
| `ok` | none of the above |

`pg_horizon_check.message` states which condition decided the severity, then names the dominant holder.

## Configuration

| GUC | Default | Context |
| --- | --- | --- |
| `pg_horizon.min_xmin_age` | `0` | user |
| `pg_horizon.terminate_blockers` | `off` | superuser |

`min_xmin_age` hides holders younger than that many XIDs. It applies to `pg_horizon_blockers`, to `blocker_count`, to `horizon_holders`, to the dominant holder, and to the report, so they always agree. It does not change severity.

`pg_horizon.terminate_blockers` is only a switch. Turning it on does not terminate anyone.

```sql
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(12345);
```

That calls `pg_terminate_backend` on a backend that holds xmin, which aborts its open transaction. It will not signal slots, prepared transactions, walsenders, autovacuum, or the current backend. `force` also allows an `active` backend; it does not skip the other checks. The decision is made from a fresh read of the session's state and re-checked immediately before the signal, so a session that moved on since you looked is not terminated. The termination is logged only after it has been allowed.

## Permissions

Reading the diagnostic views does not require superuser (`GRANT SELECT` / `GRANT EXECUTE` to `PUBLIC`).

What a caller learns about *another role's session* follows `pg_stat_activity`: `state`, `backend_type`, `xact_start`, wait events, query text, and the session-specific advice (`reason`, `recommended_sql`, `is_idle_in_transaction`, `safe_to_terminate`) are shown to superusers, members of `pg_read_all_stats`, and the session's own role, and are `NULL` (query: `<insufficient privilege>`) for everyone else. The row itself, with `pid`, database, role, application name and xid/xmin, stays visible, as in `pg_stat_activity`. Replication slots and prepared transactions are public catalogs and are always shown in full.

`pg_horizon_terminate_blocker` is revoked from `PUBLIC`. The GUC is superuser-only. The function also requires superuser, `pg_signal_backend`, or that the target session belongs to a role the caller has the privileges of; the core checks in `pg_terminate_backend` apply on top (only a superuser may signal a superuser's session).

## How it works

Each call copies the relevant shared state once under `ProcArrayLock` (PGPROC entries, prepared-transaction dummy procs, the slot aggregate), applies the rules `ComputeXidHorizons()` applies to it, minus the calling backend, and attaches a reason to each holder it can see. Details, and why the core wrapper is not used, are in [docs/internals.md](docs/internals.md).

## Limitations

- Tested on PostgreSQL 16–18.
- Not installable where user C extensions are disallowed.
- Results are a snapshot at call time.
- Suggested SQL is not executed for you.
- Per-table `autovacuum_freeze_max_age`, `autovacuum_freeze_min_age` and `autovacuum_multixact_*` reloptions are not read; the global settings are used.
- Query text is clipped to 1023 bytes.
- On a standby the horizons include the querying session (see above).

## Testing

```bash
make installcheck                                               # SQL tests in sql/, then the TAP tests in t/
make installcheck REGRESS= PROVE_TESTS=t/002_horizons.pl        # one TAP file only
```

`installcheck` needs a running server with the extension installed for the SQL tests (the TAP tests start their own clusters). CI uses PostgreSQL 16, 17, and 18, plus an assertion-enabled build of 18.

The TAP tests use `VACUUM (VERBOSE)` as an oracle: the "removable cutoff" it prints is compared with the horizons pg_horizon reports, in each situation that has gone wrong before (holders in other databases, prepared transactions, logical and invalidated slots, `user_catalog_table`, the observer's own snapshot).

```bash
./scripts/docker-test.sh 16
./scripts/docker-test.sh 17
./scripts/docker-test.sh 18
./scripts/docker-test-cassert.sh      # PostgreSQL 18 built with --enable-cassert
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Security

See [SECURITY.md](SECURITY.md).

## License

PostgreSQL License (SPDX: `PostgreSQL`). Portions are derived from PostgreSQL source code and keep its copyright notice; see [LICENSE](LICENSE).

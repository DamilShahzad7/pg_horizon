# Contributing

pg_horizon is a PostgreSQL C extension. Match contrib style: tabs in C, `ereport` for user-facing errors, `palloc` for memory, no process-global mutable state beyond GUCs.

## Rules for this codebase

- Horizons must be what a `VACUUM` started elsewhere would see, so the calling backend is excluded. The core wrappers (`GetOldestNonRemovableTransactionId` and friends) include it and must not be used for the reported horizons outside recovery. The computation in `horizon_collect.c` mirrors `ComputeXidHorizons()`; if you touch it, `t/002_horizons.pl` (which compares against `VACUUM (VERBOSE)`'s `removable cutoff`) must pass on 16, 17 and 18.
- Do not recommend `VACUUM FULL` or raising `autovacuum_freeze_max_age` as a freeze fix.
- Do not auto-drop replication slots. Do not terminate wraparound autovacuum.
- Recommended SQL must be safe to paste: any statement that kills, drops, commits or rolls back is commented out.
- Never take a lock on a user relation in a diagnostic; read `pg_class` through the syscache. Do not allocate while holding `ProcArrayLock`.
- Text copied into fixed buffers goes through `horizon_copy_clip()` / `horizon_setf()`, never `strlcpy()`/`snprintf()` (they can cut a multibyte character in half).
- Activity data must be fresh (`horizon_load_activity()` clears the pgstat snapshot). Anything that decides to signal a backend must re-check immediately before signalling.
- What a caller learns about another role's session must stay aligned with `pg_stat_activity` (`b->visible`).
- New SQL functions need GRANT/REVOKE and a `COMMENT` in the install script, a matching change in the upgrade script, and a regression or TAP test.
- SQL upgrades: the library is installed before `ALTER EXTENSION UPDATE`, so a new library must work with the previous SQL. Only ever **append** OUT columns and never change the type of an existing one; change views with `CREATE OR REPLACE VIEW`. Keep `pg_horizon--<new>.sql` and `pg_horizon--<old>--<new>.sql` in step, `sql/004_upgrade.sql` compares them.
- A bug fix comes with a test that fails without it. (Check: temporarily revert the fix and run the test.)

## Build

PostgreSQL 16+ development headers and `pg_config` in `PATH`:

```
make
make install
make installcheck                                          # SQL tests, then TAP tests
make installcheck REGRESS= PROVE_TESTS=t/002_horizons.pl   # one TAP file only
```

`./scripts/docker-test.sh 18` builds against the official image. `./scripts/docker-test-cassert.sh` builds PostgreSQL 18 from source with `--enable-cassert` and runs the whole suite (assertions catch lock, memory-context and buffer-overrun mistakes that a release build hides); run it before any change to the collector.

## Pull requests

Include: the user-visible behaviour, the core API you are reading, and which test covers the change.

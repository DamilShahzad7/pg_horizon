# Security policy

pg_horizon is a C extension that runs inside the PostgreSQL server process, next to the data. We take reports about it seriously and want to hear about them privately first.

## Supported versions

Security fixes are made for the latest release line only.

| Version | Supported |
| --- | --- |
| 1.1.x | yes |
| 1.0.x | no; upgrade with `ALTER EXTENSION pg_horizon UPDATE` (see the README) |

The extension is built and tested against PostgreSQL 16, 17 and 18. A vulnerability that only exists on a PostgreSQL major version its own project no longer supports may be closed as out of scope.

## Reporting a vulnerability

**Please do not open a public issue, pull request or discussion for a suspected vulnerability.**

Report it privately with GitHub's private vulnerability reporting: go to the repository's **Security** tab and choose **Report a vulnerability** (`https://github.com/DamilShahzad7/pg_horizon/security/advisories/new`). Only the maintainers can see the report.

Please include, as far as you can:

- the pg_horizon version (`SELECT extversion FROM pg_extension WHERE extname = 'pg_horizon'`) and PostgreSQL version;
- what an attacker needs to start with (an unprivileged login role? `pg_signal_backend`? superuser?) and what they gain;
- the smallest sequence of SQL, or the input, that reproduces it;
- whether it needs non-default settings (`pg_horizon.terminate_blockers = on`, `max_prepared_transactions`, `wal_level = logical`, a standby, ...);
- your assessment of impact, and whether you plan to publish.

If you cannot use GitHub's form, open a public issue that says **only** that you have a security report and how to reach you, with no technical detail, and a maintainer will move the conversation to a private channel.

## What to expect

pg_horizon is maintained by volunteers, so these are targets, not guarantees.

| Step | Target |
| --- | --- |
| Acknowledge your report | within 5 business days |
| Initial assessment (accepted, needs more information, or declined, with reasons) | within 14 days |
| Fix or mitigation for a confirmed issue | within 90 days; sooner for high severity |
| Coordinated disclosure | with the fix, or after 90 days at the latest, whichever comes first; we will agree the date with you |

For confirmed issues we publish a GitHub security advisory (requesting a CVE when the issue warrants one), release a fixed version, describe the issue and the upgrade path in `CHANGELOG.md`, and credit you unless you ask us not to.

## Scope

**In scope**

- Memory-safety problems in the C code: buffer overruns, use after free, crashes or hangs reachable from SQL by a role that is allowed to call the function, including through crafted database, role, application, slot or prepared-transaction names, GIDs, query text, or multibyte input.
- Any way to make `pg_horizon_terminate_blocker` signal something it documents that it refuses (replication slots, prepared transactions, walsenders, autovacuum, wraparound vacuum, the current backend), to signal without `pg_horizon.terminate_blockers = on` and the required privileges, or to bypass the checks that `pg_terminate_backend` applies.
- Privilege escalation, or the GUC being turned on by a role that is not a superuser.
- Disclosure of information about another role's session (state, wait events, `xact_start`, query text, session-specific advice) to a caller who could not see it in `pg_stat_activity`.
- Injection through the advice text (`recommended_sql`, `reason`) or through the SQL the extension itself runs.
- A way for an unprivileged caller to make the diagnostics block other sessions or the server (for example by holding locks, or by unbounded work).
- Weaknesses in the build, test and release tooling in this repository that could put a malicious artifact into a build.

**Out of scope**

- Anything that already requires superuser. A superuser can load arbitrary C code and read any data, so it cannot be made to do less by this extension.
- Vulnerabilities in PostgreSQL itself; report those to the PostgreSQL project (`security@postgresql.org`, see https://www.postgresql.org/support/security/).
- Information that PostgreSQL already shows to every role: `pg_class`, `pg_database`, `pg_stat_all_tables`, `pg_replication_slots` and `pg_prepared_xacts` are public catalogs, and, as in `pg_stat_activity`, the pid, database, role, application name and xid/xmin of any session are visible to every role.
- Damage caused by running text from `recommended_sql` or `vacuum_sql` yourself. The extension only returns text; it never runs it.
- Denial of service that needs privileges the caller would already have to disrupt the server (for example a role with `pg_signal_backend` terminating sessions it is allowed to terminate).
- Reports produced only by automated scanners, without a demonstrated impact.

## Security model

What the extension does and does not do, so that you can judge a report and configure it:

- **Installation** needs a superuser: the extension is not marked `trusted`.
- **Reading** the views and functions does not need superuser. They are granted to `PUBLIC`.
- **Other roles' session details** (state, backend type, `xact_start`, wait events, query text, and the `reason` / `recommended_sql` / `is_idle_in_transaction` / `safe_to_terminate` fields for that row) are shown only to superusers, members of `pg_read_all_stats`, and the session's own role. Everyone else gets `NULL` (query text: `<insufficient privilege>`). Replication slots and prepared transactions are shown in full, as in their public catalog views.
- **Terminating** is off by default. `pg_horizon_terminate_blocker` has `EXECUTE` revoked from `PUBLIC`, only signals idle-in-transaction backends that hold an xmin (or, with `force`, an active one), never a slot, a prepared transaction, a walsender, autovacuum, wraparound vacuum or the calling backend, needs superuser, `pg_signal_backend`, or the privileges of the target's role, and inherits the core rule that only a superuser may signal a superuser's session. The state it acts on is read fresh and re-checked immediately before the signal. Every termination is logged at `LOG` level as `pg_horizon terminated pid ...`.
- **`pg_horizon.terminate_blockers`** can only be set by a superuser. Setting it before the library is loaded in a session is accepted as a placeholder but is re-checked, and reverted with a warning, when the library loads.
- **Text it produces** is text only. Literals and identifiers in it (slot names, prepared-transaction GIDs, table names) are quoted with `quote_literal_cstr` / `quote_identifier`, and destructive statements are emitted commented out. Text copied into fixed buffers is clipped on a character boundary.
- **The extension itself** opens no sockets, spawns no processes, and writes no files. It reads shared memory and catalogs, and its only side effect is the guarded `SIGTERM` above. Its one internal query is constant, schema-qualified (`pg_catalog.pg_prepared_xacts`), and run as the caller.
- **Cost.** The diagnostics take short shared locks (`ProcArrayLock` and others), and the holder listing briefly reads the whole lock table (as `pg_locks` does). They take no locks on user tables. Any role that can call them can call them repeatedly, so revoke `EXECUTE` from roles that should not (below) if that matters to you.

## Recommendations for operators

- Leave `pg_horizon.terminate_blockers` off (the default). If you need it, enable it for a specific role rather than the cluster: `ALTER ROLE dba_oncall SET pg_horizon.terminate_blockers = on;` and `GRANT EXECUTE ON FUNCTION pg_horizon_terminate_blocker(integer, boolean) TO dba_oncall;`.
- Give monitoring a dedicated role in `pg_monitor` (which includes `pg_read_all_stats`) instead of a superuser.
- If not every role should be able to run the diagnostics, revoke from `PUBLIC` and grant to a monitoring role (`monitoring` below; for example a role that is a member of `pg_monitor`):

  ```sql
  REVOKE SELECT ON pg_horizon, pg_horizon_blockers, pg_horizon_databases,
                   pg_horizon_relations, pg_horizon_check FROM PUBLIC;
  REVOKE EXECUTE ON FUNCTION pg_horizon_status(), pg_horizon_blockers(boolean),
                             pg_horizon_databases(), pg_horizon_relations(bigint),
                             pg_horizon_explain(regclass), pg_horizon_report(),
                             pg_horizon_check(), pg_horizon_vacuum_sql(regclass)
                             FROM PUBLIC;

  GRANT SELECT ON pg_horizon, pg_horizon_blockers, pg_horizon_databases,
                  pg_horizon_relations, pg_horizon_check TO monitoring;
  GRANT EXECUTE ON FUNCTION pg_horizon_status(), pg_horizon_blockers(boolean),
                            pg_horizon_databases(), pg_horizon_relations(bigint),
                            pg_horizon_explain(regclass), pg_horizon_report(),
                            pg_horizon_check(), pg_horizon_vacuum_sql(regclass)
                            TO monitoring;
  ```

  Revoke both: the views call the functions with the caller's privileges, so a role needs `EXECUTE` on the functions as well as `SELECT` on the views. (Tested: after this, an ordinary role is denied every view and function, and the monitoring role can use all of them. `ALTER EXTENSION pg_horizon UPDATE` does not touch these privileges. `DROP EXTENSION` followed by `CREATE EXTENSION` resets them to the defaults, so re-apply them after that.)
- Log at `LOG` level or above and alert on `pg_horizon terminated pid`.
- Build the extension from a release tag you have reviewed, on a machine you trust, against the `pg_config` of the server that will load it. Do not install prebuilt binaries from an untrusted source: a C extension runs with the server's privileges.
- Keep PostgreSQL and pg_horizon on supported versions, and read `CHANGELOG.md` when upgrading.

## Supply chain of this repository

- CI runs with a read-only `GITHUB_TOKEN`, and third-party GitHub Actions are pinned to a full commit SHA. Dependabot proposes updates to the pinned actions and to the Docker base images (`.github/dependabot.yml`).
- The PostgreSQL source used for the assertion-enabled test build is downloaded over HTTPS and verified against the SHA-256 published by the PostgreSQL project before it is unpacked.
- The extension has no runtime dependencies beyond PostgreSQL itself.

## Safe harbour

If you make a good-faith effort to follow this policy (test only against systems you own or are authorised to test, avoid privacy violations and data destruction, and give us reasonable time to fix an issue before disclosing it), we will not pursue or support legal action against you for your research, and we will work with you to understand and resolve the issue.

#!/usr/bin/env bash
# Build PostgreSQL from source with --enable-cassert, build pg_horizon against
# it, and run the SQL regression tests and every TAP test with assertions on.
# The first run compiles PostgreSQL (several minutes); the image is cached
# afterwards.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PG_VERSION="${1:-18.6}"
IMAGE="pg_horizon:cassert-${PG_VERSION}"

cd "$ROOT"

echo "==> Building PostgreSQL ${PG_VERSION} with assertions"
docker build -f docker/Dockerfile.cassert --build-arg PG_VERSION="${PG_VERSION}" -t "${IMAGE}" .

echo "==> Building pg_horizon and running SQL + TAP tests (assertions on)"
docker run --rm "${IMAGE}" bash -c '
  set -euo pipefail
  su postgres -c "cd /work && make USE_PGXS=1 PG_CONFIG=/opt/pg/bin/pg_config"
  make -C /work USE_PGXS=1 PG_CONFIG=/opt/pg/bin/pg_config install

  # SQL regression tests against a running server
  su postgres -c "
    set -euo pipefail
    initdb -D /tmp/pgdata -A trust >/dev/null
    pg_ctl -D /tmp/pgdata -l /tmp/pg.log -w start \
      -o \"-c max_prepared_transactions=8 -c max_replication_slots=8 -c max_wal_senders=8 -c wal_level=logical -c autovacuum_naptime=3600\" >/dev/null
    cd /work
    \$PG_SRC/src/test/regress/pg_regress --inputdir=./ --bindir=/opt/pg/bin \
      --dbname=contrib_regression 001_basic 002_explain 003_permissions 004_upgrade 005_edge
    pg_ctl -D /tmp/pgdata -m fast stop >/dev/null
    if grep -qE \"TRAP:|PANIC\" /tmp/pg.log; then echo \"assertion failure in server log\"; cat /tmp/pg.log; exit 1; fi
  "

  # TAP tests: each builds its own clusters from PATH
  su postgres -c "
    set -euo pipefail
    cd /work
    rm -rf tmp_check && mkdir -p tmp_check
    export TESTLOGDIR=/work/tmp_check/log TESTDATADIR=/work/tmp_check
    export PG_REGRESS=\$PG_SRC/src/test/regress/pg_regress
    prove -I \$PG_SRC/src/test/perl -I ./ t/*.pl
    if grep -rlE \"TRAP:|PANIC\" /work/tmp_check; then echo \"assertion failure in a TAP node log\"; exit 1; fi
  "
'
echo "==> pg_horizon tests passed on PostgreSQL ${PG_VERSION} with assertions"

#!/usr/bin/env bash
# Build pg_horizon against official postgres:MAJOR and run smoke + installcheck.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PG_MAJOR="${1:-18}"
IMAGE="pg_horizon:test-${PG_MAJOR}"
NAME="pg_horizon_test_${PG_MAJOR}_$$"

cd "$ROOT"

echo "==> Building image for PostgreSQL ${PG_MAJOR}"
docker build --build-arg PG_MAJOR="${PG_MAJOR}" -t "${IMAGE}" .

echo "==> Starting postgres"
docker run -d --rm \
  --name "${NAME}" \
  -e POSTGRES_HOST_AUTH_METHOD=trust \
  "${IMAGE}" >/dev/null

cleanup() {
  docker stop "${NAME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> Waiting for readiness"
for i in $(seq 1 60); do
  if docker exec "${NAME}" pg_isready -U postgres >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
docker exec "${NAME}" pg_isready -U postgres

echo "==> Smoke tests"
docker exec -u postgres "${NAME}" /usr/local/bin/pg-horizon-test

echo "==> Regression tests"
docker exec -u postgres -e PGUSER=postgres --workdir /usr/src/pg_horizon "${NAME}" \
  make installcheck || {
    echo "installcheck failed; dumping regression.diffs and TAP logs if present"
    docker exec "${NAME}" bash -c 'cat /usr/src/pg_horizon/regression.diffs 2>/dev/null || true'
    docker exec "${NAME}" bash -c 'find /usr/src/pg_horizon/tmp_check -name "*.log" -print -exec tail -n 80 {} \; 2>/dev/null || true'
    exit 1
  }

echo "==> pg_horizon tests passed on PostgreSQL ${PG_MAJOR}"

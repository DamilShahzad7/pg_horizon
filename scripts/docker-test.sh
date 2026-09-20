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

# The official image first starts a temporary server that listens on the unix
# socket only, runs the init scripts, stops it, and then starts the real one.
# pg_isready over the socket succeeds against the temporary server, so wait on
# TCP, which only the real server serves.
echo "==> Waiting for readiness"
ready=0
for i in $(seq 1 90); do
  if docker exec "${NAME}" pg_isready -U postgres -h 127.0.0.1 >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done
if [ "${ready}" -ne 1 ]; then
  echo "postgres did not become ready" >&2
  docker logs "${NAME}" >&2 || true
  exit 1
fi

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

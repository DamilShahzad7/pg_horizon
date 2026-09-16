#!/bin/bash
# Run inside the postgres container after the server is ready.
set -euo pipefail

psql -v ON_ERROR_STOP=1 -d postgres -c "CREATE EXTENSION IF NOT EXISTS pg_horizon;"
psql -v ON_ERROR_STOP=1 -d postgres -c "SELECT severity, xid_age > 0 AS has_age, xid_headroom > 0 AS has_headroom FROM pg_horizon;"
psql -v ON_ERROR_STOP=1 -d postgres -c "SELECT count(*) >= 1 FROM pg_horizon_databases;"
psql -v ON_ERROR_STOP=1 -d postgres <<'SQL'
CREATE TABLE IF NOT EXISTS horizon_smoke(id int);
INSERT INTO horizon_smoke SELECT generate_series(1,5);
SELECT freeze_constraint IN ('ok','horizon','vacuum_lag')
FROM pg_horizon_explain('horizon_smoke');
SQL

echo "pg_horizon smoke tests passed"

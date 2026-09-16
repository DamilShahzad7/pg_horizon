ARG PG_MAJOR=18
FROM postgres:${PG_MAJOR}

ARG PG_MAJOR
USER root

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        libipc-run-perl \
        postgresql-server-dev-${PG_MAJOR} \
    && rm -rf /var/lib/apt/lists/*

COPY . /usr/src/pg_horizon
WORKDIR /usr/src/pg_horizon

RUN make clean && make && make install \
    && chown -R postgres:postgres /usr/src/pg_horizon

COPY docker/run-tests.sh /usr/local/bin/pg-horizon-test
RUN chmod +x /usr/local/bin/pg-horizon-test \
    && cp docker/init.sql /docker-entrypoint-initdb.d/zzz-pg-horizon.sql

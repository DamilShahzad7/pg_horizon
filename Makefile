#-------------------------------------------------------------------------
#
# Makefile for pg_horizon
#
#-------------------------------------------------------------------------

MODULE_big = pg_horizon
OBJS = \
	$(WIN32RES) \
	pg_horizon.o \
	horizon_collect.o \
	horizon_funcs.o \
	horizon_action.o

EXTENSION = pg_horizon
DATA = pg_horizon--1.0.sql pg_horizon--1.0--1.1.sql pg_horizon--1.1.sql
PGFILEDESC = "pg_horizon - XID/MultiXact freeze horizon diagnosis"

REGRESS = 001_basic 002_explain 003_permissions 004_upgrade 005_edge
TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

.PHONY: docker-test
docker-test:
	chmod +x scripts/docker-test.sh docker/run-tests.sh
	./scripts/docker-test.sh 18

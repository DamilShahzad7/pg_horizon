/*-------------------------------------------------------------------------
 *
 * pg_horizon.h
 *	  XID / MultiXact horizon diagnosis for PostgreSQL
 *
 * Horizons are read from the same exported backend functions VACUUM uses
 * (GetOldestNonRemovableTransactionId). Catalog xmin is taken from a
 * non-shared catalog (pg_proc), not from pg_class, which is shared and would
 * return the shared horizon. Blockers are attributed by walking PGPROC,
 * prepared-xact dummy procs, and replication slots under the documented
 * shared-memory locks — not by stitching pg_stat_activity SQL together after
 * the fact.
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 * IDENTIFICATION
 *	  pg_horizon.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_HORIZON_H
#define PG_HORIZON_H

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"
#include "storage/proc.h"

#if PG_VERSION_NUM < 160000
#error "pg_horizon requires PostgreSQL 16 or later"
#endif

/*
 * Same definition procarray.c uses. Not exported in a public header.
 */
#ifndef UINT32_ACCESS_ONCE
#define UINT32_ACCESS_ONCE(var) ((uint32) (*((volatile uint32 *) &(var))))
#endif

/* Keep a hard cap so a pathological cluster cannot blow up a tuplestore. */
#define HORIZON_MAX_WAITERS			16
#define HORIZON_QUERY_CHARS			1024
#define HORIZON_REASON_CHARS			1024
#define HORIZON_SQL_CHARS			2048
#define HORIZON_NAME_CHARS			NAMEDATALEN

typedef enum HorizonSeverity
{
	HORIZON_SEV_OK = 0,
	HORIZON_SEV_WARNING,
	HORIZON_SEV_CRITICAL,
	HORIZON_SEV_EMERGENCY
} HorizonSeverity;

typedef enum HorizonBlockerType
{
	HORIZON_BLK_BACKEND = 0,
	HORIZON_BLK_PREPARED,
	HORIZON_BLK_PHYSICAL_SLOT,
	HORIZON_BLK_LOGICAL_SLOT,
	HORIZON_BLK_STANDBY_FEEDBACK
} HorizonBlockerType;

typedef struct HorizonBlocker
{
	HorizonBlockerType type;
	int			pid;			/* 0 for prepared xacts and inactive slots */
	TransactionId xid;
	TransactionId xmin;
	TransactionId catalog_xmin;
	Oid			databaseId;
	Oid			roleId;
	uint8		statusFlags;
	uint32		wait_event_info;
	bool		affects_data;
	bool		affects_catalog;
	bool		is_horizon_holder;
	bool		is_observer;	/* the backend running this query */
	bool		holds_waited_lock;
	int			nwaiters;
	int			waiter_pids[HORIZON_MAX_WAITERS];
	char		slot_name[HORIZON_NAME_CHARS];
	char		plugin[HORIZON_NAME_CHARS];
	char		prepared_gid[GIDSIZE];
	int64		wal_retained_bytes;
	bool		slot_active;
	pid_t		slot_active_pid;
	XLogRecPtr	restart_lsn;
	char		state[32];
	char		backend_type[64];
	char		application_name[NAMEDATALEN];
	char		usename[NAMEDATALEN];
	char		datname[NAMEDATALEN];
	char		query[HORIZON_QUERY_CHARS];
	char		wait_event_type[64];
	char		wait_event[64];
	TimestampTz xact_start;
	TimestampTz proc_start;
	char		reason[HORIZON_REASON_CHARS];
	char		recommended_sql[HORIZON_SQL_CHARS];
} HorizonBlocker;

typedef struct HorizonSnapshot
{
	TransactionId next_xid;
	TransactionId oldest_xid;
	Oid			oldest_xid_db;
	TransactionId xid_vac_limit;
	TransactionId xid_warn_limit;
	TransactionId xid_stop_limit;
	TransactionId xid_wrap_limit;
	TransactionId shared_xmin;
	TransactionId catalog_xmin;
	TransactionId data_xmin;
	TransactionId oldest_running_xid;
	TransactionId slot_xmin;
	TransactionId slot_catalog_xmin;
	MultiXactId next_mxid;
	MultiXactId oldest_mxid;
	bool		in_recovery;
	int			nblockers;
	int			maxblockers;
	HorizonBlocker *blockers;
} HorizonSnapshot;

/* GUCs */
extern int	pg_horizon_min_xmin_age;
extern bool pg_horizon_terminate_blockers;

/* Collection */
extern HorizonSnapshot *horizon_snapshot_collect(void);
extern void horizon_snapshot_free(HorizonSnapshot *snap);
extern int64 horizon_xid_age(TransactionId xid, TransactionId next_xid);
extern int64 horizon_xid_remaining(TransactionId now, TransactionId limit);
extern int64 horizon_blocker_age(const HorizonBlocker *b, TransactionId next_xid);
extern HorizonSeverity horizon_compute_severity(const HorizonSnapshot *snap);
extern const char *horizon_severity_name(HorizonSeverity sev);
extern const char *horizon_blocker_type_name(HorizonBlockerType t);
extern bool horizon_has_stat_visibility(Oid roleId);
extern bool horizon_blocker_safe_to_terminate(const HorizonBlocker *b,
											  bool force);

/* SQL helpers shared by SRFs */
extern void horizon_fill_blocker_reason(HorizonBlocker *b,
										const HorizonSnapshot *snap);

#endif							/* PG_HORIZON_H */

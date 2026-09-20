/*-------------------------------------------------------------------------
 *
 * pg_horizon.h
 *	  XID / MultiXact horizon diagnosis for PostgreSQL
 *
 * The xmin horizons are computed the way ComputeXidHorizons() computes them
 * (see horizon_collect.c), from one copy of PGPROC / prepared-transaction
 * state taken under ProcArrayLock, but *excluding the calling backend*. The
 * core function GetOldestNonRemovableTransactionId() includes the caller's own
 * snapshot xmin, which is inherited from any running XID in any database, so
 * it cannot be used to say what a VACUUM started elsewhere would see.
 *
 * Blockers are attributed by walking PGPROC, prepared-xact dummy procs, and
 * replication slots, not by stitching pg_stat_activity SQL together after the
 * fact.
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
#include "catalog/pg_class.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"
#include "storage/proc.h"

#if PG_VERSION_NUM < 160000
#error "pg_horizon requires PostgreSQL 16 or later"
#endif

/* Keep a hard cap so a pathological cluster cannot blow up a tuplestore. */
#define HORIZON_MAX_WAITERS			16
#define HORIZON_QUERY_CHARS			1024
#define HORIZON_REASON_CHARS			1024
#define HORIZON_SQL_CHARS			2048
#define HORIZON_NAME_CHARS			NAMEDATALEN

/*
 * pg_horizon's own severity thresholds. Everything else comes from the core
 * limits (xidVacLimit / xidWarnLimit / xidStopLimit) or from GUCs.
 */
#define HORIZON_EMERGENCY_HEADROOM	10000000	/* XIDs left before stop limit */
#define HORIZON_STALE_HOLDER_AGE	1000000		/* idle xact / inactive slot */

/* What horizon_snapshot_collect() should gather. Each flag needs the ones above it. */
#define HORIZON_COLLECT_LIMITS		0x01	/* XID/MultiXact limits and ages */
#define HORIZON_COLLECT_HORIZONS	0x02	/* xmin horizons (one PGPROC scan) */
#define HORIZON_COLLECT_BLOCKERS	0x04	/* holder rows, activity, names */
#define HORIZON_COLLECT_DETAILS		0x08	/* prepared gids, lock waiters, reasons */
#define HORIZON_COLLECT_ALL			0x0F

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

/* Same split as GlobalVisHorizonKindForRel(). */
typedef enum HorizonKind
{
	HORIZON_KIND_SHARED = 0,
	HORIZON_KIND_CATALOG,
	HORIZON_KIND_DATA,
	HORIZON_KIND_TEMP
} HorizonKind;

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
	bool		holds_shared;	/* pins the shared horizon */
	bool		holds_catalog;	/* pins the catalog horizon of this database */
	bool		holds_data;		/* pins the data horizon of this database */
	bool		is_horizon_holder;	/* holds_shared || holds_catalog || holds_data */
	bool		is_observer;	/* the backend running this query */
	bool		visible;		/* caller may see session details */
	bool		holds_waited_lock;
	int			nwaiters;
	int			waiter_pids[HORIZON_MAX_WAITERS];
	int64		xmin_age;		/* age of min(xid, xmin); -1 if neither set */
	int64		catalog_age;	/* age of catalog_xmin; -1 if not set */
	int64		age;			/* oldest of the above, 0 if none */
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
	int			flags;
	Oid			database;		/* MyDatabaseId: data/catalog horizons are for it */
	bool		in_recovery;

	/* HORIZON_COLLECT_LIMITS */
	TransactionId next_xid;
	uint32		next_xid_epoch;
	TransactionId oldest_xid;
	Oid			oldest_xid_db;
	TransactionId xid_vac_limit;
	TransactionId xid_warn_limit;
	TransactionId xid_stop_limit;
	TransactionId xid_wrap_limit;
	MultiXactId next_mxid;
	MultiXactId oldest_mxid;	/* wraparound frontier: min(datminmxid) */

	/* HORIZON_COLLECT_HORIZONS */
	TransactionId shared_xmin;
	TransactionId catalog_xmin;
	TransactionId data_xmin;
	TransactionId temp_xmin;
	TransactionId oldest_running_xid;
	TransactionId slot_xmin;
	TransactionId slot_catalog_xmin;
	MultiXactId mxid_horizon;	/* GetOldestMultiXactId(): what VACUUM uses */

	/* HORIZON_COLLECT_BLOCKERS */
	int			nblockers;
	int			maxblockers;
	HorizonBlocker **blockers;
} HorizonSnapshot;

/* GUCs */
extern int	pg_horizon_min_xmin_age;
extern bool pg_horizon_terminate_blockers;

/* Collection (horizon_collect.c) */
extern HorizonSnapshot *horizon_snapshot_collect(int flags);
extern void horizon_snapshot_free(HorizonSnapshot *snap);

/* Helpers (pg_horizon.c) */
extern int64 horizon_xid_age(TransactionId xid, TransactionId next_xid);
extern int64 horizon_xid_remaining(TransactionId now, TransactionId limit);
extern HorizonSeverity horizon_compute_severity(const HorizonSnapshot *snap,
												char **why);
extern const char *horizon_severity_name(HorizonSeverity sev);
extern const char *horizon_blocker_type_name(HorizonBlockerType t);
extern bool horizon_has_stat_visibility(Oid roleId);
extern bool horizon_blocker_is_idle_xact(const HorizonBlocker *b);
extern bool horizon_blocker_safe_to_terminate(const HorizonBlocker *b,
											  bool force);
extern bool horizon_blocker_listed(const HorizonBlocker *b);
extern int	horizon_count_listed(const HorizonSnapshot *snap, bool holders_only);
extern HorizonBlocker *horizon_dominant_blocker(const HorizonSnapshot *snap,
												bool have_kind, HorizonKind kind);
extern void horizon_copy_clip(char *dst, size_t dstsize, const char *src);
extern void horizon_fill_blocker_reason(HorizonBlocker *b,
										const HorizonSnapshot *snap);

/* Relation classification (horizon_funcs.c uses these) */
extern HorizonKind horizon_kind_for_class(Oid relid, Oid relnamespace,
										  bool relisshared, char relpersistence,
										  bool user_catalog,
										  const HorizonSnapshot *snap);
extern TransactionId horizon_xmin_for_kind(const HorizonSnapshot *snap,
										   HorizonKind kind);

#endif							/* PG_HORIZON_H */

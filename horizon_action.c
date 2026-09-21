/*-------------------------------------------------------------------------
 *
 * horizon_action.c
 *	  Guarded terminate of idle-in-transaction xmin holders
 *
 * This issues SIGTERM through the same path as pg_terminate_backend, after
 * refusing slots, prepared xacts, wraparound vacuum, and walsenders. It is
 * not a substitute for COMMIT/ROLLBACK of a live application transaction.
 *
 * The decision is made from a fresh activity read, and re-checked from a
 * second fresh read immediately before signalling, so a session that has moved
 * on (committed and started a new query, say) since it was first seen is not
 * killed on the strength of stale state. A window of microseconds remains
 * between the second read and the signal; that is inherent to signalling a
 * pid and is why the target set is deliberately narrow.
 *
 * Copyright (c) 2026, pg_horizon authors
 *
 * IDENTIFICATION
 *	  horizon_action.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

#include "pg_horizon.h"

PG_FUNCTION_INFO_V1(pg_horizon_terminate_blocker);

#define HORIZON_TERMINATE_FLAGS \
	(HORIZON_COLLECT_LIMITS | HORIZON_COLLECT_HORIZONS | HORIZON_COLLECT_BLOCKERS)

static HorizonBlocker *
horizon_find_pid(HorizonSnapshot *snap, int pid)
{
	int			i;

	for (i = 0; i < snap->nblockers; i++)
	{
		if (snap->blockers[i]->pid == pid)
			return snap->blockers[i];
	}
	return NULL;
}

/* Copy the parts of a blocker we need once its snapshot is freed. */
typedef struct HorizonTarget
{
	HorizonBlockerType type;
	char		slot_name[HORIZON_NAME_CHARS];
	char		state[32];
	Oid			roleId;
	int64		age;
} HorizonTarget;

Datum
pg_horizon_terminate_blocker(PG_FUNCTION_ARGS)
{
	int32		pid;
	bool		force = false;
	HorizonSnapshot *snap;
	HorizonBlocker *found;
	HorizonTarget target;
	Datum		result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	pid = PG_GETARG_INT32(0);
	if (!PG_ARGISNULL(1))
		force = PG_GETARG_BOOL(1);

	if (!pg_horizon_terminate_blockers)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_horizon.terminate_blockers is off"),
				 errhint("Set pg_horizon.terminate_blockers = on as a superuser after you have identified an abandoned session. This GUC never drops replication slots or prepared transactions.")));

	if (pid <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pid must be a live backend, not a prepared transaction or slot")));

	if (pid == MyProcPid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("refusing to terminate the current backend")));

	snap = horizon_snapshot_collect(HORIZON_TERMINATE_FLAGS);
	found = horizon_find_pid(snap, pid);

	if (found == NULL)
	{
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pid %d is not holding an xmin/xid that pg_horizon can see", pid),
				 errhint("Terminate ordinary backends with pg_terminate_backend; this function is only for xmin holders.")));
	}

	if (found->type == HORIZON_BLK_PHYSICAL_SLOT ||
		found->type == HORIZON_BLK_LOGICAL_SLOT)
	{
		char		slot[HORIZON_NAME_CHARS];

		strlcpy(slot, found->slot_name, sizeof(slot));
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("refusing to drop or signal a replication slot"),
				 errdetail("Slot \"%s\" must be consumed or dropped with pg_drop_replication_slot after you confirm the replica or consumer is gone.",
						   slot)));
	}

	if (found->type == HORIZON_BLK_PREPARED)
	{
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prepared transactions cannot be killed by pid"),
				 errhint("Use COMMIT PREPARED or ROLLBACK PREPARED on the gid.")));
	}

	if (!horizon_blocker_safe_to_terminate(found, force))
	{
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pid %d is not a session pg_horizon will terminate", pid),
				 errdetail("This function only signals idle-in-transaction backends that hold xmin. It never signals walsenders, autovacuum, wraparound vacuum, replication slots, or prepared transactions."),
				 force ? 0 :
				 errhint("Cancel a live query with pg_cancel_backend first; use force => true only if cancel is ignored.")));
	}

	/*
	 * Same privilege model as pg_signal_backend(): a superuser, a member of
	 * pg_signal_backend, or a role that has the target's privileges. Core
	 * re-checks (and additionally refuses superuser targets for anyone but a
	 * superuser) inside pg_terminate_backend().
	 */
	if (!superuser() && !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
	{
		if (!OidIsValid(found->roleId) || !has_privs_of_role(GetUserId(), found->roleId))
		{
			horizon_snapshot_free(snap);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be a member of pg_signal_backend to terminate another role's session")));
		}
	}

	target.type = found->type;
	strlcpy(target.slot_name, found->slot_name, sizeof(target.slot_name));
	strlcpy(target.state, found->state, sizeof(target.state));
	target.roleId = found->roleId;
	target.age = found->age;
	horizon_snapshot_free(snap);

	/* Second, fresh look right before signalling. */
	snap = horizon_snapshot_collect(HORIZON_TERMINATE_FLAGS);
	found = horizon_find_pid(snap, pid);
	if (found == NULL ||
		found->type != target.type ||
		found->roleId != target.roleId ||
		!horizon_blocker_safe_to_terminate(found, force))
	{
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pid %d changed state while it was being checked; not terminating", pid),
				 errhint("Look at it again with pg_horizon_blockers.")));
	}
	horizon_snapshot_free(snap);

	if (BackendPidGetProc(pid) == NULL)
		PG_RETURN_BOOL(false);

	/* Same signal path as pg_terminate_backend(), including Windows. */
	result = DirectFunctionCall2(pg_terminate_backend,
								 Int32GetDatum(pid),
								 Int64GetDatum(0));

	/* Only claim it after the core permission checks have let it through. */
	if (DatumGetBool(result))
		ereport(LOG,
				(errmsg("pg_horizon terminated pid %d (%s, xmin age %lld)",
						pid, target.state, (long long) target.age)));

	PG_RETURN_DATUM(result);
}

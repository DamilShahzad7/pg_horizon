/*-------------------------------------------------------------------------
 *
 * horizon_action.c
 *	  Guarded terminate of idle-in-transaction xmin holders
 *
 * This issues SIGTERM through the same path as pg_terminate_backend, after
 * refusing slots, prepared xacts, wraparound vacuum, and walsenders. It is
 * not a substitute for COMMIT/ROLLBACK of a live application transaction.
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

Datum
pg_horizon_terminate_blocker(PG_FUNCTION_ARGS)
{
	int32		pid;
	bool		force = false;
	HorizonSnapshot *snap;
	HorizonBlocker *found = NULL;
	int			i;
	PGPROC	   *proc;

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

	snap = horizon_snapshot_collect();
	for (i = 0; i < snap->nblockers; i++)
	{
		if (snap->blockers[i].pid == pid)
		{
			found = &snap->blockers[i];
			break;
		}
	}

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
		horizon_snapshot_free(snap);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("refusing to drop or signal a replication slot"),
				 errdetail("Slot \"%s\" must be consumed or dropped with pg_drop_replication_slot after you confirm the replica or consumer is gone.",
						   found->slot_name)));
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
				 errdetail("This function only signals idle-in-transaction backends that hold xmin%s. It never signals walsenders, autovacuum, wraparound vacuum, replication slots, or prepared transactions.",
						   force ? "" : ". Cancel a live query with pg_cancel_backend first; force => true only if cancel is ignored")));
	}

	/* Same privilege model as pg_signal_backend(). */
	if (!superuser() && !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
	{
		if (!OidIsValid(found->roleId) || found->roleId != GetUserId())
		{
			horizon_snapshot_free(snap);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be a member of pg_signal_backend to terminate another role's session")));
		}
	}

	proc = BackendPidGetProc(pid);
	if (proc == NULL)
	{
		horizon_snapshot_free(snap);
		PG_RETURN_BOOL(false);
	}

	ereport(LOG,
			(errmsg("pg_horizon terminating pid %d (%s, xmin age %lld)",
					pid,
					found->state,
					(long long) horizon_xid_age(TransactionIdOlder(found->xmin, found->xid),
												snap->next_xid))));

	horizon_snapshot_free(snap);

	/* Same signal path as pg_terminate_backend(), including Windows. */
	PG_RETURN_DATUM(DirectFunctionCall2(pg_terminate_backend,
									   Int32GetDatum(pid),
									   Int64GetDatum(0)));
}

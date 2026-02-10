/*-------------------------------------------------------------------------
 *
 * execMain_px.c
 *	  Standard execution function for PolarDB PX.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/executor/execMain_px.c
 *
 *-------------------------------------------------------------------------
 */

#include "access/xlogrecovery.h"
#include "utils/ps_status.h"
#include "px/px_disp.h"
#include "px/px_adaptive_paging.h"
#include "px/memquota.h"

/*
 * use XLogWriteRecPtr instead of XLogInsertRecPtr, because XLogInsertRecPtr not
 * equal to XLogReplayRecPtr after switch xlog
 */
#define polar_px_max_valid_lsn() \
	((polar_is_primary() && !RecoveryInProgress()) ? \
		GetXLogWriteRecPtr() : GetXLogReplayRecPtr(NULL))

/* ----------------------------------------------------------------
 *		POLAR px functions
 * ----------------------------------------------------------------
 */

bool
should_px_executor(QueryDesc *queryDesc)
{
	return queryDesc &&
		queryDesc->plannedstmt &&
		PLANGEN_PX == queryDesc->plannedstmt->planGen;
}

void
standard_ExecutorStart_PX(QueryDesc *queryDesc, int eflags)
{
	EState	   *estate;
	MemoryContext oldcontext;
	bool		shouldDispatch;
	PxExecIdentity exec_identity;

	/* sanity checks: queryDesc must not be started already */
	Assert(queryDesc != NULL);
	Assert(queryDesc->estate == NULL);

	if (px_role == PX_ROLE_QC)
	{
		if (!IS_PX_SETUP_DONE())
		{
			px_setup();
			on_proc_exit(px_cleanup, 0);
		}
		if (!IS_PX_ADPS_SETUP_DONE())
			pxdisp_createPqThread();
	}

	/* TODO: enable it after resource manager integration */
#if 0
	/* distribute memory to operators. */
	if (px_role == PX_ROLE_QC)
	{
		if (!IsResManagerMemoryPolicyNone() &&
			LogResManagerMemory())
		{
			elog(PX_RESMANAGER_MEMORY_LOG_LEVEL, "query requested %.0fKB of memory",
				 (double) queryDesc->plannedstmt->query_mem / 1024.0);
		}
	}
#endif

	/*
	 * If the transaction is read-only, we need to check if any writes are
	 * planned to non-temporary tables.  EXPLAIN is considered read-only.
	 *
	 * Don't allow writes in parallel mode.  Supporting UPDATE and DELETE
	 * would require (a) storing the combocid hash in shared memory, rather
	 * than synchronizing it just once at the start of parallelism, and (b) an
	 * alternative to heap_update()'s reliance on xmax for mutual exclusion.
	 * INSERT may have no such troubles, but we forbid it to simplify the
	 * checks.
	 *
	 * We have lower-level defenses in CommandCounterIncrement and elsewhere
	 * against performing unsafe operations in parallel mode, but this gives a
	 * more user-friendly error message.
	 */

	if ((XactReadOnly || IsInParallelMode() ||
		 (!MyProc->issuper && polar_force_trans_ro_non_sup)) &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecCheckXactReadOnly(queryDesc->plannedstmt);

	/*
	 * Build EState, switch into per-query memory context for startup.
	 */
	estate = CreateExecutorState();
	queryDesc->estate = estate;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Fill in external parameters, if any, from queryDesc; and allocate
	 * workspace for internal parameters
	 */
	estate->es_param_list_info = queryDesc->params;

	if (queryDesc->plannedstmt->nParamExec > 0)
	{
		int			nParamExec;

		nParamExec = queryDesc->plannedstmt->nParamExec;
		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(nParamExec * sizeof(ParamExecData));
	}

	/* We now require all callers to provide sourceText */
	Assert(queryDesc->sourceText != NULL);
	estate->es_sourceText = queryDesc->sourceText;

	/*
	 * Fill in the query environment, if any, from queryDesc.
	 */
	estate->es_queryEnv = queryDesc->queryEnv;

	/*
	 * If non-read-only query, set the command ID to mark output tuples with
	 */
	switch (queryDesc->operation)
	{
		case CMD_SELECT:

			/*
			 * SELECT FOR [KEY] UPDATE/SHARE and modifying CTEs need to mark
			 * tuples
			 */
			if (queryDesc->plannedstmt->rowMarks != NIL ||
				queryDesc->plannedstmt->hasModifyingCTE)
				estate->es_output_cid = GetCurrentCommandId(true);

			/*
			 * A SELECT without modifying CTEs can't possibly queue triggers,
			 * so force skip-triggers mode. This is just a marginal efficiency
			 * hack, since AfterTriggerBeginQuery/AfterTriggerEndQuery aren't
			 * all that expensive, but we might as well do it.
			 */
			if (!queryDesc->plannedstmt->hasModifyingCTE)
				eflags |= EXEC_FLAG_SKIP_TRIGGERS;
			break;

		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_UPDATE:
		case CMD_MERGE:
			estate->es_output_cid = GetCurrentCommandId(true);
			break;

		default:
			elog(ERROR, "unrecognized operation code: %d",
				 (int) queryDesc->operation);
			break;
	}

	/*
	 * Copy other important information into the EState
	 */
	estate->es_snapshot = RegisterSnapshot(queryDesc->snapshot);
	estate->es_crosscheck_snapshot = RegisterSnapshot(queryDesc->crosscheck_snapshot);
	estate->es_top_eflags = eflags;
	estate->es_instrument = queryDesc->instrument_options;
	estate->es_jit_flags = queryDesc->plannedstmt->jitFlags;
	/* POLAR px */
	estate->showstatctx = queryDesc->showstatctx;
	/* POLAR end */

	/*
	 * Shared input info is needed when ROLE_EXECUTE or sequential plan
	 */
	estate->es_sharenode = NIL;

	/*
	 * Handling of the Slice table depends on context.
	 */
	if (px_role == PX_ROLE_QC)
	{
		/* Set up the slice table. */
		SliceTable *sliceTable;

		sliceTable = InitSliceTable(estate, queryDesc->plannedstmt);
		estate->es_sliceTable = sliceTable;

		if (sliceTable->slices[0].gangType != GANGTYPE_UNALLOCATED ||
			sliceTable->hasMotions)
		{
			if (queryDesc->ddesc == NULL)
			{
				queryDesc->ddesc = makeNode(QueryDispatchDesc);
				queryDesc->ddesc->useChangedAOOpts = true;
			}

			/* Pass EXPLAIN ANALYZE flag to qExecs. */
			estate->es_sliceTable->instrument_options = queryDesc->instrument_options;

			/* set our global sliceid variable for elog. */
			currentSliceId = LocallyExecutingSliceIndex(estate);

			/*
			 * InitPlan() will acquire locks by walking the entire plan tree
			 * we'd like to avoid acquiring the locks until *after* we've set
			 * up the interconnect
			 */
			if (estate->es_sliceTable->hasMotions)
				estate->motionlayer_context = createMotionLayerState(queryDesc->plannedstmt->numSlices - 1);

			shouldDispatch = !(eflags & EXEC_FLAG_EXPLAIN_ONLY);
		}
		else
		{
			/* QC-only query, no dispatching required */
			shouldDispatch = false;
		}
		/* update the ps status */
		set_ps_display(CreateCommandName((Node *) queryDesc->plannedstmt));
	}
	else if (px_role == PX_ROLE_PX)
	{
		QueryDispatchDesc *ddesc = queryDesc->ddesc;

		shouldDispatch = false;

		/* qDisp should have sent us a slice table via POLARPX */
		if (ddesc && ddesc->sliceTable != NULL)
		{
			SliceTable *sliceTable;
			ExecSlice  *slice;

			sliceTable = ddesc->sliceTable;
			Assert(IsA(sliceTable, SliceTable));
			slice = &sliceTable->slices[sliceTable->localSlice];

			estate->es_sliceTable = sliceTable;
			estate->es_cursorPositions = ddesc->cursorPositions;

			estate->currentSliceId = slice->rootIndex;

			/* set our global sliceid variable for elog. */
			currentSliceId = LocallyExecutingSliceIndex(estate);

			/* Should we collect statistics for EXPLAIN ANALYZE? */
			estate->es_instrument = sliceTable->instrument_options;
			queryDesc->instrument_options = sliceTable->instrument_options;

			/*
			 * InitPlan() will acquire locks by walking the entire plan tree
			 * we'd like to avoid acquiring the locks until *after* we've set
			 * up the interconnect
			 */
			if (estate->es_sliceTable->hasMotions)
			{
				estate->motionlayer_context = createMotionLayerState(queryDesc->plannedstmt->numSlices - 1);

				PG_TRY();
				{
					/*
					 * Initialize the motion layer for this query.
					 */
					Assert(!estate->interconnect_context);
					SetupInterconnect(estate);
					UpdateMotionExpectedReceivers(estate->motionlayer_context, estate->es_sliceTable);

					Assert(estate->interconnect_context);
				}
				PG_CATCH();
				{
					ExecutorCleanup_PX(queryDesc);
					PG_RE_THROW();
				}
				PG_END_TRY();
			}
		}
		else
		{
			/* local query in PX. */
		}
	}
	else
		shouldDispatch = false;

	/*
	 * We don't eliminate aliens if we don't have an PX plan or we are
	 * executing on master.
	 */
#ifdef FAULT_INJECTOR
	INJECT_PX_HANG_FOR_SECOND("pdml_deadlock_creation", "", "pdml_test_table", 20);
#endif
	estate->eliminateAliens = (px_execute_pruned_plan &&
							   estate->es_sliceTable &&
							   estate->es_sliceTable->hasMotions &&
							   px_role != PX_ROLE_QC);

	/*
	 * Assign a Motion Node to every Plan Node. This makes it easy to identify
	 * which slice any Node belongs to
	 */
	AssignParentMotionToPlanNodes(queryDesc->plannedstmt);

	/*
	 * If the interconnect has been set up; we need to catch any errors to
	 * shut it down -- so we have to wrap InitPlan in a PG_TRY() block.
	 */
	PG_TRY();
	{
		/*
		 * Initialize the plan state tree
		 */
		Assert(CurrentMemoryContext == estate->es_query_cxt);
		InitPlan(queryDesc, eflags);

		Assert(queryDesc->planstate);

		if (debug_print_slice_table && px_role == PX_ROLE_QC)
			elog_node_display(DEBUG3, "slice table", estate->es_sliceTable, true);

		/*
		 * If we're running as a PX and there's a slice table in our
		 * queryDesc, then we need to finish the EState setup we prepared for
		 * back in PxExecQuery.
		 */
		if (px_role == PX_ROLE_PX && estate->es_sliceTable != NULL)
		{
			MotionState *motionstate pg_attribute_unused() = NULL;

			/*
			 * Note that, at this point on a PX, the estate is setup (based on
			 * the slice table transmitted from the QC) so that fields
			 * es_sliceTable, cur_root_idx and es_cur_slice_idx are correct
			 * for the PX.
			 *
			 * If responsible for a non-root slice, arrange to enter the plan
			 * at the slice's sending Motion node rather than at the top.
			 */
			if (LocallyExecutingSliceIndex(estate) != RootSliceIndex(estate))
			{
				motionstate = getMotionState(queryDesc->planstate, LocallyExecutingSliceIndex(estate));
				Assert(motionstate != NULL && IsA(motionstate, MotionState));
			}

			if (debug_print_slice_table)
				elog_node_display(DEBUG3, "slice table", estate->es_sliceTable, true);

			if (px_interconnect_log >= PXVARS_VERBOSITY_DEBUG)
				elog(DEBUG1, "seg%d executing slice%d under root slice%d",
					 PxIdentity.workerid,
					 LocallyExecutingSliceIndex(estate),
					 RootSliceIndex(estate));
		}

		/*
		 * if in dispatch mode, time to serialize plan and query trees, and
		 * fire off px_exec command to each of the pxxecs
		 */
		if (shouldDispatch)
		{
			/*
			 * POLAR px: wal lsn in QC, send it to PXs to make PXs see the
			 * data replayed.
			 */
			px_sql_wal_lsn = polar_px_max_valid_lsn();

			if (queryDesc->ddesc != NULL)
			{
				queryDesc->ddesc->sliceTable = estate->es_sliceTable;
			}

			/*
			 * First, see whether we need to pre-execute any initPlan
			 * subplans.
			 */
			if (queryDesc->plannedstmt->nParamExec > 0)
			{
				ParamListInfoData *pli = queryDesc->params;

				/*
				 * First, use paramFetch to fetch any "lazy" parameters, so
				 * that they are dispatched along with the queries. The PX
				 * nodes cannot call the callback function on their own.
				 */
				if (pli && pli->paramFetch)
				{
					ParamExternData prmdata;
					int			iparam;

					for (iparam = 0; iparam < queryDesc->params->numParams; iparam++)
					{
						ParamExternData *prm = &pli->params[iparam];

						if (!OidIsValid(prm->ptype))
							(*pli->paramFetch) (pli, iparam + 1, false, &prmdata);
					}
				}
			}

			/*
			 * This call returns after launching the threads that send the
			 * plan to the appropriate segdbs.  It does not wait for them to
			 * finish unless an error is detected before all slices have been
			 * dispatched.
			 *
			 * Main plan is parallel, send plan to it.
			 */
			if (estate->es_sliceTable->slices[0].gangType != GANGTYPE_UNALLOCATED ||
				estate->es_sliceTable->slices[0].children)
				PxDispatchPlan(queryDesc, false, true);
		}

		/*
		 * Get executor identity (who does the executor serve). we can assume
		 * Forward scan direction for now just for retrieving the identity.
		 */
		if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
			exec_identity = getPxExecIdentity(queryDesc, ForwardScanDirection, estate);
		else
			exec_identity = PX_IGNORE;

		/*
		 * If we have no slice to execute in this process, mark currentSliceId
		 * as invalid.
		 */
		if (exec_identity == PX_IGNORE)
		{
			estate->currentSliceId = -1;
			currentSliceId = -1;
		}

#ifdef USE_ASSERT_CHECKING
		/* non-root on PX */
		if (exec_identity == PX_NON_ROOT_ON_PX)
		{
			MotionState *motionState = getMotionState(queryDesc->planstate, LocallyExecutingSliceIndex(estate));

			Assert(motionState);

			Assert(IsA(motionState->ps.plan, Motion));
		}
		else
#endif
		if (exec_identity == PX_ROOT_SLICE)
		{
			/* Run a root slice. */
			if (queryDesc->planstate != NULL &&
				estate->es_sliceTable &&
				estate->es_sliceTable->slices[0].gangType == GANGTYPE_UNALLOCATED &&
				estate->es_sliceTable->slices[0].children &&
				!estate->es_interconnect_is_setup)
			{
				Assert(!estate->interconnect_context);
				SetupInterconnect(estate);
				Assert(estate->interconnect_context);
				UpdateMotionExpectedReceivers(estate->motionlayer_context, estate->es_sliceTable);
			}
		}
		else if (exec_identity != PX_IGNORE)
		{
			/* should never happen */
			Assert(!"unsupported POLAR px strategy");
		}

		if (estate->es_interconnect_is_setup)
			Assert(estate->interconnect_context != NULL);

	}
	PG_CATCH();
	{
		ExecutorCleanup_PX(queryDesc);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG, (errmsg("duration to ExecutorStart end: %s ms", msec_str)));
				break;
		}
	}

	/*
	 * Set up an AFTER-trigger statement context, unless told not to, or
	 * unless it's EXPLAIN-only mode (when ExecutorFinish won't be called).
	 */
	if (!(eflags & (EXEC_FLAG_SKIP_TRIGGERS | EXEC_FLAG_EXPLAIN_ONLY)))
		AfterTriggerBeginQuery();

	MemoryContextSwitchTo(oldcontext);
}


static void
standard_ExecutorRun_PX(QueryDesc *queryDesc,
						ScanDirection direction, uint64 count)
{
	EState	   *estate;
	CmdType		operation;
	DestReceiver *dest;
	bool		sendTuples;
	MemoryContext oldcontext;

	/*
	 * NOTE: Any local vars that are set in the PG_TRY block and examined in
	 * the PG_CATCH block should be declared 'volatile'. (setjmp shenanigans)
	 */
	ExecSlice  *currentSlice;
	PxExecIdentity exec_identity;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * Switch into per-query memory context
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Allow instrumentation of Executor overall runtime */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	currentSlice = getCurrentSlice(estate, LocallyExecutingSliceIndex(estate));
	if (currentSlice)
	{
		if (px_role == PX_ROLE_PX ||
			sliceRunsOnQC(currentSlice))
			currentSliceId = currentSlice->sliceIndex;
	}

	/*
	 * extract information from the query descriptor and the query feature.
	 */
	operation = queryDesc->operation;
	dest = queryDesc->dest;

	/*
	 * startup tuple receiver, if we will be emitting tuples
	 */
	estate->es_processed = 0;

	sendTuples = (queryDesc->tupDesc != NULL &&
				  (operation == CMD_SELECT ||
				   queryDesc->plannedstmt->hasReturning));

	if (sendTuples)
		(*dest->rStartup) (dest, operation, queryDesc->tupDesc);

	/*
	 * Need a try/catch block here so that if an ereport is called from within
	 * ExecutePlan, we can clean up by calling PxCheckDispatchResult. This
	 * cleans up the asynchronous commands running through the threads
	 * launched from PxDispatchCommand.
	 */
	PG_TRY();
	{
		/*----------
		 * Run the plan locally.  There are three ways;
		 *
		 * 1. Do nothing
		 * 2. Run a root slice
		 * 3. Run a non-root slice on a PX.
		 *
		 * Here we decide what is our identity -- root slice, non-root on PX
		 * or other (in which case we do nothing), and then run the plan if
		 * required. For more information see getPxExecIdentity() in
		 * execUtils.
		 *----------
		 */
		exec_identity = getPxExecIdentity(queryDesc, direction, estate);

		if (exec_identity == PX_IGNORE)
		{
			/* do nothing */
			estate->es_got_eos = true;
		}
		else if (exec_identity == PX_NON_ROOT_ON_PX)
		{
			/*
			 * Run a non-root slice on a PX.
			 *
			 * Since the top Plan node is a (Sending) Motion, run the plan
			 * forward to completion. The plan won't return tuples locally
			 * (tuples go out over the interconnect), so the destination is
			 * uninteresting.  The command type should be SELECT, however, to
			 * avoid other sorts of DML processing..
			 *
			 * This is the center of slice plan activity -- here we arrange to
			 * blunder into the middle of the plan rather than entering at the
			 * root.
			 */

			MotionState *motionState = getMotionState(queryDesc->planstate, LocallyExecutingSliceIndex(estate));
			PlanState  *old_root = queryDesc->planstate;

			Assert(motionState);

			queryDesc->planstate = (PlanState *) motionState;
			ExecutePlan(queryDesc,
						CMD_SELECT,
						sendTuples,
						0,
						ForwardScanDirection,
						dest);
			queryDesc->planstate = old_root;
		}
		else if (exec_identity == PX_ROOT_SLICE)
		{
			/*----------
			 * Run a root slice
			 * It corresponds to the "normal" path through the executor in that
			 * we enter the plan at the top and count on the motion nodes at the
			 * fringe of the top slice to return without ever calling nodes
			 * below them.
			 *----------
			 */
			ExecutePlan(queryDesc,
						operation,
						sendTuples,
						count,
						direction,
						dest);
		}
		else
		{
			/* should never happen */
			Assert(!"undefined POLAR px strategy");
		}
	}
	PG_CATCH();
	{
		/* POLAR px : If EXPLAIN ANALYZE, let px try to return stats to qc. */
		if (estate->es_sliceTable &&
			estate->es_sliceTable->instrument_options &&
			(estate->es_sliceTable->instrument_options & INSTRUMENT_PX) &&
			px_role == PX_ROLE_PX)
		{
			PG_TRY();
			{
				pxexplain_sendExecStats(queryDesc);
			}
			PG_CATCH();
			{
				/* Close down interconnect etc. */
				ExecutorCleanup_PX(queryDesc);
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
		/* Close down interconnect etc. */
		ExecutorCleanup_PX(queryDesc);
		PG_RE_THROW();
		/* POLAR end */
	}
	PG_END_TRY();

	/*
	 * shutdown tuple receiver, if we started it
	 */
	if (sendTuples)
		(*dest->rShutdown) (dest);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, estate->es_processed);

	MemoryContextSwitchTo(oldcontext);
}


void
standard_ExecutorEnd_PX(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/*
	 * Check that ExecutorFinish was called, unless in EXPLAIN-only mode. This
	 * Assert is needed because ExecutorFinish is new as of 9.1, and callers
	 * might forget to call it.
	 */
	Assert(estate->es_finished ||
		   (estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * Switch into per-query memory context to run ExecEndPlan
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	{
		/*
		 * POLAR px : If EXPLAIN ANALYZE, px returns stats to qc now.
		 */
		if (estate->es_sliceTable &&
			estate->es_sliceTable->instrument_options &&
			(estate->es_sliceTable->instrument_options & INSTRUMENT_PX) &&
			px_role == PX_ROLE_PX)
			pxexplain_sendExecStats(queryDesc);

		/*
		 * if needed, collect mpp dispatch results and tear down all mpp
		 * specific resources (e.g. interconnect).
		 */
		PG_TRY();
		{
			ExecutorFinishup_PX(queryDesc);
		}
		PG_CATCH();
		{
			/*
			 * we got an error. do all the necessary cleanup.
			 */
			ExecutorCleanup_PX(queryDesc);

			/*
			 * Remove our own query's motion layer.
			 */
			RemoveMotionLayer(estate->motionlayer_context);

			/*
			 * Release EState and per-query memory context.
			 */
			FreeExecutorState(estate);

			PG_RE_THROW();
		}
		PG_END_TRY();
		/* POLAR end */
	}

	ExecEndPlan(queryDesc->planstate, estate);

	/*
	 * Remove our own query's motion layer.
	 */
	RemoveMotionLayer(estate->motionlayer_context);

	/* do away with our snapshots */
	UnregisterSnapshot(estate->es_snapshot);
	UnregisterSnapshot(estate->es_crosscheck_snapshot);

	/*
	 * Must switch out of context before destroying it
	 */
	MemoryContextSwitchTo(oldcontext);

	/* POLAR px */
	queryDesc->es_processed = estate->es_processed;
	/* POLAR end */

	/*
	 * Release EState and per-query memory context.  This should release
	 * everything the executor has allocated.
	 */
	FreeExecutorState(estate);

	/* Reset queryDesc fields that no longer point to anything */
	queryDesc->tupDesc = NULL;
	queryDesc->estate = NULL;
	queryDesc->planstate = NULL;
	queryDesc->totaltime = NULL;
}

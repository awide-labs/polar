/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2011 EMC Greenplum, Inc.
*	Copyright (C) 2021, Alibaba Group Holding Limiteds
*
*	@filename:
*		COptTasks.cpp
*
*	@doc:
*		Routines to perform optimization related tasks using the gpos framework
*
*	@test:
*
*-------------------------------------------------------------------------
*/

#include "px_optimizer_util/utils/px_defs.h"
#include "px_optimizer_util/utils/CConstExprEvaluatorProxy.h"
#include "px_optimizer_util/utils/COptTasks.h"
#include "px_optimizer_util/relcache/CMDProviderRelcache.h"
#include "px_optimizer_util/config/CConfigParamMapping.h"
#include "gpopt/translate/CTranslatorDXLToExpr.h"
#include "gpopt/translate/CTranslatorExprToDXL.h"
#include "px_optimizer_util/translate/CTranslatorUtils.h"
#include "px_optimizer_util/translate/CTranslatorQueryToDXL.h"
#include "px_optimizer_util/translate/CTranslatorDXLToPlStmt.h"
#include "px_optimizer_util/translate/CContextDXLToPlStmt.h"
#include "px_optimizer_util/translate/CTranslatorRelcacheToDXL.h"
#include "gpopt/eval/CConstExprEvaluatorDXL.h"
#include "gpopt/engine/CHint.h"

#include "px/px_vars.h"
#include "utils/guc.h"
#include "utils/fmgroids.h"

#include "gpos/base.h"
#include "gpos/error/CException.h"
#undef setstate

#include "gpos/_api.h"
#include "gpos/common/CAutoP.h"
#include "gpos/io/COstreamFile.h"
#include "gpos/io/COstreamString.h"
#include "gpos/memory/CAutoMemoryPool.h"
#include "gpos/task/CAutoTraceFlag.h"
#include "gpos/common/CAutoP.h"

#include "gpopt/translate/CTranslatorDXLToExpr.h"
#include "gpopt/translate/CTranslatorExprToDXL.h"

#include "gpopt/base/CAutoOptCtxt.h"
#include "gpopt/engine/CEnumeratorConfig.h"
#include "gpopt/engine/CStatisticsConfig.h"
#include "gpopt/engine/CCTEConfig.h"
#include "gpopt/mdcache/CAutoMDAccessor.h"
#include "gpopt/mdcache/CMDCache.h"
#include "gpopt/minidump/CMinidumperUtils.h"
#include "gpopt/optimizer/COptimizer.h"
#include "gpopt/optimizer/COptimizerConfig.h"
#include "gpopt/xforms/CXformFactory.h"
#include "gpopt/exception.h"

#include "naucrates/init.h"
#include "naucrates/traceflags/traceflags.h"

#include "naucrates/base/CQueryToDXLResult.h"

#include "naucrates/md/IMDId.h"
#include "naucrates/md/CMDIdRelStats.h"

#include "naucrates/md/CSystemId.h"
#include "naucrates/md/IMDRelStats.h"
#include "naucrates/md/CMDIdCast.h"
#include "naucrates/md/CMDIdScCmp.h"

#include "naucrates/dxl/operators/CDXLNode.h"
#include "naucrates/dxl/parser/CParseHandlerDXL.h"
#include "naucrates/dxl/CDXLUtils.h"
#include "naucrates/dxl/CIdGenerator.h"
#include "naucrates/exception.h"

#include "gpdbcost/CCostModelGPDB.h"
#include "gpdbcost/CCostModelPolarDB.h"


#include "px_optimizer_util/px_wrappers.h"

using namespace gpos;
using namespace gpopt;
using namespace gpdxl;
using namespace gpdbcost;

// definition of default AutoMemoryPool
#define AUTO_MEM_POOL(amp) CAutoMemoryPool amp(CAutoMemoryPool::ElcExc)

// default id for the source system
const CSystemId default_sysid(IMDId::EmdidGPDB, GPOS_WSZ_STR_LENGTH("GPDB"));


/*-------------------------------------------------------------------------
*	@function:
*		SOptContext::SOptContext
*
*	@doc:
*		Ctor
*
*-------------------------------------------------------------------------
*/
SOptContext::SOptContext()
	:
	m_query_dxl(NULL),
	m_query(NULL),
	m_plan_dxl(NULL),
	m_plan_stmt(NULL),
	m_should_generate_plan_stmt(false),
	m_should_serialize_plan_dxl(false),
	m_is_unexpected_failure(false),
	m_should_error_out(false),
	m_error_msg(NULL)
{}

/*-------------------------------------------------------------------------
*	@function:
*		SOptContext::Free
*
*	@doc:
*		Free all members except those pointed to by either input or
*		output
*
*-------------------------------------------------------------------------
*/
void
SOptContext::Free
	(
	SOptContext::EPin input,
	SOptContext::EPin output
	)
{
	if (NULL != m_query_dxl && epinQueryDXL != input && epinQueryDXL != output)
	{
		px::GPDBFree(m_query_dxl);
	}
	
	if (NULL != m_query && epinQuery != input && epinQuery != output)
	{
		px::GPDBFree(m_query);
	}
	
	if (NULL != m_plan_dxl && epinPlanDXL != input && epinPlanDXL != output)
	{
		px::GPDBFree(m_plan_dxl);
	}
	
	if (NULL != m_plan_stmt && epinPlStmt != input && epinPlStmt != output)
	{
		px::GPDBFree(m_plan_stmt);
	}
	
	if (NULL != m_error_msg && epinErrorMsg != input && epinErrorMsg != output)
	{
		px::GPDBFree(m_error_msg);
	}
}

/*-------------------------------------------------------------------------
*	@function:
*		SOptContext::CloneErrorMsg
*
*	@doc:
*		Clone m_error_msg to given memory context. Return NULL if there is no
*		error message.
*
*-------------------------------------------------------------------------
*/
CHAR*
SOptContext::CloneErrorMsg
	(
	MemoryContext context
	)
{
	if (NULL == context ||
		NULL == m_error_msg)
	{
		return NULL;
	}
	return px::MemCtxtStrdup(context, m_error_msg);
}


/*-------------------------------------------------------------------------
*	@function:
*		SOptContext::Cast
*
*	@doc:
*		Casting function
*
*-------------------------------------------------------------------------
*/
SOptContext *
SOptContext::Cast
	(
	void *ptr
	)
{
	GPOS_ASSERT(NULL != ptr);

	return reinterpret_cast<SOptContext*>(ptr);
}

/*-------------------------------------------------------------------------
*	@function:
*		CreateMultiByteCharStringFromWCString
*
*	@doc:
*		Return regular string from wide-character string
*
*-------------------------------------------------------------------------
*/
CHAR *
COptTasks::CreateMultiByteCharStringFromWCString
	(
	const WCHAR *wcstr
	)
{
	GPOS_ASSERT(NULL != wcstr);

	const ULONG input_len = GPOS_WSZ_LENGTH(wcstr);
	const ULONG wchar_size = GPOS_SIZEOF(WCHAR);
	const ULONG max_len = input_len * wchar_size + 1;

	CHAR *str = (CHAR *) px::GPDBAlloc(max_len);

	gpos::clib::Wcstombs(str, const_cast<WCHAR *>(wcstr), max_len);
	str[max_len - 1] = '\0';

	return str;
}

/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::Execute
*
*	@doc:
*		Execute a task using GPOS. TODO extend gpos to provide
*		this functionality
*
*-------------------------------------------------------------------------
*/
void
COptTasks::Execute
	(
	void *(*func) (void *) ,
	void *func_arg
	)
{
	Assert(func);

	CHAR *err_buf = (CHAR *) palloc(PXOPT_ERROR_BUFFER_SIZE);
	err_buf[0] = '\0';

	// initialize DXL support
	InitDXL();

	bool abort_flag = false;

	CAutoMemoryPool amp(CAutoMemoryPool::ElcNone);

	gpos_exec_params params;
	params.func = func;
	params.arg = func_arg;
	params.stack_start = &params;
	params.error_buffer = err_buf;
	params.error_buffer_size = PXOPT_ERROR_BUFFER_SIZE;
	params.abort_requested = &abort_flag;

	// execute task and send log message to server log
	GPOS_TRY
	{
		(void) gpos_exec(&params);
	}
	GPOS_CATCH_EX(ex)
	{
		BOOL fUnexpectedFailure = IsLoggableFailure(ex);

		/* optimizer failed to produce a plan, log failure */
		if (px_optimizer_log && 
			((OPTIMIZER_ALL_FAIL == px_optimizer_log_failure) ||
			 (fUnexpectedFailure && OPTIMIZER_UNEXPECTED_FAIL == px_optimizer_log_failure) ||    /* unexpected fall back */
			 (!fUnexpectedFailure && OPTIMIZER_EXPECTED_FAIL == px_optimizer_log_failure)     /* expected fall back */
			))
			LogExceptionMessageAndDelete(err_buf, ex.SeverityLevel());

		GPOS_RETHROW(ex);
	}
	GPOS_CATCH_END;
	LogExceptionMessageAndDelete(err_buf);
}

void
COptTasks::LogExceptionMessageAndDelete(CHAR* err_buf, ULONG severity_level)
{

	if ('\0' != err_buf[0] && px_optimizer_log)
	{
		int gpdb_severity_level;

		if (severity_level == CException::ExsevDebug1)
			gpdb_severity_level = DEBUG1;
		else
			gpdb_severity_level = LOG;

		elog(gpdb_severity_level, "%s", CreateMultiByteCharStringFromWCString((WCHAR *)err_buf));
	}

	pfree(err_buf);
}


/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::ConvertToPlanStmtFromDXL
*
*	@doc:
*		Translate a DXL tree into a planned statement
*
*-------------------------------------------------------------------------
*/
PlannedStmt *
COptTasks::ConvertToPlanStmtFromDXL
	(
	CMemoryPool *mp,
	CMDAccessor *md_accessor,
	const Query *orig_query,
	const CDXLNode *dxlnode,
	bool can_set_tag,
	DistributionHashOpsKind distribution_hashops
	)
{

	GPOS_ASSERT(NULL != md_accessor);
	GPOS_ASSERT(NULL != dxlnode);

	CIdGenerator plan_id_generator(1 /* ulStartId */);
	CIdGenerator motion_id_generator(1 /* ulStartId */);
	CIdGenerator param_id_generator(0 /* ulStartId */);

	CContextDXLToPlStmt dxl_to_plan_stmt_ctxt
							(
							mp,
							&plan_id_generator,
							&motion_id_generator,
							&param_id_generator,
							distribution_hashops
							);
	
	// translate DXL -> PlannedStmt
	CTranslatorDXLToPlStmt dxl_to_plan_stmt_translator(mp, md_accessor, &dxl_to_plan_stmt_ctxt, px::GetPxWorkerCount());
	return dxl_to_plan_stmt_translator.GetPlannedStmtFromDXL(dxlnode, orig_query, can_set_tag);

}


/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::LoadSearchStrategy
*
*	@doc:
*		Load search strategy from given file
*
*-------------------------------------------------------------------------
*/
CSearchStageArray *
COptTasks::LoadSearchStrategy
	(
	CMemoryPool *mp,
	char *path
	)
{
	CSearchStageArray *search_strategy_arr = NULL;
	CParseHandlerDXL *dxl_parse_handler = NULL;

	GPOS_TRY
	{
		if (NULL != path)
		{
			dxl_parse_handler = CDXLUtils::GetParseHandlerForDXLFile(mp, path, NULL);
			if (NULL != dxl_parse_handler)
			{
				elog(DEBUG2, "\n[OPT]: Using search strategy in (%s)", path);

				search_strategy_arr = dxl_parse_handler->GetSearchStageArray();
				search_strategy_arr->AddRef();
			}
		}
	}
	GPOS_CATCH_EX(ex)
	{
		if (GPOS_MATCH_EX(ex, gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError)) {
			GPOS_RETHROW(ex);
		}
		elog(DEBUG2, "\n[OPT]: Using default search strategy");
		GPOS_RESET_EX;
	}
	GPOS_CATCH_END;

	GPOS_DELETE(dxl_parse_handler);

	return search_strategy_arr;
}

/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::CreateOptimizerConfig
*
*	@doc:
*		Create the optimizer configuration
*
*-------------------------------------------------------------------------
*/
COptimizerConfig *
COptTasks::CreateOptimizerConfig
	(
	CMemoryPool *mp,
	ICostModel *cost_model
	)
{
	// get chosen plan number, cost threshold
	ULLONG plan_id = (ULLONG) px_optimizer_plan_id;
	ULLONG num_samples = (ULLONG) px_optimizer_samples_number;
	DOUBLE cost_threshold = (DOUBLE) px_optimizer_cost_threshold;

	DOUBLE damping_factor_filter = (DOUBLE) px_optimizer_damping_factor_filter;
	DOUBLE damping_factor_join = (DOUBLE) px_optimizer_damping_factor_join;
	DOUBLE damping_factor_groupby = (DOUBLE) px_optimizer_damping_factor_groupby;

	ULONG cte_inlining_cutoff = (ULONG) px_optimizer_cte_inlining_bound;
	ULONG join_arity_for_associativity_commutativity = (ULONG) px_optimizer_join_arity_for_associativity_commutativity;
	ULONG array_expansion_threshold = (ULONG) px_optimizer_array_expansion_threshold;
	ULONG join_order_threshold = (ULONG) px_optimizer_join_order_threshold;
	ULONG broadcast_threshold = (ULONG) px_optimizer_penalize_broadcast_threshold;
	ULONG push_group_by_below_setop_threshold = (ULONG) px_optimizer_push_group_by_below_setop_threshold;
	/* POLAR px */
	ULONG insert_dop_num = (ULONG) local_px_insert_dop_num;
	/* POLAR end */

	return GPOS_NEW(mp) COptimizerConfig
						(
						GPOS_NEW(mp) CEnumeratorConfig(mp, plan_id, num_samples, cost_threshold),
						GPOS_NEW(mp) CStatisticsConfig(mp, damping_factor_filter, damping_factor_join, damping_factor_groupby, MAX_STATS_BUCKETS),
						GPOS_NEW(mp) CCTEConfig(cte_inlining_cutoff),
						cost_model,
						GPOS_NEW(mp) CHint
								(
								gpos::int_max /* optimizer_parts_to_force_sort_on_insert */,
								join_arity_for_associativity_commutativity,
								array_expansion_threshold,
								join_order_threshold,
								broadcast_threshold,
								false, /* don't create Assert nodes for constraints, we'll
								      * enforce them ourselves in the executor */
								push_group_by_below_setop_threshold,
								/* POLAR px */
								insert_dop_num
								/* POLAR end */
								),
						GPOS_NEW(mp) CWindowOids(OID(F_ROW_NUMBER), OID(F_RANK_))
						);
}

/*-------------------------------------------------------------------------
*		@function:
//			COptTasks::SetCostModelParams
*
*      @doc:
//			Set cost model parameters
*
*-------------------------------------------------------------------------
*/
void
COptTasks::SetCostModelParams
	(
	ICostModel *cost_model
	)
{
	GPOS_ASSERT(NULL != cost_model);

	if (px_optimizer_nestloop_factor > 1.0)
	{
		// change NLJ cost factor
		ICostModelParams::SCostParam *cost_param = NULL;
		if (OPTIMIZER_GPDB_CALIBRATED == px_optimizer_cost_model)
		{
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsGPDB::EcpNLJFactor);
		}
		else  if(OPTIMIZER_POLARDB == px_optimizer_cost_model)
		{
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsPolarDB::EcpNLJFactor);
		}
		else
		{
			GPOS_ASSERT(false);
		}
		
		CDouble nlj_factor(px_optimizer_nestloop_factor);
		cost_model->GetCostModelParams()->SetParam(cost_param->Id(), nlj_factor, nlj_factor - 0.5, nlj_factor + 0.5);
	}

	if (px_optimizer_sort_factor > 1.0 || px_optimizer_sort_factor < 1.0)
	{
		// change sort cost factor
		ICostModelParams::SCostParam *cost_param = NULL;
		if (OPTIMIZER_GPDB_CALIBRATED == px_optimizer_cost_model)
		{
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsGPDB::EcpSortTupWidthCostUnit);

			CDouble sort_factor(px_optimizer_sort_factor);
			cost_model->GetCostModelParams()->SetParam(cost_param->Id(), cost_param->Get() * px_optimizer_sort_factor, cost_param->GetLowerBoundVal() * px_optimizer_sort_factor, cost_param->GetUpperBoundVal() * px_optimizer_sort_factor);
		}
		else  if(OPTIMIZER_POLARDB == px_optimizer_cost_model)		
		{
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsPolarDB::EcpSortTupWidthCostUnit);

			CDouble sort_factor(px_optimizer_sort_factor);
			cost_model->GetCostModelParams()->SetParam(cost_param->Id(), cost_param->Get() * px_optimizer_sort_factor, cost_param->GetLowerBoundVal() * px_optimizer_sort_factor, cost_param->GetUpperBoundVal() * px_optimizer_sort_factor);
		}
	}

	// POLAR px: set table sharescan cost factor
	if (px_optimizer_share_tablescan_factor != 1.0)
	{
		// change sort cost factor
		ICostModelParams::SCostParam *cost_param = NULL;
		CDouble factor(px_optimizer_share_tablescan_factor);
		if (OPTIMIZER_GPDB_CALIBRATED == px_optimizer_cost_model)
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsGPDB::EcpPolarTableShareScanFactor);
		else if(OPTIMIZER_POLARDB == px_optimizer_cost_model)
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsPolarDB::EcpPolarTableShareScanFactor);
		if (cost_param)
			cost_model->GetCostModelParams()->SetParam(cost_param->Id(),
						cost_param->Get() * factor,
						cost_param->GetLowerBoundVal() * factor,
						cost_param->GetUpperBoundVal() * factor);
	}
	// POLAR px: set share indexscan cost factor
	if (px_optimizer_share_indexscan_factor != 1.0)
	{
		// change sort cost factor
		ICostModelParams::SCostParam *cost_param = NULL;
		CDouble factor(px_optimizer_share_indexscan_factor);
		if (OPTIMIZER_GPDB_CALIBRATED == px_optimizer_cost_model)
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsGPDB::EcpPolarShareIndexScanFactor);
		else if(OPTIMIZER_POLARDB == px_optimizer_cost_model)
			cost_param = cost_model->GetCostModelParams()->PcpLookup(CCostModelParamsPolarDB::EcpPolarShareIndexScanFactor);
		if (cost_param)
			cost_model->GetCostModelParams()->SetParam(cost_param->Id(),
						cost_param->Get() * factor,
						cost_param->GetLowerBoundVal() * factor,
						cost_param->GetUpperBoundVal() * factor);
	}
}


/*-------------------------------------------------------------------------
*      @function:
//			COptTasks::GetCostModel
*
*      @doc:
//			Generate an instance of optimizer cost model
*
*-------------------------------------------------------------------------
*/
ICostModel *
COptTasks::GetCostModel
	(
	CMemoryPool *mp,
	ULONG num_segments
	)
{
	ICostModel *cost_model = NULL;
	if (OPTIMIZER_GPDB_CALIBRATED == px_optimizer_cost_model)
	{
		cost_model = GPOS_NEW(mp) CCostModelGPDB(mp, num_segments);
	}
	else 
	{
		cost_model = GPOS_NEW(mp) CCostModelPolarDB(mp, num_segments);
	}

	SetCostModelParams(cost_model);

	return cost_model;
}

/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::OptimizeTask
*
*	@doc:
*		task that does the optimizes query to physical DXL
*
*-------------------------------------------------------------------------
*/
void*
COptTasks::OptimizeTask
	(
	void *ptr
	)
{
	GPOS_ASSERT(NULL != ptr);
	SOptContext *opt_ctxt = SOptContext::Cast(ptr);

	GPOS_ASSERT(NULL != opt_ctxt->m_query);
	GPOS_ASSERT(NULL == opt_ctxt->m_plan_dxl);
	GPOS_ASSERT(NULL == opt_ctxt->m_plan_stmt);

	AUTO_MEM_POOL(amp);
	CMemoryPool *mp = amp.Pmp();

	// Does the metadatacache need to be reset?
	//
	// On the first call, before the cache has been initialized, we
	// don't care about the return value of MDCacheNeedsReset(). But
	// we need to call it anyway, to give it a chance to initialize
	// the invalidation mechanism.
	bool reset_mdcache = px::MDCacheNeedsReset();

	// initialize metadata cache, or purge if needed, or change size if requested
	if (!CMDCache::FInitialized())
	{
		CMDCache::Init();
		CMDCache::SetCacheQuota(px_optimizer_mdcache_size * 1024L);
	}
	else if (reset_mdcache)
	{
		CMDCache::Reset();
		CMDCache::SetCacheQuota(px_optimizer_mdcache_size * 1024L);
	}
	else if (CMDCache::ULLGetCacheQuota() != (ULLONG) px_optimizer_mdcache_size * 1024L)
	{
		CMDCache::SetCacheQuota(px_optimizer_mdcache_size * 1024L);
	}


	// load search strategy
	CSearchStageArray *search_strategy_arr = LoadSearchStrategy(mp, px_optimizer_search_strategy_path);

	CBitSet *trace_flags = NULL;
	CBitSet *enabled_trace_flags = NULL;
	CBitSet *disabled_trace_flags = NULL;
	CDXLNode *plan_dxl = NULL;

	IMdIdArray *col_stats = NULL;
	MdidHashSet *rel_stats = NULL;

	GPOS_TRY
	{
		// set trace flags
		trace_flags = CConfigParamMapping::PackConfigParamInBitset(mp, CXform::ExfSentinel);
		SetTraceflags(mp, trace_flags, &enabled_trace_flags, &disabled_trace_flags);

		// set up relcache MD provider
		CMDProviderRelcache *relcache_provider = GPOS_NEW(mp) CMDProviderRelcache(mp);

		{
			// scope for MD accessor
			CMDAccessor mda(mp, CMDCache::Pcache(), default_sysid, relcache_provider);

			ULONG num_segments_for_costing = px_optimizer_segments;
			ULONG num_segments = px::GetPxWorkerCount();
			if (0 == num_segments_for_costing)
			{
				num_segments_for_costing = num_segments;
			}

			CAutoP<CTranslatorQueryToDXL> query_to_dxl_translator;
			query_to_dxl_translator = CTranslatorQueryToDXL::QueryToDXLInstance
							(
							mp,
							&mda,
							(Query*) opt_ctxt->m_query
							);
			
			ICostModel *cost_model = GetCostModel(mp, num_segments_for_costing);
			COptimizerConfig *optimizer_config = CreateOptimizerConfig(mp, cost_model);
			CConstExprEvaluatorProxy expr_eval_proxy(mp, &mda);
			IConstExprEvaluator *expr_evaluator =
					GPOS_NEW(mp) CConstExprEvaluatorDXL(mp, &mda, &expr_eval_proxy);

			CDXLNode *query_dxl = query_to_dxl_translator->TranslateQueryToDXL();
			CDXLNodeArray *query_output_dxlnode_array = query_to_dxl_translator->GetQueryOutputCols();
			CDXLNodeArray *cte_dxlnode_array = query_to_dxl_translator->GetCTEs();
			GPOS_ASSERT(NULL != query_output_dxlnode_array);

			BOOL is_master_only = !px_optimizer_enable_motions ||
						(!px_optimizer_enable_motions_masteronly_queries && !query_to_dxl_translator->HasDistributedTables());
			// See NoteDistributionPolicyOpclasses() in src/backend/gpopt/translate/CTranslatorQueryToDXL.cpp
			BOOL use_legacy_opfamilies = (query_to_dxl_translator->GetDistributionHashOpsKind() == DistrUseLegacyHashOps);
			CAutoTraceFlag atf1(EopttraceDisableMotions, is_master_only);
			CAutoTraceFlag atf2(EopttraceUseLegacyOpfamilies, use_legacy_opfamilies);

			// begin to output dxl
			if (px_optimizer_log)
			{
				CWStringDynamic wstr(mp);
				COstreamString os(&wstr);
				CXMLSerializer xml_serializer(mp, os, true);
				query_dxl->SerializeToDXL(&xml_serializer);

				CHAR *sz = CUtils::CreateMultiByteCharStringFromWCString(
					mp, const_cast<WCHAR *>(wstr.GetBuffer()));
				elog(INFO, "\n[query_dxl]: %s\n", sz);
				GPOS_DELETE_ARRAY(sz);
			}

			plan_dxl = COptimizer::PdxlnOptimize
									(
									mp,
									&mda,
									query_dxl,
									query_output_dxlnode_array,
									cte_dxlnode_array,
									expr_evaluator,
									num_segments,
									px_session_id,
									px_command_count,
									search_strategy_arr,
									optimizer_config
									);

			if (opt_ctxt->m_should_serialize_plan_dxl)
			{
				// serialize DXL to xml
				CWStringDynamic plan_str(mp);
				COstreamString oss(&plan_str);
				CDXLUtils::SerializePlan(mp, oss, plan_dxl, optimizer_config->GetEnumeratorCfg()->GetPlanId(), optimizer_config->GetEnumeratorCfg()->GetPlanSpaceSize(), true /*serialize_header_footer*/, true /*indentation*/);
				opt_ctxt->m_plan_dxl = CreateMultiByteCharStringFromWCString(plan_str.GetBuffer());
			}

			// translate DXL->PlStmt only when needed
			if (opt_ctxt->m_should_generate_plan_stmt)
			{
				// always use opt_ctxt->m_query->can_set_tag as the query_to_dxl_translator->Pquery() is a mutated Query object
				opt_ctxt->m_plan_stmt = (PlannedStmt *) px::CopyObject(ConvertToPlanStmtFromDXL(mp, &mda, opt_ctxt->m_query, plan_dxl, opt_ctxt->m_query->canSetTag, query_to_dxl_translator->GetDistributionHashOpsKind()));


				if (px_optimizer_log)
				{
					CWStringDynamic wstr(mp);
					COstreamString os(&wstr);
					CXMLSerializer xml_serializer(mp, os, true);
					plan_dxl->SerializeToDXL(&xml_serializer);

					CHAR *sz = CUtils::CreateMultiByteCharStringFromWCString(
						mp, const_cast<WCHAR *>(wstr.GetBuffer()));
					elog(INFO, "\n[plan_dxl]: %s\n", sz);
					GPOS_DELETE_ARRAY(sz);
				}
			}

			CStatisticsConfig *stats_conf = optimizer_config->GetStatsConf();
			col_stats = GPOS_NEW(mp) IMdIdArray(mp);
			stats_conf->CollectMissingStatsColumns(col_stats);

			rel_stats = GPOS_NEW(mp) MdidHashSet(mp);
			PrintMissingStatsWarning(mp, &mda, col_stats, rel_stats);

			rel_stats->Release();
			col_stats->Release();

			expr_evaluator->Release();
			query_dxl->Release();
			optimizer_config->Release();
			plan_dxl->Release();
		}
	}
	GPOS_CATCH_EX(ex)
	{
		ResetTraceflags(enabled_trace_flags, disabled_trace_flags);
		CRefCount::SafeRelease(rel_stats);
		CRefCount::SafeRelease(col_stats);
		CRefCount::SafeRelease(enabled_trace_flags);
		CRefCount::SafeRelease(disabled_trace_flags);
		CRefCount::SafeRelease(trace_flags);
		CRefCount::SafeRelease(plan_dxl);
		CMDCache::Shutdown();

		IErrorContext *errctxt = CTask::Self()->GetErrCtxt();

		opt_ctxt->m_should_error_out = ShouldErrorOut(ex);
		opt_ctxt->m_is_unexpected_failure = IsLoggableFailure(ex);
		opt_ctxt->m_error_msg = CreateMultiByteCharStringFromWCString(errctxt->GetErrorMsg());

		GPOS_RETHROW(ex);
	}
	GPOS_CATCH_END;

	// cleanup
	ResetTraceflags(enabled_trace_flags, disabled_trace_flags);
	CRefCount::SafeRelease(enabled_trace_flags);
	CRefCount::SafeRelease(disabled_trace_flags);
	CRefCount::SafeRelease(trace_flags);
	if (!px_optimizer_metadata_caching)
	{
		CMDCache::Shutdown();
	}

	return NULL;
}


/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::PrintMissingStatsWarning
*
*	@doc:
*		Print warning messages for columns with missing statistics
*
*-------------------------------------------------------------------------
*/
void
COptTasks::PrintMissingStatsWarning
	(
	CMemoryPool *mp,
	CMDAccessor *md_accessor,
	IMdIdArray *col_stats,
	MdidHashSet *rel_stats
	)
{
	GPOS_ASSERT(NULL != md_accessor);
	GPOS_ASSERT(NULL != col_stats);
	GPOS_ASSERT(NULL != rel_stats);

	CWStringDynamic wcstr(mp);
	COstreamString oss(&wcstr);

	const ULONG num_missing_col_stats = col_stats->Size();
	for (ULONG ul = 0; ul < num_missing_col_stats; ul++)
	{
		IMDId *mdid = (*col_stats)[ul];
		CMDIdColStats *mdid_col_stats = CMDIdColStats::CastMdid(mdid);

		IMDId *rel_mdid = mdid_col_stats->GetRelMdId();
		const ULONG pos = mdid_col_stats->Position();
		const IMDRelation *rel = md_accessor->RetrieveRel(rel_mdid);

		if (IMDRelation::ErelstorageExternal != rel->RetrieveRelStorageType())
		{
			if (!rel_stats->Contains(rel_mdid))
			{
				if (0 != ul)
				{
					oss << ", ";
				}

				rel_mdid->AddRef();
				rel_stats->Insert(rel_mdid);
				oss << rel->Mdname().GetMDName()->GetBuffer();
			}

			CMDName mdname = rel->GetMdCol(pos)->Mdname();

			char msgbuf[NAMEDATALEN * 2 + 100];
			snprintf(msgbuf, sizeof(msgbuf), "Missing statistics for column: %s.%s", CreateMultiByteCharStringFromWCString(rel->Mdname().GetMDName()->GetBuffer()), CreateMultiByteCharStringFromWCString(mdname.GetMDName()->GetBuffer()));
			PxEreport(ERRCODE_SUCCESSFUL_COMPLETION,
						   LOG,
						   msgbuf,
						   NULL);
		}
	}

	if (0 < rel_stats->Size())
	{
		int length = NAMEDATALEN * rel_stats->Size() + 200;
		char msgbuf[length];
		snprintf(msgbuf, sizeof(msgbuf), "One or more columns in the following table(s) do not have statistics: %s", CreateMultiByteCharStringFromWCString(wcstr.GetBuffer()));
		if (px_optimizer_log)
		{
			PxEreport(ERRCODE_SUCCESSFUL_COMPLETION,
					   NOTICE,
					   msgbuf,
					   "For non-partitioned tables, run analyze <table_name>(<column_list>)."
					   " For partitioned tables, run analyze rootpartition <table_name>(<column_list>)."
					   " See log for columns missing statistics.");
		}
	}

}

/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::Optimize
*
*	@doc:
*		optimizes a query to physical DXL
*
*-------------------------------------------------------------------------
*/
char *
COptTasks::Optimize
	(
	Query *query
	)
{
	Assert(query);

	SOptContext gpopt_context;
	gpopt_context.m_query = query;
	gpopt_context.m_should_serialize_plan_dxl = true;
	Execute(&OptimizeTask, &gpopt_context);

	// clean up context
	gpopt_context.Free(gpopt_context.epinQuery, gpopt_context.epinPlanDXL);

	return gpopt_context.m_plan_dxl;
}


/*-------------------------------------------------------------------------
*	@function:
*		COptTasks::PXOPTOptimizedPlan
*
*	@doc:
*		optimizes a query to plannedstmt
*
*-------------------------------------------------------------------------
*/
PlannedStmt *
COptTasks::PXOPTOptimizedPlan
	(
	Query *query,
	SOptContext *gpopt_context
	)
{
	Assert(query);
	Assert(gpopt_context);

	gpopt_context->m_query = query;
	gpopt_context->m_should_generate_plan_stmt= true;
	Execute(&OptimizeTask, gpopt_context);
	return gpopt_context->m_plan_stmt;
}

//---------------------------------------------------------------------------
//	@function:
//		COptTasks::SetXform
//
//	@doc:
//		Enable/Disable a given xform
//
//---------------------------------------------------------------------------
bool
COptTasks::SetXform(char *xform_str, bool should_disable)
{
	CXform *xform = CXformFactory::Pxff()->Pxf(xform_str);
	if (nullptr != xform)
	{
		optimizer_xforms[xform->Exfid()] = should_disable;

		return true;
	}

	return false;
}

// EOF

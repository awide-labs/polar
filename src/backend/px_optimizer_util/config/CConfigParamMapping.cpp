/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2012 EMC Corp.
*	Copyright (C) 2021, Alibaba Group Holding Limiteds
*
*	@filename:
*		CConfigParamMapping.cpp
*
*	@doc:
*		Implementation of GPDB config params->trace flags mapping
*
*	@test:
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"

#include "utils/guc.h"
/* reuse common flags(e.g enable_indexscan ) */
#include "optimizer/cost.h"

#include "px_optimizer_util/config/CConfigParamMapping.h"
#include "gpopt/xforms/CXform.h"

using namespace gpos;
using namespace gpdxl;
using namespace gpopt;

// array mapping GUCs to traceflags
CConfigParamMapping::SConfigMappingElem CConfigParamMapping::m_elements[] =
{
		{
		EopttracePrintQuery,
		&px_optimizer_print_query,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints the optimizer's input query expression tree.")
		},

		{
		EopttracePrintPlan,
		&px_optimizer_print_plan,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints the plan expression tree produced by the optimizer.")
		},

		{
		EopttracePrintXform,
		&px_optimizer_print_xform,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints the input and output expression trees of the optimizer transformations.")
		},

		{
		EopttracePrintXformResults,
		&px_optimizer_print_xform_results,
		false, // m_negate_param
		GPOS_WSZ_LIT("Print input and output of xforms.")
		},

		{
		EopttracePrintMemoAfterExploration,
		&px_optimizer_print_memo_after_exploration,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints MEMO after exploration.")
		},

		{
		EopttracePrintMemoAfterImplementation,
		&px_optimizer_print_memo_after_implementation,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints MEMO after implementation.")
		},

		{
		EopttracePrintMemoAfterOptimization,
		&px_optimizer_print_memo_after_optimization,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints MEMO after optimization.")
		},

		{
		EopttracePrintJobScheduler,
		&px_optimizer_print_job_scheduler,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints jobs in scheduler on each job completion.")
		},

		{
		EopttracePrintExpressionProperties,
		&px_optimizer_print_expression_properties,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints expression properties.")
		},

		{
		EopttracePrintGroupProperties,
		&px_optimizer_print_group_properties,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints group properties.")
		},

		{
		EopttracePrintOptimizationContext,
		&px_optimizer_print_optimization_context,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints optimization context.")
		},

		{
		EopttracePrintOptimizationStatistics,
		&px_optimizer_print_optimization_stats,
		false, // m_negate_param
		GPOS_WSZ_LIT("Prints optimization stats.")
		},

		{
		EopttraceMinidump,
		// GPDB_91_MERGE_FIXME: I turned px_optimizer_minidump from bool into
		// an enum-type GUC. It's a bit dirty to cast it like this..
		(bool *) &px_optimizer_minidump,
		false, // m_negate_param
		GPOS_WSZ_LIT("Generate optimizer minidump.")
		},

		{
		EopttraceDisableMotions,
		&px_optimizer_enable_motions,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable motion nodes in optimizer.")
		},

		{
		EopttraceDisableMotionBroadcast,
		&px_optimizer_enable_motion_broadcast,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable motion broadcast nodes in optimizer.")
		},

		{
		EopttraceDisableMotionGather,
		&px_optimizer_enable_motion_gather,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable motion gather nodes in optimizer.")
		},

		{
		EopttraceDisableMotionHashDistribute,
		&px_optimizer_enable_motion_redistribute,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable motion hash-distribute nodes in optimizer.")
		},

		{
		EopttraceDisableMotionRandom,
		&px_optimizer_enable_motion_redistribute,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable motion random nodes in optimizer.")
		},

		{
		EopttraceDisableMotionRountedDistribute,
		&px_optimizer_enable_motion_redistribute,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable motion routed-distribute nodes in optimizer.")
		},

		{
		EopttraceDisableSort,
		&px_optimizer_enable_sort,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable sort nodes in optimizer.")
		},

		{
		EopttraceDisableSpool,
		&px_optimizer_enable_materialize,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable spool nodes in optimizer.")
		},

		{
		EopttraceDisablePartPropagation,
		&px_optimizer_enable_partition_propagation,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable partition propagation nodes in optimizer.")
		},

		{
		EopttraceDisablePartSelection,
		&px_optimizer_enable_partition_selection,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable partition selection in optimizer.")
		},

		{
		EopttraceDisableOuterJoin2InnerJoinRewrite,
		&px_optimizer_enable_outerjoin_rewrite,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable outer join to inner join rewrite in optimizer.")
		},

		{
		EopttraceDonotDeriveStatsForAllGroups,
		&px_optimizer_enable_derive_stats_all_groups,
		true, // m_negate_param
		GPOS_WSZ_LIT("Disable deriving stats for all groups after exploration.")
		},

		{
		EopttraceEnableSpacePruning,
		&px_optimizer_enable_space_pruning,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable space pruning in optimizer.")
		},

		{
		EopttraceForceMultiStageAgg,
		&px_optimizer_force_multistage_agg,
		false, // m_negate_param
		GPOS_WSZ_LIT("Force optimizer to always pick multistage aggregates when such a plan alternative is generated.")
		},

		{
		EopttracePrintColsWithMissingStats,
		&px_optimizer_print_missing_stats,
		false, // m_negate_param
		GPOS_WSZ_LIT("Print columns with missing statistics.")
		},

		{
		EopttraceEnableRedistributeBroadcastHashJoin,
		&px_optimizer_enable_hashjoin_redistribute_broadcast_children,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable generating hash join plan where outer child is Redistribute and inner child is Broadcast.")
		},

		{
		EopttraceExtractDXLStats,
		&px_optimizer_extract_dxl_stats,
		false, // m_negate_param
		GPOS_WSZ_LIT("Extract plan stats in dxl.")
		},

		{
		EopttraceExtractDXLStatsAllNodes,
		&px_optimizer_extract_dxl_stats_all_nodes,
		false, // m_negate_param
		GPOS_WSZ_LIT("Extract plan stats for all physical dxl nodes.")
		},

		{
		EopttraceDeriveStatsForDPE,
		&px_optimizer_dpe_stats,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable stats derivation of partitioned tables with dynamic partition elimination.")
		},

		{
		EopttraceEnumeratePlans,
		&px_optimizer_enumerate_plans,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable plan enumeration.")
		},

		{
		EopttraceSamplePlans,
		&px_optimizer_sample_plans,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable plan sampling.")
		},

		{
		EopttraceEnableCTEInlining,
		&px_optimizer_cte_inlining,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable CTE inlining.")
		},

		{
		EopttraceEnableConstantExpressionEvaluation,
		&px_optimizer_enable_constant_expression_evaluation,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Enable constant expression evaluation in the optimizer")
		},

		{
		EopttraceUseExternalConstantExpressionEvaluationForInts,
		&px_optimizer_use_external_constant_expression_evaluation_for_ints,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Enable constant expression evaluation for integers in the optimizer")
		},

		{
		EopttraceApplyLeftOuter2InnerUnionAllLeftAntiSemiJoinDisregardingStats,
		&px_optimizer_apply_left_outer_to_union_all_disregarding_stats,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Always apply Left Outer Join to Inner Join UnionAll Left Anti Semi Join without looking at stats")
		},


		/* POLAR px */
		{
		EopttraceRemoveSuperfluousOrder,
		&px_optimizer_remove_superfluous_order,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Remove superfluous OrderBy")
		},

		{
		EopttraceRemoveOrderBelowDML,
		&px_optimizer_remove_order_below_dml,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Remove OrderBy below a DML operation")
		},

		{
		EopttraceDisableReplicateInnerNLJOuterChild,
		&px_optimizer_enable_broadcast_nestloop_outer_child,
		true,  // m_negate_param
		GPOS_WSZ_LIT("Enable plan alternatives where NLJ's outer child is replicated")
		},

		{
		EopttraceMotionHazardHandling,
		&px_optimizer_enable_streaming_material,
		false,  // m_fNegate
		GPOS_WSZ_LIT("Enable motion hazard handling during NLJ optimization and generate streaming material when appropriate")
		},

		{
		EopttraceDisableNonMasterGatherForDML,
		&px_optimizer_enable_gather_on_segment_for_dml,
		true,  // m_fNegate
		GPOS_WSZ_LIT("Enable DML optimization by enforcing a non-master gather when appropriate")
		},

		{
		EopttraceEnforceCorrelatedExecution,
		&px_optimizer_enforce_subplans,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Enforce correlated execution in the optimizer")
		},

		{
		EopttraceForceExpandedMDQAs,
		&px_optimizer_force_expanded_distinct_aggs,
		false,  // m_negate_param
		GPOS_WSZ_LIT("Always pick plans that expand multiple distinct aggregates into join of single distinct aggregate in the optimizer")
		},
		{
		EopttraceDisablePushingCTEConsumerReqsToCTEProducer,
		&px_optimizer_push_requirements_from_consumer_to_producer,
		true,  // m_negate_param
		GPOS_WSZ_LIT("Optimize CTE producer plan on requirements enforced on top of CTE consumer")
		},

		{
		EopttraceDisablePruneUnusedComputedColumns,
		&px_optimizer_prune_computed_columns,
		true,  // m_negate_param
		GPOS_WSZ_LIT("Prune unused computed columns when pre-processing query")
		},

		{
		EopttraceForceThreeStageScalarDQA,
		&px_optimizer_force_three_stage_scalar_dqa,
		false, // m_negate_param
		GPOS_WSZ_LIT("Force optimizer to always pick 3 stage aggregate plan for scalar distinct qualified aggregate.")
		},

		{
		EopttraceEnableParallelAppend,
		&px_optimizer_parallel_union,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable parallel execution for UNION/UNION ALL queries.")
		},

		{
		EopttraceArrayConstraints,
		&px_optimizer_array_constraints,
		false, // m_negate_param
		GPOS_WSZ_LIT("Allows the constraint framework to derive array constraints in the optimizer.")
		},

		{
		EopttraceForceAggSkewAvoidance,
		&px_optimizer_force_agg_skew_avoidance,
		false, // m_negate_param
		GPOS_WSZ_LIT("Always pick a plan for aggregate distinct that minimizes skew.")
        },

        {
		EopttraceEnableEagerAgg,
		&px_optimizer_enable_eageragg,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable Eager Agg transform for pushing aggregate below an innerjoin.")
		},
		{
		EopttraceExpandFullJoin,
		&px_optimizer_expand_fulljoin,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable Expand Full Join transform for converting FULL JOIN into UNION ALL.")
		},
		{
		EopttracePenalizeSkewedHashJoin,
		&px_optimizer_penalize_skew,
		true, // m_negate_param
		GPOS_WSZ_LIT("Penalize a hash join with a skewed redistribute as a child.")
		},
		{
		EopttraceEnableLeftIndexNLJoin,
		&px_enable_left_index_nestloop_join,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable left index nestloop join.")
		},

		/* POLAR px */
		{
		EopttraceEnablePartitionWiseJoin,
		&px_enable_partitionwise_join,
		false, // m_negate_param
		GPOS_WSZ_LIT("Enable partition wise join.")
		},
		{
		EopttraceTranslateUnusedColrefs,
		&px_optimizer_prune_unused_columns,
		true, // m_negate_param
		GPOS_WSZ_LIT("Prune unused columns from the query.")
		}
	
};

/*-------------------------------------------------------------------------
*	@function:
*		CConfigParamMapping::PackConfigParamInBitset
*
*	@doc:
*		Pack the GPDB config params into a bitset
*
*-------------------------------------------------------------------------
*/
CBitSet *
CConfigParamMapping::PackConfigParamInBitset
	(
	CMemoryPool *mp,
	ULONG xform_id // number of available xforms
	)
{
	CBitSet *traceflag_bitset = GPOS_NEW(mp) CBitSet(mp, EopttraceSentinel);

	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(m_elements); ul++)
	{
		SConfigMappingElem elem = m_elements[ul];
		GPOS_ASSERT(!traceflag_bitset->Get((ULONG) elem.m_trace_flag) &&
					"trace flag already set");

		BOOL value = *elem.m_is_param;
		if (elem.m_negate_param)
		{
			// negate the value of config param
			value = !value;
		}

		if (value)
		{
#ifdef GPOS_DEBUG
			BOOL is_traceflag_set =
#endif // GPOS_DEBUG
				traceflag_bitset->ExchangeSet((ULONG) elem.m_trace_flag);
			GPOS_ASSERT(!is_traceflag_set);
		}
	}

	// pack disable flags of xforms
	for (ULONG ul = 0; ul < xform_id; ul++)
	{
		GPOS_ASSERT(!traceflag_bitset->Get(EopttraceDisableXformBase + ul) &&
					"xform trace flag already set");

		if (optimizer_xforms[ul])
		{
#ifdef GPOS_DEBUG
			BOOL is_traceflag_set =
#endif // GPOS_DEBUG
				traceflag_bitset->ExchangeSet(EopttraceDisableXformBase + ul);
			GPOS_ASSERT(!is_traceflag_set);
		}
	}

	if (!px_optimizer_enable_indexjoin)
	{
		CBitSet *index_join_bitset = CXform::PbsIndexJoinXforms(mp);
		traceflag_bitset->Union(index_join_bitset);
		index_join_bitset->Release();
	}

	// disable bitmap scan if the corresponding GUC is turned off
	if (!px_optimizer_enable_bitmapscan)
	{
		CBitSet *bitmap_index_bitset = CXform::PbsBitmapIndexXforms(mp);
		traceflag_bitset->Union(bitmap_index_bitset);
		bitmap_index_bitset->Release();
	}

	// disable outerjoin to unionall transformation if GUC is turned off
	if (!px_optimizer_enable_outerjoin_to_unionall_rewrite)
	{
		 traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfLeftOuter2InnerUnionAllLeftAntiSemiJoin));
	}

	// disable Assert MaxOneRow plans if GUC is turned off
	if (!px_optimizer_enable_assert_maxonerow)
	{
		 traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfMaxOneRow2Assert));
	}

	if (!px_optimizer_enable_hashjoin)
	{
		// disable hash-join if the corresponding GUC is turned off
		CBitSet *hash_join_bitste = CXform::PbsHashJoinXforms(mp);
		traceflag_bitset->Union(hash_join_bitste);
		hash_join_bitste->Release();
	}

	if (!px_optimizer_enable_dynamictablescan)
	{
		// disable dynamic table scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfDynamicGet2DynamicTableScan));
	}

	/* POLAR px */
	if (!px_optimizer_enable_dynamicshareindexscan)
	{
		// disable partition share index scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfDynamicIndexGet2DynamicShareIndexScan));
	}

	if (!px_optimizer_enable_seqscan)
	{
		// disable table scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfGet2TableScan));
	}

	if (!px_optimizer_enable_seqsharescan)
	{
		// disable table scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfGet2TableShareScan));
	}

	if (!px_optimizer_enable_indexscan)
	{
		// disable index scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfIndexGet2IndexScan));
	}

	if (!px_optimizer_enable_indexonlyscan)
	{
		// disable index only scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfIndexGet2IndexOnlyScan));
	}

	if (!px_optimizer_enable_shareindexscan)
	{
		// disable share index scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfIndexGet2ShareIndexScan));
	}

	if (!px_optimizer_enable_dynamicindexscan)
	{
		// disable dynamic index scan if the corresponding GUC is turned off
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfDynamicIndexGet2DynamicIndexScan));
	}

	if (!px_optimizer_enable_hashagg)
	{
		 traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfGbAgg2HashAgg));
		 traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfGbAggDedup2HashAggDedup));
	}

	if (!px_optimizer_enable_groupagg)
	{
		 traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfGbAgg2StreamAgg));
		 traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfGbAggDedup2StreamAggDedup));
	}

	if (!px_optimizer_enable_mergejoin)
	{
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfImplementFullOuterMergeJoin));
	}

	if (!px_optimizer_enable_nestloopjoin)
	{
		// disable nestloop-join if the corresponding GUC is turned off
		CBitSet *nestloop_join_bitste = CXform::PbsNestloopJoinXforms(mp);
		traceflag_bitset->Union(nestloop_join_bitste);
		nestloop_join_bitste->Release();
	}

	if (!px_optimizer_enable_lasj_notin)
	{
		CBitSet *asj_not_in_bitste = CXform::PbsAntSemiJoinNotInXforms(mp);
		traceflag_bitset->Union(asj_not_in_bitste);
		asj_not_in_bitste->Release();
	}

	if (!px_optimizer_enable_crossproduct)
	{
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfLeftSemiJoin2CrossProduct));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfLeftAntiSemiJoin2CrossProduct));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfLeftAntiSemiJoinNotIn2CrossProduct));
	}

	if (!px_enable_cte)
	{
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfCTEAnchor2Sequence));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfCTEAnchor2TrivialSelect));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfInlineCTEConsumer));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfInlineCTEConsumerUnderSelect));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfImplementCTEProducer));
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfImplementCTEConsumer));
	}

	if (!px_enable_cte_shared_scan)
	{
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfImplementCTEProducer));
	}

	CBitSet *join_heuristic_bitset = NULL;
	switch (px_optimizer_join_order)
	{
		case JOIN_ORDER_IN_QUERY:
			join_heuristic_bitset = CXform::PbsJoinOrderInQueryXforms(mp);
			break;
		case JOIN_ORDER_GREEDY_SEARCH:
			join_heuristic_bitset = CXform::PbsJoinOrderOnGreedyXforms(mp);
			break;
		case JOIN_ORDER_EXHAUSTIVE_SEARCH:
			join_heuristic_bitset = CXform::PbsJoinOrderOnExhaustiveXforms(mp);
			break;
		case JOIN_ORDER_EXHAUSTIVE2_SEARCH:
			join_heuristic_bitset = CXform::PbsJoinOrderOnExhaustive2Xforms(mp);
			break;
		default:
			elog(ERROR, "Invalid value for px_optimizer_join_order, must \
// 				 not come here");
			break;
	}
	traceflag_bitset->Union(join_heuristic_bitset);
	join_heuristic_bitset->Release();

	// disable join associativity transform if the corresponding GUC
	// is turned off independent of the join order algorithm chosen
	if (!px_optimizer_enable_associativity)
	{
		traceflag_bitset->ExchangeSet(GPOPT_DISABLE_XFORM_TF(CXform::ExfJoinAssociativity));
	}

	// enable nested loop index plans using nest params
	// instead of outer reference as in the case with GPDB 4/5
	traceflag_bitset->ExchangeSet(EopttraceIndexedNLJOuterRefAsParams);

	// enable using opfamilies in distribution specs for GPDB 6
	if (px_enable_opfamily_for_distribution)
		traceflag_bitset->ExchangeSet(EopttraceConsiderOpfamiliesForDistribution);

	return traceflag_bitset;
}

// EOF

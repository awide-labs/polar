/*-------------------------------------------------------------------------
 *
 * px_plannerconfig.h
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
 *	  src/include/nodes/px_plannerconfig.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PLANNERCONFIG_H_
#define PLANNERCONFIG_H_

/**
 * Planning configuration information
 */
typedef struct PlannerConfig
{
	bool		enable_sort;
	bool		enable_hashagg;
	bool		enable_groupagg;
	bool		enable_nestloop;
	bool		enable_mergejoin;
	bool		enable_hashjoin;
	bool		px_enable_hashjoin_size_heuristic;
	bool		px_enable_predicate_propagation;
	int			constraint_exclusion;

	bool		px_enable_minmax_optimization;
	bool		px_enable_multiphase_agg;
	bool		px_enable_preunique;
	bool		px_eager_preunique;
	bool		px_hashagg_streambottom;
	bool		px_enable_agg_distinct;
	bool		px_enable_dqa_pruning;
	bool		px_eager_dqa_pruning;
	bool		px_eager_one_phase_agg;
	bool		px_eager_two_phase_agg;
	bool		px_enable_sort_distinct;

	bool		px_enable_direct_dispatch;

	bool		px_cte_sharing; /* Indicate whether sharing is to be disabled
								 * on any CTEs */

	bool		honor_order_by;

	bool		is_under_subplan;	/* True for plan rooted at a subquery
									 * which is planned as a subplan */

#if 0
	/* These ones are tricky */
	PxRoleValue px_role;
	/* TODO:this one is tricky */
#endif
} PlannerConfig;

#endif							/* PLANNERCONFIG_H_ */

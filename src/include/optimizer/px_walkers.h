/*-------------------------------------------------------------------------
 *
 * px_walkers.h
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
 *	  src/include/optimizer/px_walkers.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef WALKERS_H_
#define WALKERS_H_

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"

/*
 * POLAR px
 * The plan associated with a SubPlan is found in a list.  During planning this is in
 * the global structure found through the root PlannerInfo.  After planning this is in
 * the PlannedStmt.
 *
 * Structure plan_tree_base_prefix carries the appropriate pointer for GPDB's general plan
 * tree walker/mutator framework.  All users of the framework must prefix their context
 * structure with a plan_tree_base_prefix and initialize it appropriately.
 */
typedef struct plan_tree_base_prefix
{
	Node	   *node;			/* PlannerInfo* or PlannedStmt* */
} plan_tree_base_prefix;

extern void exec_init_plan_tree_base(plan_tree_base_prefix *base, PlannedStmt *stmt);
extern Plan *plan_tree_base_subplan_get_plan(plan_tree_base_prefix *base, SubPlan *subplan);
extern void plan_tree_base_subplan_put_plan(plan_tree_base_prefix *base, SubPlan *subplan, Plan *plan);

extern bool walk_plan_node_fields(Plan *plan, bool (*walker) (), void *context);

extern bool plan_tree_walker(Node *node, bool (*walker) (), void *context, bool recurse_into_subplans);

extern Plan *change_scantype(Plan *node);
#ifdef __cplusplus
extern "C"
{
#endif

 /**
  * Useful functions that aggregate information from expressions or plans.
  */
extern List *extract_nodes(PlannerGlobal *glob, Node *node, int nodeTag);
extern List *extract_nodes_plan(Plan *pl, int nodeTag, bool descendIntoSubqueries);
extern List *extract_nodes_expression(Node *node, int nodeTag, bool descendIntoSubqueries);
extern int	find_nodes(Node *node, List *nodeTags);
extern int	check_collation(Node *node);

#ifdef __cplusplus
}
#endif

#endif							/* WALKERS_H_ */

/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2011 Greenplum, Inc.
*	Copyright (C) 2021, Alibaba Group Holding Limiteds
*
*	@filename:
*		CContextDXLToPlStmt.cpp
*
*	@doc:
*		Implementation of the functions that provide
*		access to CIdGenerators (needed to number initplans, motion
*		nodes as well as params), list of RangeTableEntires and Subplans
*		generated so far during DXL-->PlStmt translation.
*
*	@test:
*
-------------------------------------------------------------------------*/

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "utils/rel.h"

#include "px_optimizer_util/translate/CContextDXLToPlStmt.h"
#include "px_optimizer_util/px_wrappers.h"
#include "gpos/base.h"

#include "naucrates/exception.h"

using namespace gpdxl;

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::CContextDXLToPlStmt
*
*	@doc:
*		Ctor
*
-------------------------------------------------------------------------*/
CContextDXLToPlStmt::CContextDXLToPlStmt
	(
	CMemoryPool *mp,
	CIdGenerator *plan_id_counter,
	CIdGenerator *motion_id_counter,
	CIdGenerator *param_id_counter,
	DistributionHashOpsKind distribution_hashops
	)
	:
	m_mp(mp),
	m_plan_id_counter(plan_id_counter),
	m_motion_id_counter(motion_id_counter),
	m_param_id_counter(param_id_counter),
	m_param_types_list(NIL),
	m_distribution_hashops(distribution_hashops),
	m_rtable_entries_list(NULL),
	m_partitioned_tables_list(NULL),
	m_num_partition_selectors_array(NULL),
	m_subplan_entries_list(NULL),
	m_subplan_sliceids_list(NULL),
	m_slices_list(NULL),
	m_current_slice(NULL),
	m_result_relation_index(0),
	m_into_clause(NULL),
	m_distribution_policy(NULL),
	m_part_selector_to_param_map(nullptr)
{
	m_cte_consumer_info = GPOS_NEW(m_mp) HMUlCTEConsumerInfo(m_mp);
	m_num_partition_selectors_array = GPOS_NEW(m_mp) ULongPtrArray(m_mp);
	m_part_selector_to_param_map = GPOS_NEW(m_mp) UlongToUlongMap(m_mp);
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::~CContextDXLToPlStmt
*
*	@doc:
*		Dtor
*
-------------------------------------------------------------------------*/
CContextDXLToPlStmt::~CContextDXLToPlStmt()
{
	m_cte_consumer_info->Release();
	m_num_partition_selectors_array->Release();
	m_part_selector_to_param_map->Release();
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::GetNextPlanId
*
*	@doc:
*		Get the next plan id
*
-------------------------------------------------------------------------*/
ULONG
CContextDXLToPlStmt::GetNextPlanId()
{
	return m_plan_id_counter->next_id();
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::GetNextParamId
*
*	@doc:
*		Get the next plan id
*
-------------------------------------------------------------------------*/
ULONG
CContextDXLToPlStmt::GetNextParamId(OID typeoid)
{
	m_param_types_list = px::LAppendOid(m_param_types_list, typeoid);

	return m_param_id_counter->next_id();
}



//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::AddCTEConsumerInfo
//
//	@doc:
//		Add information about the newly found CTE entry
//
//---------------------------------------------------------------------------
void
CContextDXLToPlStmt::AddCTEConsumerInfo(ULONG cte_id,
										ShareInputScan *share_input_scan)
{
	GPOS_ASSERT(NULL != share_input_scan);

	SCTEConsumerInfo *cte_info = m_cte_consumer_info->Find(&cte_id);
	if (NULL != cte_info)
	{
		cte_info->AddCTEPlan(share_input_scan);
		return;
	}

	List *cte_plan = ListMake1(share_input_scan);

	ULONG *key = GPOS_NEW(m_mp) ULONG(cte_id);
	BOOL result GPOS_ASSERTS_ONLY = m_cte_consumer_info->Insert(
		key, GPOS_NEW(m_mp) SCTEConsumerInfo(cte_plan));

	GPOS_ASSERT(result);
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::AddRTE
*
*	@doc:
*		Add a RangeTableEntries
*
-------------------------------------------------------------------------*/
void
CContextDXLToPlStmt::AddRTE
	(
	RangeTblEntry *rte,
	BOOL is_result_relation
	)
{
	m_rtable_entries_list = px::LAppend(m_rtable_entries_list, rte);

	rte->inFromCl = true;

	if (is_result_relation)
	{
		GPOS_ASSERT(0 == m_result_relation_index && "Only one result relation supported");
		rte->inFromCl = false;
		m_result_relation_index = px::ListLength(m_rtable_entries_list);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::GetParamTypes
//
//	@doc:
//		Get the current param types list
//
//---------------------------------------------------------------------------
List *
CContextDXLToPlStmt::GetParamTypes()
{
	return m_param_types_list;
}

//---------------------------------------------------------------------------
//	@function:
//		CContextDXLToPlStmt::GetParamTypes
//
//	@doc:
//		Get the current param types list
//
//---------------------------------------------------------------------------
List *
CContextDXLToPlStmt::GetRTLists()
{
	return m_rtable_entries_list;
}


/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::AddSubplan
*
*	@doc:
*		Add a subplan
*
-------------------------------------------------------------------------*/
void
CContextDXLToPlStmt::AddSubplan(Plan *plan)
{
	m_subplan_entries_list = px::LAppend(m_subplan_entries_list, plan);
	m_subplan_sliceids_list = px::LAppendInt(m_subplan_sliceids_list, m_current_slice->sliceIndex);
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::GetSubplanSliceIdArray
*
*	@doc:
*		Get the slice IDs of each subplan as an array.
*
-------------------------------------------------------------------------*/
int *
CContextDXLToPlStmt::GetSubplanSliceIdArray()
{
  int numSubplans = list_length(m_subplan_entries_list);
  int *sliceIdArray;
  ListCell *lc;
  int i;

  sliceIdArray = (int *) px::GPDBAlloc(numSubplans * sizeof(int));

  i = 0;
  foreach(lc, m_subplan_sliceids_list)
	{
      sliceIdArray[i++] = lfirst_int(lc);
	}

  return sliceIdArray;
}


/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::AddSlice
*
*	@doc:
*		Add a plan slice
*
-------------------------------------------------------------------------*/
int
CContextDXLToPlStmt::AddSlice(PlanSlice *slice)
{
  slice->sliceIndex = list_length(m_slices_list);
  m_slices_list = px::LAppend(m_slices_list, slice);

  return slice->sliceIndex;
}

PlanSlice *
CContextDXLToPlStmt::GetSlices(int *numSlices_p)
{
  int numSlices = list_length(m_slices_list);
  PlanSlice *sliceArray;
  ListCell *lc;
  int i;

  sliceArray = (PlanSlice *) px::GPDBAlloc(numSlices * sizeof(PlanSlice));

  i = 0;
  foreach(lc, m_slices_list)
	{
      PlanSlice *src = (PlanSlice *) lfirst(lc);

      memcpy(&sliceArray[i], src, sizeof(PlanSlice));

      i++;
	}

  m_current_slice = NULL;
  px::ListFreeDeep(m_slices_list);

  *numSlices_p = numSlices;
  return sliceArray;
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::GetDistributionHashOpclassForType
*
*	@doc:
*		Return a hash operator class to use for computing
*		distribution key values for the given datatype.
*
*		This returns either the default opclass, or the legacy
*		opclass of the type, depending on what opclasses we have
*		seen being used in tables so far.
---------------------------------------------------------------------------*/
Oid
CContextDXLToPlStmt::GetDistributionHashOpclassForType(Oid typid)
{
	Oid opclass = InvalidOid;

	switch (m_distribution_hashops)
	{
		case DistrUseDefaultHashOps:
			opclass = px::GetDefaultDistributionOpclassForType(typid);
			break;

		case DistrUseLegacyHashOps:
			opclass = InvalidOid;
			break;

		case DistrHashOpsNotDeterminedYet:
            // None of the tables we have seen so far have been
            // hash distributed, so we haven't made up our mind
            // on which opclasses to use yet. But we have to
            // pick something now.
            //
            // FIXME: It's quite unoptimal that this ever happens.
            // To avoid this we should make a pass over the tree to
            // determine the opclasses, before translating
            // anything. But there is no convenient way to "walk"
            // the DXL representation AFAIK.
            //
            // Example query where this happens:
            // select * from dd_singlecol_1 t1,
            //               generate_series(1,10) g
            // where t1.a=g.g and t1.a=1 ;
            //
            // The PXOPT plan consists of a join between
            // Result+FunctionScan and TableScan. The
            // Result+FunctionScan side is processed first, and
            // this gets called to generate a "hash filter" for
            // it Result. The TableScan is encountered and
            // added to the range table only later. If it uses
            // legacy ops, we have already decided to use default
            // ops here, and we fall back unnecessarily.
			m_distribution_hashops = DistrUseDefaultHashOps;
			opclass = px::GetDefaultDistributionOpclassForType(typid);
			break;
	}

	return opclass;
}

/*-------------------------------------------------------------------------
*	@function:
*		CContextDXLToPlStmt::GetDistributionHashFuncForType
*
*	@doc:
*		Return a hash function to use for computing distribution key
*		values for the given datatype.
*
*		This returns the hash function either from the default
*		opclass, or the legacy opclass of the type, depending on
*		what opclasses we have seen being used in tables so far.
---------------------------------------------------------------------------*/
Oid
CContextDXLToPlStmt::GetDistributionHashFuncForType(Oid typid)
{
	Oid opclass;
	Oid opfamily;
	Oid hashproc;

	opclass = GetDistributionHashOpclassForType(typid);

	if (opclass == InvalidOid)
	{
		GPOS_RAISE(gpdxl::ExmaMD, gpdxl::ExmiMDObjUnsupported, GPOS_WSZ_LIT("no default hash opclasses found"));
	}

	opfamily = px::GetOpclassFamily(opclass);
	hashproc = px::GetHashProcInOpfamily(opfamily, typid);

	return hashproc;
}

ULONG
CContextDXLToPlStmt::GetParamIdForSelector(OID oid_type, ULONG selectorId)
{
	ULONG *param_id = m_part_selector_to_param_map->Find(&selectorId);
	if (nullptr == param_id)
	{
		param_id = GPOS_NEW(m_mp) ULONG(GetNextParamId(oid_type));
		ULONG *selector_id = GPOS_NEW(m_mp) ULONG(selectorId);
		m_part_selector_to_param_map->Insert(selector_id, param_id);
	}
	return *param_id;
}

Index
CContextDXLToPlStmt::FindRTE(Oid reloid)
{
	ListCell *lc;
	int idx = 0;

	ForEachWithCount(lc, m_rtable_entries_list, idx)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		if (rte->relid == reloid)
		{
			return idx + 1;
		}
	}
	return -1;
}
// EOF

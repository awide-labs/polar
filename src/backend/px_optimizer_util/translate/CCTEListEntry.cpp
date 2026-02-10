/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2012 EMC Corp.
*	Copyright (C) 2021, Alibaba Group Holding Limiteds
*
*	@filename:
*		CCTEListEntry.cpp
*
*	@doc:
*		Implementation of the class representing the list of common table
*		expression defined at a query level
*
*	@test:
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "px_optimizer_util/translate/CCTEListEntry.h"

#include "nodes/parsenodes.h"

#include "gpos/base.h"
#include "px_optimizer_util/px_wrappers.h"
using namespace gpdxl;

/*-------------------------------------------------------------------------
*	@function:
*		CCTEListEntry::CCTEListEntry
*
*	@doc:
*		Ctor: single CTE
*
*-------------------------------------------------------------------------
*/
CCTEListEntry::CCTEListEntry
	(
	CMemoryPool *mp,
	ULONG query_level,
	CommonTableExpr *cte,
	CDXLNode *cte_producer
	)
	:
	m_query_level(query_level),
	m_cte_info(NULL)
{
	GPOS_ASSERT(NULL != cte && NULL != cte_producer);
	
	m_cte_info = GPOS_NEW(mp) HMSzCTEInfo(mp);
	Query *cte_query = (Query*) cte->ctequery;
		
#ifdef GPOS_DEBUG
		BOOL result =
#endif
	m_cte_info->Insert(cte->ctename, GPOS_NEW(mp) SCTEProducerInfo(cte_producer, cte_query->targetList));
		
	GPOS_ASSERT(result);
}

/*-------------------------------------------------------------------------
*	@function:
*		CCTEListEntry::GetCTEProducer
*
*	@doc:
*		Return the query of the CTE referenced in the range table entry
*
*-------------------------------------------------------------------------
*/
const CDXLNode *
CCTEListEntry::GetCTEProducer
	(
	const CHAR *cte_str
	)
	const
{
	SCTEProducerInfo *cte_info = m_cte_info->Find(cte_str);
	if (NULL == cte_info)
	{
		return NULL; 
	}
	
	return cte_info->m_cte_producer;
}

/*-------------------------------------------------------------------------
*	@function:
*		CCTEListEntry::GetCTEProducerTargetList
*
*	@doc:
*		Return the target list of the CTE referenced in the range table entry
*
*-------------------------------------------------------------------------
*/
List *
CCTEListEntry::GetCTEProducerTargetList
	(
	const CHAR *cte_str
	)
	const
{
	SCTEProducerInfo *cte_info = m_cte_info->Find(cte_str);
	if (NULL == cte_info)
	{
		return NULL; 
	}
	
	return cte_info->m_target_list;
}

/*-------------------------------------------------------------------------
*	@function:
*		CCTEListEntry::AddCTEProducer
*
*	@doc:
*		Add a new CTE producer to this query level
*
*-------------------------------------------------------------------------
*/
void
CCTEListEntry::AddCTEProducer
	(
	CMemoryPool *mp,
	CommonTableExpr *cte,
	const CDXLNode *cte_producer
	)
{
	GPOS_ASSERT(NULL == m_cte_info->Find(cte->ctename) && "CTE entry already exists");
	Query *cte_query = (Query*) cte->ctequery;
	
#ifdef GPOS_DEBUG
	BOOL result =
#endif
	m_cte_info->Insert(cte->ctename, GPOS_NEW(mp) SCTEProducerInfo(cte_producer, cte_query->targetList));
	
	GPOS_ASSERT(result);
}

// EOF

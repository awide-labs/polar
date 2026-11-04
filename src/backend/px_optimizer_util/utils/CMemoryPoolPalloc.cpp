/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2019 Pivotal, Inc.
*	Copyright (C) 2021, Alibaba Group Holding Limiteds
*
*	@filename:
*		CMemoryPoolPalloc.cpp
*
*	@doc:
*		CMemoryPool implementation that uses PostgreSQL memory
*		contexts.
*
*-------------------------------------------------------------------------
*/

extern "C" {
#include "postgres.h"
#include "utils/memutils.h"
}

#include "gpos/memory/CMemoryPool.h"
#include "px_optimizer_util/px_wrappers.h"

#include "px_optimizer_util/utils/CMemoryPoolPalloc.h"

using namespace gpos;

// ctor
CMemoryPoolPalloc::CMemoryPoolPalloc()
	: m_cxt(NULL)
{
	m_cxt = px::GPDBAllocSetContextCreate();
}

void *
CMemoryPoolPalloc::NewImpl
	(
	const ULONG bytes,
	const CHAR *,
	const ULONG,
	CMemoryPool::EAllocationType eat
	)
{
	// if it's a singleton allocation, allocate requested memory
	if (CMemoryPool::EatSingleton == eat)
	{
		return px::GPDBMemoryContextAlloc(m_cxt, bytes);
	}
	// if it's an array allocation, allocate header + requested memory
	else
	{
		ULONG alloc_size = GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader) + GPOS_MEM_ALIGNED_SIZE(bytes);

		void *ptr = px::GPDBMemoryContextAlloc(m_cxt, alloc_size);

		if (NULL == ptr)
		{
			return NULL;
		}

		SArrayAllocHeader *header = static_cast<SArrayAllocHeader*>(ptr);

		header->m_user_size = bytes;
		return static_cast<BYTE*>(ptr) + GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader);
	}
}

void
CMemoryPoolPalloc::DeleteImpl(void *ptr, CMemoryPool::EAllocationType eat)
{
	if (CMemoryPool::EatSingleton == eat)
	{
		px::GPDBFree(ptr);
	}
	else
	{
		void* header = static_cast<BYTE*>(ptr) - GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader);
		px::GPDBFree(header);
	}
}

// Prepare the memory pool to be deleted
void
CMemoryPoolPalloc::TearDown()
{
	px::GPDBMemoryContextDelete(m_cxt);
}

// Total allocated size including management overheads
ULLONG
CMemoryPoolPalloc::TotalAllocatedSize() const
{
	return MemoryContextMemAllocated(m_cxt, true);
}

// get user requested size of array allocation. Note: this is ONLY called for arrays
ULONG
CMemoryPoolPalloc::UserSizeOfAlloc(const void *ptr)
{
	GPOS_ASSERT(ptr != NULL);
	void* void_header = static_cast<BYTE*>(const_cast<void*>(ptr)) - GPOS_MEM_ALIGNED_STRUCT_SIZE(SArrayAllocHeader);
	const SArrayAllocHeader *header = static_cast<SArrayAllocHeader*>(void_header);
	return header->m_user_size;
}


// EOF

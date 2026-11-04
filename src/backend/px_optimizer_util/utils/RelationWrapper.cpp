//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2020 VMware, Inc.
//---------------------------------------------------------------------------
#include "px_optimizer_util/utils/RelationWrapper.h"
#include "px_optimizer_util/px_wrappers.h" // for CloseRelation

namespace px
{
RelationWrapper::~RelationWrapper() noexcept(false)
{
	if (m_relation)
		CloseRelation(m_relation);
}

void
RelationWrapper::Close()
{
	if (m_relation)
	{
		CloseRelation(m_relation);
		m_relation = nullptr;
	}
}
}  // namespace gpdb

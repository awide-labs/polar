/*-------------------------------------------------------------------------
 *
 * polar_feature_utilization.c
 *	  Record the usage of PolarDB developed feature in the real case.
 *
 * Copyright (c) 2024, Alibaba Group Holding Limited
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
 *	  src/backend/utils/misc/polar_feature_utilization.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/shmem.h"
#include "utils/polar_features.h"

PolarUniqueFeatureUtils *adv_utils = NULL;
int			polar_statistics_level = 1;

#define ENUM_OPT_TYPE_CASE(x)   case x: return(#x)

const char *
polar_get_unique_feature_name(polar_unique_feature_t i)
{
	if (i >= PolarUniqueFeatureCount || i < 0)
		elog(ERROR, "polar unique feature index must be between 0 and %d", PolarUniqueFeatureCount - 1);

	switch (i)
	{
			ENUM_OPT_TYPE_CASE(SimpleProtocolExecCount);
			ENUM_OPT_TYPE_CASE(UnnamedStmtExecCount);
			ENUM_OPT_TYPE_CASE(UnparameterizedStmtExecCount);
			ENUM_OPT_TYPE_CASE(DeallocateStmtExecCount);
			ENUM_OPT_TYPE_CASE(RepackTableCount);
			ENUM_OPT_TYPE_CASE(RepackIndexCount);
			ENUM_OPT_TYPE_CASE(RepackApplyLogCount);
			/* impossible be here since precondition. */
			ENUM_OPT_TYPE_CASE(PolarUniqueFeatureCount);
	}
	Assert(false);
	return "impossible";
}

uint64
polar_unique_feature_value(polar_unique_feature_t i)
{
	if (i >= PolarUniqueFeatureCount || i < 0)
		elog(ERROR, "polar unique feature index must be between 0 and %d", PolarUniqueFeatureCount - 1);

	return pg_atomic_read_u64(&adv_utils->stats[i]);
}

void
increase_polar_unique_feature_cnt(int index, int cnt)
{
	if (index < 0 || index >= N_POLAR_UNIQUE_FEATURE_RESERVE)
		elog(ERROR, "invalid input parameter for polar unique feature ID.");
	if (polar_statistics_level >= 1)
	{
		pg_atomic_add_fetch_u64(&adv_utils->stats[index], cnt);
		if (polar_statistics_level >= 2)
		{
			/* Log the feature name and SQL statement. */
			elog(LOG, "increase polar unique feature %s by %d", polar_get_unique_feature_name(index), cnt);
		}
	}
}

Size
polar_feature_shmem_size()
{
	if (PolarUniqueFeatureCount >= N_POLAR_UNIQUE_FEATURE_RESERVE)
		elog(ERROR, "too many polar unique features registered.");
	return sizeof(*adv_utils);
}

void
polar_feature_shmem_init()
{
	bool		found;

	adv_utils = ShmemInitStruct("adv_utils", sizeof(*adv_utils), &found);

	if (!found)
	{
		int			i;

		for (i = 0; i < N_POLAR_UNIQUE_FEATURE_RESERVE; i++)
		{
			pg_atomic_init_u64(&adv_utils->stats[i], 0);
		}
	}
}

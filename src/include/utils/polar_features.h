/*-------------------------------------------------------------------------
 *
 * polar_features.h
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
 *	  src/include/utils/polar_features.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_FEATURE_UTILIZATION_H
#define POLAR_FEATURE_UTILIZATION_H

#include "postgres.h"

#include "port/atomics.h"

#define N_POLAR_UNIQUE_FEATURE_RESERVE 128

typedef struct PolarUniqueFeatureUtils
{
	pg_atomic_uint64 stats[N_POLAR_UNIQUE_FEATURE_RESERVE];
} PolarUniqueFeatureUtils;

/*
 * To record a usage of a polar unique feature, developer just need to
 * add a new type here and modify the polar_get_unique_feature_name
 * accordingly,  then whenever the feature is used, just invoke
 * hit_polar_unique_feature(feature_enum). Then the result can be
 * viewed by 'polar_unique_feature_usage;'
 */
typedef enum polar_unique_feature_t
{
	SimpleProtocolExecCount = 0,
	UnnamedStmtExecCount,
	UnparameterizedStmtExecCount,
	DeallocateStmtExecCount,
	RepackTableCount,
	RepackIndexCount,
	RepackApplyLogCount,
	/* All the enums should be defined prior to PolarUniqueFeatureCount !!! */
	PolarUniqueFeatureCount
} polar_unique_feature_t;


extern PolarUniqueFeatureUtils *adv_utils;

extern Size polar_feature_shmem_size(void);
extern void polar_feature_shmem_init(void);
extern const char *polar_get_unique_feature_name(polar_unique_feature_t i);
extern uint64 polar_unique_feature_value(polar_unique_feature_t i);
extern void increase_polar_unique_feature_cnt(int index, int cnt);
#define hit_polar_unique_feature(index)	increase_polar_unique_feature_cnt(index, 1)

#endif							/* POLAR_FEATURE_UTILIZATION_H */

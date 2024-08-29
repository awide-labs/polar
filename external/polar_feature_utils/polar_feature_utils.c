/*-------------------------------------------------------------------------
 *
 * polar_feature_utils.c
 *	  Export polar feature usage with functions.
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
 *	  external/polar_feature_utils/polar_feature_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/polar_features.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(polar_advisor_feature_name);
PG_FUNCTION_INFO_V1(polar_advisor_feature_value);
PG_FUNCTION_INFO_V1(polar_advisor_feature_count);

Datum
polar_advisor_feature_name(PG_FUNCTION_ARGS)
{
	int			i = PG_GETARG_INT32(0);
	const char *name = polar_get_unique_feature_name(i);

	PG_RETURN_TEXT_P(cstring_to_text(name));
}

Datum
polar_advisor_feature_value(PG_FUNCTION_ARGS)
{
	int			i = PG_GETARG_INT32(0);
	const uint64 value = polar_unique_feature_value(i);

	PG_RETURN_DATUM(value);
}

Datum
polar_advisor_feature_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(PolarUniqueFeatureCount);
}

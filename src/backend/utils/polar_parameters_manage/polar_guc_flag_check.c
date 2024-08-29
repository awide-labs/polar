/*-------------------------------------------------------------------------
 *
 * polar_guc_flag_check.c
 *
 * Implementation of check POLAR guc flag in debug mode
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
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
 *	  src/backend/utils/polar_parameters_manage/polar_guc_flag_check.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/guc.h"
#include "utils/guc_tables.h"

#define POLAR_GUC_HEAD	 "polar"

static const char *skip_check_guc_names_array[] = {
#include "polar_uncheck_guc_list/polar_skip_guc_names.c"
};

static const char *skip_check_extensions_names_array[] = {
#include "polar_uncheck_guc_list/polar_skip_extension_names.c"
};

static void polar_parameter_manage_check_one(struct config_generic *guc_variables);
static bool polar_can_skip_check_flag(const char *name);

/*
 * POLAR:
 * check POLAR guc flag to hint us add them.
 */
void
polar_parameter_manage_check_flag(struct config_generic **guc_variables, int size)
{
	int			i;

#ifndef USE_ASSERT_CHECKING
	/* parameter manage check flag only in debug mode */
	return;
#endif

	for (i = 0; i < size; ++i)
		polar_parameter_manage_check_one(guc_variables[i]);
}

/*
 * POLAR: check one guc flag.
 */
static void
polar_parameter_manage_check_one(struct config_generic *guc)
{
#define HINT_MESSAGE "\n===========================Important===========================\n"                                                                                                                  \
					 "If you need %s's value in online is different from default value in guc.c, Please set that value in polar_settings.conf.[${compability_mode}].[${delpoy_mode}].sample\n"              \
					 "=============================重要=============================\n"                                                                                                                   \
					 "如果期望%s在线上的设定值和guc.c中的默认值不一致,一定要把线上的设定值加入polar_settings.conf.[${compability_mode}].[${delpoy_mode}].sample文件中\n" \
					 "=============================================================",                                                                                                                       \
					 guc->name, guc->name

	/* For safety, skip bad input */
	if (guc == NULL || guc->name == NULL)
		return;

	/* igore planceholder guc */
	if (guc->flags & GUC_CUSTOM_PLACEHOLDER)
		return;

	if (polar_can_skip_check_flag(guc->name))
		return;

	if (!(guc->flags & (POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE | POLAR_GUC_IS_UNCHANGABLE)))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\nPlease mask %s POLAR_GUC_IS_VISIBLE or POLAR_GUC_IS_INVISIBLE and POLAR_GUC_IS_CHANGABLE or POLAR_GUC_IS_UNCHANGABLE", guc->name),
				 errhint(HINT_MESSAGE)));

	if (!(guc->flags & (POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_INVISIBLE)))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\nPlease mask %s POLAR_GUC_IS_VISIBLE or POLAR_GUC_IS_INVISIBLE", guc->name),
				 errhint(HINT_MESSAGE)));

	if (!(guc->flags & (POLAR_GUC_IS_CHANGABLE | POLAR_GUC_IS_UNCHANGABLE)))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\nPlease mask %s POLAR_GUC_IS_CHANGABLE or POLAR_GUC_IS_UNCHANGABLE", guc->name),
				 errhint(HINT_MESSAGE)));
}

/*
 * POLAR: Skip the stock parameter we checked flag
 */
static bool
polar_can_skip_check_flag(const char *name)
{
	const char *extension_suffix_name;
	int			i;

	Assert(name);

	/*
	 * Community parameters can be skipped, including extension guc and kernel
	 * guc.
	 */
	if (strlen(name) < strlen(POLAR_GUC_HEAD) ||
		strncasecmp(name, POLAR_GUC_HEAD, strlen(POLAR_GUC_HEAD)) != 0)
		return true;

	/* Check polar extension guc */
	extension_suffix_name = strchr(name, '.');
	if (extension_suffix_name)
	{
		for (i = 0; i < sizeof(skip_check_extensions_names_array) / sizeof(char *); ++i)
		{
			const char *skip_extname = skip_check_extensions_names_array[i];

			/* Get the length of the input name's extension name */
			int			extname_len = (extension_suffix_name - name);

			Assert((name + extname_len)[0] == '.');

			/* Extension name must fully match can be skipped */
			if (strlen(skip_extname) == extname_len &&
				strncasecmp(name, skip_extname, extname_len) == 0)
				return true;
		}
	}

	/* Check polar kernel guc */
	for (i = 0; i < sizeof(skip_check_guc_names_array) / sizeof(char *); ++i)
	{
		const char *gucname = skip_check_guc_names_array[i];

		if (strcasecmp(name, gucname) == 0)
			return true;
	}

	/* Default need check flag */
	return false;
}

/*-------------------------------------------------------------------------
 *
 * polar_cluster_settings.h
 *	  External declarations pertaining to src/backend/utils/polar_parameters_manage/polar_cluster_settings.c
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
 *	  src/include/utils/polar_cluster_settings.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_GLOBAL_SETTINGS_H
#define POLAR_GLOBAL_SETTINGS_H

#include "access/xlogreader.h"

#define POLAR_SETTING_FILENAME		"polar_settings.conf"

typedef enum PolarAlterOption
{
	ALTOPT_CLUSTER = 1 << 0,	/* Identify if changes are made to
								 * polar_settings.conf */
	ALTOPT_RELOAD = 1 << 1,		/* Identify if reload after command done */
	ALTOPT_FORCE = 1 << 2		/* Identify if force set polar_ guc */
} PolarAlterOption;

typedef enum PolarSettingAction
{
	POLAR_SETTING_SET = 1,		/* ALTER SYSTEM FOR CLUSTER SET ... */
	POLAR_SETTING_RESET = 2,	/* ALTER SYSTEM FOR CLUSTER RESET ... */
	POLAR_SETTING_RESET_ALL = 3 /* ALTER SYSTEM FOR CLUSTER RESET ALL */
} PolarSettingAction;

typedef struct PolarSettingWalHeader
{
	PolarWalType pwt_type;		/* Use POLAR_WAL, our subtype is
								 * PWT_ALTERSYSTEMFORCLUSTER */
	PolarSettingAction action;
} PolarSettingWalHeader;

/*
 * The following content after header is determined by action.
 * 1. POLAR_SETTING_SET: following the string 'name' and 'value', means SET $name to $value.
 * 2. POLAR_SETTING_RESET: following only the string 'name', means RESET $name.
 * 3. POLAR_SETTING_RESET_ALL: following nothing, means RESET ALL.
 * Note: 'name' or 'value' strings are variable length, always end with '\0'.
 */

extern void polar_mount_and_extra_load_polar_settings(const bool polar_is_show_guc_mode);
extern void polar_check_config_file_change(bool docheck);

/* WAL related to ensure consistency between diffrent node */
extern void polar_cluster_setting_wal_redo(XLogReaderState *record);
extern void polar_cluster_setting_wal_write(char *name, char *value, bool resetall, bool reload);;

/* alter system force command check */
extern void polar_alter_force_check(char *name, char *value);

#endif							/* POLAR_GLOBAL_SETTINGS_H */

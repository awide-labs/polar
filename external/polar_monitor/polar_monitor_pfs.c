/*-------------------------------------------------------------------------
 *
 * polar_monitor_pfs.c
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
 *	  external/polar_monitor/polar_monitor_pfs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "miscadmin.h"
#include "utils/guc.h"

/*
 * return the PFS du for a specific depth and datadir
 */
PG_FUNCTION_INFO_V1(pfs_du_with_depth);
Datum
pfs_du_with_depth(PG_FUNCTION_ARGS)
{
	/* Input: depth of the datadir */
	int32		depth = PG_GETARG_INT32(0);
	text	   *data_dir = PG_GETARG_TEXT_P(1);
	char	   *path = text_to_cstring(data_dir);

	FILE	   *fp;

	char		command[1024];
	char		buffer[1024];
	int			status;
	text	   *result_text;
	ssize_t		bytes_read;
	StringInfoData result_data;

	if (polar_disk_name == NULL)
	{
		pfree(path);
		ereport(ERROR, (errmsg("the current_setting(polar_disk_name) is NULL")));
	}

	/* Add logic to check if current user is a superuser */
	if (!superuser())
	{
		pfree(path);
		ereport(ERROR, (errmsg("Only superuser can execute pfs_du function")));
	}

	initStringInfo(&result_data);
	memset(buffer, 0, sizeof(buffer));
	snprintf(command, sizeof(command), "pfsadm du -d %d /%s/%s/", depth, polar_disk_name, path);

	fp = popen(command, "r");
	if (fp == NULL)
	{
		pfree(path);
		ereport(ERROR, (errmsg("popen failed when exec pfsadm du command")));
	}

	while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
	{
		for (ssize_t i = 0; i < bytes_read; i++)
		{
			if (isprint((unsigned char) buffer[i]) || buffer[i] == '\t' || buffer[i] == '\n')
			{
				appendStringInfoChar(&result_data, buffer[i]);
			}
		}
		/* appendStringInfoString(&result_data, buffer); */
		memset(buffer, 0, sizeof(buffer));
	}

	status = pclose(fp);
	pfree(path);

	if (status != 0)
	{
		ereport(LOG, (errmsg("exe pfsadm du command failed, the status is: %d, command is: %s", status, command)));
	}

	result_text = cstring_to_text_with_len(result_data.data, result_data.len);
	pfree(result_data.data);

	PG_RETURN_TEXT_P(result_text);
}

/*
 * return the PFS info
 */
PG_FUNCTION_INFO_V1(pfs_info);
Datum
pfs_info(PG_FUNCTION_ARGS)
{
	FILE	   *fp;

	char		command[1024];
	char		buffer[1024];
	int			status;
	text	   *result_text;
	ssize_t		bytes_read;
	StringInfoData result_data;

	if (polar_disk_name == NULL)
	{
		ereport(ERROR, (errmsg("the current_setting(polar_disk_name) is NULL")));
	}

	/* Add logic to check if current user is a superuser */
	if (!superuser())
	{
		ereport(ERROR, (errmsg("Only superuser can execute pfs_du function")));
	}

	initStringInfo(&result_data);
	memset(buffer, 0, sizeof(buffer));
	snprintf(command, sizeof(command), "pfsadm info %s", polar_disk_name);

	fp = popen(command, "r");
	if (fp == NULL)
	{
		ereport(ERROR, (errmsg("popen failed when exec pfsadm info command")));
	}

	while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
	{
		for (ssize_t i = 0; i < bytes_read; i++)
		{
			if (isprint((unsigned char) buffer[i]) || buffer[i] == '\t' || buffer[i] == '\n')
			{
				appendStringInfoChar(&result_data, buffer[i]);
			}
		}
		/* appendStringInfoString(&result_data, buffer); */
		memset(buffer, 0, sizeof(buffer));
	}

	status = pclose(fp);

	if (status != 0)
	{
		ereport(LOG, (errmsg("exe pfsadm du command failed, the status is: %d, command is: %s", status, command)));
	}

	result_text = cstring_to_text_with_len(result_data.data, result_data.len);
	pfree(result_data.data);

	PG_RETURN_TEXT_P(result_text);
}

/*-------------------------------------------------------------------------
 *
 * polar_stat_env.c
 *	  Collecting environment information of PolarDB-PG.
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
 *	  external/polar_stat_env/polar_stat_env.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "commands/explain.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PG_MODULE_MAGIC;

#define ENV_LANG "LANG=C "

const char *cpu_commands[][2] =
{
	{"Architecture", ENV_LANG "lscpu | grep -i architecture | awk '{print $NF}'"},
	{"Model Name", ENV_LANG "lscpu | grep -i 'model name\\:' | awk -F: '{gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); print $2}'"},
	{"Vendor ID", ENV_LANG "lscpu | grep -i 'Vendor ID\\:' | awk -F: '{gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); print $2}'"},
	{"CPU Cores", ENV_LANG "lscpu | grep '^CPU(s)\\:' | awk '{print $NF}'"},
	{"CPU Thread Per Cores", ENV_LANG "lscpu | grep Thread | awk '{print $NF}'"},
	{"CPU Core Per Socket", ENV_LANG "lscpu | grep Core | awk '{print $NF}'"},
	{"NUMA Nodes", ENV_LANG "lscpu | grep -i 'numa node(s)\\:' | awk '{print $NF}'"},
	{"L1d cache", ENV_LANG "lscpu | grep 'L1d cache:' | awk -F: '{gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); print $2}'"},
	{"L1i cache", ENV_LANG "lscpu | grep 'L1i cache:' | awk -F: '{gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); print $2}'"},
	{"L2 cache", ENV_LANG "lscpu | grep 'L2 cache:' | awk -F: '{gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); print $2}'"},
	{"L3 cache", ENV_LANG "lscpu | grep 'L3 cache:' | awk -F: '{gsub(/^[ \\t]+|[ \\t]+$/, \"\", $2); print $2}'"}
};

const char *mem_commands[][2] =
{
	{"Memory Total (GB)", "cat /proc/meminfo | grep MemTotal | awk '{print int($2 / 1024 / 1024)}'"},
	{"HugePage Size (MB)", "expr $(cat /proc/meminfo  | grep Hugepagesize | awk '{print $2}') / 1024"},
	{"HugePage Total Size (GB)", "expr $(awk '/Hugepagesize/ {print $2}' /proc/meminfo) \\* $(awk '/HugePages_Total/ {print $2}' /proc/meminfo) / 1024 / 1024"}
};

const char *os_commands[][2] =
{
	{"Kernel", "uname -r"},
	{"Distribution", "awk -F= '/^PRETTY_NAME=/ {gsub(/\"/,\"\",$2); print $2}' /etc/os-release"},
	{"Swappiness (1-100)", "cat /proc/sys/vm/swappiness"},
	{"Vfs Cache Pressure (0-1000)", "cat /proc/sys/vm/vfs_cache_pressure"},
	{"Min Free KBytes (KB)", "cat /proc/sys/vm/min_free_kbytes"}
};

#define NUM_OF_CPU_COMMANDS (sizeof(cpu_commands) / sizeof(cpu_commands[0]))
#define NUM_OF_MEM_COMMANDS (sizeof(mem_commands) / sizeof(mem_commands[0]))
#define NUM_OF_OS_COMMANDS (sizeof(os_commands) / sizeof(os_commands[0]))

static void
remove_newlines(char *str)
{
	char	   *read = str;
	char	   *write = str;

	while (*read)
	{
		/* Copy only if the character is not a newline */
		if (*read != '\n')
		{
			*write++ = *read;
		}
		read++;
	}
	*write = '\0';				/* Null - terminate the string */
}

static bool
exec_collect_command(const char *command, StringInfoData *result)
{
	FILE	   *fp;
	char		buffer[128];
	size_t		bytes_read;

	fp = popen(command, "r");
	if (fp == NULL)
	{
		elog(WARNING, "Failed to run command: %s", command);
		return false;
	}

	resetStringInfo(result);

	while ((bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp)) > 0)
	{
		buffer[bytes_read] = '\0';
		appendStringInfoString(result, buffer);
	}
	pclose(fp);

	return true;
}

static void
collect_cpu(ExplainState *es)
{
	bool		success = false;
	StringInfoData cur_data;

	initStringInfo(&cur_data);
	ExplainOpenGroup("CPU", "CPU", true, es);
	for (int i = 0; i < NUM_OF_CPU_COMMANDS; i++)
	{
		resetStringInfo(&cur_data);
		success = exec_collect_command(cpu_commands[i][1], &cur_data);
		if (success)
		{
			if (cur_data.len > 1 && cur_data.data[cur_data.len - 1] == '\n')
			{
				cur_data.data[cur_data.len - 1] = '\0';
			}

			/* Add property into text */
			ExplainPropertyText(cpu_commands[i][0], cur_data.data, es);
		}
	}

	ExplainCloseGroup("CPU", "CPU", true, es);
	pfree(cur_data.data);
	return;
}

static void
collect_mem(ExplainState *es)
{
	bool		success = false;
	StringInfoData cur_data;

	initStringInfo(&cur_data);
	ExplainOpenGroup("Memory", "Memory", true, es);
	for (int i = 0; i < NUM_OF_MEM_COMMANDS; i++)
	{
		resetStringInfo(&cur_data);
		success = exec_collect_command(mem_commands[i][1], &cur_data);
		if (success)
		{
			if (cur_data.len > 1 && cur_data.data[cur_data.len - 1] == '\n')
			{
				cur_data.data[cur_data.len - 1] = '\0';
			}

			/* Add property into text */
			ExplainPropertyText(mem_commands[i][0], cur_data.data, es);
		}
	}

	ExplainCloseGroup("Memory", "Memory", true, es);
	pfree(cur_data.data);
	return;
}

static void
collect_os(ExplainState *es)
{
	bool		success = false;
	StringInfoData cur_data;

	initStringInfo(&cur_data);
	ExplainOpenGroup("OS Params", "OS Params", true, es);
	for (int i = 0; i < NUM_OF_OS_COMMANDS; i++)
	{
		resetStringInfo(&cur_data);
		success = exec_collect_command(os_commands[i][1], &cur_data);
		if (success)
		{
			if (cur_data.len > 1 && cur_data.data[cur_data.len - 1] == '\n')
			{
				cur_data.data[cur_data.len - 1] = '\0';
			}

			/* Add property into text */
			ExplainPropertyText(os_commands[i][0], cur_data.data, es);
		}
	}

	ExplainCloseGroup("OS Params", "OS Params", true, es);
	pfree(cur_data.data);
	return;
}

static void
collect_node_type(ExplainState *es)
{
	static const char *role = "Role";

	switch (polar_get_node_type())
	{
		case POLAR_PRIMARY:
			ExplainPropertyText(role, "Primary", es);
			break;
		case POLAR_REPLICA:
			ExplainPropertyText(role, "Replica", es);
			break;
		case POLAR_STANDBY:
			ExplainPropertyText(role, "Standby", es);
			break;
		default:
			ExplainPropertyText(role, "Unknown", es);
			break;
	}
}

static void
collect_all(ExplainState *es)
{
	ExplainBeginOutput(es);

	collect_node_type(es);

	/* Collect CPU info */
	collect_cpu(es);
	/* Collect memory info */
	collect_mem(es);
	/* Collect OS info */
	collect_os(es);

	ExplainEndOutput(es);

	/* Remove last line break */
	if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
		es->str->data[--es->str->len] = '\0';

	/* Fix JSON to output an object */
	if (es->format == EXPLAIN_FORMAT_JSON)
	{
		es->str->data[0] = '{';
		es->str->data[es->str->len - 1] = '}';
	}
}

static Datum
stat_env(FunctionCallInfo fcinfo, bool need_newline)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;
	text	   *format_txt = PG_GETARG_TEXT_PP(0);
	char	   *format = text_to_cstring(format_txt);
	ExplainState *es = NewExplainState();

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build tuplestore to hold the result rows */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "nodeenv",
					   TEXTOID, -1, 0);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (strcmp(format, "text") == 0)
		es->format = EXPLAIN_FORMAT_TEXT;
	else if (strcmp(format, "xml") == 0)
		es->format = EXPLAIN_FORMAT_XML;
	else if (strcmp(format, "json") == 0)
		es->format = EXPLAIN_FORMAT_JSON;
	else if (strcmp(format, "yaml") == 0)
		es->format = EXPLAIN_FORMAT_YAML;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized value for output format: \"%s\"", format)));
	pfree(format);

	collect_all(es);

	if (!need_newline && es && es->str)
		remove_newlines(es->str->data);

	tuplestore_puttuple(tupstore,
						BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupdesc),
											   &es->str->data));
	pfree(es->str->data);
	pfree(es);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(polar_stat_env);
Datum
polar_stat_env(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use polar_stat_env"))));

	return stat_env(fcinfo, true);
}

/*
 * Usage:
 * 1. COPY (SELECT polar_stat_env_no_format('json')) TO '/path/to/output_file.json';
 * 2. cat /path/to/output_file.json | jq .
 */
PG_FUNCTION_INFO_V1(polar_stat_env_no_format);
Datum
polar_stat_env_no_format(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use polar_stat_env"))));

	return stat_env(fcinfo, false);
}

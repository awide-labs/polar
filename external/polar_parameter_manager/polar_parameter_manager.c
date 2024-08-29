/*-------------------------------------------------------------------------
 *
 * polar_parameter_manager.c
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
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
 *	  external/polar_parameter_manager/polar_parameter_manager.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "funcapi.h"
#include "miscadmin.h"

#define POLAR_DESC_INFO_LEN 4

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(polar_get_guc_info);

Datum
polar_get_guc_info(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext oldcontext;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	struct config_generic *conf;
	Datum		values[POLAR_DESC_INFO_LEN];
	bool		nulls[POLAR_DESC_INFO_LEN];
	bool		is_visible;
	bool		is_changable;
	bool		is_list_input;	/* for new version parameter manger flag
								 * judged by CM */

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context "
						"that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	conf = polar_parameter_check_name_internal(name);

	if (conf == NULL)
	{
		MemSet(nulls, 1, sizeof(nulls));
		goto end_ret;
	}

	/* set is_visible, default is false */
	is_visible = false;
	if (conf->flags & POLAR_GUC_IS_VISIBLE)
		is_visible = true;
	else if (conf->flags & POLAR_GUC_IS_INVISIBLE)
		is_visible = false;
	/* Make the configuration of autovacuum available to users */
	else if (conf->group == AUTOVACUUM)
		is_visible = true;

	/* set is_changable, default is true */
	is_changable = true;
	if (conf->flags & POLAR_GUC_IS_CHANGABLE)
		is_changable = true;
	else if (conf->flags & POLAR_GUC_IS_UNCHANGABLE)
		is_changable = false;
	/* heuristic rules for unchangable */
	else if (conf->context == PGC_INTERNAL)
		is_changable = false;
	else if (
#ifdef POLAR_TODO
			 conf->group == POLAR_NODE_STATIC ||
#endif
			 conf->group == RESOURCES_BGWRITER ||
			 conf->group == REPLICATION_STANDBY ||
			 conf->group == REPLICATION_SENDING ||
			 conf->group == WAL_ARCHIVING ||
			 conf->group == WAL_CHECKPOINTS ||
			 conf->group == WAL_SETTINGS ||
			 conf->group == WAL_RECOVERY_TARGET ||
			 conf->group == CONN_AUTH_SETTINGS ||
			 conf->group == CONN_AUTH_SSL ||
			 conf->group == FILE_LOCATIONS ||
			 conf->group == LOCK_MANAGEMENT ||
			 conf->group == LOGGING_WHERE ||
			 conf->group == LOGGING_WHAT ||
			 conf->group == STATS_CUMULATIVE ||
			 conf->group == STATS_MONITORING)
		is_changable = false;
	else if (conf->flags & (GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_DISALLOW_IN_AUTO_FILE | GUC_CUSTOM_PLACEHOLDER))
		is_changable = false;

	/* set Datum */
	values[0] = BoolGetDatum(is_visible);
	values[1] = BoolGetDatum(is_changable);
	/* set optional */
	if (conf->optional != NULL)
		values[2] = CStringGetTextDatum(conf->optional);
	else
	{
		values[2] = CStringGetTextDatum("");
		nulls[2] = true;
	}
	/* for new version parameter manger */
	if (tupdesc->natts > 3)
	{
		if (conf->flags & GUC_LIST_INPUT)
			is_list_input = true;
		else
			is_list_input = false;

		values[3] = BoolGetDatum(is_list_input);
	}

end_ret:
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

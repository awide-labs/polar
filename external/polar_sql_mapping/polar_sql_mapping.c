/*-------------------------------------------------------------------------
 *
 * polar_sql_mapping.c
 *	  A plugin that can mapping the source SQL to target SQL
 * at the beginning of the run.
 *
 * error detective
 * A hook is placed in errfinish(). When an error arising, we will record the
 * error sql string and some additional info in a hash table which allocated
 * in shared memory. User can get the records through "error_sql_info"
 * view which created when creating the "polar_sql_mapping" extension. A reset
 * function is also provided to reset the hash table.
 *
 * Copyright (c) 2023, Alibaba Group Holding Limited
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
 *	  external/polar_sql_mapping/polar_sql_mapping.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/prepare.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/guc.h"

#include "polar_sql_mapping.h"
PG_MODULE_MAGIC;

/* GUC variables */
static bool use_sql_mapping = false;
int			log_usage = 0;
int			psm_max_num;		/* max sqls to record */
bool		record_error_sql;
char	   *unexpected_error_catagory;	/* contain the unexpected error type */
char	   *error_pattern = NULL;	/* define the pattern of error sqls */

#define GetSqlMappingSchemeOid() (get_namespace_oid("polar_sql_mapping", true))
#define GetIdSeqOid()			 (get_sqlmapping_relid("polar_sql_mapping_id_sequences"))
#define GetIndexSourceSqlOid()	 (get_sqlmapping_relid("polar_sql_mapping_source_sql_unique_idx"))

/* Saved hook values in case of unload */
static sql_mapping_hook_type pre_sql_mapping_hook = NULL;
#if (PG_VERSION_NUM >= 150000)
shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif
shmem_startup_hook_type prev_shmem_startup_hook = NULL;
polar_record_error_sql_hook_type prev_record_error_sql_hook = NULL;

enum
{
	Anum_sqlmapping_id = 1,
	Anum_sqlmapping_sourcesql = 2,
	Anum_sqlmapping_targetsql = 3
} SqlMappingTableAttributes;
#define Natts_sqlmapping 3

static const struct config_enum_entry log_usage_options[] = {
	{"none", 0, true},
	{"debug", DEBUG2, true},
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"log", LOG, false},
	{"info", INFO, true},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{NULL, 0, false}
};

void		_PG_init(void);

PG_FUNCTION_INFO_V1(error_sql_info);
PG_FUNCTION_INFO_V1(error_sql_info_clear);

/* hook functions */
#if (PG_VERSION_NUM >= 150000)
static void sql_mapping_shmem_request(void);
#endif
static const char *do_sql_mapping(const char *query_string);

/* sqlmapping functions */
static const char *search_sqlmapping(const char *source_sql);
static bool psm_check_pattern(const char *sql_text);

/* sqlmapping tools */
static Oid	get_sqlmapping_relid(const char *relname);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system. (We don't throw error here because it seems useful to
	 * allow the polar_sql_mapping functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;


	/* Define custom GUC variables. */

	/* Set the max number of error sql records. */
	DefineCustomIntVariable("polar_sql_mapping.max_num",
							"Sets the maximum number of error sqls tracked by polar_sql_mapping.",
							NULL,
							&psm_max_num,
							10,
							0,
							5000,
							PGC_POSTMASTER,
							POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("polar_sql_mapping.record_error_sql",
							 "Set the polar_sql_mapping.record_error_sql to determine psm_record_error_sql function on/off.",
							 NULL,
							 &record_error_sql,
							 false,
							 PGC_SUSET,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomStringVariable("polar_sql_mapping.unexpected_error_catagory",
							   "Set the unexcepted ERROR catagory. Different type should be split by ','.",
							   NULL,
							   &unexpected_error_catagory,
							   "23,25",
							   PGC_SUSET,
							   POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomStringVariable("polar_sql_mapping.error_pattern",
							   "Set the ERROR SQL pattern, force capture the sql which LIKE pattern.",
							   NULL,
							   &error_pattern,
							   "",
							   PGC_SUSET,
							   POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomBoolVariable("polar_sql_mapping.use_sql_mapping",
							 "Set the polar_sql_mapping.use_sql_mapping to determine polar_sql_mapping function on/off.",
							 NULL,
							 &use_sql_mapping,
							 false,
							 PGC_SUSET,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("polar_sql_mapping.log_usage",
							 "Set polar_sql_mapping.log_usage to determine polar_sql_mapping's different log usage.",
							 NULL,
							 &log_usage,
							 0,
							 log_usage_options,
							 PGC_SUSET,
							 POLAR_GUC_IS_INVISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	EmitWarningsOnPlaceholders("polar_sql_mapping");

#if (PG_VERSION_NUM >= 150000)
	prev_shmem_request_hook = shmem_request_hook;
#else

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in psm_shmem_startup().
	 */
	RequestAddinShmemSpace(psmss_memsize());
	RequestNamedLWLockTranche("psm_error_sql_state", 1);
#endif
	/* Install hooks. */
	shmem_request_hook = sql_mapping_shmem_request;
	pre_sql_mapping_hook = sql_mapping_hook;
	sql_mapping_hook = do_sql_mapping;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = psm_shmem_startup;
	prev_record_error_sql_hook = polar_record_error_sql_hook;
	polar_record_error_sql_hook = psm_record_error_sql;
}

#if (PG_VERSION_NUM >= 150000)
static void
sql_mapping_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in psm_shmem_startup().
	 */
	RequestAddinShmemSpace(psmss_memsize());
	RequestNamedLWLockTranche("psm_error_sql_state", 1);
}
#endif

/*
 * do_sql_mapping
 * If we find source_sql in sql_mapping_table equals to query_string,
 * We mapping it into target_sql.
 */
static const char *
do_sql_mapping(const char *query_string)
{
	const char *target_sql_text = NULL;

	if (pre_sql_mapping_hook)
		query_string = (*pre_sql_mapping_hook) (query_string);

	Assert(query_string);

	/* Force record sql like error_pattern. */
	if (record_error_sql && psm_check_pattern(query_string))
		psm_insert_error_sql(query_string, "Error Pattern Force Record");

	/*
	 * We need to make sure we are in a valid transaction (TRANS_INPROGRESS),
	 * so that we can open relation and search sql mapping. If the transaction
	 * is going to rollback because of a previous sql's execution failure, we
	 * can't pass assert statement in RelationIdGetRelation().
	 */
	if (!use_sql_mapping || !IsTransactionState())
		return query_string;

	target_sql_text = search_sqlmapping(query_string);

	if (target_sql_text)
	{
		elog(log_usage, "sql mapping: change sql to \'%s\'.", target_sql_text);
		return target_sql_text;
	}

	return query_string;
}

/*
 * Search polar_sql_mapping_table to find whether source_sql is exist,
 * if exist, return the target_sql.
 */
static const char *
search_sqlmapping(const char *source_sql)
{
	ScanKeyData source_sql_key;
	Relation	sql_mapping_heap;
	LOCKMODE	heap_lock = AccessShareLock;
	SysScanDesc scandesc;
	HeapTuple	htup;
	const char *target_sql = NULL;
	Oid			mapping_rel_oid = get_sqlmapping_relid("polar_sql_mapping_table");
	Oid			index_source_sql_oid = GetIndexSourceSqlOid();

	if (!OidIsValid(mapping_rel_oid) || !OidIsValid(index_source_sql_oid))
		return NULL;

	ScanKeyInit(&source_sql_key,
				Anum_sqlmapping_sourcesql,
				BTEqualStrategyNumber,
				F_TEXTEQ,
				CStringGetTextDatum(source_sql));

	sql_mapping_heap = table_open(mapping_rel_oid, heap_lock);
	scandesc = systable_beginscan(sql_mapping_heap, index_source_sql_oid, true,
								  NULL, 1, &source_sql_key);

	htup = systable_getnext(scandesc);

	if (HeapTupleIsValid(htup))
	{
		Datum		values[Natts_sqlmapping];
		bool		nulls[Natts_sqlmapping];

		heap_deform_tuple(htup, RelationGetDescr(sql_mapping_heap), values, nulls);

		target_sql = TextDatumGetCString(values[Anum_sqlmapping_targetsql - 1]);

		elog(log_usage, "sql mapping exist. The id = %ld", DatumGetInt64(values[Anum_sqlmapping_id - 1]));
	}

	systable_endscan(scandesc);
	table_close(sql_mapping_heap, heap_lock);

	return target_sql;
}

/*
 * psm_check_pattern
 * 		Check whether sql_text LIKE error_pattern.
 */
static bool
psm_check_pattern(const char *sql_text)
{
	/* Mismatch pattern by default */
	if (error_pattern == NULL || *error_pattern == '\0')
		return false;

	Assert(sql_text);

	return DatumGetBool(DirectFunctionCall2(textlike, CStringGetTextDatum(sql_text), CStringGetTextDatum(error_pattern)));
}

/*
 * Get Oid of the relation needed by the sql_mapping.
 * If it is not exist return InvalidOid.
 */
static Oid
get_sqlmapping_relid(const char *relname)
{
	Oid			nsp = GetSqlMappingSchemeOid();

	if (OidIsValid(nsp))
		return get_relname_relid(relname, nsp);

	return InvalidOid;
}


/*
 * error_sql_info_clear
 *		Reset all records in hash map.
 */
Datum
error_sql_info_clear(PG_FUNCTION_ARGS)
{
	/* Reset! */
	psm_entry_reset();
	return (Datum) 0;
}

/*
 * error_sql_info
 * 		Legacy entry point for error_sql_info() API versions 1.0.
 *
 * Note: User can use this function like a view due to we set up in #.sql.
 */
Datum
error_sql_info(PG_FUNCTION_ARGS)
{
	psm_sql_mapping_error_internal(fcinfo);

	return (Datum) 0;
}

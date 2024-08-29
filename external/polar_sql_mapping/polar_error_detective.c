/*-------------------------------------------------------------------------
 *
 * polar_error_detective.c
 *	  Record the error SQL and some message in shared-memory.
 *
 * A hook is placed in errfinish(). When an error arising, we will record the
 * error sql string and some additional info in a hash table which allocated
 * in shared memory. User can get the records through "polar_sql_mapping_error"
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
 *	  external/polar_sql_mapping/polar_error_detective.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "commands/sequence.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/varlena.h"

#include "polar_sql_mapping.h"


#define PSM_ERRSQL_MAXLEN		(1024 * 16)
#define PSM_ERRMESSAGE_MAXLEN	(128)
/* Number of output arguments (columns) for various API versions */
#define POLAR_SQL_MAPPING_ERROR_COLS		(4) /* maximum of above */

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * sqls by sql string and sql length. The spaces at the beginning and end of
 * the sql string have been removed by scanner.
 *
 * Right now, this structure contains no padding.  If you add any, the hashkey
 * value will not change.  Because we defined psm_hash_value() to hash this which
 * only determined by error_sql_str and error_sql_len. You can change psm_hash_value()
 * or use internal hash functions if you want to change hashkey value too.
 */
typedef struct psmHashKey
{
	char		error_sql_str[PSM_ERRSQL_MAXLEN];	/* error sql string */
	uint32		error_sql_len;	/* sql length */
} psmHashKey;

/*
 * Info of per record
 *
 * Note: Array of char was used to store sql string and error message. So they
 * were allocated in shared memory and some space was wasted. Use pointer of
 * char to identify address and use file to store sql string and error message
 * can avoid it. We just limit the max number of entries, max length of sql string
 * and max length of error message at present.
 */
typedef struct psmEntry
{
	psmHashKey	key;			/* hash key of entry - MUST BE FIRST */
	char		errmessage[PSM_ERRMESSAGE_MAXLEN];	/* error message of sql
													 * string */
	pg_atomic_uint64 calls;		/* # of times executed */
	int64		id;				/* identifier of enrty */
} psmEntry;

/* Global shared state */
typedef struct psmSharedstate
{
	LWLock	   *lock;			/* protects hashtable search/modification */
	int64		id_count;		/* generate the id for one error sql record */
} psmSharedstate;

/* Links to shared memory state */
static psmSharedstate *psmss = NULL;
static HTAB *psmss_hash = NULL;

static bool psm_is_unexpected_error(ErrorData *edata);
static uint32 psm_hash_value(const void *key, Size keysize);
static int	psm_hash_compare(const void *key1, const void *key2, Size keysize);


/*
 * psm_shmem_startup
 *		Allocate or attach to shared memory.
 */
void
psm_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	size_t		init_size;
	size_t		max_size;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	psmss = NULL;
	psmss_hash = NULL;
	init_size = max_size = psm_max_num;

	/* Create or attach to the shared memory state, including hash table. */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	psmss = ShmemInitStruct("psm_error_sql_state",
							sizeof(psmSharedstate),
							&found);

	if (!found)
	{
		/* Initialize the psmss. */
		psmss->lock = &(GetNamedLWLockTranche("psm_error_sql_state"))->lock;
		psmss->id_count = 1;
	}

	/*
	 * Set info. It will instruct to create hash table. Hash key, hash entry
	 * hash function and key comparison function were defined by ourselves. So
	 * HASH_ELEM, HASH_FUNCTION, HASH_COMPARE are used.
	 */
	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(psmHashKey);
	info.entrysize = sizeof(psmEntry);
	info.hash = psm_hash_value;
	info.match = psm_hash_compare;
	psmss_hash = ShmemInitHash("psm_error_sql record",
							   init_size, max_size,
							   &info,
							   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);
}

/*
 * psm_record_error_sql
 * 		Record the error sql string in hash table. ERROR is not expected in
 * 		this function. And return directly if the detected result is not
 * 		expected. The hash table does not record any error sql string when
 * 		the hash table is full.
 */

void
psm_record_error_sql(ErrorData *edata)
{
	if (prev_record_error_sql_hook)
		prev_record_error_sql_hook(edata);

	if (!record_error_sql)
		return;

	/*
	 * The debug_query_string can be NULL in some special cases. We do not
	 * handle those cases.
	 */
	if (debug_query_string == NULL)
		return;

	/*
	 * We can't catch the error inside the PSM, otherwise it will recurse
	 * infinitely. We check this case using in_error_recursion_trouble.
	 */
	if (in_error_recursion_trouble())
		return;

	/* Only capture ERROR sql with expected error type. */
	if (edata->elevel == ERROR && !psm_is_unexpected_error(edata))
		psm_insert_error_sql(debug_query_string, edata->message);
}

/*
 * Insert error sql and error message to polar_sql_mapping.error_sql_info.
 */
void
psm_insert_error_sql(const char *sql_text, const char *emessage)
{
	psmHashKey	key;
	psmEntry   *entry;
	bool		found;
	int64		id;

	/* Control log output frequency */
	static TimestampTz last_log_time_tbl = 0;

	/* For safety. */
	if (!psmss || !psmss_hash)
		return;

	Assert(sql_text);

	key.error_sql_len = strlen(sql_text);

	/*
	 * We use shared memory to save sql_string. Therefore, we do not capture
	 * too long SQL.
	 */
	if (key.error_sql_len >= PSM_ERRSQL_MAXLEN)
		return;

	strcpy(key.error_sql_str, sql_text);

	/*
	 * Firstly, get a LW_SHARED lock to find whether a entry existed in hash
	 * table or not. If the entry is not in hash table and hash table is full,
	 * hash table will not record the entry. Otherwise, the entry->calls will
	 * update.
	 */
	LWLockAcquire(psmss->lock, LW_SHARED);

	entry = hash_search(psmss_hash, &key, HASH_FIND, &found);
	if (!found)
	{
		/* Hash table map is full. Do not record error sqls anymore. */
		if (hash_get_num_entries(psmss_hash) >= psm_max_num)
		{
			LWLockRelease(psmss->lock);
			/* Do not log frequently, every 10s log one */
			if (TimestampDifferenceExceeds(last_log_time_tbl, GetCurrentTimestamp(), 10000))
			{
				elog(LOG, "psm_error_sql record table is full. \
						use polar_sql_mapping.error_sql_info_clear() function to reset it. ");
				last_log_time_tbl = GetCurrentTimestamp();
			}

			return;
		}
	}
	else
	{
		/*
		 * The entry was found and update the calls member. The type of calls
		 * is pg_atomic_uint64 which ensure its value is correct in the case
		 * of concurrency. And then return directly.
		 */
		pg_atomic_fetch_add_u64(&entry->calls, 1);
		LWLockRelease(psmss->lock);
		return;
	}

	LWLockRelease(psmss->lock);

	LWLockAcquire(psmss->lock, LW_EXCLUSIVE);

	/*
	 * The action is HASH_ENTER_NULL. If there is no space in the hash table,
	 * NULL will be returned instead of throwing an ERROR. Although a check
	 * has been performed above, between releasing the lock and re-acquiring
	 * it, another process may have filled up the hash table.
	 *
	 * The entry is not found in hash table above. Perhaps another process has
	 * recorded the entry in hash table. Check the value of found again. Set
	 * calls and id if found is fasle. Otherwise just update calls.
	 */
	entry = (psmEntry *) hash_search(psmss_hash, &key, HASH_ENTER_NULL, &found);
	if (!entry)
	{
		LWLockRelease(psmss->lock);
		/* Do not log frequently, every 10s log one */
		if (TimestampDifferenceExceeds(last_log_time_tbl, GetCurrentTimestamp(), 10000))
		{
			elog(LOG, "psm_error_sql record table is full. \
					use polar_sql_mapping.error_sql_info_clear() function to reset it. ");
			last_log_time_tbl = GetCurrentTimestamp();
		}
		return;
	}

	if (!found)
	{
		/* Inital the entry. */
		strncpy(entry->errmessage, emessage, PSM_ERRMESSAGE_MAXLEN - 1);
		entry->errmessage[PSM_ERRMESSAGE_MAXLEN - 1] = '\0';
		id = (psmss->id_count)++;
		Assert(id > 0);
		entry->id = id;
		pg_atomic_init_u64(&entry->calls, 1);
	}
	else
	{
		pg_atomic_fetch_add_u64(&entry->calls, 1);
	}
	LWLockRelease(psmss->lock);
}

/*
 * psm_sql_mapping_error_internal
 * 		Common code for all versions of polar_sql_mapping_error(). Output the records
 * 		stored in hash table. The output numbers and types shoule be euqal to the
 * 		function defined in #.sql associated with polar_sql_mapping_error().
 */
void
psm_sql_mapping_error_internal(FunctionCallInfo fcinfo)
{
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	HASH_SEQ_STATUS hash_seq;
	psmEntry   *entry;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* hash table must exist already */
	if (!psmss || !psmss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("polar_sql_mapping must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	Assert(tupdesc->natts == POLAR_SQL_MAPPING_ERROR_COLS);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(psmss->lock, LW_SHARED);

	/* Sequentialy scan hash table. */
	hash_seq_init(&hash_seq, psmss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[POLAR_SQL_MAPPING_ERROR_COLS];
		bool		nulls[POLAR_SQL_MAPPING_ERROR_COLS];
		char	   *enc;
		int			i = 0;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->id);
		enc = pg_any_to_server(entry->key.error_sql_str,
							   entry->key.error_sql_len,
							   GetDatabaseEncoding());
		values[i++] = CStringGetTextDatum(enc);
		enc = pg_any_to_server(entry->errmessage,
							   strlen(entry->errmessage),
							   GetDatabaseEncoding());
		values[i++] = CStringGetTextDatum(enc);
		values[i++] = Int64GetDatumFast(pg_atomic_read_u64(&entry->calls));

		Assert(i == POLAR_SQL_MAPPING_ERROR_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	LWLockRelease(psmss->lock);
	tuplestore_donestoring(tupstore);
}

/*
 * psm_is_unexpected_error
 * 		Detect the error whether is unexpected type or not.
 *
 * Note: Unpack the sqlerrcode and compare with unexpected error type.
 * The error type can be find in errors.h and errcodes.h. Please refer
 * to https://www.postgresql.org/docs/12/errcodes-appendix.html for
 * more details. Discard the errcode "23XXX", "25XXX" by default. The
 * errcode represents :
 * Class 23 — Integrity Constraint Violation,
 * Class 25 — Invalid Transaction State.
 *
 * You can change the unexpected error type by setting the variable
 * polar_sql_mapping.unexpected_error_catagory. The error type must
 * be split by ','.
 */
static bool
psm_is_unexpected_error(ErrorData *edata)
{
	char	   *source_errcode;
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;

	/* unpack the error code */
	source_errcode = unpack_sql_state(edata->sqlerrcode);

	/* Need a modifiable copy of string */
	rawstring = pstrdup(unexpected_error_catagory);

	/* Parse string into list of identifiers */
	if (SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* Check whether each token is an error type. */
		foreach(l, elemlist)
		{
			char	   *per_str = (char *) lfirst(l);
			int			per_strlen = 0;

			per_strlen = strlen(per_str);
			Assert(per_strlen >= 0);

			/*
			 * The max length of error type is 5. If length of a assigned
			 * error type greater than 5, we will ignore this error type check
			 * and skip to next one.
			 */
			if (per_strlen > 5)
			{
				/* Used by unexpected_error_catagory */
				static TimestampTz last_log_time_uec = 0;

				/* Do not log frequently, every 10s log one */
				if (TimestampDifferenceExceeds(last_log_time_uec, GetCurrentTimestamp(), 10000))
				{
					elog(LOG, "ERROR type %s is assigned illegally. \
						The max length of ERROR type is 5. Please reset the unexpected_error_catagory.", per_str);
					last_log_time_uec = GetCurrentTimestamp();
				}
				continue;
			}

			if (strncmp(source_errcode, per_str, per_strlen) == 0)
			{
				pfree(rawstring);
				list_free(elemlist);
				return true;
			}
		}
	}
	elog(LOG, "The source error code is %s.", source_errcode);
	pfree(rawstring);
	list_free(elemlist);
	return false;
}

/*
 * psm_entry_reset
 * 		Release all entries.
 */
void
psm_entry_reset(void)
{
	HASH_SEQ_STATUS hash_seq;
	psmEntry   *entry;

	if (!psmss || !psmss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("polar_sql_mapping must be loaded via shared_preload_libraries")));

	LWLockAcquire(psmss->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, psmss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(psmss_hash, &entry->key, HASH_REMOVE, NULL);
	}

	LWLockRelease(psmss->lock);
}

/*
 * psmss_memsize
 * 		Estimate shared memory space needed.
 */
Size
psmss_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(psmSharedstate));
	size = add_size(size, hash_estimate_size(psm_max_num, sizeof(psmEntry)));

	return size;
}

/*
 * psm_hash_value
 * 		Get a hashkey value according the string. The same string will get the
 * 		same hashkey value.
 */
static uint32
psm_hash_value(const void *key, Size keysize)
{
	const psmHashKey *item = (const psmHashKey *) key;

	return DatumGetUInt32(hash_any((const unsigned char *) item->error_sql_str, item->error_sql_len));
}

/*
 * psm_hash_compare
 * 		Define how to compare with key1 and key2. The parameters and return type
 * 		keep consistence with internal hash compare function.
 */
static int
psm_hash_compare(const void *key1, const void *key2, Size keysize)
{
	psmHashKey *k1 = (psmHashKey *) key1;
	psmHashKey *k2 = (psmHashKey *) key2;

	if (k1->error_sql_len != k2->error_sql_len)
		return 1;

	if (strncmp(k1->error_sql_str, k2->error_sql_str, k1->error_sql_len))
		return 1;

	return 0;
}

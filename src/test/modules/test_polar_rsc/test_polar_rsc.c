/*-------------------------------------------------------------------------
 *
 * test_polar_rsc.c
 *	  Test module for PolarDB-PG relation size cache.
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
 *	  src/test/modules/test_polar_rsc/test_polar_rsc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "storage/smgr.h"

#include "storage/polar_rsc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_polar_rsc);
Datum
test_polar_rsc(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_polar_rsc_stat_entries);
Datum
test_polar_rsc_stat_entries(PG_FUNCTION_ARGS)
{
	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	polar_rsc_stat_pool_entries((ReturnSetInfo *) fcinfo->resultinfo, true);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(test_polar_rsc_search_by_ref);
Datum
test_polar_rsc_search_by_ref(PG_FUNCTION_ARGS)
{
	Oid			relnode = PG_GETARG_OID(0);
	RelFileNode rnode;
	SMgrRelation reln;
	BlockNumber nblocks;

	rnode.dbNode = MyDatabaseId;
	rnode.spcNode = MyDatabaseTableSpace;
	rnode.relNode = relnode;

	reln = smgropen(rnode, InvalidBackendId);
	nblocks = polar_rsc_search_by_ref(reln, MAIN_FORKNUM);
	smgrclose(reln);

	PG_RETURN_UINT32(nblocks);
}

PG_FUNCTION_INFO_V1(test_polar_rsc_search_by_mapping);
Datum
test_polar_rsc_search_by_mapping(PG_FUNCTION_ARGS)
{
	Oid			relnode = PG_GETARG_OID(0);
	RelFileNode rnode;
	SMgrRelation reln;
	BlockNumber nblocks;

	rnode.dbNode = MyDatabaseId;
	rnode.spcNode = MyDatabaseTableSpace;
	rnode.relNode = relnode;

	reln = smgropen(rnode, InvalidBackendId);
	nblocks = polar_rsc_search_by_mapping(reln, MAIN_FORKNUM);
	smgrclose(reln);

	PG_RETURN_UINT32(nblocks);
}

PG_FUNCTION_INFO_V1(test_polar_rsc_update_entry);
Datum
test_polar_rsc_update_entry(PG_FUNCTION_ARGS)
{
	Oid			relnode = PG_GETARG_OID(0);
	RelFileNode rnode;
	SMgrRelation reln;
	BlockNumber nblocks;

	rnode.dbNode = MyDatabaseId;
	rnode.spcNode = MyDatabaseTableSpace;
	rnode.relNode = relnode;

	reln = smgropen(rnode, InvalidBackendId);
	nblocks = polar_rsc_update_entry(reln, MAIN_FORKNUM, InvalidBlockNumber);
	smgrclose(reln);

	PG_RETURN_UINT32(nblocks);
}

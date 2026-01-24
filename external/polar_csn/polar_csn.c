/*-------------------------------------------------------------------------
 *
 * polar_csn.c
 *		various csn related function.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
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
 *	  external/polar_csn/polar_csn.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"
#include "access/transam.h"

/*
 * txid and TxidSnapshot type are
 * copied from txid.c
 */

/* Use unsigned variant internally */
typedef uint64 txid;

/*
 * Snapshot containing 8byte txids.
 */
typedef struct
{
	/*
	 * 4-byte length hdr, should not be touched directly.
	 *
	 * Explicit embedding is ok as we want always correct alignment anyway.
	 */
	int32		__varsz;

	uint32		nxip;			/* number of txids in xip array */
	txid		xmin;
	txid		xmax;
	/* in-progress txids, xmin <= xip[i] < xmax: */

	/*
	 * POLAR csn
	 *
	 * To make txid_snapshot storage compatible, we should store csn in xip
	 * and store an invalid xid in front of csn to differentiate csn snapshot
	 * with xid snapshot
	 */
	txid		xip[FLEXIBLE_ARRAY_MEMBER];
} TxidSnapshot;

extern bool is_csn_snapshot(const TxidSnapshot *snap);
extern CommitSeqNo txid_snapshot_get_csn(const TxidSnapshot *snap);

PG_MODULE_MAGIC;

/*
 * txid_snapshot_csn(txid_snapshot) returns int8
 *
 *		return snapshot's csn
 */
PG_FUNCTION_INFO_V1(txid_snapshot_csn);

Datum
txid_snapshot_csn(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);
	CommitSeqNo csn = InvalidCommitSeqNo;

	if (is_csn_snapshot(snap))
	{
		csn = txid_snapshot_get_csn(snap);
	}

	PG_RETURN_INT64(csn);
}

/*
 * txid_csn(int8) returns int8
 *
 *		return xid's csn
 */
PG_FUNCTION_INFO_V1(txid_csn);

Datum
txid_csn(PG_FUNCTION_ARGS)
{
	TransactionId xid = (TransactionId) PG_GETARG_INT64(0);

	if (polar_csn_enable)
		PG_RETURN_UINT64(polar_xact_get_csn(xid, POLAR_CSN_MAX_NORMAL, false));
	else
		PG_RETURN_UINT64(InvalidCommitSeqNo);
}

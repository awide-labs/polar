/*-------------------------------------------------------------------------
 *
 * polar_datamax_internal.h
 *	  datamax corresponding definitions for client-side .c files.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
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
 *    src/include/polar_datamax/polar_datamax_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_DATAMAX_INTERNAL_H
#define POLAR_DATAMAX_INTERNAL_H

#include "c.h"
#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "port/pg_crc32c.h"

#define POLAR_DATAMAX_MAGIC           (0X0518)
/* different version from that before POLARDB_11_REL_20201230 */
#define POLAR_DATAMAX_VERSION         (0x0002)

#define POLAR_DATAMAX_NOT_INITIAL_STREAM 0x00

#define POLAR_LOG_DATAMAX_META_INFO(meta) \
	do { \
		elog(LOG, "datamax meta: magic=%x, version=%x, min_received_lsn=%lx, last_received_lsn=%lx, last_valid_received_lsn=%lx, upstream_last_removed_segno=%lx", \
			 (meta)->magic, (meta)->version, \
			 (meta)->min_received_lsn, (meta)->last_received_lsn, \
			 (meta)->last_valid_received_lsn, (meta)->upstream_last_removed_segno); \
	} while (0)

#define POLAR_DATAMAX_META  (&polar_datamax_ctl->meta)
#define POLAR_DATAMAX_STREAMING_TIMELINE (polar_datamax_ctl->meta.last_timeline_id)
#define POLAR_DATAMAX_STREAMING_LSN (polar_datamax_ctl->meta.last_valid_received_lsn)

#define POLAR_INVALID_TIMELINE_ID     ((uint32)0)

#define POLAR_DATAMAX_IS_VALID_CLEAN_TASK(task)     (!XLogRecPtrIsInvalid((task)->reserved_lsn))

#define POLAR_DATAMAX_META_FILE         "polar_datamax_meta"

typedef struct polar_datamax_meta_data_t
{
	uint32		magic;
	uint32		version;
	TimeLineID	min_timeline_id;
	XLogRecPtr	min_received_lsn;
	TimeLineID	last_timeline_id;	/* last received timeline id */
	XLogRecPtr	last_received_lsn;	/* last received lsn */
	XLogRecPtr	last_valid_received_lsn;	/* end of the last valid record
											 * received from primay */
	XLogSegNo	upstream_last_removed_segno;	/* last removed segno in
												 * upstream */

	pg_crc32c	crc;
}			polar_datamax_meta_data_t;

typedef struct polar_datamax_clean_task_t
{
	XLogRecPtr	reserved_lsn;
	bool		force;
}			polar_datamax_clean_task_t;

typedef struct polar_datamax_xlog_parse_private
{
	TimeLineID	timeline;
	XLogRecPtr	start_ptr;
	XLogRecPtr	end_ptr;
	bool		reach_end_ptr;
}			polar_datamax_xlog_parse_private;

typedef struct polar_datamax_valid_lsn_list
{
	dlist_head	valid_lsn_lhead;	/* list head */
	int			list_length;	/* list length */
} polar_datamax_valid_lsn_list;

typedef struct polar_datamax_valid_lsn
{
	XLogRecPtr	primary_valid_lsn;
	dlist_node	valid_lsn_lnode;	/* list link */
}			polar_datamax_valid_lsn;

typedef enum
{
	POLAR_DATAMAX_OPEN_FAILED,
	POLAR_DATAMAX_SEEK_FAILED,
	POLAR_DATAMAX_READ_FAILED
}			polar_datamax_io_error_cause;

#endif							/* !POLAR_DATAMAX_INTERNAL_H */

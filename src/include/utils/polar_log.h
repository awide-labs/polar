/*-------------------------------------------------------------------------
 *
 * polar_log.h
 *	  Log data structure information.
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
 *	  src/include/utils/polar_log.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_LOG_H
#define POLAR_LOG_H

#include "access/polar_logindex.h"
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "replication/walreceiver.h"
#include "storage/buf_internals.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_flush.h"

#define POLAR_LOG_BUFFER_TAG_FORMAT "'[%u, %u, %u], %u, %u'"
#define POLAR_LOG_BUFFER_TAG(tag__) (tag__)->rnode.spcNode,(tag__)->rnode.dbNode,(tag__)->rnode.relNode,(tag__)->forkNum,(tag__)->blockNum

/* Log information of PageHeader */
#define POLAR_LOG_PAGE_INFO(page) \
	do { \
		PageHeader phdr = (PageHeader) (page);  \
		ereport(LOG, (errmsg("%s page info: lsn=%X/%X, lower=%u, upper=%u, special=%u", \
							 PG_FUNCNAME_MACRO, LSN_FORMAT_ARGS(PageGetLSN(page)), \
							 phdr->pd_lower, phdr->pd_upper, phdr->pd_special), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of BufferTag */
#define POLAR_LOG_BUFFER_TAG_INFO(tag) \
	do { \
		ereport(LOG, (errmsg("%s buffer tag info: page_tag=" POLAR_LOG_BUFFER_TAG_FORMAT, \
							 PG_FUNCNAME_MACRO,  POLAR_LOG_BUFFER_TAG(tag)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of CopyBufferDesc */
#define POLAR_LOG_COPY_BUFFER_DESC(copy_buf) \
	do { \
		POLAR_LOG_BUFFER_TAG_INFO(&((copy_buf)->tag)); \
		ereport(LOG, (errmsg("%s copy buffer desc: buf_id=%d, oldest_lsn=%X/%X, origin_buffer=%u, state=%x, pass_count=%u, is_flushed=%d", \
							 PG_FUNCNAME_MACRO, (copy_buf)->buf_id, LSN_FORMAT_ARGS((copy_buf)->oldest_lsn), (copy_buf)->origin_buffer, \
							 (copy_buf)->state, pg_atomic_read_u32(&(copy_buf)->pass_count), (copy_buf)->is_flushed), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log consistent lsn */
#define POLAR_LOG_CONSISTENT_LSN() \
	do { \
		ereport(LOG, (errmsg("%s consistent lsn=%X/%X, background_replayed_lsn=%X/%X, background_redo_state=%d", \
							 PG_FUNCNAME_MACRO, \
							 LSN_FORMAT_ARGS(polar_is_replica() ? polar_get_primary_consistent_lsn() : polar_get_consistent_lsn()), \
							 LSN_FORMAT_ARGS(polar_bg_redo_get_replayed_lsn(polar_logindex_redo_instance)), \
							 pg_atomic_read_u32(&polar_logindex_redo_instance->bg_redo_state)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of XLogReaderState */
#define POLAR_LOG_XLOG_RECORD_INFO(record) \
	do { \
		if (strlen((record)->errormsg_buf)) \
			ereport(LOG, (errmsg("%s Read xlog and got err='%s'", \
								 PG_FUNCNAME_MACRO, (record)->errormsg_buf), \
						  errhidestmt(true), errhidecontext(true))); \
		ereport(LOG, (errmsg("%s xlog record: "POLAR_XLOG_READER_STATE_FORMAT, \
							 PG_FUNCNAME_MACRO, POLAR_XLOG_READER_STATE_ARGS(record)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of BufferDesc */
#define POLAR_LOG_BUFFER_DESC(buf) \
	do { \
		Page page = BufferGetPage(BufferDescriptorGetBuffer(buf)); \
		ereport(LOG, (errmsg("%s buf=%d " POLAR_LOG_BUFFER_TAG_FORMAT " desc: state=%x, o_lsn=%X/%X, p_flags=%u, m_count=%u, p_lsn=%X/%X", \
							 PG_FUNCNAME_MACRO, (buf)->buf_id, POLAR_LOG_BUFFER_TAG(&((buf)->tag)), pg_atomic_read_u32(&((buf)->state)), \
							 LSN_FORMAT_ARGS((buf)->oldest_lsn), (buf)->polar_flags, (buf)->recently_modified_count, LSN_FORMAT_ARGS(PageGetLSN(page))), \
					  errhidestmt(true), errhidecontext(true))); \
		if ((buf)->copy_buffer) \
			POLAR_LOG_COPY_BUFFER_DESC((buf)->copy_buffer); \
	} while (0)

/* Log information of BufferDesc and FlushList */
#define POLAR_LOG_BUFFER_DESC_WITH_FLUSHLIST(buf) \
	do { \
		ereport(LOG, errmsg("%s buffer tag info: page_tag='[%u, %u, %u], %d, %u', state=%u, oldest_lsn=%X/%X, polar_flags=%u, flush list size: %d, flush_prev: %d, flush_next: %d", \
							PG_FUNCNAME_MACRO, (buf)->tag.rnode.spcNode, (buf)->tag.rnode.dbNode, (buf)->tag.rnode.relNode, \
							(buf)->tag.forkNum, (buf)->tag.blockNum, pg_atomic_read_u32(&((buf)->state)), \
							LSN_FORMAT_ARGS((buf)->oldest_lsn), (buf)->polar_flags, FLUSH_LIST_LEN, (buf)->flush_prev, (buf)->flush_next), \
					 errhidestmt(true), errhidecontext(true)); \
	} while (0)

#define POLAR_LOG_LOGINDEX_PREV_LSN_INFO(lsn, prev_lsn) \
	do { \
		ereport(LOG, (errmsg("%s logindex index lsn info: lsn=%X/%X, prev_lsn=%X/%X", \
							 PG_FUNCNAME_MACRO, LSN_FORMAT_ARGS(lsn), LSN_FORMAT_ARGS(prev_lsn)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of log_index_lsn_t */
#define POLAR_LOG_LOGINDEX_LSN_INFO(index) \
	do { \
		POLAR_LOG_BUFFER_TAG_INFO((index)->tag); \
		POLAR_LOG_LOGINDEX_PREV_LSN_INFO((index)->lsn, (index)->prev_lsn); \
	} while (0)

/* Log redo information of PageHeader and XLogReaderState */
#define POLAR_LOG_REDO_INFO(page, record) \
	do { \
		POLAR_LOG_CONSISTENT_LSN();  \
		polar_log_page_iter_context(); \
		POLAR_LOG_PAGE_INFO(page); \
		POLAR_LOG_XLOG_RECORD_INFO(record); \
	} while (0)

/* Log information of log_index_meta_t */
#define POLAR_LOG_LOGINDEX_META_INFO(meta) \
	do { \
		ereport(LOG, (errmsg("%s logindex meta: magic=%x version=%x max_table_id=%ld seg.no=%ld seg.max_lsn=%X/%X" \
							 " seg.max_id=%ld seg.min_id=%ld start_lsn=%X/%X max_lsn=%X/%X crc=%x\n", \
							 PG_FUNCNAME_MACRO, (meta)->magic, (meta)->version, (meta)->max_idx_table_id, (meta)->min_segment_info.segment_no, \
							 LSN_FORMAT_ARGS((meta)->min_segment_info.max_lsn), (meta)->min_segment_info.max_idx_table_id, \
							 (meta)->min_segment_info.min_idx_table_id, LSN_FORMAT_ARGS((meta)->start_lsn), LSN_FORMAT_ARGS((meta)->max_lsn), (meta)->crc), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of log_idx_table_data_t */
#define POLAR_LOG_LOGINDEX_TABLE_INFO(table) \
	do { \
		ereport(LOG, (errmsg("%s logindex table info: table_id=%lu, min_lsn=%X/%X, max_lsn=%X/%X, prefix_lsn=%X, crc=%u, last_order=%u", \
							 PG_FUNCNAME_MACRO, (table)->idx_table_id, LSN_FORMAT_ARGS((table)->min_lsn), \
							 LSN_FORMAT_ARGS((table)->max_lsn), (table)->prefix_lsn, \
							 (table)->crc, (table)->last_order), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of log_mem_table_t */
#define POLAR_LOG_LOGINDEX_MEM_TABLE_INFO(table) \
	do { \
		ereport(LOG, (errmsg("%s logindex mem table info: free_head=%d, state=%d", \
							 PG_FUNCNAME_MACRO, (table)->free_head, LOG_INDEX_MEM_TBL_STATE(table)), \
					  errhidestmt(true), errhidecontext(true))); \
		POLAR_LOG_LOGINDEX_TABLE_INFO(&((table)->data)); \
	} while (0)

#define POLAR_ASSERT_PANIC(condition)	\
	do { \
		if (unlikely(!(condition))) \
			ereport(PANIC, \
					(errmsg("polar assertion failed. " \
							"condition:%s, file:%s, line:%d", \
							#condition, __FILE__, __LINE__))); \
	} while(0)

#endif

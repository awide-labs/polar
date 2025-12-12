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

#include "access/xlog.h"
#include "access/xlogreader.h"
#include "replication/walreceiver.h"
#include "storage/buf_internals.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_flush.h"

#define POLAR_LOG_BUFFER_TAG_FORMAT "'[%u, %u, %u], %u, %u'"
#define POLAR_LOG_BUFFER_TAG(tag__) (tag__)->spcOid,(tag__)->dbOid,(tag__)->relNumber,(tag__)->forkNum,(tag__)->blockNum

/* Log information of PageHeader */
#define POLAR_LOG_PAGE_INFO(page) \
	do { \
		PageHeader phdr = (PageHeader) (page);  \
		ereport(LOG, (errmsg("%s page info: lsn=%X/%X, lower=%u, upper=%u, special=%u", \
							 __func__, LSN_FORMAT_ARGS(PageGetLSN(page)), \
							 phdr->pd_lower, phdr->pd_upper, phdr->pd_special), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of BufferTag */
#define POLAR_LOG_BUFFER_TAG_INFO(tag) \
	do { \
		ereport(LOG, (errmsg("%s buffer tag info: page_tag=" POLAR_LOG_BUFFER_TAG_FORMAT, \
							 __func__, POLAR_LOG_BUFFER_TAG(tag)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of CopyBufferDesc */
#define POLAR_LOG_COPY_BUFFER_DESC(copy_buf) \
	do { \
		POLAR_LOG_BUFFER_TAG_INFO(&((copy_buf)->tag)); \
		ereport(LOG, (errmsg("%s copy buffer desc: buf_id=%d, oldest_lsn=%X/%X, origin_buffer=%u, state=%x, pass_count=%u, is_flushed=%d", \
							 __func__, (copy_buf)->buf_id, LSN_FORMAT_ARGS((copy_buf)->oldest_lsn), (copy_buf)->origin_buffer, \
							 (copy_buf)->state, pg_atomic_read_u32(&(copy_buf)->pass_count), (copy_buf)->is_flushed), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log consistent lsn */
#define POLAR_LOG_CONSISTENT_LSN() \
	do { \
		ereport(LOG, (errmsg("%s consistent lsn=%X/%X, background_replayed_lsn=%X/%X, background_redo_state=%d", \
							 __func__, \
							 LSN_FORMAT_ARGS(polar_is_replica() ? polar_get_primary_consistent_lsn() : polar_get_consistent_lsn()), \
							 LSN_FORMAT_ARGS(polar_get_bg_replayed_lsn(polar_logindex_redo_instance)), \
							 pg_atomic_read_u32(&polar_logindex_redo_instance->bg_redo_state)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of XLogReaderState */
#define POLAR_LOG_XLOG_RECORD_INFO(record) \
	do { \
		if (strlen((record)->errormsg_buf)) \
			ereport(LOG, (errmsg("%s Read xlog and got err='%s'", \
								 __func__, (record)->errormsg_buf), \
						  errhidestmt(true), errhidecontext(true))); \
		ereport(LOG, (errmsg("%s xlog record: "POLAR_XLOG_READER_STATE_FORMAT, \
							 __func__, POLAR_XLOG_READER_STATE_ARGS(record)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of BufferDesc */
#define POLAR_LOG_BUFFER_DESC(buf) \
	do { \
		Page _page = BufferGetPage(BufferDescriptorGetBuffer(buf)); \
		ereport(LOG, (errmsg("%s buf=%d " POLAR_LOG_BUFFER_TAG_FORMAT " desc: state=%x, o_lsn=%X/%X, p_flags=%u, m_count=%u, p_lsn=%X/%X", \
							 __func__, (buf)->buf_id, POLAR_LOG_BUFFER_TAG(&((buf)->tag)), pg_atomic_read_u32(&((buf)->state)), \
							 LSN_FORMAT_ARGS((buf)->oldest_lsn), (buf)->polar_flags, (buf)->recently_modified_count, LSN_FORMAT_ARGS(PageGetLSN(_page))), \
					  errhidestmt(true), errhidecontext(true))); \
		if ((buf)->copy_buffer) \
			POLAR_LOG_COPY_BUFFER_DESC((buf)->copy_buffer); \
	} while (0)

/* Log information of BufferDesc and FlushList */
#define POLAR_LOG_BUFFER_DESC_WITH_FLUSHLIST(buf, ctl) \
	do { \
		ereport(LOG, errmsg("%s buffer tag info: page_tag='[%u, %u, %u], %d, %u', state=%u, oldest_lsn=%X/%X, polar_flags=%u, flush list size: %d, flush_prev: %d, flush_next: %d", \
							__func__, (buf)->tag.spcOid, (buf)->tag.dbOid, (buf)->tag.relNumber, \
							(buf)->tag.forkNum, (buf)->tag.blockNum, pg_atomic_read_u32(&((buf)->state)), \
							LSN_FORMAT_ARGS((buf)->oldest_lsn), (buf)->polar_flags, FLUSH_LIST_LEN(ctl), (buf)->flush_prev, (buf)->flush_next), \
					 errhidestmt(true), errhidecontext(true)); \
	} while (0)

#define POLAR_LOG_LOGINDEX_PREV_LSN_INFO(lsn, prev_lsn) \
	do { \
		ereport(LOG, (errmsg("%s logindex index lsn info: lsn=%X/%X, prev_lsn=%X/%X", \
							 __func__, LSN_FORMAT_ARGS(lsn), LSN_FORMAT_ARGS(prev_lsn)), \
					  errhidestmt(true), errhidecontext(true))); \
	} while (0)

/* Log information of log_index_lsn_info */
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

#endif

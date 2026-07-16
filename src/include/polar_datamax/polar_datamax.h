/*-------------------------------------------------------------------------
 *
 * polar_datamax.h
 *
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
 *   src/include/polar_datamax/polar_datamax.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_DATAMAX_H
#define POLAR_DATAMAX_H

#include "port/atomics.h"
#include "postmaster/bgworker_internals.h"
#include "polar_datamax/polar_datamax_internal.h"
#include "replication/slot.h"
#include "storage/lwlock.h"

#define POLAR_DATAMAX_SET_PRIMARY_NEXTXID(next_xid) (pg_atomic_write_u32(&polar_datamax_ctl->polar_primary_next_xid, next_xid))
#define POLAR_DATAMAX_SET_PRIMARY_NEXTEPOCH(epoch) (pg_atomic_write_u32(&polar_datamax_ctl->polar_primary_epoch, epoch))

typedef struct polar_datamax_ctl_t
{
	LWLock		meta_lock;
	polar_datamax_meta_data_t meta;

	slock_t		lock;
	polar_datamax_clean_task_t clean_task;

	/* POLAR: record nextxid and epoch of primary */
	pg_atomic_uint32 polar_primary_next_xid;
	pg_atomic_uint32 polar_primary_epoch;
} polar_datamax_ctl_t;

extern bool polar_datamax_mode_requested;
extern bool polar_is_datamax_mode;
extern polar_datamax_ctl_t *polar_datamax_ctl;
extern volatile sig_atomic_t polar_datamax_shutdown_requested;

extern Size polar_datamax_shmem_size(void);
extern void polar_datamax_shmem_init(void);

extern uint64 polar_datamax_get_sys_identifier(void);

/* meta operations */
extern bool polar_datamax_is_initial(polar_datamax_ctl_t *ctl);
extern void polar_datamax_init_meta(polar_datamax_meta_data_t *meta, bool create);
extern bool polar_datamax_meta_file_exist(void);
extern void polar_datamax_load_meta(polar_datamax_ctl_t *ctl);
extern void polar_datamax_write_meta(polar_datamax_ctl_t *ctl, bool update);
extern void polar_datamax_init_received_info(polar_datamax_ctl_t *ctl, TimeLineID tli, XLogRecPtr lsn);
extern void polar_datamax_update_received_info(polar_datamax_ctl_t *ctl, TimeLineID tli, XLogRecPtr lsn);
extern void polar_datamax_update_min_received_info(polar_datamax_ctl_t *ctl, TimeLineID tli, XLogRecPtr lsn);
extern void polar_datamax_get_last_received_info(polar_datamax_ctl_t *ctl, TimeLineID *tli, XLogRecPtr *lsn);
extern XLogRecPtr polar_datamax_get_min_received_lsn(polar_datamax_ctl_t *ctl, TimeLineID *tli);
extern XLogRecPtr polar_datamax_get_last_received_lsn(polar_datamax_ctl_t *ctl, TimeLineID *tli);
extern void polar_datamax_update_last_valid_received_lsn(polar_datamax_ctl_t *ctl, XLogRecPtr lsn);
extern XLogRecPtr polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl_t *ctl, TimeLineID *tli);
extern void polar_datamax_update_upstream_last_removed_segno(polar_datamax_ctl_t *ctl, XLogSegNo segno);
extern XLogSegNo polar_datamax_get_upstream_last_removed_segno(polar_datamax_ctl_t *ctl);

/* WAL */
extern void polar_datamax_handle_timeline_switch(polar_datamax_ctl_t *ctl);
extern void polar_datamax_remove_old_wal(XLogRecPtr lsn, bool force);
extern int	polar_datamax_remove_wal_file(char *seg_name);
extern void polar_datamax_handle_clean_task(polar_datamax_ctl_t *ctl);
extern bool polar_datamax_set_clean_task(XLogRecPtr reserved_lsn, bool force);
extern void polar_datamax_reset_clean_task(polar_datamax_ctl_t *ctl);

/* replication */
extern void polar_datamax_save_replication_slots(void);
extern XLogRecPtr polar_datamax_replication_start_lsn(ReplicationSlot *slot);
extern XLogRecPtr polar_get_smallest_walfile_lsn(void);

/* utility */
extern void polar_datamax_validate_dir(void);
extern void polar_datamax_check_mkdir(const char *path, int emode);

/* file path */
extern void polar_datamax_wal_file_path(char *path, TimeLineID tli, XLogSegNo logSegNo, int wal_segsz_bytes);
extern void polar_datamax_status_file_path(char *path, const char *xlog, char *suffix);
extern void polar_datamax_tl_history_file_path(char *path, TimeLineID tli);

/* archive func */
extern void polar_datamax_archive(void);
extern void polar_datamax_remove_archivedone_wal(polar_datamax_ctl_t *ctl);
extern void polar_datamax_handle_remove_archivedone_wal(polar_datamax_ctl_t *ctl);

/* prealloc wal file */
extern void polar_datamax_prealloc_wal_file(polar_datamax_ctl_t *ctl);
extern void polar_datamax_handle_prealloc_walfile(polar_datamax_ctl_t *ctl);

/* valid lsn list operation */
extern polar_datamax_valid_lsn_list *polar_datamax_create_valid_lsn_list(void);
extern void polar_datamax_insert_last_valid_lsn(polar_datamax_valid_lsn_list *list, XLogRecPtr primary_last_valid_lsn);
extern void polar_datamax_update_cur_valid_lsn(polar_datamax_valid_lsn_list *list, XLogRecPtr flush_lsn);
extern void polar_datamax_free_valid_lsn_list(int code, Datum arg);

/* parse xlog */
extern void polar_datamax_parse_xlog(polar_datamax_ctl_t *ctl);
extern void polar_datamax_report_io_error(const char *path, int log_level);

/* func for test */
extern char *polar_datamax_get_primary_info(void);
extern char *polar_datamax_get_primary_slot_name(void);
extern uint64 polar_datamax_get_datamax_sys_identifier(void);

/* inline func */
/*
 * POLAR: Calculate crc for datamax meta, here we use unsigned char as data type
 * is to be consistent with inner calc function pg_comp_crc32c_sb8.
 */
static inline pg_crc32c
polar_datamax_calc_meta_crc(unsigned char *data, size_t len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, data, len);
	FIN_CRC32C(crc);

	return crc;
}

/* main entry */
extern void polar_datamax_main(void);

#endif							/* !POLAR_DATAMAX_H */

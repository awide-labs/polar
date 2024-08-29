/*-------------------------------------------------------------------------
 *
 * polar_io_stat.c
 *	  io stat for polar vfs
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
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
 *	  external/polar_io_stat/polar_io_stat.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/sinvaladt.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"

/* POLAR */
#include "polar_vfs/polar_vfs_interface.h"
#include "storage/polar_io_stat.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

static void polar_io_stat_shmem_shutdown(int code, Datum arg);
static void POLARProcIOStatAdd(PolarProcIOStat x[][POLARIO_LOC_SIZE], PolarProcIOStat y[][POLARIO_LOC_SIZE]);
static void NumLatencyDistAdd(uint64 x[][LATENCY_KIND_LEN][LATENCY_INTERVAL_LEN], uint64 y[][LATENCY_KIND_LEN][LATENCY_INTERVAL_LEN]);
static void print_current_process_io_info(int io_type, const char *pro_name, const char *polar_dir_type_name);
static void polario_location_register(int kind, int loc);
static void polar_io_shmem_exit_cleanup(void);

/* Links to shared memory state */
int			polar_kind_to_location[POLAR_VFS_KIND_SIZE];
static polar_postmaster_child_init_register prev_polar_stat_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static int	save_errno;

/* POLAR : IO statistics collection functions */
static bool polar_enable_track_io_timing;
static instr_time tmp_io_start;
static instr_time tmp_io_time;
static instr_time vfs_start;
static instr_time vfs_end;

static inline int vfs_data_type(const char *path);
static inline void vfs_timer_end(instr_time *time);
static void polar_stat_io_open_info(int vfdkind, int vfdtype);
static void polar_stat_io_close_info(int vfdkind, int vfdtype);
static void polar_stat_io_read_info(int vfdkind, int vfdtype, ssize_t size);
static void polar_stat_io_write_info(int vfdkind, int vfdtype, ssize_t size);
static void polar_stat_io_seek_info(int vfdkind, int vfdtype);
static void polar_stat_io_creat_info(int vfdkind, int vfdtype);
static void polar_stat_io_falloc_info(int vfdkind, int vfdtype);
static void polar_stat_io_fsync_info(int vfdkind, int vfdtype);
static inline void polar_set_distribution_interval(instr_time *intervaltime, int loc, int kind);
static inline void polar_vfs_timer_begin_iostat(void);
static inline void polar_vfs_timer_end_iostat(instr_time *time, int loc, int kind);
static void pgis_shmem_request(void);

/* POLAR end */

/* POLAR: polar_io_stat hooks for polar_vfs. */
static void polar_stat_env_init_hook(polar_vfs_ops ops);
static void polar_stat_env_destroy_hook(polar_vfs_ops ops);
static void polar_stat_file_before_hook(const char *path, vfs_vfd *vfdp, polar_vfs_ops ops);
static void polar_stat_file_after_hook(const char *path, vfs_vfd *vfdp, polar_vfs_ops ops);
static void polar_stat_io_before_hook(vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops);
static void polar_stat_io_after_hook(vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops);
static void polar_stat_register_vfs_hooks(void);


static polar_vfs_env_hook_type polar_vfs_env_init_hook_prev = NULL;
static polar_vfs_env_hook_type polar_vfs_env_destroy_hook_prev = NULL;
static polar_vfs_file_hook_type polar_vfs_file_before_hook_prev = NULL;
static polar_vfs_file_hook_type polar_vfs_file_after_hook_prev = NULL;
static polar_vfs_io_hook_type polar_vfs_io_before_hook_prev = NULL;
static polar_vfs_io_hook_type polar_vfs_io_after_hook_prev = NULL;
/* POLAR end */

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	EmitWarningsOnPlaceholders("polar_io_stat.enable_track_io_timing");

	DefineCustomBoolVariable("polar_io_stat.enable_track_io_timing",
							 "enable io timing",
							 NULL,
							 &polar_enable_track_io_timing,
							 true,
							 PGC_BACKEND,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * Install hooks.
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgis_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = polar_io_stat_shmem_startup;
	polar_register_io_shmem_exit_cleanup();
	polar_stat_register_vfs_hooks();
}

/*
 * shmem_request hook: request additional shared resources.  We'll allocate or
 * attach to the shared resources in pgss_shmem_startup().
 */
static void
pgis_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(mul_size(sizeof(POLAR_PROC_IO), PolarNumProcIOStatSlots));
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 */
void
polar_io_stat_shmem_startup(void)
{
	bool		found = false;
	Size		total_size = 0;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* The 0th index is used as a summary statistics */
	total_size = mul_size(sizeof(POLAR_PROC_IO), PolarNumProcIOStatSlots);

	/*
	 * Create or attach to the shared memory state
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	PolarIOStatArray = ShmemInitStruct("PolarIOStatArray",
									   total_size, &found);

	elog(LOG, "PolarIOStatArray share memory total size is %d", (int) total_size);

	if (!found)
	{
		int			i;

		/* First time through ... */
		memset(PolarIOStatArray, 0, total_size);
		/* POLAR: polar io stat register all kinds of vfs interfaces. */
		for (i = 0; i < POLAR_VFS_KIND_SIZE; i++)
			polar_kind_to_location[i] = -1;
		polario_location_register(POLAR_VFS_LOCAL_BIO, POLARIO_LOCAL);
		polario_location_register(POLAR_VFS_PFS, POLARIO_SHARED);
		polario_location_register(POLAR_VFS_LOCAL_DIO, POLARIO_SHARED);
	}

	LWLockRelease(AddinShmemInitLock);
}

void
polar_register_io_shmem_exit_cleanup(void)
{
	/* register a func to total io statistics before shmem exit */
	prev_polar_stat_hook = polar_stat_hook;
	polar_stat_hook = polar_io_shmem_exit_cleanup;
}

static void
polar_io_shmem_exit_cleanup(void)
{
	if (prev_polar_stat_hook)
		prev_polar_stat_hook();
	on_shmem_exit(polar_io_stat_shmem_shutdown, (Datum) 0);
}

static void
polar_io_stat_shmem_shutdown(int code, Datum arg)
{
	int			my_proc_index;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!PolarIOStatArray)
		return;

	if (MyBackendId == 0)
	{
		elog(LOG, "Excepted, backendid(0) exit.");
	}

	my_proc_index = polar_get_io_proc_index();

	if (AmStartupProcess())
	{
		print_current_process_io_info(POLARIO_TYPE_SIZE, "startup process", "Total");
		print_current_process_io_info(POLARIO_WAL, "startup process", "WAL");
		print_current_process_io_info(POLARIO_DATA, "startup process", "DATA");
	}

	if (my_proc_index > 0)
	{
		/*
		 * We think that 0 of backendid is not used by the process or by the
		 * master, so we will summarize the summary information of IO here so
		 * that we can display it in the view. NB: This process does not
		 * require locked because we don't need to care about data
		 * consistency.
		 */
		POLARProcIOStatAdd(PolarIOStatArray[0].polar_proc_io_stat_dist, PolarIOStatArray[my_proc_index].polar_proc_io_stat_dist);
		NumLatencyDistAdd(PolarIOStatArray[0].num_latency_dist, PolarIOStatArray[my_proc_index].num_latency_dist);
		MemSet(&PolarIOStatArray[my_proc_index], 0, sizeof(POLAR_PROC_IO));
	}
}

/* Add two PLOARPROCIOSTATs*/
static void
POLARProcIOStatAdd(PolarProcIOStat x[][POLARIO_LOC_SIZE], PolarProcIOStat y[][POLARIO_LOC_SIZE])
{
	int			i;
	int			j;

	for (i = 0; i < POLARIO_TYPE_SIZE; i++)
		for (j = 0; j < POLARIO_LOC_SIZE; j++)
		{
			x[i][j].io_number_read += y[i][j].io_number_read;
			x[i][j].io_number_write += y[i][j].io_number_write;
			x[i][j].io_throughtput_read += y[i][j].io_throughtput_read;
			x[i][j].io_throughtput_write += y[i][j].io_throughtput_write;
			INSTR_TIME_ADD(x[i][j].io_latency_read, y[i][j].io_latency_read);
			INSTR_TIME_ADD(x[i][j].io_latency_write, y[i][j].io_latency_write);
			x[i][j].io_open_num += y[i][j].io_open_num;
			x[i][j].io_close_num += y[i][j].io_close_num;
			INSTR_TIME_ADD(x[i][j].io_open_time, y[i][j].io_open_time);
			INSTR_TIME_ADD(x[i][j].io_seek_time, y[i][j].io_seek_time);
			x[i][j].io_seek_count += y[i][j].io_seek_count;
			INSTR_TIME_ADD(x[i][j].io_creat_time, y[i][j].io_creat_time);
			INSTR_TIME_ADD(x[i][j].io_falloc_time, y[i][j].io_falloc_time);
			INSTR_TIME_ADD(x[i][j].io_fsync_time, y[i][j].io_fsync_time);
			x[i][j].io_creat_count += y[i][j].io_creat_count;
			x[i][j].io_fsync_count += y[i][j].io_fsync_count;
			x[i][j].io_falloc_count += y[i][j].io_falloc_count;
		}
}

/* Add two PLOARPROCIOSTATs*/
static void
NumLatencyDistAdd(uint64 x[][LATENCY_KIND_LEN][LATENCY_INTERVAL_LEN], uint64 y[][LATENCY_KIND_LEN][LATENCY_INTERVAL_LEN])
{
	int			i;
	int			j;
	int			k;

	for (i = 0; i < POLARIO_LOC_SIZE; i++)
		for (j = 0; j < LATENCY_KIND_LEN; j++)
			for (k = 0; k < LATENCY_INTERVAL_LEN; k++)
			{
				x[i][j][k] += y[i][j][k];
			}
}

/* Calculate the I/O statistical index of the current process */
int
polar_get_io_proc_index(void)
{
	return MyBackendId == InvalidBackendId ?
		(MyAuxProcType == NotAnAuxProcess ? -1 : MaxBackends + MyAuxProcType + 1)
		: MyBackendId;
}

static void
print_current_process_io_info(int io_type, const char *pro_name, const char *polar_dir_type_name)
{
	int			backendid = polar_get_io_proc_index();
	bool		once = true;
	int			index = io_type;
	instr_time	vfs_open_time,
				vfs_write_time,
				vfs_read_time,
				vfs_seek_time,
				vfs_fsync_time;
	uint64		vfs_open_count,
				vfs_write_count,
				vfs_read_count,
				vfs_seek_count,
				vfs_fsync_count;
	PolarProcIOStat *tmp_stat;

	if (backendid == -1)
		return;

	if (io_type > POLARIO_TYPE_SIZE || io_type < 0)
		return;

	/* initial and clean up */
	vfs_open_count = 0;
	vfs_write_count = 0;
	vfs_read_count = 0;
	vfs_seek_count = 0;
	vfs_fsync_count = 0;
	INSTR_TIME_SET_ZERO(vfs_open_time);
	INSTR_TIME_SET_ZERO(vfs_write_time);
	INSTR_TIME_SET_ZERO(vfs_read_time);
	INSTR_TIME_SET_ZERO(vfs_seek_time);
	INSTR_TIME_SET_ZERO(vfs_fsync_time);

	if (io_type == POLARIO_TYPE_SIZE)
	{
		once = false;
		index = 0;
	}

	do
	{
		int			i;

		for (i = 0; i < POLARIO_LOC_SIZE; i++)
		{
			tmp_stat = &PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][i];
			INSTR_TIME_ADD(vfs_open_time, tmp_stat->io_open_time);
			INSTR_TIME_ADD(vfs_write_time, tmp_stat->io_latency_write);
			INSTR_TIME_ADD(vfs_read_time, tmp_stat->io_latency_read);
			INSTR_TIME_ADD(vfs_seek_time, tmp_stat->io_seek_time);
			INSTR_TIME_ADD(vfs_fsync_time, tmp_stat->io_fsync_time);
			vfs_open_count += tmp_stat->io_open_num;
			vfs_read_count += tmp_stat->io_number_read;
			vfs_write_count += tmp_stat->io_number_write;
			vfs_seek_count += tmp_stat->io_seek_count;
			vfs_fsync_count += tmp_stat->io_fsync_count;
		}
	} while (!once && (++index) < POLARIO_TYPE_SIZE);

	elog(LOG, "%s %s IO follows:"
		 "open %.6lf s, count %lu, "
		 "write %.6lf s, count %lu, "
		 "read %.6lf s, count %lu, "
		 "lseek %.6lf s, count %lu,"
		 "fsync %.6lf s, count %lu",
		 pro_name, polar_dir_type_name,
		 INSTR_TIME_GET_DOUBLE(vfs_open_time), vfs_open_count,
		 INSTR_TIME_GET_DOUBLE(vfs_write_time), vfs_write_count,
		 INSTR_TIME_GET_DOUBLE(vfs_read_time), vfs_read_count,
		 INSTR_TIME_GET_DOUBLE(vfs_seek_time), vfs_seek_count,
		 INSTR_TIME_GET_DOUBLE(vfs_fsync_time), vfs_fsync_count);
}

static void
polario_location_register(int kind, int loc)
{
	if ((kind >= 0 && kind < POLAR_VFS_KIND_SIZE) &&
		(loc >= 0 && loc < POLARIO_LOC_SIZE))
		polar_kind_to_location[kind] = loc;
	else
		elog(ERROR, "polario_location_register error at "
			 "kind = %d, loc = %d, "
			 "POLAR_VFS_KIND_SIZE = %d, "
			 "POLARIO_LOC_SIZE = %d",
			 kind, loc, POLAR_VFS_KIND_SIZE, POLARIO_LOC_SIZE);
}

int
polario_kind_to_location(int kind)
{
	int			loc = -1;

	if (kind >= 0 && kind < POLAR_VFS_KIND_SIZE)
		loc = polar_kind_to_location[kind];
	return loc;
}


/*
 * collect the statistics of backends.
 *
 * We need to check the result, when call this funcition.
 * The value of result, -1 means fail, 0 means succeed.
 */
int
polar_collect_io_stat(PolarCollectIoStat *io_stat_info, int backendid)
{
	int			index = 0;

	uint64		shared_read_ps = 0;
	uint64		shared_write_ps = 0;
	uint64		shared_read_throughput = 0;
	uint64		shared_write_throughput = 0;
	instr_time	shared_read_latency;
	instr_time	shared_write_latency;
	int64		io_open_num = 0;
	int64		io_seek_count = 0;
	instr_time	io_open_time;
	instr_time	io_seek_time;

	/* check backendid */
	if (backendid < 0 || backendid >= PolarNumProcIOStatSlots)
	{
		return -1;
	}

	INSTR_TIME_SET_ZERO(shared_read_latency);
	INSTR_TIME_SET_ZERO(shared_write_latency);
	INSTR_TIME_SET_ZERO(io_open_time);
	INSTR_TIME_SET_ZERO(io_seek_time);

	/* Each process accumulates it‘s file type by file location */
	if (PolarIOStatArray)
	{
		for (index = 0; index < POLARIO_TYPE_SIZE; index++)
		{

			shared_read_ps += PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_number_read;
			shared_write_ps += PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_number_write;
			shared_read_throughput += PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_throughtput_read;
			shared_write_throughput += PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_throughtput_write;
			INSTR_TIME_ADD(shared_read_latency, PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_latency_read);
			INSTR_TIME_ADD(shared_write_latency, PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_latency_write);

			io_open_num += PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_open_num;
			io_seek_count += PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_seek_count;

			INSTR_TIME_ADD(io_open_time, PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_open_time);
			INSTR_TIME_ADD(io_seek_time, PolarIOStatArray[backendid].polar_proc_io_stat_dist[index][POLARIO_SHARED].io_seek_time);

		}

		/* pfs iops */
		io_stat_info->shared_read_ps = shared_read_ps;
		io_stat_info->shared_write_ps = shared_write_ps;

		/* pfs io throughput */
		io_stat_info->shared_read_throughput = shared_read_throughput;
		io_stat_info->shared_write_throughput = shared_write_throughput;

		/* pfs io latency */
		io_stat_info->shared_read_latency = INSTR_TIME_GET_MILLISEC(shared_read_latency);
		io_stat_info->shared_write_latency = INSTR_TIME_GET_MILLISEC(shared_write_latency);

		/* open and seek count */
		io_stat_info->io_open_num = io_open_num;
		io_stat_info->io_seek_count = io_seek_count;

		/* open and seek time */
		io_stat_info->io_open_time = INSTR_TIME_GET_MILLISEC(io_open_time);
		io_stat_info->io_seek_time = INSTR_TIME_GET_MILLISEC(io_seek_time);

	}

	return 0;
}

/* Determine if the datatype is data ,xlog , clog ... and so on*/
static inline int
vfs_data_type(const char *path)
{
	if (strstr(path, "base"))
		return POLARIO_DATA;
	else if (strstr(path, "pg_wal"))
		return POLARIO_WAL;
	else if (strstr(path, "pg_xact"))
		return POLARIO_CLOG;
	else if (strstr(path, "global"))
		return POLARIO_GLOBAL;
	else if (strstr(path, "logindex"))
		return POLARIO_LOGINDEX;
	else if (strstr(path, "multixact"))
		return POLARIO_MULTIXACT;
	else if (strstr(path, "subtrans"))
		return POLARIO_SUBTRANS;
	else if (strstr(path, "twophase"))
		return POLARIO_TWOPHASE;
	else if (strstr(path, "replslot"))
		return POLARIO_REPLSOT;
	else if (strstr(path, "snapshots"))
		return POLARIO_SNAPSHOTS;
	else
		return POLARIO_OTHER;
}

static inline void
polar_vfs_timer_begin_iostat(void)
{
	if (polar_enable_track_io_timing)
		INSTR_TIME_SET_CURRENT(tmp_io_start);
	else
		INSTR_TIME_SET_ZERO(tmp_io_start);
}

static inline void
vfs_timer_end(instr_time *time)
{
	if (!polar_enable_track_io_timing)
		return;

	INSTR_TIME_SET_CURRENT(tmp_io_time);
	INSTR_TIME_SUBTRACT(tmp_io_time, vfs_start);

	if (time)
		INSTR_TIME_ADD(*time, tmp_io_time);
}

static inline void
polar_vfs_timer_end_iostat(instr_time *time, int loc, int kind)
{
	if (!polar_enable_track_io_timing)
		return;

	INSTR_TIME_SET_CURRENT(tmp_io_time);
	INSTR_TIME_SUBTRACT(tmp_io_time, tmp_io_start);

	if (INSTR_TIME_GET_DOUBLE(tmp_io_time) > 1000)
	{
		elog(WARNING, "This io time took %lf seconds, which is abnormal and we will not count."
			 ,INSTR_TIME_GET_DOUBLE(tmp_io_time));
		return;
	}

	polar_set_distribution_interval(&tmp_io_time, loc, kind);
	if (time)
		INSTR_TIME_ADD(*time, tmp_io_time);
}

/* As easy as understanding its function name */
static void
polar_stat_io_open_info(int vfdkind, int vfdtype)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
		{
			elog(WARNING, "polar io stat does not recognize that: kind = %d, loc = %d", vfdkind, loc);
			return;
		}
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_open_time, loc, LATENCY_open);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_open_num++;
		PolarIOStatArray[index].pid = MyProcPid;
	}
}

/* As easy as understanding its function name */
static void
polar_stat_io_close_info(int vfdkind, int vfdtype)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_close_num++;
	}
}

/* As easy as understanding its function name */
static void
polar_stat_io_read_info(int vfdkind, int vfdtype, ssize_t size)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_latency_read, loc, LATENCY_read);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_number_read++;
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_throughtput_read += size;
	}
}

/* As easy as understanding its function name */
static void
polar_stat_io_write_info(int vfdkind, int vfdtype, ssize_t size)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_latency_write, loc, LATENCY_write);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_number_write++;
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_throughtput_write += size;
	}
}

static void
polar_stat_io_seek_info(int vfdkind, int vfdtype)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_seek_time, loc, LATENCY_seek);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_seek_count++;
	}
}

static void
polar_stat_io_creat_info(int vfdkind, int vfdtype)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_creat_time, loc, LATENCY_creat);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_creat_count++;
	}
}

static void
polar_stat_io_falloc_info(int vfdkind, int vfdtype)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_falloc_time, loc, LATENCY_falloc);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_falloc_count++;
	}
}

static void
polar_stat_io_fsync_info(int vfdkind, int vfdtype)
{
	if (PolarIOStatArray != NULL &&
		UsedShmemSegAddr != NULL)
	{
		int			index,
					loc;

		index = polar_get_io_proc_index();
		if (index < 0)
			return;
		loc = polario_kind_to_location(vfdkind);
		if (loc < 0 || loc >= POLARIO_LOC_SIZE)
			return;
		polar_vfs_timer_end_iostat(&PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_fsync_time, loc, LATENCY_fsync);
		PolarIOStatArray[index].polar_proc_io_stat_dist[vfdtype][loc].io_fsync_count++;
	}
}

static inline void
polar_set_distribution_interval(instr_time *intervaltime, int loc, int kind)
{
	int			index = polar_get_io_proc_index();
	int			interval;
	uint64		valus = 0;

	if (UsedShmemSegAddr == NULL)
		return;
	if (index < 0)
		return;

	if (kind >= LATENCY_KIND_LEN)
		return;

	valus = INSTR_TIME_GET_MICROSEC(*intervaltime);

	if (valus < 1000)
		interval = valus / 200;
	else if (valus < 10000)
		interval = LATENCY_10ms;
	else if (valus < 100000)
		interval = LATENCY_100ms;
	else
		interval = LATENCY_OUT;

	if (PolarIOStatArray)
		PolarIOStatArray[index].num_latency_dist[loc][kind][interval]++;
}

static void
polar_stat_env_init_hook(polar_vfs_ops ops)
{
	int			index;

	if (polar_vfs_env_init_hook_prev)
		polar_vfs_env_init_hook_prev(ops);

	index = polar_get_io_proc_index();

	memset(&vfs_start, 0, sizeof(vfs_start));
	INSTR_TIME_SET_CURRENT(vfs_start);
	if (PolarIOStatArray && index > 0 && UsedShmemSegAddr != NULL)
		PolarIOStatArray[index].pid = MyProcPid;
}

static void
polar_stat_env_destroy_hook(polar_vfs_ops ops)
{
	if (polar_vfs_env_destroy_hook_prev)
		polar_vfs_env_destroy_hook_prev(ops);
	vfs_timer_end(&vfs_end);
}

static void
polar_stat_file_before_hook(const char *path, vfs_vfd *vfdp, polar_vfs_ops ops)
{
	if (polar_vfs_file_before_hook_prev)
		polar_vfs_file_before_hook_prev(path, vfdp, ops);
	switch (ops)
	{
		case VFS_CREAT:
		case VFS_OPEN:
			vfdp->type = vfs_data_type(path);
			/* begin stat info for io, wait_time, wait_object */
			polar_vfs_timer_begin_iostat();
			polar_stat_wait_obj_and_time_set(vfdp->fd, &tmp_io_start, PGPROC_WAIT_FD);
			/* end */
			if (ops == VFS_OPEN)
				pgstat_report_wait_start(WAIT_EVENT_DATA_VFS_FILE_OPEN);
			break;
		default:
			break;
	}
}

static void
polar_stat_file_after_hook(const char *path, vfs_vfd *vfdp, polar_vfs_ops ops)
{
	if (polar_vfs_file_after_hook_prev)
		polar_vfs_file_after_hook_prev(path, vfdp, ops);

	save_errno = errno;
	switch (ops)
	{
		case VFS_CREAT:
			/* end stat info for io, wait_time, wait_object */
			polar_stat_io_creat_info(vfdp->kind, vfdp->type);
			polar_stat_wait_obj_and_time_clear();
			/* end */
			break;
		case VFS_OPEN:
			/* end stat info for io, wait_time, wait_object */
			polar_stat_io_open_info(vfdp->kind, vfdp->type);
			polar_stat_wait_obj_and_time_clear();
			/* end */
			pgstat_report_wait_end();
			break;
		case VFS_CLOSE:
			polar_stat_io_close_info(vfdp->kind, vfdp->type);
			break;
		default:
			break;
	}
	errno = save_errno;
}

static void
polar_stat_io_before_hook(vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops)
{
	if (polar_vfs_io_before_hook_prev)
		polar_vfs_io_before_hook_prev(vfdp, ret, ops);

	switch (ops)
	{
		case VFS_WRITE:
		case VFS_PWRITE:
			/* begin stat info for io, wait_time, wait_object */
			polar_vfs_timer_begin_iostat();
			polar_stat_wait_obj_and_time_set(vfdp->fd, &tmp_io_start, PGPROC_WAIT_FD);
			break;
		case VFS_READ:
		case VFS_PREAD:
			/* begin stat info for io, wait_time, wait_object */
			polar_vfs_timer_begin_iostat();
			polar_stat_wait_obj_and_time_set(vfdp->fd, &tmp_io_start, PGPROC_WAIT_FD);
			break;
		case VFS_LSEEK:
			/* begin stat info for io, wait_time, wait_object */
			polar_vfs_timer_begin_iostat();
			polar_stat_wait_obj_and_time_set(vfdp->fd, &tmp_io_start, PGPROC_WAIT_FD);
			pgstat_report_wait_start(WAIT_EVENT_DATA_VFS_FILE_LSEEK);
			break;
		case VFS_FSYNC:
			/* begin stat info for io, wait_time, wait_object */
			polar_vfs_timer_begin_iostat();
			polar_stat_wait_obj_and_time_set(vfdp->fd, &tmp_io_start, PGPROC_WAIT_FD);
			break;
		case VFS_FALLOCATE:
			/* begin stat info for io, wait_time, wait_object */
			polar_vfs_timer_begin_iostat();
			break;
		default:
			break;
	}
}

static void
polar_stat_io_after_hook(vfs_vfd *vfdp, ssize_t ret, polar_vfs_ops ops)
{
	if (polar_vfs_io_after_hook_prev)
		polar_vfs_io_after_hook_prev(vfdp, ret, ops);

	save_errno = errno;
	switch (ops)
	{
		case VFS_WRITE:
		case VFS_PWRITE:
			/* end stat info for io, wait_time, wait_object */
			polar_stat_io_write_info(vfdp->kind, vfdp->type, ret);
			polar_stat_wait_obj_and_time_clear();
			break;
		case VFS_READ:
		case VFS_PREAD:
			polar_stat_io_read_info(vfdp->kind, vfdp->type, ret);
			polar_stat_wait_obj_and_time_clear();
			break;
		case VFS_LSEEK:
			pgstat_report_wait_end();
			/* end stat info for io, wait_time, wait_object */
			polar_stat_io_seek_info(vfdp->kind, vfdp->type);
			polar_stat_wait_obj_and_time_clear();
			break;
		case VFS_FSYNC:
			/* end stat info for io, wait_time, wait_object */
			polar_stat_io_fsync_info(vfdp->kind, vfdp->type);
			polar_stat_wait_obj_and_time_clear();
			break;
		case VFS_FALLOCATE:
			polar_stat_io_falloc_info(vfdp->kind, vfdp->type);
			break;
		default:
			break;
	}
	errno = save_errno;
}

static void
polar_stat_register_vfs_hooks(void)
{
	polar_vfs_env_init_hook_prev = polar_vfs_env_init_hook;
	polar_vfs_env_init_hook = polar_stat_env_init_hook;
	polar_vfs_env_destroy_hook_prev = polar_vfs_env_destroy_hook;
	polar_vfs_env_destroy_hook = polar_stat_env_destroy_hook;
	polar_vfs_file_before_hook_prev = polar_vfs_file_before_hook;
	polar_vfs_file_before_hook = polar_stat_file_before_hook;
	polar_vfs_file_after_hook_prev = polar_vfs_file_after_hook;
	polar_vfs_file_after_hook = polar_stat_file_after_hook;
	polar_vfs_io_before_hook_prev = polar_vfs_io_before_hook;
	polar_vfs_io_before_hook = polar_stat_io_before_hook;
	polar_vfs_io_after_hook_prev = polar_vfs_io_after_hook;
	polar_vfs_io_after_hook = polar_stat_io_after_hook;
}

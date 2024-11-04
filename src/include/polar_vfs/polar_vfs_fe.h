/*-------------------------------------------------------------------------
 *
 * polar_vfs_fe.h
 *	  PolarDB  file storage definitions for fontend.
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
 *	  src/include/polar_vfs/polar_vfs_fe.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_VFS_FE_H
#define POLAR_VFS_FE_H

#include "postgres_fe.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "storage/polar_fd.h"

extern bool localfs_mode;
extern char *polar_datadir;
extern char *polar_argv0;
extern char *polar_disk_name;
extern int	polar_hostid;
extern char *polar_storage_cluster_name;

extern int	polar_mkdir_p(char *path, int omode);
extern void polar_vfs_init_fe(bool is_pfs, char *fname, char *storage_cluster_name, char *polar_disk_name, int flag);
extern void polar_vfs_destroy_fe(char *ftype, char *polar_disk_name);
extern bool polar_in_shared_storage_mode_fe(char *pgconfig);
extern bool polar_in_localfs_mode_fe(char *pgconfig);
extern bool polar_in_replica_mode_fe(const char *pgconfig);
extern void polar_vfs_init_simple_fe(char *pgconfig, char *pg_datadir, int flag);
extern void polar_vfs_destroy_simple_fe(void);
extern int	polar_vfs_state_backup_current(void);
extern int	polar_vfs_state_restore_current(int index);
extern int	polar_vfs_state_backup(bool is_shared, bool is_localfs, int hostid,
								   PolarNodeType node_type, char *datadir, char *disk_name);
extern int	polar_vfs_state_backup_current(void);
extern int	polar_vfs_state_restore_current(int index);
extern void polar_path_add_protocol(char *src, int max_len, bool is_localfs);

#endif

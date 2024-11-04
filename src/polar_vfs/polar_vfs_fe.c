/*-------------------------------------------------------------------------
 *
 * polar_vfs_fe.c
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
 *	  src/polar_vfs/polar_vfs_fe.c
 *
 *-------------------------------------------------------------------------
 */
#include <sys/mman.h>

#include "polar_vfs/polar_vfs_fe.h"
#include "polar_vfs/polar_vfs_interface.h"

#define LOCAL_IO_INDEX 0
#define PFS_IO_INDEX 1
/* Just like pfs tool, pg tool set polar_hostid to 0. */
#define PG_TOOL_HOSTID 0
#define POSTGRES_NAME "postgres"

typedef struct polar_vfs_state_t
{
	bool		polar_enable_shared_storage_mode;
	bool		localfs_mode;
	int			polar_hostid;
	PolarNodeType polar_local_node_type;
	char		polar_datadir[MAXPGPATH];
	char		polar_disk_name[MAXPGPATH];
} polar_vfs_state_t;

/* source instance and target instance. */
#define MAX_VFS_STATE 2
static polar_vfs_state_t polar_vfs_state[MAX_VFS_STATE];
static int	polar_vfs_next = 0;

/* polar elog/ereport for frontend code. */
static int	polar_elevel_fe;
static char *polar_errmsg = NULL;

char	   *polar_storage_cluster_name = NULL;
char	   *polar_disk_name = NULL;
char	   *polar_argv0 = NULL;
char	   *polar_datadir = NULL;
int			polar_hostid = PG_TOOL_HOSTID;

int			polar_vfs_switch = POLAR_VFS_SWITCH_LOCAL;
int			max_safe_fds = 32;	/* default if not changed */
bool		polar_enable_shared_storage_mode = false;

/*
 * Record the initial node type, it is initialized by postmaster, inherited by
 * other backends. We record this value in shared memory later.
 */
PolarNodeType polar_local_node_type = POLAR_UNKNOWN;

uint32		polar_local_vfs_state = POLAR_VFS_UNKNOWN;
AuxProcType MyAuxProcType = NotAnAuxProcess;
int			MyProcPid;
int			MyBackendId;

/*
 * The virtual file interface for frontend pg tools.
 * 1 The standard file interface function is used by default.
 * polar_vfs_switch = POLAR_VFS_SWITCH_LOCAL
 * 2 When a storage plugin is accessed, all io call implemented in the plugin.
 * polar_vfs_switch = POLAR_VFS_SWITCH_PLUGIN
 * 3 polar_vfs means use function in plugin to read write data which include local file or
 * stared storage file.
 */
vfs_mgr		polar_vfs[] =
{
	{
		.vfs_env_init = NULL,
		.vfs_env_destroy = NULL,
		.vfs_mount = NULL,
		.vfs_remount = NULL,
		.vfs_open = (vfs_open_type) open,
		.vfs_creat = creat,
		.vfs_close = close,
		.vfs_read = read,
		.vfs_write = write,
		.vfs_pread = pread,
		.vfs_preadv = preadv,
		.vfs_pwrite = pwrite,
		.vfs_pwritev = pwritev,
		.vfs_stat = stat,
		.vfs_fstat = fstat,
		.vfs_lstat = lstat,
		.vfs_lseek = lseek,
		.vfs_access = access,
		.vfs_fsync = fsync,
		.vfs_unlink = unlink,
		.vfs_rename = rename,
		.vfs_posix_fallocate = posix_fallocate,
#ifdef __linux__
		.vfs_fallocate = fallocate,
#else
		.vfs_fallocate = NULL,
#endif
		.vfs_ftruncate = ftruncate,
		.vfs_truncate = truncate,
		.vfs_opendir = opendir,
		.vfs_readdir = readdir,
		.vfs_closedir = closedir,
		.vfs_mkdir = mkdir,
		.vfs_rmdir = rmdir,
		.vfs_mgr_func = polar_get_local_vfs_mgr,
		.vfs_chmod = chmod,
		.vfs_mmap = mmap,
		.vfs_type = NULL,
	},
	{
		.vfs_env_init = NULL,
		.vfs_env_destroy = NULL,
		.vfs_mount = NULL,
		.vfs_remount = NULL,
		.vfs_open = NULL,
		.vfs_creat = NULL,
		.vfs_close = NULL,
		.vfs_read = NULL,
		.vfs_write = NULL,
		.vfs_pread = NULL,
		.vfs_preadv = NULL,
		.vfs_pwrite = NULL,
		.vfs_pwritev = NULL,
		.vfs_stat = NULL,
		.vfs_fstat = NULL,
		.vfs_lstat = NULL,
		.vfs_lseek = NULL,
		.vfs_access = NULL,
		.vfs_fsync = NULL,
		.vfs_unlink = NULL,
		.vfs_rename = NULL,
		.vfs_posix_fallocate = NULL,
		.vfs_fallocate = NULL,
		.vfs_ftruncate = NULL,
		.vfs_truncate = NULL,
		.vfs_opendir = NULL,
		.vfs_readdir = NULL,
		.vfs_closedir = NULL,
		.vfs_mkdir = NULL,
		.vfs_rmdir = NULL,
		.vfs_mgr_func = NULL,
		.vfs_chmod = NULL,
		.vfs_mmap = NULL,
		.vfs_type = NULL,
	}
};

static char *find_other_exec_or_ignore(const char *argv0, const char *target, const char *versionstr);
static char *trim(char *str);
static PolarNodeType polar_simple_node_type_by_file(const char *pgconfig);
static bool parse_bool_with_len(const char *value, size_t len, bool *result);
static bool parse_bool(const char *value, bool *result);
static void polar_get_config(const char *name, char *value, int value_len, const char *pgconfig);
static bool polar_get_config_bool(const char *name, const char *pgconfig);
static int	polar_get_config_int(const char *name, const char *pgconfig);
static char *polar_get_config_string(const char *name, const char *pgconfig);
static void polar_acquire_absolute_path(char *datadir, char *pg_datadir);

/*
 * Try to interpret value as boolean value.  Valid values are: true,
 * false, yes, no, on, off, 1, 0; as well as unique prefixes thereof.
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 */
static bool
parse_bool(const char *value, bool *result)
{
	return parse_bool_with_len(value, strlen(value), result);
}

static bool
parse_bool_with_len(const char *value, size_t len, bool *result)
{
	switch (*value)
	{
		case 't':
		case 'T':
			if (strncasecmp(value, "true", len) == 0)
			{
				if (result)
					*result = true;
				return true;
			}
			break;
		case 'f':
		case 'F':
			if (strncasecmp(value, "false", len) == 0)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case 'y':
		case 'Y':
			if (strncasecmp(value, "yes", len) == 0)
			{
				if (result)
					*result = true;
				return true;
			}
			break;
		case 'n':
		case 'N':
			if (strncasecmp(value, "no", len) == 0)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case 'o':
		case 'O':
			/* 'o' is not unique enough */
			if (strncasecmp(value, "on", (len > 2 ? len : 2)) == 0)
			{
				if (result)
					*result = true;
				return true;
			}
			else if (strncasecmp(value, "off", (len > 2 ? len : 2)) == 0)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case '1':
			if (len == 1)
			{
				if (result)
					*result = true;
				return true;
			}
			break;
		case '0':
			if (len == 1)
			{
				if (result)
					*result = false;
				return true;
			}
			break;
		case '\'':
		case '\"':
			return parse_bool_with_len(value + 1, len - 2, result);
			break;
		default:
			break;
	}

	if (result)
		*result = false;		/* suppress compiler warning */
	return false;
}

int
polar_mkdir_p(char *path, int omode)
{
	struct stat sb;
	mode_t		numask,
				oumask;
	int			last,
				retval;
	char	   *p;
	bool		is_polar_disk_name = true;

	retval = 0;
	p = path;

#ifdef WIN32
	/* skip network and drive specifiers for win32 */
	if (strlen(p) >= 2)
	{
		if (p[0] == '/' && p[1] == '/')
		{
			/* network drive */
			p = strstr(p + 2, "/");
			if (p == NULL)
			{
				errno = EINVAL;
				return -1;
			}
		}
		else if (p[1] == ':' &&
				 ((p[0] >= 'a' && p[0] <= 'z') ||
				  (p[0] >= 'A' && p[0] <= 'Z')))
		{
			/* local drive */
			p += 2;
		}
	}
#endif

	/*
	 * POSIX 1003.2: For each dir operand that does not name an existing
	 * directory, effects equivalent to those caused by the following command
	 * shall occur:
	 *
	 * mkdir -p -m $(umask -S),u+wx $(dirname dir) && mkdir [-m mode] dir
	 *
	 * We change the user's umask and then restore it, instead of doing
	 * chmod's.  Note we assume umask() can't change errno.
	 */
	oumask = umask(0);
	numask = oumask & ~(S_IWUSR | S_IXUSR);
	(void) umask(numask);

	if (p[0] == '/')			/* Skip leading '/'. */
		++p;
	for (last = 0; !last; ++p)
	{
		if (p[0] == '\0')
			last = 1;
		else if (p[0] != '/')
			continue;

		if (!localfs_mode && is_polar_disk_name)
		{
			is_polar_disk_name = false;
			continue;
		}

		*p = '\0';
		if (!last && p[1] == '\0')
			last = 1;

		if (last)
			(void) umask(oumask);

		/* check for pre-existing directory */
		if (polar_stat(path, &sb) == 0)
		{
			if (!S_ISDIR(sb.st_mode))
			{
				if (last)
					errno = EEXIST;
				else
					errno = ENOTDIR;
				retval = -1;
				break;
			}
		}
		else if (polar_mkdir(path, last ? omode : S_IRWXU | S_IRWXG | S_IRWXO) < 0)
		{
			retval = -1;
			break;
		}
		if (!last)
			*p = '/';
	}

	/* ensure we restored umask */
	(void) umask(oumask);

	return retval;
}

/*
 * Init polar file system for frontend
 * There are differences between backend and frontend:
 * (1) hosid is not same with backend, it may be PG_TOOL_HOSTID by root.
 * (2) mount could redirect stderr which is harmful to frontend but not backend.
 */
void
polar_vfs_init_fe(bool is_pfs, char *fname, char *storage_cluster_name, char *polar_disk_name, int flag)
{
	int			old_stderr;
	int			hostid = PG_TOOL_HOSTID;
	vfs_mount_arg_t polar_mount_arg;
	pfsd_mount_arg_t pfsd_mount_arg;

	if (!is_pfs)
		localfs_mode = true;
	else
		localfs_mode = false;

	/*
	 * Do not init polar vfs when instance is not in shared storage mode.
	 */
	if (!polar_enable_shared_storage_mode)
		return;

	polar_init_vfs_cache();
	polar_init_vfs_function();

	if (flag == POLAR_VFS_RDWR)
	{
#ifndef WIN32
		/* PG_TOOL_HOSTID can only be used by root. */
		if (geteuid() != 0)
			hostid = polar_hostid;
#endif
	}

	/* Set PFS_TOOL flag to work like pfs tool */
	if (hostid == PG_TOOL_HOSTID)
		flag |= POLAR_VFS_TOOL;

	if (localfs_mode)
	{
		if (!POLAR_DIRECTIO_IS_ALIGNED(polar_max_direct_io_size))
		{
			fprintf(stderr, "polar_max_direct_io_size is not aligned!\n");
			exit(EXIT_FAILURE);
		}
		else if (polar_directio_buffer == NULL &&
				 posix_memalign((void **) &polar_directio_buffer,
								PG_IO_ALIGN_SIZE,
								polar_max_direct_io_size) != 0)
		{
			fprintf(stderr, "posix_memalign alloc polar_directio_buffer failed!\n");
			exit(EXIT_FAILURE);
		}
		else
			polar_vfs_switch = POLAR_VFS_SWITCH_PLUGIN;
		return;
	}

	if (polar_disk_name == NULL ||
		strlen(polar_disk_name) < 1)
	{
		fprintf(stderr, "invalid polar_disk_name\n");
		exit(EXIT_FAILURE);
	}

	/* mount should use pfsd_mount interface, so switch vfs kind */
	polar_vfs_switch = POLAR_VFS_SWITCH_PLUGIN;

	PFSD_INIT_MOUNT_ARG(pfsd_mount_arg, storage_cluster_name, polar_disk_name, hostid, flag);
	VFS_INIT_MOUNT_ARG(polar_mount_arg, fname, &pfsd_mount_arg);

	/* pfsd_mount could redirect stderr, so dup and dup2 it. */
	old_stderr = dup(fileno(stderr));
	if (polar_mount(&polar_mount_arg) != 0)
	{
		dup2(old_stderr, fileno(stderr));
		close(old_stderr);
		fprintf(stderr, "can't mount PBD %s, id %d with flag 0x%x",
				polar_disk_name, hostid, flag);
		exit(EXIT_FAILURE);
	}
	dup2(old_stderr, fileno(stderr));
	close(old_stderr);
	polar_local_vfs_state = flag;
}

/*
 * Unmount polar file system for frontend.
 */
void
polar_vfs_destroy_fe(char *ftype, char *disk_name)
{
	/*
	 * Do not destroy polar vfs when instance is not in shared storage mode.
	 */
	if (localfs_mode || !polar_enable_shared_storage_mode)
		return;
	else if (disk_name == NULL)
		fprintf(stderr, "disk name is NULL!");
	else if (polar_umount(ftype, disk_name) != 0)
		fprintf(stderr, _("can't unmount PBD %s"), disk_name);
}

static char *
find_other_exec_or_ignore(const char *argv0, const char *target, const char *versionstr)
{
	int			ret;
	char	   *found_path;

	found_path = pg_malloc(MAXPGPATH);

	if ((ret = find_other_exec(argv0, target, versionstr, found_path)) < 0)
	{
		pg_free(found_path);
		found_path = NULL;
	}

	return found_path;
}

static char *
trim(char *str)
{
	char	   *p = str,
			   *p1;

	if (p)
	{
		while (*p && isspace(*p))
			p++;
		p1 = str + strlen(str) - 1;
		while (p1 > p && isspace(*p1))
		{
			*p1 = '\0';
			p1--;
		}
	}
	return p;
}

static void
polar_get_config(const char *name, char *value, int value_len, const char *pgconfig)
{
#define MAX_STRING 1024
	char		cmd[MAX_STRING],
				cmd_output[MAX_STRING];
	FILE	   *output;
	int			old_stderr,
				fd;
	char	   *exec_path = NULL;

	if (value == NULL)
		return;
	exec_path = find_other_exec_or_ignore(polar_argv0, POSTGRES_NAME, PG_BACKEND_VERSIONSTR);
	if (!exec_path)
		return;
	snprintf(cmd, sizeof(cmd), "%s -C %s -D \"%s\"", exec_path, name, pgconfig);
	/* Any error message from popen will be ignored! */
	fd = open(DEVNULL, O_WRONLY, 0);
	if (fd < 0)
	{
		fprintf(stderr, _("%s: could not open %s"), __func__, DEVNULL);
		exit(EXIT_FAILURE);
	}
	old_stderr = dup(fileno(stderr));
	(void) dup2(fd, fileno(stderr));
	close(fd);
	output = popen(cmd, "r");
	if (output && fgets(cmd_output, sizeof(cmd_output) - 1, output) != NULL)
		strncpy(value, trim(cmd_output), value_len > MAX_STRING ? MAX_STRING : value_len);
	(void) dup2(old_stderr, fileno(stderr));
	close(old_stderr);
	if (output)
		pclose(output);
}

static bool
polar_get_config_bool(const char *name, const char *pgconfig)
{
#define GUC_VALUE_LEN 20
	char		value[GUC_VALUE_LEN] = {0x0};
	bool		ret = false;

	polar_get_config(name, value, GUC_VALUE_LEN, pgconfig);
	if (strlen(value) > 0 && !parse_bool(value, &ret))
	{
		fprintf(stderr, _("parameter %s requires a Boolean value"), name);
		exit(EXIT_FAILURE);
	}
	return ret;
}

static int
polar_get_config_int(const char *name, const char *pgconfig)
{
#define GUC_VALUE_LEN 20
	char		value[GUC_VALUE_LEN] = {0x0};
	int			ret = 0;

	polar_get_config(name, value, GUC_VALUE_LEN, pgconfig);
	if (strlen(value) > 0)
		ret = atoi(value);
	return ret;
}

static char *
polar_get_config_string(const char *name, const char *pgconfig)
{
	char	   *val = NULL;

	val = pg_malloc0(MAXPGPATH);

	polar_get_config(name, val, MAXPGPATH, pgconfig);

	if (strlen(val) < 1)
	{
		pfree(val);
		val = NULL;
	}

	return val;
}


static void
polar_acquire_absolute_path(char *datadir, char *pg_datadir)
{
	const char *path = NULL;
	char		current_path[MAXPGPATH] = {0};

	path = polar_path_remove_protocol(datadir);

	if (!path || is_absolute_path(path))
		return;

	/* POLAR: get current dir */
	if (!getcwd(current_path, MAXPGPATH))
	{
		fprintf(stderr, "can not get current path %s\n", current_path);
		exit(EXIT_FAILURE);
	}

	if (chdir(pg_datadir) < 0)
	{
		fprintf(stderr, "can not change working directory to pg datadir %s\n", pg_datadir);
		exit(EXIT_FAILURE);
	}

	/* POLAR : change working directory to data dir */
	if (chdir(path) < 0)
	{
		fprintf(stderr, "can not change working directory to polar datadir %s\n", path);
		exit(EXIT_FAILURE);
	}

	MemSet(datadir, 0, MAXPGPATH);
	if (!getcwd(datadir, MAXPGPATH))
	{
		fprintf(stderr, "get polar datadir absolute path %s fail\n", datadir);
		exit(EXIT_FAILURE);
	}

	if (chdir(current_path) < 0)
	{
		fprintf(stderr, "can not change working directory to current dir %s\n", current_path);
		exit(EXIT_FAILURE);
	}
}

bool
polar_in_shared_storage_mode_fe(char *pgconfig)
{
#define GUC_NAME_STORAGE_MODE "polar_enable_shared_storage_mode"
	return polar_get_config_bool(GUC_NAME_STORAGE_MODE, pgconfig);
}

/*
 * The guc parameters of polar vfs plugin will be string type
 * before the plugin was loaded.
 */
bool
polar_in_localfs_mode_fe(char *pgconfig)
{
#define GUC_NAME_LOCALFS_MODE "polar_vfs.localfs_mode"
	return polar_get_config_bool(GUC_NAME_LOCALFS_MODE, pgconfig);
}

static PolarNodeType
polar_simple_node_type_by_file(const char *pgconfig)
{
	struct stat stat_buf;
	PolarNodeType polar_node_type = POLAR_PRIMARY;
	char		path[MAXPGPATH];

	sprintf(path, "%s/%s", pgconfig, STANDBY_SIGNAL_FILE);
	if (stat(path, &stat_buf) == 0)
	{
		polar_node_type = POLAR_STANDBY;
		return polar_node_type;
	}

	sprintf(path, "%s/%s", pgconfig, REPLICA_SIGNAL_FILE);
	if (stat(path, &stat_buf) == 0)
	{
		polar_node_type = POLAR_REPLICA;
		return polar_node_type;
	}

	return polar_node_type;
}

bool
polar_in_replica_mode_fe(const char *pgconfig)
{
	return polar_local_node_type != POLAR_UNKNOWN ?
		polar_local_node_type == POLAR_REPLICA :
		(polar_simple_node_type_by_file(pgconfig) == POLAR_REPLICA);
}

void
polar_vfs_init_simple_fe(char *pgconfig, char *pg_datadir, int flag)
{
	polar_datadir = polar_get_config_string("polar_datadir", pgconfig);
	polar_enable_shared_storage_mode = polar_in_shared_storage_mode_fe(pgconfig);
	localfs_mode = polar_in_localfs_mode_fe(pgconfig);

	if (polar_datadir != NULL && pg_datadir != NULL)
		polar_acquire_absolute_path(polar_datadir, pg_datadir);

	if (localfs_mode)
		polar_vfs_init_fe(false, polar_datadir, NULL, NULL, flag);
	else
	{
		polar_storage_cluster_name = polar_get_config_string("polar_storage_cluster_name", pgconfig);
		polar_disk_name = polar_get_config_string("polar_disk_name", pgconfig);
		polar_hostid = polar_get_config_int("polar_hostid", pgconfig);
		polar_vfs_init_fe(true, polar_datadir, polar_storage_cluster_name, polar_disk_name, flag);
	}
	/* we add polar vfs protocol tag at the start of polar_datadir */
	if (polar_datadir == NULL)
		polar_datadir = pg_malloc0(MAXPGPATH);
	polar_path_add_protocol(polar_datadir, MAXPGPATH, localfs_mode);
	polar_local_node_type = polar_simple_node_type_by_file(pgconfig);
}

void
polar_vfs_destroy_simple_fe(void)
{
	if (polar_disk_name != NULL)
	{
		polar_vfs_destroy_fe(polar_datadir, polar_disk_name);
		pg_free(polar_disk_name);
		polar_disk_name = NULL;
	}
	if (polar_datadir)
	{
		pg_free(polar_datadir);
		polar_datadir = NULL;
	}
	if (polar_storage_cluster_name != NULL)
	{
		pg_free(polar_storage_cluster_name);
		polar_storage_cluster_name = NULL;
	}
}

int
polar_vfs_state_backup(bool is_shared, bool is_localfs, int hostid, PolarNodeType node_type, char *datadir, char *disk_name)
{
	if (polar_vfs_next >= MAX_VFS_STATE)
	{
		fprintf(stderr, _("%s: polar_vfs state stack is full!"), __func__);
		exit(EXIT_FAILURE);
	}
	if (datadir == NULL)
	{
		fprintf(stderr, _("%s: polar_vfs state backup failed!"), __func__);
		exit(EXIT_FAILURE);
	}
	polar_vfs_state[polar_vfs_next].localfs_mode = is_localfs;
	polar_vfs_state[polar_vfs_next].polar_enable_shared_storage_mode = is_shared;
	polar_vfs_state[polar_vfs_next].polar_local_node_type = node_type;
	polar_vfs_state[polar_vfs_next].polar_hostid = hostid;
	strncpy(polar_vfs_state[polar_vfs_next].polar_datadir, datadir, MAXPGPATH);
	if (disk_name)
		strncpy(polar_vfs_state[polar_vfs_next].polar_disk_name, disk_name, MAXPGPATH);
	polar_vfs_next++;
	return polar_vfs_next - 1;
}

int
polar_vfs_state_backup_current(void)
{
	return polar_vfs_state_backup(polar_enable_shared_storage_mode, localfs_mode, polar_hostid,
								  polar_local_node_type, polar_datadir, polar_disk_name);
}

int
polar_vfs_state_restore_current(int index)
{
	int			ret = -1;
	static int	cur_index = -1;

	if (index >= MAX_VFS_STATE || index < 0)
	{
		fprintf(stderr, _("%s: index(%d) is out of polar_vfs state's stack(%d)!"), __func__, index, MAX_VFS_STATE);
		exit(EXIT_FAILURE);
	}
	if (cur_index == index)
		return cur_index;
	if (polar_datadir == NULL)
		polar_datadir = pg_malloc0(MAXPGPATH);
	if (polar_disk_name == NULL)
		polar_disk_name = pg_malloc0(MAXPGPATH);
	localfs_mode = polar_vfs_state[index].localfs_mode;
	polar_enable_shared_storage_mode = polar_vfs_state[index].polar_enable_shared_storage_mode;
	polar_local_node_type = polar_vfs_state[index].polar_local_node_type;
	polar_hostid = polar_vfs_state[index].polar_hostid;
	strncpy(polar_disk_name, polar_vfs_state[index].polar_disk_name, MAXPGPATH);
	strncpy(polar_datadir, polar_vfs_state[index].polar_datadir, MAXPGPATH);
	ret = cur_index;
	cur_index = index;
	return ret;
}

void
polar_path_add_protocol(char *src, int max_len, bool is_localfs)
{
	if (polar_path_remove_protocol(src) == src)
	{
		char		tmp[MAXPGPATH];

		MemSet(tmp, 0x0, sizeof(tmp));
		if (is_localfs)
			snprintf(tmp, MAXPGPATH, "%s%s", POLAR_VFS_PROTOCOL_LOCAL_BIO, src);
		else
			snprintf(tmp, MAXPGPATH, "%s%s", POLAR_VFS_PROTOCOL_PFS, src);
		if (max_len > strlen(tmp))
			strncpy(src, tmp, max_len);
		else
			fprintf(stderr, _("%s: max_len(%d) is not big enough!"), __func__, max_len);
	}
}

/*
 * Provide errstart/errfinish/errcode/errmsg to make ereport(...) function available
 * for frontend pg tools's source code.
 */
int
errcode(int sqlerrcode)
{
	return 0;
}

int
errmsg(const char *fmt,...)
{
	int			polar_errmsg_len = 10;

	if (polar_errmsg)
	{
		pg_free(polar_errmsg);
		polar_errmsg = NULL;
	}
	polar_errmsg = pg_malloc(polar_errmsg_len);
	while (true)
	{
		int			needed;
		va_list		args;

		va_start(args, fmt);
		needed = vsnprintf(polar_errmsg, polar_errmsg_len - 1, fmt, args);
		va_end(args);
		if (needed <= polar_errmsg_len)
			break;
		polar_errmsg_len += needed;
		polar_errmsg = pg_realloc(polar_errmsg, polar_errmsg_len);
	}
	return 0;
}

bool
errstart(int elevel, const char *domain)
{
	polar_elevel_fe = elevel;
	return true;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	if (polar_errmsg)
	{
		if (polar_elevel_fe > LOG && polar_elevel_fe < ERROR)
			fprintf(stdout, "%s\n", polar_errmsg);
		else if (polar_elevel_fe >= ERROR)
			fprintf(stderr, "%s\n", polar_errmsg);
		pg_free(polar_errmsg);
		polar_errmsg = NULL;
		if (polar_elevel_fe >= ERROR)
			exit(EXIT_FAILURE);
	}
}

int
errmsg_internal(const char *fmt,...)
{
	int			polar_errmsg_len = 10;

	if (polar_errmsg)
	{
		pg_free(polar_errmsg);
		polar_errmsg = NULL;
	}
	polar_errmsg = pg_malloc(polar_errmsg_len);
	while (true)
	{
		int			needed;
		va_list		args;

		va_start(args, fmt);
		needed = vsnprintf(polar_errmsg, polar_errmsg_len - 1, fmt, args);
		va_end(args);
		if (needed <= polar_errmsg_len)
			break;
		polar_errmsg_len += needed;
		polar_errmsg = pg_realloc(polar_errmsg, polar_errmsg_len);
	}
	return 0;
}


pg_attribute_cold
bool
errstart_cold(int elevel, const char *domain)
{
	return errstart(elevel, domain);
}

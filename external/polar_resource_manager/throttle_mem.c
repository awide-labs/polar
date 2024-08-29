/*-------------------------------------------------------------------------
 *
 * throttle_mem.c
 *	  API to get memory statistics.
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
 *	  external/polar_resource_manager/throttle_mem.c
 *
 *-------------------------------------------------------------------------
 */

#include "throttle_mem.h"

#include <dirent.h>
#include <sys/types.h>

#include "utils/hsearch.h"

typedef enum PROCARGTYPE
{
	PROCNUM = 0,
	PROCSTR = 1,
}			PROCARGTYPE;

static const char *delim = " ";

char		polar_cgroup_mem_path[MAXPGPATH] = "";

static char *readone(char *curstr, void *x, PROCARGTYPE argtype);

static char *
readone(char *curstr, void *x, PROCARGTYPE argtype)
{
	char	   *token = NULL;
	char	   *savestr = NULL;

	token = strtok_r(curstr, delim, &savestr);
	if (token)
	{
		switch (argtype)
		{
			case PROCNUM:
				*(long int *) x = (long int) atoll(token);
				break;

			case PROCSTR:
				strncpy((char *) x, token, savestr - curstr);
				break;

			default:
				break;
		}
	}
	return savestr;
}

/* Get the file handle of /proc/[pid]/stat according to pid */
FILE *
polar_get_statfd_by_pid(int pid)
{
	static char paths[MAXPGPATH];
	FILE	   *fd = NULL;

	if (PidIsInvalid(pid))
		return NULL;

	/* Get the statm file name according to pid */
	memset(paths, 0, sizeof(paths));
	sprintf(paths, "/proc/%d/stat", pid);

	fd = fopen(paths, "r");

	return fd;
}

/* Get the file handle of /proc/[pid]/statm according to pid */
FILE *
polar_get_statmfd_by_pid(int pid)
{
	static char paths[MAXPGPATH];
	FILE	   *fd = NULL;

	if (PidIsInvalid(pid))
		return NULL;

	/* Get the statm file name according to pid */
	memset(paths, 0, sizeof(paths));
	sprintf(paths, "/proc/%d/statm", pid);

	fd = fopen(paths, "r");

	return fd;
}

/* Get the process rss and pid according to process name. */
int
polar_get_procrss_by_name(char *procname, int *pid, Size *rss)
{
	char	   *curstr = NULL;
	char		paths[MAXPGPATH];
	char		procbuf[MAXSTATLEN];
	char		cmd[MAXPGPATH];
	FILE	   *fd = NULL;
	char		tty_null[64];
	DIR		   *dir;
	struct dirent *ptr;
	char		pidstr[64];
	int			procpid = InvalidPid;
	int			i = 0;

	*rss = 0;
	dir = opendir("/proc");

	while (*rss == 0)
	{
		if (PidIsValid(*pid))
		{
			/* If the pid is valid, get the process rss according to the pid */
			fd = polar_get_statfd_by_pid(*pid);
		}
		else
		{
			/*
			 * If the pid is invalid, get the process rss according to the
			 * name
			 */
			if (NULL != dir)
			{
				ptr = readdir(dir);

				if (ptr == NULL)
					break;

				/* Ignore invalid directories and filenames */
				if ((strcmp(ptr->d_name, ".") == 0) || (strcmp(ptr->d_name, "..") == 0))
					continue;
				if (DT_DIR != ptr->d_type)
					continue;
				if (ptr->d_name[0] < '0' || ptr->d_name[0] > '9')
					continue;

				memset(paths, 0, sizeof(paths));
				memset(procbuf, 0, MAXSTATLEN);

				/* Get the statm file name according to ptr->d_name */
				snprintf(paths, sizeof(paths), "/proc/%s/stat", ptr->d_name);
				fd = fopen(paths, "r");
			}
		}

		/* Continue if the current file handle is invalid */
		if (fd == NULL)
		{
			*pid = InvalidPid;
			continue;
		}

		/* Get file content */
		if (!fread(procbuf, MAXSTATLEN, 1, fd))
		{
			if (ferror(fd))
			{
				*pid = InvalidPid;
				fclose(fd);
				fd = NULL;
				continue;
			}
		}

		curstr = readone(procbuf, pidstr, PROCSTR);
		procpid = (int) atoi(pidstr);
		curstr = readone(curstr, cmd, PROCSTR);

		/* Check if the process name in the file is the same as the procname */
		if (strcmp(cmd, procname) != 0)
		{
			fclose(fd);
			*pid = InvalidPid;
			continue;
		}

		/* We don't need extra here */
		for (i = 0; i < 21; i++)
			curstr = readone(curstr, tty_null, PROCSTR);

		/* Get process rss */
		curstr = readone(curstr, rss, PROCNUM);
		*rss *= 4096;
		*pid = procpid;
		fclose(fd);
		break;
	}

	closedir(dir);
	if (*rss == 0)
		return 1;
	else
		return 0;
}

/* Get the process rss according to process pid. */
int
polar_get_procrss_by_pidstat(int pid, Size *rss)
{
	char	   *curstr = NULL;
	char		procbuf[MAXSTATLEN];
	char		tty_null[64];
	int			i;
	FILE	   *fd = NULL;

	if (PidIsInvalid(pid))
		return 1;

	/* Get the file handle of /proc/[pid]/stat according to pid */
	fd = polar_get_statfd_by_pid(pid);

	if (NULL == fd)
	{
		return 1;
	}

	/* Get file content */
	if (!fread(procbuf, MAXSTATLEN, 1, fd))
	{
		if (ferror(fd))
		{
			fclose(fd);
			return 1;
		}
	}

	curstr = readone(procbuf, tty_null, PROCSTR);

	/* We don't need extra here */
	for (i = 0; i < 22; i++)
		curstr = readone(curstr, tty_null, PROCSTR);

	/* Get process rss */
	curstr = readone(curstr, rss, PROCNUM);
	*rss *= 4096;

	return fclose(fd);
}

/*
 * Get the process rss according to process pid.
 * Parse a proc/[pid]/statm file.
 */
int
polar_get_procrss_by_pidstatm(int pid, int procflag, Size *rss)
{
	char		procbuf[MAXSTATLEN];
	char	   *token = NULL;
	char	   *savestr = NULL;
	char	   *savestr2 = NULL;
	FILE	   *fd = NULL;

	if (PidIsInvalid(pid))
		return 1;

	/* Get the file handle of /proc/[pid]/statm according to pid */
	fd = polar_get_statmfd_by_pid(pid);

	if (NULL == fd)
	{
		return 1;
	}

	/* Get file content */
	if (!fread(procbuf, MAXSTATLEN, 1, fd))
	{
		if (ferror(fd))
		{
			fclose(fd);
			return 1;
		}
	}

	/* Get process rss */
	strtok_r(procbuf, delim, &savestr);
	token = strtok_r(savestr, delim, &savestr2);

	switch (procflag)
	{
			/* The master process memory size is rss */
		case RM_TBLOCK_DEFAULT:
		case RM_TBLOCK_INPROGRESS:
			*rss = (Size) (atoll(token) << 12);
			break;
			/* The parallel process memory size is rss - share */
		case RM_TBLOCK_PARALLEL_INPROGRESS:
			{
				char	   *savestr3 = NULL;
				char	   *token2 = NULL;

				token2 = strtok_r(savestr2, delim, &savestr3);
				*rss = (Size) ((atoll(token) - atoll(token2)) << 12);
			}
			break;
		default:
			break;
	}

	return fclose(fd);
}

/*
 * Get the instance memory limit and memory usage.
 * Parse a /sys/fs/cgroup/memory/memory.stat file.
 */
int
polar_get_ins_memorystat(Size *rss, Size *mapped_file, Size *limit)
{
	char	   *curstr = NULL;
	char		key[64];
	char		procbuf[MAXSTATLEN];

	FILE	   *fd = NULL;

	*rss = 0;
	*mapped_file = 0;

	/* Get the memory.stat file handle */
	fd = fopen(polar_cgroup_mem_path, "r");

	if (NULL == fd)
		return 1;

	/*
	 * Hierarchical_memory_limit is the instance memory limit. Rss and
	 * mapped_file is the instance memory usage.
	 */
	while (fgets(procbuf, MAXSTATLEN, fd) != NULL)
	{
		curstr = readone(procbuf, key, PROCSTR);
		if (strcmp(key, "rss") == 0)
		{
			curstr = readone(curstr, rss, PROCNUM);
		}
		else if (strcmp(key, "mapped_file") == 0)
		{
			curstr = readone(curstr, mapped_file, PROCNUM);
		}
		else if (strcmp(key, "hierarchical_memory_limit") == 0)
		{
			curstr = readone(curstr, limit, PROCNUM);
			break;
		}
	}

	fclose(fd);

	return *rss == 0 || *limit == 0;
}

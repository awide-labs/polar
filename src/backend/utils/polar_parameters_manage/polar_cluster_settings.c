/*-------------------------------------------------------------------------
 *
 * polar_cluster_settings.c
 *
 * Implementation of cluster configure file.
 *
 * The features including parameter initialization, parameter reading,
 * and parameter modification at the cluster level.
 *
 * The core design point is the cluster configure file is on the shared
 * storage. Each node(RW/RO/STANDBY) has it's own local configuration
 * file as the cache of the cluster configuration file.
 *
 * Copy the cluster configuration file to the local cache file when
 * node start up, and maintain the consistency of the local cache file
 * and the cluster file when the parameters are modified.
 *
 * The reason for this design:
 * 1. pfs not support FILE* interface which is neceesary for parser
 * configure file in PG.
 *
 * 2. The concurrency control of the file depends on the file rename.
 * If the file is copied from the shared storage when reading the file
 * each time, the local local file will be written and reading concurrently,
 * and the write interface cannot guarantee atomicity. So may read an
 * intermediate state.
 *
 * 3. when Reading cluster setting, only needs to read the local cache
 * file, the performance is better.
 *
 * Detailed implementation of the features:
 * 1. parameter initialization
 * initdb will initialize different parameter templates according to
 * deploy-mode and compatibility_mode.
 *
 * 2. parameter reading
 * copy cluster settings to local cache file when start up, after
 * that only need to reading local cache file.
 *
 * 3. parameter modification
 * modefy cluster setting and local cache file at the same time,
 * write wal to ensure the consistency of RW/RO/STANDBY.
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
 *	  src/backend/utils/polar_parameters_manage/polar_cluster_settings.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/polar_fd.h"
#ifdef FAULT_INJECTOR
#include "utils/faultinjector.h"
#endif
#include "utils/guc.h"
#include "utils/polar_cluster_settings.h"

static bool polar_setting_file_global_to_local(void);
static pg_crc32c polar_get_file_crc32c(char *filename);
static bool polar_make_absolute_file_path(char *path);


/*
 * POLAR:
 * Ensure some config file content not be changed.
 * docheck = false: Caculate file crc and save them.
 * docheck = true: Calculate file crc and verify
 * that they are the same as last saved.
 */
void
polar_check_config_file_change(bool docheck)
{
#define POLAR_CHECK_FILE_NUMS		  2
#define SET_CHECK_FILE_NAME(filename) (check_file_names[i++] = filename)

	char	   *check_file_names[POLAR_CHECK_FILE_NUMS];
	int			i = 0;
	static pg_crc32c check_file_crcs[POLAR_CHECK_FILE_NUMS];

	SET_CHECK_FILE_NAME(ConfigFileName);
	SET_CHECK_FILE_NAME(PG_AUTOCONF_FILENAME);

	Assert(i == POLAR_CHECK_FILE_NUMS);

	if (docheck)
	{
		for (i = 0; i < POLAR_CHECK_FILE_NUMS; ++i)
			if (!EQ_CRC32C(check_file_crcs[i], polar_get_file_crc32c(check_file_names[i])))
			{
				ereport(FATAL,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("configuration file \"%s\" cannot be changed "
								"during database startup, Please try restart",
								check_file_names[i])));
			}
	}
	else
	{
		for (i = 0; i < POLAR_CHECK_FILE_NUMS; ++i)
			check_file_crcs[i] = polar_get_file_crc32c(check_file_names[i]);
	}
}

/*
 * POLAR:
 * Mount shared storage and ProcessConfigFile
 * including polar_settings.conf
 */
void
polar_mount_and_extra_load_polar_settings(const bool polar_is_show_guc_mode)
{
	Assert(!IsUnderPostmaster);

	/*
	 * when "-C guc" was specified, This is just to show the parameter value,
	 * there is a high probability that there are other postgres services for
	 * users. mount vfs may failed, so only read local cache file.
	 */
	if (polar_is_show_guc_mode)
	{
		/*
		 * extra load polar_settings.conf Use this virtual context here to
		 * make sure this load only additionally loads polar_settings.conf.set
		 * same guc to same value twice will make some problem. Consider the
		 * following configuration in postgresql.conf: recovery_target = ''
		 * recovery_target_time = '2021-10-31'
		 */
		ProcessConfigFile(PGC_POSTMASTER_ONLY_LOAD_POLAR_SETTINGS);

		/*
		 * Because the parameters are reloaded once again, we need to ensure
		 * the config files have not been changed. Otherwise parameters will
		 * be inconsistent and postmaster should not continue to startup.
		 */
		polar_check_config_file_change(true);

		return;
	}

	/*
	 * POLAR: init node type before load shared libraries which may need to
	 * check node type.
	 */
	polar_init_node_type();

	/*
	 * Mount shared storage in advance. We can only copy polar_settings.conf
	 * after mount.
	 */
	polar_load_vfs();

	/*
	 * polar_settings.conf is a configuration file placed in the shared
	 * storage with the instance level. We copy the configuration file and
	 * store it locally when the postmaster startup. Only when the copy is
	 * successful can the local file be parsed.
	 *
	 * The reason we do this is that the file interface for VFS can't return
	 * FILE* interface for parsing configuration files.
	 */
	if (!polar_enable_shared_storage_mode || polar_setting_file_global_to_local())
	{
		if (polar_enable_shared_storage_mode)
			elog(LOG, "\"%s\" is copied from sharedstorage success, additional load it.", POLAR_SETTING_FILENAME);
		else
			elog(LOG, "\"%s\" is in non-sharedstorage mode, additional load it.", POLAR_SETTING_FILENAME);

		/*
		 * extra load polar_settings.conf Use this virtual context here to
		 * make sure this load only additionally loads polar_settings.conf.set
		 * same guc to same value twice will make some problem. Consider the
		 * following configuration in postgresql.conf: recovery_target = ''
		 * recovery_target_time = '2021-10-31'
		 */
		ProcessConfigFile(PGC_POSTMASTER_ONLY_LOAD_POLAR_SETTINGS);

		/*
		 * Because the parameters are reloaded once again, we need to ensure
		 * the config files have not been changed. Otherwise parameters will
		 * be inconsistent and postmaster should not continue to startup.
		 */
		polar_check_config_file_change(true);
	}
	else
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("global settings file \"%s\" is not exists "
						"during database startup, may be caused by an old instance upgrade.",
						POLAR_SETTING_FILENAME)));
	}

	return;
}


/*
 * POLAR:
 * Log wal to record the global polar_settings.conf change.
 * START_CRIT_SECTION before the end of flush wal.
 * So call END_CRIT_SECTION after call polar_cluster_setting_wal_write.
 */
void
polar_cluster_setting_wal_write(char *name, char *value, bool resetall, bool reload)
{
	PolarSettingWalHeader header;
#ifdef FAULT_INJECTOR
	SIMPLE_FAULT_INJECTOR("POLAR ALTER SYSTEM FOR CLUSTER BEFORE WAL WRITE");
#endif

	/*
	 * ALTER SYSTEM FOR CLUSTER use POLAR WAL which subtype is
	 * PWT_ALTERSYSTEMFORCLUSTER, PWT_ALTERSYSTEMFORCLUSTER_RELOAD
	 */
	if (reload)
		header.pwt_type = PWT_ALTERSYSTEMFORCLUSTER_RELOAD;
	else
		header.pwt_type = PWT_ALTERSYSTEMFORCLUSTER;

	if (resetall)
	{
		Assert(name == NULL && value == NULL);
		header.action = POLAR_SETTING_RESET_ALL;
	}
	else if (name && value)
	{
		header.action = POLAR_SETTING_SET;
	}
	else if (name)
	{
		Assert(value == NULL);
		header.action = POLAR_SETTING_RESET;
	}
	else
		elog(ERROR, "unexpected ALTER SYSTEM FOR CLUSTER command");

	/* Begin write WAL */
	XLogBeginInsert();

	/* Register our header */
	XLogRegisterData((char *) &header, sizeof(PolarSettingWalHeader));

	/*
	 * Register flexible length value Register strlen + 1 to make sure string
	 * end with '\0' in wal
	 */
	switch (header.action)
	{
			/* ALTER SYSTEM SET folowing with both name and value */
		case POLAR_SETTING_SET:
			{
				XLogRegisterData(name, strlen(name) + 1);
				XLogRegisterData(value, strlen(value) + 1);
				break;
			}

			/* ALTER SYSTEM RESET only folowing with name */
		case POLAR_SETTING_RESET:
			{
				XLogRegisterData(name, strlen(name) + 1);
				break;
			}

			/* ALTER SYSTEM RESET ALL contains nothing */
		case POLAR_SETTING_RESET_ALL:
			break;

		default:
			elog(ERROR, "unexpected ALTER SYSTEM FOR CLUSTER WAL action");
			break;
	}

	/*
	 * After FlushWAL rename local file and global file in critical mode to
	 * ensure consistency Also make flush wal can't be ERROR.
	 */
	START_CRIT_SECTION();

	/* Flush wal to make sure file change persistence */
	XLogFlush(XLogInsert(RM_XLOG_ID, POLAR_WAL));
#ifdef FAULT_INJECTOR
	SIMPLE_FAULT_INJECTOR("POLAR ALTER SYSTEM FOR CLUSTER AFTER WAL WRITE");
#endif
}

/*
 * POLAR:
 * Apply wal to ensure the consistency of RW/RO/STANDBY
 */
void
polar_cluster_setting_wal_redo(XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	char	   *name = NULL;
	char	   *value = NULL;
	bool		resetall = false;
	PolarSettingWalHeader *header;
	StringInfoData buf;
	PolarWalType type;
	int			polar_options = ALTOPT_CLUSTER;

	/* Read header to get polar_wal type */
	memcpy(&type, XLogRecGetData(record), sizeof(PolarWalType));
	if (type == PWT_ALTERSYSTEMFORCLUSTER_RELOAD)
		polar_options |= ALTOPT_RELOAD;

	initStringInfo(&buf);
	xlog_desc(&buf, record);
	elog(LOG, "ALTER SYSTEM FOR CLUSTER try redo, record is \'%s\'", buf.data);

	header = (PolarSettingWalHeader *) rec;

	switch (header->action)
	{
			/* ALTER SYSTEM SET folowing both name and value */
		case POLAR_SETTING_SET:
			{
				name = rec + sizeof(PolarSettingWalHeader);
				value = rec + sizeof(PolarSettingWalHeader) + strlen(name) + 1; /* +1 to skip '\0' */
				break;
			}

			/* ALTER SYSTEM RESET only folowing name */
		case POLAR_SETTING_RESET:
			name = rec + sizeof(PolarSettingWalHeader);
			break;

			/* ALTER SYSTEM RESET ALL folowing nothing */
		case POLAR_SETTING_RESET_ALL:
			resetall = true;
			break;

		default:
			elog(ERROR, "unexpected ALTER SYSTEM FOR CLUSTER WAL action");
			break;
	}

	AlterSystemSetConfigFileInternal(name,
									 value,
									 resetall,
									 true,	/* is polar_redo */
									 polar_options);	/* polar_options */
}

/*
 * POLAR:
 * Calculate the crc of the entire file,
 * return 0 if parsing the file fails.
 */
static pg_crc32c
polar_get_file_crc32c(char *filename)
{
#define READ_FILE_BUF_SIZE 4096

	char		path[MAXPGPATH] = {0};
	char		buf[READ_FILE_BUF_SIZE] = {0};
	pg_crc32c	crc;
	int			fd;
	int			size;

	if (filename == NULL)
		return 0;

	if (is_absolute_path(filename))
		snprintf(path, sizeof(path), "%s", filename);
	else
		snprintf(path, sizeof(path), "%s/%s", DataDir, filename);

	/* open and read file */
	fd = open(path, O_RDONLY | PG_BINARY);

	if (fd < 0)
		return 0;

	INIT_CRC32C(crc);
	while ((size = read(fd, buf, sizeof(buf) - 1)) > 0)
	{
		buf[size] = '\0';
		COMP_CRC32C(crc, buf, size);
	}
	FIN_CRC32C(crc);

	close(fd);
	return crc;
}

/*
 * POLAR:
 * make absolute file path for shared storage,
 * if path no need to change, return false.
 * else return true.
 */
static bool
polar_make_absolute_file_path(char *path)
{
	char		copy_path[MAXPGPATH];

	if (!is_absolute_path(polar_path_remove_protocol((const char *) path)))
	{
		strncpy(copy_path, path, MAXPGPATH);

		/* Generate absolute path without vfs protocol header */
		snprintf(path,
				 MAXPGPATH,
				 "%s/%s",
				 DataDir,
				 polar_path_remove_protocol(copy_path));

		return true;
	}

	return false;
}

/*
 * POLAR:
 * Copy polar_settings.conf in shared storage to local storage
 * when database start up.
 * After that, we only need to read the local file instead of
 * global file.
 */
static bool
polar_setting_file_global_to_local(void)
{
	char		LocalAllSettingFileName[MAXPGPATH * 2];
	char		GlobalAllSettingFileName[MAXPGPATH * 2];

	Assert(polar_enable_shared_storage_mode);

	snprintf(LocalAllSettingFileName,
			 sizeof(LocalAllSettingFileName),
			 "%s/%s",
			 DataDir,
			 POLAR_SETTING_FILENAME);
	polar_make_file_path_level3(GlobalAllSettingFileName, "global", POLAR_SETTING_FILENAME);

	/* Considering polar_datadir is relative path */
	if (polar_make_absolute_file_path(GlobalAllSettingFileName))
		elog(LOG, "Change \"%s\" absolute file path to %s", POLAR_SETTING_FILENAME, GlobalAllSettingFileName);

	/*
	 * Delete local polar_settings when startup, Ensure consistency of local
	 * conf and shared storage conf
	 */
	unlink(LocalAllSettingFileName);

	Assert(!polar_file_exists(LocalAllSettingFileName));

	if (!polar_file_exists(GlobalAllSettingFileName))
		return false;

	polar_copy_file(GlobalAllSettingFileName, LocalAllSettingFileName, false);

	if (!polar_file_exists(LocalAllSettingFileName))
		ereport(FATAL,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("copy global settings file \"%s\" to local failed ",
						POLAR_SETTING_FILENAME)));

	return true;
}

/*
 * POLAR:
 * ALTER FORCE command check.
 *
 * Simplified version of check guc to prevent
 * invalid parameters from causing failure to start
 */
void
polar_alter_force_check(char *name, char *value)
{
	Assert(name);

	/*
	 * We must also reject values containing newlines, because the grammar for
	 * config files doesn't support embedded newlines in string literals.
	 */
	if (value && strchr(value, '\n'))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter value for ALTER SYSTEM must not contain a newline")));

	/*
	 * extension guc can alter force, load configure file will ignore invalid
	 * extension guc
	 */
	if (strchr(name, GUC_QUALIFIER_SEPARATOR))
		;

	/*
	 * polar guc can alter force, load configure file will ignore invalid
	 * polar guc
	 */
	else if (strlen(name) > 6 &&
			 strncasecmp(name, "polar_", 6) == 0)
		;
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid configuration parameter \"%s\"",
						name),
				 errhint("force set only support polar and extension parameter")));

	if (value)
		ereport(WARNING,
				(errmsg("force set configuration parameter \"%s\" to \"%s\"",
						name,
						value),
				 errhint("skip check name and value, Please ensure valid setting")));
	else
		ereport(WARNING,
				(errmsg("force reset configuration parameter \"%s\"",
						name),
				 errhint("skip check name, Please ensure valid setting")));
}

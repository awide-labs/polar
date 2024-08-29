/*-------------------------------------------------------------------------
 *
 * control_data_change.c
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
 *	  src/bin/polar_tools/control_data_change.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_tools.h"

#include "catalog/pg_control.h"

static void
usage(void)
{
	printf("Control file data change\n");
	printf("-f, --file-path control file path\n");
	printf("-n, --new-path new control file path\n");
	printf("-c, --checkpoint-location\n");
	printf("-p, --redo-position new redo position\n");
	printf("-?, --help show this help, then exit\n");
}

static struct option long_options[] = {
	{"file-path", required_argument, NULL, 'f'},
	{"new-path", required_argument, NULL, 'n'},
	{"checkpoint-location", required_argument, NULL, 'c'},
	{"redo-position", required_argument, NULL, 'p'},
	{"help", no_argument, NULL, '?'},
	{NULL, 0, NULL, 0}
};

static bool
write_control_data(char *file_path, ControlFileData *data)
{
	FILE	   *fp;
	bool		succeed = false;

	pg_crc32c	crc;

	/* Check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				(char *) data,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	data->crc = crc;

	fp = fopen(file_path, "w");
	if (!fp || fwrite(data, 1, sizeof(ControlFileData), fp) != sizeof(ControlFileData))
	{
		fprintf(stderr, "Failed to open or write control data\n");
	}
	else
		succeed = true;

	fclose(fp);

	return succeed;
}

static ControlFileData *
read_control_data(char *file_path)
{
	FILE	   *fp = fopen(file_path, "r");
	ControlFileData *data = NULL;
	pg_crc32c	crc;

	if (!fp)
	{
		fprintf(stderr, "Failed to open %s\n", file_path);
		return NULL;
	}

	data = palloc0(sizeof(ControlFileData));

	if (fread(data, 1, sizeof(ControlFileData), fp) != sizeof(ControlFileData))
	{
		fprintf(stderr, "Failed to read ControlFileData\n");
		data = NULL;
	}

	fclose(fp);

	/* Check the CRC. */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc,
				(char *) data,
				offsetof(ControlFileData, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, data->crc))
	{
		fprintf(stderr, "Failed to check crc, expected %X but got %X\n", data->crc, crc);
		pfree(data);
		return NULL;
	}

	/* Make sure the control file is valid byte order. */
	if (data->pg_control_version % 65536 == 0 &&
		data->pg_control_version / 65536 != 0)
		printf("WARNING: possible byte ordering mismatch\n"
			   "The byte ordering used to store the pg_control file might not match the one\n"
			   "used by this program.  In that case the results below would be incorrect, and\n"
			   "the PostgreSQL installation would be incompatible with this data directory.\n");

	return data;
}

int
control_data_change_main(int argc, char **argv)
{
	int			option;
	int			optindex = 0;
	char	   *file_path = NULL;
	char	   *new_file_path = NULL;
	bool		succeed = false;
	ControlFileData *data;
	XLogRecPtr	redo_position = InvalidXLogRecPtr;
	XLogRecPtr	checkpoint_loc = InvalidXLogRecPtr;

	if (argc <= 1)
	{
		usage();
		return -1;
	}

	while ((option = getopt_long(argc, argv, "f:n:p:c:?", long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'f':
				file_path = pg_strdup(optarg);
				break;
			case 'n':
				new_file_path = pg_strdup(optarg);
				break;
			case 'p':
				if (sscanf(optarg, "%lX", &redo_position) != 1)
				{
					fprintf(stderr, "Could not parse redo position\n");
					goto end;
				}
				break;
			case 'c':
				if (sscanf(optarg, "%lX", &checkpoint_loc) != 1)
				{
					fprintf(stderr, "Could not parse checkpoint position\n");
					goto end;
				}
				break;
			case '?':
				usage();
				exit(0);
				break;
			default:
				usage();
				goto end;
		}
	}

	if (!file_path || !new_file_path)
		goto end;

	data = read_control_data(file_path);

	if (!data)
		goto end;

	if (redo_position != InvalidXLogRecPtr)
		data->checkPointCopy.redo = redo_position;
	if (checkpoint_loc != InvalidXLogRecPtr)
		data->checkPoint = checkpoint_loc;

	if (write_control_data(new_file_path, data))
		succeed = true;

end:
	if (file_path)
		free(file_path);

	if (new_file_path)
		free(new_file_path);

	return succeed ? 0 : -1;
}

/*-------------------------------------------------------------------------
 *
 * logindex_meta_dump.c
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
 *	  src/bin/polar_tools/logindex_meta_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_tools.h"
#include "access/polar_logindex.h"
#include "access/polar_logindex_internal.h"

static struct option long_options[] = {
	{"file-path", required_argument, NULL, 'f'},
	{"help", no_argument, NULL, '?'},
	{NULL, 0, NULL, 0}
};

static void
usage(void)
{
	printf("Dump logindex meta usage:\n");
	printf("-f, --file_path  Specify logindex meta file path\n");
	printf("-?, --help show this help, then exit\n");
}

int
logindex_meta_main(int argc, char **argv)
{
	int			option;
	int			optindex = 0;
	char	   *file_path = NULL;
	FILE	   *fp = NULL;
	bool		succeed = false;
	log_index_meta_t meta;
	pg_crc32	crc;

	if (argc <= 1)
	{
		usage();
		return -1;
	}

	while ((option = getopt_long(argc, argv, "f:?", long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'f':
				file_path = pg_strdup(optarg);
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

	if (file_path == NULL)
		goto end;

	fp = fopen(file_path, "r");
	if (fp == NULL)
	{
		fprintf(stderr, "Failed to open %s\n", file_path);
		goto end;
	}

	if (fread(&meta, 1, sizeof(log_index_meta_t), fp) != sizeof(log_index_meta_t))
	{
		fprintf(stderr, "Failed to read logindex meta\n");
		goto end;
	}

	crc = meta.crc;
	if (meta.magic != LOG_INDEX_MAGIC)
		fprintf(stderr, "The magic number of meta file is incorrect, got %d, expect %d",
				meta.magic, LOG_INDEX_MAGIC);

	if (meta.version != LOG_INDEX_VERSION)
		fprintf(stderr, "The version is incorrect, got %d, expect %d", meta.version, LOG_INDEX_VERSION);

	meta.crc = 0;
	meta.crc = log_index_calc_crc((unsigned char *) &meta, sizeof(log_index_meta_t));

	if (crc != meta.crc)
		fprintf(stderr, "The crc is incorrect, got %u, expect %u", crc, meta.crc);

	printf("magic=%x version=%x max_table_id=%ld seg.no=%ld seg.max_lsn=%X/%X seg.max_id=%ld seg.min_id=%ld start_lsn=%X/%X max_lsn=%X/%X crc=%x\n",
		   meta.magic, meta.version, meta.max_idx_table_id, meta.min_segment_info.segment_no,
		   LSN_FORMAT_ARGS(meta.min_segment_info.max_lsn), meta.min_segment_info.max_idx_table_id,
		   meta.min_segment_info.min_idx_table_id, LSN_FORMAT_ARGS(meta.start_lsn), LSN_FORMAT_ARGS(meta.max_lsn), meta.crc);

	succeed = true;
end:
	if (fp)
		fclose(fp);
	if (file_path)
		free(file_path);

	return succeed ? 0 : -1;
}

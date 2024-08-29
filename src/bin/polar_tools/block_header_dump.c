/*-------------------------------------------------------------------------
 *
 * block_header_dump.c
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
 *	  src/bin/polar_tools/block_header_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_tools.h"
#include "storage/bufpage.h"

static struct option long_options[] = {
	{"file-path", required_argument, NULL, 'f'},
	{"block-number", required_argument, NULL, 'b'},
	{"help", no_argument, NULL, '?'},
	{NULL, 0, NULL, 0}
};

static void
usage(void)
{
	printf("Dump block header usage:\n");
	printf("-f, --file_path  Specify the file to be dumped\n");
	printf("-b, --block-number Specify the block number to be dumped\n");
	printf("-?, --help show this help, then exit\n");
}

int
block_header_dump_main(int argc, char **argv)
{

	int			option;
	int			optindex = 0;
	char	   *file_path = NULL;
	int			block = -1;
	FILE	   *fp = NULL;
	PageHeaderData head;
	bool		succeed = false;

	if (argc <= 1)
	{
		usage();
		return -1;
	}

	while ((option = getopt_long(argc, argv, "f:b:?", long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'f':
				file_path = pg_strdup(optarg);
				break;
			case 'b':
				if (sscanf(optarg, "%d", &block) != 1)
				{
					usage();
					return -1;
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

	if (file_path == NULL || block == -1)
		goto end;

	fp = fopen(file_path, "r");

	if (fp == NULL)
	{
		fprintf(stderr, "Failed to open %s\n", file_path);
		goto end;
	}

	if (fseek(fp, block * BLCKSZ, SEEK_SET) != 0)
	{
		fprintf(stderr, "Failed to seek %s block %d\n", file_path, block);
		goto end;
	}

	if (fread(&head, 1, sizeof(PageHeaderData), fp) != sizeof(PageHeaderData))
	{
		fprintf(stderr, "Failed to read PageHeaderData\n");
		goto end;
	}

	printf("lsn=%X/%X, checksum=%d, flags=%d, lower=%d,upper=%d,special=%d,version=%d,xid=%d\n",
		   head.pd_lsn.xlogid, head.pd_lsn.xrecoff, head.pd_checksum, head.pd_flags,
		   head.pd_lower, head.pd_upper, head.pd_special, head.pd_pagesize_version,
		   head.pd_prune_xid);

	succeed = true;

end:
	if (fp)
		fclose(fp);

	if (file_path)
		free(file_path);

	return succeed ? 0 : -1;
}

/*-------------------------------------------------------------------------
 *
 * logindex_table_dump.c
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
 *	  src/bin/polar_tools/logindex_table_dump.c
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
	printf("Dump logindex bloom usage:\n");
	printf("-f, --file_path  Specify logindex table file path\n");
	printf("-?, --help show this help, then exit\n");
}

int
logindex_table_main(int argc, char **argv)
{
	int			option;
	int			optindex = 0;
	char	   *file_path = NULL;
	FILE	   *fp = NULL;
	bool		succeed = false;
	log_idx_table_data_t *table = NULL;
	pg_crc32	crc;
	int			ret;

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

	table = malloc(sizeof(log_idx_table_data_t));
	if (!table)
		goto end;

	if (file_path == NULL)
		goto end;

	fp = fopen(file_path, "r");
	if (fp == NULL)
	{
		fprintf(stderr, "Failed to open %s\n", file_path);
		goto end;
	}

	while ((ret = fread(table, 1, sizeof(log_idx_table_data_t), fp)) == sizeof(log_idx_table_data_t))
	{
		crc = table->crc;
		table->crc = 0;
		table->crc = log_index_calc_crc((unsigned char *) table, sizeof(log_idx_table_data_t));

		if (crc != table->crc)
			fprintf(stderr, "The table crc is incorrect, got %u, expect %u\n", table->crc, crc);

		printf("idx_table_id=%ld min_lsn=%lX max_lsn=%lX prefix_lsn=%X crc=%u last_order=%u\n",
			   table->idx_table_id, table->min_lsn, table->max_lsn, table->prefix_lsn,
			   table->crc, table->last_order);
	}

	if ((ret > 0 && ret != sizeof(log_idx_table_data_t)) || errno != 0)
	{
		fprintf(stderr, "Failed to read logindex table, errno=%d\n", errno);
		goto end;
	}

	succeed = true;

end:
	if (fp)
		fclose(fp);

	if (file_path)
		free(file_path);

	if (table)
		free(table);

	return succeed ? 0 : -1;
}

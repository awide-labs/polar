/*-------------------------------------------------------------------------
 *
 * logindex_bloom_dump.c
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
 *	  src/bin/polar_tools/logindex_bloom_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_tools.h"
#include "access/polar_logindex.h"
#include "access/polar_logindex_internal.h"

static struct option long_options[] = {
	{"file-path", required_argument, NULL, 'p'},
	{"spcnode", required_argument, NULL, 's'},
	{"dbnode", required_argument, NULL, 'd'},
	{"relnode", required_argument, NULL, 'r'},
	{"forknum", required_argument, NULL, 'f'},
	{"blocknum", required_argument, NULL, 'b'},
	{"help", no_argument, NULL, '?'},
	{NULL, 0, NULL, 0}
};

static void
usage(void)
{
	printf("Dump logindex bloom usage:\n");
	printf("-p, --file_path  Specify logindex bloom file path\n");
	printf("-s, --spcnode\n");
	printf("-d, --dbnode\n");
	printf("-r, --relnode\n");
	printf("-f, --forknum\n");
	printf("-b, --blocknum\n");
	printf("-?, --help show this help, then exit\n");
}

int
logindex_bloom_main(int argc, char **argv)
{
	int			option;
	int			optindex = 0;
	char	   *file_path = NULL;
	FILE	   *fp = NULL;
	bool		succeed = false;
	log_file_table_bloom_t *bloom = NULL;
	pg_crc32	crc;
	int			ret;
	BufferTag	tag;

	if (argc <= 1)
	{
		usage();
		return -1;
	}

	CLEAR_BUFFERTAG(tag);

	while ((option = getopt_long(argc, argv, "p:?s:d:r:f:b:", long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'p':
				file_path = pg_strdup(optarg);
				break;
			case 's':
				tag.rnode.spcNode = atoi(optarg);
				break;
			case 'd':
				tag.rnode.dbNode = atoi(optarg);
				break;
			case 'r':
				tag.rnode.relNode = atoi(optarg);
				break;
			case 'f':
				tag.forkNum = atoi(optarg);
				break;
			case 'b':
				tag.blockNum = atoi(optarg);
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

	bloom = malloc(LOG_INDEX_FILE_TBL_BLOOM_SIZE);
	if (!bloom)
		goto end;

	if (file_path == NULL)
		goto end;

	fp = fopen(file_path, "r");
	if (fp == NULL)
	{
		fprintf(stderr, "Failed to open %s\n", file_path);
		goto end;
	}

	while ((ret = fread(bloom, 1, LOG_INDEX_FILE_TBL_BLOOM_SIZE, fp)) == LOG_INDEX_FILE_TBL_BLOOM_SIZE)
	{
		bool		not_exists = true;

		crc = bloom->crc;
		bloom->crc = 0;
		bloom->crc = log_index_calc_crc((unsigned char *) bloom, LOG_INDEX_FILE_TBL_BLOOM_SIZE);

		if (crc != bloom->crc)
			fprintf(stderr, "The bloom crc is incorrect ,got %u, expect %u\n", bloom->crc, crc);
		else if (tag.rnode.spcNode != InvalidOid && bloom->crc != 0)
		{
			bloom_filter *filter;

			filter = polar_bloom_init_struct(bloom->bloom_bytes, bloom->buf_size, LOG_INDEX_BLOOM_ELEMS_NUM, 0);
			not_exists = bloom_lacks_element(filter, (unsigned char *) &tag, sizeof(BufferTag));
		}

		printf("idx_table_id=%ld min_lsn=%lX max_lsn=%lX buf_size=%u crc=%u not_exists=%d\n",
			   bloom->idx_table_id, bloom->min_lsn, bloom->max_lsn, bloom->buf_size, bloom->crc, not_exists);

	}

	if ((ret > 0 && ret != LOG_INDEX_FILE_TBL_BLOOM_SIZE) || errno != 0)
	{
		fprintf(stderr, "Failed to read logindex bloom, errno=%d\n", errno);
		goto end;
	}

	succeed = true;

end:
	if (fp)
		fclose(fp);

	if (file_path)
		free(file_path);

	if (bloom)
		free(bloom);

	return succeed ? 0 : -1;
}

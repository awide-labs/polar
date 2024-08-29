/*-------------------------------------------------------------------------
 *
 * logindex_page_dump.c
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
 *	  src/bin/polar_tools/logindex_page_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "polar_tools.h"
#include "access/polar_logindex.h"
#include "access/polar_logindex_internal.h"


static struct option long_options[] = {
	{"table-path", required_argument, NULL, 'p'},
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
	printf("Dump logindex page usage:\n");
	printf("-p, --table_path  Specify logindex table path\n");
	printf("-s, --spcnode\n");
	printf("-d, --dbnode\n");
	printf("-r, --relnode\n");
	printf("-f, --forknum\n");
	printf("-b, --blocknum\n");
	printf("-?, --help show this help, then exit\n");
}

static char data[LOG_INDEX_TABLE_CACHE_SIZE];


static log_item_head_t *
logindex_mem_tbl_exists_page(BufferTag *tag,
							 log_idx_table_data_t * table, uint32 key)
{
	log_seg_id_t exists = LOG_INDEX_TBL_SLOT_VALUE(table, key);
	log_item_head_t *item;

	item = LOG_INDEX_ITEM_HEAD(table, exists);

	while (item != NULL &&
		   !BUFFERTAGS_EQUAL(item->tag, *tag))
	{
		exists = item->next_item;
		item = LOG_INDEX_ITEM_HEAD(table, exists);
	}

	return item;
}

static void
logindex_search_table(BufferTag *tag, log_idx_table_data_t * table, uint32 key)
{
	log_item_head_t *item_head = logindex_mem_tbl_exists_page(tag, table, key);
	log_seg_id_t item_id;
	log_item_seg_t *item;
	size_t		i;
	XLogRecPtr	lsn;

	if (item_head == NULL)
		return;

	item_id = item_head->tail_seg;

	do
	{
		item = LOG_INDEX_ITEM_SEG(table, item_id);

		if ((void *) item == (void *) item_head)
			break;

		for (i = item->number; i > 0; i--)
		{
			lsn = LOG_INDEX_COMBINE_LSN(table, item->suffix_lsn[i - 1]);
			printf("%lX,", lsn);
		}

		item_id = item->prev_seg;
	} while (true);

	for (i = item_head->number; i > 0; i--)
	{
		lsn = LOG_INDEX_COMBINE_LSN(table, item_head->suffix_lsn[i - 1]);
		printf("%lX,", lsn);
	}

	printf("\n=========================================\n");
}

int
logindex_page_main(int argc, char **argv)
{
	BufferTag	tag;
	char	   *table_path = NULL;
	int			option;
	int			optindex = 0;
	bool		succeed = false;
	FILE	   *fp = NULL;
	size_t		size;
	char	   *table;
	int			offset = 0;
	uint32		key;

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
				table_path = pg_strdup(optarg);
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
			default:
				usage();
				goto end;
		}
	}

	fp = fopen(table_path, "r");

	if (!fp)
		goto end;

	size = fread(data, 1, LOG_INDEX_TABLE_CACHE_SIZE, fp);

	key = hash_any((const unsigned char *) &tag, sizeof(BufferTag)) % LOG_INDEX_MEM_TBL_HASH_NUM;

	while (offset < size)
	{
		table = (data + offset);
		logindex_search_table(&tag, (log_idx_table_data_t *) table, key);
		offset += sizeof(log_idx_table_data_t);
	}

end:
	if (table_path)
		free(table_path);

	if (fp)
		fclose(fp);

	return succeed ? 0 : -1;
}

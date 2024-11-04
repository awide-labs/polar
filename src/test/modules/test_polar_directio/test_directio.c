/*-------------------------------------------------------------------------
 *
 * test_directio.c
 *	  vfs dio test case
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
 *	  src/test/modules/test_polar_directio/test_directio.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "access/xlogdefs.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "polar_vfs/polar_directio.h"
#include "port/pg_iovec.h"
#include "storage/fd.h"
#include "storage/polar_fd.h"
#include "utils/guc.h"

#define TEST_DATA_DIRECTIO "directio.dat"
#define TEST_DATA_BUFFERIO "bufferio.dat"

#define TEST_DATA_MAX_LEN (5 * 1024 * 1024)
#define SUBAPI_LOOP 50
#define MAIN_LOOP 50
#define IOVCNT 3

static char directio_file[MAXPGPATH + 1];
static char bufferio_file[MAXPGPATH + 1];

PG_MODULE_MAGIC;

static void prepare_file_with_length(char *path, ssize_t len);
static void test_lseek_read_work(int directio_fd, int bufferio_fd);
static void test_pread_work(int directio_fd, int bufferio_fd);
static void test_preadv_work(int directio_fd, int bufferio_fd);
static void test_lseek_write_work(int directio_fd, int bufferio_fd);
static void test_pwrite_work(int directio_fd, int bufferio_fd);
static void test_pwritev_work(int directio_fd, int bufferio_fd);
static void test_aligned_buffer_offset_len(int directio_fd, int bufferio_fd);
static void test_checksum(int directio_fd, int bufferio_fd);

typedef void (*test_func) (int directio_fd, int bufferio_fd);

#define MAX_TEST_FUNC_APIS 8
const		test_func test_func_apis[MAX_TEST_FUNC_APIS] =
{
	test_lseek_read_work, test_pread_work, test_lseek_write_work,
	test_pwrite_work, test_pwritev_work, test_preadv_work,
	test_checksum, test_aligned_buffer_offset_len
};

PG_FUNCTION_INFO_V1(test_directio);

Datum
test_directio(PG_FUNCTION_ARGS)
{
	int			i;
	ssize_t		len;
	int			directio_fd;
	int			bufferio_fd;

	srandom((unsigned int) time(NULL));
	snprintf(directio_file, MAXPGPATH, "%s/%s", DataDir, TEST_DATA_DIRECTIO);
	snprintf(bufferio_file, MAXPGPATH, "%s/%s", DataDir, TEST_DATA_BUFFERIO);

	if (polar_directio_buffer == NULL &&
		posix_memalign((void **) &polar_directio_buffer,
					   PG_IO_ALIGN_SIZE,
					   polar_max_direct_io_size) != 0)
		elog(PANIC, "posix_memalign alloc polar_directio_buffer failed!");

	for (i = 0; i < MAIN_LOOP; i++)
	{
		int			j;

		len = random() % TEST_DATA_MAX_LEN + 1;
		prepare_file_with_length(directio_file, len);

		directio_fd = polar_directio_open(directio_file, O_RDWR | PG_O_DIRECT, 0);

		if (directio_fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file with PG_O_DIRECT \"%s\": %m", directio_file)));

		prepare_file_with_length(bufferio_file, len);
		bufferio_fd = open(bufferio_file, O_RDWR, 0);

		if (bufferio_fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", bufferio_file)));

		for (j = 0; j < MAX_TEST_FUNC_APIS; j++)
			test_func_apis[random() % MAX_TEST_FUNC_APIS] (directio_fd, bufferio_fd);

		test_lseek_write_work(directio_fd, bufferio_fd);
		test_lseek_read_work(directio_fd, bufferio_fd);
		test_pwrite_work(directio_fd, bufferio_fd);
		test_pwritev_work(directio_fd, bufferio_fd);
		test_pread_work(directio_fd, bufferio_fd);
		test_preadv_work(directio_fd, bufferio_fd);
		test_checksum(directio_fd, bufferio_fd);

		close(directio_fd);
		close(bufferio_fd);

		Assert(unlink(directio_file) == 0);
		Assert(unlink(bufferio_file) == 0);
	}

	PG_RETURN_VOID();
}

static void
prepare_file_with_length(char *path, ssize_t len)
{
	char	   *buffer;
	int			fd = open(path, O_CREAT | O_RDWR, pg_file_create_mode);

	Assert(fd >= 0);
	/* create file with length */
	buffer = (char *) malloc(len);
	MemSet(buffer, 0x1, len);
	Assert(len == write(fd, buffer, len));
	free(buffer);
	close(fd);
}

static void
test_lseek_write_work(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat directio_st;
	struct stat bufferio_st;
	char	   *buffer = NULL;
	int			i;
	int			len;

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		len = random() % (polar_max_direct_io_size * 4);
		buffer = (char *) malloc(len);
		MemSet(buffer, 0x2, len);
		Assert(0 == polar_stat(directio_file, &directio_st));
		Assert(0 == polar_stat(bufferio_file, &bufferio_st));
		Assert(directio_st.st_size == bufferio_st.st_size);
		offset = random() % directio_st.st_size;
		Assert(offset == lseek(directio_fd, offset, SEEK_SET));
		Assert(offset == lseek(bufferio_fd, offset, SEEK_SET));
		Assert(len == polar_directio_write(directio_fd, buffer, len));
		Assert(len == write(bufferio_fd, buffer, len));
		Assert(lseek(directio_fd, 0, SEEK_CUR) == lseek(bufferio_fd, 0, SEEK_CUR));
		Assert(0 == polar_stat(directio_file, &directio_st));
		Assert(0 == polar_stat(bufferio_file, &bufferio_st));
		Assert(directio_st.st_size == bufferio_st.st_size);
		free(buffer);
	}
}

static void
test_lseek_read_work(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat directio_st;
	struct stat bufferio_st;
	char	   *directio_buffer = NULL;
	char	   *bufferio_buffer = NULL;
	int			i;
	int			len;
	int			directio_res = -1;
	int			bufferio_res = -1;

	Assert(0 == polar_stat(directio_file, &directio_st));
	Assert(0 == polar_stat(bufferio_file, &bufferio_st));
	Assert(directio_st.st_size == bufferio_st.st_size);

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		len = random() % (polar_max_direct_io_size * 4);
		directio_buffer = (char *) malloc(len);
		bufferio_buffer = (char *) malloc(len);
		MemSet(directio_buffer, 0x0, len);
		MemSet(bufferio_buffer, 0x0, len);
		offset = random() % directio_st.st_size;
		Assert(offset == lseek(directio_fd, offset, SEEK_SET));
		Assert(offset == lseek(bufferio_fd, offset, SEEK_SET));
		directio_res = polar_directio_read(directio_fd, directio_buffer, len);
		bufferio_res = read(bufferio_fd, bufferio_buffer, len);
		Assert(directio_res == bufferio_res &&
			   (directio_res == len ||
				directio_res == (directio_st.st_size - offset)));
		Assert(lseek(directio_fd, 0, SEEK_CUR) == lseek(bufferio_fd, 0, SEEK_CUR));
		Assert(0 == memcmp(directio_buffer, bufferio_buffer, len));
		free(directio_buffer);
		free(bufferio_buffer);
	}
}

static void
test_pwrite_work(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat directio_st;
	struct stat bufferio_st;
	char	   *buffer = NULL;
	int			i;
	int			len;
	off_t		directio_cur = lseek(directio_fd, 0, SEEK_CUR);
	off_t		bufferio_cur = lseek(bufferio_fd, 0, SEEK_CUR);

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		len = random() % (polar_max_direct_io_size * 4);
		buffer = (char *) malloc(len);
		MemSet(buffer, 0x3, len);
		Assert(0 == polar_stat(directio_file, &directio_st));
		Assert(0 == polar_stat(bufferio_file, &bufferio_st));
		Assert(directio_st.st_size == bufferio_st.st_size);
		offset = random() % directio_st.st_size;
		Assert(len == polar_directio_pwrite(directio_fd, buffer, len, offset));
		Assert(len == pwrite(bufferio_fd, buffer, len, offset));
		Assert(directio_cur == lseek(directio_fd, 0, SEEK_CUR));
		Assert(bufferio_cur == lseek(bufferio_fd, 0, SEEK_CUR));
		Assert(0 == polar_stat(directio_file, &directio_st));
		Assert(0 == polar_stat(bufferio_file, &bufferio_st));
		Assert(directio_st.st_size == bufferio_st.st_size);
		free(buffer);
	}
}

static void
test_pwritev_work(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat directio_st;
	struct stat bufferio_st;
	int			i,
				j;
	struct iovec iov[PG_IOV_MAX];
	int			sum;
	int			len = 0;
	char	   *buffer[IOVCNT];

	off_t		directio_cur = lseek(directio_fd, 0, SEEK_CUR);
	off_t		bufferio_cur = lseek(bufferio_fd, 0, SEEK_CUR);

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		sum = 0;
		for (j = 0; j < IOVCNT; j++)
		{
			len = random() % (polar_max_direct_io_size * 4);
			buffer[j] = (char *) malloc(len);
			MemSet(buffer[j], 0x3, len);
			iov[j].iov_base = buffer[j];
			iov[j].iov_len = len;
			sum += len;
		}
		Assert(0 == polar_stat(directio_file, &directio_st));
		Assert(0 == polar_stat(bufferio_file, &bufferio_st));
		Assert(directio_st.st_size == bufferio_st.st_size);
		offset = random() % directio_st.st_size;
		Assert(sum == polar_directio_pwritev(directio_fd, iov, IOVCNT, offset));
		Assert(sum == pg_pwritev(bufferio_fd, iov, IOVCNT, offset));
		Assert(directio_cur == lseek(directio_fd, 0, SEEK_CUR));
		Assert(bufferio_cur == lseek(bufferio_fd, 0, SEEK_CUR));
		Assert(0 == polar_stat(directio_file, &directio_st));
		Assert(0 == polar_stat(bufferio_file, &bufferio_st));
		Assert(directio_st.st_size == bufferio_st.st_size);

		for (j = 0; j < IOVCNT; j++)
			free(buffer[j]);
	}
}

static void
test_pread_work(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat directio_st;
	struct stat bufferio_st;
	char	   *directio_buffer = NULL;
	char	   *bufferio_buffer = NULL;
	int			i;
	int			len;
	int			directio_res = -1;
	int			bufferio_res = -1;
	off_t		directio_cur = lseek(directio_fd, 0, SEEK_CUR);
	off_t		bufferio_cur = lseek(bufferio_fd, 0, SEEK_CUR);

	Assert(0 == polar_stat(directio_file, &directio_st));
	Assert(0 == stat(bufferio_file, &bufferio_st));
	Assert(directio_st.st_size == bufferio_st.st_size);

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		len = random() % (polar_max_direct_io_size * 4);
		directio_buffer = (char *) malloc(len);
		bufferio_buffer = (char *) malloc(len);
		MemSet(directio_buffer, 0x0, len);
		MemSet(bufferio_buffer, 0x0, len);
		offset = random() % directio_st.st_size;
		directio_res = polar_directio_pread(directio_fd, directio_buffer, len, offset);
		bufferio_res = pread(bufferio_fd, bufferio_buffer, len, offset);
		Assert(directio_res == bufferio_res &&
			   (directio_res == len ||
				directio_res == (directio_st.st_size - offset)));
		Assert(lseek(directio_fd, 0, SEEK_CUR) == directio_cur);
		Assert(lseek(bufferio_fd, 0, SEEK_CUR) == bufferio_cur);
		Assert(0 == memcmp(directio_buffer, bufferio_buffer, len));
		free(directio_buffer);
		free(bufferio_buffer);
	}
}

static void
test_preadv_work(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat directio_st;
	struct stat bufferio_st;
	int			i,
				j;
	struct iovec iov_dio[PG_IOV_MAX];
	struct iovec iov_bio[PG_IOV_MAX];
	int			directio_res = -1;
	int			bufferio_res = -1;
	int			len = 0;
	int			sum;
	char	   *buffer_dio[IOVCNT];
	char	   *buffer_bio[IOVCNT];

	off_t		directio_cur = lseek(directio_fd, 0, SEEK_CUR);
	off_t		bufferio_cur = lseek(bufferio_fd, 0, SEEK_CUR);

	Assert(0 == polar_stat(directio_file, &directio_st));
	Assert(0 == stat(bufferio_file, &bufferio_st));
	Assert(directio_st.st_size == bufferio_st.st_size);

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		sum = 0;
		for (j = 0; j < IOVCNT; j++)
		{
			len = random() % (polar_max_direct_io_size * 4);
			buffer_dio[j] = (char *) malloc(len);
			buffer_bio[j] = (char *) malloc(len);
			MemSet(buffer_dio[j], 0x0, len);
			MemSet(buffer_bio[j], 0x0, len);
			iov_dio[j].iov_base = buffer_dio[j];
			iov_bio[j].iov_base = buffer_bio[j];
			iov_dio[j].iov_len = len;
			iov_bio[j].iov_len = len;
			sum += len;
		}

		offset = random() % directio_st.st_size;
		directio_res = polar_directio_preadv(directio_fd, iov_dio, IOVCNT, offset);
		bufferio_res = pg_preadv(bufferio_fd, iov_bio, IOVCNT, offset);

		Assert(directio_res == bufferio_res &&
			   (directio_res == sum ||
				directio_res == (directio_st.st_size - offset)));
		Assert(lseek(directio_fd, 0, SEEK_CUR) == directio_cur);
		Assert(lseek(bufferio_fd, 0, SEEK_CUR) == bufferio_cur);

		for (j = 0; j < IOVCNT; j++)
		{
			Assert(0 == memcmp(buffer_dio[j], buffer_bio[j], iov_dio[j].iov_len));
			free(buffer_dio[j]);
			free(buffer_bio[j]);
		}
	}
}

static void
test_checksum(int directio_fd, int bufferio_fd)
{
	struct stat directio_st;
	struct stat bufferio_st;
	FILE	   *directio_fp;
	FILE	   *bufferio_fp;
	char		cmd[1024];
	char		directio_md5[33];
	char		bufferio_md5[33];

	Assert(0 == polar_stat(directio_file, &directio_st));
	Assert(0 == polar_stat(bufferio_file, &bufferio_st));
	Assert(directio_st.st_size == bufferio_st.st_size);
	MemSet(cmd, 0x0, sizeof(cmd));
	snprintf(cmd, sizeof(cmd), "/usr/bin/md5sum %s", directio_file);
	Assert((directio_fp = popen(cmd, "r")) != NULL);
	MemSet(directio_md5, 0x0, sizeof(directio_md5));
	Assert(fgets(directio_md5, sizeof(directio_md5) - 1, directio_fp) != NULL);
	pclose(directio_fp);

	MemSet(cmd, 0x0, sizeof(cmd));
	snprintf(cmd, sizeof(cmd), "/usr/bin/md5sum %s", bufferio_file);
	Assert((bufferio_fp = popen(cmd, "r")) != NULL);
	MemSet(bufferio_md5, 0x0, sizeof(bufferio_md5));
	Assert(fgets(bufferio_md5, sizeof(bufferio_md5) - 1, bufferio_fp) != NULL);
	pclose(bufferio_fp);

	Assert(0 == memcmp(directio_md5, bufferio_md5, 32));
	elog(LOG, "directio_md5 is %s, bufferio_md5 is %s.", directio_md5, bufferio_md5);
}

static void
test_aligned_buffer_offset_len(int directio_fd, int bufferio_fd)
{
	off_t		offset;
	struct stat stat_buf;
	char	   *buffer = NULL;
	int			i;
	int			len;

	for (i = 0; i < SUBAPI_LOOP; i++)
	{
		len = POLAR_DIRECTIO_ALIGN(random() % polar_max_direct_io_size);
		Assert(0 == posix_memalign((void **) &buffer, PG_IO_ALIGN_SIZE, len));
		MemSet(buffer, 0x4, len);
		Assert(0 == polar_stat(directio_file, &stat_buf));
		offset = POLAR_DIRECTIO_ALIGN_DOWN(random() % stat_buf.st_size);
		Assert(offset == lseek(directio_fd, offset, SEEK_SET));
		Assert(offset == lseek(bufferio_fd, offset, SEEK_SET));
		Assert(len == polar_directio_write(directio_fd, buffer, len));
		Assert(len == write(bufferio_fd, buffer, len));
		offset = POLAR_DIRECTIO_ALIGN_DOWN(random() % stat_buf.st_size);
		Assert(offset == lseek(directio_fd, offset, SEEK_SET));
		Assert(offset == lseek(bufferio_fd, offset, SEEK_SET));
		Assert(polar_directio_read(directio_fd, buffer, len) ==
			   read(bufferio_fd, buffer, len));
		free(buffer);
		test_checksum(directio_fd, bufferio_fd);
	}
}

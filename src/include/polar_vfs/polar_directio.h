/*-------------------------------------------------------------------------
 *
 * polar_directio.h
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
 *	  src/include/polar_vfs/polar_directio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_DIRECTIO_H
#define POLAR_DIRECTIO_H

#include <unistd.h>
#include <sys/time.h>
#include "postgres.h"
#include "access/xlogdefs.h"
#ifndef FRONTEND
#include "funcapi.h"
#endif

#include "storage/fd.h"
#include "storage/polar_fd.h"


#define POLAR_DIRECTIO_MIN_IOSIZE       (4 * 1024)
#define POLAR_DIRECTIO_DEFAULT_IOSIZE   (1 * 1024 * 1024)
#define POLAR_DIRECTIO_MAX_IOSIZE       (128 * 1024 * 1024)

extern int	polar_max_direct_io_size;
extern char *polar_directio_buffer;
extern const vfs_mgr polar_vfs_dio;

#define POLAR_ACCESS_MODE_MASK      0x3
#define POLAR_DIRECTIO_ALIGN_DOWN(LEN)  TYPEALIGN_DOWN(PG_IO_ALIGN_SIZE, LEN)
#define POLAR_DIRECTIO_ALIGN(LEN)       TYPEALIGN(PG_IO_ALIGN_SIZE, LEN)
#define POLAR_DIRECTIO_IS_ALIGNED(LEN)  !((uintptr_t)(LEN) & (uintptr_t)(PG_IO_ALIGN_SIZE - 1))

extern int	polar_directio_open(const char *path, int flags, mode_t mode);
extern ssize_t polar_directio_read(int fd, void *buf, size_t len);
extern ssize_t polar_directio_pread(int fd, void *buffer, size_t len, off_t offset);
extern ssize_t polar_directio_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
extern ssize_t polar_directio_write(int fd, const void *buf, size_t len);
extern ssize_t polar_directio_pwrite(int fd, const void *buffer, size_t len, off_t offset);
extern ssize_t polar_directio_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);

#endif							/* POLAR_DIRECTIO_H */

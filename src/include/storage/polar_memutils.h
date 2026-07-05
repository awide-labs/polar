/*-------------------------------------------------------------------------
 *
 * polar_memutils.h
 *
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
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
 *  src/include/storage/polar_memutils.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_MEMUTILS_H
#define POLAR_MEMUTILS_H

#include "port/atomics.h"
#include "storage/s_lock.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/palloc.h"

#define FREELIST_COUNT				2
#define POLAR_SHM_ASET_MAGIC		0xFEDC1234

extern bool polar_enable_shm_aset;
#define	polar_shm_aset_enabled()	(polar_enable_shm_aset == true)

#define dsaptr_to_rawptr(dsa_p, base_p)	\
    ((char *)(base_p) + (dsa_p))
#define rawptr_to_dsaptr(raw_p, base_p)	\
	((dsa_pointer)((char *)(raw_p) - (char *)(base_p)))

typedef struct ShmAllocSetContext ShmAllocSetContext;

typedef enum
{
	SHM_ASET_TYPE_GPC = 0,
	SHM_ASET_TYPE_SHARED_SERVER,
	/* Other types */
	SHM_ASET_TYPE_NUM
} ShmAsetType;

typedef struct ShmAsetFreeList
{
	slock_t		lock;
	int			num_free;		/* current list length */
	ShmAllocSetContext *first_free; /* list header */
} ShmAsetFreeList;

typedef struct ShmAsetCtl
{
	uint32		magic;
	ShmAsetType type;
	Size		size;
	void	   *base;
	ShmAsetFreeList context_freelists[FREELIST_COUNT];
} ShmAsetCtl;

extern ShmAsetCtl *shm_aset_ctl;
extern dsa_area *local_area[SHM_ASET_TYPE_NUM];

#define polar_shm_aset_set_area(type, area)	(local_area[(type)] = area)
#define polar_shm_aset_get_area(type)	(local_area[(type)])

/* POLAR: polar_shm_aset.c */
extern Size polar_shm_aset_ctl_size(void);
extern void polar_shm_aset_ctl_init(void);
extern MemoryContext ShmAllocSetContextCreate(MemoryContext parent,
											  const char *name,
											  Size minContextSize,
											  Size initBlockSize,
											  Size maxBlockSize,
											  ShmAsetCtl *ctl);
#endif

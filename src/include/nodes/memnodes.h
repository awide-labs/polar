/*-------------------------------------------------------------------------
 *
 * memnodes.h
 *	  POSTGRES memory context node definitions.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/memnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMNODES_H
#define MEMNODES_H

#include "nodes/nodes.h"

typedef struct DSALockStat
{
	uint64		lock_area_count;
	uint64		lock_area_time_us;
	uint64		lock_area_max_time_us;
	uint64		lock_sclass_count;
	uint64		lock_sclass_time_us;
	uint64		lock_sclass_max_time_us;
	uint64		lock_freelist_count;
	uint64		lock_freelist_time_us;
	uint64		lock_freelist_max_time_us;
} DSALockStat;

/*
 * MemoryContextCounters
 *		Summarization state for MemoryContextStats collection.
 *
 * The set of counters in this struct is biased towards AllocSet; if we ever
 * add any context types that are based on fundamentally different approaches,
 * we might need more or different counters here.  A possible API spec then
 * would be to print only nonzero counters, but for now we just summarize in
 * the format historically used by AllocSet.
 */
typedef struct MemoryContextCounters
{
	Size		nblocks;		/* Total number of malloc blocks */
	Size		freechunks;		/* Total number of free chunks */
	Size		totalspace;		/* Total bytes requested from malloc */
	Size		freespace;		/* The unused portion of totalspace */
} MemoryContextCounters;

/* POLAR */
typedef struct DSAContextCounters
{
	Size		total_segment_size;
	Size		max_total_segment_size;
	int			refcnt;
	bool		pinned;
	Size		usable_pages;
	Size		max_contiguous_pages;
} DSAContextCounters;

/*
 * MemoryContext
 *		A logical context in which memory allocations occur.
 *
 * MemoryContext itself is an abstract type that can have multiple
 * implementations.
 * The function pointers in MemoryContextMethods define one specific
 * implementation of MemoryContext --- they are a virtual function table
 * in C++ terms.
 *
 * Node types that are actual implementations of memory contexts must
 * begin with the same fields as MemoryContextData.
 *
 * Note: for largely historical reasons, typedef MemoryContext is a pointer
 * to the context struct rather than the struct type itself.
 */

typedef void (*MemoryStatsPrintFunc) (MemoryContext context, void *passthru,
									  const char *stats_string,
									  bool print_to_stderr);

typedef struct MemoryContextMethods
{
	void	   *(*alloc) (MemoryContext context, Size size);
	/* call this free_p in case someone #define's free() */
	void		(*free_p) (MemoryContext context, void *pointer);
	void	   *(*realloc) (MemoryContext context, void *pointer, Size size);
	void		(*reset) (MemoryContext context);
	void		(*delete_context) (MemoryContext context);
	Size		(*get_chunk_space) (MemoryContext context, void *pointer);
	bool		(*is_empty) (MemoryContext context);
	void		(*stats) (MemoryContext context,
						  MemoryStatsPrintFunc printfunc, void *passthru,
						  MemoryContextCounters *totals,
						  bool print_to_stderr);
#ifdef MEMORY_CONTEXT_CHECKING
	void		(*check) (MemoryContext context);
#endif
	/* POLAR px */
	void		(*declare_accounting_root) (MemoryContext context);
	Size		(*get_peak_usage) (MemoryContext context);
	Size		(*malloc_usable_size) (MemoryContext context, void *pointer);
	/* POLAR end */
} MemoryContextMethods;

/* POLAR: Shared Server - fallback data for shared memory contexts */
typedef struct MemoryContextFallbackData
{
	const char *parent_name;	/* context name (just for debugging) */
	const char *parent_ident;	/* context ID if any (just for debugging) */
	Size		minContextSize;
	Size		initBlockSize;
	Size		maxBlockSize;
	MemoryContext mcxt;
} MemoryContextFallbackData;

typedef struct MemoryContextData
{
	NodeTag		type;			/* identifies exact kind of context */
	/* these two fields are placed here to minimize alignment wastage: */
	bool		isReset;		/* T = no space alloced since last reset */
	bool		allowInCritSection; /* allow palloc in critical section */
	Size		mem_allocated;	/* track memory allocated for this context */
	const MemoryContextMethods *methods;	/* virtual function table */
	MemoryContext parent;		/* NULL if no parent (toplevel context) */
	MemoryContext firstchild;	/* head of linked list of children */
	MemoryContext prevchild;	/* previous child of same parent */
	MemoryContext nextchild;	/* next child of same parent */
	const char *name;			/* context name (just for debugging) */
	const char *ident;			/* context ID if any (just for debugging) */
	MemoryContextCallback *reset_cbs;	/* list of reset/delete callbacks */
	MemoryContextFallbackData fallback; /* POLAR: Shared Server */
} MemoryContextData;

/* utils/palloc.h contains typedef struct MemoryContextData *MemoryContext */


/*
 * MemoryContextIsValid
 *		True iff memory context is valid.
 *
 * Add new context types to the set accepted by this macro.
 */
#define MemoryContextIsValid(context)                                         \
	((context) != NULL && \
	 (IsA((context), AllocSetContext) || \
	  IsA((context), SlabContext) || \
	  IsA((context), GenerationContext) || \
	  IsA((context), AlignedAllocRedirectContext) || \
	  IsA((context), ShmAllocSetContext)))	/* POLAR: Shared Server */

#endif							/* MEMNODES_H */

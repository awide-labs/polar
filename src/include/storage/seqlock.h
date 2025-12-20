/*-------------------------------------------------------------------------
 *
 * seqlock.h
 *	  Lightweight sequence lock (seqlock) implementation for PostgreSQL
 *
 * This file provides a simple seqlock primitive using PostgreSQL's
 * atomic operations (pg_atomic_uint64). Seqlocks allow multiple
 * concurrent readers to access shared data without blocking,
 * while writers serialize updates using an incrementing sequence counter.
 *
 * Readers retry if a write is in progress, ensuring they always
 * see a consistent snapshot of the data. Writers must still
 * serialize among themselves (e.g., using a spinlock or LWLock)
 * to avoid concurrent writes.
 *
 * Usage:
 *    pg_seqlock mylock;
 *    pg_seqlock_init(&mylock);
 *
 *    // Writer:
 *    pg_seqlock_write_begin(&mylock);
 *    ... modify shared data ...
 *    pg_seqlock_write_end(&mylock);
 *
 *    // Reader:
 *    uint64 seq = pg_seqlock_read_begin(&mylock);
 *    ... read shared data ...
 *    if (pg_seqlock_read_retry(&mylock, seq)) retry;
 *
 *-------------------------------------------------------------------------
 */

#ifndef SEQLOCK_H
#define SEQLOCK_H

#include "port/atomics.h"
#include "c.h"

typedef struct {
	pg_atomic_uint64 seq;
} pg_seqlock;

static inline void pg_seqlock_init(pg_seqlock *lock) {
	pg_atomic_init_u64(&lock->seq, 0);
}

static inline void pg_seqlock_write_begin(pg_seqlock *lock) {
	pg_atomic_fetch_add_u64(&lock->seq, 1);
}

static inline void pg_seqlock_write_end(pg_seqlock *lock) {
	pg_atomic_fetch_add_u64(&lock->seq, 1);
}

static inline uint64 pg_seqlock_read_begin(pg_seqlock *lock) {
	uint64 seq;
	for (;;) {
		seq = pg_atomic_read_u64(&lock->seq);
		if ((seq & 1) == 0) {
			pg_read_barrier();
			return seq;
		}
		/* busy-wait */
		pg_spin_delay();
	}
}

static inline bool pg_seqlock_read_retry(pg_seqlock *lock, uint64 startseq) {
	pg_read_barrier();
	return (startseq != pg_atomic_read_u64(&lock->seq)) || (startseq & 1);
}

#endif

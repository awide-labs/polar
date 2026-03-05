/*
 * pg_bulkload: include/pg_bulkload.h
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 */
#ifndef BULKLOAD_H_INCLUDED
#define BULKLOAD_H_INCLUDED

#include "postgres.h"

#undef ENABLE_GSS
#undef USE_SSL

#if PG_VERSION_NUM < 80300
#error pg_bulkload does not support PostgreSQL 8.2 or earlier versions.
#endif

#include "access/htup.h"
#include "access/tupdesc.h"
#include "fmgr.h"

/**
 * @file
 * @brief General definition in pg_bulkload.
 */

/**
 * @brief Callback type for WHERE-clause predicate evaluation.
 *
 * Invoked by @c ReaderNext() for each successfully parsed tuple.  Returning
 * @c false causes the tuple to be skipped (not passed to the writer).
 *
 * @param tuple    The fully-formed, coerced, constraint-checked HeapTuple.
 * @param tupdesc  Tuple descriptor of @p tuple (from the target relation).
 * @param state    Opaque caller-supplied state (e.g. compiled ExprState).
 * @return @c true if the tuple satisfies the predicate, @c false to skip it.
 */
typedef bool (*WherePredicateFn)(HeapTuple tuple, TupleDesc tupdesc, void *state);

/**
 * @brief Run the pg_bulkload pipeline (parse options, load, finalize).
 *
 * @param fcinfo           Call information (argument 0 = @c text[] options).
 * @param where_predicate  Per-tuple WHERE callback, or NULL for no filtering.
 *                         When set, tuples for which the callback returns
 *                         @c false are skipped inside @c ReaderNext().
 * @param where_state      Opaque state forwarded to @p where_predicate.
 *
 * @return Datum of the result composite row (skip, count, parse_errors, etc.).
 */
PGDLLEXPORT Datum pg_bulkload_run(FunctionCallInfo fcinfo,
								  WherePredicateFn where_predicate,
								  void *where_state);

/*-------------------------------------------------------------------------
 *
 * Parser -> Writer
 * |              |
 * +--------------+
 *      Loader
 *
 * Source:
 *  - FileSource   : from local file
 *  - RemoteSource : from remote client using copy protocol
 *
 * Parser:
 *  - BinaryParser : known as FixedParser before
 *  - CSVParser    : csv file
 *  - TupleParser  : almost noop
 *
 * Writer:
 *  - BufferedWriter : to file using shared buffers
 *  - DirectLoader   : to file using local buffers
 *  - ParallelWriter : to another process
 *
 *-------------------------------------------------------------------------
 */
typedef struct Source	Source;
typedef struct Parser	Parser;
typedef struct Writer	Writer;
typedef struct Reader	Reader;

typedef enum ON_DUPLICATE
{
	ON_DUPLICATE_KEEP_NEW,
	ON_DUPLICATE_KEEP_OLD
} ON_DUPLICATE;

extern const char *ON_DUPLICATE_NAMES[2];

typedef Parser *(*ParserCreate)(void);

#define PG_BULKLOAD_COLS	8

/*
 * 64bit integer utils
 */

#ifndef INT64_MAX
#ifdef LLONG_MAX
#define INT64_MAX	LLONG_MAX
#else
#define INT64_MAX	INT64CONST(0x7FFFFFFFFFFFFFFF)
#endif
#endif

#if SIZEOF_LONG == 8
#define int64_FMT		"%ld"
#else
#define int64_FMT		"%lld"
#endif

#if !defined(__GNUC__) || (__GNUC__ == 2 && __GNUC_MINOR__ < 96)
#define __builtin_expect(x, expected_value) (x)
#endif

#ifndef likely
#define likely(x)   __builtin_expect((x),1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((x),0)
#endif

/*
 * True when pg_bulkload() is invoked from the backend's COPY FROM ... DIRECT
 * path (CallPgBulkload).  Set by the backend via COPY_FROM option; readable by
 * extension code.
 */
extern bool copy_from;

#endif   /* BULKLOAD_H_INCLUDED */

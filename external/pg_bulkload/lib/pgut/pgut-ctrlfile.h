/*-------------------------------------------------------------------------
 *
 * pgut-ctrlfile.h
 *
 * Copyright (c) 2009-2026, NTT, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGUT_CTRLFILE_H
#define PGUT_CTRLFILE_H

#include "c.h"

#ifndef PGUT_EXPORT
#define PGUT_EXPORT
#endif

/**
 * @brief Parse a single `KEY=VALUE` line from a pg_bulkload control file.
 *
 * This function extracts `keyword` and `value` from a line formatted as
 * `KEY=VALUE`. It trims leading/trailing whitespace, ignores comments
 * starting with `#` outside of quoted strings, and supports quoted string
 * values with `"` and escape sequences using `\`.
 *
 * @param buf [in/out] Input line buffer. The function may modify the
 *	memory in-place (e.g. it truncates at newline/comment boundaries,
 *	strips whitespace, unquotes the value). On success, `outKeyword` and
 *	`outValue` point into this buffer.
 * @param outKeyword [out] Output pointer to the parsed keyword string.
 * @param outValue [out] Output pointer to the parsed (possibly unquoted)
 *	value string.
 *
 * @return true when a non-empty `KEY=VALUE` pair was parsed; false when
 *	the line is empty or comment-only after trimming; errors out on invalid
 *	syntax (e.g. missing `=` outside quotes, empty keyword/value, or
 *	unterminated quotes).
 */
extern bool PGUT_EXPORT ParseControlFileLine(char buf[], char **outKeyword, char **outValue);

#endif							/* PGUT_CTRLFILE_H */

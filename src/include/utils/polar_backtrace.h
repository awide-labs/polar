/*-------------------------------------------------------------------------
 *
 * polar_backtrace.h
 *	  Print callstack of the current process
 *
 * Copyright (c) 2024, Alibaba Group Holding Limited
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
 *	  src/include/utils/polar_backtrace.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_BACKTRACE_H
#define POLAR_BACKTRACE_H

#ifdef USE_LIBUNWIND

#include "postgres.h"

#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/elog.h"
#include "utils/guc.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#define POLAR_DUMP_BACKTRACE() \
	{ \
		int			n = 0; \
		unw_context_t context; \
		unw_cursor_t cursor; \
		int			ret; \
		if ((ret = unw_getcontext(&context)) != 0) \
		{ \
			write_stderr("unw_getcontext failed: %d", ret); \
			goto POLAR_DUMP_BACKTRACE_END; \
		} \
		if ((ret = unw_init_local(&cursor, &context)) != 0) \
		{ \
			write_stderr("unw_getcontext failed: %d", ret); \
			goto POLAR_DUMP_BACKTRACE_END; \
		} \
		write_stderr("Process Info:\n"); \
		write_stderr("  pid:\t%d\n", MyProcPid); \
		write_stderr("  type:\t%s\n", GetBackendTypeDesc(MyBackendType)); \
		write_stderr("  sql:\t%s\n", debug_query_string ? debug_query_string : "<NULL>"); \
		write_stderr("Backtrace:\n"); \
		do \
		{ \
			unw_word_t	ip, \
						off; \
			char symbol[256] = "<unknown>"; \
			/* do not unwind too deeply... */ \
			if (n > 100) \
				break; \
			unw_get_reg(&cursor, UNW_REG_IP, &ip); \
			unw_get_proc_name(&cursor, symbol, sizeof(symbol), &off); \
			/* just print it to stderr, and it will be in the error log */ \
			write_stderr("  #%-2d %s+0x%llx [0x%llx]\n", \
						 n, \
						 symbol, \
						 (unsigned long long) off, \
						 (unsigned long long) ip); \
			n++; \
		} while ((ret = unw_step(&cursor)) > 0); \
		if (ret < 0) \
			write_stderr("unw_step failed: %d\n", ret); \
POLAR_DUMP_BACKTRACE_END: \
		Assert(true); \
	}

/*
 * POLAR: ignore coredump events
 *
 * If the name/address of C function triggers the signal/PANIC is in the list of
 * GUC polar_ignore_coredump_functions, the coredump will be ignored.
 *
 * If polar_ignore_coredump_fuzzy_match is on, it will backtrace the whole stack
 * to find such function. If it matches, the coredump will be ignored.
 *
 * num_skip specifies how many inner frames to skip. Use this to locate the
 * right stack where the actual coredump signal/PANIC happens.
 */
#define POLAR_IGNORE_COREDUMP_BACKTRACE(signal, num_skip) \
	{ \
		int			n = 0; \
		unw_context_t context; \
		unw_cursor_t cursor; \
		int			ret; \
		if (!polar_ignore_coredump_function_list) \
			goto POLAR_IGNORE_COREDUMP_BACKTRACE_END; \
		if ((ret = unw_getcontext(&context)) != 0) \
		{ \
			write_stderr("unw_getcontext failed: %d", ret); \
			goto POLAR_IGNORE_COREDUMP_BACKTRACE_END; \
		} \
		if ((ret = unw_init_local(&cursor, &context)) != 0) \
		{ \
			write_stderr("unw_getcontext failed: %d", ret); \
			goto POLAR_IGNORE_COREDUMP_BACKTRACE_END; \
		} \
		do \
		{ \
			unw_word_t	ip, \
						off; \
			char symbol[256] = "<unknown>"; \
			char		ip_str[256]; \
			char		ip_longstr[256]; \
			if (n < num_skip - 1) \
			{ \
				n++; \
				continue; \
			} \
			/* If polar_ignore_coredump_fuzzy_match is off, match the first stack */ \
			if (!polar_ignore_coredump_fuzzy_match && n >= num_skip) \
				break; \
			/* do not unwind too deeply... */ \
			if (n > 100) \
				break; \
			unw_get_reg(&cursor, UNW_REG_IP, &ip); \
			unw_get_proc_name(&cursor, symbol, sizeof(symbol), &off); \
			snprintf(ip_str, 256, "0x%llx", (unsigned long long) ip); \
			snprintf(ip_longstr, 256, "0x%016llx", (unsigned long long) ip); \
			/* C function symbol/short-address/long-address are all acceptable */ \
			if (matches_backtrace_functions(polar_ignore_coredump_function_list, symbol) || \
				matches_backtrace_functions(polar_ignore_coredump_function_list, ip_str) || \
				matches_backtrace_functions(polar_ignore_coredump_function_list, ip_longstr)) \
			{ \
				/* restore coredump signal handler */ \
				polar_set_program_error_handler(0); \
				PG_SETMASK(&UnBlockSig); \
				ereport(polar_ignore_coredump_level, \
						(errbacktrace(), \
						 errmsg("Ignore coredump signal(%d) in process(%d)", \
								signal, MyProcPid))); \
				goto POLAR_IGNORE_COREDUMP_BACKTRACE_END; \
			} \
			n++; \
		} while ((ret = unw_step(&cursor)) > 0); \
		if (ret < 0) \
			write_stderr("unw_step failed: %d\n", ret); \
POLAR_IGNORE_COREDUMP_BACKTRACE_END: \
		Assert(true); \
	}

#endif

#endif							/* POLAR_BACKTRACE_H */

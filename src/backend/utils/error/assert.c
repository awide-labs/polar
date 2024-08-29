/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert support code.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/assert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

/* POLAR */
#include "utils/guc.h"

#include <unistd.h>
#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "utils/polar_backtrace.h"

/* POLAR: GUC */
int			polar_release_assert_level;

/*
 * Options for release assert level.
 */
const struct config_enum_entry polar_release_assert_level_options[] = {
	{"none", POLAR_RELEASE_ASSERT_L_NONE, false},
	{"log", POLAR_RELEASE_ASSERT_L_LOG, false},
	{"coredump", POLAR_RELEASE_ASSERT_L_COREDUMP, false},
	{NULL, 0, false}
};

static void record_error_message(const char *conditionName,
								 const char *errorType,
								 const char *fileName,
								 int lineNumber);

/*
 * polar_exceptional_condition - Handles the failure of release assert.
 *
 * The difference between this function and `ExceptionalCondition()` is
 * that this function is not marked as `pg_attribute_noreturn()`.
 * So this function may return to keep the program running, when the
 * release assert level is lower than `POLAR_RELEASE_ASSERT_L_COREDUMP`.
 */
void
polar_exceptional_condition(const char *conditionName,
							const char *errorType,
							const char *fileName,
							int lineNumber)
{
	record_error_message(conditionName, errorType, fileName, lineNumber);

	if (polar_release_assert_level == POLAR_RELEASE_ASSERT_L_COREDUMP)
		abort();
}

/*
 * ExceptionalCondition - Handles the failure of an Assert()
 *
 * We intentionally do not go through elog() here, on the grounds of
 * wanting to minimize the amount of infrastructure that has to be
 * working to report an assertion failure.
 */
void
ExceptionalCondition(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int lineNumber)
{
	record_error_message(conditionName, errorType, fileName, lineNumber);
	abort();
}

void
record_error_message(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int lineNumber)
{
	/* Report the failure on stderr (or local equivalent) */
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName)
		|| !PointerIsValid(errorType))
		write_stderr("TRAP: ExceptionalCondition: bad arguments in PID %d\n",
					 (int) getpid());
	else
		write_stderr("TRAP: %s(\"%s\", File: \"%s\", Line: %d, PID: %d)\n",
					 errorType, conditionName,
					 fileName, lineNumber, (int) getpid());

	/* Usually this shouldn't be needed, but make sure the msg went out */
	fflush(stderr);

	/* If we have support for it, dump a simple backtrace */
#ifdef USE_LIBUNWIND
	POLAR_DUMP_BACKTRACE();
	polar_reset_program_error_handler();
#elif defined(HAVE_BACKTRACE_SYMBOLS)
	{
		void	   *buf[100];
		int			nframes;

		nframes = backtrace(buf, lengthof(buf));
		backtrace_symbols_fd(buf, nframes, fileno(stderr));
	}
#endif

	/*
	 * If configured to do so, sleep indefinitely to allow user to attach a
	 * debugger.  It would be nice to use pg_usleep() here, but that can sleep
	 * at most 2G usec or ~33 minutes, which seems too short.
	 */
#ifdef SLEEP_ON_ASSERT
	sleep(1000000);
#endif
}

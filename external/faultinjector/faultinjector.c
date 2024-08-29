/*
 * faultinjector.c
 *
 * SQL interface to inject a pre-defined fault in backend code.
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "libpq-fe.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"

PG_MODULE_MAGIC;

extern Datum inject_fault(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(inject_fault);

/*
 * SQL UDF to inject a fault by associating an action against it.  See
 * the accompanying README for more details.
 */
Datum
inject_fault(PG_FUNCTION_ARGS)
{
	char	   *faultName = !PG_ARGISNULL(0) ? TextDatumGetCString(PG_GETARG_DATUM(0)) : "";
	char	   *type = !PG_ARGISNULL(1) ? TextDatumGetCString(PG_GETARG_DATUM(1)) : "";
	char	   *databaseName = !PG_ARGISNULL(2) ? TextDatumGetCString(PG_GETARG_DATUM(2)) : "";
	char	   *tableName = !PG_ARGISNULL(3) ? TextDatumGetCString(PG_GETARG_DATUM(3)) : "";
	int			startOccurrence = PG_GETARG_INT32(4);
	int			endOccurrence = PG_GETARG_INT32(5);
	int			extraArg = PG_GETARG_INT32(6);
	char	   *response;

	response = InjectFault(
						   faultName, type, databaseName, tableName,
						   startOccurrence, endOccurrence, extraArg);
	if (!response)
		elog(ERROR, "failed to inject fault");
	if (strncmp(response, "Success:", strlen("Success:")) != 0)
		elog(ERROR, "%s", response);
	PG_RETURN_TEXT_P(cstring_to_text(response));
}


/*
 * polar_sql_mapping.h
 *		Headers for polar_sql_mapping extension.
 *
 * IDENTIFICATION
 *	  external/polar_sql_mapping/polar_sql_mappin.h
 */
#ifndef POLAR_SQL_MAPPING_H__
#define POLAR_SQL_MAPPING_H__

#include <postgres.h>


/* polar_sql_mapping.c */
extern int	log_usage;
extern int	psm_max_num;		/* max sqls to record */
extern bool record_error_sql;	/* T-enable record error sql */
extern char *unexpected_error_catagory; /* contain the unexpected error type */
extern char *error_pattern;		/* define the pattern of error sqls */

extern shmem_startup_hook_type prev_shmem_startup_hook;
extern polar_record_error_sql_hook_type prev_record_error_sql_hook;

/* polar_error_detective.c  */
extern Size psmss_memsize(void);
extern void psm_shmem_startup(void);
extern void psm_record_error_sql(ErrorData *edata);
extern void psm_insert_error_sql(const char *sql_text, const char *emessage);
extern void psm_entry_reset(void);
extern void psm_sql_mapping_error_internal(FunctionCallInfo fcinfo);

#endif

/*-------------------------------------------------------------------------
 *
 * ps_status.h
 *
 * Declarations for backend/utils/misc/ps_status.c
 *
 * src/include/utils/ps_status.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PS_STATUS_H
#define PS_STATUS_H

extern PGDLLIMPORT bool update_process_title;

extern char **save_ps_display_args(int argc, char **argv);

extern void init_ps_display(const char *fixed_part);

extern void set_ps_display(const char *activity);

extern void polar_ss_init_ps_display(const char *username, const char *dbname,
									 const uint32 polar_startup_gucs_hash,
									 const char *host_info, const char *initial_str);

extern const char *get_ps_display(int *displen);

#endif							/* PS_STATUS_H */

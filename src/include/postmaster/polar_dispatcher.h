/*-------------------------------------------------------------------------
 *
 * polar_dispatcher.h
 *	  Exports from postmaster/polar_dispatcher.c.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/polar_dispatcher.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POLAR_DISPATCHER_H
#define _POLAR_DISPATCHER_H

#include "storage/sinval.h"
#include "storage/s_lock.h"
#include "catalog/namespace.h"
#include "common/ip.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/polar_double_linked_list.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#include "postmaster/postmaster.h"
#include "postmaster/fork_process.h"
#include "protothread/pt.h"
#include "access/htup_details.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "tcop/tcopprot.h"
#include "utils/timeout.h"
#include "utils/ps_status.h"


#define  POLAR_DISPATCHER_MAX_COUNT	64
/* MaxPolarDispatcher is now a global variable in miscadmin.h */

extern bool am_px_worker;
extern int	polar_ss_dispatcher_count;

typedef struct PolarSessionContext PolarSessionContext;
typedef struct PolarDispatcher PolarDispatcher;

typedef struct PolarDbRoleSetting
{
	uint32		database_name_hash;
	uint32		role_name_hash;
	uint32		version;
} PolarDbRoleSetting;

typedef struct PolarDbRoleSettingArray
{
	LWLockPadded db_role_setting_lock;
	uint32		used_count;
	uint32		max_count;
	PolarDbRoleSetting setting[FLEXIBLE_ARRAY_MEMBER];
} PolarDbRoleSettingArray;

/*
 * Each backend has a PolarDispatcherProc struct in shared memory.
 * Information in share dmemory about dispatcher state (used for session scheduling and monitoring)
 */
typedef struct PolarDispatcherProc
{
	int			pipes[2];		/* 0 for postmaster read/write, 1 for
								 * dispatcher read/write. */
	int			id;				/* proxy worker id */
	volatile int pid;			/* proxy worker pid */
	int			n_clients;		/* total number of clients */
	int			n_ssl_clients;	/* number of clients using SSL connection */
	int			n_idle_clients; /* number of idle clients */
	int			n_pending_clients;	/* number of idle clients */
	int			n_signalling_clients;	/* number of idle clients */
	int			n_startup_clients;	/* Number of accepted, but not yet
									 * established connections (startup packet
									 * is not received and db/role are not
									 * known) */
	int			n_pools;		/* nubmer of dbname/role combinations */
	int			n_backends;		/* totatal number of launched backends */
	int			n_idle_backends;	/* number of idle backends */
	int			n_dedicated_backends;	/* number of tainted backends */
	uint64		tx_bytes;		/* amount of data sent to client */
	uint64		rx_bytes;		/* amount of data send to server */
	uint64		n_transactions; /* total number of proroceeded transactions */

	slock_t		signalling_session_lock;	/* for canceling_clients */
	Bitmapset  *signalling_sessions;
} PolarDispatcherProc;

extern PGDLLIMPORT int polar_my_dispatcher_id;
extern PolarDispatcherProc *polar_dispatcher_proc;
extern PGDLLIMPORT PolarDispatcher *polar_my_dispatcher;
extern PolarDbRoleSettingArray *polar_ss_db_role_setting_array;



extern Size polar_ss_shared_memory_shmem_size(void);
extern void polar_ss_shared_memory_shmem_init(void);
extern void polar_ss_shmem_aset_init_backend(void);
extern Size polar_ss_db_role_setting_shmem_size(void);
extern void polar_ss_db_role_setting_shmem_init(void);
extern Size polar_ss_dispatcher_shmem_size(void);
extern void polar_ss_dispatcher_shmem_init(void);
extern Size polar_ss_session_context_shmem_size(void);
extern void polar_ss_session_context_shmem_init(void);
extern void polar_shared_server_exit(void);
extern int	polar_session_SI_get_data_entries(PolarSessionContext *session, SharedInvalidationMessage *data, int datasize);
extern void dispatcher_main(int argc, char *argv[]);
extern void polar_update_db_role_setting_version(Oid databaseid, Oid roleid, bool is_drop);
extern uint32 get_startup_gucs_hash_from_db_role_setting(char *database_name, char *role_name, uint32 seed);

struct SessionPool;
struct PolarDispatcher;

typedef struct
{
	char		database[NAMEDATALEN];
	char		username[NAMEDATALEN];
	uint32		polar_startup_gucs_hash;
} SessionPoolKey;


typedef enum ScheduleSource
{
	CLIENT_READABLE,
	CLIENT_WRITEABLE,
	BACKEND_READABLE,
	BACKEND_WRITEABLE,
	BACKEND_ATTACHED,
	CLIENT_TERMINATING,
	SENDFD_DONE,
} ScheduleSource;

typedef struct PostmasterChannel
{
	int			type;
	int			magic;
	pgsocket	post_sock;
	int			event_pos;		/* Position of wait event returned by
								 * AddWaitEventToSet */
	uint32		events;			/* registered events */
	struct pt	pt;				/* prototherad */
	List	   *pending_clients;
} PostmasterChannel;

/*
 * channel represent client
 */
typedef struct ClientChannel
{
	int			type;
	int			magic;
	char	   *buf;
	int			rx_pos;
	int			tx_pos;
	int			tx_size;
	int			buf_size;
	uint64		received_command_count;
	int			event_pos;		/* Position of wait event returned by
								 * AddWaitEventToSet */
	uint32		events;			/* registered events */

	struct pt	pt;				/* prototherad */
	struct PtTempVariable
	{
		int32		msg_len;
		char	   *error;
	}			pt_tmp;

	PolarSessionContext *session;
	bool		is_disconnected;	/* connection is lost */
	bool		is_idle;		/* no activity on this channel */
	bool		is_backend_error;
	bool		is_terminating;
	bool		is_wait_send_to_postmaster; /* this conn is returned back to
											 * postmaster. waiting send. */

	TimestampTz start_timestamp;	/* time of backend start */

	struct list_head list_node; /* List node */
	struct list_head pending_list_node;
	struct BackendChannel *backend;
	struct BackendChannel *last_backend;
	struct PolarDispatcher *dispatcher;
	struct SessionPool *pool;
} ClientChannel;

/*
 * channel represent backend
 */
typedef struct BackendChannel
{
	int			type;
	int			magic;
	char	   *buf;
	int			rx_pos;
	int			tx_pos;
	int			tx_size;
	int			buf_size;
	int			event_pos;		/* Position of wait event returned by
								 * AddWaitEventToSet */
	uint32		events;
	uint64		n_transactions;

	struct pt	pt;				/* prototherad */
	int			pipes[2];		/* 0 for dispatcher read/write, 1 for backend
								 * read/write. */
	pgsocket	backend_socket;
	PGPROC	   *backend_proc;
	int			backend_pid;
	bool		is_private;		/* Backend is bound to 1 session and serves
								 * only that session. */
	bool		backend_is_ready;	/* ready for query */
	bool		is_disconnected;	/* connection is lost */
	bool		is_idle;		/* no activity on this channel */
	bool		is_startup_failed;

	/*
	 * We need to save startup packet response to be able to send it to new
	 * connection
	 */
	int			handshake_response_size;
	char	   *handshake_response;
	char	   *handshake_response_pid_pos;
	char	   *handshake_response_key_pos;
	TimestampTz backend_last_activity;	/* time of last backend activity */
	TimestampTz start_timestamp;	/* time of backend start */
	struct ClientChannel *client;
	struct PolarDispatcher *dispatcher;
	struct SessionPool *pool;
	struct BackendChannel *next;

	struct list_head list_node; /* List node */
	struct list_head idle_list_node;	/* List node */
} BackendChannel;


/*
 * Control structure for connection proxies (several dispatcher workers can be launched and each has its own dispatcher instance).
 * PolarDispatcher contains hash of session pools for reach role/dbname combination.
 */
typedef struct PolarDispatcher
{
	WaitEventSet *wait_events;	/* Set of socket descriptors of backends and
								 * clients socket descriptors */
	HTAB	   *pools;			/* Session pool map with dbname/role used as a
								 * key */
	PostmasterChannel postmaster;	/* channel with postmaster to transmit fd. */
	MemoryContext memory_context;	/* Maximal number of backends per database */
	int			max_backends;	/* Maximal number of backends per database */
	bool		shutdown;		/* Shutdown flag */
	PolarDispatcherProc *proc;	/* proc of dispatcher */

	struct list_head hangout_clients_list;	/* List of disconnected clients */
	struct list_head hangout_backends_list; /* List of disconnected backends */
	TimestampTz last_idle_timeout_check;	/* Time of last check for idle
											 * worker timeout expration */
	TimestampTz last_wait_timeout_check;	/* Time of last check for wait
											 * session timeout expration */
} PolarDispatcher;

/*
 * Connection pool to particular role/dbname
 */
typedef struct SessionPool
{
	SessionPoolKey key;
	struct list_head live_backends_list;	/* List of all live clients belong
											 * to this session pool */
	struct list_head idle_backends_list;	/* List of clients waiting for
											 * free backend */

	struct list_head live_clients_list; /* List of all live clients belong to
										 * this session pool */
	struct list_head pending_clients_list;	/* List of clients waiting for
											 * free backend */
	PolarDispatcher *dispatcher;	/* Owner of this pool */
	int			n_backends;		/* Total number of launched backends */
	int			n_dedicated_backends;	/* Number of private (tainted)
										 * backends */
	int			n_idle_backends;	/* Number of backends in idle state */

	int			n_clients;		/* Total number of connected clients */
	int			n_idle_clients; /* Number of clients in idle state */
	int			n_pending_clients;	/* Number of clients waiting for free
									 * backend */
	ProtocolVersion proto;		/* FE/BE protocol version */
} SessionPool;

/* Shared cache invalidation memory segment for PolarDispatcher */
#define MAXNUMMESSAGES 4096
#define MAXNUMMESSAGES_PROCESS_THRESHOLD 100
typedef struct PolarDispatcherSISeg
{
	LWLockPadded inval_read_lock;

	/*
	 * General state information
	 */
	int			minMsgNum;		/* oldest message still needed */
	int			maxMsgNum;		/* next message number to be assigned */

	/*
	 * Circular buffer holding shared-inval messages
	 */
	SharedInvalidationMessage buffer[MAXNUMMESSAGES];
} PolarDispatcherSISeg;

#endif

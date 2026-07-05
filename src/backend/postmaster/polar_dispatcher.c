#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "postgres.h"

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
#include "postmaster/polar_dispatcher.h"
#include "postmaster/postmaster.h"
#include "postmaster/interrupt.h"
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
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "tcop/tcopprot.h"
#include "utils/timeout.h"
#include "utils/ps_status.h"
#include "utils/polar_coredump.h"
#include "utils/inval.h"
#include "commands/dbcommands.h"
#include "storage/smgr.h"
#include "storage/bufmgr.h"
#include "storage/polar_memutils.h"
#include "storage/polar_session_context.h"

#define PT_WAIT_THREAD_HANDLE_ERROR(pt, thread)      \
	do                                               \
	{                                                \
		PT_WAIT_THREAD(pt, (pt_result = (thread)));  \
		ELOG_PSS(DEBUG5, "PT_WAIT %s, %d", #thread, __LINE__);\
		if (unlikely(pt_result == PT_EXITED_ERROR))  \
    		goto error;                              \
    } while(false)


#define PT_HANDLE_NETIO_WRITE_ERROR(chan)                                     \
	do                                                                        \
	{                                                                         \
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))		      \
		{																      \
			/* register event WL_SOCKET_WRITEABLE only when needed. */		  \
			if (!(post->events & WL_SOCKET_WRITEABLE))						  \
			{                                                                 \
				chan->events |= WL_SOCKET_WRITEABLE;                          \
				ModifyWaitEvent(polar_my_dispatcher->wait_events, chan->event_pos, chan->events, NULL);  \
			}                                                                 \
			pt_result = PT_WAITING;                                           \
		}                                                                     \
		ELOG_PSS(DEBUG5, "PT_HANDLE %m %d", __LINE__);				\
		pt_result = PT_EXITED_ERROR;                                          \
	} while(false)

#define PT_REMOVE_WRITE_EVENT(chan)                                   \
	do{                                                               \
		/* all write finished. */                                     \
		if (unlikely(chan->events & WL_SOCKET_WRITEABLE))             \
		{                                                             \
			chan->events &= ~WL_SOCKET_WRITEABLE;                     \
			ModifyWaitEvent(polar_my_dispatcher->wait_events, chan->event_pos, chan->events, NULL); \
		}                                                             \
	} while(false)


#define INIT_BUF_SIZE	   (64*1024)
#define MAX_READY_EVENTS   128
#define DB_HASH_SIZE	   101
#define DISPATCHER_WAIT_TIMEOUT 1000 /* 1 second */

#define CHANNEL_MAGIC    0xDEFA1234U

static void client_channel_remove(ClientChannel* client);
static void backend_channel_remove(BackendChannel* backend);
static PT_THREAD(client_channel_read(ClientChannel* client, size_t max_len));
static PT_THREAD(backend_channel_read(BackendChannel* backend));
static PT_THREAD(client_channel_write(ClientChannel* client));
static PT_THREAD(client_channel_write_self(ClientChannel* client));
static PT_THREAD(backend_channel_write(BackendChannel* backend));
static PT_THREAD(postmaster_channel_write(PostmasterChannel* post));
static void client_channel_hangout(ClientChannel* client);
static void backend_channel_hangout(BackendChannel* backend);

static void report_error_to_client(struct Port *client_port, char const* error);
static bool process_startup_packet(ClientChannel* client, int startup_packet_size,
	bool *rollback_dedicated);
static void client_attach_register(ClientChannel* client);
static PT_THREAD(fiber_client_main(ClientChannel* client, ScheduleSource schedule_source));
static PT_THREAD(fiber_backend_main(BackendChannel* backend, ScheduleSource schedule_source));
static PT_THREAD(launch_new_backend(SessionPool* pool));
static void mark_all_session_reload_config(void);

/* invalidation message */
static void inval_all(void);
static void local_execute_invalidation_message(SharedInvalidationMessage *msg);

PolarDispatcher *polar_my_dispatcher = NULL;
int polar_my_dispatcher_id;
PolarDispatcherProc	*polar_dispatcher_proc = NULL;
PolarDbRoleSettingArray	*polar_ss_db_role_setting_array = NULL;
static TimestampTz      approximate_current_timestamp;

PolarDispatcherSISeg *dispatcher_siseg_buffers = NULL;
PolarDispatcherSISeg *polar_my_dispatcher_siseg_buffer = NULL;

static char terminate_message[5] = {'X', 0, 0, 0, 4};
#define terminate_message_len (5)

static dsa_area *area = NULL;

bool		am_px_worker = false;

typedef enum PolarChannelType {
	PCT_Dispatcher = 0,
	PCT_Client,
	PCT_Backend
} PolarChannelType;

static void
bind_client_backend(ClientChannel* client, BackendChannel* backend)
{
	backend->client = client;
	if (backend->backend_proc)
		backend->backend_proc->polar_shared_session = client->session;
	client->backend = backend;
	client->last_backend = backend;
}

/**
 * Backend is ready for next command outside transaction block (idle state).
 * Now if backend is not tainted it is possible to schedule some other client to this backend.
 */
static void
backend_reschedule(BackendChannel* backend)
{
	SessionPool* pool = backend->pool;
	backend->backend_is_ready = false;
	backend->n_transactions += 1;

	/* If backend is not storing some session context */
	if (!backend->is_private &&
		!(backend->backend_proc && backend->backend_proc->polar_is_backend_dedicated))
	{
		int last_session_id pg_attribute_unused() = -1;
		if (backend->client)
		{
			last_session_id = backend->client->session->session_id;
			backend->client->received_command_count -= pg_atomic_read_u64(&backend->client->session->finished_command_count);
			pg_atomic_write_u64(&backend->client->session->finished_command_count, 0);
			backend->client->session->n_transactions += 1;
			backend->client->session->last_wait_start_timestamp = 0;
			backend->backend_proc->polar_shared_session = NULL;
			backend->client->backend = NULL;
			backend->client->is_idle = true;
			backend->client = NULL;
			pool->n_idle_clients += 1;
			pool->dispatcher->proc->n_idle_clients += 1;
		}

		if (polar_ss_backend_keepalive_timeout)
		{
			if (backend->start_timestamp + polar_ss_backend_keepalive_timeout * USECS_PER_SEC < approximate_current_timestamp)
			{
				ELOG_PSS(DEBUG1, "Destory too old shared backend %d with start time %ld", 
					backend->backend_pid, backend->start_timestamp / USECS_PER_SEC);
				backend_channel_hangout(backend);
				backend = NULL;
			}
		}

		if (polar_ss_session_schedule_policy == SESSION_SCHEDULE_DISPOSABLE && backend->n_transactions > 1)
		{
			ELOG_PSS(DEBUG1, "Destory disposable shared backend %d(%lu) after session %d used",
					backend->backend_pid, backend->n_transactions, last_session_id);
			backend_channel_hangout(backend);
			backend = NULL;
		}

		if (!list_empty(&pool->pending_clients_list))
		{
			/* Has pending clients: serve one of them */
			if (backend)
			{
				ClientChannel* client = NULL;
				if (polar_ss_session_schedule_policy == SESSION_SCHEDULE_RANDOM)
				{
					bool found = false;
					list_for_each_entry(client, &pool->pending_clients_list, pending_list_node)
					{
						if (random() % 2 == 0)
						{
							found = true;
							break;
						}
					}

					if (!found)
					{
						ELOG_PSS(DEBUG1, "Backend %d is not suggest to any pending client session", 
							backend->backend_pid);
						client = NULL;
					}
					else
					{
						ELOG_PSS(DEBUG1, "Backend %d is suggest to client session %d", 
							backend->backend_pid, client->session->session_id);
					}
				}
				else
				{
					client = list_first_entry(&pool->pending_clients_list, ClientChannel, pending_list_node);
				}

				if (client != NULL)
				{
					Assert(client->type == PCT_Client);
					list_del_init(&client->pending_list_node);
					ELOG_PSS(DEBUG1, "Backend %d is reassigned to client %d(wait %ldus)", 
							backend->backend_pid, client->session->session_id, 
							approximate_current_timestamp - client->session->last_wait_start_timestamp);
					pool->n_pending_clients -= 1;
					pool->dispatcher->proc->n_pending_clients -= 1;
					bind_client_backend(client, backend);
					fiber_client_main(client, BACKEND_ATTACHED);
					return ;
				}
				else
				{
					ELOG_PSS(DEBUG1, "Backend %d is idle", backend->backend_pid);
					list_add_tail(&backend->idle_list_node, &pool->idle_backends_list);
					pool->n_idle_backends += 1;
					pool->dispatcher->proc->n_idle_backends += 1;
					backend->is_idle = true;
				}
			}

			if (pool->n_backends < pool->dispatcher->max_backends)
				launch_new_backend(pool);
		}
		else /* return backend to the list of idle backends */
		{
			if (backend)
			{
				ELOG_PSS(DEBUG1, "Backend %d is idle", backend->backend_pid);
				list_add_tail(&backend->idle_list_node, &pool->idle_backends_list);
				pool->n_idle_backends += 1;
				pool->dispatcher->proc->n_idle_backends += 1;
				backend->is_idle = true;
			}
		}
	}
	else
	{
		if (!backend->is_private) /* if it was not marked as tainted before... */
		{
			ELOG_PSS(DEBUG1, "Backend %d set tainted", backend->backend_pid);
			backend->is_private = true;
			backend->dispatcher->proc->n_dedicated_backends += 1;
			pool->n_dedicated_backends += 1;
		}

		if (backend->client)
		{
			ELOG_PSS(DEBUG1, "Backend %d, Session %d is already tainted %lu",
				backend->backend_pid, backend->client->session->session_id, backend->client->session->n_transactions);
			backend->client->received_command_count -= pg_atomic_read_u64(&backend->client->session->finished_command_count);
			pg_atomic_write_u64(&backend->client->session->finished_command_count, 0);
			backend->client->session->n_transactions += 1;
			backend->client->session->last_wait_start_timestamp = 0;
		}
	}
}

static
PT_THREAD(client_read_and_parse_startup(ClientChannel* client))
{
	int32 msg_len;
	int32 value_len;

	if (client->rx_pos < 4)
	{
		if (client_channel_read(client, 4 - client->rx_pos) == PT_EXITED_ERROR)
			return PT_EXITED_ERROR;
		if (client->rx_pos < 4)
			return PT_WAITING;
	}

	memcpy(&msg_len, client->buf, sizeof(msg_len));
	msg_len = pg_ntoh32(msg_len);
	value_len = msg_len - 4;
	if (value_len < (int32) sizeof(ProtocolVersion) ||
		value_len > MAX_STARTUP_PACKET_LENGTH)
	{
		ELOG_PSS(DEBUG1, "client_read_and_parse_startup %d", value_len);
		report_error_to_client(client->session->info->client_port, "invalid length of startup packet");
		return PT_EXITED_ERROR;
	}
	if (msg_len > client->buf_size)
	{
		client->buf_size = msg_len;
		client->buf = repalloc(client->buf, client->buf_size);
		return PT_WAITING;
	}
	if (client->rx_pos < msg_len)
	{
		if (client_channel_read(client, msg_len - client->rx_pos) == PT_EXITED_ERROR)
			return PT_EXITED_ERROR;
	}
	if (client->rx_pos < msg_len)  /*  wait more data */
		return PT_WAITING;
	return PT_ENDED;
}


static
PT_THREAD(client_read_and_parse_command(ClientChannel* client, int32* msg_len_ret))
{
	int32 msg_len;

	if (unlikely(client_channel_read(client, 0) == PT_EXITED_ERROR))
		return PT_EXITED_ERROR;
	if (client->rx_pos < 5)
		return PT_WAITING;
	memcpy(&msg_len, client->buf + 1, sizeof(msg_len));
	msg_len = pg_ntoh32(msg_len) + 1;

	if (msg_len > client->buf_size)
	{
		client->buf_size = msg_len;
		client->buf = repalloc(client->buf, client->buf_size);
		return PT_WAITING;
	}
	if (client->rx_pos < msg_len)  /*  wait more data */
		return PT_WAITING;
	*msg_len_ret = msg_len;
	return PT_ENDED;
}

static
PT_THREAD(backend_read_and_parse_command(BackendChannel* client, int32* msg_len_ret))
{
	int32 msg_len;

	if (unlikely(backend_channel_read(client) == PT_EXITED_ERROR))
		return PT_EXITED_ERROR;

	if (client->rx_pos < 5)
		return PT_WAITING;
	memcpy(&msg_len, client->buf + 1, sizeof(msg_len));
	msg_len = pg_ntoh32(msg_len) + 1;

	if (msg_len > client->buf_size)
	{
		client->buf_size = msg_len;
		client->buf = repalloc(client->buf, client->buf_size);
		return PT_WAITING;
	}
	if (client->rx_pos < msg_len)  /*  wait more data */
		return PT_WAITING;
	*msg_len_ret = msg_len;
	return PT_ENDED;
}

/*
 * Parse request from auth command, corresponds to sendAuthRequest().
 * Caller ensure that command is full.
 */
static AuthRequest
parse_auth_command_request(char* command)
{
	AuthRequest req;
	Assert(sizeof(AuthRequest) == sizeof(uint32));
	memcpy(&req, command + 5, sizeof(AuthRequest));
	req = pg_ntoh32(req);
	return req;
}

/* client main fiber */
static
PT_THREAD(fiber_client_main(ClientChannel* client, ScheduleSource schedule_source))
{
	static PT_RESULT pt_result;
	if (unlikely(client->is_disconnected))
		PT_EXIT_ERROR(&client->pt);

	PT_BEGIN(&client->pt);
	/*----------------------------------------------------------------
	 * 1. startup, auth and handshake
	 *----------------------------------------------------------------
	 */
	PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_read_and_parse_startup(client));

	client->session->info->client_port->proto = pg_ntoh32(*(ProtocolVersion*)(client->buf + 4));
	if (unlikely(client->session->info->client_port->proto == CANCEL_REQUEST_CODE))
	{
		processCancelRequest(client->session->info->client_port, client->buf + 4);
		goto error;
	}

	if (client->session->info->client_port->proto == NEGOTIATE_SSL_CODE)
	{
		/* shared server doesn't support SSL yet.*/
		/* drop ssl startup packet, and send 'N' to client to reject ssl */
		client->buf[0] = 'N';
		client->rx_pos = client->tx_size = 1;
		client->tx_pos = 0;
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write_self(client));

		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_read_and_parse_startup(client));
		client->session->info->client_port->proto = pg_ntoh32(*(ProtocolVersion*)(client->buf + 4));
	}

	if (PG_PROTOCOL_MAJOR(client->session->info->client_port->proto) < 3)
	{
		report_error_to_client(client->session->info->client_port, "shared server only supprt postgresql protocol >= 3");
		goto error;
	}

	{
		bool rollback_dedicated = false;
		if (!process_startup_packet(client, client->rx_pos, &rollback_dedicated))
		{
			if (rollback_dedicated) /* return this session back to postmaster. */
			{
				client->is_wait_send_to_postmaster = true;
				client->tx_size = client->rx_pos;  /* send startup packet to postmaster. */
				polar_my_dispatcher->postmaster.pending_clients = lappend(polar_my_dispatcher->postmaster.pending_clients, client);
				postmaster_channel_write(&polar_my_dispatcher->postmaster);
			}
			goto error;
		}
	}

	/* drop startup packet */
	client->rx_pos = 0;
	/* insert an shared session init command to send to shared backend */
	{
		char shared_session_auth_command[6] = {'Y', 0, 0, 0, 5, 'A'};
		int auth_command_len = 6;
		memcpy(client->buf, shared_session_auth_command, auth_command_len);
		client->rx_pos = client->tx_size = auth_command_len;
	}

	/* attach a baceknd */
	if (NULL == client->backend)
	{
		client_attach_register(client);
		PT_WAIT_UNTIL(&client->pt, client->backend != NULL);
		if (unlikely(client->backend->is_startup_failed))
			goto backend_startup_failed;
	}

	/* dispatcher --> backend:  shared session auth command */
	PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, backend_channel_write(client->backend));
	/* dispatcher <-- backend: 'R' command */
	PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, backend_read_and_parse_command(client->backend, &client->pt_tmp.msg_len));
	if (client->backend->buf[0] != 'R') /* error replay, rejected by hba check */
	{
		client->backend->tx_size = client->pt_tmp.msg_len;
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write(client));
		goto error;
	}
	/* process 'R' command, extract auth_request. */
	if (parse_auth_command_request(client->backend->buf) != AUTH_REQ_OK)  /* don't match 'trust' */
	{
		/* client <-- dispatcher: 'R' command */
		client->backend->tx_size = client->pt_tmp.msg_len;
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write(client));

		/* client --> dispatcher: encrypted password command */
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_read_and_parse_command(client, &client->pt_tmp.msg_len));

		/* dispatcher --> backend: encrypted password command */
		client->tx_size = client->pt_tmp.msg_len;
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, backend_channel_write(client->backend));

		/* dispatcher <-- backend: 'R' command */
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, backend_read_and_parse_command(client->backend, &client->pt_tmp.msg_len));

		if (client->backend->buf[0] != 'R')  /* error replay, auth failed */
		{
			client->backend->tx_size = client->pt_tmp.msg_len;
			PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write(client));
			goto error;
		}
		Assert(parse_auth_command_request(client->backend->buf) == AUTH_REQ_OK);
	}
	/* client <-- dispatcher: 'R' command with AUTH_REQ_OK */
	client->backend->tx_size = client->pt_tmp.msg_len;
	PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write(client));

	do
	{
		/* dispatcher <-- backend: 'S' command */
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, backend_read_and_parse_command(client->backend, &client->pt_tmp.msg_len));
		if (client->backend->buf[0] != 'S')
			break;
		/* client <-- dispatcher: 'S' coomand */
		client->backend->tx_size = client->pt_tmp.msg_len;
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write(client));
	} while(true);

	if (!(client->backend->buf[0] == 'Z' && client->backend->buf[5] == 'I'))
		goto error;
	/* There must be no additional command */
	Assert(client->backend->rx_pos == client->pt_tmp.msg_len);

	/* rewrite BackendKeyData in handshake_response */
	{
		BackendChannel* backend = client->backend;
		int32 tmp_pid = pg_hton32(client->session->session_id);
		int32 tmp_key = pg_hton32(client->session->info->cancel_key);
		memcpy(backend->handshake_response_pid_pos, (void *)&tmp_pid, sizeof(int32));
		memcpy(backend->handshake_response_key_pos, (void *)&tmp_key, sizeof(int32));
	}
	/* drop backend 'Z' command, and send handshake message to client. */
	{
        /* If we attach new client to the existed backend, then we need to send handshake response to the client */
		BackendChannel* backend = client->backend;
		memcpy(backend->buf, backend->handshake_response, backend->handshake_response_size);
		backend->rx_pos = backend->tx_size = backend->handshake_response_size;
		backend->tx_pos = 0;
	}
	PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write(client));

	/* important, release resources if they are not used immediately */
	client->backend->backend_is_ready = true;
	backend_reschedule(client->backend);

	/*----------------------------------------------------------------
	 * 2. main loop, async read/write of client and backend
	 *----------------------------------------------------------------
	 */
	/* client->session->finished_command_count was upated by the code above. must reset it. */
	pg_atomic_write_u64(&client->session->finished_command_count, 0);
	client->received_command_count = 0;
	schedule_source = CLIENT_READABLE;
	while(true)
	{
		ELOG_PSS(DEBUG5, "%p receive %d, %lu", client, schedule_source, client->received_command_count);

		/*----------------------------------------------------------------
		 * 2.1 CLIENT_READABLE
		 *----------------------------------------------------------------
		 */
		if (likely(schedule_source == CLIENT_READABLE))
		{
			if (unlikely(client_channel_read(client, 0) == PT_EXITED_ERROR))
			{
				if (client->tx_pos < client->tx_size)
					goto wait_next_schedule;
				else
					goto error;
			}

			if (unlikely(client->is_terminating))
				goto wait_next_schedule;

			/* parse client request */
			{ 
				int32 msg_start = client->tx_size;
				int32 command_count = 0;

				/* Loop through all received messages */
				while (client->rx_pos - msg_start >= 5) /* has message code + length */
				{
					int32 msg_len;

					ELOG_PSS(DEBUG5, "%p receive message %c", client, client->buf[msg_start]);
					memcpy(&msg_len, client->buf + msg_start + 1, sizeof(msg_len));
					msg_len = pg_ntoh32(msg_len) + 1;

					if (msg_start + msg_len > client->buf_size)
					{
						/* Reallocate buffer to fit complete message body */
						client->buf_size = msg_start + msg_len;
						client->buf = repalloc(client->buf, client->buf_size);
					}
					if (client->rx_pos - msg_start >= msg_len) /* Message is completely fetched */
					{
						if (unlikely(client->buf[msg_start] == 'X'))  /* session terminate */
						{
							client->is_terminating = true;
							msg_start += msg_len;
							command_count += 1;
							/* drop command after 'X' command */
							client->rx_pos = msg_start;
							client->tx_size = msg_start;
							client->received_command_count += command_count;
							ELOG_PSS(DEBUG5, "%p receive message X %d", client, client->session->id);
							goto error;
						}
						msg_start += msg_len;
						command_count += 1;
					}
					else
					{
						ELOG_PSS(DEBUG5, "%p Incomplete message %d %d %d %d", client, client->rx_pos, msg_start, msg_len, client->buf_size);
						break; /* Incomplete message. */
					}
				} /* while (client->rx_pos - msg_start >= 5) */
				ELOG_PSS(DEBUG5, "client Message size %d, %.*s", msg_start, msg_start - 5, client->buf + 5);
				client->tx_size = msg_start;
				client->received_command_count += command_count;
			} /* parse client request */
			if (client->tx_size != 0)
			{
				/* Has some complete messages to send to peer, try to get backend */
				if (!client->backend)
				{
					client_attach_register(client);
					if (client->backend && client->backend->is_startup_failed)
						goto backend_startup_failed;
				}
				/* if can't get a backend immediately, for performance, not wait backend here */
				if (!client->backend)
					goto wait_next_schedule;

				/* try write, don't need to wait complete here */
				if (unlikely(backend_channel_write(client->backend) == PT_EXITED_ERROR))
					goto error;
			}
		} /* if (likely(schedule_source == CLIENT_READABLE)) */
		/*---X_X-------------------------------------------------------------
		 * 2.2 BACKEND_READABLE
		 *----------------------------------------------------------------
		 */
		else if (likely(schedule_source == BACKEND_READABLE))
		{
			if (unlikely(backend_channel_read(client->backend) == PT_EXITED_ERROR))
				goto error;

			{  /* parse backend message */
				BackendChannel* backend = client->backend;
				int32 msg_start = backend->tx_size;
				/* Loop through all received messages */
				while (backend->rx_pos - msg_start >= 5) /* has message code + length */
				{
					int32 msg_len;

					ELOG_PSS(DEBUG5, "%d receive message %c", backend->backend_pid, backend->buf[msg_start]);
					memcpy(&msg_len, backend->buf + msg_start + 1, sizeof(msg_len));
					msg_len = pg_ntoh32(msg_len) + 1;

					if (msg_start + msg_len > backend->buf_size)
					{
						/* Reallocate buffer to fit complete message body */
						backend->buf_size = msg_start + msg_len;
						backend->buf = repalloc(backend->buf, backend->buf_size);
					}
					if (backend->rx_pos - msg_start >= msg_len) /* Message is completely fetched */
					{
						if (backend->buf[msg_start] == 'Z'	/* Ready for query */
							&& backend->buf[msg_start+5] == 'I') /* Transaction block status is idle */
						{
							/* barrier keep order between check 'Z' and  client->session->finished_command_count. */
							backend->dispatcher->proc->n_transactions += 1;
							if (backend->rx_pos - msg_start == msg_len   /* last message */
								&& client->received_command_count == 
									pg_atomic_read_u64(&client->session->finished_command_count)) /* all command processed */
							{
								backend->backend_is_ready = true; /* Backend is ready for query */
							}
						}
						else if (backend->buf[msg_start] == 'X')  /* session close */
						{
							Assert(backend->rx_pos - msg_start == msg_len); /* last message */
							backend->rx_pos = msg_start;  /* X command don't send to client */
							backend->tx_size = msg_start;
							PT_WAIT_THREAD(&client->pt, (pt_result = client_channel_write(client)));
							if (pt_result == PT_EXITED_ERROR)
							{
								/* drop backend buffer remaining bytes */
								client->backend->rx_pos = client->backend->tx_pos = client->backend->tx_size = 0;
							}
							client->backend->backend_is_ready = true;
							backend_reschedule(client->backend);
							goto error;
						}
						msg_start += msg_len;
					}
					else
						break; /* Incomplete message. */
				}
				ELOG_PSS(DEBUG5, "backend Message size %d, %.*s", msg_start, msg_start - 5, backend->buf + 5);
				backend->tx_size = msg_start;
			} /* parse backend message */

			pt_result = client_channel_write(client);
			if (unlikely(PT_EXITED_ERROR == pt_result))
				goto error;
			if (PT_ENDED == pt_result)
			{
				if (client->backend->backend_is_ready)
					backend_reschedule(client->backend);
			}
		} /* if (likely(schedule_source == BACKEND_READABLE)) */
		/*----------------------------------------------------------------
		 * 2.3 BACKEND_ATTACHED or BACKEND_WRITEABLE
		 *----------------------------------------------------------------
		 */
		else if (schedule_source == BACKEND_ATTACHED || schedule_source == BACKEND_WRITEABLE)
		{
			if (unlikely(client->backend->is_startup_failed))
				goto backend_startup_failed;
			if (unlikely(backend_channel_write(client->backend) == PT_EXITED_ERROR))
				goto error;
		} /* if (schedule_source == BACKEND_ATTACHED || schedule_source == BACKEND_WRITEABLE) */
		/*----------------------------------------------------------------
		 * 2.4 CLIENT_WRITEABLE
		 *----------------------------------------------------------------
		 */
		else if (schedule_source == CLIENT_WRITEABLE)
		{
			pt_result = client_channel_write(client);
			if (unlikely(PT_EXITED_ERROR == pt_result))
				goto error;
			if (PT_ENDED == pt_result)
			{
				if (client->backend->backend_is_ready)
					backend_reschedule(client->backend);
			}
		}
		/*----------------------------------------------------------------
		 * 2.5 CLIENT_TERMINATING
		 *----------------------------------------------------------------
		 */
		else if (schedule_source == CLIENT_TERMINATING)
		{
			goto error;
		}

	wait_next_schedule:
		PT_YIELD(&client->pt);
	} /* while(true) */

	/*----------------------------------------------------------------
	 * 3. end, error handling and exit
	 *----------------------------------------------------------------
	 */

	ELOG_PSS(DEBUG5, "%p arrive backend_startup_failed  %d", client, client && client->session ? client->session->id : -1);

 backend_startup_failed:
	{
		BackendChannel* backend = client->backend;
		Assert(client->buf_size >= backend->handshake_response_size);
		memcpy(client->buf, backend->handshake_response, backend->handshake_response_size);
		client->rx_pos = client->tx_size = backend->handshake_response_size;
		client->tx_pos = 0;
		backend_channel_hangout(backend);
		/*
		 * hangout backend before PT_WAIT_THREAD_HANDLE_ERROR(client_channel_write_self(client)).
		 * Because the latter may cause goto error, then hangout backend is skipped.
		 */
		PT_WAIT_THREAD_HANDLE_ERROR(&client->pt, client_channel_write_self(client));
		goto error;
	}

 error:
	ELOG_PSS(DEBUG5, "%p arrive error  %d,%d", 
		client, client->session ? client->session->id : -1, 
		client->session ? client->session->session_id : -1);

	if (client->is_backend_error)
	{
		ELOG_PSS(DEBUG5, "%p is_backend_error  %d,%d", 
			client, client->session ? client->session->id : -1, 
				client->session ? client->session->session_id : -1);
		PT_WAIT_UNTIL(&client->pt, pg_atomic_read_u32(&client->session->is_shared_backend_exit) == 1);
		backend_channel_hangout(client->backend);
		Assert(NULL == client->backend);
		/* drop all remaining client input */
		client->rx_pos = client->tx_pos = client->tx_size = 0;
	}

	if (NULL == client->backend &&
		polar_session_has_temp_namespace())
	{
		/* get a backend to send 'X' command, to delete temp table. */
		client_attach_register(client);
		PT_WAIT_UNTIL(&client->pt, client->backend != NULL);
		if (unlikely(client->backend->is_startup_failed))
			backend_channel_hangout(client->backend);

		/*
		 * If unlikely can't get a backend to delete temp table, just OK.
		 * Leaving a small amount of temp table as orphans temp table is OK.
		 * Orphans temp table can be delete by vacuum or new session with same id later.
		 */
	}

	if (client->backend != NULL)
	{
		/* get here, backend must be ok */
		if (!client->is_terminating)
		{
			ELOG_PSS(DEBUG5, "%p arrive !is_terminating  %d", 
				client, client->session ? client->session->id : -1);
			/* make a 'X' command manually */
			if (client->tx_size + terminate_message_len > client->buf_size)
			{
				client->buf_size = client->tx_size + terminate_message_len;
				client->buf = repalloc(client->buf, client->buf_size);
			}
			memcpy(client->buf + client->tx_size, terminate_message, terminate_message_len);
			client->tx_size += terminate_message_len;
			client->rx_pos = client->tx_size;
		}

		do
		{
			/*  ELOG_PSS(DEBUG5, "%p arrive is_terminating  %d",  */
			/*  	client, client->session ? client->session->id : -1); */

			/* write session 'X' command */
			if (client->tx_size != 0)
			{
				if (backend_channel_write(client->backend) == PT_EXITED_ERROR)
					goto error;
			}

			if (client->backend == NULL)
				goto error;

			if (backend_channel_read(client->backend) == PT_EXITED_ERROR)
				goto error;
			/* drop all backend read */
			client->backend->rx_pos = client->backend->tx_pos = client->backend->tx_size = 0;

			if (pg_atomic_read_u32(&client->session->status) == PSSE_CLOSED)
			{
				/* cleanup the socket */
				do {
					if (client->backend == NULL)
						goto error;
					pt_result = backend_channel_read(client->backend);
					if (PT_EXITED_ERROR == pt_result)
						goto error;
					/* drop all backend read */
					client->backend->rx_pos = client->backend->tx_pos = client->backend->tx_size = 0;
					/* has nothing to read */
					if (pt_result < PT_EXITED_ERROR)
						break;
				} while(true);
				break;
			}
			PT_YIELD(&client->pt);
		} while(true);

		client->backend->backend_is_ready = true;
		backend_reschedule(client->backend);
		if (client->backend != NULL) /* backend is tainted */
		{
			ELOG_PSS(DEBUG5, "client->backend != NULL %p, %d", client->backend, client->backend->backend_pid);
			backend_channel_hangout(client->backend);
		}
	}

	Assert(NULL == client->backend);
	client_channel_hangout(client);

	PT_END(&client->pt);
}

/* backend main fiber */
static
PT_THREAD(fiber_backend_main(BackendChannel* backend, ScheduleSource schedule_source))
{
	static PT_RESULT pt_result;
	PT_BEGIN(&backend->pt);

	{
		/*  TODO check length */
		char* dst = backend->buf;
		SessionPool* pool = backend->pool;
		int32     startup_len;

		/* reserve for length */
		dst += 4;
		/* ProtocolVersion */
		*(ProtocolVersion*)dst = pg_hton32(pool->proto);
		dst += 4;

		dst += sprintf(dst, "database");
		*dst++ = 0;
		dst += sprintf(dst, "%s", pool->key.database);
		*dst++ = 0;

		dst += sprintf(dst, "user");
		*dst++ = 0;
		dst += sprintf(dst, "%s", pool->key.username);
		*dst++ = 0;

		*dst++ = 0;
		startup_len = dst - backend->buf;
		*(int32*)backend->buf = pg_hton32(startup_len);
		backend->tx_pos = 0;
		backend->tx_size = startup_len;
	}

	polar_my_dispatcher->postmaster.pending_clients = lappend(polar_my_dispatcher->postmaster.pending_clients, backend);
	postmaster_channel_write(&polar_my_dispatcher->postmaster);
	PT_WAIT_UNTIL(&backend->pt, backend->tx_pos == backend->tx_size);
	backend->tx_size = backend->tx_pos = 0;

	closesocket(backend->pipes[0]);

	PT_WAIT_THREAD_HANDLE_ERROR(
		&backend->pt,
		((
			{
				bool need_break = true;
				do
				{
					pt_result = backend_channel_read(backend);
					if (PT_EXITED_ERROR == pt_result)
						break;
					while (backend->rx_pos - backend->tx_size >= 5) /* has message code + length */
					{
						int32 msg_len;
						need_break = false;
						memcpy(&msg_len, backend->buf + backend->tx_size + 1, sizeof(msg_len));
						msg_len = pg_ntoh32(msg_len) + 1;
						if (backend->tx_size + msg_len > backend->buf_size)
						{
							/* Reallocate buffer to fit complete message body */
							backend->buf_size = backend->tx_size + msg_len;
							backend->buf = repalloc(backend->buf, backend->buf_size);
						}
						if (backend->rx_pos - backend->tx_size >= msg_len) /* Message is completely fetched */
						{
							int32 command_begin= backend->tx_size;
							backend->tx_size += msg_len;
							ELOG_PSS(DEBUG5, "backend->buf[command_begin] == %c-%d, %d-%d-%d", backend->buf[command_begin], backend->buf[command_begin], backend->rx_pos, backend->tx_size, msg_len);
							if (backend->buf[command_begin] == 'E')
							{
								if (command_begin != 0)
								{
									memmove(backend->buf, backend->buf + command_begin, msg_len);
									backend->rx_pos = backend->tx_size = msg_len;
								}
								pt_result = PT_ENDED;
								need_break = true;
								break;
							}
							else if (backend->buf[command_begin] == 'R' || backend->buf[command_begin] == 'N' || backend->buf[command_begin] == 'S')
							{
								/* drop this command */
								memmove(backend->buf + command_begin, backend->buf + backend->tx_size, backend->rx_pos - backend->tx_size);
								backend->rx_pos -= msg_len;
								backend->tx_size -= msg_len;
								continue;
							}
							else if (backend->buf[command_begin] == 'Z')  /* Ready for query */
							{
								Assert(backend->buf[command_begin + 5] == 'I'); /* Transaction block status is idle */
								Assert(backend->rx_pos == backend->tx_size);    /* no other message */
								pt_result = PT_ENDED;
								need_break = true;
								break;
							}
						}
					} /* while(backend->rx_pos - backend->tx_size >= 5) */
				}
				while(!need_break);
			}
		),
		pt_result)
	);

	backend->handshake_response_size = backend->tx_size;
	backend->handshake_response = palloc(backend->handshake_response_size);
	memcpy(backend->handshake_response, backend->buf, backend->handshake_response_size);
	/* Extract backend pid */
	if (backend->handshake_response[0] != 'E') /* not error response */
	{
		char	*msg = backend->handshake_response;
		char	*source = msg;
		int32   msg_len;
		while (*msg != 'K') /* Scan handshake response until we reach PID message */
		{
			memcpy(&msg_len, ++msg, sizeof(msg_len));
			msg_len = pg_ntoh32(msg_len);
			msg += msg_len;
			Assert(msg < backend->handshake_response + backend->handshake_response_size);
		}
		memcpy(&msg_len, msg + 5, sizeof(msg_len));
		backend->backend_pid = pg_ntoh32(msg_len);
		backend->backend_proc = BackendPidGetProc(backend->backend_pid);
		backend->handshake_response_pid_pos = (backend->handshake_response + (msg + 5 - source));
		backend->handshake_response_key_pos = (backend->handshake_response + (msg + 9 - source));
	}
	else
		backend->is_startup_failed = true;

	backend->rx_pos = backend->tx_pos = backend->tx_size = 0;
	backend->backend_is_ready = true;
	backend->start_timestamp = approximate_current_timestamp;
	backend_reschedule(backend);
	PT_YIELD(&backend->pt);

	while(true)
	{
		if (backend->client != NULL)
			fiber_client_main(backend->client, schedule_source);
		else
		{
			/* unix domain socket has extra mesage */
			if (backend_channel_read(backend) >= PT_EXITED_ERROR)
				goto error;
			if (backend->rx_pos != 0 || backend->tx_size != 0 || backend->tx_pos != 0)
				goto error;
		}

		PT_YIELD(&backend->pt);
	}

 error:
	ELOG_PSS(DEBUG5, "fiber_backend_main ");
	backend_channel_hangout(backend);
	PT_END(&backend->pt);
}

/**
 * Parse client's startup packet and assign client to proper connection pool based on dbname/role
 */
static bool
process_startup_packet(ClientChannel* client, int startup_packet_size, 
	bool *rollback_dedicated)
{
	bool found;
	SessionPoolKey key;
	char* startup_packet = client->buf;
	*rollback_dedicated = false;


	if (client->session->memory_context->type != T_ShmAllocSetContext ||
		!POLAR_SHARED_SERVER_ENABLE())
	{
		*rollback_dedicated = true;
		elog(LOG, "polar shared server use origion backend for client %d", client->session->session_id);
		return false;
	}

	if (polar_parse_startup_packet(client->session->info->client_port,
								   client->session->memory_context,
								   startup_packet + 4,         /* skip packet size */
								   startup_packet_size - 4,
								   true, true) != STATUS_OK)
	{
		elog(WARNING, "Failed to parse startup packet for client %d", client->session->session_id);
		return false;
	}

	if (am_walsender ||
		am_px_worker ||
		polar_check_dbuser_dedicated(client->session->info->client_port->database_name,
			client->session->info->client_port->user_name))
	{
		*rollback_dedicated = true;
		elog(LOG, "polar shared server use origion backend for client %d", client->session->session_id);
		return false;
	}

	client->dispatcher->proc->n_ssl_clients += client->session->info->client_port->ssl_in_use;
	pg_set_noblock(client->session->info->client_port->sock); /* SSL handshake may switch socket to blocking mode */
	memset(&key, 0, sizeof(key));
	strlcpy(key.database, client->session->info->client_port->database_name, NAMEDATALEN);
	strlcpy(key.username, client->session->info->client_port->user_name, NAMEDATALEN);

	client->session->info->client_port->polar_startup_gucs_hash = get_startup_gucs_hash_from_db_role_setting(
			key.database, key.username, client->session->info->client_port->polar_startup_gucs_hash);

	key.polar_startup_gucs_hash = client->session->info->client_port->polar_startup_gucs_hash;

	ELOG_PSS(DEBUG1, "Client %d connects to %s/%s/%u", 
		client->session->session_id, key.database, key.username,
		key.polar_startup_gucs_hash);

	client->pool = (SessionPool*)hash_search(client->dispatcher->pools, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* First connection to this role/dbname */
		client->dispatcher->proc->n_pools += 1;
		memset((char*)client->pool + sizeof(SessionPoolKey), 0, sizeof(SessionPool) - sizeof(SessionPoolKey));
		INIT_LIST_HEAD(&client->pool->pending_clients_list);
		INIT_LIST_HEAD(&client->pool->live_clients_list);
		INIT_LIST_HEAD(&client->pool->idle_backends_list);
		INIT_LIST_HEAD(&client->pool->live_backends_list);
		client->pool->proto = client->session->info->client_port->proto;
	}
	list_add(&client->list_node, &client->pool->live_clients_list);

	client->pool->dispatcher = client->dispatcher;
	client->pool->dispatcher->proc->n_startup_clients -= 1;
	client->pool->dispatcher->proc->n_idle_clients += 1;
	client->pool->n_idle_clients += 1;
	client->pool->n_clients += 1;
	client->is_idle = true;
	return true;
}

/*
 * Try to send error message to the client. It happens that the client is going to close, so it doesn't matter if the reports fails.
 */
static void
report_error_to_client(struct Port *client_port, char const* error)
{
	StringInfoData msgbuf;
	initStringInfo(&msgbuf);
	pq_sendbyte(&msgbuf, 'E');
	pq_sendint32(&msgbuf, 7 + strlen(error));
	pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);
	pq_sendstring(&msgbuf, error);
	pq_sendbyte(&msgbuf, '\0');

	{
#ifdef USE_SSL
		int waitfor = 0;
		if (client_port && client_port->ssl_in_use)
			be_tls_write(client_port, msgbuf.data, msgbuf.len, &waitfor);
		else
#endif
			secure_raw_write(client_port, msgbuf.data, msgbuf.len);
	}
	pfree(msgbuf.data);
}

/*
 * Attach client to backend.
 */
static void
client_attach_register(ClientChannel* client)
{
	BackendChannel* backend = NULL;

	/* check if client is already on pending_clients_list */
	if (unlikely(!list_empty(&client->pending_list_node)))
		return;

	client->is_idle = false;
	client->pool->n_idle_clients -= 1;
	client->pool->dispatcher->proc->n_idle_clients -= 1;
	if (!list_empty(&client->pool->idle_backends_list))
	{
		if (polar_ss_session_schedule_policy == SESSION_SCHEDULE_FIFO ||
			polar_ss_session_schedule_policy == SESSION_SCHEDULE_DISPOSABLE)
		{
			backend = list_first_entry(&client->pool->idle_backends_list, BackendChannel, idle_list_node);
		}
		else
		{
			bool found = false;

			list_for_each_entry(backend, &client->pool->idle_backends_list, idle_list_node)
			{
				Assert(backend->type == PCT_Backend);
				if (random() % 2 == 0)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				ELOG_PSS(DEBUG1, "Client %d is not suggest to use any idle backends", 
							client->session->session_id);
				backend = NULL;
			}
			else
			{
				ELOG_PSS(DEBUG1, "Backend %d is suggest to client %d", backend->backend_pid, client->session->session_id);
			}
		}
	}

	if (backend != NULL)
	{
		/* has some idle backend */
		Assert(!backend->is_private);
		bind_client_backend(client, backend);
		list_del_init(&backend->idle_list_node);

		client->pool->n_idle_backends -= 1;
		client->pool->dispatcher->proc->n_idle_backends -= 1;
		backend->is_idle = false;
		backend->backend_last_activity = approximate_current_timestamp;
		client->session->last_wait_start_timestamp = 0;
		ELOG_PSS(DEBUG1,  "Attach client %d to backend %p (pid %d) %ld", 
				client->session->session_id, backend, backend->backend_pid, 
				approximate_current_timestamp);
		return;
	}
	else /* all backends are busy */
	{
		/* Postpone handshake until some backend is available */
		ELOG_PSS(DEBUG1, "Client %p %d is waiting for available backends", client, client->session->session_id);
		list_add_tail(&client->pending_list_node, &client->pool->pending_clients_list);
		client->pool->n_pending_clients += 1;
		client->pool->dispatcher->proc->n_pending_clients += 1;
		client->session->last_wait_start_timestamp = approximate_current_timestamp;

		if (client->pool->n_backends < client->dispatcher->max_backends)
			launch_new_backend(client->pool);
		return;
	}
}

/*
 * Handle communication failure for this channel.
 * It is not possible to remove channel immediately because it can be triggered by other epoll events.
 * So link all channels in L1 list for pending delete.
 */
static void
client_channel_hangout(ClientChannel* client)
{
	if (client->is_disconnected)
	   return;

	if (client->event_pos >= 0)
	{
		DeleteWaitEventFromSet(client->dispatcher->wait_events, client->event_pos);
		client->event_pos = -1;
	}

	if (!list_empty(&client->pending_list_node))
	{
		list_del_init(&client->pending_list_node);
		client->pool->n_pending_clients -= 1;
		client->pool->dispatcher->proc->n_pending_clients -= 1;
	}

	if (client->is_idle)
	{
		client->pool->n_idle_clients -= 1;
		client->pool->dispatcher->proc->n_idle_clients -= 1;
		client->is_idle = false;
	}
	list_move(&client->list_node, &polar_my_dispatcher->hangout_clients_list);
	client->is_disconnected = true;

	ELOG_PSS(DEBUG5, "client_channel_hangout %p, %d", client, client->session->id);
}

/*
 * Handle communication failure for this channel.
 * It is not possible to remove channel immediately because it can be triggered by other epoll events.
 * So link all channels in L1 list for pending delete.
 */
static void
backend_channel_hangout(BackendChannel* backend)
{
	if (backend->is_disconnected)
	   return;

	if (backend->client)
	{
		backend->client->backend = NULL;
		backend->client = NULL;
		if (backend->backend_proc)
			backend->backend_proc->polar_shared_session = NULL;
	}

	if (!list_empty(&backend->idle_list_node))
	{
		list_del_init(&backend->idle_list_node);
		backend->pool->n_idle_backends -= 1;
		backend->pool->dispatcher->proc->n_idle_backends -= 1;
		backend->is_idle = false;
	}

	list_move(&backend->list_node, &backend->dispatcher->hangout_backends_list);
	backend->backend_is_ready = false;
	backend->is_disconnected = true;
	ELOG_PSS(DEBUG5, "backend_channel_hangout %p, %d", backend, backend->backend_pid);
}

static
PT_THREAD(client_channel_write_self(ClientChannel* client))
{
	while (client->tx_pos < client->tx_size)
	{
		ssize_t rc;
#ifdef USE_SSL
		if (client->session->info->client_port->ssl_in_use)
		{
			int waitfor = 0;
			rc = be_tls_write(client->session->info->client_port, 
				client->buf + client->tx_pos, client->tx_size - client->tx_pos, 
				&waitfor);
		}
		else
#endif
			rc = secure_raw_write(client->session->info->client_port, 
				client->buf + client->tx_pos, client->tx_size - client->tx_pos);

		if (unlikely(rc <= 0))
		{
			if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				/* register event WL_SOCKET_WRITEABLE only when needed. */
				if (!(client->events & WL_SOCKET_WRITEABLE))
				{  
					client->events |= WL_SOCKET_WRITEABLE;
					ModifyWaitEvent(client->dispatcher->wait_events, client->event_pos, client->events, NULL);
				}
				return PT_WAITING;
			}
			return PT_EXITED_ERROR;
		}
		client->tx_pos += rc;
		client->dispatcher->proc->tx_bytes += rc;
	}

	/* all write OK, remove WL_SOCKET_WRITEABLE if exits. */
	if (unlikely(client->events & WL_SOCKET_WRITEABLE))
	{
		client->events &= ~WL_SOCKET_WRITEABLE;
		ModifyWaitEvent(client->dispatcher->wait_events, client->event_pos, client->events, NULL);
	}

	if (likely(client->tx_size != 0))
	{
		/* Copy rest of received data to the beginning of the buffer */
		memmove(client->buf, client->buf + client->tx_size, client->rx_pos - client->tx_size);
		client->rx_pos -= client->tx_size;
		client->tx_pos = client->tx_size = 0;
	}
	ELOG_PSS(DEBUG5, "client_channel_write_self 1 %p", client);

	return PT_ENDED;
}


/*
 * Try to send some data to the client.
 */
static
PT_THREAD(client_channel_write(ClientChannel* client))
{
	BackendChannel* peer = client->backend;
	ELOG_PSS(DEBUG5, "client_channel_write 0 %p", client);

	Assert(peer != NULL);
	while (peer->tx_pos < peer->tx_size) /* has something to write */
	{
		ssize_t rc;
#ifdef USE_SSL
		int waitfor = 0;
		if (client->session->info->client_port->ssl_in_use)
			rc = be_tls_write(client->session->info->client_port, peer->buf + peer->tx_pos, peer->tx_size - peer->tx_pos, &waitfor);
		else
#endif
			rc = secure_raw_write(client->session->info->client_port, peer->buf + peer->tx_pos, peer->tx_size - peer->tx_pos);

		ELOG_PSS(DEBUG5, "%p: write %d tx_pos=%d, tx_size=%d, errno=%m", client, (int)rc, peer->tx_pos, peer->tx_size);

		if (unlikely(rc <= 0))
		{
			if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				/* register event WL_SOCKET_WRITEABLE only when needed. */
				if (!(client->events & WL_SOCKET_WRITEABLE))
				{  
					client->events |= WL_SOCKET_WRITEABLE;
					ModifyWaitEvent(client->dispatcher->wait_events, client->event_pos, client->events, NULL);
				}
				return PT_WAITING;
			}
			return PT_EXITED_ERROR;
		}

		ELOG_PSS(DEBUG5, "Send reply %c to client %d from backend %d (%p:ready=%d)", 
			peer->buf[peer->tx_pos], client->session->info->client_port->sock,
			peer->backend_pid, peer, peer->backend_is_ready);

		peer->tx_pos += rc;
		client->dispatcher->proc->tx_bytes += rc;
	}

	/* all write OK, remove WL_SOCKET_WRITEABLE if exits. */
	if (unlikely(client->events & WL_SOCKET_WRITEABLE))
	{
		client->events &= ~WL_SOCKET_WRITEABLE;
		ModifyWaitEvent(client->dispatcher->wait_events, client->event_pos, client->events, NULL);
	}

	if (likely(peer->tx_size != 0))
	{
		/* Copy rest of received data to the beginning of the buffer */
		Assert(peer->rx_pos >= peer->tx_size);
		memmove(peer->buf, peer->buf + peer->tx_size, peer->rx_pos - peer->tx_size);
		peer->rx_pos -= peer->tx_size;
		peer->tx_pos = peer->tx_size = 0;
	}
	ELOG_PSS(DEBUG5, "client_channel_write 1 %p", client);

	return PT_ENDED;
}

/*
 * Try to send some data to the channel.
 */
static
PT_THREAD(backend_channel_write(BackendChannel* backend))
{
	ClientChannel* client;
	client = backend->client;

	ELOG_PSS(DEBUG5, "backend_channel_write 0 %p", backend);

	while (client->tx_pos < client->tx_size) /* has something to write */
	{
		ssize_t rc = send(backend->backend_socket, client->buf + client->tx_pos, client->tx_size - client->tx_pos, 0);
		ELOG_PSS(DEBUG5, "%p: write %d tx_pos=%d, tx_size=%d: error=%m", backend, (int)rc, client->tx_pos, client->tx_size);

		if (unlikely(rc <= 0))
		{
			if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				/* register event WL_SOCKET_WRITEABLE only when needed. */
				if (!(backend->events & WL_SOCKET_WRITEABLE))
				{
					backend->events |= WL_SOCKET_WRITEABLE;
					ModifyWaitEvent(backend->dispatcher->wait_events, backend->event_pos, backend->events, NULL);
				}
				return PT_WAITING;
			}
			client->is_backend_error = true;
			return PT_EXITED_ERROR;
		}

		ELOG_PSS(DEBUG5, "%p Send command %c from client %d to backend %d (%p:ready=%d)", 
			backend, client->buf[client->tx_pos], client->session->info->client_port->sock,
			 backend->backend_pid, backend, backend->backend_is_ready);

		client->tx_pos += rc;
		backend->backend_is_ready = false;
		backend->dispatcher->proc->rx_bytes += rc;
	}

	/* all write OK, remove WL_SOCKET_WRITEABLE if exits. */
	if (unlikely(backend->events & WL_SOCKET_WRITEABLE))
	{
		backend->events &= ~WL_SOCKET_WRITEABLE;
		ModifyWaitEvent(backend->dispatcher->wait_events, backend->event_pos, backend->events, NULL);
	}

	if (likely(client->tx_size != 0))
	{
		/* Copy rest of received data to the beginning of the buffer */
		Assert(client->rx_pos >= client->tx_size);
		memmove(client->buf, client->buf + client->tx_size, client->rx_pos - client->tx_size);
		client->rx_pos -= client->tx_size;
		client->tx_pos = client->tx_size = 0;
	}
	ELOG_PSS(DEBUG5, "backend_channel_write 1 %p", backend);

	return PT_ENDED;
}

static
PT_THREAD(postmaster_channel_write(PostmasterChannel* post))
{
	static PT_RESULT pt_result;
	PT_BEGIN(&post->pt);
	while(true)
	{
		if (!post->pending_clients)
		{
			PT_REMOVE_WRITE_EVENT(post);
			PT_YIELD(&post->pt);
			continue;
		}

		PT_WAIT_THREAD_HANDLE_ERROR(
			&post->pt,
			(({
					PolarChannelType type = *(PolarChannelType*)linitial(post->pending_clients);
					pgsocket sock_to_send;
					ssize_t  rc;

					if (PCT_Client == type)
					{
						ClientChannel* client = (ClientChannel*)linitial(post->pending_clients);
						sock_to_send = client->session->info->client_port->sock;
					}
					else
					{
						BackendChannel* backend = (BackendChannel*)linitial(post->pending_clients);
						sock_to_send = backend->pipes[0];
						pg_set_block(post->post_sock);
					}

					rc = polar_pg_send_sock(post->post_sock, sock_to_send);

					if (PCT_Client == type)
						pg_set_noblock(post->post_sock);

					if (rc > 0)
						pt_result = PT_ENDED;
					else
						PT_HANDLE_NETIO_WRITE_ERROR(post);
				}),
				pt_result)
			);

		PT_WAIT_THREAD_HANDLE_ERROR(
			&post->pt,
			(({
					PolarChannelType type = *(PolarChannelType*)linitial(post->pending_clients);
					ssize_t        rc;

					if (PCT_Client == type)
					{
						ClientChannel* client = (ClientChannel*)linitial(post->pending_clients);
						rc = send(post->post_sock, client->buf + client->tx_pos, client->tx_size - client->tx_pos, 0);
						if (rc > 0)
						{
							client->tx_pos += rc;
							if (client->tx_pos == client->tx_size)
								pt_result = PT_ENDED;
							else
								pt_result = PT_WAITING;
						}
						else
							PT_HANDLE_NETIO_WRITE_ERROR(post);
					}
					else
					{
						BackendChannel* backend = (BackendChannel*)linitial(post->pending_clients);
						rc = send(post->post_sock, backend->buf + backend->tx_pos, backend->tx_size - backend->tx_pos, 0);
						if (rc > 0)
						{
							backend->tx_pos += rc;
							if (backend->tx_pos == backend->tx_size)
								pt_result = PT_ENDED;
							else
								pt_result = PT_WAITING;
						}
						else
							PT_HANDLE_NETIO_WRITE_ERROR(post);
					}
				}),
				pt_result)
			);

		/* remove first client */
		{
			void* chan = linitial(post->pending_clients);
			PolarChannelType type = *(PolarChannelType*)chan;
			post->pending_clients = list_delete_first(post->pending_clients);
			if (PCT_Client == type)
			{
				ClientChannel* client = (ClientChannel*)chan;
				client->is_wait_send_to_postmaster = false;
			}
		}
	} /* while(true) */

error:
	Assert(false); /* socketpair between postmaster an dispatcher should not have error. */
	PT_END(&post->pt);
}

/*
 * Try to read more data from the channel.
 */
static
PT_THREAD(client_channel_read(ClientChannel* client, size_t max_len))
{
	ssize_t rc;
	size_t read_len = client->buf_size - client->rx_pos;
	if (unlikely(max_len != 0))
		read_len = Min(read_len, max_len);

#ifdef USE_SSL
	if (client->session->info->client_port->ssl_in_use)
	{
		int waitfor = 0;
		rc = be_tls_read(client->session->info->client_port, client->buf + client->rx_pos, read_len, &waitfor);
	}
	else
#endif
		rc = secure_raw_read(client->session->info->client_port, client->buf + client->rx_pos, read_len);

	ELOG_PSS(DEBUG5, "%p: client_channel_read %d, read_len=%lu, error: %m", client, (int)rc, read_len);

	if (unlikely(rc <= 0))
	{
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return PT_WAITING;
		/*
		 * rc == 0 means EOF (client closed connection).
		 * rc < 0 with other errno means error.
		 * In both cases, treat as error to close the channel.
		 */
		ELOG_PSS(DEBUG1, "%p: client_channel_read EOF or error: rc=%d: %m", client, (int)rc);
		return PT_EXITED_ERROR;
	}

	ELOG_PSS(DEBUG5, "Receive command %c %d bytes from client %d to backend %d (%p:ready=%d)",
		 client->buf[0] ? client->buf[0] : '?', (int)rc + client->rx_pos, 
		 client->session->info->client_port->sock, 
		 client->backend ? client->backend->backend_pid : -1, 
		 client->backend, 
		 client->backend ? client->backend->backend_is_ready : -1);

	client->rx_pos += rc;
	ELOG_PSS(DEBUG5, "client_channel_read 1 %p", client);

	return PT_ENDED;
}

/*
 * Try to read more data from the channel and send it to the peer.
 */
static
PT_THREAD(backend_channel_read(BackendChannel* backend))
{
	ssize_t rc;
	rc = recv(backend->backend_socket, backend->buf + backend->rx_pos, backend->buf_size - backend->rx_pos, 0);

	if (unlikely(rc <= 0))
	{
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return PT_WAITING;
		if (backend->client)
			backend->client->is_backend_error = true;

		return PT_EXITED_ERROR;
	}

	ELOG_PSS(DEBUG5, "Receive reply %c %d bytes from backend %d (%p:ready=%d) to client %d", 
		backend->buf[0] ? backend->buf[0] : '?',
		(int)rc + backend->rx_pos, backend->backend_pid, backend,
		backend->backend_is_ready, 
		(backend->client && backend->client->session) ? backend->client->session->info->client_port->sock : -1);

	backend->rx_pos += rc;
	ELOG_PSS(DEBUG5, "backend_channel_read 1 %p", backend);

	return PT_ENDED;
}

/*
 * Create new ClientChannel.
 */
static ClientChannel*
client_channel_create(PolarDispatcher* dispatcher, pgsocket client_sock)
{
	ClientChannel* client = (ClientChannel*)palloc0(sizeof(ClientChannel));
	client->type = PCT_Client;
	client->magic = CHANNEL_MAGIC;
	client->dispatcher = dispatcher;
	client->buf = palloc(INIT_BUF_SIZE);
	client->buf_size = INIT_BUF_SIZE;
	client->tx_pos = client->rx_pos = client->tx_size = 0;
	client->received_command_count = 0;
	client->event_pos = -1;
	client->is_backend_error = false;
	PT_INIT(&client->pt);
	INIT_LIST_HEAD(&client->list_node);
	INIT_LIST_HEAD(&client->pending_list_node);

	client->session = polar_session_create(true);
	client->session->info->shm_inval_buffer = polar_my_dispatcher_siseg_buffer;
	client->session->info->nextMsgNum = polar_my_dispatcher_siseg_buffer->maxMsgNum;
	client->session->info->client_port = (Port *)MemoryContextAllocZero(client->session->memory_context, sizeof(Port));
	client->session->info->client_port->sock = client_sock;
	client->session->info->client = client;
	client->start_timestamp = approximate_current_timestamp;
#if defined(ENABLE_GSS) || defined(ENABLE_SSPI)
	client->session->info->client_port->gss = (pg_gssinfo *)MemoryContextAllocZero(client->session->memory_context, sizeof(pg_gssinfo));
#endif

	if (!RandomCancelKey(&client->session->info->cancel_key))
	{
		ereport(LOG, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("could not generate random cancel key")));
		client->session->info->cancel_key = MyCancelKey;
	}
	ELOG_PSS(DEBUG1, "client_channel_create %p:%d", client, client->session->session_id);
	return client;
}

/*
 * Create new BackendChannel.
 */
static BackendChannel*
backend_channel_create(PolarDispatcher* dispatcher)
{
	BackendChannel* backend = (BackendChannel*)palloc0(sizeof(BackendChannel));
	backend->type = PCT_Backend;
	backend->magic = CHANNEL_MAGIC;
	backend->dispatcher = dispatcher;
	backend->buf = palloc(INIT_BUF_SIZE);
	backend->buf_size = INIT_BUF_SIZE;
	backend->tx_pos = backend->rx_pos = backend->tx_size = 0;
	backend->event_pos = -1;
	INIT_LIST_HEAD(&backend->list_node);
	INIT_LIST_HEAD(&backend->idle_list_node);
	PT_INIT(&backend->pt);
	return backend;
}

/*
 * Register new client channel in wait event set.
 */
static bool
client_channel_register(PolarDispatcher* dispatcher, ClientChannel* client)
{
	pgsocket sock = client->session->info->client_port->sock;
	/* Using non-blocking sockets for async I/O */
	pg_set_noblock(sock);
	/* Use level-triggered mode with WL_SOCKET_CLOSED to detect client disconnect */
	client->events = WL_SOCKET_READABLE | WL_SOCKET_CLOSED;
	client->event_pos = AddWaitEventToSet(polar_my_dispatcher->wait_events, client->events, sock, NULL, client);
	if (client->event_pos < 0)
	{
		elog(WARNING, "Dispatcher: Failed to add new client - too much sessions: %d clients, %d backends. "
					 "Try to increase 'max_connections' configuration parameter.",
					 polar_my_dispatcher->proc->n_clients, polar_my_dispatcher->proc->n_backends);
		return false;
	}
	return true;
}

/*
 * Register new backend channel in wait event set.
 */
static bool
backend_channel_register(PolarDispatcher* dispatcher, BackendChannel* backend)
{
	pgsocket sock = backend->backend_socket;
	pg_set_noblock(sock);
	/* Use level-triggered mode with WL_SOCKET_CLOSED to detect backend disconnect */
	backend->events = WL_SOCKET_READABLE | WL_SOCKET_CLOSED;
	backend->event_pos = AddWaitEventToSet(polar_my_dispatcher->wait_events, backend->events, sock, NULL, backend);
	if (backend->event_pos < 0)
	{
		elog(WARNING, "Dispatcher: Failed to add new backend - too much sessions: %d clients, %d backends. "
					 "Try to increase 'max_sessions' configuration parameter.",
					 polar_my_dispatcher->proc->n_clients, polar_my_dispatcher->proc->n_backends);
		return false;
	}
	return true;
}

/*
 * Start new backend for particular pool associated with dbname/role combination.
 * Backend is forked using BackendStartup function.
 */
static
PT_THREAD(launch_new_backend(SessionPool* pool))
{
	BackendChannel* backend;
	backend = backend_channel_create(pool->dispatcher);
	backend->pool = pool;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, backend->pipes) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not create socket pair for launching shared backend: %m")));
	backend->backend_socket = backend->pipes[1];
	pool->dispatcher->proc->n_backends += 1;
	pool->n_backends += 1;

	list_add(&backend->list_node, &pool->live_backends_list);

	if (!backend_channel_register(pool->dispatcher, backend))
	{
		elog(WARNING, "dispatcher launch shared backend failed, too much sessions: "
			 "try to increase 'max_connections' configuration parameter.");
		backend_channel_hangout(backend);
		return PT_EXITED_ERROR;
	}
	fiber_backend_main(backend, BACKEND_WRITEABLE);
	return PT_ENDED;
}

/*
 * Set port's sock attr and fill port's addr info.
 * Correspondint to StreamConnection() and BackendInitialize().
 */
static bool
port_connection_initialize(Port *port, MemoryContext mctx)
{
	int         ret;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	if (StreamConnection(0, port, true) != STATUS_OK)
		return false;

	/* save process start time */
	port->SessionStartTime = GetCurrentTimestamp();

	/* set these to empty in case they are needed before we set them up */
	port->remote_host = "";
	port->remote_port = "";

	/* POLAR orgin connection info init */
	MemSet(&port->polar_proxy_client_raddr, 0, sizeof(port->polar_proxy_client_raddr));
	port->polar_proxy = false;
	port->polar_proxy_send_lsn = false;
	port->polar_proxy_ssl_in_use = false;
	port->polar_proxy_ssl_cipher_name = NULL;
	port->polar_proxy_ssl_version = NULL;
	/* POLAR end */

	/*
	 * Get the remote host name and port for logging and status display.
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if ((ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								  remote_host, sizeof(remote_host),
								  remote_port, sizeof(remote_port),
								  (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV)) != 0)
		ereport(WARNING,
				(errmsg_internal("pg_getnameinfo_all() failed: %s",
								 gai_strerror(ret))));
	/*
	 * Save remote_host and remote_port in port structure (after this, they
	 * will appear in log_line_prefix data for log messages).
	 */
	port->remote_host = MemoryContextStrdup(mctx, remote_host);
	port->remote_port = MemoryContextStrdup(mctx, remote_port);

	/* And now we can issue the Log_connections message, if wanted */
	if (Log_connections)
	{
		if (remote_port[0])
			ereport(LOG,
					(errmsg("connection received: host=%s port=%s",
							remote_host,
							remote_port)));
		else
			ereport(LOG,
					(errmsg("connection received: host=%s",
							remote_host)));
	}

	/*
	 * If we did a reverse lookup to name, we might as well save the results
	 * rather than possibly repeating the lookup during authentication.
	 *
	 * Note that we don't want to specify NI_NAMEREQD above, because then we'd
	 * get nothing useful for a client without an rDNS entry.  Therefore, we
	 * must check whether we got a numeric IPv4 or IPv6 address, and not save
	 * it into remote_hostname if so.  (This test is conservative and might
	 * sometimes classify a hostname as numeric, but an error in that
	 * direction is safe; it only results in a possible extra lookup.)
	 */
	if (log_hostname &&
		ret == 0 &&
		strspn(remote_host, "0123456789.") < strlen(remote_host) &&
		strspn(remote_host, "0123456789ABCDEFabcdef:") < strlen(remote_host))
		port->remote_hostname = MemoryContextStrdup(mctx, remote_host);

	return true;
}

/*
 * Add new client accepted by postmaster. This client will be assigned to concrete session pool
 * when it's startup packet is received.
 */
static void
add_client(PolarDispatcher* dispatcher, pgsocket client_sock)
{
	ClientChannel	*client = NULL;
	if (polar_session_id_full())
	{
		struct Port *client_port = (struct Port *)malloc(sizeof(struct Port));
		memset(client_port, 0, sizeof(struct Port));
		client_port->sock = client_sock;
		report_error_to_client(client_port, "Too many sessions");
		free(client_port);
		goto error;
	}

	client = client_channel_create(dispatcher, client_sock);
	if (!port_connection_initialize(client->session->info->client_port, client->session->memory_context))
	{
		report_error_to_client(client->session->info->client_port, "failed to port_connection_initialize");
		goto error;
	}

	if (client_channel_register(dispatcher, client))
	{
		ELOG_PSS(DEBUG1, "Add new client %p:%d", client, client->session->session_id);
		polar_my_dispatcher->proc->n_startup_clients += 1;
		polar_my_dispatcher->proc->n_clients += 1;
	}
	else
	{
		report_error_to_client(client->session->info->client_port, "Too many sessions. Try to increase 'max_connections' configuration parameter");
		goto error;
	}
	return;

 error:
	/* Too many sessions, error report was already logged */
	closesocket(client_sock);
	if (client != NULL)
	{
		polar_session_delete(client->session);
		pfree(client->buf);
		pfree(client);
	}
	return;
}

/*
 * Perform delayed deletion of channel
 */
static void
client_channel_remove(ClientChannel* client)
{
	Assert(client->is_disconnected); /* should be marked as disconnected by channel_hangout */

	ELOG_PSS(DEBUG5, "client_channel_remove %p, %d", client, client->session->id);

	if (client->pool)
		client->pool->n_clients -= 1;
	else
		client->dispatcher->proc->n_startup_clients -= 1;
	client->dispatcher->proc->n_clients -= 1;
	client->dispatcher->proc->n_ssl_clients -= client->session->info->client_port->ssl_in_use;

	closesocket(client->session->info->client_port->sock);
	polar_session_delete(client->session);
	pfree(client->buf);
 	pfree(client);
}

/*
 * Perform delayed deletion of channel
 */
static void
backend_channel_remove(BackendChannel* backend)
{
	SessionPool* pool = backend->pool;
	Assert(backend->is_disconnected); /* should be marked as disconnected by channel_hangout */

	ELOG_PSS(DEBUG5, "backend_channel_remove %p, %d", backend, backend->backend_pid);

	if (backend->event_pos >= 0)
	{
		DeleteWaitEventFromSet(backend->dispatcher->wait_events, backend->event_pos);
		backend->event_pos = -1;
	}
	closesocket(backend->pipes[1]);

	backend->dispatcher->proc->n_backends -= 1;
	backend->dispatcher->proc->n_dedicated_backends -= backend->is_private;
	backend->pool->n_backends -= 1;
	backend->pool->n_dedicated_backends -= backend->is_private;
	if (backend->handshake_response)
		pfree(backend->handshake_response);
	pfree(backend->buf);
	pfree(backend);

	if (!list_empty(&pool->pending_clients_list))
	{
		if (pool->n_backends < pool->dispatcher->max_backends)
			launch_new_backend(pool);
	}
}

/*
 * Create new dispatcher.
 */
static PolarDispatcher*
dispatcher_create(PolarDispatcherProc* proc)
{
	HASHCTL ctl;
	PolarDispatcher*	dispatcher;
	MemoryContext memctx = AllocSetContextCreate(TopMemoryContext,
												"PolarDispatcher",
												ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(memctx);
	dispatcher = palloc0(sizeof(PolarDispatcher));
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(SessionPoolKey);
	ctl.entrysize = sizeof(SessionPool);
	ctl.hcxt = memctx;
	dispatcher->memory_context = memctx;
	dispatcher->pools = hash_create("Pool by database and user", DB_HASH_SIZE,
							   &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	 /* We need events both for clients and backends so multiply MaxConnection by two */
	dispatcher->wait_events = CreateWaitEventSet(TopMemoryContext, MaxPolarSessionsPerDispatcher * 2 + 1);
	dispatcher->max_backends = MaxPolarSharedBackendsPerDispatcher;
	INIT_LIST_HEAD(&dispatcher->hangout_clients_list);
	INIT_LIST_HEAD(&dispatcher->hangout_backends_list);
	dispatcher->proc = proc;

	dispatcher->postmaster.type = PCT_Dispatcher;
	dispatcher->postmaster.magic = CHANNEL_MAGIC;
	dispatcher->postmaster.post_sock = proc->pipes[1];
	pg_set_noblock(dispatcher->postmaster.post_sock);
	dispatcher->postmaster.events = WL_SOCKET_READABLE;
	dispatcher->postmaster.event_pos = AddWaitEventToSet(dispatcher->wait_events,
														 dispatcher->postmaster.events,
														 dispatcher->postmaster.post_sock,
														 NULL,
														 &dispatcher->postmaster);
	dispatcher->postmaster.pending_clients = NULL;
	PT_INIT(&dispatcher->postmaster.pt);
	return dispatcher;
}


static void
check_session_signal()
{
	int local_id = -1;
	int n_signalling_clients = 0;
	Bitmapset *signalling_sessions = NULL;

	SpinLockAcquire(&polar_my_dispatcher->proc->signalling_session_lock);
	if (polar_my_dispatcher->proc->n_signalling_clients > 0)
	{
		n_signalling_clients = polar_my_dispatcher->proc->n_signalling_clients;
		signalling_sessions = bms_copy(polar_my_dispatcher->proc->signalling_sessions);

		polar_bms_reset(polar_my_dispatcher->proc->signalling_sessions, MaxPolarSessionsPerDispatcher);
		polar_my_dispatcher->proc->n_signalling_clients = 0;
	}
	SpinLockRelease(&polar_my_dispatcher->proc->signalling_session_lock);

	if (n_signalling_clients <= 0)
		return ;

	ELOG_PSS(DEBUG1, "n_signalling_clients: %d:%d", polar_my_dispatcher_id, n_signalling_clients);
	while ((local_id = bms_next_member(signalling_sessions, local_id)) >= 0)
	{
		int id = local_id + polar_my_dispatcher_id * MaxPolarSessionsPerDispatcher;
		PolarSessionContext *session = &polar_session_contexts[id];
		int status;

		n_signalling_clients--;

		if ((status = pg_atomic_read_u32(&session->status)) > PSSE_ACTIVE)
			continue;

		ELOG_PSS(LOG, "cancel session %d, %d, %d, active:%d, count:%d", 
			session->session_id, session->current_pid, session->signal,
			status, n_signalling_clients);

		SpinLockAcquire(&session->holding_lock);
		if ((status = pg_atomic_read_u32(&session->status)) == PSSE_ACTIVE)
		{
			int kill_result;
			if (session->signal > __SIGRTMAX)
				kill_result = SendProcSignal(session->current_pid, 
					session->signal - __SIGRTMAX, InvalidBackendId);
			else
			{
#ifdef HAVE_SETSID
				kill_result = kill(-session->current_pid, session->signal);
#else
				kill_result = kill(session->current_pid, session->signal);
#endif
			}
			session->signal = -1;
			if (kill_result)
				ereport(WARNING,
						(errmsg("could not send signal to process %d: %m", session->current_pid)));
		}
		else if (status == PSSE_IDLE)
		{
			if (session->signal == __SIGRTMAX + POLAR_PROCSIG_BACKEND_MEMORY_CONTEXT)
			{
				polar_monitor_hook(POLAR_SS_CHECK_SIGNAL_MCTX, session);
			}
			else if (session->signal == SIGTERM)
			{
				session->info->client->is_terminating = true;
				SpinLockRelease(&session->holding_lock);

				fiber_client_main(session->info->client, CLIENT_TERMINATING);
			}
			else if (session->signal > __SIGRTMAX)
			{
				ereport(WARNING,
					(errmsg("polar shared server not support signal %d now for process %d: %m", 
					session->signal - __SIGRTMAX, session->current_pid)));
			}
			else
			{
				/* do nothing */
			}
			session->signal = -1;
		}
		SpinLockRelease(&session->holding_lock);
	}

	if (signalling_sessions)
	{
		pfree(signalling_sessions);
		Assert(n_signalling_clients == 0);
	}
}

/*
 * Main dispatcher loop
 */
static void
dispatcher_loop()
{
	int i, n_ready;
	WaitEvent ready[MAX_READY_EVENTS];
	approximate_current_timestamp = GetCurrentTimestamp();

	/* Main loop */
	while (!polar_my_dispatcher->shutdown)
	{
		ClientChannel*  client;
		BackendChannel* backend;

		/*
		 * SIGHUP received, relaod config file.
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
			mark_all_session_reload_config();
		}

		n_ready = WaitEventSetWait(polar_my_dispatcher->wait_events, 
			DISPATCHER_WAIT_TIMEOUT, ready, MAX_READY_EVENTS, PG_WAIT_CLIENT);
		approximate_current_timestamp = GetCurrentTimestamp();
		for (i = 0; i < n_ready; i++)
		{
			PolarChannelType type = *((PolarChannelType*)ready[i].user_data);
			switch(type)
			{
				case PCT_Dispatcher:
				{
					PostmasterChannel* post = (PostmasterChannel*)ready[i].user_data;
					if (unlikely(ready[i].events & WL_SOCKET_WRITEABLE))
						postmaster_channel_write(post);

					  /* new fd from postmaster */
					if (likely(ready[i].events & WL_SOCKET_READABLE))
					{
						pgsocket client_sock = polar_pg_recv_sock(ready[i].fd);
						if (client_sock != PGINVALID_SOCKET)
							add_client(polar_my_dispatcher, client_sock);
						else
							elog(WARNING, "Failed to receive session socket: %m");
					}
				}
				break;

				case PCT_Client:
				{
					client = (ClientChannel*)ready[i].user_data;
					/* Handle client socket close/error - remove channel */
					if (unlikely(ready[i].events & WL_SOCKET_CLOSED))
					{
						ELOG_PSS(DEBUG1, "Client socket closed: %p", client);
						client_channel_hangout(client);
						break;
					}
					if (unlikely(ready[i].events & WL_SOCKET_WRITEABLE))
						fiber_client_main(client, CLIENT_WRITEABLE);
					if (likely(ready[i].events & WL_SOCKET_READABLE))
						fiber_client_main(client, CLIENT_READABLE);
				}
				break;

				case PCT_Backend:
				{
					backend = (BackendChannel*)ready[i].user_data;
					/* Handle backend socket close/error - remove channel */
					if (unlikely(ready[i].events & WL_SOCKET_CLOSED))
					{
						ELOG_PSS(DEBUG1, "Backend socket closed: %p (pid %d)", backend, backend->backend_pid);
						backend_channel_hangout(backend);
						break;
					}
					if (unlikely(ready[i].events & WL_SOCKET_WRITEABLE))
						fiber_backend_main(backend, BACKEND_WRITEABLE);
					if (likely(ready[i].events & WL_SOCKET_READABLE))
						fiber_backend_main(backend, BACKEND_READABLE);
				}
				break;

				default:
					Assert(false);
			} /* switch(type) */
		} /* for (i = 0; i < n_ready; i++) */

		CHECK_FOR_INTERRUPTS();

		if (polar_ss_backend_idle_timeout)
		{
			TimestampTz now = approximate_current_timestamp;
			TimestampTz timeout_usec = polar_ss_backend_idle_timeout * USECS_PER_SEC;
			if (polar_my_dispatcher->last_idle_timeout_check + timeout_usec < now)
			{
				HASH_SEQ_STATUS     seq;
				struct SessionPool* pool;
				BackendChannel*     backend;
				BackendChannel* 	tmp_backend;
				int					remove_count = 0;
				polar_my_dispatcher->last_idle_timeout_check = now;
				hash_seq_init(&seq, polar_my_dispatcher->pools);
				while ((pool = hash_seq_search(&seq)) != NULL)
				{
					list_for_each_entry_safe(backend, tmp_backend, &pool->idle_backends_list, idle_list_node)
					{
						Assert(backend->type == PCT_Backend);
						if (backend->backend_last_activity + timeout_usec < now &&
							(polar_ss_backend_pool_min_size == 0 ||
							 pool->n_backends - remove_count > polar_ss_backend_pool_min_size))
						{
							ELOG_PSS(DEBUG5, "idle timeout %p, %d", backend, backend->backend_pid);
							backend_channel_hangout(backend);
							remove_count++;
						}
					}

					if (pool->n_backends == 0 &&
						pool->n_dedicated_backends == 0 &&
						pool->n_idle_backends == 0 &&
						pool->n_clients == 0 &&
						pool->n_idle_clients == 0 &&
						pool->n_pending_clients == 0)
					{
						ELOG_PSS(DEBUG1, "remove empty session pool %s-%s-%u",
								pool->key.database, pool->key.username, pool->key.polar_startup_gucs_hash);
						hash_search(polar_my_dispatcher->pools, &pool->key, HASH_REMOVE, NULL);
					}
				}
			}
		}

		if (polar_ss_session_wait_timeout)
		{
			TimestampTz now = approximate_current_timestamp;
			TimestampTz timeout_usec = polar_ss_session_wait_timeout * USECS_PER_MSEC;
			if (polar_my_dispatcher->last_wait_timeout_check + timeout_usec < now)
			{
				HASH_SEQ_STATUS     seq;
				struct SessionPool* pool;
				ClientChannel*     client;
				ClientChannel* 	tmp_client;
				polar_my_dispatcher->last_wait_timeout_check = now;
				hash_seq_init(&seq, polar_my_dispatcher->pools);
				while ((pool = hash_seq_search(&seq)) != NULL)
				{
					list_for_each_entry_safe(client, tmp_client, &pool->pending_clients_list, pending_list_node)
					{
						Assert(client->type == PCT_Client);
						if (client->session->last_wait_start_timestamp + timeout_usec < now)
						{
							ELOG_PSS(DEBUG5, "wait timeout %p, %d", client, client->session->session_id);
							report_error_to_client(client->session->info->client_port, "wait idle backend timeout");
							client_channel_hangout(client);
						}
					}
				}
			}
		}

		/*
		 * Delayed deallocation of disconnected channels.
		 * We can not delete channels immediately because of presence of peer events.
		 */
		if (!list_empty(&polar_my_dispatcher->hangout_clients_list))
		{
			ClientChannel* client;
			ClientChannel* tmp_client;
			list_for_each_entry_safe(client, tmp_client, &polar_my_dispatcher->hangout_clients_list, list_node)
			{
				if (!client->is_wait_send_to_postmaster)
				{
					list_del_init(&client->list_node);
					client_channel_remove(client);
				}
			}
		}

		if (!list_empty(&polar_my_dispatcher->hangout_backends_list))
		{
			BackendChannel* backend;
			BackendChannel* tmp_backend;
			list_for_each_entry_safe(backend, tmp_backend, &polar_my_dispatcher->hangout_backends_list, list_node)
			{
				list_del_init(&backend->list_node);
				backend_channel_remove(backend);
			}
		}

		check_session_signal();

		ReceiveSharedInvalidMessages(local_execute_invalidation_message, inval_all);

		CHECK_FOR_INTERRUPTS();
	}

/* cleanup: */
	/* Cleanup */
	{
		HASH_SEQ_STATUS   seq;
		SessionPool*      pool;
		ClientChannel* 		client;
		ClientChannel* 		tmp_client;
		BackendChannel* 	backend;
		BackendChannel* 	tmp_backend;
		ELOG_PSS(DEBUG1, "dispatcher exit  %d", polar_my_dispatcher_id);

		hash_seq_init(&seq, polar_my_dispatcher->pools);
		while ((pool = hash_seq_search(&seq)) != NULL)
		{
			list_for_each_entry_safe(client, tmp_client, &pool->live_clients_list, list_node)
			{
				Assert(client->type == PCT_Client);
				closesocket(client->session->info->client_port->sock);
				polar_session_delete(client->session);
				pfree(client->buf);
			 	pfree(client);
			}
			
			list_for_each_entry_safe(backend, tmp_backend, &pool->live_backends_list, list_node)
			{
				Assert(backend->type == PCT_Backend);
				closesocket(backend->pipes[0]);
				closesocket(backend->pipes[1]);
			}
		}
		closesocket(polar_my_dispatcher->postmaster.post_sock);

		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextDelete(polar_my_dispatcher->memory_context);
	}
}

/*
 * Handle normal shutdown of Postgres instance
 */
static void
handle_sigterm(SIGNAL_ARGS)
{
	if (polar_my_dispatcher)
		polar_my_dispatcher->shutdown = true;
}

static void
handle_sighup(SIGNAL_ARGS)
{
	ConfigReloadPending = true;
}

void
dispatcher_main(int argc, char *argv[])
{
	sigjmp_buf	local_sigjmp_buf;

	/* Identify myself via ps */
	init_ps_display("PSS dispatcher");
	IsPolarDispatcher = true;

	ereport(DEBUG1, (errmsg("PSS dispatcher started")));

	SetProcessingMode(InitProcessing);

	pqsignal(SIGHUP, handle_sighup);
	pqsignal(SIGTERM, handle_sigterm);
	pqsignal(SIGQUIT, quickdie);
	pqsignal(SIGPIPE, SIG_IGN);	
	pqsignal(SIGINT, SIG_IGN);

	InitializeTimeouts();		/* establishes SIGALRM handler */

	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	/* POLAR: check interrupts when allocating memory signal */
	pqsignal(SIGUSR2, polar_procsignal_sigusr2_handler);
	/* POLAR end */
	pqsignal(SIGFPE, FloatExceptionHandler);
	
#ifndef _WIN32
#ifdef SIGILL
	pqsignal(SIGILL, polar_program_error_handler);
#endif
#ifdef SIGSEGV
	pqsignal(SIGSEGV, polar_program_error_handler);
#endif
#ifdef SIGBUS
	pqsignal(SIGBUS, polar_program_error_handler);
#endif
#endif	/* _WIN32 */

	/*
	 * Create a per-backend PGPROC struct in shared memory, except in the
	 * EXEC_BACKEND case where this was done in SubPostmasterMain. We must do
	 * this before we can use LWLocks (and in the EXEC_BACKEND case we already
	 * had to do some stuff with LWLocks).
	 */
#ifndef EXEC_BACKEND
	InitProcess();
#endif

	/* Early initialization */
	BaseInit();

	InitializeSessionUserIdStandalone();

	/* POLAR: Get MyBackendId */
	MyBackendId = InvalidBackendId;

	SharedInvalBackendInit(false);

	if (MyBackendId > MaxBackends || MyBackendId <= 0)
		elog(FATAL, "bad dispatcher backend ID: %d", MyBackendId);

	/* Now that we have a BackendId, we can participate in ProcSignal */
	ProcSignalInit(MyBackendId);

	polar_ss_shmem_aset_init_backend();

	polar_session_id_generator_initialize();

	pgstat_beinit();
	pgstat_bestart();

	polar_my_dispatcher = dispatcher_create(&polar_dispatcher_proc[polar_my_dispatcher_id]);
	polar_my_dispatcher_siseg_buffer = &dispatcher_siseg_buffers[polar_my_dispatcher_id];

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in postgres.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in bgwriter, but we do have LWLocks, buffers, and temp files.
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		/* Note: AbortBufferIO() is now called per-buffer via ResourceOwnerRelease */
		UnlockBuffers();
		/* buffer pins are released here: */
		ResourceOwnerRelease(CurrentResourceOwner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 false, true);
		/* we needn't bother with the other ResourceOwnerRelease phases */
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(polar_my_dispatcher->memory_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(polar_my_dispatcher->memory_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);

		/*
		 * Close all open files after any error.  This is helpful on Windows,
		 * where holding deleted files open causes various strange errors.
		 * It's not clear we need it elsewhere, but shouldn't hurt.
		 */
		smgrreleaseall();	/* PG15: renamed from smgrcloseall */

		/*
		 * We can now go away.	Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 */
		proc_exit(0);
	}
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	PG_SETMASK(&UnBlockSig);

	dispatcher_loop();

	proc_exit(0);
}

Size polar_ss_db_role_setting_shmem_size(void)
{
	Size size = 0;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return size;

	size = mul_size(polar_ss_db_role_setting_max_size,
					MAXALIGN(sizeof(PolarDbRoleSetting)));
	size = add_size(size, MAXALIGN(sizeof(PolarDbRoleSettingArray)));
	return size;
}

Size polar_ss_dispatcher_shmem_size(void)
{
	Size size = 0;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return size;

	Assert(MaxPolarSessionsPerDispatcher > 0);

	size += mul_size(polar_ss_dispatcher_count,
					MAXALIGN(sizeof(PolarDispatcherSISeg)));

	size += mul_size(polar_ss_dispatcher_count,
					MAXALIGN(sizeof(PolarDispatcherProc)));

	size += mul_size(polar_ss_dispatcher_count,
					MAXALIGN(polar_bms_alloc_size(MaxPolarSessionsPerDispatcher)));

	return size;
}

/*
 * We need some place in shared memory to provide information about proxies proc.
 */
Size polar_ss_session_context_shmem_size(void)
{
	Size size = 0;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return size;

	size = mul_size(MaxPolarSessions, MAXALIGN(sizeof(PolarSessionContext)));

	return size;
}


Size
polar_ss_shared_memory_shmem_size(void)
{
	Size size = 0;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return size;

	size = mul_size(polar_ss_shared_memory_size, 1024);
	return size;
}

/*
 * Attach the in-place dsa and init backend local dsa area that will be used by
 * ShmAllocSet. This function should be called by all backends that use
 * ShmAllocSet to manage memory.
 */
void
polar_ss_shmem_aset_init_backend(void)
{
	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	if (area == NULL)
	{
		MemoryContext old_mcxt;
		ShmAsetCtl *ctl = &shm_aset_ctl[SHM_ASET_TYPE_SHARED_SERVER];
		old_mcxt = MemoryContextSwitchTo(TopMemoryContext);
		area = dsa_attach_in_place(ctl->base, NULL);
		polar_shm_aset_set_area(SHM_ASET_TYPE_SHARED_SERVER, area);
		Assert(area != NULL);
		MemoryContextSwitchTo(old_mcxt);
	}
}

/*
 * Called when backend exit.
 *	1. Clean all prepared statements to minus the shared server plan ref count
 *  2. detach dsa area
 */
void
polar_shared_server_exit(void)
{
	if (area != NULL)
	{
		dsa_detach(area);
		area = NULL;
	}
}

/* Init shared memory and create a dsa that will be used by GPC. */
void
polar_ss_shared_memory_shmem_init(void)
{
	void *base;
	bool found;
	Size size;
	ShmAsetCtl *ctl;
	MemoryContext old_mcxt;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	size = polar_ss_shared_memory_shmem_size();

	old_mcxt = MemoryContextSwitchTo(TopMemoryContext);
	base = ShmemInitStruct("pss shmem", size, &found);

	elog(LOG, "pss shmem share memory total size is %lu", size);
	if (!found)
	{
		dsa_area *area;
		area = dsa_create_in_place(base, size, 0, NULL);
		dsa_set_size_limit(area, size);

		/* To prevent the area from being released */
		dsa_pin(area);
	}
	else
	{
		/* Attach to an existing area */
		dsa_attach_in_place(base, NULL);
	}

	/* Init shm_aset_ctl for global plan cache */
	ctl = &shm_aset_ctl[SHM_ASET_TYPE_SHARED_SERVER];
	ctl->base = base;
	ctl->size = size;

	MemoryContextSwitchTo(old_mcxt);
}

void
polar_ss_db_role_setting_shmem_init(void)
{
	bool  found;
	char* shmem;
	Size size;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	size = polar_ss_db_role_setting_shmem_size();
	shmem = ShmemInitStruct("pss global", size, &found);

	polar_ss_db_role_setting_array = (PolarDbRoleSettingArray *)shmem;

	elog(LOG, "pss global share memory total size is %lu", size);
	if (!found)
	{
		memset(polar_ss_db_role_setting_array, 0, size);

		LWLockRegisterTranche(LWTRANCHE_POLAR_SS_DB_ROLE_SETTING, "polar db role setting");

		LWLockInitialize(&polar_ss_db_role_setting_array->db_role_setting_lock.lock, LWTRANCHE_POLAR_SS_DB_ROLE_SETTING);
		polar_ss_db_role_setting_array->max_count = polar_ss_db_role_setting_max_size;
	}
}

void
polar_ss_dispatcher_shmem_init(void)
{
	bool  found;
	char* shmem;
	Size size;
	int i ;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	size = polar_ss_dispatcher_shmem_size();
	shmem = ShmemInitStruct("pss dispatcher", size, &found);

	dispatcher_siseg_buffers = (PolarDispatcherSISeg *)shmem;
	shmem += polar_ss_dispatcher_count * MAXALIGN(sizeof(PolarDispatcherSISeg));

	polar_dispatcher_proc = (PolarDispatcherProc *)shmem;
	shmem += (polar_ss_dispatcher_count * MAXALIGN(sizeof(PolarDispatcherProc)));

	elog(LOG, "pss dispatcher share memory total size is %lu", size);

	if (!found)
	{
		memset(dispatcher_siseg_buffers, 0, size);

		LWLockRegisterTranche(LWTRANCHE_POLAR_SS_DISPATCHER_INVAL_BUFFER, "polar dispatcher inval buffer");

		for (i = 0; i < polar_ss_dispatcher_count; ++i)
		{
			SpinLockInit(&polar_dispatcher_proc[i].signalling_session_lock);
			LWLockInitialize(&dispatcher_siseg_buffers[i].inval_read_lock.lock, LWTRANCHE_POLAR_SS_DISPATCHER_INVAL_BUFFER);

			polar_dispatcher_proc[i].signalling_sessions = (Bitmapset *)shmem;
			shmem += MAXALIGN(polar_bms_alloc_size(MaxPolarSessionsPerDispatcher));

			polar_bms_reset(polar_dispatcher_proc[i].signalling_sessions, MaxPolarSessionsPerDispatcher);
		}
	}
}

void
polar_ss_session_context_shmem_init(void)
{
	bool  found;
	void *shmem;
	Size size;

	if (!POLAR_SHARED_SERVER_RUNNING())
		return;

	size = polar_ss_session_context_shmem_size();
	shmem = ShmemInitStruct("pss session", size, &found);

	polar_session_contexts = (PolarSessionContext *)shmem;

	elog(LOG, "pss session_contexts share memory total size is %lu", size);

	if (!found)
	{
		int i ;
		memset(shmem, 0, size);

		LWLockRegisterTranche(LWTRANCHE_POLAR_SS_SESSION_CONTEXT, "polar session context");
		for (i = 0; i < MaxPolarSessions; ++i)
		{
			pg_atomic_init_u32(&polar_session_contexts[i].status, PSSE_RELEASED);
			pg_atomic_init_u32(&polar_session_contexts[i].is_shared_backend_exit, 0);
			pg_atomic_init_u64(&polar_session_contexts[i].finished_command_count, 0);
		}
	}
}


static void
add_a_invalidation_message(SharedInvalidationMessage *msg)
{
	int numMsgs = polar_my_dispatcher_siseg_buffer->maxMsgNum - polar_my_dispatcher_siseg_buffer->minMsgNum;
	if (numMsgs >= MAXNUMMESSAGES)  /* must clean up queue */
	{
		LWLockAcquire(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock, LW_EXCLUSIVE);
		polar_my_dispatcher_siseg_buffer->minMsgNum = polar_my_dispatcher_siseg_buffer->maxMsgNum - MAXNUMMESSAGES/2; /* clean up queue */
		LWLockRelease(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock);
	}
	else if (numMsgs >= (MAXNUMMESSAGES * 3 / 4))  /* try clean up queue */
	{
		if (LWLockConditionalAcquire(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock, LW_EXCLUSIVE))
		{
			polar_my_dispatcher_siseg_buffer->minMsgNum = polar_my_dispatcher_siseg_buffer->maxMsgNum - MAXNUMMESSAGES/2; /* clean up queue */
			LWLockRelease(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock);
		}
	}

	polar_my_dispatcher_siseg_buffer->buffer[polar_my_dispatcher_siseg_buffer->maxMsgNum % MAXNUMMESSAGES] = *msg;

	LWLockAcquire(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock, LW_EXCLUSIVE);
	polar_my_dispatcher_siseg_buffer->maxMsgNum++;
	LWLockRelease(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock);
}

static void
inval_all(void)
{
	LWLockAcquire(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock, LW_EXCLUSIVE);
	polar_my_dispatcher_siseg_buffer->minMsgNum += MAXNUMMESSAGES;
	polar_my_dispatcher_siseg_buffer->maxMsgNum = polar_my_dispatcher_siseg_buffer->minMsgNum;
	LWLockRelease(&polar_my_dispatcher_siseg_buffer->inval_read_lock.lock);
}

static void
local_execute_invalidation_message(SharedInvalidationMessage *msg)
{
	/* be careful, the processing method must correspond to InitPlanCache()*/
	if (msg->id >= 0)
	{
		switch(msg->cc.id)
		{
			case PROCOID:
			case NAMESPACEOID:
			case OPEROID:
			case AMOPOPID:
			case FOREIGNSERVEROID:
			case FOREIGNDATAWRAPPEROID:
				add_a_invalidation_message(msg);
				break;
		};
	}
	else if (msg->id == SHAREDINVALCATALOG_ID)
	{
		inval_all();
	}
	else if (msg->id == SHAREDINVALRELCACHE_ID)
	{
		add_a_invalidation_message(msg);
	}
	/* other type has nothing to do with PlanCache */
}

/*
 * polar_session_SI_get_data_entries
 *		get next SI message(s) for current backend, if there are any
 *
 * Possible return values:
 *	0:	 no SI message available
 *	n>0: next n SI messages have been extracted into data[]
 * -1:	 SI reset message extracted
 *
 * If the return value is less than the array size "datasize", the caller
 * can assume that there are no more SI messages after the one(s) returned.
 * Otherwise, another call is needed to collect more messages.
 *
 * Note: we assume that "datasize" is not so large that it might be important
 * to break our hold on SInvalReadLock into segments.
 */
int
polar_session_SI_get_data_entries(PolarSessionContext* session, SharedInvalidationMessage *data, int datasize)
{
	PolarDispatcherSISeg* si_seg = session->info->shm_inval_buffer;
	int count;
	/*First check, without acquiring the lock */ 
	if (session->info->nextMsgNum < si_seg->minMsgNum)
	{
		session->info->nextMsgNum = si_seg->maxMsgNum;
		return -1;
	}

	if (session->info->nextMsgNum == si_seg->maxMsgNum)
		return 0;

	LWLockAcquire(&si_seg->inval_read_lock.lock, LW_SHARED);
	if (session->info->nextMsgNum < si_seg->minMsgNum)  /* catch up, reset all */
	{
		session->info->nextMsgNum = si_seg->maxMsgNum;
		count = -1;
	}
	else if (session->info->nextMsgNum == si_seg->maxMsgNum)
	{
		count = 0;
	}
	else 
	{
		if (si_seg->maxMsgNum - si_seg->minMsgNum >= MAXNUMMESSAGES_PROCESS_THRESHOLD)
		{
			session->info->nextMsgNum = si_seg->maxMsgNum;
			count = -1;
		}
		else
		{
			count = 0;
			while(count < datasize && session->info->nextMsgNum < si_seg->maxMsgNum)
			{
				data[count] = si_seg->buffer[session->info->nextMsgNum % MAXNUMMESSAGES];
				session->info->nextMsgNum++;
				count++;
			}
		}
	}
	LWLockRelease(&si_seg->inval_read_lock.lock);
	return count;
}

static void
mark_all_session_reload_config(void)
{
	HASH_SEQ_STATUS   seq;
	SessionPool*      pool;
	ClientChannel* client;
	ClientChannel* tmp_client;

	hash_seq_init(&seq, polar_my_dispatcher->pools);
	while ((pool = hash_seq_search(&seq)) != NULL)
	{
		list_for_each_entry_safe(client, tmp_client, &pool->live_clients_list, list_node)
		{
			client->session->info->m_ConfigReloadPending = true;
		}
	}
}

void
polar_update_db_role_setting_version(Oid databaseid, Oid roleid, bool is_drop)
{
	uint32 i = 0;
	char *database_name = NULL;
	char *role_name = NULL;
	uint32 database_name_hash = 0;
	uint32 role_name_hash = 0;
	uint32 used_count = 0;
	bool found = false;

	if (polar_ss_db_role_setting_array == NULL)
		return;

	if (OidIsValid(databaseid))
	{
		database_name = get_database_name(databaseid);
		database_name_hash = polar_murmurhash2(database_name, strlen(database_name), database_name_hash);
	}

	if (OidIsValid(roleid))
	{
		role_name = GetUserNameFromId(roleid, false);
		role_name_hash = polar_murmurhash2(role_name, strlen(role_name), role_name_hash);
	}

	ELOG_PSS(DEBUG1, "polar_update_db_role_setting_version is_drop:%d, %d-%s-%u, %d-%s-%u", 
		is_drop,
		databaseid, database_name ? "null" : database_name, database_name_hash,
		roleid, role_name ? "null" : role_name, role_name_hash);

	LWLockAcquire(&polar_ss_db_role_setting_array->db_role_setting_lock.lock, LW_EXCLUSIVE);//LW_SHARED
	used_count = polar_ss_db_role_setting_array->used_count;

	for (i = 0; i < used_count; i++)
	{
		PolarDbRoleSetting *setting = polar_ss_db_role_setting_array->setting + i;
		if (setting->version == 0)
			continue;

		if ((OidIsValid(databaseid) && OidIsValid(roleid) &&
			 database_name_hash == setting->database_name_hash &&
			 role_name_hash == setting->role_name_hash) ||
			(OidIsValid(databaseid) &&
			 database_name_hash == setting->database_name_hash) ||
			(OidIsValid(roleid) &&
			 role_name_hash == setting->role_name_hash)
			)
		{
			found = true;
			if (is_drop)
			{
				setting->database_name_hash = 0;
				setting->role_name_hash = 0;
				setting->version = 0;
			}
			else
			{
				setting->version++;
			}

			if(OidIsValid(databaseid) && OidIsValid(roleid))
				break;
		}
	}

	if (!is_drop && !found)
	{
		PolarDbRoleSetting *setting = NULL;
		for (i = 0; i < used_count; i++)
			if (polar_ss_db_role_setting_array->setting[i].version == 0)
				break;

		if (i >= used_count)
			polar_ss_db_role_setting_array->used_count++;

		if (polar_ss_db_role_setting_array->used_count > polar_ss_db_role_setting_max_size)
			elog(ERROR, "polar_ss_db_role_setting_array extend max size %d, need update guc 'polar_ss_db_role_setting_max_size'", 
				polar_ss_db_role_setting_max_size);

		setting = polar_ss_db_role_setting_array->setting + i;

		setting->database_name_hash = database_name_hash;
		setting->role_name_hash = role_name_hash;
		setting->version++;
	}
	LWLockRelease(&polar_ss_db_role_setting_array->db_role_setting_lock.lock);
}


uint32
get_startup_gucs_hash_from_db_role_setting(char *database_name, char *role_name, uint32 seed)
{
	uint32 database_name_hash = 0;
	uint32 role_name_hash = 0;
	uint32 ret = seed;
	uint32 i = 0;
	Assert(database_name != NULL);
	Assert(role_name != NULL);

	database_name_hash = polar_murmurhash2(database_name, strlen(database_name), database_name_hash);
	role_name_hash = polar_murmurhash2(role_name, strlen(role_name), role_name_hash);

	LWLockAcquire(&polar_ss_db_role_setting_array->db_role_setting_lock.lock, LW_SHARED);
	/* 
	 *	ref to process_settings()
	 *	ApplySetting(snapshot, databaseid, roleid, relsetting, PGC_S_DATABASE_USER);
	 *	ApplySetting(snapshot, InvalidOid, roleid, relsetting, PGC_S_USER);
	 *	ApplySetting(snapshot, databaseid, InvalidOid, relsetting, PGC_S_DATABASE);
	 * 	if (!MyProc->issuper || polar_apply_global_guc_for_super)
	 *		ApplySetting(snapshot, InvalidOid, InvalidOid, relsetting, PGC_S_GLOBAL);
	 */

	for (i = 0; i < polar_ss_db_role_setting_array->used_count; i++)
	{
		PolarDbRoleSetting *setting = polar_ss_db_role_setting_array->setting + i;
		if (setting->version > 0 &&
			database_name_hash == setting->database_name_hash &&
			role_name_hash == setting->role_name_hash)
		{
			ret = polar_murmurhash2(&setting->version, sizeof(uint32), ret);
			break;
		}
	}
	for (i = 0; i < polar_ss_db_role_setting_array->used_count; i++)
	{
		PolarDbRoleSetting *setting = polar_ss_db_role_setting_array->setting + i;
		if (setting->version > 0 &&
			0 == setting->database_name_hash &&
			role_name_hash == setting->role_name_hash)
			ret = polar_murmurhash2(&setting->version, sizeof(uint32), ret);
	}
	for (i = 0; i < polar_ss_db_role_setting_array->used_count; i++)
	{
		PolarDbRoleSetting *setting = polar_ss_db_role_setting_array->setting + i;
		if (setting->version > 0 &&
			database_name_hash == setting->database_name_hash &&
			0 == setting->role_name_hash)
			ret = polar_murmurhash2(&setting->version, sizeof(uint32), ret);
	}
	for (i = 0; i < polar_ss_db_role_setting_array->used_count; i++)
	{
		PolarDbRoleSetting *setting = polar_ss_db_role_setting_array->setting + i;
		if (setting->version > 0 &&
			0 == setting->database_name_hash &&
			0 == setting->role_name_hash)
			ret = polar_murmurhash2(&setting->version, sizeof(uint32), ret);
	}
	LWLockRelease(&polar_ss_db_role_setting_array->db_role_setting_lock.lock);

	ELOG_PSS(DEBUG1, "get_startup_gucs_hash_from_db_role_setting %s/%s/%u", 
		database_name, role_name, ret);

	return ret;
}

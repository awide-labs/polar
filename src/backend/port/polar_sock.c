/*-------------------------------------------------------------------------
 *
 * polar_sock.c
 *	  Send socket descriptor to another process
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/port/polar_sock.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Send socket descriptor "sendfd" to backend process through Unix socket "fd"
 */
int
polar_pg_send_sock(pgsocket fd, pgsocket sendfd)
{
	static char m_buffer[1] = {};
	struct msghdr msg;
	struct iovec iov[1];
	union
	{
		struct cmsghdr cm;
		char		control[CMSG_SPACE(sizeof(pgsocket))];
	}			control_un;
	struct cmsghdr *cmptr;
	int			rc;

	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);
	cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len = CMSG_LEN(sizeof(pgsocket));
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	*((int *) CMSG_DATA(cmptr)) = sendfd;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;

	iov[0].iov_base = m_buffer;
	iov[0].iov_len = 1;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	if (unlikely((rc = sendmsg(fd, &msg, 0)) < 0))
	{
		elog(WARNING, "polar_pg_send_sock: sendmsg: chan:%d, sock:%d: %m", fd, sendfd);
		return rc;
	}
	return rc;
}


/*
 * Receive socket descriptor from postmaster process through Unix socket "fd"
 */
pgsocket
polar_pg_recv_sock(pgsocket fd)
{
	static char m_buffer[1] = {};
	struct msghdr msg;
	struct cmsghdr *cmptr;
	struct iovec iov[1];
	int			n;
	union
	{
		struct cmsghdr cm;
		char		control[CMSG_SPACE(sizeof(pgsocket))];
	}			control_un;
	pgsocket	recvfd = PGINVALID_SOCKET;

	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	iov[0].iov_base = m_buffer;
	iov[0].iov_len = 1;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	if (unlikely((n = recvmsg(fd, &msg, 0)) <= 0))
		elog(WARNING, "recv fd, failed chan:%d, %d: %m", fd, n);
	else if (unlikely((cmptr = CMSG_FIRSTHDR(&msg)) == NULL))
		elog(WARNING, "recv fd, null cmptr, fd not passed, chan:%d, %m", fd);
	else if (unlikely(cmptr->cmsg_len != CMSG_LEN(sizeof(pgsocket))))
		elog(WARNING, "recv fd, message len if incorrect, chan:%d, %ld %m", fd, cmptr->cmsg_len);
	else if (unlikely(cmptr->cmsg_level != SOL_SOCKET))
		elog(WARNING, "recv fd, control level != SOL_SOCKET, chan:%d, %d: %m", fd, cmptr->cmsg_level);
	else if (unlikely(cmptr->cmsg_type != SCM_RIGHTS))
		elog(WARNING, "recv fd, control type != SCM_RIGHTS, chan:%d, %d: %m", fd, cmptr->cmsg_type);
	else
		recvfd = *((int *) CMSG_DATA(cmptr));

	return recvfd;
}

bool
polar_pg_is_block(pgsocket sock)
{
	int			flags;

	flags = fcntl(sock, F_GETFL);
	if (flags < 0)
		return false;

	return (flags & O_NONBLOCK) == 0;
}

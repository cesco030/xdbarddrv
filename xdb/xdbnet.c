/*
 * xdbnet.c - helpers shared by the XDB backends that implement a wire
 * protocol themselves (MySQL, SQL Server): connection strings, password
 * file, TCP connect.
 */
#ifdef __ILEC400__
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "xdb_be.h"
#ifndef __ILEC400__
#include <sys/select.h>			/* ILE declares select() in sys/time.h */
#endif

extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);

void
xnet_err(xbe_err *e, const char *sqlstate, const char *fmt,...)
{
	va_list		ap;

	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", sqlstate);
	va_start(ap, fmt);
	vsnprintf(e->msg, sizeof e->msg, fmt, ap);
	va_end(ap);
}

const char *
xnet_kv(const char *info, const char *key, char *out, int outsize)
{
	size_t		kl = strlen(key);
	const char *p = info;

	out[0] = '\0';
	while (p && *p)
	{
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, key, kl) == 0 && p[kl] == '=')
		{
			const char *v = p + kl + 1;
			int			n = 0;

			if (*v == '\'')		/* quoted value, '' is a quote */
			{
				for (v++; *v && n < outsize - 1; v++)
				{
					if (*v == '\'')
					{
						if (v[1] != '\'')
							break;
						v++;
					}
					out[n++] = *v;
				}
			}
			else
				while (v[n] && v[n] != ' ' && v[n] != '\t' && n < outsize - 1)
				{
					out[n] = v[n];
					n++;
				}
			out[n] = '\0';
			return out;
		}
		/* skip this word (a quoted value may contain blanks) */
		while (*p && *p != ' ' && *p != '\t')
		{
			if (*p == '\'')
			{
				for (p++; *p; p++)
					if (*p == '\'')
					{
						if (p[1] != '\'')
							break;
						p++;
					}
				if (*p == '\0')
					break;
			}
			p++;
		}
	}
	return NULL;
}

/* next field of a passfile line; returns 1 if it matches want (or is *) */
static int
pass_field(const char **pp, const char *want, char *val, int valsize)
{
	const char *p = *pp;
	int			n = 0;

	while (*p && *p != ':' && *p != '\n' && *p != '\r')
	{
		if (*p == '\\' && p[1])
			p++;
		if (n < valsize - 1)
			val[n++] = *p;
		p++;
	}
	val[n] = '\0';
	if (*p == ':')
		p++;
	*pp = p;
	return want == NULL || strcmp(val, "*") == 0 || strcmp(val, want) == 0;
}

int
xnet_passfile(const char *path, const char *host, const char *port,
			  const char *db, const char *user, char *out, int outsize)
{
	FILE	   *f = fopen(path, "rb");
	char		line[1024],
				v[512];
	int			found = 1;

	out[0] = '\0';
	if (f == NULL)
		return -1;
	while (found != 0 && fgets(line, sizeof line, f))
	{
		const char *p = line;

		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		if (pass_field(&p, host, v, sizeof v) & pass_field(&p, port, v, sizeof v) &
			pass_field(&p, db, v, sizeof v) & pass_field(&p, user, v, sizeof v))
		{
			pass_field(&p, NULL, v, sizeof v);
			snprintf(out, outsize, "%s", v);
			found = 0;
		}
	}
	memset(line, 0, sizeof line);
	memset(v, 0, sizeof v);
	fclose(f);
	return found;
}

int
xnet_connect(const char *host, const char *port, int timeout, xbe_err *e)
{
	struct addrinfo hint,
			   *res = NULL,
			   *a;
	int			fd = -1,
				rc,
				one = 1,
				lasterr = 0;

	memset(&hint, 0, sizeof hint);
	hint.ai_family = AF_UNSPEC;
	hint.ai_socktype = SOCK_STREAM;
#ifdef __ILEC400__
	{
		/* the resolver wants its strings in the job CCSID */
		char		h[300],
					p[40];
		int			hl = ile_conv(1208, 0, host, strlen(host), h, sizeof h - 1);
		int			pl = ile_conv(1208, 0, port, strlen(port), p, sizeof p - 1);

		if (hl < 0 || pl < 0)
			rc = -1;
		else
		{
			h[hl] = '\0';
			p[pl] = '\0';
			rc = getaddrinfo(h, p, &hint, &res);
		}
	}
#else
	rc = getaddrinfo(host, port, &hint, &res);
#endif
	if (rc != 0 || res == NULL)
	{
		xnet_err(e, "08001", "cannot resolve host \"%s\"", host);
		return -1;
	}
	for (a = res; a; a = a->ai_next)
	{
		int			fl;

		fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0)
		{
			lasterr = errno;
			continue;
		}
		fl = fcntl(fd, F_GETFL, 0);
		if (timeout > 0 && fl >= 0)
			fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		rc = connect(fd, a->ai_addr, a->ai_addrlen);
		if (rc != 0 && timeout > 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK))
		{
			fd_set		w;
			struct timeval tv;
			int			soerr = 0;
#ifdef __ILEC400__
			int			sl = sizeof soerr;
#else
			socklen_t	sl = sizeof soerr;
#endif

			FD_ZERO(&w);
			FD_SET(fd, &w);
			tv.tv_sec = timeout;
			tv.tv_usec = 0;
			rc = select(fd + 1, NULL, &w, NULL, &tv);
			if (rc == 0)
			{
				errno = ETIMEDOUT;
				rc = -1;
			}
			else if (rc > 0)
			{
				getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &soerr, &sl);
				errno = soerr;
				rc = soerr ? -1 : 0;
			}
		}
		if (rc == 0)
		{
			if (timeout > 0 && fl >= 0)
				fcntl(fd, F_SETFL, fl);
			break;
		}
		lasterr = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
	{
#ifdef __ILEC400__
		/* strerror() answers in the job CCSID here: give the number */
		if (lasterr == ETIMEDOUT)
			xnet_err(e, "08001", "cannot connect to %s port %s: timeout", host, port);
		else if (lasterr == ECONNREFUSED)
			xnet_err(e, "08001", "cannot connect to %s port %s: connection refused", host, port);
		else
			xnet_err(e, "08001", "cannot connect to %s port %s: errno %d", host, port, lasterr);
#else
		xnet_err(e, "08001", "cannot connect to %s port %s: %s", host, port,
				 lasterr == ETIMEDOUT ? "timeout" : strerror(lasterr));
#endif
		return -1;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof one);
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof one);
	return fd;
}

int
xnet_wait_readable(int fd, int timeout)
{
	fd_set		rd;
	struct timeval tv;
	int			r;

	if (fd < 0)
		return -1;
	FD_ZERO(&rd);
	FD_SET(fd, &rd);
	tv.tv_sec = timeout > 0 ? timeout : 0;
	tv.tv_usec = 0;
	r = select(fd + 1, &rd, NULL, NULL, &tv);
	return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

int
xnet_dead(int fd, int unsolicited)
{
	char		b[1];
	int			r;

	r = xnet_wait_readable(fd, 0);
	if (r < 0)
		return 1;
	if (r == 0)
		return 0;
	r = (int) recv(fd, b, 1, MSG_PEEK);
	if (r <= 0)
		return 1;				/* end of stream or reset */
	return unsolicited ? 1 : 0;
}

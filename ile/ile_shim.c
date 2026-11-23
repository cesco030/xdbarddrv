/*
 * ile_shim.c - job-CCSID wrappers for OS calls used by the UTF-8 compiled
 * libpq, plus the CCSID conversion helper shared with the ARD program.
 *
 * On IBM i conversions use QtqIconvOpen (numeric CCSIDs).  With
 * XDB_LINUX_EMU the OS wrappers pass through and ile_conv() maps a few
 * CCSIDs to iconv names, so the same code paths can be tested on Linux.
 */
#define XDB_ILE_SHIM_IMPL
#ifdef __ILEC400__
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif
#include "ile_shim.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <iconv.h>
#ifdef __ILEC400__
#include <qtqiconv.h>
#include <qp0z1170.h>
#endif

#define XDB_PATHMAX 1024
#define XDB_NCONV 24

typedef struct
{
	int			used;
	int			from;
	int			to;
	iconv_t		cd;
} conv_slot;

static conv_slot conv_tab[XDB_NCONV];

#ifndef __ILEC400__
static int	emu_job_ccsid = 0;

static const char *
emu_name(int ccsid)
{
	static char buf[32];

	switch (ccsid)
	{
		case 1208:
			return "UTF-8";
		case 819:
			return "ISO-8859-1";
		case 1200:
		case 13488:
		case 61952:
			return "UTF-16BE";
		case 37:
			return "IBM037";
		case 874:
		case 1250:
		case 1251:
		case 1252:
		case 1253:
		case 1254:
		case 1255:
		case 1256:
		case 1257:
		case 1258:
			snprintf(buf, sizeof buf, "CP%d", ccsid);
			return buf;
		case 943:
			return "CP932";
		case 1386:
			return "GBK";
		case 1363:
			return "CP949";
		case 950:
			return "BIG5";
		case 65535:
			return NULL;
		default:
			snprintf(buf, sizeof buf, "IBM%d", ccsid);
			return buf;
	}
}
#endif

int
ile_job_ccsid(void)
{
#ifdef __ILEC400__
	return 0;					/* 0 = job CCSID for QtqIconvOpen */
#else
	if (emu_job_ccsid == 0)
	{
		const char *e = getenv("XDB_EMU_JOBCCSID");

		emu_job_ccsid = e ? atoi(e) : 1208;
	}
	return emu_job_ccsid;
#endif
}

static conv_slot *
conv_get(int from, int to)
{
	int			i;
	conv_slot  *s = NULL;

	for (i = 0; i < XDB_NCONV; i++)
	{
		if (conv_tab[i].used && conv_tab[i].from == from && conv_tab[i].to == to)
			return &conv_tab[i];
		if (!conv_tab[i].used && s == NULL)
			s = &conv_tab[i];
	}
	if (s == NULL)
		return NULL;
#ifdef __ILEC400__
	{
		QtqCode_T	f,
					t;

		memset(&f, 0, sizeof f);
		memset(&t, 0, sizeof t);
		f.CCSID = from;
		t.CCSID = to;
		s->cd = QtqIconvOpen(&t, &f);
		if (s->cd.return_value == -1)
			return NULL;
	}
#else
	{
		const char *fn = emu_name(from == 0 ? ile_job_ccsid() : from);
		char		fbuf[32];
		const char *tn;

		if (fn == NULL)
			return NULL;
		snprintf(fbuf, sizeof fbuf, "%s", fn);
		tn = emu_name(to == 0 ? ile_job_ccsid() : to);
		if (tn == NULL)
			return NULL;
		s->cd = iconv_open(tn, fbuf);
		if (s->cd == (iconv_t) -1)
			return NULL;
	}
#endif
	s->used = 1;
	s->from = from;
	s->to = to;
	return s;
}

/*
 * Convert inlen bytes; returns the number of bytes written (a terminating
 * zero is added when there is room and not counted), or -1.
 */
int
ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
		 char *out, size_t outsize)
{
	conv_slot  *s;
	char	   *ip = (char *) in;
	char	   *op = out;
	size_t		il = inlen;
	size_t		ol = outsize;

	if (from_ccsid == to_ccsid || from_ccsid == 65535 || to_ccsid == 65535)
	{
		if (inlen > outsize)
			return -1;
		memcpy(out, in, inlen);
		if (inlen < outsize)
			out[inlen] = '\0';
		return (int) inlen;
	}
	s = conv_get(from_ccsid, to_ccsid);
	if (s == NULL)
		return -1;
#ifdef __ILEC400__
	if (il > 0 && iconv(s->cd, &ip, &il, &op, &ol) == (size_t) -1)
	{
		iconv(s->cd, NULL, NULL, NULL, NULL);
		return -1;
	}
#else
	/*
	 * The system puts the substitution character of the target where the
	 * target has no such character and goes on; the iconv of Linux stops
	 * there.  The emulation does what the system does: a character of the
	 * source that is one, and that the target lacks, becomes a substitution
	 * character (X'3F' in EBCDIC, X'1A' elsewhere).
	 */
	while (il > 0 && iconv(s->cd, &ip, &il, &op, &ol) == (size_t) -1)
	{
		int			f = from_ccsid == 0 ? ile_job_ccsid() : from_ccsid;
		int			t = to_ccsid == 0 ? ile_job_ccsid() : to_ccsid;
		const unsigned char *u = (const unsigned char *) ip;
		size_t		skip = 1,
					k;
		int			wide_to = t == 1200 || t == 13488 || t == 61952;
		int			ascii_to = t == 1208 || t == 819 || t == 874 || (t >= 1250 && t <= 1258) ||
			t == 943 || t == 1386 || t == 1363 || t == 950;

		if (errno != EILSEQ)
		{
			iconv(s->cd, NULL, NULL, NULL, NULL);
			return -1;
		}
		if (f == 1208)
		{
			skip = u[0] >= 0xF0 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC0 ? 2 : 1;
			if (u[0] < 0xC2 || u[0] > 0xF4 || skip > il)
				skip = 0;		/* not a character: that is an error on the system too */
			for (k = 1; k < skip; k++)
				if ((u[k] & 0xC0) != 0x80)
					skip = 0;
		}
		else if (f == 1200 || f == 13488 || f == 61952)
		{
			skip = 2;
			if (il < 2 || (u[0] >= 0xDC && u[0] <= 0xDF))
				skip = 0;
			else if (u[0] >= 0xD8 && u[0] <= 0xDB)
				skip = (il >= 4 && u[2] >= 0xDC && u[2] <= 0xDF) ? 4 : 0;
		}
		if (skip == 0 || ol < (size_t) (wide_to ? 2 : 1))
		{
			iconv(s->cd, NULL, NULL, NULL, NULL);
			return -1;
		}
		if (wide_to)
		{
			*op++ = 0x00;
			ol--;
		}
		*op++ = (wide_to || ascii_to) ? 0x1A : 0x3F;
		ol--;
		ip += skip;
		il -= skip;
	}
#endif
	if (ol > 0)
		*op = '\0';
	return (int) (outsize - ol);
}

#ifdef __ILEC400__

/* UTF-8 -> job CCSID, zero terminated; NULL stays NULL */
static char *
u2j(const char *s, char *buf, size_t n)
{
	if (s == NULL)
		return NULL;
	if (ile_conv(1208, 0, s, strlen(s), buf, n - 1) < 0)
		return NULL;
	return buf;
}

/* job CCSID -> UTF-8 in place (same or longer length is truncated to n) */
static void
j2u_inplace(char *s, size_t n)
{
	char		tmp[XDB_PATHMAX];
	size_t		l;

	if (s == NULL || n == 0)
		return;
	l = strlen(s);
	if (l >= sizeof tmp)
		l = sizeof tmp - 1;
	if (ile_conv(0, 1208, s, l, tmp, sizeof tmp - 1) < 0)
		return;
	strncpy(s, tmp, n - 1);
	s[n - 1] = '\0';
}

int
ile_getaddrinfo(const char *node, const char *service,
				const struct addrinfo *hints, struct addrinfo **res)
{
	char		nb[XDB_PATHMAX];
	char		sb[64];

	return getaddrinfo(u2j(node, nb, sizeof nb), u2j(service, sb, sizeof sb),
					   hints, res);
}

int
ile_getnameinfo(const struct sockaddr *sa, int salen,
				char *host, int hostlen, char *serv, int servlen, int flags)
{
	int			rc = getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);

	if (rc == 0)
	{
		if (host && hostlen > 0)
			j2u_inplace(host, hostlen);
		if (serv && servlen > 0)
			j2u_inplace(serv, servlen);
	}
	return rc;
}

const char *
ile_gai_strerror(int ecode)
{
	static char buf[256];
	const char *m = gai_strerror(ecode);

	if (m == NULL)
		return "getaddrinfo error";
	strncpy(buf, m, sizeof buf - 1);
	buf[sizeof buf - 1] = '\0';
	j2u_inplace(buf, sizeof buf);
	return buf;
}

int
ile_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
			   struct passwd **result)
{
	int			rc = getpwuid_r(uid, pwd, buf, buflen, result);

	if (rc == 0 && result && *result)
	{
		if ((*result)->pw_name)
			j2u_inplace((*result)->pw_name, strlen((*result)->pw_name) + 1);
		if ((*result)->pw_dir)
			j2u_inplace((*result)->pw_dir, strlen((*result)->pw_dir) + 1);
	}
	return rc;
}

int
ile_stat(const char *path, struct stat *st)
{
	char		pb[XDB_PATHMAX];
	char	   *p = u2j(path, pb, sizeof pb);

	if (p == NULL)
	{
		errno = ENOENT;
		return -1;
	}
	return stat(p, st);
}

int
ile_open(const char *path, int oflag,...)
{
	char		pb[XDB_PATHMAX];
	char	   *p = u2j(path, pb, sizeof pb);
	int			mode = 0;

	if (p == NULL)
	{
		errno = ENOENT;
		return -1;
	}
	if (oflag & O_CREAT)
	{
		va_list		ap;

		va_start(ap, oflag);
		mode = va_arg(ap, int);
		va_end(ap);
		return open(p, oflag, mode);
	}
	return open(p, oflag);
}

/* environment: names and values live in the job CCSID */
#define XDB_NENV 48
static struct
{
	char	   *name;
	char	   *value;
}			env_tab[XDB_NENV];

char *
ile_getenv(const char *name)
{
	char		nb[256];
	char		vb[XDB_PATHMAX];
	char	   *v;
	int			ccsid = 0;
	int			i;
	int			n;

	if (name == NULL || u2j(name, nb, sizeof nb) == NULL)
		return NULL;
	v = Qp0zGetEnv(nb, &ccsid);
	if (v == NULL)
		return NULL;
	if (ccsid == 65535)
		ccsid = 0;
	n = ile_conv(ccsid, 1208, v, strlen(v), vb, sizeof vb - 1);
	if (n < 0)
		return NULL;
	for (i = 0; i < XDB_NENV; i++)
	{
		if (env_tab[i].name == NULL)
			break;
		if (strcmp(env_tab[i].name, name) == 0)
		{
			if (strcmp(env_tab[i].value, vb) != 0)
			{
				free(env_tab[i].value);
				env_tab[i].value = strdup(vb);
			}
			return env_tab[i].value;
		}
	}
	if (i == XDB_NENV)
		return NULL;
	env_tab[i].name = strdup(name);
	env_tab[i].value = strdup(vb);
	return env_tab[i].value;
}

#else							/* Linux emulation: OS strings are already UTF-8 */

int
ile_getaddrinfo(const char *node, const char *service,
				const struct addrinfo *hints, struct addrinfo **res)
{
	return getaddrinfo(node, service, hints, res);
}

int
ile_getnameinfo(const struct sockaddr *sa, int salen,
				char *host, int hostlen, char *serv, int servlen, int flags)
{
	return getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
}

const char *
ile_gai_strerror(int ecode)
{
	return gai_strerror(ecode);
}

int
ile_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
			   struct passwd **result)
{
	return getpwuid_r(uid, pwd, buf, buflen, result);
}

int
ile_stat(const char *path, struct stat *st)
{
	return stat(path, st);
}

int
ile_open(const char *path, int oflag,...)
{
	int			mode = 0;

	if (oflag & O_CREAT)
	{
		va_list		ap;

		va_start(ap, oflag);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return open(path, oflag, mode);
}

char *
ile_getenv(const char *name)
{
	return getenv(name);
}

#endif

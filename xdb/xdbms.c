/*
 * xdbms.c - Microsoft SQL Server backend of the XDB driver.
 *
 * A native client for the TDS protocol as documented by Microsoft
 * ([MS-TDS]).  It asks for TDS 7.4 and works at whatever level the server
 * answers with: 7.1 (SQL Server 2000), 7.2 (2005), 7.3 (2008) or 7.4
 * (2012 and later).  The differences between the levels (request headers,
 * widths of some reply fields, the date/time/(max)/xml types, rows with a
 * NULL bitmap) are handled where they occur; tds=7.1 in the connection
 * string keeps to the oldest level.
 *
 *   - statements without parameters go as SQL batches, the others through
 *     sp_executesql with parameter types taken from the host variables;
 *   - result columns are described with sp_prepare (nothing is executed);
 *   - cursors are API server cursors (sp_cursoropen / sp_cursorfetch /
 *     sp_cursorclose), so several cursors and other statements can be
 *     active on one connection and rows are fetched in blocks; statements
 *     that cannot have a server cursor are read into client memory.
 *
 * Encryption (encrypt= in the connection string): prefer = the login is
 * encrypted when the server can and everything when it insists, on =
 * everything, strict = TDS 8.0 (TLS before anything else), off = never.
 * With TDS 7.x the TLS handshake travels inside pre-login packets; GSKit
 * cannot do that, so on IBM i the choices are off (default) and strict.
 *
 * Not implemented: Windows (NTLM/Kerberos) logins, named instances (give
 * the TCP port), redirection by an Azure gateway.
 *
 * All integers on the wire are handled byte by byte, so the code is the
 * same on big-endian IBM i.  Compiled as UTF-8 (LOCALETYPE(*LOCALEUTF)).
 */
#ifdef __ILEC400__
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "xdb_be.h"
extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);

#define TDS_VERSION 0x71000001u
#define PKT_BATCH 1
#define PKT_RPC 3
#define PKT_REPLY 4
#define PKT_LOGIN 16
#define PKT_PRELOGIN 18

#define MAXRV 8

#define LOB_PIECE 32768			/* a file travels in pieces of this size */
#define LOB_MAXSINK 32
#define LOB_MAXSRC 32
#define LOB_NAMEMAX 1024		/* XLOB_PATHMAX of xdblob.h */
#define LOB_NAMEUNITS 1100		/* characters of a file name the server is asked for */

typedef struct
{
	unsigned char type;
	int			maxlen;
	int			prec;
	int			scale;
	int			ccsid;			/* code page of char data */
	int			plp;			/* (max) type: value comes in chunks */
	char		name[64];
} mcol;

typedef struct
{
	int			fd;
	int			dead;			/* protocol or I/O failure: unusable */
	int			psize;			/* negotiated packet size */
	unsigned char *ob;			/* outgoing message */
	size_t		oblen,
				obcap;
	unsigned char *pk;			/* current reply packet (payload) */
	size_t		pklen,
				pkpos,
				pkcap;
	unsigned char *in;			/* bytes read ahead from the socket (sock_read) */
	size_t		inlen,
				inpos;
	int			last;			/* current packet ends the reply */
	unsigned char *sb;			/* scratch for one value */
	size_t		sbcap;
	int			version;		/* major * 100 + minor */
	/* from the answer to the login, for XDB CONNECTION INFO */
	char		prod[64];		/* how the server names itself */
	unsigned char ver4[4];		/* major, minor, build (2 bytes) */
	char		dbname[200];	/* the database the session is in */
	int			spid;			/* number of the session on the server */
	unsigned char coll[5];		/* server default collation */
	int			ccsid;			/* code page of the default collation */
	int			ccsid_force;	/* charset= in conninfo */
	int			strmode;		/* 0 auto, 1 varchar, 2 nvarchar */
	int			in_txn;
	int			tds_req;		/* protocol level asked for at login (0x71, 0x74) */
	int			tds;			/* level the server answered with: 0x70 ... 0x74 */
	unsigned char txd[8];		/* transaction descriptor (TDS 7.2+), zero = none */
	int			txd_seen;		/* the server does report transactions (not all do) */
	int			dt2;			/* datetime2=on: date/time/datetime2 parameters */
	int			enc;			/* encrypt=: 0 off, 1 prefer, 2 on, 3 strict */
	int			verify;			/* check the server certificate chain */
	char		host[256];
	xtls	   *tls;
	int			tls_on;			/* packets travel inside TLS */
	int			tls_login;		/* ... only up to the login packet */
	int			tls_failed;		/* the handshake did not work */
	int			hs;				/* handshake in progress: TLS data rides in TDS packets */
	unsigned char *hsout;
	size_t		hsoutlen,
				hsoutcap;
	unsigned char *hsin;
	size_t		hsinlen,
				hsinpos,
				hsincap;
	/* state of the reply being read */
	mcol	   *cols;
	int			ncols;
	int			got_meta;
	int			keep_meta;		/* next reply: rows use the columns already set */
	int			first_meta;		/* next reply: keep the columns of the first result set */
	mcol	   *cols1;
	int			ncols1;
	int			sets;			/* result sets with columns met in the reply being read */
	mcol	   *cols0;			/* rows are kept: a copy of the columns of the first of them */
	int			ncols0;
	long long	count;
	int			rv[MAXRV];
	int			nrv;
	xbe_err		err;
	int			have_err;
	/* file functions (xdblob.c) */
	unsigned char obid;			/* number of the next packet of a message sent in parts */
	int			obsent;			/* packets of the message being built have gone out */
	int			sink[LOB_MAXSINK];	/* file columns of the statement described next */
	int			nsink;
	struct
	{
		xlob_src	src;
		int			kind;
		const char *tok;		/* the parameter value that stands for the file */
		long long	len;		/* bytes it has as a parameter value */
	}			lsrc[LOB_MAXSRC];	/* files of the statement executed next */
	int			nlsrc;
	xlob_dst   *dst;			/* where the file columns of the row fetched next go */
	int			ndst;
	const int  *rsink;			/* file columns of the rows of the reply being read */
	int			nrsink;
	xbe_err		ferr;			/* the first failure with a file while a reply was read */
	int			fbad;
	int			bigtext;		/* open cursors with file columns: TEXTSIZE is unlimited */
	unsigned char *fbuf;		/* a piece of a file */
	unsigned char *fbuf2;		/* ... converted */
} msc;

/* what describe() remembers of a statement, for open() */
typedef struct
{
	int			ndesc;			/* columns the describe told, 0 = none or not known */
	int			nocursor;		/* the rows are XML or JSON text: the server opens no cursor on that */
	int			nsink;
	int			sink[LOB_MAXSINK];
} msstmt;

typedef struct
{
	int			handle;			/* server cursor, 0 = rows held in inl */
	mcol	   *cols;			/* the columns of the rows as they came */
	int			ncols;
	int			nvis;			/* ... of them the columns of the statement: a server
								 * cursor adds the status of the row as a last one */
	xbe_batch	inl;
	int			inl_used;
	int			nsink;			/* columns whose values go to files (PUT_xLOB_TO_FILE) */
	int			sink[LOB_MAXSINK];
} mscur;

/* ------------------------------------------------------------- utilities */

static unsigned
le16(const unsigned char *p)
{
	return (unsigned) p[0] | ((unsigned) p[1] << 8);
}

static unsigned
le32(const unsigned char *p)
{
	return (unsigned) p[0] | ((unsigned) p[1] << 8) | ((unsigned) p[2] << 16) | ((unsigned) p[3] << 24);
}

/* UTF-8 -> UCS-2/UTF-16 little-endian; out needs 2 bytes per input byte */
static size_t
u8_to_u16(const char *s, size_t n, unsigned char *out)
{
	const unsigned char *p = (const unsigned char *) s,
			   *e = p + n;
	unsigned char *o = out;

	while (p < e)
	{
		unsigned	c = *p++;

		if (c >= 0xF0 && e - p >= 3)
		{
			c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
			p += 3;
		}
		else if (c >= 0xE0 && e - p >= 2)
		{
			c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
			p += 2;
		}
		else if (c >= 0xC0 && e - p >= 1)
		{
			c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
			p += 1;
		}
		else if (c >= 0x80)
			c = '?';
		if (c >= 0x10000)
		{
			unsigned	hi,
						lo;

			c -= 0x10000;
			hi = 0xD800 | (c >> 10);
			lo = 0xDC00 | (c & 0x3FF);
			*o++ = (unsigned char) hi;
			*o++ = (unsigned char) (hi >> 8);
			*o++ = (unsigned char) lo;
			*o++ = (unsigned char) (lo >> 8);
		}
		else
		{
			*o++ = (unsigned char) c;
			*o++ = (unsigned char) (c >> 8);
		}
	}
	return (size_t) (o - out);
}

/* UTF-16 little-endian -> UTF-8; out needs 3 bytes per 2 input bytes + 1 */
static size_t
u16_to_u8(const unsigned char *p, size_t n, char *out)
{
	unsigned char *o = (unsigned char *) out;
	size_t		i;

	for (i = 0; i + 1 < n; i += 2)
	{
		unsigned	c = le16(p + i);

		if (c >= 0xD800 && c < 0xDC00 && i + 3 < n)
		{
			unsigned	lo = le16(p + i + 2);

			if (lo >= 0xDC00 && lo < 0xE000)
			{
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (c < 0x80)
			*o++ = (unsigned char) c;
		else if (c < 0x800)
		{
			*o++ = (unsigned char) (0xC0 | (c >> 6));
			*o++ = (unsigned char) (0x80 | (c & 0x3F));
		}
		else if (c < 0x10000)
		{
			*o++ = (unsigned char) (0xE0 | (c >> 12));
			*o++ = (unsigned char) (0x80 | ((c >> 6) & 0x3F));
			*o++ = (unsigned char) (0x80 | (c & 0x3F));
		}
		else
		{
			*o++ = (unsigned char) (0xF0 | (c >> 18));
			*o++ = (unsigned char) (0x80 | ((c >> 12) & 0x3F));
			*o++ = (unsigned char) (0x80 | ((c >> 6) & 0x3F));
			*o++ = (unsigned char) (0x80 | (c & 0x3F));
		}
	}
	*o = '\0';
	return (size_t) (o - (unsigned char *) out);
}

/* Windows code page (as IBM CCSID) of a collation: LCID + flags, sort id */
static int
coll_ccsid(const unsigned char *k)
{
	unsigned	v = le32(k);
	unsigned	lcid = v & 0xFFFFF;
	unsigned	sortid = k[4];

	if (v & 0x04000000u)		/* UTF-8 collation (SQL Server 2019) */
		return 1208;
	if (sortid != 0)			/* SQL collations */
	{
		if (sortid >= 30 && sortid <= 34)
			return 437;
		if (sortid >= 40 && sortid <= 61 && sortid != 50 && sortid != 51 && sortid != 52 &&
			sortid != 53 && sortid != 54)
			return 850;
		if (sortid >= 80 && sortid <= 98)
			return 1250;
		if (sortid >= 104 && sortid <= 108)
			return 1251;
		if (sortid >= 112 && sortid <= 124)
			return 1253;
		if (sortid >= 128 && sortid <= 130)
			return 1254;
		if (sortid >= 136 && sortid <= 138)
			return 1255;
		if (sortid >= 144 && sortid <= 146)
			return 1256;
		if (sortid >= 152 && sortid <= 160)
			return 1257;
		return 1252;
	}
	switch (lcid & 0x3FF)		/* primary language */
	{
		case 0x01:				/* Arabic */
		case 0x20:				/* Urdu */
		case 0x29:				/* Farsi */
			return 1256;
		case 0x02:				/* Bulgarian */
		case 0x19:				/* Russian */
		case 0x22:				/* Ukrainian */
		case 0x23:				/* Belarusian */
		case 0x2F:				/* Macedonian */
			return 1251;
		case 0x04:				/* Chinese */
			return (lcid == 0x0804 || lcid == 0x1004) ? 1386 : 950;
		case 0x05:				/* Czech */
		case 0x0E:				/* Hungarian */
		case 0x15:				/* Polish */
		case 0x18:				/* Romanian */
		case 0x1A:				/* Croatian, Serbian (Latin) */
		case 0x1B:				/* Slovak */
		case 0x1C:				/* Albanian */
		case 0x24:				/* Slovenian */
			return 1250;
		case 0x08:				/* Greek */
			return 1253;
		case 0x0D:				/* Hebrew */
			return 1255;
		case 0x11:				/* Japanese */
			return 943;
		case 0x12:				/* Korean */
			return 1363;
		case 0x1E:				/* Thai */
			return 874;
		case 0x1F:				/* Turkish */
		case 0x2C:				/* Azeri */
			return 1254;
		case 0x25:				/* Estonian */
		case 0x26:				/* Latvian */
		case 0x27:				/* Lithuanian */
			return 1257;
		case 0x2A:				/* Vietnamese */
			return 1258;
		default:
			return 1252;
	}
}

/* server error number -> SQLSTATE */
static const char *
err_state(int n, int cls)
{
	switch (n)
	{
		case 2601:
		case 2627:
			return "23505";
		case 547:
			return "23503";
		case 515:
			return "23502";
		case 208:
		case 3701:
			return "42S02";
		case 207:
			return "42S22";
		case 2714:
			return "42S01";
		case 102:
		case 105:
		case 156:
		case 170:
		case 319:
			return "42601";
		case 229:
		case 230:
		case 262:
		case 297:
			return "42501";
		case 1205:
			return "40001";
		case 1222:
			return "57033";
		case 8152:
		case 2628:
			return "22001";
		case 8134:
			return "22012";
		case 220:
		case 232:
		case 8115:
			return "22003";
		case 241:
		case 242:
			return "22007";
		case 245:
		case 8114:
			return "22018";
		case 18456:
		case 18452:
			return "28000";
		case 4060:
		case 911:
			return "08004";
		case 2812:
			return "42883";
		default:
			return cls >= 20 ? "08006" : "HY000";
	}
}

/* ------------------------------------------------------------ packet I/O */

static int
raw_read(msc *c, unsigned char *b, size_t n)
{
	while (n > 0)
	{
		int			r = (int) recv(c->fd, (char *) b, n, 0);

		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t) r;
	}
	return 0;
}

static int
raw_write(msc *c, const unsigned char *b, size_t n)
{
	while (n > 0)
	{
		int			r = (int) send(c->fd, (char *) b, n, 0);

		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t) r;
	}
	return 0;
}

/* sends the handshake data collected so far as pre-login packets */
static int
hs_flush(msc *c)
{
	size_t		off = 0;

	while (off < c->hsoutlen)
	{
		size_t		n = c->hsoutlen - off > 4088 ? 4088 : c->hsoutlen - off;
		unsigned char h[8];

		h[0] = 0x12;
		h[1] = (off + n >= c->hsoutlen) ? 1 : 0;
		h[2] = (unsigned char) ((n + 8) >> 8);
		h[3] = (unsigned char) (n + 8);
		h[4] = h[5] = h[7] = 0;
		h[6] = 0;
		if (raw_write(c, h, 8) < 0 || raw_write(c, c->hsout + off, n) < 0)
			return -1;
		off += n;
	}
	c->hsoutlen = 0;
	return 0;
}

/* transport under TLS: the socket, or TDS packets while the handshake runs */
static int
tls_wr(void *ctx, const void *buf, int len)
{
	msc		   *c = ctx;

	if (!c->hs)
		return (int) send(c->fd, (char *) buf, len, 0);
	if (c->hsoutlen + (size_t) len > c->hsoutcap)
	{
		unsigned char *nb = realloc(c->hsout, (c->hsoutlen + (size_t) len) * 2);

		if (nb == NULL)
			return -1;
		c->hsout = nb;
		c->hsoutcap = (c->hsoutlen + (size_t) len) * 2;
	}
	memcpy(c->hsout + c->hsoutlen, buf, (size_t) len);
	c->hsoutlen += (size_t) len;
	return len;
}

static int
tls_rd(void *ctx, void *buf, int len)
{
	msc		   *c = ctx;
	size_t		k;

	if (!c->hs)
		return (int) recv(c->fd, (char *) buf, len, 0);
	if (c->hsinpos >= c->hsinlen)
	{
		unsigned char h[8];
		size_t		l;

		if (hs_flush(c) < 0 || raw_read(c, h, 8) < 0)
			return -1;
		l = ((size_t) h[2] << 8) | h[3];
		if (l <= 8)
			return -1;
		l -= 8;
		if (l > c->hsincap)
		{
			unsigned char *nb = realloc(c->hsin, l);

			if (nb == NULL)
				return -1;
			c->hsin = nb;
			c->hsincap = l;
		}
		if (raw_read(c, c->hsin, l) < 0)
			return -1;
		c->hsinlen = l;
		c->hsinpos = 0;
	}
	k = c->hsinlen - c->hsinpos;
	if (k > (size_t) len)
		k = (size_t) len;
	memcpy(buf, c->hsin + c->hsinpos, k);
	c->hsinpos += k;
	return (int) k;
}

static int
tls_begin(msc *c, int wrapped, const char *alpn, xbe_err *e)
{
	xtls_io		io;
	char		err[256];

	io.ctx = c;
	io.rd = tls_rd;
	io.wr = tls_wr;
	c->hs = wrapped;
	c->hsoutlen = c->hsinlen = c->hsinpos = 0;
	err[0] = '\0';
	/* TDS 7.x allows TLS 1.2 at most; strict mode (no wrapping) any version */
	c->tls = xtls_start_io(c->fd, wrapped ? &io : NULL, c->host, c->verify, wrapped, alpn, err, sizeof err);
	if (c->tls == NULL || hs_flush(c) < 0)
	{
		c->hs = 0;
		c->tls_failed = 1;
		c->dead = 1;
		xnet_err(e, "08001", "%s", err[0] ? err : "TLS handshake with the SQL Server failed");
		return -1;
	}
	c->hs = 0;
	c->tls_on = 1;
	return 0;
}

/*
 * Reads through a buffer: a reply arrives in packets of a few thousand
 * bytes, each with a header of eight, and asking the system for the header
 * and then for the rest of every packet is two calls for each of them.  One
 * call brings what has arrived, up to IN_BUF bytes, and the packets are
 * taken from there.  Nothing stays in the buffer between two requests: a
 * reply is read to its end, and the server sends nothing unasked.
 */
#define IN_BUF 65536

static int
sock_read(msc *c, unsigned char *b, size_t n)
{
	while (n > 0)
	{
		size_t		k = c->inlen - c->inpos;

		if (k == 0)
		{
			int			r;

			if (c->in == NULL && (c->in = malloc(IN_BUF)) == NULL)
				return -1;
			r = c->tls_on ? xtls_read(c->tls, c->in, IN_BUF) : (int) recv(c->fd, (char *) c->in, IN_BUF, 0);
			if (r <= 0)
				return -1;
			c->inpos = 0;
			c->inlen = k = (size_t) r;
		}
		if (k > n)
			k = n;
		memcpy(b, c->in + c->inpos, k);
		c->inpos += k;
		b += k;
		n -= k;
	}
	return 0;
}

static int
sock_write(msc *c, const unsigned char *b, size_t n)
{
	while (n > 0)
	{
		int			r = c->tls_on ? xtls_write(c->tls, b, (int) n) : (int) send(c->fd, (char *) b, n, 0);

		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t) r;
	}
	return 0;
}

static void
dead(msc *c, xbe_err *e, const char *what)
{
	c->dead = 1;
	if (e)
		xnet_err(e, "08006", "SQL Server connection lost (%s)", what);
}

static int
ob_put(msc *c, const void *p, size_t n)
{
	if (c->oblen + n > c->obcap)
	{
		size_t		nc = (c->oblen + n) * 2 + 256;
		unsigned char *nb = realloc(c->ob, nc);

		if (nb == NULL)
			return -1;
		c->ob = nb;
		c->obcap = nc;
	}
	if (p)
		memcpy(c->ob + c->oblen, p, n);
	else
		memset(c->ob + c->oblen, 0, n);
	c->oblen += n;
	return 0;
}

static void
ob_u8(msc *c, unsigned v)
{
	unsigned char b = (unsigned char) v;

	ob_put(c, &b, 1);
}

static void
ob_u16(msc *c, unsigned v)
{
	unsigned char b[2];

	b[0] = (unsigned char) v;
	b[1] = (unsigned char) (v >> 8);
	ob_put(c, b, 2);
}

static void
ob_u32(msc *c, unsigned v)
{
	unsigned char b[4];

	b[0] = (unsigned char) v;
	b[1] = (unsigned char) (v >> 8);
	b[2] = (unsigned char) (v >> 16);
	b[3] = (unsigned char) (v >> 24);
	ob_put(c, b, 4);
}

/* UTF-8 text appended as UCS-2; returns the number of bytes written */
static size_t
ob_ucs2(msc *c, const char *s)
{
	size_t		l = strlen(s);
	size_t		at = c->oblen,
				n;

	if (ob_put(c, NULL, l * 2 + 2) < 0)
		return 0;
	n = u8_to_u16(s, l, c->ob + at);
	c->oblen = at + n;
	return n;
}

/* sends the message in c->ob as packets of the negotiated size */
static int
msg_send(msc *c, int type, xbe_err *e)
{
	size_t		off = 0;
	size_t		chunk = (size_t) c->psize - 8;
	unsigned char id = c->obsent ? c->obid : 1;
	unsigned char *pkt;

	c->obsent = 0;
	if (c->dead)
	{
		xnet_err(e, "08006", "SQL Server connection is not usable");
		return -1;
	}
	pkt = malloc((size_t) c->psize);
	if (pkt == NULL)
		return -1;
	do
	{
		size_t		n = c->oblen - off > chunk ? chunk : c->oblen - off;

		pkt[0] = (unsigned char) type;
		pkt[1] = (off + n >= c->oblen) ? 1 : 0;
		pkt[2] = (unsigned char) ((n + 8) >> 8);
		pkt[3] = (unsigned char) (n + 8);
		pkt[4] = pkt[5] = 0;
		pkt[6] = id++;
		pkt[7] = 0;
		memcpy(pkt + 8, c->ob + off, n);
		if (sock_write(c, pkt, n + 8) < 0)
		{
			free(pkt);
			dead(c, e, "write");
			return -1;
		}
		off += n;
	} while (off < c->oblen);
	free(pkt);
	c->pklen = c->pkpos = 0;
	c->last = 0;
	return 0;
}

/*
 * A message that is too long to be built whole (a file as a parameter):
 * the full packets of what is in c->ob go out now, the rest stays for
 * msg_send, which ends the message.  The last packet is never left empty.
 */
static int
msg_flush(msc *c, int type, xbe_err *e)
{
	size_t		off = 0;
	size_t		chunk = (size_t) c->psize - 8;
	unsigned char *pkt;

	if (c->oblen <= chunk)
		return 0;
	if (c->dead)
	{
		xnet_err(e, "08006", "SQL Server connection is not usable");
		return -1;
	}
	pkt = malloc((size_t) c->psize);
	if (pkt == NULL)
		return -1;
	if (!c->obsent)
		c->obid = 1;
	while (c->oblen - off > chunk)
	{
		pkt[0] = (unsigned char) type;
		pkt[1] = 0;
		pkt[2] = (unsigned char) ((chunk + 8) >> 8);
		pkt[3] = (unsigned char) (chunk + 8);
		pkt[4] = pkt[5] = 0;
		pkt[6] = c->obid++;
		pkt[7] = 0;
		memcpy(pkt + 8, c->ob + off, chunk);
		c->obsent = 1;
		if (sock_write(c, pkt, chunk + 8) < 0)
		{
			free(pkt);
			dead(c, e, "write");
			return -1;
		}
		off += chunk;
	}
	free(pkt);
	memmove(c->ob, c->ob + off, c->oblen - off);
	c->oblen -= off;
	return 0;
}

/* next reply packet */
static int
pk_next(msc *c)
{
	unsigned char h[8];
	size_t		l;

	if (c->last || sock_read(c, h, 8) < 0)
		return -1;
	l = ((size_t) h[2] << 8) | h[3];
	if (l < 8)
		return -1;
	l -= 8;
	if (l > c->pkcap)
	{
		unsigned char *nb = realloc(c->pk, l + 64);

		if (nb == NULL)
			return -1;
		c->pk = nb;
		c->pkcap = l + 64;
	}
	if (l > 0 && sock_read(c, c->pk, l) < 0)
		return -1;
	c->pklen = l;
	c->pkpos = 0;
	c->last = h[1] & 1;
	if (h[4] || h[5])
		c->spid = (h[4] << 8) | h[5];
	return 0;
}

/* reply stream: n bytes into dst (dst NULL = skip) */
static int
rb(msc *c, void *dst, size_t n)
{
	unsigned char *d = dst;

	while (n > 0)
	{
		size_t		k;

		if (c->pkpos >= c->pklen && pk_next(c) < 0)
			return -1;
		k = c->pklen - c->pkpos;
		if (k > n)
			k = n;
		if (d)
		{
			memcpy(d, c->pk + c->pkpos, k);
			d += k;
		}
		c->pkpos += k;
		n -= k;
	}
	return 0;
}

static int
rb8(msc *c, unsigned *v)
{
	unsigned char b;

	if (rb(c, &b, 1) < 0)
		return -1;
	*v = b;
	return 0;
}

static int
rb16(msc *c, unsigned *v)
{
	unsigned char b[2];

	if (rb(c, b, 2) < 0)
		return -1;
	*v = le16(b);
	return 0;
}

static int
rb32(msc *c, unsigned *v)
{
	unsigned char b[4];

	if (rb(c, b, 4) < 0)
		return -1;
	*v = le32(b);
	return 0;
}

/* n bytes of the reply in the scratch buffer */
static unsigned char *
rbuf(msc *c, size_t n)
{
	if (n + 8 > c->sbcap)
	{
		unsigned char *nb = realloc(c->sb, n + 256);

		if (nb == NULL)
			return NULL;
		c->sb = nb;
		c->sbcap = n + 256;
	}
	if (rb(c, c->sb, n) < 0)
		return NULL;
	return c->sb;
}

/* UCS-2 string of nchars characters from the reply as UTF-8 */
static int
rb_ustr(msc *c, unsigned nchars, char *out, int outsize)
{
	unsigned char *p = rbuf(c, (size_t) nchars * 2);
	char	   *t;

	if (p == NULL)
		return -1;
	t = malloc((size_t) nchars * 3 + 1);
	if (t == NULL)
		return -1;
	u16_to_u8(p, (size_t) nchars * 2, t);
	snprintf(out, outsize, "%s", t);
	free(t);
	return 0;
}

/* true when the whole reply has been consumed */
static int
at_end(msc *c)
{
	return c->last && c->pkpos >= c->pklen;
}

/* ------------------------------------------------------ type information */

static int
is_fixed(unsigned t)
{
	return t == 0x1F || t == 0x30 || t == 0x32 || t == 0x34 || t == 0x38 || t == 0x3A ||
		t == 0x3B || t == 0x3C || t == 0x3D || t == 0x3E || t == 0x7A || t == 0x7F;
}

static int
fixed_len(unsigned t)
{
	switch (t)
	{
		case 0x1F:
			return 0;
		case 0x30:
		case 0x32:
			return 1;
		case 0x34:
			return 2;
		case 0x38:
		case 0x3A:
		case 0x3B:
		case 0x7A:
			return 4;
		default:
			return 8;
	}
}

/* TYPE_INFO; colmeta = 1 also reads the table name of text/ntext/image */
static int
read_typeinfo(msc *c, mcol *m, int colmeta)
{
	unsigned	t,
				v;
	unsigned char k[5];

	if (rb8(c, &t) < 0)
		return -1;
	m->type = (unsigned char) t;
	m->maxlen = 0;
	m->prec = m->scale = 0;
	m->ccsid = c->ccsid;
	if (is_fixed(t))
	{
		m->maxlen = fixed_len(t);
		return 0;
	}
	switch (t)
	{
		case 0x24:				/* GUID */
		case 0x26:				/* INTN */
		case 0x68:				/* BITN */
		case 0x6D:				/* FLTN */
		case 0x6E:				/* MONEYN */
		case 0x6F:				/* DATETIMN */
		case 0x2F:				/* legacy CHAR */
		case 0x27:				/* legacy VARCHAR */
		case 0x2D:				/* legacy BINARY */
		case 0x25:				/* legacy VARBINARY */
			if (rb8(c, &v) < 0)
				return -1;
			m->maxlen = (int) v;
			return 0;
		case 0x37:				/* DECIMAL */
		case 0x3F:				/* NUMERIC */
		case 0x6A:				/* DECIMALN */
		case 0x6C:				/* NUMERICN */
			if (rb8(c, &v) < 0)
				return -1;
			m->maxlen = (int) v;
			if (rb8(c, &v) < 0)
				return -1;
			m->prec = (int) v;
			if (rb8(c, &v) < 0)
				return -1;
			m->scale = (int) v;
			return 0;
		case 0xA5:				/* BIGVARBINARY */
		case 0xAD:				/* BIGBINARY */
			if (rb16(c, &v) < 0)
				return -1;
			m->maxlen = (int) v;
			if (v == 0xFFFF)	/* varbinary(max) */
			{
				m->maxlen = 0;
				m->plp = 1;
			}
			return 0;
		case 0xA7:				/* BIGVARCHAR */
		case 0xAF:				/* BIGCHAR */
		case 0xE7:				/* NVARCHAR */
		case 0xEF:				/* NCHAR */
			if (rb16(c, &v) < 0 || rb(c, k, 5) < 0)
				return -1;
			m->maxlen = (int) v;
			if (v == 0xFFFF)	/* varchar(max), nvarchar(max) */
			{
				m->maxlen = 0;
				m->plp = 1;
			}
			m->ccsid = c->ccsid_force ? c->ccsid_force : coll_ccsid(k);
			return 0;
		case 0x23:				/* TEXT */
		case 0x63:				/* NTEXT */
			if (rb32(c, &v) < 0 || rb(c, k, 5) < 0)
				return -1;
			m->maxlen = 0;
			m->ccsid = c->ccsid_force ? c->ccsid_force : coll_ccsid(k);
			break;
		case 0x22:				/* IMAGE */
			if (rb32(c, &v) < 0)
				return -1;
			m->maxlen = 0;
			break;
		case 0x62:				/* SQL_VARIANT */
			if (rb32(c, &v) < 0)
				return -1;
			m->maxlen = 0;
			return 0;
		case 0x28:				/* DATE (TDS 7.3) */
			m->maxlen = 3;
			return 0;
		case 0x29:				/* TIME(n) */
		case 0x2A:				/* DATETIME2(n) */
		case 0x2B:				/* DATETIMEOFFSET(n) */
			if (rb8(c, &v) < 0)
				return -1;
			m->scale = (int) v;
			return 0;
		case 0xF1:				/* XML (TDS 7.2): optional schema collection */
			m->plp = 1;
			if (rb8(c, &v) < 0)
				return -1;
			if (v)
			{
				if (rb8(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0 ||
					rb8(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0 ||
					rb16(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0)
					return -1;
			}
			return 0;
		case 0xF0:				/* CLR user-defined type: binary value */
			m->plp = 1;
			if (rb16(c, &v) < 0 ||
				rb8(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0 ||
				rb8(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0 ||
				rb8(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0 ||
				rb16(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0)
				return -1;
			return 0;
		default:
			return -2;			/* a type this client does not know */
	}
	if (colmeta)
	{
		/* table name: one string, or (TDS 7.2+) a count of name parts */
		unsigned	parts = 1;

		if (c->tds >= 0x72 && rb8(c, &parts) < 0)
			return -1;
		while (parts-- > 0)
			if (rb16(c, &v) < 0 || rb(c, NULL, (size_t) v * 2) < 0)
				return -1;
	}
	return 0;
}

/* XDB column description of a TDS column */
static void
col_kind(const mcol *m, xbe_col *x)
{
	memset(x, 0, sizeof *x);
	snprintf(x->name, sizeof x->name, "%s", m->name);
	switch (m->type)
	{
		case 0x30:
		case 0x32:
		case 0x68:
		case 0x34:
			x->kind = XK_SMALLINT;
			break;
		case 0x38:
			x->kind = XK_INTEGER;
			break;
		case 0x7F:
			x->kind = XK_BIGINT;
			break;
		case 0x26:
			x->kind = m->maxlen <= 2 ? XK_SMALLINT : m->maxlen == 4 ? XK_INTEGER : XK_BIGINT;
			break;
		case 0x3B:
		case 0x3E:
		case 0x6D:
			x->kind = XK_DOUBLE;
			break;
		case 0x3C:
		case 0x7A:
		case 0x6E:
			x->kind = XK_DECIMAL;
			x->prec = (m->type == 0x7A || (m->type == 0x6E && m->maxlen == 4)) ? 10 : 19;
			x->scale = 4;
			break;
		case 0x37:
		case 0x3F:
		case 0x6A:
		case 0x6C:
			x->kind = XK_DECIMAL;
			x->prec = m->prec;
			x->scale = m->scale;
			break;
		case 0x3A:
		case 0x3D:
		case 0x6F:
			x->kind = XK_TIMESTAMP;
			break;
		case 0x24:
			x->kind = XK_CHAR;
			x->len = 36;
			break;
		case 0x28:
			x->kind = XK_DATE;
			break;
		case 0x29:
			x->kind = XK_TIME;
			break;
		case 0x2A:
			x->kind = XK_TIMESTAMP;
			break;
		case 0x2B:
			x->kind = XK_VARCHAR;
			x->len = 34;
			break;
		case 0xAF:
		case 0x2F:
			x->kind = XK_CHAR;
			x->len = m->maxlen;
			break;
		case 0xEF:
			x->kind = XK_CHAR;
			x->len = m->maxlen / 2;
			break;
		case 0xE7:
			x->kind = XK_VARCHAR;
			x->len = m->maxlen / 2;
			break;
		case 0xA5:
		case 0xAD:
		case 0x2D:
		case 0x25:
			x->kind = XK_VARCHAR;
			x->len = m->maxlen * 2;
			break;
		default:
			x->kind = XK_VARCHAR;
			x->len = m->maxlen;
			break;
	}
}

/* ------------------------------------------------------- value formatting */

/* days since 1900-01-01 -> y, m, d */
static void
civil(long days, int *y, int *m, int *d)
{
	long		z = days - 25567 + 719468;
	long		era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned long doe = (unsigned long) (z - era * 146097);
	unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned long mp = (5 * doy + 2) / 153;

	*d = (int) (doy - (153 * mp + 2) / 5 + 1);
	*m = (int) (mp < 10 ? mp + 3 : mp - 9);
	*y = (int) ((long) yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

/* unsigned little-endian integer of n bytes (n <= 16) as decimal digits */
static void
mag_str(const unsigned char *p, int n, char *out)
{
	unsigned	w[4] = {0, 0, 0, 0};
	char		tmp[48];
	int			i,
				k = 0,
				nz;

	for (i = 0; i < n && i < 16; i++)
		w[i / 4] |= (unsigned) p[i] << (8 * (i % 4));
	do
	{
		unsigned long long rem = 0;

		nz = 0;
		for (i = 3; i >= 0; i--)
		{
			unsigned long long cur = (rem << 32) | w[i];

			w[i] = (unsigned) (cur / 10);
			rem = cur % 10;
			if (w[i])
				nz = 1;
		}
		tmp[k++] = (char) ('0' + (int) rem);
	} while (nz && k < 46);
	for (i = 0; i < k; i++)
		out[i] = tmp[k - 1 - i];
	out[k] = '\0';
}

/* digits with an implied scale -> "[-]int.frac" */
static void
scaled(const char *digits, int neg, int scale, char *out)
{
	int			l = (int) strlen(digits);
	char	   *o = out;
	int			i;

	if (neg)
		*o++ = '-';
	if (l <= scale)
	{
		*o++ = '0';
		if (scale > 0)
		{
			*o++ = '.';
			for (i = 0; i < scale - l; i++)
				*o++ = '0';
			memcpy(o, digits, l);
			o += l;
		}
	}
	else
	{
		memcpy(o, digits, l - scale);
		o += l - scale;
		if (scale > 0)
		{
			*o++ = '.';
			memcpy(o, digits + l - scale, scale);
			o += scale;
		}
	}
	*o = '\0';
}

static double
get_double(const unsigned char *p, int n)
{
	unsigned char r[8];
	int			one = 1,
				i;
	double		d;
	float		f;

	for (i = 0; i < n; i++)
		r[i] = *(char *) &one ? p[i] : p[n - 1 - i];
	if (n == 4)
	{
		memcpy(&f, r, 4);
		return (double) f;
	}
	memcpy(&d, r, 8);
	return d;
}

/*
 * One value (len bytes at d, never NULL) of the given type as UTF-8 text;
 * the result is malloc'ed.
 */
static char *
fmt_value(unsigned type, const unsigned char *d, size_t len, int prec, int scale, int ccsid)
{
	char		t[96];
	char	   *out;
	int			y,
				mo,
				dd;

	t[0] = '\0';
	switch (type)
	{
		case 0x30:
			snprintf(t, sizeof t, "%u", (unsigned) d[0]);
			break;
		case 0x32:
		case 0x68:
			snprintf(t, sizeof t, "%d", d[0] ? 1 : 0);
			break;
		case 0x34:
			snprintf(t, sizeof t, "%d", (int) (short) le16(d));
			break;
		case 0x38:
			snprintf(t, sizeof t, "%d", (int) le32(d));
			break;
		case 0x7F:
			snprintf(t, sizeof t, "%lld",
					 (long long) ((unsigned long long) le32(d) | ((unsigned long long) le32(d + 4) << 32)));
			break;
		case 0x26:
			if (len == 1)
				return fmt_value(0x30, d, len, 0, 0, 0);
			if (len == 2)
				return fmt_value(0x34, d, len, 0, 0, 0);
			if (len == 4)
				return fmt_value(0x38, d, len, 0, 0, 0);
			return fmt_value(0x7F, d, len, 0, 0, 0);
		case 0x3B:
			snprintf(t, sizeof t, "%.9g", get_double(d, 4));
			break;
		case 0x3E:
			snprintf(t, sizeof t, "%.17g", get_double(d, 8));
			break;
		case 0x6D:
			return fmt_value(len == 4 ? 0x3B : 0x3E, d, len, 0, 0, 0);
		case 0x7A:				/* smallmoney: int32 of 1/10000 */
		case 0x3C:				/* money: high int32, low uint32 */
		case 0x6E:
			{
				long long	v;
				unsigned long long u;
				char		dig[32];

				if (len == 4)
					v = (int) le32(d);
				else
					v = (long long) (((unsigned long long) le32(d) << 32) | le32(d + 4));
				u = v < 0 ? (unsigned long long) (-(v + 1)) + 1 : (unsigned long long) v;
				snprintf(dig, sizeof dig, "%llu", u);
				scaled(dig, v < 0, 4, t);
				break;
			}
		case 0x3A:				/* smalldatetime: days, minutes */
			civil((long) le16(d), &y, &mo, &dd);
			snprintf(t, sizeof t, "%04d-%02d-%02d %02u:%02u:00.000", y, mo, dd,
					 le16(d + 2) / 60, le16(d + 2) % 60);
			break;
		case 0x3D:				/* datetime: days, 1/300 s */
			{
				unsigned	tk = le32(d + 4);
				unsigned	ms = (unsigned) (((unsigned long long) (tk % 300) * 1000 + 150) / 300);
				unsigned	sec = tk / 300;

				if (ms >= 1000)
				{
					ms -= 1000;
					sec++;
				}
				civil((long) (int) le32(d), &y, &mo, &dd);
				snprintf(t, sizeof t, "%04d-%02d-%02d %02u:%02u:%02u.%03u", y, mo, dd,
						 sec / 3600, (sec / 60) % 60, sec % 60, ms);
				break;
			}
		case 0x6F:
			return fmt_value(len == 4 ? 0x3A : 0x3D, d, len, 0, 0, 0);
		case 0x37:
		case 0x3F:
		case 0x6A:
		case 0x6C:
			{
				char		dig[48];

				mag_str(d + 1, (int) len - 1, dig);
				scaled(dig, d[0] == 0, scale, t);
				break;
			}
		case 0x28:				/* date: days since 0001-01-01 */
			civil((long) (d[0] | (d[1] << 8) | (d[2] << 16)) - 693595, &y, &mo, &dd);
			snprintf(t, sizeof t, "%04d-%02d-%02d", y, mo, dd);
			break;
		case 0x29:				/* time, datetime2, datetimeoffset */
		case 0x2A:
		case 0x2B:
			{
				int			tl = (int) len - (type == 0x29 ? 0 : type == 0x2A ? 3 : 5);
				unsigned long long v = 0;
				long long	days = 0,
							secs;
				int			i,
							off = 0,
							frac;

				if (tl < 3 || tl > 5)
					return strdup("");
				for (i = tl - 1; i >= 0; i--)
					v = (v << 8) | d[i];
				for (i = scale; i < 7; i++)
					v *= 10;	/* now in 100 ns units */
				if (type != 0x29)
					days = (long long) (d[tl] | (d[tl + 1] << 8) | (d[tl + 2] << 16)) - 693595;
				if (type == 0x2B)
				{
					/* the value is UTC: show local time and the offset */
					long long	t100;

					off = (int) (short) le16(d + tl + 3);
					t100 = (long long) v + (long long) off * 600000000LL;
					if (t100 < 0)
					{
						t100 += 864000000000LL;
						days--;
					}
					else if (t100 >= 864000000000LL)
					{
						t100 -= 864000000000LL;
						days++;
					}
					v = (unsigned long long) t100;
				}
				secs = (long long) (v / 10000000ULL);
				frac = (int) (v % 10000000ULL);
				if (type == 0x29)
				{
					snprintf(t, sizeof t, "%02d:%02d:%02d", (int) (secs / 3600), (int) (secs / 60 % 60), (int) (secs % 60));
					break;
				}
				civil((long) days, &y, &mo, &dd);
				if (type == 0x2A)
					snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d:%02d.%06d", y, mo, dd,
							 (int) (secs / 3600), (int) (secs / 60 % 60), (int) (secs % 60), frac / 10);
				else
					snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d:%02d.%07d %c%02d:%02d", y, mo, dd,
							 (int) (secs / 3600), (int) (secs / 60 % 60), (int) (secs % 60), frac,
							 off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
				break;
			}
		case 0x24:
			snprintf(t, sizeof t, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
					 d[3], d[2], d[1], d[0], d[5], d[4], d[7], d[6], d[8], d[9],
					 d[10], d[11], d[12], d[13], d[14], d[15]);
			break;
		case 0xE7:
		case 0xEF:
		case 0x63:
		case 0xF1:
			out = malloc(len / 2 * 3 + 4);
			if (out)
				u16_to_u8(d, len, out);
			return out;
		case 0xA7:
		case 0xAF:
		case 0x2F:
		case 0x27:
		case 0x23:
			out = malloc(len * 4 + 4);
			if (out == NULL)
				return NULL;
			if (ccsid == 1208 || len == 0)
			{
				memcpy(out, d, len);
				out[len] = '\0';
			}
			else
			{
				int			n = ile_conv(ccsid, 1208, (const char *) d, len, out, len * 4 + 2);

				if (n < 0)		/* unknown code page: keep the ASCII part */
				{
					size_t		i;

					for (i = 0; i < len; i++)
						out[i] = d[i] < 0x80 ? (char) d[i] : '?';
					n = (int) len;
				}
				out[n] = '\0';
			}
			return out;
		default:				/* binary, image, anything else: hex */
			{
				static const char hx[] = "0123456789ABCDEF";
				size_t		i;

				out = malloc(len * 2 + 1);
				if (out == NULL)
					return NULL;
				for (i = 0; i < len; i++)
				{
					out[2 * i] = hx[d[i] >> 4];
					out[2 * i + 1] = hx[d[i] & 15];
				}
				out[2 * len] = '\0';
				return out;
			}
	}
	return strdup(t);
}

/* reads one value of column m from the reply; *out = NULL for SQL NULL */
static int
read_value(msc *c, const mcol *m, char **out)
{
	unsigned	t = m->type,
				v;
	size_t		len;
	unsigned char *d;

	*out = NULL;
	if (m->plp)
	{
		/* 8-byte total length, then chunks of 4-byte length + data, ended by 0 */
		unsigned char h[8];
		size_t		have = 0;

		if (rb(c, h, 8) < 0)
			return -1;
		if (le32(h) == 0xFFFFFFFFu && le32(h + 4) == 0xFFFFFFFFu)
			return 0;
		for (;;)
		{
			if (rb32(c, &v) < 0)
				return -1;
			if (v == 0)
				break;
			if (have + v + 8 > c->sbcap)
			{
				unsigned char *nb = realloc(c->sb, (have + v) * 2 + 256);

				if (nb == NULL)
					return -1;
				c->sb = nb;
				c->sbcap = (have + v) * 2 + 256;
			}
			if (rb(c, c->sb + have, v) < 0)
				return -1;
			have += v;
		}
		if (c->sb == NULL && (c->sb = malloc(256)) != NULL)
			c->sbcap = 256;
		if (c->sb == NULL)
			return -1;
		*out = fmt_value(t, c->sb, have, 0, 0, m->ccsid);
		return *out ? 0 : -1;
	}
	if (is_fixed(t))
	{
		if (t == 0x1F)
			return 0;
		len = (size_t) fixed_len(t);
	}
	else
		switch (t)
		{
			case 0x28:
			case 0x29:
			case 0x2A:
			case 0x2B:
			case 0x24:
			case 0x26:
			case 0x68:
			case 0x6D:
			case 0x6E:
			case 0x6F:
			case 0x37:
			case 0x3F:
			case 0x6A:
			case 0x6C:
			case 0x2F:
			case 0x27:
			case 0x2D:
			case 0x25:
				if (rb8(c, &v) < 0)
					return -1;
				if (v == 0 && t != 0x2F && t != 0x27 && t != 0x2D && t != 0x25)
					return 0;
				if (v == 0xFF && (t == 0x2F || t == 0x27 || t == 0x2D || t == 0x25))
					return 0;
				len = v;
				break;
			case 0xA5:
			case 0xAD:
			case 0xA7:
			case 0xAF:
			case 0xE7:
			case 0xEF:
				if (rb16(c, &v) < 0)
					return -1;
				if (v == 0xFFFF)
					return 0;
				len = v;
				break;
			case 0x23:
			case 0x63:
			case 0x22:
				if (rb8(c, &v) < 0)
					return -1;
				if (v == 0)
					return 0;	/* no text pointer: NULL */
				if (rb(c, NULL, (size_t) v + 8) < 0 || rb32(c, &v) < 0)
					return -1;
				len = v;
				break;
			case 0x62:			/* sql_variant: base type, properties, data */
				{
					unsigned	total,
								base,
								pl;
					unsigned char props[16];
					int			prec = 0,
								scale = 0,
								ccsid = c->ccsid;

					if (rb32(c, &total) < 0)
						return -1;
					if (total == 0)
						return 0;
					if (total < 2 || rb8(c, &base) < 0 || rb8(c, &pl) < 0 || pl > sizeof props ||
						total < 2 + pl || rb(c, props, pl) < 0)
						return -1;
					len = total - 2 - pl;
					d = rbuf(c, len);
					if (d == NULL)
						return -1;
					if ((base == 0x6A || base == 0x6C || base == 0x37 || base == 0x3F) && pl >= 2)
					{
						prec = props[0];
						scale = props[1];
					}
					if ((base == 0x29 || base == 0x2A || base == 0x2B) && pl >= 1)
						scale = props[0];
					if ((base == 0xA7 || base == 0xAF || base == 0xE7 || base == 0xEF) && pl >= 5)
						ccsid = c->ccsid_force ? c->ccsid_force : coll_ccsid(props);
					*out = fmt_value(base, d, len, prec, scale, ccsid);
					return *out ? 0 : -1;
				}
			default:
				return -1;
		}
	d = rbuf(c, len);
	if (d == NULL)
		return -1;
	*out = fmt_value(t, d, len, m->prec, m->scale, m->ccsid);
	return *out ? 0 : -1;
}

/* ------------------------------------- file columns (see xdblob.c): reading */

static void
lob_err(xbe_err *e, const char *state, const char *msg)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", state);
	snprintf(e->msg, sizeof e->msg, "%s", msg);
}

static int
lob_bufs(msc *c)
{
	if (c->fbuf == NULL)
		c->fbuf = malloc(LOB_PIECE + 16);
	if (c->fbuf2 == NULL)
		c->fbuf2 = malloc(2 * LOB_PIECE + 64);
	return c->fbuf && c->fbuf2 ? 0 : -1;
}

/* the value of a column as it arrives, whatever way the server sends it */
typedef struct
{
	int			plp;			/* in chunks, each with its length */
	unsigned long long left;	/* bytes of the chunk, or of the value, not read yet */
	int			end;
} msvs;

/* 0 = a value follows, 1 = it is NULL, -1 = the reply cannot be followed */
static int
vs_begin(msc *c, const mcol *m, msvs *v)
{
	unsigned	x;
	unsigned char h[8];

	memset(v, 0, sizeof *v);
	if (m->plp)
	{
		if (rb(c, h, 8) < 0)
			return -1;
		if (le32(h) == 0xFFFFFFFFu && le32(h + 4) == 0xFFFFFFFFu)
			return 1;
		v->plp = 1;
		return 0;
	}
	switch (m->type)
	{
		case 0x22:				/* image, text, ntext: text pointer, timestamp, length */
		case 0x23:
		case 0x63:
			if (rb8(c, &x) < 0)
				return -1;
			if (x == 0)
				return 1;
			if (rb(c, NULL, (size_t) x + 8) < 0 || rb32(c, &x) < 0)
				return -1;
			v->left = x;
			return 0;
		case 0xA5:
		case 0xAD:
		case 0xA7:
		case 0xAF:
		case 0xE7:
		case 0xEF:
			if (rb16(c, &x) < 0)
				return -1;
			if (x == 0xFFFF)
				return 1;
			v->left = x;
			return 0;
		default:
			return -1;
	}
}

/* up to n bytes of the value; *got = 0 when it has ended */
static int
vs_read(msc *c, msvs *v, unsigned char *buf, size_t n, size_t *got)
{
	*got = 0;
	if (v->end)
		return 0;
	if (v->left == 0)
	{
		unsigned	x;

		if (!v->plp)
		{
			v->end = 1;
			return 0;
		}
		if (rb32(c, &x) < 0)
			return -1;
		if (x == 0)
		{
			v->end = 1;
			return 0;
		}
		v->left = x;
	}
	if (n > v->left)
		n = (size_t) v->left;
	if (rb(c, buf, n) < 0)
		return -1;
	v->left -= n;
	*got = n;
	return 0;
}

/* a failure with a file while a reply is read: the first one is kept, the reply is read on */
static void
sink_fail(msc *c, const char *state, const char *msg)
{
	if (!c->fbad)
	{
		lob_err(&c->ferr, state, msg);
		c->fbad = 1;
	}
}

/*
 * A piece of the value of a file column to its file.  Text arrives as
 * UTF-16 and is written as UTF-8; what a piece cuts (half a character, the
 * first half of a surrogate pair) waits in pend for the next.
 */
static void
sink_write(msc *c, xlob_dst *dst, int clob, const unsigned char *p, size_t n,
		   unsigned char *pend, int *npend, int last)
{
	if (c->fbad || dst == NULL)
		return;
	if (!clob)
	{
		if (n > 0 && dst->write(dst->ctx, (const char *) p, (int) n, &c->ferr) < 0)
			c->fbad = 1;
		return;
	}
	while (n > 0 || (last && *npend > 0))
	{
		unsigned char w[4096 + 8];
		char		o[6144 + 32];
		size_t		k = n < 4096 ? n : 4096,
					tot,
					use,
					m;

		memcpy(w, pend, (size_t) *npend);
		memcpy(w + *npend, p, k);
		tot = (size_t) *npend + k;
		p += k;
		n -= k;
		use = tot & ~(size_t) 1;
		if (use >= 2 && !(last && n == 0))
		{
			unsigned	u = le16(w + use - 2);

			if (u >= 0xD800 && u < 0xDC00)
				use -= 2;		/* its second half comes with the next piece */
		}
		*npend = (int) (tot - use);
		memcpy(pend, w + use, (size_t) *npend);
		if (last && n == 0)
			*npend = 0;			/* an odd byte at the very end is no character */
		m = u16_to_u8(w, use, o);
		if (m > 0 && dst->write(dst->ctx, o, (int) m, &c->ferr) < 0)
		{
			c->fbad = 1;
			return;
		}
	}
}

/*
 * The value of file column k of a row (ms_lob_out_sql): the letter B or
 * C, the name of the file and a zero, all as UTF-16, then the bytes (for C
 * text as UTF-16).  It goes to its destination as it arrives; *out gets 24
 * bytes for the core, or stays NULL for a null value.  -1 = the reply cannot
 * be followed any further.  A failure with the file does not stop the
 * reading: c->ferr keeps it.
 */
static int
read_sink(msc *c, const mcol *m, int k, char **out)
{
	msvs		v;
	unsigned char head[2 + 2 * LOB_NAMEUNITS + 2];
	char		path[3 * LOB_NAMEUNITS + 8];
	size_t		have = 0,
				got,
				z;
	xlob_dst   *dst = (c->dst && k < c->ndst) ? &c->dst[k] : NULL;
	unsigned char pend[4];
	int			npend = 0,
				clob,
				rc = vs_begin(c, m, &v);

	*out = NULL;
	if (rc != 0)
		return rc < 0 ? -1 : 0;
	if (lob_bufs(c) < 0)
		return -1;
	while (have < sizeof head)
	{
		if (vs_read(c, &v, head + have, sizeof head - have, &got) < 0)
			return -1;
		if (got == 0)
			break;
		have += got;
	}
	clob = have >= 2 && head[0] == 'C';
	for (z = 2; z + 1 < have; z += 2)
		if (head[z] == 0 && head[z + 1] == 0)
			break;
	if (have < 4 || head[1] != 0 || (head[0] != 'B' && head[0] != 'C') || (z + 1 >= have && have < sizeof head))
		sink_fail(c, "58004", "a file column came back from the server in a form the driver does not know");
	else if (z + 1 >= have || u16_to_u8(head + 2, z - 2, path) >= LOB_NAMEMAX)
		sink_fail(c, "428A1", "file name of a row is too long (reason 2)");
	else if (dst == NULL)
		sink_fail(c, "58004", "a file column arrived and nothing says where it goes");
	else if (!c->fbad && dst->open(dst->ctx, path, &c->ferr) < 0)
		c->fbad = 1;
	if (z + 2 < have)
		sink_write(c, dst, clob, head + z + 2, have - z - 2, pend, &npend, 0);
	for (;;)
	{
		if (vs_read(c, &v, c->fbuf, LOB_PIECE, &got) < 0)
			return -1;
		if (got == 0)
			break;
		sink_write(c, dst, clob, c->fbuf, got, pend, &npend, 0);
	}
	sink_write(c, dst, clob, NULL, 0, pend, &npend, 1);
	if (!c->fbad && dst != NULL)
		dst->done(dst->ctx);
	*out = calloc(1, 24);
	return *out ? 0 : -1;
}

/* is column j of the rows being read one whose value goes to a file?  Its number among them, or -1 */
static int
rsink_of(const msc *c, int j)
{
	int			k;

	for (k = 0; k < c->nrsink; k++)
		if (c->rsink[k] == j)
			return k;
	return -1;
}

/* ---------------------------------------------------------------- replies */

static void
batch_release(xbe_batch *b)
{
	int			i;

	for (i = 0; i < b->nrows * b->ncols; i++)
		free(b->val[i]);
	free(b->val);
	memset(b, 0, sizeof *b);
}

/* ERROR / INFO token body */
static int
tok_message(msc *c, int is_error)
{
	unsigned	l,
				num,
				state,
				cls,
				n;
	char		msg[512];

	if (rb16(c, &l) < 0 || rb32(c, &num) < 0 || rb8(c, &state) < 0 || rb8(c, &cls) < 0 ||
		rb16(c, &n) < 0 || rb_ustr(c, n, msg, sizeof msg) < 0)
		return -1;
	/* server name, procedure name, line number */
	if (l < 8 + n * 2 || rb(c, NULL, l - 8 - n * 2) < 0)
		return -1;
	if (is_error && cls > 10 && !c->have_err)
	{
		memset(&c->err, 0, sizeof c->err);
		c->err.native = (int) num;
		snprintf(c->err.sqlstate, sizeof c->err.sqlstate, "%s", err_state((int) num, (int) cls));
		snprintf(c->err.msg, sizeof c->err.msg, "%s", msg);
		c->have_err = 1;
	}
	return 0;
}

static int
tok_envchange(msc *c)
{
	unsigned	l,
				type,
				n;
	unsigned char *p;

	if (rb16(c, &l) < 0 || l < 1)
		return -1;
	p = rbuf(c, l);
	if (p == NULL)
		return -1;
	type = p[0];
	if (type == 4 && l >= 2)	/* packet size: new value as a UCS-2 number */
	{
		char		t[16];
		unsigned	i;

		n = p[1];
		for (i = 0; i < n && i < 15 && 2 + 2 * i < l; i++)
			t[i] = (char) p[2 + 2 * i];
		t[i] = '\0';
		if (atoi(t) >= 512 && atoi(t) <= 32767)
			c->psize = atoi(t);
	}
	else if (type == 8 && l >= 10 && p[1] == 8)	/* a transaction began */
	{
		memcpy(c->txd, p + 2, 8);
		c->txd_seen = 1;
	}
	else if (type == 9 || type == 10 || type == 17) /* committed, rolled back, ended */
		memset(c->txd, 0, 8);
	else if (type == 7 && l >= 7 && p[1] == 5)	/* SQL collation */
	{
		memcpy(c->coll, p + 2, 5);
		c->ccsid = c->ccsid_force ? c->ccsid_force : coll_ccsid(c->coll);
	}
	else if (type == 1 && l >= 2)	/* database: the new name in UCS-2 */
	{
		unsigned	i,
					k = 0;

		n = p[1];
		for (i = 0; i < n && 3 + 2 * i < l && k + 4 < sizeof c->dbname; i++)
		{
			unsigned	u = p[2 + 2 * i] | ((unsigned) p[3 + 2 * i] << 8);

			if (u < 0x80)
				c->dbname[k++] = (char) u;
			else if (u < 0x800)
			{
				c->dbname[k++] = (char) (0xC0 | (u >> 6));
				c->dbname[k++] = (char) (0x80 | (u & 0x3F));
			}
			else
			{
				c->dbname[k++] = (char) (0xE0 | (u >> 12));
				c->dbname[k++] = (char) (0x80 | ((u >> 6) & 0x3F));
				c->dbname[k++] = (char) (0x80 | (u & 0x3F));
			}
		}
		c->dbname[k] = '\0';
	}
	return 0;
}

static int
tok_colmetadata(msc *c)
{
	unsigned	n,
				v,
				i;

	if (rb16(c, &n) < 0)
		return -1;
	if (c->first_meta && c->got_meta && c->cols1 == NULL && c->ncols > 0)
	{
		c->cols1 = c->cols;		/* set aside: restored when the reply ends */
		c->ncols1 = c->ncols;
		c->cols = NULL;
	}
	free(c->cols);
	c->cols = NULL;
	c->ncols = 0;
	c->got_meta = 1;
	if (n == 0xFFFF || n == 0)
		return 0;
	c->cols = calloc(n, sizeof(mcol));
	if (c->cols == NULL)
		return -1;
	for (i = 0; i < n; i++)
	{
		mcol	   *m = &c->cols[i];
		int			rc;

		/* user type (2 bytes up to TDS 7.1, then 4) and flags */
		if (rb(c, NULL, c->tds >= 0x72 ? 6 : 4) < 0)
			return -1;
		rc = read_typeinfo(c, m, 1);
		if (rc < 0)
			return rc;
		if (rb8(c, &v) < 0 || rb_ustr(c, v, m->name, sizeof m->name) < 0)
			return -1;
	}
	c->ncols = (int) n;
	return 0;
}

/*
 * Reads a whole reply.  Rows go to sink (NULL: they are dropped).  Returns
 * 0, or -1 with e filled in: either the first error the server reported or
 * a lost connection.
 *
 * A reply can carry several result sets (a procedure with more than one
 * SELECT), each with columns of its own.  The sink is one table: it takes
 * the rows of the first result set only, and c->cols0 is a copy of their
 * columns; the rows of the others are read and dropped.
 */
static int
reply(msc *c, xbe_batch *sink, xbe_err *e)
{
	c->have_err = 0;
	c->count = -1;
	c->nrv = 0;
	c->got_meta = 0;
	c->sets = 0;
	free(c->cols0);
	c->cols0 = NULL;
	c->ncols0 = 0;
	if (!c->keep_meta)
	{
		/* columns of an earlier reply must not be taken for this one's */
		free(c->cols);
		c->cols = NULL;
		c->ncols = 0;
	}
	c->keep_meta = 0;
	while (!at_end(c))
	{
		unsigned	tok,
					v,
					l;
		int			rc = 0;

		if (rb8(c, &tok) < 0)
		{
			dead(c, e, "read");
			return -1;
		}
		switch (tok)
		{
			case 0x81:
				rc = tok_colmetadata(c);
				if (rc == 0 && c->ncols > 0 && ++c->sets == 1 && sink)
				{
					c->cols0 = malloc(sizeof(mcol) * (size_t) c->ncols);
					if (c->cols0 == NULL)
						rc = -1;
					else
					{
						memcpy(c->cols0, c->cols, sizeof(mcol) * (size_t) c->ncols);
						c->ncols0 = c->ncols;
					}
				}
				break;
			case 0xD1:			/* ROW */
			case 0xD2:			/* NBCROW (TDS 7.3): a bitmap marks the NULL columns */
				{
					int			j;
					char	  **row = NULL;
					unsigned char nbc[512];

					memset(nbc, 0, sizeof nbc);
					if (tok == 0xD2 && (c->ncols > (int) sizeof nbc * 8 || rb(c, nbc, (size_t) (c->ncols + 7) / 8) < 0))
					{
						rc = -1;
						break;
					}

					if (sink && c->sets <= 1)
					{
						if (xnet_batch_room(sink, c->ncols) < 0)
						{
							rc = -1;
							break;
						}
						row = &sink->val[sink->nrows * c->ncols];
						for (j = 0; j < c->ncols; j++)
							row[j] = NULL;
						sink->ncols = c->ncols;
						sink->nrows++;
					}
					for (j = 0; j < c->ncols && rc == 0; j++)
					{
						char	   *val = NULL;

						if (!(nbc[j / 8] & (1 << (j % 8))))
						{
							int			fk = c->nrsink > 0 ? rsink_of(c, j) : -1;

							rc = fk >= 0 ? read_sink(c, &c->cols[j], fk, &val)
								: read_value(c, &c->cols[j], &val);
						}
						if (row)
							row[j] = val;
						else
							free(val);
					}
					break;
				}
			case 0xFD:			/* DONE */
			case 0xFE:			/* DONEPROC */
			case 0xFF:			/* DONEINPROC */
				{
					unsigned char b[12];

					/* the row count has 8 bytes from TDS 7.2 on */
					if (rb(c, b, 8) < 0 || (c->tds >= 0x72 && rb(c, NULL, 4) < 0))
						rc = -1;
					else if ((le16(b) & 0x10) && tok != 0xFE)
						c->count = (long long) le32(b + 4);
					break;
				}
			case 0xAA:
				rc = tok_message(c, 1);
				break;
			case 0xAB:
				rc = tok_message(c, 0);
				break;
			case 0xE3:
				rc = tok_envchange(c);
				break;
			case 0xAD:			/* LOGINACK */
				{
					unsigned char *p;

					if (rb16(c, &l) < 0 || (p = rbuf(c, l)) == NULL || l < 10)
						rc = -1;
					else
					{
						unsigned	i,
									k = 0;

						c->version = p[l - 4] * 100 + p[l - 3];
						/* 71 00 00 01, 74 00 00 04 ...; SQL Server 2000 before SP1 says 07 01 00 00 */
						c->tds = p[1] == 0x07 ? 0x70 + p[2] : p[1] == 0x08 ? 0x80 : p[1];
						/* its name (UCS-2, plain letters) and the four parts of its version */
						memcpy(c->ver4, p + l - 4, 4);
						for (i = 0; i < p[5] && 7 + 2 * i < l - 4 && k < sizeof c->prod - 1; i++)
							if (p[6 + 2 * i] >= 0x20 && p[6 + 2 * i] < 0x7F && p[7 + 2 * i] == 0)
								c->prod[k++] = (char) p[6 + 2 * i];
						c->prod[k] = '\0';
					}
					break;
				}
			case 0x79:			/* RETURNSTATUS */
				rc = rb32(c, &v);
				break;
			case 0xAC:			/* RETURNVALUE */
				{
					mcol		m;
					char	   *val = NULL;

					memset(&m, 0, sizeof m);
					/* ordinal, name, status, user type (2), flags (2) */
					if (rb(c, NULL, 2) < 0 || rb8(c, &v) < 0 ||
						rb(c, NULL, (size_t) v * 2 + (c->tds >= 0x72 ? 7 : 5)) < 0)
						rc = -1;
					else if ((rc = read_typeinfo(c, &m, 0)) == 0 && (rc = read_value(c, &m, &val)) == 0)
					{
						if (c->nrv < MAXRV)
							c->rv[c->nrv++] = val ? atoi(val) : 0;
						free(val);
					}
					break;
				}
			case 0xA9:			/* ORDER */
			case 0xA4:			/* TABNAME */
			case 0xA5:			/* COLINFO */
			case 0xA0:
			case 0xA1:
			case 0xED:			/* SSPI */
				if (rb16(c, &l) < 0 || rb(c, NULL, l) < 0)
					rc = -1;
				break;
			default:
				rc = -2;
				break;
		}
		if (rc != 0)
		{
			/* the stream cannot be followed any further */
			c->dead = 1;
			if (c->have_err)
				*e = c->err;
			else if (rc == -2)
				xnet_err(e, "08006", "SQL Server sent data this client does not understand (token x%02X)", tok);
			else
				xnet_err(e, "08006", "SQL Server connection lost (read)");
			return -1;
		}
	}
	if (c->cols1)
	{
		free(c->cols);
		c->cols = c->cols1;
		c->ncols = c->ncols1;
		c->cols1 = NULL;
	}
	c->first_meta = 0;
	if (c->have_err)
	{
		*e = c->err;
		return -1;
	}
	return 0;
}

/* --------------------------------------------------------------- requests */

/* starts a request; from TDS 7.2 on it carries the transaction descriptor */
static void
req_begin(msc *c)
{
	c->oblen = 0;
	c->obsent = 0;
	if (c->tds >= 0x72)
	{
		ob_u32(c, 22);			/* all headers */
		ob_u32(c, 18);			/* this header */
		ob_u16(c, 2);			/* transaction descriptor */
		ob_put(c, c->txd, 8);
		ob_u32(c, 1);			/* outstanding requests */
	}
}

static int
send_batch(msc *c, const char *sql, xbe_err *e)
{
	req_begin(c);
	ob_ucs2(c, sql);
	return msg_send(c, PKT_BATCH, e);
}

/* runs a batch and drops its rows */
static int
batch(msc *c, const char *sql, xbe_err *e)
{
	if (send_batch(c, sql, e) < 0)
		return -1;
	return reply(c, NULL, e);
}

static void
rpc_begin(msc *c, const char *proc)
{
	req_begin(c);
	ob_u16(c, (unsigned) strlen(proc));
	ob_ucs2(c, proc);
	ob_u16(c, 0);				/* option flags */
}

static void
rpc_int(msc *c, int v, int isnull, int output)
{
	ob_u8(c, 0);				/* no name */
	ob_u8(c, output ? 1 : 0);
	ob_u8(c, 0x26);
	ob_u8(c, 4);
	if (isnull)
		ob_u8(c, 0);
	else
	{
		ob_u8(c, 4);
		ob_u32(c, (unsigned) v);
	}
}

/* a string parameter: nvarchar, or ntext when it is longer than 4000 characters */
static void
rpc_str(msc *c, const char *s)
{
	size_t		l = s ? strlen(s) : 0;
	unsigned char *u = NULL;
	size_t		n = 0;

	if (s)
	{
		u = malloc(l * 2 + 2);
		if (u == NULL)
			return;
		n = u8_to_u16(s, l, u);
	}
	ob_u8(c, 0);
	ob_u8(c, 0);
	if (n <= 8000)
	{
		ob_u8(c, 0xE7);
		ob_u16(c, 8000);
		ob_put(c, c->coll, 5);
		if (s == NULL)
			ob_u16(c, 0xFFFF);
		else
		{
			ob_u16(c, (unsigned) n);
			ob_put(c, u, n);
		}
	}
	else
	{
		ob_u8(c, 0x63);
		ob_u32(c, (unsigned) n);
		ob_put(c, c->coll, 5);
		ob_u32(c, (unsigned) n);
		ob_put(c, u, n);
	}
	free(u);
}

/* highest parameter number @Pn used in a statement */
static int
count_markers(const char *sql)
{
	const char *p = sql;
	int			max = 0;

	while (*p)
	{
		if (*p == '\'' || *p == '"')
		{
			char		q = *p++;

			while (*p && *p != q)
				p++;
			if (*p)
				p++;
			continue;
		}
		if (p[0] == '@' && p[1] == 'P' && p[2] >= '1' && p[2] <= '9' &&
			(p == sql || !(p[-1] == '@' || p[-1] == '_' || (p[-1] >= '0' && p[-1] <= '9') ||
						   ((p[-1] | 0x20) >= 'a' && (p[-1] | 0x20) <= 'z'))))
		{
			int			n = atoi(p + 2);

			if (n > max)
				max = n;
		}
		p++;
	}
	return max;
}

/* is every character of s available in the server's code page? */
static int
fits_codepage(msc *c, const char *s)
{
	const unsigned char *p;
	int			ascii = 1;
	size_t		l = strlen(s);
	char	   *a,
			   *b;
	int			n,
				m,
				ok;

	for (p = (const unsigned char *) s; *p; p++)
		if (*p >= 0x80)
			ascii = 0;
	if (ascii || c->ccsid == 1208)
		return 1;
	a = malloc(l * 2 + 8);
	b = malloc(l * 4 + 8);
	ok = 0;
	if (a && b)
	{
		n = ile_conv(1208, c->ccsid, s, l, a, l * 2 + 4);
		if (n >= 0)
		{
			m = ile_conv(c->ccsid, 1208, a, (size_t) n, b, l * 4 + 4);
			ok = (m == (int) l && memcmp(b, s, l) == 0);
		}
	}
	free(a);
	free(b);
	return ok;
}

/* days since 1900-01-01 of a civil date */
static long
days_1900(int y, int m, int d)
{
	long		era,
				yoe,
				doy,
				doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468 + 25567;
}

/* "YYYY-MM-DD[ HH:MM:SS[.ffffff]]" or "HH:MM:SS[.f]" -> days since 1900, 100 ns of the day */
static int
parse_ts(const char *v, long *days, unsigned long long *t100)
{
	int			y = 1900,
				mo = 1,
				d = 1,
				h = 0,
				mi = 0,
				sec = 0,
				n = 0;
	unsigned long long frac = 0;
	const char *p = v;

	if (strlen(v) >= 10 && v[4] == '-')
	{
		if (sscanf(v, "%4d-%2d-%2d", &y, &mo, &d) != 3)
			return -1;
		p = v + 10;
		if (*p == ' ' || *p == 'T' || *p == '-')
			p++;
	}
	if (*p)
	{
		if (sscanf(p, "%2d%*c%2d%*c%2d", &h, &mi, &sec) != 3)
			return -1;
		p += 8;
		if (*p == '.')
			for (p++; *p >= '0' && *p <= '9' && n < 7; p++, n++)
				frac = frac * 10 + (unsigned) (*p - '0');
		for (; n < 7; n++)
			frac *= 10;
	}
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59)
		return -1;
	*days = days_1900(y, mo, d);
	*t100 = ((unsigned long long) h * 3600 + (unsigned long long) mi * 60 + (unsigned long long) sec) * 10000000ULL + frac;
	return 0;
}

static void
rpc_phdr(msc *c)
{
	ob_u8(c, 0);				/* no name: positional */
	ob_u8(c, 0);
}

/*
 * One parameter value in the TDS type that its declaration names, so that
 * the server has nothing to convert.  Returns -1 when the text is not a
 * value of that type.
 */
static int
rpc_typed(msc *c, char code, const char *v, int scale)
{
	unsigned char b[24];
	int			i;

	rpc_phdr(c);
	switch (code)
	{
		case 'i':				/* int */
		case 'l':				/* bigint */
			{
				int			sz = code == 'i' ? 4 : 8;
				long long	x = v ? strtoll(v, NULL, 10) : 0;

				ob_u8(c, 0x26);
				ob_u8(c, (unsigned) sz);
				ob_u8(c, v ? (unsigned) sz : 0);
				for (i = 0; v && i < sz; i++)
					ob_u8(c, (unsigned) ((unsigned long long) x >> (8 * i)) & 0xFF);
				return 0;
			}
		case 'f':				/* float */
			{
				double		d = v ? strtod(v, NULL) : 0;
				int			one = 1;

				ob_u8(c, 0x6D);
				ob_u8(c, 8);
				ob_u8(c, v ? 8 : 0);
				if (v)
				{
					memcpy(b, &d, 8);
					for (i = 0; i < 8; i++)
						ob_u8(c, *(char *) &one ? b[i] : b[7 - i]);
				}
				return 0;
			}
		case 'n':				/* decimal(38,scale): sign, 16-byte magnitude */
			{
				unsigned	w[4] = {0, 0, 0, 0};
				const char *p = v ? v : "";
				int			neg = 0,
							digits = 0;

				ob_u8(c, 0x6C);
				ob_u8(c, 17);
				ob_u8(c, 38);
				ob_u8(c, (unsigned) scale);
				if (v == NULL)
				{
					ob_u8(c, 0);
					return 0;
				}
				while (*p == ' ')
					p++;
				if (*p == '-' || *p == '+')
					neg = (*p++ == '-');
				for (; *p; p++)
				{
					unsigned long long carry;

					if (*p == '.')
						continue;
					if (*p < '0' || *p > '9' || ++digits > 38)
						return -1;
					carry = (unsigned) (*p - '0');
					for (i = 0; i < 4; i++)
					{
						unsigned long long t = (unsigned long long) w[i] * 10 + carry;

						w[i] = (unsigned) t;
						carry = t >> 32;
					}
				}
				ob_u8(c, 17);
				ob_u8(c, neg ? 0 : 1);
				for (i = 0; i < 4; i++)
					ob_u32(c, w[i]);
				return 0;
			}
		case 'd':				/* datetime: days since 1900, 1/300 s */
		case 'D':				/* date (7.3): days since 0001 */
		case 'T':				/* time(7) */
		case 'S':				/* datetime2(7) */
			{
				long		days = 0;
				unsigned long long t100 = 0;

				if (v && parse_ts(v, &days, &t100) < 0)
					return -1;
				if (code == 'd')
				{
					unsigned long long tk = (t100 * 3 + 50000) / 100000;	/* 1/300 s, rounded */

					ob_u8(c, 0x6F);
					ob_u8(c, 8);
					ob_u8(c, v ? 8 : 0);
					if (v)
					{
						if (tk >= 25920000ULL)
						{
							tk = 0;
							days++;
						}
						ob_u32(c, (unsigned) days);
						ob_u32(c, (unsigned) tk);
					}
					return 0;
				}
				ob_u8(c, code == 'D' ? 0x28 : code == 'T' ? 0x29 : 0x2A);
				if (code != 'D')
					ob_u8(c, 7);	/* scale */
				if (v == NULL)
				{
					ob_u8(c, 0);
					return 0;
				}
				ob_u8(c, code == 'D' ? 3 : code == 'T' ? 5 : 8);
				if (code != 'D')
					for (i = 0; i < 5; i++)
						ob_u8(c, (unsigned) (t100 >> (8 * i)) & 0xFF);
				if (code != 'T')
				{
					long		d1 = days + 693595;

					if (d1 < 0)
						return -1;
					ob_u8(c, (unsigned) d1 & 0xFF);
					ob_u8(c, (unsigned) (d1 >> 8) & 0xFF);
					ob_u8(c, (unsigned) (d1 >> 16) & 0xFF);
				}
				return 0;
			}
		case 'v':				/* varchar(8000): bytes in the server code page */
		case 't':				/* text */
			{
				size_t		l = v ? strlen(v) : 0;
				char	   *a = NULL;
				int			n = 0;

				if (v)
				{
					a = malloc(l * 2 + 8);
					if (a == NULL)
						return -1;
					if (c->ccsid == 1208 || l == 0)
					{
						memcpy(a, v, l);
						n = (int) l;
					}
					else
						n = ile_conv(1208, c->ccsid, v, l, a, l * 2 + 4);
					if (n < 0)
					{
						free(a);
						return -1;
					}
				}
				if (code == 'v')
				{
					ob_u8(c, 0xA7);
					ob_u16(c, 8000);
					ob_put(c, c->coll, 5);
					ob_u16(c, v ? (unsigned) n : 0xFFFF);
				}
				else
				{
					ob_u8(c, 0x23);
					ob_u32(c, (unsigned) n);
					ob_put(c, c->coll, 5);
					ob_u32(c, (unsigned) n);
				}
				if (v)
					ob_put(c, a, (size_t) n);
				free(a);
				return 0;
			}
		default:				/* nvarchar(4000) / ntext */
			c->oblen -= 2;		/* rpc_str writes its own parameter header */
			rpc_str(c, v);
			return 0;
	}
}

/* ------------------------------------- file values (see xdblob.c): sending */

/* the file that parameter value v stands for, or -1 */
static int
lsrc_of(const msc *c, const char *v)
{
	int			k;

	if (v != NULL)
		for (k = 0; k < c->nlsrc; k++)
			if (c->lsrc[k].tok == v)
				return k;
	return -1;
}

/* length of the part of p[0..n) that does not end inside a UTF-8 character */
static size_t
u8_whole(const unsigned char *p, size_t n)
{
	size_t		i = n;
	int			back = 0;

	while (i > 0 && back < 4)
	{
		unsigned	ch = p[i - 1];
		int			need;

		back++;
		if ((ch & 0xC0) == 0x80)
		{
			i--;
			continue;
		}
		need = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
		return need > back ? i - 1 : n;
	}
	return n;
}

/*
 * Reads the file of source k from its start to its end, as the bytes it
 * has as a parameter value: those of the file, or its text as UTF-16.
 * how = 0 only counts them (*total).  how = 1 puts them into the message
 * being built, which goes out packet by packet: just as many as were
 * counted, for their number has been announced.  how = 2 puts them there
 * as the chunks of a (max) value, each with its length.
 */
static int
file_pass(msc *c, int k, int how, long long *total, xbe_err *e)
{
	xlob_src   *src = &c->lsrc[k].src;
	int			clob = c->lsrc[k].kind == XLOB_CLOB;
	long long	want = c->lsrc[k].len,
				done = 0;
	unsigned char pend[4];
	int			npend = 0;

	if (lob_bufs(c) < 0)
	{
		lob_err(e, "57011", "not enough memory for a file value");
		return -1;
	}
	for (;;)
	{
		unsigned char *in = c->fbuf + 8;
		const unsigned char *out = in;
		size_t		m;
		int			n = src->read(src->ctx, (char *) in, LOB_PIECE, e);

		if (n < 0)
			return -1;
		if (n == 0 && npend == 0)
			break;
		m = (size_t) n;
		if (clob)
		{
			unsigned char *st = in - npend;
			size_t		tot = (size_t) npend + (size_t) n,
						whole;

			memcpy(st, pend, (size_t) npend);
			whole = n == 0 ? tot : u8_whole(st, tot);
			npend = (int) (tot - whole);
			memcpy(pend, st + whole, (size_t) npend);
			m = u8_to_u16((const char *) st, whole, c->fbuf2);
			out = c->fbuf2;
		}
		if (how == 1 && done + (long long) m > want)
			m = (size_t) (want - done);		/* the file has grown since it was measured */
		if (how != 0 && m > 0)
		{
			if (how == 2)
				ob_u32(c, (unsigned) m);
			if (ob_put(c, out, m) < 0)
			{
				lob_err(e, "57011", "not enough memory for a file value");
				return -1;
			}
			if (msg_flush(c, PKT_RPC, e) < 0)
				return -1;
		}
		done += (long long) m;
		if (n == 0 || (how == 1 && done >= want))
			break;
	}
	*total = done;
	return 0;
}

/*
 * A file as a parameter value, sent while the file is read.  From TDS 7.2
 * on it is a (max) value: chunks, each with its length, and a zero at the
 * end, so nothing has to be known beforehand.  At TDS 7.1 it is image for
 * bytes and ntext for text, which carry their length in front.  If the
 * sending stops half way the connection is of no use any more: the server
 * waits for the rest of a message that will not come (and to end the value
 * early would store a part of the file as if it were all of it).
 */
static int
rpc_file(msc *c, int k, xbe_err *e)
{
	long long	len = c->lsrc[k].len,
				sent = 0;
	int			clob = c->lsrc[k].kind == XLOB_CLOB;
	int			rc;

	rpc_phdr(c);
	if (c->tds >= 0x72)
	{
		ob_u8(c, clob ? 0xE7 : 0xA5);
		ob_u16(c, 0xFFFF);
		if (clob)
			ob_put(c, c->coll, 5);
		ob_u32(c, 0xFFFFFFFEu); /* length not known */
		ob_u32(c, 0xFFFFFFFFu);
		rc = file_pass(c, k, 2, &sent, e);
		if (rc == 0)
			ob_u32(c, 0);
	}
	else
	{
		ob_u8(c, clob ? 0x63 : 0x22);
		ob_u32(c, (unsigned) len);
		if (clob)
			ob_put(c, c->coll, 5);
		ob_u32(c, (unsigned) len);
		rc = c->lsrc[k].src.rewind(c->lsrc[k].src.ctx, e);
		if (rc == 0)
			rc = file_pass(c, k, 1, &sent, e);
		if (rc == 0 && sent != len)
		{
			lob_err(e, "58030", "a file became shorter while it was sent to the server");
			rc = -1;
		}
	}
	if (rc < 0 && c->obsent)
	{
		xbe_err		keep = *e;

		c->dead = 1;
		c->obsent = 0;
		*e = keep;
	}
	return rc;
}

/*
 * Appends the parameter definitions ("@P1 int,@P2 varchar(8000)") and the
 * values of a parameterized statement to the RPC being built; each value
 * goes in the type of its declaration.  describe = 1 declares everything
 * nvarchar(4000) and passes NULLs.
 */
static int
rpc_params(msc *c, int np, char **pv, const int *pk, int describe, xbe_err *e)
{
	char	   *defs = malloc((size_t) np * 32 + 8);
	char	   *codes = malloc((size_t) np + 1);
	int		   *scales = calloc((size_t) np + 1, sizeof(int));
	char	   *o = defs;
	int			i,
				rc = 0;

	if (defs == NULL || codes == NULL || scales == NULL)
	{
		free(defs);
		free(codes);
		free(scales);
		return -1;
	}
	defs[0] = '\0';
	for (i = 0; i < np; i++)
	{
		int			kind = (pk && !describe) ? pk[i] : XK_VARCHAR;
		const char *v = describe ? NULL : pv[i];
		const char *type = "nvarchar(4000)";
		char		tb[40];
		int			new_types = c->dt2 && c->tds >= 0x73;
		int			fk = describe ? -1 : lsrc_of(c, pv[i]);

		codes[i] = 'u';
		if (fk >= 0)
		{
			/* a file: see rpc_file */
			type = c->tds >= 0x72 ? (c->lsrc[fk].kind == XLOB_CLOB ? "nvarchar(max)" : "varbinary(max)")
				: (c->lsrc[fk].kind == XLOB_CLOB ? "ntext" : "image");
			codes[i] = 'F';
		}
		else if (!describe)
			switch (kind)
			{
				case XK_SMALLINT:
				case XK_INTEGER:
				case XK_BOOL:
					type = "int";
					codes[i] = 'i';
					break;
				case XK_BIGINT:
					type = "bigint";
					codes[i] = 'l';
					break;
				case XK_DOUBLE:
					type = "float";
					codes[i] = 'f';
					break;
				case XK_DECIMAL:
					{
						const char *dot = v ? strchr(v, '.') : NULL;

						scales[i] = dot ? (int) strlen(dot + 1) : 0;
						if (scales[i] > 38)
							scales[i] = 38;
						snprintf(tb, sizeof tb, "decimal(38,%d)", scales[i]);
						type = tb;
						codes[i] = 'n';
						break;
					}
				case XK_DATE:
					type = new_types ? "date" : "datetime";
					codes[i] = new_types ? 'D' : 'd';
					break;
				case XK_TIMESTAMP:
					type = new_types ? "datetime2" : "datetime";
					codes[i] = new_types ? 'S' : 'd';
					break;
				case XK_TIME:
					type = new_types ? "time" : "varchar(8000)";
					codes[i] = new_types ? 'T' : 'v';
					break;
				default:
					{
						size_t		l = v ? strlen(v) : 0;
						int			wide = c->strmode == 2 ||
							(c->strmode == 0 && v && !fits_codepage(c, v));

						if (wide)
							type = l > 4000 ? "ntext" : "nvarchar(4000)";
						else
						{
							type = l > 8000 ? "text" : "varchar(8000)";
							codes[i] = l > 8000 ? 't' : 'v';
						}
						break;
					}
			}
		o += sprintf(o, "%s@P%d %s", i ? "," : "", i + 1, type);
	}
	rpc_str(c, defs);
	for (i = 0; i < np && rc == 0; i++)
		if (codes[i] == 'F')
			rc = rpc_file(c, lsrc_of(c, pv[i]), e);
		else if (rpc_typed(c, codes[i], describe ? NULL : pv[i], scales[i]) < 0)
		{
			xnet_err(e, "22018", "parameter %d: \"%.60s\" is not a valid value for its type", i + 1,
					 pv[i] ? pv[i] : "");
			rc = -1;
		}
	free(scales);
	free(codes);
	free(defs);
	return rc;
}

/* a failed statement may have cost the transaction: keep one open */
static void
check_txn(msc *c, xbe_err *e)
{
	xbe_batch	b;
	xbe_err		e2;
	int			gone = 0;

	if (!c->in_txn || c->dead)
		return;
	memset(&b, 0, sizeof b);
	if (c->tds >= 0x72 && c->txd_seen)
	{
		/* the server reports the end of a transaction itself */
		static const unsigned char zero[8];

		gone = memcmp(c->txd, zero, 8) == 0;
	}
	else
	{
		xbe_err		keep = c->err;
		int			had = c->have_err;

		if (send_batch(c, "SELECT @@TRANCOUNT", &e2) == 0 && reply(c, &b, &e2) == 0 &&
			b.nrows == 1 && b.val[0] && atoi(b.val[0]) == 0)
			gone = 1;
		batch_release(&b);
		c->err = keep;
		c->have_err = had;
	}
	if (gone)
	{
		batch(c, "BEGIN TRANSACTION", &e2);
		e->rolled_back = 1;
	}
}

/* ---------------------------------------------------------------- connect */

static void
ms_disconnect(void *vc)
{
	msc		   *c = vc;

	if (c->tls)
		xtls_end(c->tls);
	if (c->fd >= 0)
		close(c->fd);
	free(c->hsout);
	free(c->hsin);
	free(c->ob);
	free(c->pk);
	free(c->in);
	free(c->sb);
	free(c->fbuf);
	free(c->fbuf2);
	free(c->cols);
	free(c->cols1);
	free(c->cols0);
	free(c);
}

static int
prelogin(msc *c, xbe_err *e)
{
	static const unsigned char body[] = {
		0x00, 0x00, 0x15, 0x00, 0x06,	/* VERSION    at 21, 6 bytes */
		0x01, 0x00, 0x1B, 0x00, 0x01,	/* ENCRYPTION at 27, 1 byte */
		0x02, 0x00, 0x1C, 0x00, 0x01,	/* INSTOPT    at 28, 1 byte */
		0x03, 0x00, 0x1D, 0x00, 0x04,	/* THREADID   at 29, 4 bytes */
		0xFF,
		0x08, 0x00, 0x01, 0x55, 0x00, 0x00, /* client version 8.0.341 */
		0x02,					/* encryption: set below */
		0x00,					/* default instance */
		0x00, 0x00, 0x00, 0x00
	};
	unsigned char *p;
	size_t		n,
				i;
	unsigned	sv = 0x02;

	c->oblen = 0;
	ob_put(c, body, sizeof body);
	/* 0 = encrypt the login if the server can, 1 = everything, 2 = cannot */
	c->ob[27] = (unsigned char) (c->enc == 0 ? 2 : c->enc == 1 ? 0 : 1);
	if (msg_send(c, PKT_PRELOGIN, e) < 0)
		return -1;
	if (pk_next(c) < 0)
	{
		dead(c, e, "no answer to the pre-login request");
		return -1;
	}
	p = c->pk;
	n = c->pklen;
	c->pkpos = c->pklen;
	for (i = 0; i + 4 < n && p[i] != 0xFF; i += 5)
		if (p[i] == 0x01)
		{
			size_t		off = ((size_t) p[i + 1] << 8) | p[i + 2];

			if (off < n)
				sv = p[off];
		}
	while (!c->last)
		if (pk_next(c) < 0)
			break;
	if (c->enc == 3)
		return 0;				/* strict: already inside TLS */
	if (c->enc == 0 && (sv == 0x01 || sv == 0x03))
	{
#ifdef __ILEC400__
		xnet_err(e, "08001", "the SQL Server requires encryption: possible from IBM i only with encrypt=strict (SQL Server 2022 or later)");
#else
		xnet_err(e, "08001", "the SQL Server requires encryption and encrypt=off is set");
#endif
		return -1;
	}
	if (c->enc == 2 && sv == 0x02)
	{
		xnet_err(e, "08001", "encrypt=on is set but the SQL Server does not offer encryption");
		return -1;
	}
	if (c->enc == 0 || sv == 0x02)
		return 0;
	c->tls_login = (sv == 0x00);
	return tls_begin(c, 1, NULL, e);
}

static int
login(msc *c, const char *host, const char *user, const char *pw, const char *db,
	  const char *app, xbe_err *e)
{
	const char *str[9];
	size_t		off[9],
				len[9];
	size_t		base = c->tds_req >= 0x72 ? 94 : 86,
				at,
				i,
				pwat = 0,
				pwlen = 0;

	str[0] = "XDB";				/* client host name */
	str[1] = user;
	str[2] = pw;
	str[3] = app;
	str[4] = host;				/* server name */
	str[5] = "";				/* unused */
	str[6] = "XDB";				/* client library */
	str[7] = "";				/* language: server default */
	str[8] = db;
	c->oblen = 0;
	ob_put(c, NULL, base);
	for (i = 0; i < 9; i++)
	{
		off[i] = c->oblen;
		len[i] = ob_ucs2(c, str[i]) / 2;
		if (i == 2)
		{
			pwat = off[i];
			pwlen = len[i] * 2;
		}
	}
	/* the password is not sent in clear: nibbles swapped, xor A5 */
	for (i = 0; i < pwlen; i++)
	{
		unsigned char b = c->ob[pwat + i];

		c->ob[pwat + i] = (unsigned char) (((b << 4) | (b >> 4)) ^ 0xA5);
	}
	{
		unsigned char *h = c->ob;
		size_t		total = c->oblen;
		unsigned	v;

#define P32(o, x) do { v = (unsigned) (x); h[o] = (unsigned char) v; h[(o) + 1] = (unsigned char) (v >> 8); \
		h[(o) + 2] = (unsigned char) (v >> 16); h[(o) + 3] = (unsigned char) (v >> 24); } while (0)
#define P16(o, x) do { v = (unsigned) (x); h[o] = (unsigned char) v; h[(o) + 1] = (unsigned char) (v >> 8); } while (0)
		P32(0, total);
		P32(4, c->tds_req >= 0x80 ? 0x08000000u : c->tds_req >= 0x72 ? 0x74000004u : TDS_VERSION);
		P32(8, 4096);			/* packet size */
		P32(12, 0x00000100);	/* client program version */
		P32(16, (unsigned) getpid());
		P32(20, 0);
		h[24] = 0xE0;			/* USE_DB, database is fatal, SET_LANG */
		h[25] = 0x03;			/* language is fatal, ODBC settings (ANSI defaults) */
		h[26] = 0;
		h[27] = 0;
		P32(28, 0);
		P32(32, 0);
		at = 36;
		for (i = 0; i < 9; i++)
		{
			P16(at, off[i]);
			P16(at + 2, len[i]);
			at += 4;
		}
		memset(h + at, 0, 6);	/* client id */
		at += 6;
		P16(at, total);			/* SSPI */
		P16(at + 2, 0);
		P16(at + 4, total);		/* attached database file */
		P16(at + 6, 0);
		if (base == 94)
		{
			P16(at + 8, total); /* new password */
			P16(at + 10, 0);
			P32(at + 12, 0);	/* long SSPI length */
		}
#undef P32
#undef P16
	}
	if (msg_send(c, PKT_LOGIN, e) < 0)
	{
		memset(c->ob, 0, c->oblen);
		return -1;
	}
	memset(c->ob, 0, c->oblen);
	if (c->tls_login)
		c->tls_on = 0;			/* only the login packet was to be encrypted */
	if (reply(c, NULL, e) < 0)
		return -1;
	if (c->version == 0)
	{
		xnet_err(e, "08001", "SQL Server did not acknowledge the login");
		return -1;
	}
	return 0;
}

/*
 * conninfo keys: host port (1433) dbname connect_timeout socket_buffer passfile appname
 *                charset=<CCSID of varchar data, default from the collation>
 *                string_params=auto|varchar|nvarchar (default auto)
 */
static void *
ms_connect(const xbe_opts *o, xbe_err *e)
{
	char		host[256],
				port[16],
				db[128],
				app[64],
				pwf[256],
				pw[256],
				t[32],
				init[512];
	msc		   *c = calloc(1, sizeof(msc));
	int			timeout;

	memset(e, 0, sizeof *e);
	if (c == NULL)
		return NULL;
	if (xnet_kv(o->conninfo, "host", host, sizeof host) == NULL)
		snprintf(host, sizeof host, "127.0.0.1");
	if (xnet_kv(o->conninfo, "port", port, sizeof port) == NULL)
		snprintf(port, sizeof port, "1433");
	xnet_kv(o->conninfo, "dbname", db, sizeof db);
	if (xnet_kv(o->conninfo, "appname", app, sizeof app) == NULL)
		snprintf(app, sizeof app, "XDB for IBM i");
	timeout = xnet_kv(o->conninfo, "connect_timeout", t, sizeof t) ? atoi(t) : 15;
	if (xnet_kv(o->conninfo, "charset", t, sizeof t))
		c->ccsid_force = atoi(t);
	if (xnet_kv(o->conninfo, "string_params", t, sizeof t))
		c->strmode = strcmp(t, "varchar") == 0 ? 1 : strcmp(t, "nvarchar") == 0 ? 2 : 0;
	if (xnet_kv(o->conninfo, "datetime2", t, sizeof t))
		c->dt2 = strcmp(t, "on") == 0;
	/* tds=7.1 keeps to the SQL Server 2000 level; default: 7.4, the server lowers it */
	c->tds_req = (xnet_kv(o->conninfo, "tds", t, sizeof t) && strcmp(t, "7.1") == 0) ? 0x71 : 0x74;
	/* encrypt=off|prefer|on|strict (default prefer), tls_verify=on checks the certificate */
	c->enc = 1;
	if (xnet_kv(o->conninfo, "encrypt", t, sizeof t))
		c->enc = strcmp(t, "off") == 0 ? 0 : strcmp(t, "on") == 0 ? 2 : strcmp(t, "strict") == 0 ? 3 : 1;
	if (c->enc == 3)
		c->tds_req = 0x80;
#ifdef __ILEC400__
	/*
	 * TDS 7.x wraps the TLS handshake in its own packets, which GSKit cannot
	 * do: on IBM i only encrypt=strict (TDS 8.0, SQL Server 2022 or later)
	 * gives an encrypted connection.
	 */
	if (c->enc == 2)
	{
		xnet_err(e, "08001", "encrypt=on is not possible on IBM i (GSKit); use encrypt=strict with SQL Server 2022 or later");
		free(c);
		return NULL;
	}
	if (c->enc == 1)
		c->enc = 0;
#endif
	c->verify = xnet_kv(o->conninfo, "tls_verify", t, sizeof t) && strcmp(t, "on") == 0;
	snprintf(c->host, sizeof c->host, "%s", host);
	c->ccsid = c->ccsid_force ? c->ccsid_force : 1252;
	snprintf(pw, sizeof pw, "%s", o->password ? o->password : "");
	if (pw[0] == '\0' && xnet_kv(o->conninfo, "passfile", pwf, sizeof pwf))
		xnet_passfile(pwf, host, port, db, o->user, pw, sizeof pw);
	for (;;)
	{
		int			ok;

		c->psize = 4096;
		c->tds = 0;
		c->version = 0;
		c->dead = 0;
		c->inlen = c->inpos = 0;
		c->fd = xnet_connect(host, port, timeout, xnet_sockbuf_opt(o->conninfo), e);
		if (c->fd < 0)
		{
			memset(pw, 0, sizeof pw);
			ms_disconnect(c);
			return NULL;
		}
		c->tls_failed = c->tls_login = 0;
		ok = (c->enc != 3 || tls_begin(c, 0, "tds/8.0", e) == 0) &&
			prelogin(c, e) == 0 && login(c, host, o->user ? o->user : "", pw, db, app, e) == 0;
		if (ok)
			break;
		if (c->tls)
			xtls_end(c->tls);
		c->tls = NULL;
		c->tls_on = 0;
		/* encrypt=prefer and no working TLS in common: go on without */
		if (c->tls_failed && c->enc == 1)
		{
			close(c->fd);
			c->fd = -1;
			c->enc = 0;
			continue;
		}
		/*
		 * A server that drops a 7.4 login without a word may be one that
		 * only takes the older login record: try once more at 7.1.
		 */
		if (c->tds_req >= 0x72 && e->native == 0 && strcmp(e->sqlstate, "08006") == 0)
		{
			close(c->fd);
			c->fd = -1;
			c->tds_req = 0x71;
			continue;
		}
		memset(pw, 0, sizeof pw);
		ms_disconnect(c);
		return NULL;
	}
	memset(pw, 0, sizeof pw);
	if (c->tds == 0)
		c->tds = 0x71;
	/* long values are cut by the driver anyway (text_len) */
	if (batch(c, "SET TEXTSIZE 64000", e) < 0)
	{
		ms_disconnect(c);
		return NULL;
	}
	if (o->conf && o->conf(o->rdb, "init_sql", init, sizeof init) == 0 && init[0] &&
		batch(c, init, e) < 0)
	{
		ms_disconnect(c);
		return NULL;
	}
	return c;
}

static int
ms_version(void *vc)
{
	return ((msc *) vc)->version;
}

/* ------------------------------------------------------------- statements */

static int
kw(const char *sql, const char *word)
{
	size_t		i,
				l = strlen(word);

	/* leading blanks, parentheses and comments do not count */
	for (;;)
	{
		while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r' || *sql == '(')
			sql++;
		if (sql[0] == '/' && sql[1] == '*')
		{
			const char *e = strstr(sql + 2, "*/");

			if (e == NULL)
				return 0;
			sql = e + 2;
		}
		else if (sql[0] == '-' && sql[1] == '-')
		{
			while (*sql && *sql != '\n')
				sql++;
		}
		else
			break;
	}
	for (i = 0; i < l; i++)
		if ((sql[i] & ~0x20) != word[i])
			return 0;
	return !((sql[l] >= 'A' && sql[l] <= 'Z') || (sql[l] >= 'a' && sql[l] <= 'z') || sql[l] == '_');
}

static int
ms_exec(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
		long long *rows, xbe_err *e)
{
	msc		   *c = vc;
	int			rc;

	if (rows)
		*rows = 0;
	if (np == 0)
		rc = send_batch(c, sql, e);
	else
	{
		rpc_begin(c, "sp_executesql");
		rpc_str(c, sql);
		if (rpc_params(c, np, pv, pk, 0, e) < 0)
			return -1;
		rc = msg_send(c, PKT_RPC, e);
	}
	if (rc == 0)
		rc = reply(c, NULL, e);
	if (rc < 0)
	{
		check_txn(c, e);
		return -1;
	}
	if (rows && c->count > 0)
		*rows = c->count;
	return 0;
}

/* column descriptions of the reply just read */
static int
cols_out(msc *c, xbe_col **cols, int *ncols)
{
	int			i;

	*ncols = c->ncols;
	if (c->ncols == 0)
		return 0;
	*cols = calloc((size_t) c->ncols, sizeof(xbe_col));
	if (*cols == NULL)
		return -1;
	for (i = 0; i < c->ncols; i++)
		col_kind(&c->cols[i], &(*cols)[i]);
	return 0;
}

static int
describe1(msc *c, const char *sql, xbe_col **cols, int *ncols, xbe_err *e)
{
	int			np,
				rc,
				handle,
				is_exec = kw(sql, "EXEC") || kw(sql, "EXECUTE");
	xbe_err		e2;

	*cols = NULL;
	*ncols = 0;
	/* only statements that can return rows are worth a round trip */
	if (!(kw(sql, "SELECT") || kw(sql, "WITH") || is_exec))
		return 0;
	np = count_markers(sql);
	rpc_begin(c, "sp_prepare");
	rpc_int(c, 0, 1, 1);		/* @handle OUTPUT */
	if (np > 0)
	{
		char	   *defs = malloc((size_t) np * 24 + 8);
		char	   *o = defs;
		int			i;

		if (defs == NULL)
			return -1;
		for (i = 0; i < np; i++)
			o += sprintf(o, "%s@P%d nvarchar(4000)", i ? "," : "", i + 1);
		rpc_str(c, defs);
		free(defs);
	}
	else
		rpc_str(c, NULL);
	rpc_str(c, sql);
	rpc_int(c, 1, 0, 0);		/* @options: return the column metadata */
	if (msg_send(c, PKT_RPC, e) < 0)
		return -1;
	rc = reply(c, NULL, e);
	handle = c->nrv > 0 ? c->rv[0] : 0;
	if (rc == 0 && cols_out(c, cols, ncols) < 0)
		return -1;
	if (handle != 0 && !c->dead)
	{
		xbe_col    *keep = *cols;
		int			nkeep = *ncols;

		rpc_begin(c, "sp_unprepare");
		rpc_int(c, handle, 0, 0);
		if (msg_send(c, PKT_RPC, &e2) == 0)
			reply(c, NULL, &e2);
		*cols = keep;
		*ncols = nkeep;
	}
	if ((rc < 0 || *ncols == 0) && !c->dead)
	{
		/*
		 * Second way to the columns, the one of the older client libraries:
		 * the statement under SET FMTONLY ON, which returns the result set
		 * descriptions and executes nothing.  FMTONLY is switched off in a
		 * request of its own, so that a failure cannot leave it on.
		 */
		size_t		l = strlen(sql) + (size_t) np * 32 + 96;
		char	   *q = malloc(l);
		char	   *o = q;
		int			i,
					rc2;

		if (q == NULL)
			return -1;
		for (i = 0; i < np; i++)
			o += sprintf(o, "%s@P%d nvarchar(4000)", i ? "," : "DECLARE ", i + 1);
		sprintf(o, " SET FMTONLY ON %s", sql);
		free(*cols);
		*cols = NULL;
		*ncols = 0;
		rc2 = send_batch(c, q, &e2);
		free(q);
		if (rc2 == 0)
		{
			c->first_meta = 1;
			rc2 = reply(c, NULL, &e2);
			if (rc2 == 0 && cols_out(c, cols, ncols) < 0)
				return -1;
		}
		if (!c->dead)
		{
			xbe_col    *keep = *cols;
			int			nkeep = *ncols;

			batch(c, "SET FMTONLY OFF", &e2);
			*cols = keep;
			*ncols = nkeep;
		}
		if (rc2 == 0 && *ncols > 0)
			rc = 0;
	}
	if (rc < 0)
	{
		check_txn(c, e);
		if (is_exec && !c->dead && !e->rolled_back)
		{
			/* a procedure that cannot be described can still be executed */
			memset(e, 0, sizeof *e);
			return 0;
		}
		return -1;
	}
	return 0;
}

/*
 * SELECT ... FOR XML and FOR JSON return their text in rows of one column
 * with a fixed name, the word XML or JSON and a GUID that never changes.
 * The server opens no cursor on such a statement - and its refusal ends
 * the transaction (seen on SQL Server 2019), so the driver does not ask:
 * the statement is run as it is and its rows are kept here.
 */
static int
xml_stream(const char *name)
{
	static const char guid[] = "_F52E2B61-18A1-11d1-B105-00805F49916B";
	const char *p = strncmp(name, "XML", 3) == 0 ? name + 3 : strncmp(name, "JSON", 4) == 0 ? name + 4 : NULL;

	return p != NULL && strncmp(p, guid, sizeof guid - 1) == 0;
}

static int
ms_describe(void *vc, const char *sql, xbe_col **cols, int *ncols, void **stmt, xbe_err *e)
{
	msc		   *c = vc;
	msstmt	   *st = calloc(1, sizeof(msstmt));
	int			rc;

	*stmt = NULL;
	if (st != NULL && c->nsink > 0)
	{
		/* the file columns named for this statement (and for no other): open() needs them */
		st->nsink = c->nsink;
		memcpy(st->sink, c->sink, sizeof(int) * (size_t) c->nsink);
	}
	c->nsink = 0;
	if (st == NULL)
		return -1;
	rc = describe1(c, sql, cols, ncols, e);
	if (rc < 0 || *ncols == 0)
	{
		free(st);
		st = NULL;
	}
	else
	{
		st->ndesc = *ncols;		/* open() tells the row status column of a cursor by it */
		st->nocursor = *ncols == 1 && xml_stream((*cols)[0].name);
	}
	*stmt = st;
	return rc;
}

static void
ms_stmt_free(void *vc, void *stmt)
{
	free(stmt);
}

/* rows with file columns must come whole; other long values are cut by the server */
static void
bigtext(msc *c, int on)
{
	xbe_err		e;
	xbe_err		keep = c->err;
	int			had = c->have_err;

	if (on ? c->bigtext++ == 0 : (c->bigtext > 0 && --c->bigtext == 0))
		if (!c->dead)
			batch(c, on ? "SET TEXTSIZE 2147483647" : "SET TEXTSIZE 64000", &e);
	c->err = keep;
	c->have_err = had;
}

static mcol *
cols_dup(const mcol *src, int n)
{
	mcol	   *d = n > 0 ? malloc(sizeof(mcol) * (size_t) n) : NULL;

	if (d)
		memcpy(d, src, sizeof(mcol) * (size_t) n);
	return d;
}

/*
 * The rows of an API server cursor have one column more than the statement:
 * the server adds the status of each row as a last column, named ROWSTAT
 * and of type INT4, and announces it with the cursor and with every fetch
 * ("sent as hidden columns ... at the end of each row", sp_cursorfetch in
 * the documentation of the server).  It is no column of the statement and
 * must not be counted as one.  Returns 1 when the last of the n columns is
 * that one.
 *
 * Not every server adds it (Babelfish does not).  So when the describe has
 * told how many columns the statement has, the cursor must have exactly one
 * more: a last column of that name in a cursor of as many columns as were
 * described is the statement's own.
 */
static int
rowstat_col(const mcol *cols, int n, int ndesc)
{
	static const char name[] = "ROWSTAT";
	const mcol *m;
	int			i;

	if (cols == NULL || n < 2)
		return 0;
	if (ndesc > 0 && n != ndesc + 1)
		return 0;
	m = &cols[n - 1];
	if (!(m->type == 0x38 || (m->type == 0x26 && m->maxlen == 4)))
		return 0;				/* INT4, or INTN of four bytes */
	for (i = 0; name[i]; i++)
	{
		char		ch = m->name[i];

		if (ch >= 'a' && ch <= 'z')
			ch = (char) (ch - ('a' - 'A'));
		if (ch != name[i])
			return 0;
	}
	return m->name[i] == 0;
}

static int
ms_open(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
		int hold, void **cur, xbe_err *e)
{
	msc		   *c = vc;
	mscur	   *k = calloc(1, sizeof(mscur));
	const msstmt *st = stmt;
	int			rc = 0;

	if (k == NULL)
		return -1;
	if (st && st->nsink > 0)
	{
		k->nsink = st->nsink;
		memcpy(k->sink, st->sink, sizeof(int) * (size_t) st->nsink);
		bigtext(c, 1);
	}
	/* procedure calls are not opened as server cursors: run once, rows kept here */
	if (kw(sql, "EXEC") || kw(sql, "EXECUTE") || (st && st->nocursor))
		goto direct;
	rpc_begin(c, "sp_cursoropen");
	rpc_int(c, 0, 1, 1);		/* @cursor OUTPUT */
	rpc_str(c, sql);
	rpc_int(c, 0x0010 | (np > 0 ? 0x1000 : 0), 0, 1);	/* fast forward [, parameterized] */
	rpc_int(c, 0x0001, 0, 1);	/* read only */
	rpc_int(c, 0, 0, 1);		/* @rowcount OUTPUT */
	if (np > 0 && rpc_params(c, np, pv, pk, 0, e) < 0)
	{
		if (k->nsink > 0)
			bigtext(c, 0);
		free(k);
		return -1;
	}
	rc = msg_send(c, PKT_RPC, e);
	if (rc == 0)
		rc = reply(c, NULL, e);
	if (rc == 0 && c->nrv > 0 && c->rv[0] != 0)
	{
		k->handle = c->rv[0];
		k->cols = cols_dup(c->cols, c->ncols);
		k->ncols = c->ncols;
		k->nvis = k->ncols - rowstat_col(c->cols, c->ncols, st ? st->ndesc : 0);
		*cur = k;
		return 0;
	}
	if (rc < 0)
	{
		/*
		 * The errors 16900 to 16999 are those of the cursor procedures: the
		 * server opens no cursor on this statement.  6819 says the same of a
		 * FOR XML clause that the describe did not show, 8654 of a statement
		 * with a text pointer.  (jTDS re-executes after the same errors.)
		 * Any other error is the statement's own.
		 */
		int			refused = !c->dead && ((e->native >= 16900 && e->native <= 16999) ||
										   e->native == 6819 || e->native == 8654);

		/*
		 * A refusal can end the transaction as an error of the statement
		 * can (6819 does on SQL Server 2019): then nothing is run again,
		 * the program is told that its unit of work was rolled back.
		 */
		check_txn(c, e);
		if (!refused || e->rolled_back)
		{
			if (k->nsink > 0)
				bigtext(c, 0);
			free(k);
			return -1;
		}
	}
	/* no server cursor for this statement: run it and keep the rows here */
direct:
	if (k->nsink > 0)
	{
		/* a file can only be written while its row is read, one row at a time */
		char		why[300];

		snprintf(why, sizeof why, "%s", rc < 0 ? e->msg : "");
		bigtext(c, 0);
		free(k);
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "0A000");
		snprintf(e->msg, sizeof e->msg, "PUT_BLOB_TO_FILE and PUT_CLOB_TO_FILE need a query "
				 "that the server opens as a cursor%s%s", why[0] ? ": " : "", why);
		return -1;
	}
	if (np == 0)
		rc = send_batch(c, sql, e);
	else
	{
		rpc_begin(c, "sp_executesql");
		rpc_str(c, sql);
		if (rpc_params(c, np, pv, pk, 0, e) < 0)
		{
			free(k);
			return -1;
		}
		rc = msg_send(c, PKT_RPC, e);
	}
	if (rc == 0)
		rc = reply(c, &k->inl, e);
	if (rc < 0)
	{
		batch_release(&k->inl);
		free(k);
		check_txn(c, e);
		return -1;
	}
	/* the columns the rows really have: ms_cursor_cols tells them */
	k->cols = c->cols0;
	k->ncols = c->ncols0;
	k->nvis = k->ncols;
	c->cols0 = NULL;
	c->ncols0 = 0;
	*cur = k;
	return 0;
}

/*
 * The columns of the rows of an open cursor, as the server sent them with
 * the cursor or with the rows - without the row status column of a server
 * cursor (rowstat_col).  The describe has its columns from
 * sp_prepare or SET FMTONLY, which run nothing: a procedure they cannot
 * follow (one that works with temporary tables, for one) is described with
 * no columns, and a procedure with more than one way through it can return
 * other columns than its first SELECT has.
 */
static int
ms_cursor_cols(void *vc, void *cur, xbe_col **cols, int *ncols, xbe_err *e)
{
	mscur	   *k = cur;
	int			i;

	*cols = NULL;
	*ncols = 0;
	if (k == NULL || k->cols == NULL || k->nvis <= 0)
		return 0;
	*cols = calloc((size_t) k->nvis, sizeof(xbe_col));
	if (*cols == NULL)
	{
		xnet_err(e, "57011", "not enough memory for the columns of the statement");
		return -1;
	}
	for (i = 0; i < k->nvis; i++)
		col_kind(&k->cols[i], &(*cols)[i]);
	*ncols = k->nvis;
	return 0;
}

static int
ms_fetch(void *vc, void *cur, int maxrows, xbe_batch *b, xbe_err *e)
{
	msc		   *c = vc;
	mscur	   *k = cur;

	memset(b, 0, sizeof *b);
	b->release = batch_release;
	if (k->handle == 0)
	{
		if (!k->inl_used)
		{
			*b = k->inl;
			b->release = batch_release;
			memset(&k->inl, 0, sizeof k->inl);
			k->inl_used = 1;
		}
		b->eof = 1;
		return 0;
	}
	rpc_begin(c, "sp_cursorfetch");
	rpc_int(c, k->handle, 0, 0);
	rpc_int(c, 2, 0, 0);		/* NEXT */
	rpc_int(c, 0, 0, 0);
	rpc_int(c, maxrows, 0, 0);
	if (msg_send(c, PKT_RPC, e) < 0)
		return -1;
	/* rows are decoded with the cursor's columns unless the reply describes them again */
	free(c->cols);
	c->cols = cols_dup(k->cols, k->ncols);
	c->ncols = c->cols ? k->ncols : 0;
	c->keep_meta = 1;
	c->rsink = k->sink;
	c->nrsink = k->nsink;
	c->fbad = 0;
	if (reply(c, b, e) < 0)
	{
		c->nrsink = 0;
		batch_release(b);
		/*
		 * An error that ends the batch (a value that cannot be converted,
		 * for one) also rolls the transaction back on the server.  Seen
		 * here or never: the next statement would run outside of any
		 * transaction and stay whatever the program does afterwards.
		 */
		check_txn(c, e);
		return -1;
	}
	c->nrsink = 0;
	if (c->fbad)
	{
		/* the row arrived, a file of it could not be written */
		batch_release(b);
		*e = c->ferr;
		return -1;
	}
	b->eof = b->nrows < maxrows;
	return 0;
}

static void
ms_close(void *vc, void *cur)
{
	msc		   *c = vc;
	mscur	   *k = cur;
	xbe_err		e;

	if (k->handle != 0 && !c->dead)
	{
		rpc_begin(c, "sp_cursorclose");
		rpc_int(c, k->handle, 0, 0);
		if (msg_send(c, PKT_RPC, &e) == 0)
			reply(c, NULL, &e);
	}
	if (k->nsink > 0)
		bigtext(c, 0);
	if (k->inl.val)
		batch_release(&k->inl);
	free(k->cols);
	free(k);
}

static int
ms_txn(void *vc, int op, xbe_err *e)
{
	msc		   *c = vc;

	switch (op)
	{
		case XT_BEGIN:
			c->in_txn = 1;
			return batch(c, "BEGIN TRANSACTION", e);
		case XT_COMMIT:
			c->in_txn = 0;
			return batch(c, "IF @@TRANCOUNT > 0 COMMIT TRANSACTION", e);
		case XT_ROLLBACK:
			c->in_txn = 0;
			return batch(c, "IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION", e);
		default:
			return 0;			/* a failed statement does not end the transaction */
	}
}

/* a one row batch on an idle connection */
static int
ms_ping(void *vc, int timeout)
{
	msc		   *c = vc;
	xbe_batch	b;
	xbe_err		e;
	xbe_err		keep = c->err;
	int			had = c->have_err;
	int			ok;

	if (c->dead || c->fd < 0)
		return -1;
	if (xnet_dead(c->fd, !c->tls_on))
	{
		c->dead = 1;
		return -1;
	}
	memset(&b, 0, sizeof b);
	memset(&e, 0, sizeof e);
	if (send_batch(c, "SELECT 1", &e) < 0 || xnet_wait_readable(c->fd, timeout) <= 0)
	{
		c->dead = 1;
		return -1;
	}
	ok = reply(c, &b, &e) == 0 && b.nrows == 1;
	batch_release(&b);
	c->err = keep;
	c->have_err = had;
	if (!ok)
	{
		c->dead = 1;
		return -1;
	}
	return 0;
}

/* ----------------------------------------------- files (see xdblob.c) */

/*
 * GET_xLOB_FROM_FILE: a parameter marker takes the place of the function
 * and the file is its value, sent while it is read (rpc_file).  At TDS 7.1
 * that value is an image or ntext, good for the columns of INSERT and
 * UPDATE and little else; a server that knows the (max) types (2005 and
 * later, reached at 7.1 because tds=7.1 was asked for) is told to make one
 * of it, which can stand wherever a value can.
 */
static int
ms_lob_in_sql(void *vc, int kind, char *out, int outsize)
{
	msc		   *c = vc;

	if (c && c->tds < 0x72 && c->version >= 900)
		snprintf(out, (size_t) outsize, kind == XLOB_CLOB ? "CAST(? AS nvarchar(max))" : "CAST(? AS varbinary(max))");
	else
		snprintf(out, (size_t) outsize, "?");
	return 1;
}

/*
 * PUT_xLOB_TO_FILE: one binary value that says where it goes before the
 * bytes come (read_sink): B or C, the name, a zero, the value.  That needs
 * a binary type without a limit, which SQL Server has from 2005 on.
 */
static int
ms_lob_out_sql(void *vc, int kind, int stage, char *pre, char *mid, char *post, xbe_err *e)
{
	msc		   *c = vc;

	if (c && c->version < 900)
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "0A000");
		snprintf(e->msg, sizeof e->msg, "\"%s\" needs SQL Server 2005 or later (this is version %d.%d)",
				 kind == XLOB_CLOB ? "PUT_CLOB_TO_FILE" : "PUT_BLOB_TO_FILE", c->version / 100, c->version % 100);
		return -1;
	}
	snprintf(pre, XLOB_SQLPRE, "(%s + CAST(ISNULL(CAST((", kind == XLOB_CLOB ? "0x4300" : "0x4200");
	snprintf(mid, XLOB_SQLMID, ") AS nvarchar(%d)), N'') AS varbinary(%d)) + 0x0000 + CAST(%s(",
			 LOB_NAMEUNITS, 2 * LOB_NAMEUNITS, kind == XLOB_CLOB ? "CAST(" : "");
	snprintf(post, XLOB_SQLMID, "%s) AS varbinary(max)))", kind == XLOB_CLOB ? ") AS nvarchar(max)" : "");
	return 1;
}

static void
ms_lob_sinks(void *vc, const int *cols, int n)
{
	msc		   *c = vc;

	c->nsink = n > LOB_MAXSINK ? LOB_MAXSINK : n;
	memcpy(c->sink, cols, sizeof(int) * (size_t) c->nsink);
}

/*
 * The file is not sent here.  At TDS 7.1 its length has to be known when
 * the statement goes out: for text that means reading it once to count.
 */
static int
ms_lob_in(void *vc, int kind, xlob_src *src, char **slot, xbe_err *e)
{
	msc		   *c = vc;
	char	   *tok;
	long long	len;
	int			k = c->nlsrc;

	slot[0] = NULL;
	if (k >= LOB_MAXSRC)
	{
		lob_err(e, "54023", "too many files in one statement");
		return -1;
	}
	c->lsrc[k].src = *src;
	c->lsrc[k].kind = kind;
	c->lsrc[k].tok = NULL;
	c->lsrc[k].len = 0;
	len = 0;
	if (c->tds < 0x72)
	{
		/* image and ntext values begin with their length */
		len = kind == XLOB_BLOB ? src->size(src->ctx) : -1;
		if (len < 0 && file_pass(c, k, 0, &len, e) < 0)
			return -1;
		if (len > 0x7FFFFFFFLL)
		{
			lob_err(e, "54000", "file value too large: the server takes values up to 2147483647 bytes");
			return -1;
		}
	}
	tok = calloc(1, 1);
	if (tok == NULL)
	{
		lob_err(e, "57011", "not enough memory for a file value");
		return -1;
	}
	c->lsrc[k].len = len;
	c->lsrc[k].tok = tok;
	c->nlsrc = k + 1;
	slot[0] = tok;
	return 0;
}

static void
ms_lob_in_done(void *vc, int ok)
{
	((msc *) vc)->nlsrc = 0;
}

static void
ms_lob_out_to(void *vc, xlob_dst *dst, int n)
{
	msc		   *c = vc;

	c->dst = n > 0 ? dst : NULL;
	c->ndst = n > 0 ? n : 0;
}

static const xbe_lob ms_lob = {
	ms_lob_in_sql, ms_lob_out_sql, ms_lob_sinks, ms_lob_in, ms_lob_in_done, NULL, ms_lob_out_to
};

/* XDB CONNECTION INFO: what the server answered to the login and what is known since */
static void
ms_info(void *vc, xbe_info_add add, void *ctx)
{
	msc		   *c = vc;
	char		v[200];

	snprintf(v, sizeof v, "TDS %d.%d (asked for %d.%d)", c->tds >> 4, c->tds & 15, c->tds_req >> 4,
			 c->tds_req & 15);
	add(ctx, "link", "protocol", v);
	if (c->tls_on && c->tls_login)
		add(ctx, "link", "encryption", "the login only");
	else if (c->tls_on)
	{
		xtls_info(c->tls, v, sizeof v);
		add(ctx, "link", "encryption", v[0] ? v : "on");
	}
	else
		add(ctx, "link", "encryption", "none");
	snprintf(v, sizeof v, "%d", c->spid);
	add(ctx, "link", "session_id", v);
	add(ctx, "link", "state", c->dead ? "connection is not usable" : c->in_txn ? "in a transaction" : "idle");
	snprintf(v, sizeof v, "%d bytes", c->psize);
	add(ctx, "link", "packet_size", v);
	snprintf(v, sizeof v, "%ld bytes", xnet_sockbuf(c->fd));
	add(ctx, "link", "socket_buffer", v);

	add(ctx, "server", "product", c->prod[0] ? c->prod : "SQL Server");
	snprintf(v, sizeof v, "%d.%d.%d", c->ver4[0], c->ver4[1], (c->ver4[2] << 8) | c->ver4[3]);
	add(ctx, "server", "version", v);
	add(ctx, "server", "database", c->dbname);
	snprintf(v, sizeof v, "%d%s", c->ccsid, c->ccsid_force ? " (option charset)" : " (from its collation)");
	add(ctx, "server", "varchar_ccsid", v);
	add(ctx, "server", "reports_transactions", c->txd_seen ? "yes" : "not seen yet");

	add(ctx, "options", "string_params", c->strmode == 1 ? "varchar" : c->strmode == 2 ? "nvarchar" : "auto");
	add(ctx, "options", "datetime2", c->dt2 ? "on" : "off");
}

const xbe_driver xbe_mssql = {
	"mssql", "@P%d", "MSS", 0,
	ms_connect, ms_disconnect, ms_version,
	ms_exec, ms_describe, ms_stmt_free,
	ms_open, ms_fetch, ms_close, ms_txn, ms_ping, &ms_lob, ms_info,
	ms_cursor_cols
};

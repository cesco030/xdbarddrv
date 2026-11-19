/*
 * xdbmy.c - MySQL / MariaDB backend of the XDB driver.
 *
 * A small native client for the MySQL wire protocol (no client library):
 * handshake v10, mysql_native_password and caching_sha2_password, optional
 * TLS, text queries, server-side prepared statements with read-only
 * cursors (COM_STMT_EXECUTE + COM_STMT_FETCH) so that several cursors and
 * other statements can be active on one connection.
 *
 * All protocol integers are little-endian on the wire and are read and
 * written byte by byte, so the code is the same on big-endian IBM i.
 * Compiled as UTF-8 (LOCALETYPE(*LOCALEUTF)) on IBM i.
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "xdb_be.h"


#define CL_LONG_PASSWORD 0x1u
#define CL_FOUND_ROWS 0x2u
#define CL_LONG_FLAG 0x4u
#define CL_CONNECT_WITH_DB 0x8u
#define CL_PROTOCOL_41 0x200u
#define CL_SSL 0x800u
#define CL_TRANSACTIONS 0x2000u
#define CL_SECURE_CONNECTION 0x8000u
#define CL_MULTI_RESULTS 0x20000u
#define CL_PLUGIN_AUTH 0x80000u

#define CF_UNSIGNED 0x0020
#define CF_HEX 0x8000			/* our own marker (the wire value NUM_FLAG is not used) */

#define ST_MORE_RESULTS 0x0008
#define ST_CURSOR_EXISTS 0x0040
#define ST_LAST_ROW_SENT 0x0080

#define LOB_PIECE (1024 * 1024)	/* a file travels in pieces of this size */
#define LOB_MAXSINK 32
#define LOB_MAXSRC 32
#define XLOB_NAMEMAX 1024		/* XLOB_PATHMAX of xdblob.h */

typedef struct
{
	int			fd;
	xtls	   *tls;
	unsigned char seq;
	unsigned char *buf;			/* last packet read */
	size_t		len;
	size_t		cap;
	int			version;		/* major * 100 + minor */
	int			status;
	int			in_txn;
	int			dead;			/* the connection failed: nothing more is sent on it */
	/* from the greeting of the server, for XDB CONNECTION INFO */
	char		vertext[80];
	unsigned long thread;		/* its number for this connection */
	int			srvcoll;		/* its default collation */
	/* file functions (xdblob.c) */
	long long	maxpkt;			/* max_allowed_packet of the server, 0 = not asked yet */
	int			sink[LOB_MAXSINK];	/* file columns of the statement described next */
	int			nsink;
	struct
	{
		xlob_src	src;
		int			kind;
		const char *tok;		/* the parameter value that stands for the file */
	}			lsrc[LOB_MAXSRC];	/* files of the statement executed next */
	int			nlsrc;
	xlob_dst   *dst;			/* where the file columns of the row fetched next go */
	int			ndst;
	unsigned char *piece;		/* buffer for the pieces of a file */
} myc;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct
{
	unsigned	id;
	int			nparams;
	int			ncols;
	int			pcols;			/* columns the prepare told; 0: known only when it runs */
	xbe_col    *ocols;			/* the columns told when it ran (pcols = 0), for my_cursor_cols */
	unsigned char *ctype;
	unsigned short *cflags;		/* column flags; CF_HEX = binary data shown as hex */
	int			cursor;			/* a server cursor is open */
	int			done;			/* last row has been sent */
	xbe_batch	inl;			/* rows the server sent without a cursor */
	int			inl_used;
	int		   *sink;			/* columns whose values go to files (PUT_xLOB_TO_FILE) */
	int			nsink;
} mystmt;

/* --------------------------------------------------------------- hashing */

static unsigned
rol(unsigned x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static unsigned
ror(unsigned x, int n)
{
	return (x >> n) | (x << (32 - n));
}

/* pads msg into 64-byte blocks (big-endian bit length); caller frees */
static unsigned char *
pad64(const unsigned char *msg, size_t len, size_t *total)
{
	size_t		t = ((len + 8) / 64 + 1) * 64;
	unsigned char *p = calloc(1, t);
	unsigned long long bits = (unsigned long long) len * 8;
	int			i;

	if (p == NULL)
		return NULL;
	memcpy(p, msg, len);
	p[len] = 0x80;
	for (i = 0; i < 8; i++)
		p[t - 1 - i] = (unsigned char) (bits >> (8 * i));
	*total = t;
	return p;
}

static void
sha1(const unsigned char *msg, size_t len, unsigned char out[20])
{
	unsigned	h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	size_t		total,
				o;
	unsigned char *p = pad64(msg, len, &total);
	int			i;

	if (p == NULL)
		return;
	for (o = 0; o < total; o += 64)
	{
		unsigned	w[80],
					a = h[0],
					b = h[1],
					c = h[2],
					d = h[3],
					e = h[4],
					f,
					k,
					t;

		for (i = 0; i < 16; i++)
			w[i] = ((unsigned) p[o + 4 * i] << 24) | ((unsigned) p[o + 4 * i + 1] << 16) |
				((unsigned) p[o + 4 * i + 2] << 8) | p[o + 4 * i + 3];
		for (; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		for (i = 0; i < 80; i++)
		{
			if (i < 20)
			{
				f = (b & c) | (~b & d);
				k = 0x5A827999u;
			}
			else if (i < 40)
			{
				f = b ^ c ^ d;
				k = 0x6ED9EBA1u;
			}
			else if (i < 60)
			{
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDCu;
			}
			else
			{
				f = b ^ c ^ d;
				k = 0xCA62C1D6u;
			}
			t = rol(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rol(b, 30);
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	free(p);
	for (i = 0; i < 20; i++)
		out[i] = (unsigned char) (h[i / 4] >> (24 - 8 * (i % 4)));
}

static void
sha256(const unsigned char *msg, size_t len, unsigned char out[32])
{
	static const unsigned K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};
	unsigned	h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
	0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	size_t		total,
				o;
	unsigned char *p = pad64(msg, len, &total);
	int			i;

	if (p == NULL)
		return;
	for (o = 0; o < total; o += 64)
	{
		unsigned	w[64],
					s[8],
					t1,
					t2;

		for (i = 0; i < 16; i++)
			w[i] = ((unsigned) p[o + 4 * i] << 24) | ((unsigned) p[o + 4 * i + 1] << 16) |
				((unsigned) p[o + 4 * i + 2] << 8) | p[o + 4 * i + 3];
		for (; i < 64; i++)
			w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
				w[i - 7] + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
		memcpy(s, h, sizeof s);
		for (i = 0; i < 64; i++)
		{
			t1 = s[7] + (ror(s[4], 6) ^ ror(s[4], 11) ^ ror(s[4], 25)) +
				((s[4] & s[5]) ^ (~s[4] & s[6])) + K[i] + w[i];
			t2 = (ror(s[0], 2) ^ ror(s[0], 13) ^ ror(s[0], 22)) +
				((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
			s[7] = s[6];
			s[6] = s[5];
			s[5] = s[4];
			s[4] = s[3] + t1;
			s[3] = s[2];
			s[2] = s[1];
			s[1] = s[0];
			s[0] = t1 + t2;
		}
		for (i = 0; i < 8; i++)
			h[i] += s[i];
	}
	free(p);
	for (i = 0; i < 32; i++)
		out[i] = (unsigned char) (h[i / 4] >> (24 - 8 * (i % 4)));
}

/* mysql_native_password: SHA1(pw) XOR SHA1(nonce + SHA1(SHA1(pw))) */
static int
scramble_native(const char *pw, const unsigned char *nonce, unsigned char *out)
{
	unsigned char h1[20],
				h2[20],
				m[40];
	int			i;

	if (pw[0] == '\0')
		return 0;
	sha1((const unsigned char *) pw, strlen(pw), h1);
	sha1(h1, 20, h2);
	memcpy(m, nonce, 20);
	memcpy(m + 20, h2, 20);
	sha1(m, 40, out);
	for (i = 0; i < 20; i++)
		out[i] ^= h1[i];
	return 20;
}

/* caching_sha2_password: SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) + nonce) */
static int
scramble_sha2(const char *pw, const unsigned char *nonce, unsigned char *out)
{
	unsigned char h1[32],
				h2[32],
				m[52];
	int			i;

	if (pw[0] == '\0')
		return 0;
	sha256((const unsigned char *) pw, strlen(pw), h1);
	sha256(h1, 32, h2);
	memcpy(m, h2, 32);
	memcpy(m + 32, nonce, 20);
	sha256(m, 52, out);
	for (i = 0; i < 32; i++)
		out[i] ^= h1[i];
	return 32;
}

/* ------------------------------------------------------------ packet I/O */

static int
io_read(myc *c, unsigned char *b, size_t n)
{
	while (n > 0)
	{
		int			r = c->tls ? xtls_read(c->tls, b, (int) n) : (int) recv(c->fd, (char *) b, n, 0);

		if (r <= 0)
		{
			c->dead = 1;
			return -1;
		}
		b += r;
		n -= (size_t) r;
	}
	return 0;
}

static int
io_write(myc *c, const unsigned char *b, size_t n)
{
	if (c->dead)
		return -1;
	while (n > 0)
	{
		int			r = c->tls ? xtls_write(c->tls, b, (int) n) : (int) send(c->fd, (char *) b, n, MSG_NOSIGNAL);

		if (r <= 0)
		{
			c->dead = 1;
			return -1;
		}
		b += r;
		n -= (size_t) r;
	}
	return 0;
}

/* reads one logical packet into c->buf (joining 16 MB continuation packets) */
static int
pkt_read(myc *c)
{
	unsigned char h[4];
	size_t		l;

	c->len = 0;
	do
	{
		if (io_read(c, h, 4) < 0)
			return -1;
		l = (size_t) h[0] | ((size_t) h[1] << 8) | ((size_t) h[2] << 16);
		c->seq = (unsigned char) (h[3] + 1);
		if (c->len + l + 1 > c->cap)
		{
			unsigned char *nb = realloc(c->buf, c->len + l + 1024);

			if (nb == NULL)
				return -1;
			c->buf = nb;
			c->cap = c->len + l + 1024;
		}
		if (l > 0 && io_read(c, c->buf + c->len, l) < 0)
			return -1;
		c->len += l;
	} while (l == 0xFFFFFF);
	c->buf[c->len] = 0;
	return 0;
}

static int
pkt_write(myc *c, const unsigned char *payload, size_t l)
{
	unsigned char *b;
	int			rc;

	if (l >= 0xFFFFFF)
		return -1;				/* statements of 16 MB and more are not supported */
	b = malloc(l + 4);
	if (b == NULL)
		return -1;
	b[0] = (unsigned char) (l & 0xFF);
	b[1] = (unsigned char) ((l >> 8) & 0xFF);
	b[2] = (unsigned char) ((l >> 16) & 0xFF);
	b[3] = c->seq++;
	memcpy(b + 4, payload, l);
	rc = io_write(c, b, l + 4);
	free(b);
	return rc;
}

static unsigned
le(const unsigned char *p, int n)
{
	unsigned	v = 0;
	int			i;

	for (i = n - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void
put_le(unsigned char *p, unsigned long long v, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char) (v >> (8 * i));
}

/* length-encoded integer; *isnull set for 0xFB */
static unsigned long long
lenenc(const unsigned char **pp, int *isnull)
{
	const unsigned char *p = *pp;
	unsigned long long v = 0;
	int			n = 0,
				i;

	if (isnull)
		*isnull = 0;
	if (p[0] < 0xFB)
	{
		*pp = p + 1;
		return p[0];
	}
	if (p[0] == 0xFB)
	{
		if (isnull)
			*isnull = 1;
		*pp = p + 1;
		return 0;
	}
	n = p[0] == 0xFC ? 2 : p[0] == 0xFD ? 3 : 8;
	for (i = n; i >= 1; i--)
		v = (v << 8) | p[i];
	*pp = p + 1 + n;
	return v;
}

static int
put_lenenc(unsigned char *p, unsigned long long v)
{
	if (v < 251)
	{
		p[0] = (unsigned char) v;
		return 1;
	}
	if (v < 65536)
	{
		p[0] = 0xFC;
		put_le(p + 1, v, 2);
		return 3;
	}
	if (v < 16777216)
	{
		p[0] = 0xFD;
		put_le(p + 1, v, 3);
		return 4;
	}
	p[0] = 0xFE;
	put_le(p + 1, v, 8);
	return 9;
}

static void
io_err(xbe_err *e, const char *what)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "08006");
	snprintf(e->msg, sizeof e->msg, "MySQL connection lost (%s)", what);
}

/* ERR packet in c->buf -> xbe_err */
static void
pkt_err(myc *c, xbe_err *e)
{
	const unsigned char *p = c->buf;

	memset(e, 0, sizeof *e);
	e->native = (int) le(p + 1, 2);
	if (c->len > 9 && p[3] == '#')
	{
		memcpy(e->sqlstate, p + 4, 5);
		snprintf(e->msg, sizeof e->msg, "%.*s", (int) (c->len - 9), p + 9);
	}
	else
	{
		snprintf(e->sqlstate, sizeof e->sqlstate, "HY000");
		snprintf(e->msg, sizeof e->msg, "%.*s", (int) (c->len - 3), p + 3);
	}
	/*
	 * The server's last words before it closes the session: 4031 after
	 * wait_timeout (sent unasked, so it is read as the answer to whatever is
	 * sent next), 3169 and MariaDB's 1927 when the session was killed.
	 */
	if (e->native == 4031 || e->native == 3169 || e->native == 1927)
	{
		snprintf(e->sqlstate, sizeof e->sqlstate, "08S01");
		c->dead = 1;
	}
	/* integrity errors all arrive as 23000: tell them apart */
	if (e->native == 1062 || e->native == 1586)
		snprintf(e->sqlstate, sizeof e->sqlstate, "23505");
	else if (e->native == 1451 || e->native == 1452 || e->native == 1216 || e->native == 1217)
		snprintf(e->sqlstate, sizeof e->sqlstate, "23503");
	else if (e->native == 1048 || e->native == 1364)
		snprintf(e->sqlstate, sizeof e->sqlstate, "23502");
	else if (e->native == 1064)
		snprintf(e->sqlstate, sizeof e->sqlstate, "42601");
	else if (e->native == 1205)
		snprintf(e->sqlstate, sizeof e->sqlstate, "57033");
	else if (e->native == 1366 || e->native == 1265)
		snprintf(e->sqlstate, sizeof e->sqlstate, "22018");
	else if (e->native == 1292)
		snprintf(e->sqlstate, sizeof e->sqlstate, "22007");
	else if (e->native == 1045 || e->native == 1044 || e->native == 1698)
		snprintf(e->sqlstate, sizeof e->sqlstate, "28000");
	/*
	 * A deadlock victim loses its whole transaction (a lock wait timeout
	 * only the statement).  Start a new transaction right away, so that
	 * what follows is not committed statement by statement.
	 */
	if (e->native == 1213 && c->in_txn)
	{
		static const unsigned char q[] = "\003START TRANSACTION";
		xbe_err		keep = *e;

		c->seq = 0;
		if (pkt_write(c, q, sizeof q - 1) == 0 && pkt_read(c) == 0)
			keep.rolled_back = 1;
		*e = keep;
	}
}

static int
is_eof(myc *c)
{
	return c->len < 9 && c->buf[0] == 0xFE;
}

/* sends a command packet (sequence restarts at 0) */
static int
cmd(myc *c, const unsigned char *payload, size_t l, xbe_err *e)
{
	c->seq = 0;
	if (pkt_write(c, payload, l) < 0)
	{
		io_err(e, "write");
		return -1;
	}
	return 0;
}

/* --------------------------------------------------------------- connect */

static void
my_disconnect(void *vc)
{
	myc		   *c = vc;
	unsigned char q = 0x01;

	if (c->fd >= 0)
	{
		c->seq = 0;
		pkt_write(c, &q, 1);
		if (c->tls)
			xtls_end(c->tls);
		close(c->fd);
	}
	free(c->buf);
	free(c->piece);
	free(c);
}

/* one connection attempt; use_tls: 0 no, 1 yes */
static myc *
attempt(const char *host, const char *port, int timeout, const char *db, const char *user,
		const char *pw, int use_tls, int verify, int *tls_failed, xbe_err *e)
{
	myc		   *c = calloc(1, sizeof(myc));
	const unsigned char *p,
			   *end;
	unsigned char nonce[21],
				auth[64],
			   *o;
	unsigned char out[1024];
	char		plugin[64],
				ver[64];
	unsigned	caps,
				mycaps;
	int			alen,
				n;

	*tls_failed = 0;
	if (c == NULL)
		return NULL;
	c->fd = xnet_connect(host, port, timeout, e);
	if (c->fd < 0)
	{
		free(c);
		return NULL;
	}
	if (pkt_read(c) < 0)
	{
		io_err(e, "handshake");
		goto fail;
	}
	p = c->buf;
	end = c->buf + c->len;
	if (p[0] == 0xFF)
	{
		pkt_err(c, e);
		goto fail;
	}
	if (p[0] != 10)
	{
		io_err(e, "unsupported protocol version");
		goto fail;
	}
	snprintf(ver, sizeof ver, "%s", (const char *) p + 1);
	snprintf(c->vertext, sizeof c->vertext, "%s", ver);
	p += 1 + strlen((const char *) p + 1) + 1;
	c->thread = (unsigned long) le(p, 4);
	p += 4;						/* thread id */
	memcpy(nonce, p, 8);
	p += 9;
	caps = le(p, 2);
	p += 2;
	c->srvcoll = p[0];
	p += 1 + 2;					/* charset, status */
	caps |= le(p, 2) << 16;
	p += 2;
	alen = p[0];
	p += 1 + 10;
	memset(nonce + 8, 0, 13);
	if (p + 12 <= end)
		memcpy(nonce + 8, p, 12);
	p += (alen > 21) ? alen - 8 : 13;
	plugin[0] = '\0';
	if ((caps & CL_PLUGIN_AUTH) && p < end)
		snprintf(plugin, sizeof plugin, "%s", (const char *) p);
	{
		const char *v = strncmp(ver, "5.5.5-", 6) == 0 ? ver + 6 : ver;

		c->version = atoi(v) * 100 + (strchr(v, '.') ? atoi(strchr(v, '.') + 1) : 0);
	}
	if (use_tls && !(caps & CL_SSL))
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "08001");
		snprintf(e->msg, sizeof e->msg, "the MySQL server does not offer TLS");
		*tls_failed = 1;
		goto fail;
	}
	mycaps = CL_LONG_PASSWORD | CL_FOUND_ROWS | CL_LONG_FLAG | CL_PROTOCOL_41 | CL_TRANSACTIONS |
		CL_SECURE_CONNECTION | CL_MULTI_RESULTS | CL_PLUGIN_AUTH;
	if (db[0])
		mycaps |= CL_CONNECT_WITH_DB;
	if (use_tls)
		mycaps |= CL_SSL;
	memset(out, 0, 32);
	put_le(out, mycaps, 4);
	put_le(out + 4, 0x40000000u, 4);
	out[8] = 45;				/* utf8mb4_general_ci */
	if (use_tls)
	{
		char		terr[256];

		if (pkt_write(c, out, 32) < 0)
		{
			io_err(e, "write");
			goto fail;
		}
		c->tls = xtls_start(c->fd, host, verify, terr, sizeof terr);
		if (c->tls == NULL)
		{
			memset(e, 0, sizeof *e);
			snprintf(e->sqlstate, sizeof e->sqlstate, "08001");
			snprintf(e->msg, sizeof e->msg, "%s", terr);
			*tls_failed = 1;
			goto fail;
		}
	}
	if (strcmp(plugin, "caching_sha2_password") == 0)
		alen = scramble_sha2(pw, nonce, auth);
	else
	{
		alen = scramble_native(pw, nonce, auth);
		if (strcmp(plugin, "mysql_native_password") != 0)
			alen = 0;			/* server will switch to a plugin we know */
	}
	o = out + 32;
	n = snprintf((char *) o, 300, "%s", user);
	o += n + 1;
	*o++ = (unsigned char) alen;
	memcpy(o, auth, alen);
	o += alen;
	if (db[0])
	{
		n = snprintf((char *) o, 200, "%s", db);
		o += n + 1;
	}
	n = snprintf((char *) o, 64, "%s", plugin[0] ? plugin : "mysql_native_password");
	o += n + 1;
	if (pkt_write(c, out, (size_t) (o - out)) < 0)
	{
		io_err(e, "write");
		goto fail;
	}
	for (;;)
	{
		if (pkt_read(c) < 0)
		{
			io_err(e, "authentication");
			goto fail;
		}
		if (c->buf[0] == 0x00)
			break;
		if (c->buf[0] == 0xFF)
		{
			pkt_err(c, e);
			goto fail;
		}
		if (c->buf[0] == 0xFE)	/* switch to another authentication plugin */
		{
			snprintf(plugin, sizeof plugin, "%s", (const char *) c->buf + 1);
			memset(nonce, 0, sizeof nonce);
			memcpy(nonce, c->buf + 1 + strlen(plugin) + 1,
				   c->len - strlen(plugin) - 2 >= 20 ? 20 : c->len - strlen(plugin) - 2);
			if (strcmp(plugin, "caching_sha2_password") == 0)
				alen = scramble_sha2(pw, nonce, auth);
			else if (strcmp(plugin, "mysql_native_password") == 0)
				alen = scramble_native(pw, nonce, auth);
			else
			{
				memset(e, 0, sizeof *e);
				snprintf(e->sqlstate, sizeof e->sqlstate, "28000");
				snprintf(e->msg, sizeof e->msg, "authentication plugin %s is not supported", plugin);
				goto fail;
			}
			if (pkt_write(c, auth, alen) < 0)
			{
				io_err(e, "write");
				goto fail;
			}
			continue;
		}
		if (c->buf[0] == 0x01 && c->len >= 2 && c->buf[1] == 0x03)
			continue;			/* caching_sha2: fast authentication succeeded */
		if (c->buf[0] == 0x01 && c->len >= 2 && c->buf[1] == 0x04)
		{
			/* full authentication: the password itself, only over TLS */
			if (c->tls == NULL)
			{
				memset(e, 0, sizeof *e);
				snprintf(e->sqlstate, sizeof e->sqlstate, "28000");
				snprintf(e->msg, sizeof e->msg,
						 "caching_sha2_password needs TLS for this login (use tls=require)");
				*tls_failed = 2;
				goto fail;
			}
			if (pkt_write(c, (const unsigned char *) pw, strlen(pw) + 1) < 0)
			{
				io_err(e, "write");
				goto fail;
			}
			continue;
		}
		io_err(e, "unexpected authentication packet");
		goto fail;
	}
	return c;
fail:
	if (c->tls)
		xtls_end(c->tls);
	close(c->fd);
	free(c->buf);
	free(c);
	return NULL;
}

static int	my_exec(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
					long long *rows, xbe_err *e);

/*
 * conninfo keys: host port dbname connect_timeout passfile
 *                tls=off|prefer|require|verify (default prefer)
 * The password comes from the CONNECT statement, the password key of
 * xdb.conf or the passfile (host:port:database:user:password lines).
 */
static void *
my_connect(const xbe_opts *o, xbe_err *e)
{
	char		host[256],
				port[16],
				db[128],
				tls[16],
				pwf[256],
				pw[256],
				init[512],
				t[16];
	myc		   *c;
	int			tf = 0,
				timeout;

	memset(e, 0, sizeof *e);
	if (xnet_kv(o->conninfo, "host", host, sizeof host) == NULL)
		snprintf(host, sizeof host, "127.0.0.1");
	if (xnet_kv(o->conninfo, "port", port, sizeof port) == NULL)
		snprintf(port, sizeof port, "3306");
	xnet_kv(o->conninfo, "dbname", db, sizeof db);
	if (xnet_kv(o->conninfo, "tls", tls, sizeof tls) == NULL)
		snprintf(tls, sizeof tls, "prefer");
	timeout = xnet_kv(o->conninfo, "connect_timeout", t, sizeof t) ? atoi(t) : 15;
	snprintf(pw, sizeof pw, "%s", o->password ? o->password : "");
	if (pw[0] == '\0' && xnet_kv(o->conninfo, "passfile", pwf, sizeof pwf))
		xnet_passfile(pwf, host, port, db, o->user, pw, sizeof pw);
	if (strcmp(tls, "off") == 0)
		c = attempt(host, port, timeout, db, o->user, pw, 0, 0, &tf, e);
	else
	{
		c = attempt(host, port, timeout, db, o->user, pw, 1, strcmp(tls, "verify") == 0, &tf, e);
		if (c == NULL && tf == 1 && strcmp(tls, "prefer") == 0)
			c = attempt(host, port, timeout, db, o->user, pw, 0, 0, &tf, e);
	}
	memset(pw, 0, sizeof pw);
	if (c == NULL)
		return NULL;
	if (o->conf && o->conf(o->rdb, "init_sql", init, sizeof init) == 0 && init[0])
	{
		long long	r;

		if (my_exec(c, NULL, init, 0, NULL, NULL, &r, e) < 0)
		{
			my_disconnect(c);
			return NULL;
		}
	}
	return c;
}

/* COM_PING on an idle connection */
static int
my_ping(void *vc, int timeout)
{
	myc		   *c = vc;
	unsigned char q = 0x0e;

	if (c->dead || c->fd < 0)
		return -1;
	if (xnet_dead(c->fd, c->tls == NULL))
	{
		c->dead = 1;
		return -1;
	}
	c->seq = 0;
	if (pkt_write(c, &q, 1) < 0 || xnet_wait_readable(c->fd, timeout) <= 0 ||
		pkt_read(c) < 0 || c->len < 1 || c->buf[0] != 0x00)
	{
		c->dead = 1;
		return -1;
	}
	return 0;
}

static int
my_version(void *vc)
{
	return ((myc *) vc)->version;
}

/* ---------------------------------------------------------- result sets */

static int
mbmax(unsigned cs)
{
	if (cs == 63)
		return 1;
	if (cs == 33 || cs == 83 || (cs >= 192 && cs <= 215))
		return 3;
	if (cs == 45 || cs == 46 || (cs >= 224 && cs <= 323))
		return 4;
	return 1;
}

/* column definition packet -> xbe_col (+ wire type and flags) */
static void
coldef(myc *c, xbe_col *x, unsigned char *type, unsigned short *flags)
{
	const unsigned char *p = c->buf;
	unsigned long long l;
	unsigned	cs,
				len,
				dec;
	int			i;

	for (i = 0; i < 4; i++)
	{
		l = lenenc(&p, NULL);
		p += l;
	}
	l = lenenc(&p, NULL);
	snprintf(x->name, sizeof x->name, "%.*s", (int) (l > 63 ? 63 : l), (const char *) p);
	p += l;
	l = lenenc(&p, NULL);
	p += l;
	p += 1;
	cs = le(p, 2);
	len = le(p + 2, 4);
	*type = p[6];
	*flags = (unsigned short) (le(p + 7, 2) & ~CF_HEX);
	if (cs == 63 && (*type == 15 || (*type >= 249 && *type <= 254)))
		*flags |= CF_HEX;		/* BINARY, VARBINARY, BLOB */
	dec = p[9];
	x->len = 0;
	x->prec = 0;
	x->scale = 0;
	switch (*type)
	{
		case 1:
		case 2:
		case 13:
			x->kind = ((*flags & 0x20) && *type == 2) ? XK_INTEGER : XK_SMALLINT;
			break;
		case 3:
		case 9:
			x->kind = ((*flags & 0x20) && *type == 3) ? XK_BIGINT : XK_INTEGER;
			break;
		case 8:
		case 16:
			x->kind = XK_BIGINT;
			if (*type == 8 && (*flags & 0x20))
			{
				x->kind = XK_DECIMAL;
				x->prec = 20;
			}
			break;
		case 4:
		case 5:
			x->kind = XK_DOUBLE;
			break;
		case 0:
		case 246:
			x->kind = XK_DECIMAL;
			x->scale = (int) dec;
			x->prec = (int) len - (dec > 0 ? 1 : 0) - ((*flags & 0x20) ? 0 : 1);
			if (x->prec < 1)
				x->prec = 1;
			break;
		case 10:
		case 14:
			x->kind = XK_DATE;
			break;
		case 11:
			x->kind = XK_TIME;
			break;
		case 7:
		case 12:
			x->kind = XK_TIMESTAMP;
			break;
		case 254:
			x->kind = (*flags & (0x900 | CF_HEX)) ? XK_VARCHAR : XK_CHAR;	/* ENUM / SET */
			x->len = (int) (len / (unsigned) mbmax(cs)) * ((*flags & CF_HEX) ? 2 : 1);
			break;
		case 15:
		case 253:
			x->kind = XK_VARCHAR;
			x->len = (int) (len / (unsigned) mbmax(cs)) * ((*flags & CF_HEX) ? 2 : 1);
			break;
		default:				/* TEXT/BLOB families, JSON, GEOMETRY, NULL */
			x->kind = XK_VARCHAR;
			x->len = 0;
			break;
	}
}

static void
batch_release(xbe_batch *b)
{
	int			i;

	for (i = 0; i < b->nrows * b->ncols; i++)
		free(b->val[i]);
	free(b->val);
	memset(b, 0, sizeof *b);
}

/* binary protocol row in c->buf -> text values appended to b */
static int
row_decode(myc *c, mystmt *s, xbe_batch *b)
{
	const unsigned char *p = c->buf + 1;
	const unsigned char *nb = p;
	int			j;
	char		t[64];

	/* no columns: there is no row to keep, and nothing to allocate for one */
	if (s->ncols <= 0)
		return -1;
	if (xnet_batch_room(b, s->ncols) < 0)
		return -1;
	p += (s->ncols + 7 + 2) / 8;
	for (j = 0; j < s->ncols; j++)
	{
		char	  **slot = &b->val[b->nrows * s->ncols + j];
		int			uns = s->cflags[j] & 0x20;
		unsigned long long l;
		int			n,
					one = 1;
		unsigned char r[8];
		double		d;
		float		f;

		*slot = NULL;
		if (nb[(j + 2) / 8] & (1 << ((j + 2) % 8)))
			continue;
		switch (s->ctype[j])
		{
			case 1:
				snprintf(t, sizeof t, "%d", uns ? (int) p[0] : (int) (signed char) p[0]);
				p += 1;
				break;
			case 2:
			case 13:
				snprintf(t, sizeof t, "%d", uns ? (int) le(p, 2) : (int) (short) le(p, 2));
				p += 2;
				break;
			case 3:
			case 9:
				if (uns)
					snprintf(t, sizeof t, "%u", le(p, 4));
				else
					snprintf(t, sizeof t, "%d", (int) le(p, 4));
				p += 4;
				break;
			case 8:
				{
					unsigned long long v = (unsigned long long) le(p, 4) | ((unsigned long long) le(p + 4, 4) << 32);

					if (uns)
						snprintf(t, sizeof t, "%llu", v);
					else
						snprintf(t, sizeof t, "%lld", (long long) v);
					p += 8;
					break;
				}
			case 4:
				for (n = 0; n < 4; n++)
					r[n] = *(char *) &one ? p[n] : p[3 - n];
				memcpy(&f, r, 4);
				snprintf(t, sizeof t, "%.9g", (double) f);
				p += 4;
				break;
			case 5:
				for (n = 0; n < 8; n++)
					r[n] = *(char *) &one ? p[n] : p[7 - n];
				memcpy(&d, r, 8);
				snprintf(t, sizeof t, "%.17g", d);
				p += 8;
				break;
			case 7:
			case 10:
			case 12:
			case 14:
				n = p[0];
				if (n == 0)
					snprintf(t, sizeof t, "0000-00-00");
				else if (s->ctype[j] == 10 || s->ctype[j] == 14)
					snprintf(t, sizeof t, "%04u-%02u-%02u", le(p + 1, 2), (unsigned) p[3], (unsigned) p[4]);
				else
					snprintf(t, sizeof t, "%04u-%02u-%02u %02u:%02u:%02u.%06u", le(p + 1, 2),
							 (unsigned) p[3], (unsigned) p[4],
							 n >= 7 ? (unsigned) p[5] : 0, n >= 7 ? (unsigned) p[6] : 0,
							 n >= 7 ? (unsigned) p[7] : 0, n >= 11 ? le(p + 8, 4) : 0);
				p += 1 + n;
				break;
			case 11:
				n = p[0];
				if (n == 0)
					snprintf(t, sizeof t, "00:00:00");
				else
					snprintf(t, sizeof t, "%s%02u:%02u:%02u", p[1] ? "-" : "",
							 le(p + 2, 4) * 24 + p[6], (unsigned) p[7], (unsigned) p[8]);
				p += 1 + n;
				break;
			case 16:			/* BIT: big-endian bytes */
				{
					unsigned long long v = 0;

					l = lenenc(&p, NULL);
					for (n = 0; n < (int) l; n++)
						v = (v << 8) | p[n];
					snprintf(t, sizeof t, "%llu", v);
					p += l;
					break;
				}
			default:			/* everything else is a length-encoded string */
				l = lenenc(&p, NULL);
				*slot = malloc((size_t) l * ((s->cflags[j] & CF_HEX) ? 2 : 1) + 1);
				if (*slot == NULL)
					return -1;
				if (s->cflags[j] & CF_HEX)
				{
					static const char hx[] = "0123456789ABCDEF";
					size_t		k;

					for (k = 0; k < (size_t) l; k++)
					{
						(*slot)[2 * k] = hx[p[k] >> 4];
						(*slot)[2 * k + 1] = hx[p[k] & 15];
					}
					(*slot)[2 * l] = '\0';
				}
				else
				{
					memcpy(*slot, p, (size_t) l);
					(*slot)[l] = '\0';
				}
				p += l;
				continue;
		}
		*slot = strdup(t);
		if (*slot == NULL)
			return -1;
	}
	b->nrows++;
	b->ncols = s->ncols;
	return 0;
}

/* reads column definitions (count known) and the EOF packet that follows */
static int
read_coldefs(myc *c, int n, xbe_col *cols, unsigned char *types, unsigned short *flags, xbe_err *e)
{
	int			i;
	xbe_col		dummy;
	unsigned char t;
	unsigned short f;

	for (i = 0; i < n; i++)
	{
		if (pkt_read(c) < 0)
		{
			io_err(e, "read");
			return -1;
		}
		coldef(c, cols ? &cols[i] : &dummy, types ? &types[i] : &t, flags ? &flags[i] : &f);
	}
	if (n > 0)
	{
		if (pkt_read(c) < 0)
		{
			io_err(e, "read");
			return -1;
		}
		if (is_eof(c))
			c->status = (int) le(c->buf + 3, 2);
	}
	return 0;
}

/* consumes whatever follows a command: OK, ERR or result sets (discarded) */
static int
drain(myc *c, int binary, long long *rows, xbe_err *e)
{
	int			rc = 0;

	for (;;)
	{
		const unsigned char *p;

		if (pkt_read(c) < 0)
		{
			io_err(e, "read");
			return -1;
		}
		p = c->buf;
		if (p[0] == 0xFF)
		{
			pkt_err(c, e);
			return -1;
		}
		if (p[0] == 0x00)
		{
			unsigned long long a;

			p++;
			a = lenenc(&p, NULL);
			lenenc(&p, NULL);
			c->status = (int) le(p, 2);
			if (rows)
				*rows = (long long) a;
		}
		else
		{
			int			n = (int) lenenc(&p, NULL);

			if (read_coldefs(c, n, NULL, NULL, NULL, e) < 0)
				return -1;
			for (;;)
			{
				if (pkt_read(c) < 0)
				{
					io_err(e, "read");
					return -1;
				}
				if (c->buf[0] == 0xFF)
				{
					pkt_err(c, e);
					rc = -1;
					break;
				}
				if (is_eof(c))
				{
					c->status = (int) le(c->buf + 3, 2);
					break;
				}
			}
			if (rc < 0)
				return rc;
		}
		if (!(c->status & ST_MORE_RESULTS))
			break;
	}
	return rc;
}

/*
 * The rows of a result set that the server sends at once (no cursor), and
 * whatever follows them: the further result sets of a procedure, which are
 * dropped, and the end of its CALL.  keep = the statement that keeps the
 * rows (in keep->inl), NULL = they are dropped too.  The answer is read to
 * its end whatever happens, so that the next command finds the connection
 * in step with the server.  -1 with e set: the server reported an error,
 * the connection broke or memory ran out; the caller releases keep->inl.
 */
static int
rows_at_once(myc *c, mystmt *keep, xbe_err *e)
{
	int			nomem = 0;

	for (;;)
	{
		if (pkt_read(c) < 0)
		{
			io_err(e, "read");
			return -1;
		}
		if (c->buf[0] == 0xFF)
		{
			pkt_err(c, e);		/* an error ends the answer */
			return -1;
		}
		if (is_eof(c))
		{
			c->status = (int) le(c->buf + 3, 2);
			break;
		}
		if (keep && !nomem && row_decode(c, keep, &keep->inl) < 0)
			nomem = 1;
	}
	if (c->status & ST_MORE_RESULTS)
	{
		long long	r;

		if (drain(c, 1, &r, e) < 0)
			return -1;
	}
	if (nomem)
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "57011");
		snprintf(e->msg, sizeof e->msg, "not enough memory for the rows of the statement");
		return -1;
	}
	return 0;
}

/* ----------------------------------------------- files (see xdblob.c) */

/*
 * GET_xLOB_FROM_FILE: the place of the function is taken by a parameter
 * marker, and the file is sent as the value of that parameter in pieces
 * (COM_STMT_SEND_LONG_DATA) just before the statement is executed, so the
 * driver never holds more than one piece.  PUT_xLOB_TO_FILE: the column is
 * the name of the file, a zero byte and the value; the row is read from the
 * socket piece by piece and the value goes to the file as it arrives.
 * Either way the server limits a value to its max_allowed_packet.
 */

static void
lob_err(xbe_err *e, const char *state, const char *msg)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", state);
	snprintf(e->msg, sizeof e->msg, "%s", msg);
}

/* makes room for n more bytes in c->buf */
static int
buf_need(myc *c, size_t n)
{
	if (c->len + n + 1 > c->cap)
	{
		size_t		nc = (c->len + n) * 2 + 1024;
		unsigned char *nb = realloc(c->buf, nc);

		if (nb == NULL)
			return -1;
		c->buf = nb;
		c->cap = nc;
	}
	return 0;
}

/*
 * A small query in the text protocol: its rows as text in b (NULL = SQL
 * NULL; released with batch_release).
 */
static int
text_query(myc *c, const char *sql, xbe_batch *b, xbe_err *e)
{
	size_t		l = strlen(sql);
	unsigned char *q = malloc(l + 1);
	const unsigned char *p;
	int			n,
				j,
				rc;

	memset(b, 0, sizeof *b);
	if (q == NULL)
		return -1;
	q[0] = 0x03;
	memcpy(q + 1, sql, l);
	rc = cmd(c, q, l + 1, e);
	free(q);
	if (rc < 0)
		return -1;
	if (pkt_read(c) < 0)
	{
		io_err(e, "read");
		return -1;
	}
	p = c->buf;
	if (p[0] == 0xFF)
	{
		pkt_err(c, e);
		return -1;
	}
	if (p[0] == 0x00)
		return 0;				/* no result set */
	n = (int) lenenc(&p, NULL);
	if (read_coldefs(c, n, NULL, NULL, NULL, e) < 0)
		return -1;
	for (;;)
	{
		if (pkt_read(c) < 0)
		{
			io_err(e, "read");
			batch_release(b);
			return -1;
		}
		if (c->buf[0] == 0xFF)
		{
			pkt_err(c, e);
			batch_release(b);
			return -1;
		}
		if (is_eof(c))
			break;
		if (xnet_batch_room(b, n) < 0)
		{
			batch_release(b);
			c->dead = 1;		/* the rest of the result is not read */
			return -1;
		}
		p = c->buf;
		for (j = 0; j < n; j++)
		{
			int			isnull;
			unsigned long long vl = lenenc(&p, &isnull);
			char	   *v = NULL;

			if (!isnull && (v = malloc((size_t) vl + 1)) != NULL)
			{
				memcpy(v, p, (size_t) vl);
				v[vl] = '\0';
			}
			p += vl;
			b->val[b->nrows * n + j] = v;
		}
		b->nrows++;
		b->ncols = n;
	}
	return 0;
}

/* max_allowed_packet of the server: no value and no packet may be longer */
static int
my_maxpkt(myc *c, xbe_err *e)
{
	xbe_batch	b;

	if (c->maxpkt > 0)
		return 0;
	if (text_query(c, "SELECT @@max_allowed_packet", &b, e) < 0)
		return -1;
	if (b.nrows == 1 && b.val[0])
		c->maxpkt = strtoll(b.val[0], NULL, 10);
	batch_release(&b);
	if (c->maxpkt < 1024)
		c->maxpkt = 1024;		/* the smallest value a server accepts */
	return 0;
}

/* COM_STMT_RESET: closes a cursor and forgets the pieces sent for parameters */
static void
stmt_reset(myc *c, mystmt *s)
{
	unsigned char q[5];
	long long	r;
	xbe_err		e;

	q[0] = 0x1A;
	put_le(q + 1, s->id, 4);
	if (cmd(c, q, 5, &e) == 0)
		drain(c, 1, &r, &e);
	s->cursor = 0;
}

/* the file that parameter value v stands for, or -1 */
static int
lsrc_of(myc *c, const char *v)
{
	int			k;

	if (v != NULL)
		for (k = 0; k < c->nlsrc; k++)
			if (c->lsrc[k].tok == v)
				return k;
	return -1;
}

/*
 * Sends the file of source k as the value of parameter i of statement s.
 * Nothing comes back for these packets; what the server thinks of them it
 * says when the statement is executed.
 */
static int
long_data(myc *c, mystmt *s, int i, int k, xbe_err *e)
{
	xlob_src   *src = &c->lsrc[k].src;
	long long	total = 0;
	int			piece,
				sent = 0;

	if (my_maxpkt(c, e) < 0)
		return -1;
	if (src->size(src->ctx) > c->maxpkt)
	{
		/* known before a byte of it has gone out */
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "54000");
		snprintf(e->msg, sizeof e->msg, "file value too large: the server takes values up to %lld bytes "
				 "(its max_allowed_packet)", c->maxpkt);
		return -1;
	}
	piece = c->maxpkt - 64 < LOB_PIECE ? (int) (c->maxpkt - 64) : LOB_PIECE;
	if (c->piece == NULL && (c->piece = malloc(LOB_PIECE + 16)) == NULL)
	{
		lob_err(e, "57011", "not enough memory for a file value");
		return -1;
	}
	c->piece[0] = 0x18;			/* COM_STMT_SEND_LONG_DATA */
	put_le(c->piece + 1, s->id, 4);
	put_le(c->piece + 5, (unsigned) i, 2);
	for (;;)
	{
		int			have = 0,
					n = 1;

		/* fill the piece: a read may give less than was asked */
		while (have < piece && n > 0)
		{
			n = src->read(src->ctx, (char *) c->piece + 7 + have, piece - have, e);
			if (n > 0)
				have += n;
		}
		if (n < 0)
		{
			if (sent)
				stmt_reset(c, s);
			return -1;
		}
		total += have;
		if (total > c->maxpkt)
		{
			if (sent)
				stmt_reset(c, s);
			memset(e, 0, sizeof *e);
			snprintf(e->sqlstate, sizeof e->sqlstate, "54000");
			snprintf(e->msg, sizeof e->msg, "file value too large: the server takes values up to %lld bytes "
					 "(its max_allowed_packet)", c->maxpkt);
			return -1;
		}
		/* an empty file is one empty piece, which tells the server that the value is not null */
		if (have > 0 || !sent)
		{
			if (cmd(c, c->piece, (size_t) have + 7, e) < 0)
				return -1;
			sent = 1;
		}
		if (n == 0)
			break;				/* the end of the file */
	}
	return 0;
}

/* the logical packet being read from the socket piece by piece */
typedef struct
{
	myc		   *c;
	size_t		frag;			/* length of the physical packet being read */
	size_t		left;			/* bytes of it not read yet */
} mysr;

static int
sr_head(mysr *r)
{
	unsigned char h[4];

	if (io_read(r->c, h, 4) < 0)
		return -1;
	r->frag = (size_t) h[0] | ((size_t) h[1] << 8) | ((size_t) h[2] << 16);
	r->left = r->frag;
	r->c->seq = (unsigned char) (h[3] + 1);
	return 0;
}

static int
sr_begin(myc *c, mysr *r)
{
	r->c = c;
	return sr_head(r);
}

/* n bytes of the logical packet into b (NULL: skipped); -1 = the connection is of no use any more */
static int
sr_read(mysr *r, unsigned char *b, size_t n)
{
	unsigned char skip[2048];

	while (n > 0)
	{
		size_t		k;

		if (r->left == 0)
		{
			/* a packet of 16 MB - 1 is followed by the rest; anything else was the end */
			if (r->frag != 0xFFFFFF || sr_head(r) < 0 || r->left == 0)
			{
				r->c->dead = 1;
				return -1;
			}
		}
		k = n < r->left ? n : r->left;
		if (b == NULL && k > sizeof skip)
			k = sizeof skip;
		if (io_read(r->c, b ? b : skip, k) < 0)
			return -1;
		if (b)
			b += k;
		r->left -= k;
		n -= k;
	}
	return 0;
}

/* whatever is left of the logical packet is skipped */
static int
sr_end(mysr *r)
{
	for (;;)
	{
		if (r->left > 0 && sr_read(r, NULL, r->left) < 0)
			return -1;
		if (r->frag != 0xFFFFFF)
			return 0;
		if (sr_head(r) < 0)
		{
			r->c->dead = 1;
			return -1;
		}
	}
}

/* is column j of s one whose value goes to a file?  Its number among them, or -1 */
static int
sink_of(const mystmt *s, int j)
{
	int			k;

	for (k = 0; k < s->nsink; k++)
		if (s->sink[k] == j)
			return k;
	return -1;
}

/*
 * The value of a file column: the name of the file, a zero byte, the
 * bytes.  len bytes of it are waiting in r.  The file is opened and
 * written as they arrive; when that fails, the rest is still read (the
 * row has to be taken off the line) and *fe keeps the first failure.
 */
static int
sink_value(myc *c, mysr *r, unsigned long long len, xlob_dst *dst, xbe_err *fe, int *fbad)
{
	unsigned char head[XLOB_NAMEMAX + 8];
	size_t		pre = len < sizeof head ? (size_t) len : sizeof head;
	size_t		z;
	int			ok = !*fbad;

	if (sr_read(r, head, pre) < 0)
		return -1;
	for (z = 0; z < pre && head[z] != 0; z++)
		;
	if (z >= pre || z >= XLOB_NAMEMAX)
	{
		if (ok)
		{
			lob_err(fe, z >= pre && pre < sizeof head ? "58004" : "428A1",
					z >= pre && pre < sizeof head ?
					"a file column came back from the server in a form the driver does not know" :
					"file name of a row is too long (reason 2)");
			*fbad = 1;
		}
		return sr_read(r, NULL, (size_t) (len - pre));
	}
	if (ok && dst == NULL)
	{
		lob_err(fe, "58004", "a file column arrived and nothing says where it goes");
		*fbad = 1;
		ok = 0;
	}
	if (ok && dst->open(dst->ctx, (const char *) head, fe) < 0)
	{
		*fbad = 1;
		ok = 0;
	}
	if (ok && pre > z + 1 && dst->write(dst->ctx, (const char *) head + z + 1, (int) (pre - z - 1), fe) < 0)
	{
		*fbad = 1;
		ok = 0;
	}
	len -= pre;
	while (len > 0)
	{
		size_t		k = len < 65536 ? (size_t) len : 65536;

		if (sr_read(r, c->piece, k) < 0)
			return -1;
		if (ok && dst->write(dst->ctx, (const char *) c->piece, (int) k, fe) < 0)
		{
			*fbad = 1;
			ok = 0;
		}
		len -= k;
	}
	if (ok)
		dst->done(dst->ctx);
	return 0;
}

/*
 * A row of a statement with file columns, read from the socket (its first
 * byte, 0x00, has been read).  What is not a file column is gathered in
 * c->buf as the row packet without them and decoded as usual; a file
 * column goes to its destination and leaves 24 bytes for the core.
 * -1 = the connection is of no use any more, or memory ran out.
 */
static int
row_stream(myc *c, mystmt *s, mysr *r, xbe_batch *b, xbe_err *fe, int *fbad)
{
	size_t		nb = (size_t) (s->ncols + 7 + 2) / 8;
	int			j;

	if (c->piece == NULL && (c->piece = malloc(LOB_PIECE + 16)) == NULL)
		return -1;
	c->len = 0;
	if (buf_need(c, 1 + nb) < 0)
		return -1;
	c->buf[0] = 0x00;
	if (sr_read(r, c->buf + 1, nb) < 0)
		return -1;
	c->len = 1 + nb;
	for (j = 0; j < s->ncols; j++)
	{
		unsigned char h[9];
		size_t		hl = 0;
		unsigned long long l;
		int			k;

		if (c->buf[1 + (j + 2) / 8] & (1 << ((j + 2) % 8)))
			continue;			/* null */
		switch (s->ctype[j])
		{
			case 1:
				l = 1;
				break;
			case 2:
			case 13:
				l = 2;
				break;
			case 3:
			case 9:
			case 4:
				l = 4;
				break;
			case 8:
			case 5:
				l = 8;
				break;
			case 7:
			case 10:
			case 11:
			case 12:
			case 14:
				if (sr_read(r, h, 1) < 0)
					return -1;
				hl = 1;
				l = h[0];
				break;
			default:			/* a length-encoded string */
				if (sr_read(r, h, 1) < 0)
					return -1;
				hl = 1;
				if (h[0] < 0xFB)
					l = h[0];
				else
				{
					int			n = h[0] == 0xFC ? 2 : h[0] == 0xFD ? 3 : h[0] == 0xFE ? 8 : -1;
					int			i;

					if (n < 0 || sr_read(r, h + 1, (size_t) n) < 0)
					{
						c->dead = 1;
						return -1;
					}
					hl = 1 + (size_t) n;
					for (l = 0, i = n; i >= 1; i--)
						l = (l << 8) | h[i];
				}
				break;
		}
		k = sink_of(s, j);
		if (k >= 0)
		{
			/* in the row that is decoded, an empty string stands for it */
			if (buf_need(c, 1) < 0)
				return -1;
			c->buf[c->len++] = 0x00;
			if (sink_value(c, r, l, k < c->ndst ? &c->dst[k] : NULL, fe, fbad) < 0)
				return -1;
			continue;
		}
		if (buf_need(c, hl + (size_t) l) < 0)
		{
			c->dead = 1;
			return -1;
		}
		memcpy(c->buf + c->len, h, hl);
		c->len += hl;
		if (sr_read(r, c->buf + c->len, (size_t) l) < 0)
			return -1;
		c->len += (size_t) l;
	}
	if (sr_end(r) < 0)
		return -1;
	c->buf[c->len] = 0;
	if (row_decode(c, s, b) < 0)
		return -1;
	for (j = 0; j < s->nsink; j++)
		if (s->sink[j] >= 0 && s->sink[j] < s->ncols)
		{
			char	  **cell = &b->val[(b->nrows - 1) * s->ncols + s->sink[j]];

			if (*cell != NULL)
			{
				/* room for the number of bytes written, which the core knows */
				free(*cell);
				*cell = calloc(1, 24);
				if (*cell == NULL)
					return -1;
			}
		}
	return 0;
}

/* a fetch of one row for a statement with file columns */
static int
fetch_sink(myc *c, mystmt *s, xbe_batch *b, xbe_err *e)
{
	unsigned char q[9];
	xbe_err		fe;
	int			fbad = 0;

	q[0] = 0x1C;				/* COM_STMT_FETCH */
	put_le(q + 1, s->id, 4);
	put_le(q + 5, 1, 4);
	if (cmd(c, q, 9, e) < 0)
		return -1;
	for (;;)
	{
		mysr		r;
		unsigned char first;

		if (sr_begin(c, &r) < 0 || r.left == 0 || sr_read(&r, &first, 1) < 0)
		{
			c->dead = 1;
			io_err(e, "read");
			batch_release(b);
			return -1;
		}
		if (first == 0xFF || (first == 0xFE && r.frag < 9))
		{
			/* the end of the rows, or an error: a short packet, read whole */
			c->len = 0;
			if (buf_need(c, r.frag + 1) < 0 || sr_read(&r, c->buf + 1, r.left) < 0)
			{
				c->dead = 1;
				io_err(e, "read");
				batch_release(b);
				return -1;
			}
			c->buf[0] = first;
			c->len = r.frag;
			c->buf[c->len] = 0;
			if (first == 0xFF)
			{
				pkt_err(c, e);
				batch_release(b);
				return -1;
			}
			c->status = (int) le(c->buf + 3, 2);
			if ((c->status & ST_LAST_ROW_SENT) || b->nrows == 0)
				s->done = 1;
			b->eof = s->done;
			break;
		}
		if (first != 0x00 || row_stream(c, s, &r, b, &fe, &fbad) < 0)
		{
			c->dead = 1;
			io_err(e, "read");
			batch_release(b);
			return -1;
		}
	}
	if (fbad)
	{
		batch_release(b);
		*e = fe;
		return -1;
	}
	return 0;
}

/*
 * After a statement with file columns was opened: a value longer than
 * max_allowed_packet does not fail on this server, it becomes NULL with a
 * warning.  That must not pass for a null value.
 */
static int
sink_warnings(myc *c, mystmt *s, xbe_err *e)
{
	xbe_batch	b;
	int			i,
				found = 0;

	if (text_query(c, "SHOW WARNINGS", &b, e) < 0)
		return -1;
	for (i = 0; i < b.nrows && b.ncols >= 3; i++)
		if (b.val[i * b.ncols + 1] && atoi(b.val[i * b.ncols + 1]) == 1301)
		{
			memset(e, 0, sizeof *e);
			e->native = 1301;
			snprintf(e->sqlstate, sizeof e->sqlstate, "54000");
			snprintf(e->msg, sizeof e->msg, "file value too large for the server: %.300s",
					 b.val[i * b.ncols + 2] ? b.val[i * b.ncols + 2] : "");
			found = 1;
			break;
		}
	batch_release(&b);
	return found ? -1 : 0;
}

static int
my_lob_in_sql(void *vc, int kind, char *out, int outsize)
{
	snprintf(out, (size_t) outsize, "?");
	return 1;
}

static int
my_lob_out_sql(void *vc, int kind, int stage, char *pre, char *mid, char *post, xbe_err *e)
{
	/* the name first: it has to be known when the value arrives */
	snprintf(pre, XLOB_SQLPRE, "CONCAT(IFNULL(CONVERT((");
	snprintf(mid, XLOB_SQLMID, ") USING utf8mb4), ''), 0x00, %s(", kind == XLOB_CLOB ? "CONVERT(" : "");
	snprintf(post, XLOB_SQLMID, "%s))", kind == XLOB_CLOB ? ") USING utf8mb4" : "");
	return 1;
}

static void
my_lob_sinks(void *vc, const int *cols, int n)
{
	myc		   *c = vc;

	c->nsink = n > LOB_MAXSINK ? LOB_MAXSINK : n;
	memcpy(c->sink, cols, sizeof(int) * (size_t) c->nsink);
}

/* the file is not read here: it travels when the statement is executed */
static int
my_lob_in(void *vc, int kind, xlob_src *src, char **slot, xbe_err *e)
{
	myc		   *c = vc;
	char	   *tok;

	slot[0] = NULL;
	if (c->nlsrc >= LOB_MAXSRC)
	{
		lob_err(e, "54023", "too many files in one statement");
		return -1;
	}
	tok = calloc(1, 1);
	if (tok == NULL)
	{
		lob_err(e, "57011", "not enough memory for a file value");
		return -1;
	}
	c->lsrc[c->nlsrc].src = *src;
	c->lsrc[c->nlsrc].kind = kind;
	c->lsrc[c->nlsrc].tok = tok;
	c->nlsrc++;
	slot[0] = tok;
	return 0;
}

static void
my_lob_in_done(void *vc, int ok)
{
	((myc *) vc)->nlsrc = 0;
}

static void
my_lob_out_to(void *vc, xlob_dst *dst, int n)
{
	myc		   *c = vc;

	c->dst = n > 0 ? dst : NULL;
	c->ndst = n > 0 ? n : 0;
}

static const xbe_lob my_lob = {
	my_lob_in_sql, my_lob_out_sql, my_lob_sinks, my_lob_in, my_lob_in_done, NULL, my_lob_out_to
};

/* ------------------------------------------------------------ statements */

static void
my_stmt_free(void *vc, void *vs)
{
	myc		   *c = vc;
	mystmt	   *s = vs;
	unsigned char q[5];

	if (s == NULL)
		return;
	q[0] = 0x19;
	put_le(q + 1, s->id, 4);
	c->seq = 0;
	pkt_write(c, q, 5);			/* COM_STMT_CLOSE has no reply */
	if (s->inl.val)
		batch_release(&s->inl);
	free(s->ctype);
	free(s->cflags);
	free(s->ocols);
	free(s->sink);
	free(s);
}

static int
my_describe(void *vc, const char *sql, xbe_col **cols, int *ncols, void **stmt, xbe_err *e)
{
	myc		   *c = vc;
	size_t		l = strlen(sql);
	unsigned char *q = malloc(l + 1);
	mystmt	   *s;
	int			sink[LOB_MAXSINK];
	int			nsink = c->nsink;

	/* the file columns named for this statement: for no other */
	memcpy(sink, c->sink, sizeof(int) * (size_t) nsink);
	c->nsink = 0;
	*cols = NULL;
	*ncols = 0;
	*stmt = NULL;
	if (q == NULL)
		return -1;
	q[0] = 0x16;
	memcpy(q + 1, sql, l);
	if (cmd(c, q, l + 1, e) < 0)
	{
		free(q);
		return -1;
	}
	free(q);
	if (pkt_read(c) < 0)
	{
		io_err(e, "read");
		return -1;
	}
	if (c->buf[0] == 0xFF)
	{
		pkt_err(c, e);
		if (e->native == 1295)	/* statement cannot be prepared: run it as text */
		{
			memset(e, 0, sizeof *e);
			return 0;
		}
		return -1;
	}
	s = calloc(1, sizeof(mystmt));
	if (s == NULL)
		return -1;
	s->id = le(c->buf + 1, 4);
	s->ncols = (int) le(c->buf + 5, 2);
	s->pcols = s->ncols;
	s->nparams = (int) le(c->buf + 7, 2);
	if (read_coldefs(c, s->nparams, NULL, NULL, NULL, e) < 0)
	{
		free(s);
		return -1;
	}
	if (s->ncols > 0)
	{
		*cols = calloc(s->ncols, sizeof(xbe_col));
		s->ctype = calloc(s->ncols, 1);
		s->cflags = calloc(s->ncols, sizeof(unsigned short));
		if (*cols == NULL || s->ctype == NULL || s->cflags == NULL ||
			read_coldefs(c, s->ncols, *cols, s->ctype, s->cflags, e) < 0)
		{
			free(*cols);
			*cols = NULL;
			free(s->ctype);
			free(s->cflags);
			free(s);
			return -1;
		}
	}
	if (nsink > 0 && s->ncols > 0)
	{
		s->sink = malloc(sizeof(int) * (size_t) nsink);
		if (s->sink == NULL)
		{
			free(*cols);
			*cols = NULL;
			my_stmt_free(c, s);
			return -1;
		}
		memcpy(s->sink, sink, sizeof(int) * (size_t) nsink);
		s->nsink = nsink;
	}
	*ncols = s->ncols;
	*stmt = s;
	return 0;
}

/*
 * COM_STMT_EXECUTE with all parameters sent as strings; a parameter that
 * stands for a file (my_lob_in) has been sent before it, piece by piece.
 */
static int
stmt_execute(myc *c, mystmt *s, int np, char **pv, int cursor, xbe_err *e)
{
	size_t		need = 16 + (size_t) np * 12;
	unsigned char *q,
			   *o;
	int			i,
				rc;

	if (np != s->nparams)
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "07001");
		snprintf(e->msg, sizeof e->msg, "statement has %d parameter markers, %d values given",
				 s->nparams, np);
		return -1;
	}
	for (i = 0; i < np; i++)
	{
		int			k = lsrc_of(c, pv[i]);

		if (k >= 0 && long_data(c, s, i, k, e) < 0)
			return -1;
		need += pv[i] ? strlen(pv[i]) : 0;
	}
	q = calloc(1, need);
	if (q == NULL)
		return -1;
	q[0] = 0x17;
	put_le(q + 1, s->id, 4);
	q[5] = cursor ? 0x01 : 0x00;
	put_le(q + 6, 1, 4);
	o = q + 10;
	if (np > 0)
	{
		unsigned char *nullmap = o;

		o += (np + 7) / 8;
		*o++ = 1;				/* types follow */
		for (i = 0; i < np; i++)
		{
			int			k = lsrc_of(c, pv[i]);

			if (pv[i] == NULL)
				nullmap[i / 8] |= (unsigned char) (1 << (i % 8));
			/* VAR_STRING: the server converts; the bytes of a file are a BLOB */
			*o++ = (k >= 0 && c->lsrc[k].kind == XLOB_BLOB) ? 252 : 253;
			*o++ = 0;
		}
		for (i = 0; i < np; i++)
			if (pv[i] && lsrc_of(c, pv[i]) < 0)
			{
				size_t		l = strlen(pv[i]);

				o += put_lenenc(o, l);
				memcpy(o, pv[i], l);
				o += l;
			}
	}
	rc = cmd(c, q, (size_t) (o - q), e);
	free(q);
	return rc;
}

static int
my_exec(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
		long long *rows, xbe_err *e)
{
	myc		   *c = vc;
	mystmt	   *s = stmt;
	int			rc,
				temp = 0;

	if (rows)
		*rows = 0;
	if (s == NULL && np == 0)
	{
		size_t		l = strlen(sql);
		unsigned char *q = malloc(l + 1);

		if (q == NULL)
			return -1;
		q[0] = 0x03;			/* COM_QUERY */
		memcpy(q + 1, sql, l);
		rc = cmd(c, q, l + 1, e);
		free(q);
		return rc < 0 ? rc : drain(c, 0, rows, e);
	}
	if (s == NULL)
	{
		xbe_col    *cols;
		int			nc;
		void	   *vs;

		if (my_describe(c, sql, &cols, &nc, &vs, e) < 0)
			return -1;
		free(cols);
		s = vs;
		temp = 1;
		if (s == NULL)
		{
			memset(e, 0, sizeof *e);
			snprintf(e->sqlstate, sizeof e->sqlstate, "07001");
			snprintf(e->msg, sizeof e->msg, "this statement cannot take parameter markers on MySQL");
			return -1;
		}
	}
	if (s->cursor)
	{
		unsigned char q[5];
		long long	r;

		q[0] = 0x1A;			/* COM_STMT_RESET closes an open cursor */
		put_le(q + 1, s->id, 4);
		if (cmd(c, q, 5, e) == 0)
			drain(c, 1, &r, e);
		s->cursor = 0;
	}
	rc = stmt_execute(c, s, np, pv, 0, e);
	if (rc == 0)
		rc = drain(c, 1, rows, e);
	if (temp)
		my_stmt_free(c, s);
	return rc;
}

static int
my_open(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
		int hold, void **cur, xbe_err *e)
{
	myc		   *c = vc;
	mystmt	   *s = stmt;
	const unsigned char *p;
	const char *bad;
	int			n;

	if (s == NULL)
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "07005");
		snprintf(e->msg, sizeof e->msg, "this statement cannot be opened as a cursor on MySQL");
		return -1;
	}
	if (s->cursor)
	{
		unsigned char q[5];
		long long	r;

		q[0] = 0x1A;
		put_le(q + 1, s->id, 4);
		if (cmd(c, q, 5, e) == 0)
			drain(c, 1, &r, e);
		s->cursor = 0;
	}
	if (s->inl.val)
		batch_release(&s->inl);
	s->inl_used = 0;
	s->done = 0;
	/*
	 * A cursor is asked for only when the prepare told the columns.  When it
	 * told none (CALL, EXPLAIN, some SHOW and table maintenance statements)
	 * the server has no cursor to give and sends the rows at once; asked for
	 * one all the same, it refuses some of these statements ("Incorrect
	 * arguments to with cursor") and MySQL 8.0 ends abnormally on a prepared
	 * EXPLAIN (seen with 8.0.46: the server restarts).
	 */
	if (stmt_execute(c, s, np, pv, s->pcols > 0, e) < 0)
		return -1;
	if (pkt_read(c) < 0)
	{
		io_err(e, "read");
		return -1;
	}
	p = c->buf;
	if (p[0] == 0xFF)
	{
		pkt_err(c, e);
		return -1;
	}
	if (p[0] == 0x00)
	{
		/* it has run and has no rows to give: the answer is read to its end all the same */
		p++;
		lenenc(&p, NULL);
		lenenc(&p, NULL);
		c->status = (int) le(p, 2);
		if (c->status & ST_MORE_RESULTS)
		{
			long long	r;

			if (drain(c, 1, &r, e) < 0)
				return -1;
		}
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "07005");
		snprintf(e->msg, sizeof e->msg, "the statement ran and returned no rows: there is nothing to fetch");
		return -1;
	}
	n = (int) lenenc(&p, NULL);
	bad = NULL;
	if (s->pcols == 0)
	{
		/*
		 * The prepare told no columns: the server describes the rows only
		 * now that the statement has run (the rows of a procedure above all).
		 */
		free(s->ctype);
		free(s->cflags);
		free(s->ocols);
		s->ncols = 0;
		s->ctype = calloc((size_t) n + 1, 1);
		s->cflags = calloc((size_t) n + 1, sizeof(unsigned short));
		s->ocols = calloc((size_t) n + 1, sizeof(xbe_col));
		if (s->ctype == NULL || s->cflags == NULL || s->ocols == NULL)
			bad = "not enough memory for the columns of the statement";
		else
			s->ncols = n;
	}
	else if (n != s->ncols)
		bad = "the columns of the statement are not those it had when it was prepared: prepare it again";
	if (bad)
	{
		xbe_err		e2;

		/* the statement has run: what the server sends is read and dropped */
		memset(&e2, 0, sizeof e2);
		if (read_coldefs(c, n, NULL, NULL, NULL, &e2) == 0)
		{
			if (c->status & ST_CURSOR_EXISTS)
				stmt_reset(c, s);
			else
				rows_at_once(c, NULL, &e2);
		}
		lob_err(e, s->pcols == 0 ? "57011" : "07008", bad);
		return -1;
	}
	if (read_coldefs(c, n, s->ocols, s->ctype, s->cflags, e) < 0)
		return -1;
	if (c->status & ST_CURSOR_EXISTS)
	{
		s->cursor = 1;
		if (s->nsink > 0 && sink_warnings(c, s, e) < 0)
		{
			stmt_reset(c, s);
			return -1;
		}
	}
	else
	{
		/*
		 * The server sent the rows at once: they are kept on the client.
		 * After the rows of a procedure come its further result sets, which
		 * are dropped, and the end of the CALL.
		 */
		if (rows_at_once(c, s->nsink == 0 ? s : NULL, e) < 0)
		{
			if (s->inl.val)
				batch_release(&s->inl);
			return -1;
		}
		s->done = 1;
		if (s->nsink > 0)
		{
			/* a file can only be written while its row is read, one row at a time */
			lob_err(e, "0A000", "PUT_BLOB_TO_FILE and PUT_CLOB_TO_FILE need a query "
					"that the server opens as a cursor");
			return -1;
		}
	}
	*cur = s;
	return 0;
}

/* the columns of a cursor whose statement had none when it was prepared */
static int
my_cursor_cols(void *vc, void *cur, xbe_col **cols, int *ncols, xbe_err *e)
{
	mystmt	   *s = cur;

	*cols = NULL;
	*ncols = 0;
	if (s == NULL || s->pcols != 0 || s->ocols == NULL || s->ncols <= 0)
		return 0;
	*cols = malloc(sizeof(xbe_col) * (size_t) s->ncols);
	if (*cols == NULL)
	{
		lob_err(e, "57011", "not enough memory for the columns of the statement");
		return -1;
	}
	memcpy(*cols, s->ocols, sizeof(xbe_col) * (size_t) s->ncols);
	*ncols = s->ncols;
	return 0;
}

static int
my_fetch(void *vc, void *cur, int maxrows, xbe_batch *b, xbe_err *e)
{
	myc		   *c = vc;
	mystmt	   *s = cur;
	unsigned char q[9];

	memset(b, 0, sizeof *b);
	b->release = batch_release;
	if (!s->cursor)
	{
		if (s->inl.val && !s->inl_used)
		{
			*b = s->inl;
			b->release = batch_release;
			memset(&s->inl, 0, sizeof s->inl);
			s->inl_used = 1;
		}
		b->eof = 1;
		return 0;
	}
	if (s->done)
	{
		b->eof = 1;
		return 0;
	}
	if (s->nsink > 0)
		return fetch_sink(c, s, b, e);
	q[0] = 0x1C;				/* COM_STMT_FETCH */
	put_le(q + 1, s->id, 4);
	put_le(q + 5, (unsigned) maxrows, 4);
	if (cmd(c, q, 9, e) < 0)
		return -1;
	for (;;)
	{
		if (pkt_read(c) < 0)
		{
			io_err(e, "read");
			batch_release(b);
			return -1;
		}
		if (c->buf[0] == 0xFF)
		{
			pkt_err(c, e);
			batch_release(b);
			return -1;
		}
		if (is_eof(c))
		{
			c->status = (int) le(c->buf + 3, 2);
			if ((c->status & ST_LAST_ROW_SENT) || b->nrows == 0)
				s->done = 1;
			b->eof = s->done;
			break;
		}
		if (row_decode(c, s, b) < 0)
		{
			batch_release(b);
			return -1;
		}
	}
	return 0;
}

static void
my_close(void *vc, void *cur)
{
	myc		   *c = vc;
	mystmt	   *s = cur;
	xbe_err		e;

	if (s->cursor)
	{
		unsigned char q[5];
		long long	r;

		q[0] = 0x1A;
		put_le(q + 1, s->id, 4);
		if (cmd(c, q, 5, &e) == 0)
			drain(c, 1, &r, &e);
		s->cursor = 0;
	}
	if (s->inl.val)
		batch_release(&s->inl);
	s->inl_used = 0;
	s->done = 0;
}

static int
my_txn(void *vc, int op, xbe_err *e)
{
	long long	r;

	switch (op)
	{
		case XT_BEGIN:
			((myc *) vc)->in_txn = 1;
			return my_exec(vc, NULL, "START TRANSACTION", 0, NULL, NULL, &r, e);
		case XT_COMMIT:
			((myc *) vc)->in_txn = 0;
			if (my_exec(vc, NULL, "COMMIT", 0, NULL, NULL, &r, e) == 0)
				return 0;
			/* an error packet is the server's answer: it has not committed; a broken line tells nothing */
			if (e->native != 0)
				e->rolled_back = 1;
			return -1;
		case XT_ROLLBACK:
			((myc *) vc)->in_txn = 0;
			return my_exec(vc, NULL, "ROLLBACK", 0, NULL, NULL, &r, e);
		default:
			return 0;			/* a failed statement does not abort the transaction */
	}
}

/* XDB CONNECTION INFO: what the greeting of the server said and what is known since */
static void
my_info(void *vc, xbe_info_add add, void *ctx)
{
	myc		   *c = vc;
	char		v[160];

	add(ctx, "link", "protocol", "MySQL protocol 10");
	if (c->tls)
	{
		xtls_info(c->tls, v, sizeof v);
		add(ctx, "link", "encryption", v[0] ? v : "on");
	}
	else
		add(ctx, "link", "encryption", "none");
	snprintf(v, sizeof v, "%lu", c->thread);
	add(ctx, "link", "session_id", v);
	add(ctx, "link", "state", c->dead ? "connection is not usable" :
		(c->status & 0x0001) ? "in a transaction" : "idle");
	add(ctx, "link", "character_set", "utf8mb4");

	add(ctx, "server", "product", strstr(c->vertext, "MariaDB") ? "MariaDB" : "MySQL");
	add(ctx, "server", "version", c->vertext);
	snprintf(v, sizeof v, "%d", c->srvcoll);
	add(ctx, "server", "default_collation_id", v);
	add(ctx, "server", "autocommit", (c->status & 0x0002) ? "on" : "off");
	if (c->maxpkt > 0)
		snprintf(v, sizeof v, "%lld", c->maxpkt);
	else
		snprintf(v, sizeof v, "not asked yet (the file functions ask)");
	add(ctx, "server", "max_allowed_packet", v);
}

const xbe_driver xbe_mysql = {
	"mysql", "?", "MYS", 0,
	my_connect, my_disconnect, my_version,
	my_exec, my_describe, my_stmt_free,
	my_open, my_fetch, my_close, my_txn, my_ping, &my_lob, my_info,
	my_cursor_cols
};

/*
 * xdblob.c - large values between stream files and the server.
 *
 * Db2 for i hands an exit program at most 32K per value and no large
 * object at all, so a value of megabytes cannot pass through a host
 * variable.  It can pass beside Db2: the statement names a stream file and
 * the driver, which runs in the job of the program, moves the value
 * between that file and the server on the connection and in the unit of
 * work of the statement.  Four functions in the statement text ask for it:
 *
 *   GET_BLOB_FROM_FILE(name)            the bytes of a file, as a value
 *   GET_CLOB_FROM_FILE(name)            the text of a file, as a value
 *       INSERT INTO DOC (ID, BODY) VALUES(?, GET_BLOB_FROM_FILE(?))
 *       UPDATE DOC SET NOTE = GET_CLOB_FROM_FILE('/home/x/note.txt') WHERE ID = 7
 *     The names and the meaning are those of the Db2 for i functions.  name
 *     is a string literal or a host variable (parameter marker).  A second
 *     argument 0 or 1 is accepted as Db2 does and has no effect on stream
 *     files.  Text is converted from the CCSID of the file.
 *
 *   PUT_BLOB_TO_FILE(value, name [, option])
 *   PUT_CLOB_TO_FILE(value, name [, option [, ccsid]])
 *       SELECT ID, PUT_BLOB_TO_FILE(BODY, '/home/x/doc' || ID || '.pdf') FROM DOC
 *     A column of the result of a query (fetched through a cursor): each
 *     row writes its value to the file named by the second argument, any
 *     expression of the server, and the column gives the number of bytes
 *     the file grew by (BIGINT), or NULL when the value is NULL - then no
 *     file is touched.  option is that of a file reference variable: 8 =
 *     the file must not exist, 16 = create or replace (default), 32 =
 *     create or append.  ccsid is the CCSID a new text file gets (default
 *     1208); a file that exists keeps its CCSID and the text is converted
 *     to it, as CPYTOSTMF does.  A new file of bytes has CCSID 65535.
 *
 * This module finds the functions (xlob_parse) and does the file work; how
 * a value travels is the business of the backend (xbe_lob in xdb_be.h).
 * PostgreSQL moves a long value through a large object of the server, in
 * pieces, before or after the statement.  MySQL and SQL Server take a file
 * as the value of a parameter while the statement is sent, and deliver a
 * value for a file inside its row, so that it is written while the row is
 * read.  In no case is more than a piece of a value in memory.
 * The statement text must reach the driver as written: dynamic SQL.
 *
 * Files are opened as the user of the job: the IFS does not use adopted
 * authority, so the driver gives nobody access to a file he does not have.
 *
 * Compiled like the core: LOCALETYPE(*LOCALEUTF) TGTCCSID(1208).
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
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __ILEC400__
#include <qlg.h>
#endif
#include "xdblob.h"

extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);

/* ------------------------------------------------------------ the statement */

static const struct
{
	const char *name;
	int			kind;
	int			put;
}			fns[] = {
	{"GET_BLOB_FROM_FILE", XLOB_BLOB, 0}, {"GET_CLOB_FROM_FILE", XLOB_CLOB, 0},
	{"PUT_BLOB_TO_FILE", XLOB_BLOB, 1}, {"PUT_CLOB_TO_FILE", XLOB_CLOB, 1}
};
#define NFNS 4

typedef struct
{
	char	   *s;
	size_t		n;
	size_t		cap;
	int			bad;
} sbuf;

static void
sb_add(sbuf *b, const char *p, size_t l)
{
	if (b->bad)
		return;
	if (b->n + l + 1 > b->cap)
	{
		size_t		c = (b->n + l + 1) * 2 + 256;
		char	   *x = realloc(b->s, c);

		if (x == NULL)
		{
			b->bad = 1;
			return;
		}
		b->s = x;
		b->cap = c;
	}
	memcpy(b->s + b->n, p, l);
	b->n += l;
	b->s[b->n] = '\0';
}

static void
sb_adds(sbuf *b, const char *z)
{
	sb_add(b, z, strlen(z));
}

/* characters of a name */
static int
namec(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		c == '_' || c == '$' || (c & 0x80);
}

/* as the core's rewrite_markers() reads what follows :H */
static int
markc(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int
spacec(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/*
 * How the text at hand is read: set by xlob_text() for the statement the
 * core is about to look at (one statement at a time in a job).
 */
static const char *tbase = NULL;
static int	tflags = 0;

void
xlob_text(const char *start, int flags)
{
	tbase = start;
	tflags = flags;
}

/*
 * p at a literal, a quoted name or a comment: its end, else p.  What ends
 * a literal and what is a comment is the server's to say (XTF_*):
 * PostgreSQL has $tag$ ... $tag$ and E'...' and comments inside comments,
 * MySQL takes a backslash for an escape and # for a comment, SQL Server
 * quotes names with brackets.  The core's rewrite_markers() reads with
 * this function too.
 */
const char *
xlob_run_end(const char *p)
{
	if (*p == '\'' || *p == '"' || *p == '`')
	{
		char		q = *p;
		int			bs = 0;

		if (q == '\'')
		{
			bs = (tflags & XTF_BACKSLASH) != 0;
			if ((tflags & XTF_ESTRING) && tbase && p > tbase && (p[-1] == 'E' || p[-1] == 'e') &&
				(p - 1 == tbase || !namec((unsigned char) p[-2])))
				bs = 1;
		}
		else if (q == '"')
			bs = (tflags & XTF_BACKSLASH_DQ) != 0;
		p++;
		while (*p)
		{
			if (bs && *p == '\\' && p[1])
			{
				p += 2;
				continue;
			}
			if (*p++ == q)
			{
				if (*p != q)
					break;
				p++;
			}
		}
		return p;
	}
	if ((tflags & XTF_BRACKET) && *p == '[')
	{
		const char *e = p + 1;

		/* [name], a closing bracket inside written twice */
		while (*e)
		{
			if (*e++ == ']')
			{
				if (*e != ']')
					return e;
				e++;
			}
		}
		return e;
	}
	if ((tflags & XTF_DOLLAR) && *p == '$' && (tbase == NULL || p == tbase || !namec((unsigned char) p[-1])))
	{
		const char *t = p + 1;

		/* $$ or $tag$: to the same once more; $1 is a parameter, a$b a name */
		if (!(*t >= '0' && *t <= '9'))
		{
			while ((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z') || (*t >= '0' && *t <= '9') || *t == '_' ||
				   (*t & 0x80))
				t++;
			if (*t == '$')
			{
				size_t		tl = (size_t) (t + 1 - p);
				const char *e = t + 1;

				for (;;)
				{
					e = strchr(e, '$');
					if (e == NULL)
						return p + strlen(p);
					if (strncmp(e, p, tl) == 0)
						return e + tl;
					e++;
				}
			}
		}
		return p;
	}
	if (p[0] == '-' && p[1] == '-' &&
		(!(tflags & XTF_DASHSPACE) || p[2] == '\0' || spacec((unsigned char) p[2])))
	{
		while (*p && *p != '\n')
			p++;
		return p;
	}
	if ((tflags & XTF_HASH) && *p == '#')
	{
		while (*p && *p != '\n')
			p++;
		return p;
	}
	if (p[0] == '/' && p[1] == '*')
	{
		const char *e;

		if (tflags & XTF_NESTED)
		{
			int			depth = 1;

			for (e = p + 2; *e && depth > 0; e++)
			{
				if (e[0] == '/' && e[1] == '*')
				{
					depth++;
					e++;
				}
				else if (e[0] == '*' && e[1] == '/')
				{
					depth--;
					e++;
				}
			}
			return e;
		}
		e = strstr(p + 2, "*/");
		return e ? e + 2 : p + strlen(p);
	}
	return p;
}

#define run_end xlob_run_end

/* p at a parameter marker (? or :H, with the :H of an indicator): its end, else p */
static const char *
marker_end(const char *sql, const char *p)
{
	if (*p == '?')
		return p + 1;
	if (p[0] == ':' && p[1] == 'H' && (p == sql || p[-1] != ':') && !markc((unsigned char) p[2]))
	{
		const char *q = p + 2;

		while (*q == ' ')
			q++;
		if (q[0] == ':' && q[1] == 'H')
			return q + 2;
		return p + 2;
	}
	return p;
}

/* is the word [p, e) this keyword (upper case), whatever its case */
static int
word_is(const char *p, const char *e, const char *kw)
{
	size_t		l = strlen(kw),
				i;

	if ((size_t) (e - p) != l)
		return 0;
	for (i = 0; i < l; i++)
	{
		int			c = (unsigned char) p[i];

		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (c != kw[i])
			return 0;
	}
	return 1;
}

/* a keyword that ends the select list */
static int
list_end(const char *p, const char *e)
{
	static const char *const kw[] = {
		"FROM", "WHERE", "GROUP", "HAVING", "WINDOW", "UNION", "INTERSECT", "EXCEPT",
		"ORDER", "LIMIT", "OFFSET", "FETCH", "FOR", "INTO", NULL
	};
	int			i;

	for (i = 0; kw[i]; i++)
		if (word_is(p, e, kw[i]))
			return 1;
	return 0;
}

/*
 * The word [p, e) names one of the file functions and "(" follows: its
 * index, else -1.  *open is set after the parenthesis.
 */
static int
fn_at(const char *sql, const char *p, const char *e, const char **open)
{
	int			i;
	const char *q = e;

	if (p > sql && p[-1] == '.')
		return -1;				/* schema.function: something of the server's */
	while (spacec((unsigned char) *q))
		q++;
	if (*q != '(')
		return -1;
	for (i = 0; i < NFNS; i++)
		if (word_is(p, e, fns[i].name))
		{
			*open = q + 1;
			return i;
		}
	return -1;
}

/*
 * p after "(": the arguments up to the matching ")", split at the commas
 * between them.  Returns the position after ")" (NULL: there is none) and
 * the number of arguments; at most max are noted.
 */
static const char *
split_args(const char *p, const char **a0, const char **a1, int max, int *n)
{
	int			depth = 0;
	const char *start = p;

	*n = 0;
	for (;;)
	{
		const char *r;

		if (*p == '\0')
			return NULL;
		r = run_end(p);
		if (r != p)
		{
			p = r;
			continue;
		}
		if (*p == '(')
			depth++;
		else if (*p == ')')
		{
			if (depth == 0)
			{
				if (*n < max)
				{
					a0[*n] = start;
					a1[*n] = p;
				}
				(*n)++;
				return p + 1;
			}
			depth--;
		}
		else if (*p == ',' && depth == 0)
		{
			if (*n < max)
			{
				a0[*n] = start;
				a1[*n] = p;
			}
			(*n)++;
			start = p + 1;
		}
		p++;
	}
}

static void
trim(const char **a, const char **b)
{
	while (*a < *b && spacec((unsigned char) **a))
		(*a)++;
	while (*b > *a && spacec((unsigned char) (*b)[-1]))
		(*b)--;
}

/* [a, b) is a whole number: its value, else -1 */
static long
number(const char *a, const char *b)
{
	long		v = 0;

	if (a >= b || b - a > 9)
		return -1;
	for (; a < b; a++)
	{
		if (*a < '0' || *a > '9')
			return -1;
		v = v * 10 + (*a - '0');
	}
	return v;
}

typedef struct
{
	const char *sql;			/* the whole statement */
	sbuf		o;
	xlob_plan  *pl;
	int			user;			/* host variables seen so far */
	int			nmarkcap;
	xbe_err    *e;
	int			failed;
} pctx;

static void
perr(pctx *c, const char *state, const char *fmt, const char *a, const char *b)
{
	if (c->failed)
		return;
	c->failed = 1;
	memset(c->e, 0, sizeof *c->e);
	snprintf(c->e->sqlstate, sizeof c->e->sqlstate, "%s", state);
	snprintf(c->e->msg, sizeof c->e->msg, fmt, a, b);
}

static void
add_mark(pctx *c, int user, int file, int slot)
{
	xlob_plan  *pl = c->pl;

	if (pl->nmark >= c->nmarkcap)
	{
		int			cap = c->nmarkcap * 2 + 32;
		xlob_mark  *m = realloc(pl->mark, sizeof(xlob_mark) * (size_t) cap);

		if (m == NULL)
		{
			c->o.bad = 1;
			return;
		}
		pl->mark = m;
		c->nmarkcap = cap;
	}
	pl->mark[pl->nmark].user = (short) user;
	pl->mark[pl->nmark].file = (short) file;
	pl->mark[pl->nmark].slot = (short) slot;
	pl->nmark++;
}

/*
 * Copies [p, e), a piece without file functions of its own (an argument,
 * a parenthesis), and counts the host variables in it.
 */
static void
copy_plain(pctx *c, const char *p, const char *e, const char *owner)
{
	while (p < e && !c->failed)
	{
		const char *r = run_end(p);

		if (r != p)
		{
			if (r > e)
				r = e;
			sb_add(&c->o, p, (size_t) (r - p));
			p = r;
			continue;
		}
		r = marker_end(c->sql, p);
		if (r != p)
		{
			add_mark(c, c->user++, 0, 0);
			sb_add(&c->o, p, (size_t) (r - p));
			p = r;
			continue;
		}
		if (namec((unsigned char) *p) && (p == c->sql || !namec((unsigned char) p[-1])))
		{
			const char *w = p;
			const char *open;

			while (w < e && namec((unsigned char) *w))
				w++;
			int			fn = fn_at(c->sql, p, w, &open);

			if (fn >= 0)
			{
				perr(c, "42601", "\"%s\" cannot be used inside %s", fns[fn].name, owner);
				return;
			}
			sb_add(&c->o, p, (size_t) (w - p));
			p = w;
			continue;
		}
		sb_add(&c->o, p, 1);
		p++;
	}
}

/* GET_xLOB_FROM_FILE(name [, 0 | 1]): p after "(".  Returns the end of the call. */
static const char *
do_get(pctx *c, int fn, const char *p, const xbe_driver *be, void *bc)
{
	const char *a0[3],
			   *a1[3];
	int			n,
				k,
				slots;
	const char *end = split_args(p, a0, a1, 3, &n);
	xlob_plan  *pl = c->pl;
	xlob_in    *in;
	char		snip[XLOB_SQLIN];

	if (end == NULL)
		return NULL;			/* no closing parenthesis: the caller says so */
	if (n < 1 || n > 2)
	{
		perr(c, "42601", "\"%s\" takes a file name%s", fns[fn].name, " (and 0 or 1 as Db2 does)");
		return NULL;
	}
	if (pl->nin >= XLOB_MAXIN)
	{
		perr(c, "54023", "\"%s\": too many files in one statement%s", fns[fn].name, "");
		return NULL;
	}
	trim(&a0[0], &a1[0]);
	if (n == 2)
	{
		long		v;

		trim(&a0[1], &a1[1]);
		v = number(a0[1], a1[1]);
		if (v != 0 && v != 1)
		{
			perr(c, "42601", "\"%s\": the second argument is 0 or 1%s", fns[fn].name, "");
			return NULL;
		}
	}
	in = &pl->in[pl->nin];
	memset(in, 0, sizeof *in);
	in->kind = fns[fn].kind;
	in->path_user = -1;
	if (a0[0] < a1[0] && *a0[0] == '\'' && run_end(a0[0]) == a1[0] && a1[0] - a0[0] >= 2)
	{
		/* a literal: '' stands for one quote */
		const char *q = a0[0] + 1;
		char	   *d = malloc((size_t) (a1[0] - a0[0]));
		char	   *w = d;

		if (d == NULL)
		{
			c->o.bad = 1;
			return NULL;
		}
		while (q < a1[0] - 1)
		{
			if (*q == '\'')
				q++;
			*w++ = *q++;
		}
		*w = '\0';
		in->path = d;
	}
	else if (a0[0] < a1[0] && marker_end(c->sql, a0[0]) == a1[0])
		in->path_user = c->user++;
	else
	{
		perr(c, "42601", "\"%s\": the file name must be a literal or a host variable%s", fns[fn].name, "");
		return NULL;
	}
	snip[0] = '\0';
	slots = be->lob->in_sql(bc, in->kind, snip, (int) sizeof snip);
	if (slots < 1 || slots > XLOB_SLOTS)
	{
		free(in->path);
		in->path = NULL;
		perr(c, "0A000", "\"%s\" is not available on this connection%s", fns[fn].name, "");
		return NULL;
	}
	in->nslots = slots;
	sb_adds(&c->o, snip);
	for (k = 0; k < slots; k++)
		add_mark(c, -1, pl->nin, k);
	pl->nin++;
	return end;
}

/* PUT_xLOB_TO_FILE(value, name [, option [, ccsid]]): p after "(" */
static const char *
do_put(pctx *c, int fn, const char *p, int col, const xbe_driver *be, void *bc, int stage)
{
	const char *a0[5],
			   *a1[5];
	int			n,
				i;
	const char *end = split_args(p, a0, a1, 5, &n);
	xlob_plan  *pl = c->pl;
	xlob_out   *out;
	char		pre[XLOB_SQLPRE],
				mid[XLOB_SQLMID],
				post[XLOB_SQLMID];
	const char *q;
	xbe_err		oe;
	int			order;

	if (end == NULL)
		return NULL;			/* no closing parenthesis: the caller says so */
	if (n < 2 || n > 4 || (n == 4 && fns[fn].kind != XLOB_CLOB))
	{
		perr(c, "42601", "\"%s\" takes a value, a file name%s", fns[fn].name,
			 fns[fn].kind == XLOB_CLOB ? ", and optionally the file option and a CCSID" :
			 ", and optionally the file option");
		return NULL;
	}
	if (pl->nout >= XLOB_MAXOUT)
	{
		perr(c, "54023", "\"%s\": too many files in one statement%s", fns[fn].name, "");
		return NULL;
	}
	for (i = 0; i < n; i++)
		trim(&a0[i], &a1[i]);
	if (a0[0] == a1[0] || a0[1] == a1[1])
	{
		perr(c, "42601", "\"%s\": the value or the file name is missing%s", fns[fn].name, "");
		return NULL;
	}
	out = &pl->out[pl->nout];
	memset(out, 0, sizeof *out);
	out->kind = fns[fn].kind;
	out->col = col;
	out->option = XLOB_OVERWRITE;
	out->ccsid = 1208;
	if (n >= 3)
	{
		long		v = number(a0[2], a1[2]);

		if (v != XLOB_NEW && v != XLOB_OVERWRITE && v != XLOB_APPEND)
		{
			perr(c, "42601", "\"%s\": the file option is 8 (new file), 16 (replace) or 32 (append)%s",
				 fns[fn].name, "");
			return NULL;
		}
		out->option = (int) v;
	}
	if (n == 4)
	{
		long		v = number(a0[3], a1[3]);

		if (v < 1 || v > 65534)
		{
			perr(c, "42601", "\"%s\": the fourth argument is the CCSID of the file%s", fns[fn].name, "");
			return NULL;
		}
		out->ccsid = (int) v;
	}
	pre[0] = mid[0] = post[0] = '\0';
	order = be->lob->out_sql(bc, out->kind, stage, pre, mid, post, &oe);
	if (order < 0)
	{
		if (!c->failed)
		{
			c->failed = 1;
			*c->e = oe;
		}
		return NULL;
	}
	if (pre[0] == '\0')
	{
		perr(c, "0A000", "\"%s\" is not available on this connection%s", fns[fn].name, "");
		return NULL;
	}
	{
		/*
		 * The host variables are numbered in the order of the statement
		 * as the program wrote it: value, then name.  A backend that wants
		 * the name first gets the two pieces, and their markers, swapped.
		 */
		size_t		at = c->o.n,
					vlen,
					nlen;
		int			m0 = pl->nmark,
					m1,
					m2;
		char	   *v,
				   *nm;

		copy_plain(c, a0[0], a1[0], fns[fn].name);
		vlen = c->o.n - at;
		m1 = pl->nmark;
		copy_plain(c, a0[1], a1[1], fns[fn].name);
		nlen = c->o.n - at - vlen;
		m2 = pl->nmark;
		if (c->failed || c->o.bad)
			return NULL;
		v = malloc(vlen + 1);
		nm = malloc(nlen + 1);
		if (v == NULL || nm == NULL)
		{
			free(v);
			free(nm);
			c->o.bad = 1;
			return NULL;
		}
		memcpy(v, c->o.s + at, vlen);
		v[vlen] = '\0';
		memcpy(nm, c->o.s + at + vlen, nlen);
		nm[nlen] = '\0';
		c->o.n = at;
		c->o.s[at] = '\0';
		sb_adds(&c->o, pre);
		sb_adds(&c->o, order ? nm : v);
		sb_adds(&c->o, mid);
		sb_adds(&c->o, order ? v : nm);
		sb_adds(&c->o, post);
		free(v);
		free(nm);
		if (order && m1 > m0 && m2 > m1 && !c->o.bad)
		{
			xlob_mark  *t = malloc(sizeof(xlob_mark) * (size_t) (m2 - m0));

			if (t == NULL)
			{
				c->o.bad = 1;
				return NULL;
			}
			memcpy(t, pl->mark + m1, sizeof(xlob_mark) * (size_t) (m2 - m1));
			memcpy(t + (m2 - m1), pl->mark + m0, sizeof(xlob_mark) * (size_t) (m1 - m0));
			memcpy(pl->mark + m0, t, sizeof(xlob_mark) * (size_t) (m2 - m0));
			free(t);
		}
	}
	if (c->failed)
		return NULL;
	/* a column without a name of its own gets the name of the function */
	q = end;
	for (;;)
	{
		const char *r;

		while (spacec((unsigned char) *q))
			q++;
		r = (q[0] == '-' && q[1] == '-') || (q[0] == '/' && q[1] == '*') ? run_end(q) : q;
		if (r == q)
			break;
		q = r;
	}
	{
		const char *w = q;

		while (namec((unsigned char) *w))
			w++;
		if (*q == '\0' || *q == ',' || *q == ';' || *q == ')' || (w > q && list_end(q, w)))
		{
			sb_adds(&c->o, " AS ");
			sb_adds(&c->o, fns[fn].name);
		}
	}
	pl->nout++;
	return end;
}

void
xlob_plan_free(xlob_plan *p)
{
	int			i;

	if (p == NULL)
		return;
	for (i = 0; i < p->nin; i++)
		free(p->in[i].path);
	free(p->mark);
	free(p);
}

/* does p start with kw (upper case), whatever its case */
static int
starts_ci(const char *p, const char *kw)
{
	for (; *kw; kw++, p++)
	{
		int			c = (unsigned char) *p;

		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (c != *kw)
			return 0;			/* also at the end of p */
	}
	return 1;
}

/* could the statement hold one of the functions at all */
static int
maybe(const char *sql)
{
	const char *p;

	for (p = sql; *p; p++)
		if (*p == '_' && (starts_ci(p, "_FROM_FILE") || starts_ci(p, "_TO_FILE")))
			return 1;
	return 0;
}

int
xlob_parse(const char *sql, const xbe_driver *be, void *bc, int stage,
		   char **out, xlob_plan **plan, xbe_err *e)
{
	pctx		c;
	const char *p = sql;
	int			depth = 0;
	int			phase = 0;		/* 0 before the select list, 1 in it, 2 after it */
	int			item = 0;		/* column of the select list, from 0 */
	int			empty = 1;		/* nothing of the present column was seen yet */
	int			star = 0;		/* a * among the columns so far */

	*out = NULL;
	*plan = NULL;
	if (sql == NULL || !maybe(sql))
		return 0;
	xlob_text(sql, be && be->text_flags ? be->text_flags(bc) : 0);
	memset(&c, 0, sizeof c);
	c.sql = sql;
	c.e = e;
	c.pl = calloc(1, sizeof(xlob_plan));
	if (c.pl == NULL)
		return 0;
	while (*p && !c.failed && !c.o.bad)
	{
		const char *r = run_end(p);

		if (r != p)
		{
			if (*p == '\'' || *p == '"' || *p == '`')
				empty = 0;
			sb_add(&c.o, p, (size_t) (r - p));
			p = r;
			continue;
		}
		r = marker_end(sql, p);
		if (r != p)
		{
			add_mark(&c, c.user++, 0, 0);
			sb_add(&c.o, p, (size_t) (r - p));
			empty = 0;
			p = r;
			continue;
		}
		if (*p == '(')
		{
			depth++;
			empty = 0;
		}
		else if (*p == ')')
		{
			if (depth > 0)
				depth--;
		}
		else if (depth == 0 && phase == 1 && *p == ',')
		{
			item++;
			empty = 1;
			sb_add(&c.o, p, 1);
			p++;
			continue;
		}
		else if (depth == 0 && phase == 1 && *p == '*')
		{
			if (empty || (p > sql && p[-1] == '.'))
				star = 1;
			empty = 0;
		}
		else if (namec((unsigned char) *p) && (p == sql || !namec((unsigned char) p[-1])))
		{
			const char *w = p;
			const char *open = NULL;
			int			fn;

			while (namec((unsigned char) *w))
				w++;
			fn = fn_at(sql, p, w, &open);
			if (fn >= 0)
			{
				if (be->lob == NULL)
				{
					perr(&c, "0A000", "\"%s\" is not available for %s connections", fns[fn].name, be->name);
					break;
				}
				if (!fns[fn].put)
					r = do_get(&c, fn, open, be, bc);
				else if (phase != 1 || depth != 0 || !empty)
				{
					perr(&c, "42601", "\"%s\" must be a column of the result of a query%s", fns[fn].name, "");
					break;
				}
				else if (star)
				{
					perr(&c, "42601", "\"%s\": name the columns in front of it, * is not possible there%s",
						 fns[fn].name, "");
					break;
				}
				else
					r = do_put(&c, fn, open, item, be, bc, stage);
				if (r == NULL)
				{
					if (!c.failed && !c.o.bad)
						perr(&c, "42601", "\"%s\": the closing parenthesis is missing%s", fns[fn].name, "");
					break;
				}
				empty = 0;
				p = r;
				continue;
			}
			if (depth == 0)
			{
				if (phase == 0 && word_is(p, w, "SELECT"))
				{
					phase = 1;
					item = 0;
					empty = 1;
					sb_add(&c.o, p, (size_t) (w - p));
					p = w;
					continue;
				}
				if (phase == 1 && item == 0 && empty && (word_is(p, w, "ALL") || word_is(p, w, "DISTINCT")))
				{
					const char *q = w;

					sb_add(&c.o, p, (size_t) (w - p));
					p = w;
					/* DISTINCT ON (...) belongs to the keyword, not to the first column */
					while (spacec((unsigned char) *q))
						q++;
					if ((q[0] == 'O' || q[0] == 'o') && (q[1] == 'N' || q[1] == 'n') && !namec((unsigned char) q[2]))
					{
						const char *g = q + 2;

						while (spacec((unsigned char) *g))
							g++;
						if (*g == '(')
						{
							const char *a0[1],
									   *a1[1];
							int			n;
							const char *ge = split_args(g + 1, a0, a1, 1, &n);

							if (ge)
							{
								copy_plain(&c, p, ge, "DISTINCT ON");
								p = ge;
							}
						}
					}
					continue;
				}
				if (phase == 1 && list_end(p, w))
					phase = 2;
				/* rows of the other queries would come without what tells where the value goes */
				if (phase == 2 && c.pl->nout > 0 &&
					(word_is(p, w, "UNION") || word_is(p, w, "EXCEPT") || word_is(p, w, "INTERSECT")))
				{
					perr(&c, "42601", "\"%s\" cannot be used in a query with UNION, EXCEPT or INTERSECT%s",
						 c.pl->out[0].kind == XLOB_CLOB ? "PUT_CLOB_TO_FILE" : "PUT_BLOB_TO_FILE", "");
					break;
				}
			}
			empty = 0;
			sb_add(&c.o, p, (size_t) (w - p));
			p = w;
			continue;
		}
		else if (!spacec((unsigned char) *p))
			empty = 0;
		sb_add(&c.o, p, 1);
		p++;
	}
	if (c.failed || c.o.bad)
	{
		if (!c.failed)
		{
			memset(e, 0, sizeof *e);
			snprintf(e->sqlstate, sizeof e->sqlstate, "57011");
			snprintf(e->msg, sizeof e->msg, "not enough memory to read the statement");
		}
		free(c.o.s);
		xlob_plan_free(c.pl);
		return -1;
	}
	if (c.pl->nin == 0 && c.pl->nout == 0)
	{
		free(c.o.s);
		xlob_plan_free(c.pl);
		return 0;
	}
	c.pl->nuser = c.user;
	*out = c.o.s;
	*plan = c.pl;
	return 1;
}

/* ---------------------------------------------------------------- the files */

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#define XLOB_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

/*
 * What went wrong with a file, as an error of the statement.  A file that
 * cannot be found or made is Db2's SQLCODE -452 (SQLSTATE 428A1) with the
 * reason codes of message SQL0452; no authority is -551, a file in use
 * -913.  Every text starts with "file": the core writes these to the job
 * log, where the name of the file can be read.
 */
static void
ferr(xbe_err *e, int en, const char *what, const char *path)
{
	const char *why;
	const char *state = "58030";

	switch (en)
	{
		case ENOENT:
		case ENOTDIR:
			why = "not found (reason 4)";
			state = "428A1";
			break;
		case EEXIST:
			why = "exists already (reason 5)";
			state = "428A1";
			break;
		case ENAMETOOLONG:
			why = "the name is too long (reason 2)";
			state = "428A1";
			break;
		case EINVAL:
			why = "the name is not valid (reason 1)";
			state = "428A1";
			break;
		case EACCES:
		case EPERM:
			why = "not authorized";
			state = "42501";
			break;
		case EISDIR:
			why = "is a directory (reason 1)";
			state = "428A1";
			break;
		case ENOSPC:
			why = "no space left";
			break;
		case EBUSY:
			why = "in use";
			state = "57033";
			break;
		default:
			why = NULL;
			break;
	}
	memset(e, 0, sizeof *e);
	e->native = en;
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", state);
	if (why)
		snprintf(e->msg, sizeof e->msg, "file \"%.300s\" %s: %s", path, what, why);
	else
		snprintf(e->msg, sizeof e->msg, "file \"%.300s\" %s: error %d", path, what, en);
}

#ifdef __ILEC400__
/* the name with its CCSID: the IFS takes it as it is, whatever the job CCSID */
typedef struct
{
	Qlg_Path_Name_T h;
	char		name[XLOB_PATHMAX];
} xqlg;

static void
qlg_set(xqlg *q, const char *path)
{
	size_t		l = strlen(path);

	memset(q, 0, sizeof *q);
	q->h.CCSID = 1208;
	q->h.Path_Type = 0;
	q->h.Path_Length = (int) l;
	q->h.Path_Name_Delimiter[0] = '/';
	memcpy(q->name, path, l);
}
#endif

/* does the file exist; its size */
static int
f_stat(const char *path, long long *size, int *isdir)
{
	struct stat st;
	int			rc;

#ifdef __ILEC400__
	xqlg		q;

	qlg_set(&q, path);
	rc = QlgStat((Qlg_Path_Name_T *) &q, &st);
#else
	rc = stat(path, &st);
#endif
	if (rc != 0)
		return -1;
	*size = (long long) st.st_size;
	*isdir = S_ISDIR(st.st_mode) ? 1 : 0;
	return 0;
}

static int
name_ok(const char *path, const char *what, xbe_err *e)
{
	size_t		l = path ? strlen(path) : 0;

	if (l == 0)
	{
		ferr(e, EINVAL, what, "");
		snprintf(e->msg, sizeof e->msg, "file name is empty or null (reason 1)");
		return -1;
	}
	if (l >= XLOB_PATHMAX)
	{
		ferr(e, ENAMETOOLONG, what, path);
		return -1;
	}
	return 0;
}

/*
 * Text read from a file is checked: it has to be UTF-8 (on IBM i the
 * system has converted it from the CCSID of the file), so that every server
 * is given the same and none stores what it cannot give back.  A character
 * may be cut by the end of a read; f remembers what has to follow.
 * Returns the offset of the first byte that cannot be, or -1.
 */
static long
utf8_check(xlob_file *f, const unsigned char *p, int n)
{
	int			i;

	for (i = 0; i < n; i++)
	{
		unsigned	c = p[i];

		if (f->u8need > 0)
		{
			if (c < f->u8lo || c > f->u8hi)
				return i;
			f->u8need--;
			f->u8lo = 0x80;
			f->u8hi = 0xBF;
			continue;
		}
		if (c < 0x80)
			continue;
		f->u8lo = 0x80;
		f->u8hi = 0xBF;
		if (c >= 0xC2 && c <= 0xDF)
			f->u8need = 1;
		else if (c >= 0xE0 && c <= 0xEF)
		{
			f->u8need = 2;
			if (c == 0xE0)
				f->u8lo = 0xA0; /* no overlong forms */
			if (c == 0xED)
				f->u8hi = 0x9F; /* no surrogates */
		}
		else if (c >= 0xF0 && c <= 0xF4)
		{
			f->u8need = 3;
			if (c == 0xF0)
				f->u8lo = 0x90;
			if (c == 0xF4)
				f->u8hi = 0x8F; /* nothing above U+10FFFF */
		}
		else
			return i;
	}
	return -1;
}

static int
src_read(void *ctx, char *buf, int len, xbe_err *e)
{
	xlob_file  *f = ctx;
	int			n;

	do
		n = (int) read(f->fd, buf, (size_t) len);
	while (n < 0 && errno == EINTR);
	if (n < 0)
	{
		ferr(e, errno, "cannot be read", f->path);
		return -1;
	}
	if (f->kind == XLOB_CLOB)
	{
		long		badat = n > 0 ? utf8_check(f, (const unsigned char *) buf, n) : f->u8need > 0 ? 0 : -1;

		if (badat >= 0)
		{
			memset(e, 0, sizeof *e);
			snprintf(e->sqlstate, sizeof e->sqlstate, "22021");
			snprintf(e->msg, sizeof e->msg, "file \"%.300s\" cannot be read: its text is not valid at byte %lld",
					 f->path, f->moved + badat + 1);
			return -1;
		}
	}
	f->moved += n;
	return n;
}

/* bytes the file gives; text is converted while it is read, so only reading tells */
static long long
src_size(void *ctx)
{
	xlob_file  *f = ctx;
	struct stat st;

	if (f->kind == XLOB_CLOB || !f->isopen || fstat(f->fd, &st) != 0)
		return -1;
	return (long long) st.st_size;
}

static int
src_rewind(void *ctx, xbe_err *e)
{
	xlob_file  *f = ctx;

	if (!f->isopen || lseek(f->fd, 0, SEEK_SET) != 0)
	{
		ferr(e, f->isopen ? errno : EBADF, "cannot be read", f->path);
		return -1;
	}
	f->moved = 0;
	f->u8need = 0;
	return 0;
}

int
xlob_src_open(xlob_file *f, const char *path, int kind, xlob_src *src, xbe_err *e)
{
	long long	size = 0;
	int			isdir = 0;

	memset(f, 0, sizeof *f);
	f->fd = -1;
	f->kind = kind;
	src->ctx = f;
	src->read = src_read;
	src->size = src_size;
	src->rewind = src_rewind;
	if (name_ok(path, "cannot be read", e) < 0)
		return -1;
	snprintf(f->path, sizeof f->path, "%s", path);
	if (f_stat(path, &size, &isdir) == 0 && isdir)
	{
		ferr(e, EISDIR, "cannot be read", path);
		return -1;
	}
#ifdef __ILEC400__
	{
		xqlg		q;

		qlg_set(&q, path);
		/* text comes converted from the CCSID of the file to UTF-8 */
		if (kind == XLOB_CLOB)
			f->fd = QlgOpen((Qlg_Path_Name_T *) &q, O_RDONLY | O_TEXTDATA | O_CCSID, 0, 1208);
		else
			f->fd = QlgOpen((Qlg_Path_Name_T *) &q, O_RDONLY);
	}
#else
	f->fd = open(path, O_RDONLY | O_LARGEFILE);
#endif
	if (f->fd < 0)
	{
		ferr(e, errno, "cannot be read", path);
		return -1;
	}
	f->isopen = 1;
	return 0;
}

void
xlob_src_close(xlob_file *f)
{
	if (f->isopen)
		close(f->fd);
	f->isopen = 0;
	f->fd = -1;
}

/* all of buf to the file */
static int
put_all(xlob_file *f, const char *buf, int len, xbe_err *e)
{
	while (len > 0)
	{
		int			n = (int) write(f->fd, buf, (size_t) len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
		{
			ferr(e, n < 0 ? errno : ENOSPC, "cannot be written", f->path);
			return -1;
		}
		buf += n;
		len -= n;
	}
	return 0;
}

/* whole UTF-8 characters to a text file */
static int
put_text(xlob_file *f, const char *buf, int len, xbe_err *e)
{
#ifdef __ILEC400__
	/* the file was opened for text: the system converts to its CCSID */
	return put_all(f, buf, len, e);
#else
	if (f->ccsid == 1208 || len == 0)
		return put_all(f, buf, len, e);
	else
	{
		size_t		size = (size_t) len * 4 + 16;
		char	   *t = malloc(size);
		int			n,
					rc;

		if (t == NULL)
		{
			ferr(e, ENOMEM, "cannot be written", f->path);
			return -1;
		}
		n = ile_conv(1208, f->ccsid, buf, (size_t) len, t, size);
		if (n < 0)
		{
			free(t);
			ferr(e, EINVAL, "cannot be written", f->path);
			snprintf(e->msg, sizeof e->msg, "file \"%.300s\": the text cannot be converted to CCSID %d",
					 f->path, f->ccsid);
			snprintf(e->sqlstate, sizeof e->sqlstate, "57017");
			return -1;
		}
		rc = put_all(f, t, n, e);
		free(t);
		return rc;
	}
#endif
}

/* bytes of the UTF-8 character that starts with c */
static int
u8len(unsigned char c)
{
	return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
}

static int
dst_write(void *ctx, const char *buf, int len, xbe_err *e)
{
	xlob_file  *f = ctx;
	int			cut,
				i;

	if (!f->isopen)
	{
		ferr(e, EBADF, "cannot be written", f->path);
		return -1;
	}
	if (f->kind != XLOB_CLOB)
		return put_all(f, buf, len, e);
	/* text: a character cut by the end of the piece before waits for its rest */
	while (f->ncarry > 0 && len > 0)
	{
		int			need = u8len(f->carry[0]);

		f->carry[f->ncarry++] = (unsigned char) *buf++;
		len--;
		if (f->ncarry >= need)
		{
			if (put_text(f, (const char *) f->carry, f->ncarry, e) < 0)
				return -1;
			f->ncarry = 0;
		}
	}
	if (len == 0)
		return 0;
	/* keep back a character that this piece does not finish */
	cut = len;
	for (i = 1; i <= 3 && i <= len; i++)
	{
		unsigned char ch = (unsigned char) buf[len - i];

		if ((ch & 0xC0) == 0x80)
			continue;			/* a continuation byte: look further back */
		if (u8len(ch) > i)
			cut = len - i;
		break;
	}
	if (cut > 0 && put_text(f, buf, cut, e) < 0)
		return -1;
	for (i = cut; i < len; i++)
		f->carry[f->ncarry++] = (unsigned char) buf[i];
	return 0;
}

static int
dst_open(void *ctx, const char *path, xbe_err *e)
{
	xlob_file  *f = ctx;
	int			flags = O_WRONLY | O_CREAT | O_LARGEFILE;
	long long	size = 0;
	int			isdir = 0;

	if (f->isopen)
	{
		ferr(e, EBUSY, "cannot be written", path);
		return -1;
	}
	if (name_ok(path, "cannot be written", e) < 0)
		return -1;
	snprintf(f->path, sizeof f->path, "%s", path);
	f->existed = f_stat(path, &size, &isdir) == 0;
	if (f->existed && isdir)
	{
		ferr(e, EISDIR, "cannot be written", path);
		return -1;
	}
	f->before = (f->existed && f->option == XLOB_APPEND) ? size : 0;
	flags |= f->option == XLOB_NEW ? O_EXCL : f->option == XLOB_APPEND ? O_APPEND : O_TRUNC;
#ifdef __ILEC400__
	{
		xqlg		q;

		qlg_set(&q, path);
		if (f->kind == XLOB_CLOB)
			/* a new file gets the CCSID asked for; the text given is UTF-8 and is converted */
			f->fd = QlgOpen((Qlg_Path_Name_T *) &q, flags | O_TEXTDATA | O_CCSID | O_TEXT_CREAT,
							XLOB_MODE, f->ccsid, 1208);
		else
			/* bytes: a new file is marked as not being text, as Db2 does for a BLOB file */
			f->fd = QlgOpen((Qlg_Path_Name_T *) &q, flags | O_CCSID, XLOB_MODE, 65535);
	}
#else
	f->fd = open(path, flags, XLOB_MODE);
#endif
	if (f->fd < 0)
	{
		ferr(e, errno, "cannot be written", path);
		return -1;
	}
	f->isopen = 1;
	return 0;
}

static void
dst_done(void *ctx)
{
	((xlob_file *) ctx)->complete = 1;
}

void
xlob_dst_init(xlob_file *f, int kind, int option, int ccsid, xlob_dst *dst)
{
	memset(f, 0, sizeof *f);
	f->fd = -1;
	f->kind = kind;
	f->option = option;
	f->ccsid = ccsid > 0 ? ccsid : 1208;
	dst->ctx = f;
	dst->open = dst_open;
	dst->write = dst_write;
	dst->done = dst_done;
}

int
xlob_dst_close(xlob_file *f, int ok, long long *bytes, xbe_err *e)
{
	int			rc = 0;
	struct stat st;

	if (bytes)
		*bytes = 0;
	if (!f->isopen)
		return 0;
	if (ok && f->ncarry > 0)
	{
		/* the value ended inside a character: it goes out as it is */
		if (put_all(f, (const char *) f->carry, f->ncarry, e) < 0)
			ok = 0, rc = -1;
		f->ncarry = 0;
	}
	if (ok)
	{
		if (fstat(f->fd, &st) == 0 && bytes)
			*bytes = (long long) st.st_size - f->before;
		if (close(f->fd) != 0)
		{
			ferr(e, errno, "cannot be written", f->path);
			rc = -1;
		}
	}
	else
	{
		/* leave nothing half written: cut an extended file back, remove the others */
		if (f->option == XLOB_APPEND && f->existed)
		{
			if (ftruncate(f->fd, (off_t) f->before) != 0)
			{
				/* nothing more can be done */
			}
			close(f->fd);
		}
		else
		{
			close(f->fd);
#ifdef __ILEC400__
			{
				xqlg		q;

				qlg_set(&q, f->path);
				QlgUnlink((Qlg_Path_Name_T *) &q);
			}
#else
			unlink(f->path);
#endif
		}
	}
	f->isopen = 0;
	f->fd = -1;
	return rc;
}

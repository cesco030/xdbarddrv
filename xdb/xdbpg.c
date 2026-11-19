/*
 * xdbpg.c - PostgreSQL backend of the XDB driver (libpq).
 *
 * Things that keep a session alive whatever is typed into it:
 *   - Long values (text, bytea, json ...) are cut on the server: a cursor
 *     over a result with such columns is declared on
 *         SELECT ..., left("col", n) AS "col", ... FROM (statement) xdb_q
 *     so that a 100 MB value never crosses the line to be cut to n bytes
 *     here.  The driver delivers at most text_len characters of such a
 *     column anyway.  long_values = full in xdb.conf switches this off.
 *   - When libpq gives a connection up (server gone, a result that does
 *     not fit into memory), the connection is opened again at once; the
 *     failed statement reports it, and a transaction that was open is
 *     reported as rolled back (SQLCODE -911) and started anew.
 *   - COPY ... STDIN/STDOUT is refused cleanly.
 *
 * Values between stream files and the server (xbe_lob, see xdblob.c): a
 * value up to lob_direct KB travels in the statement or in the row, as the
 * hex text of a bytea.  A larger one is staged in a large object of the
 * server, written or read in pieces of 1 MB (lo_put, lo_get) and removed
 * again in the same transaction, so that neither libpq nor the driver ever
 * holds more than a piece - an ILE heap allocation ends at 16 MB.
 */
#ifdef __ILEC400__
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libpq-fe.h"
#include "xdb_be.h"

#define LONGCAP 32000			/* the core never delivers more than this per column */
#define LOB_DIRECT_KB 4096		/* default of lob_direct */
#define LOB_DIRECT_MAX 6144		/* twice that as hex text must stay below 16 MB */
#define LOB_CHUNK (1024 * 1024)	/* piece of a staged value */
#define LOB_MAXSINK 32
#define LOB_MAXSTAGED 32

struct pgcur;

typedef struct
{
	PGconn	   *pg;
	struct pgcur *curs;			/* open cursors */
	int			epoch;			/* changes with every savepoint and rollback to it */
	int			seq;			/* cursor name counter */
	int			sp;				/* savepoint xdb_sp exists */
	int			gen;			/* incremented when the connection is replaced */
	int			n_reset;		/* times a lost connection was opened again here */
	int			n_reset_txn;	/* ... with a transaction open */
	time_t		t_reset;		/* the last time */
	int			in_txn;			/* BEGIN sent, no COMMIT/ROLLBACK yet */
	int			text_len;		/* characters delivered of an unbounded column */
	int			cut;			/* cut long values on the server */
	int			lob_direct;		/* bytes of a file value that travel in one piece */
	int			sink[LOB_MAXSINK];	/* file columns of the statement described next */
	int			nsink;
	unsigned	lob[LOB_MAXSTAGED];	/* large objects staged for the statement that runs */
	int			nlob;
	int			lob_own;		/* a transaction was begun for them */
	int			n_notice;		/* notices and warnings the server has sent */
	char		notice[200];	/* the last one */
} pgc;

typedef struct
{
	char	   *wsql;			/* the statement wrapped to cut long values, or NULL */
	int			has_long;		/* result has columns of unbounded length */
	int			runs;			/* no query: it returns its rows when it is run (run_at_open) */
	int			ncols;			/* columns the server described */
} pgstmt;

typedef struct pgcur
{
	struct pgcur *next;
	char		name[24];
	int			gen;			/* connection the cursor lives on */
	int			epoch;			/* savepoint epoch it was declared in, -1 outside a transaction */
	int			dead;			/* gone on the server (rollback to a savepoint) */
	int			small;			/* long values come whole: fetch few rows at a time */
	PGresult   *res;			/* no server cursor: the rows of a statement that ran at the open */
	int			pos;			/* the next of them to hand out */
} pgcur;

static void
set_err(pgc *c, PGresult *r, xbe_err *e)
{
	const char *st = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : NULL;
	const char *m = r ? PQresultErrorField(r, PG_DIAG_MESSAGE_PRIMARY) : NULL;

	memset(e, 0, sizeof *e);
	if (m == NULL || m[0] == '\0')
		m = PQerrorMessage(c->pg);
	snprintf(e->msg, sizeof e->msg, "%s", m ? m : "PostgreSQL error");
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", (st && strlen(st) == 5) ? st : "58004");
	if (PQstatus(c->pg) != CONNECTION_OK)
		snprintf(e->sqlstate, sizeof e->sqlstate, "08006");
	else if (st && strcmp(st, "25001") == 0)
	{
		/*
		 * CREATE DATABASE, VACUUM and the like: refused because a transaction
		 * is open, before anything was done.  Told by the routine of the
		 * server that raised the error (its name before version 11 is the
		 * second one), not by the text, which is in the language of the
		 * server; the same SQLSTATE comes from other places too (SET
		 * TRANSACTION after the first query), and those are not meant.
		 */
		const char *fn = PQresultErrorField(r, PG_DIAG_SOURCE_FUNCTION);

		if (fn && (strcmp(fn, "PreventInTransactionBlock") == 0 || strcmp(fn, "PreventTransactionChain") == 0))
			e->alone = 1;
	}
}

static int	run(pgc *c, const char *sql, xbe_err *e);

static void
session_setup(pgc *c)
{
	run(c, "SET datestyle TO ISO; SET intervalstyle TO postgres; SET bytea_output TO hex", NULL);
}

/*
 * After a failed call: if libpq has given the connection up, open it again
 * so that the session can go on.  What was lost is said in the error.
 */
static void
recover(pgc *c, xbe_err *e)
{
	char		orig[200];
	int			mem;

	if (PQstatus(c->pg) == CONNECTION_OK)
		return;
	snprintf(orig, sizeof orig, "%s", e->msg);
	if (strchr(orig, '\n'))
		*strchr(orig, '\n') = '\0';
	mem = strstr(orig, "memory") != NULL;
	PQreset(c->pg);
	c->gen++;
	c->sp = 0;
	if (PQstatus(c->pg) != CONNECTION_OK)
	{
		snprintf(e->sqlstate, sizeof e->sqlstate, "08006");
		snprintf(e->msg, sizeof e->msg, "connection lost, reconnect failed: %s", orig);
		return;
	}
	session_setup(c);
	c->n_reset++;
	c->t_reset = time(NULL);
	/* out of memory is PostgreSQL's 53200, a lost line 08006 */
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", mem ? "53200" : "08006");
	if (c->in_txn)
	{
		run(c, "BEGIN", NULL);
		e->rolled_back = 1;
		c->n_reset_txn++;
	}
	snprintf(e->msg, sizeof e->msg, "%s; reconnected%s: %s",
			 mem ? "result too large for memory" : "connection lost",
			 c->in_txn ? ", work rolled back" : "", orig);
}

/* before a call: a connection that could not be re-established earlier */
static int
alive(pgc *c, xbe_err *e)
{
	if (PQstatus(c->pg) == CONNECTION_OK)
		return 0;
	memset(e, 0, sizeof *e);
	snprintf(e->msg, sizeof e->msg, "%s", PQerrorMessage(c->pg));
	recover(c, e);
	if (PQstatus(c->pg) != CONNECTION_OK)
		return -1;
	if (!e->rolled_back)
		memset(e, 0, sizeof *e);	/* nothing was pending: carry on */
	return e->rolled_back ? -1 : 0;
}

/* a statement left the connection in COPY mode: get out of it */
static void
end_copy(pgc *c, PGresult *r)
{
	ExecStatusType st = PQresultStatus(r);
	PGresult   *x;

	if (st == PGRES_COPY_IN || st == PGRES_COPY_BOTH)
		PQputCopyEnd(c->pg, "COPY from the client is not supported");
	if (st == PGRES_COPY_OUT || st == PGRES_COPY_BOTH)
	{
		char	   *buf;

		while (PQgetCopyData(c->pg, &buf, 0) > 0)
			PQfreemem(buf);
	}
	while ((x = PQgetResult(c->pg)) != NULL)
		PQclear(x);
}

static int
is_copy(PGresult *r)
{
	ExecStatusType st = PQresultStatus(r);

	return st == PGRES_COPY_IN || st == PGRES_COPY_OUT || st == PGRES_COPY_BOTH;
}

static int
run(pgc *c, const char *sql, xbe_err *e)
{
	PGresult   *r = PQexec(c->pg, sql);
	int			ok = (PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK);

	if (!ok && e)
		set_err(c, r, e);
	PQclear(r);
	return ok ? 0 : -1;
}

/*
 * Notices and warnings of the server ("relation x already exists,
 * skipping").  libpq would write them to stderr, which on IBM i is a spooled
 * file in a batch job and a display of its own in an interactive one.  They
 * are counted, and the last one is kept for XDB CONNECTION INFO.
 */
static void
pg_notice(void *arg, const char *message)
{
	pgc		   *c = arg;
	size_t		n;

	snprintf(c->notice, sizeof c->notice, "%s", message ? message : "");
	n = strlen(c->notice);
	while (n > 0 && (c->notice[n - 1] == '\n' || c->notice[n - 1] == '\r' || c->notice[n - 1] == ' '))
		c->notice[--n] = '\0';
	c->n_notice++;
}

static void *
pg_connect(const xbe_opts *o, xbe_err *e)
{
	pgc		   *c = calloc(1, sizeof(pgc));
	const char *kw[5],
			   *kv[5];
	char		info[2100];
	int			k = 0;

	memset(e, 0, sizeof *e);
	if (c == NULL)
		return NULL;
	snprintf(info, sizeof info, "%s client_encoding=UTF8", o->conninfo);
	kw[k] = "dbname";
	kv[k++] = info;
	if (o->user && o->user[0])
	{
		kw[k] = "user";
		kv[k++] = o->user;
	}
	if (o->password && o->password[0])
	{
		kw[k] = "password";
		kv[k++] = o->password;
	}
	kw[k] = NULL;
	kv[k] = NULL;
	c->pg = PQconnectdbParams(kw, kv, 1);
	if (PQstatus(c->pg) != CONNECTION_OK)
	{
		const char *m = PQerrorMessage(c->pg);
		const char *f = strstr(m, "failed: ");

		snprintf(e->msg, sizeof e->msg, "%s", f ? f + 8 : m);
		if (strchr(e->msg, '\n'))
			*strchr(e->msg, '\n') = '\0';
		snprintf(e->sqlstate, sizeof e->sqlstate, "%s",
				 (strstr(e->msg, "authentication") || strstr(e->msg, "password")) ? "28000" : "08001");
		PQfinish(c->pg);
		free(c);
		return NULL;
	}
	PQsetNoticeProcessor(c->pg, pg_notice, c);
	session_setup(c);
	c->text_len = 1000;
	c->cut = 1;
	if (o->conf)
	{
		char		v[64];

		if (o->conf(o->rdb, "text_len", v, sizeof v) == 0 && atoi(v) > 0)
			c->text_len = atoi(v);
		if (o->conf(o->rdb, "long_values", v, sizeof v) == 0 && strcmp(v, "full") == 0)
			c->cut = 0;
	}
	if (c->text_len > LONGCAP)
		c->text_len = LONGCAP;
	{
		char		v[64];
		int			kb = LOB_DIRECT_KB;

		if (o->conf && o->conf(o->rdb, "lob_direct", v, sizeof v) == 0 && v[0] >= '0' && v[0] <= '9')
			kb = atoi(v);
		if (kb > LOB_DIRECT_MAX)
			kb = LOB_DIRECT_MAX;
		c->lob_direct = kb * 1024;
	}
	return c;
}

static void
pg_disconnect(void *vc)
{
	pgc		   *c = vc;

	PQfinish(c->pg);
	free(c);
}

static int
pg_version(void *vc)
{
	int			v = PQserverVersion(((pgc *) vc)->pg);

	/*
	 * 160015 is 16.15: since version 10 the number is the major version and
	 * the minor release.  90602 is 9.6.2: before, two numbers made the
	 * major version, and they are what is told here (9.6).
	 */
	if (v >= 100000)
		return (v / 10000) * 100 + (v % 10000) % 100;
	return (v / 10000) * 100 + (v / 100) % 100;
}

static int
pg_exec(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
		long long *rows, xbe_err *e)
{
	pgc		   *c = vc;
	PGresult   *r;
	int			rc = 0;

	if (rows)
		*rows = 0;
	if (alive(c, e) < 0)
		return -1;
	if (np > 0)
		r = PQexecParams(c->pg, sql, np, NULL, (const char *const *) pv, NULL, NULL, 0);
	else
		r = PQexec(c->pg, sql);
	if (is_copy(r))
	{
		end_copy(c, r);
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "0A000");
		snprintf(e->msg, sizeof e->msg, "COPY to or from the client is not supported here");
		rc = -1;
	}
	else if (PQresultStatus(r) != PGRES_COMMAND_OK && PQresultStatus(r) != PGRES_TUPLES_OK)
	{
		set_err(c, r, e);
		recover(c, e);
		rc = -1;
	}
	else if (rows)
		*rows = atoll(PQcmdTuples(r));
	PQclear(r);
	return rc;
}

/* first keyword of the statement (after blanks and comments) is kw? */
static int
starts_with(const char *sql, const char *kw)
{
	size_t		i,
				l = strlen(kw);

	for (;;)
	{
		while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r')
			sql++;
		if (sql[0] == '/' && sql[1] == '*')
		{
			const char *x = strstr(sql + 2, "*/");

			if (x == NULL)
				return 0;
			sql = x + 2;
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
		if ((sql[i] & ~0x20) != kw[i])
			return 0;
	return !((sql[l] >= 'A' && sql[l] <= 'Z') || (sql[l] >= 'a' && sql[l] <= 'z') ||
			 (sql[l] >= '0' && sql[l] <= '9') || sql[l] == '_');
}

/* appends a quoted identifier, returns the new end */
static char *
quote_ident(char *o, const char *name)
{
	*o++ = '"';
	for (; *name; name++)
	{
		if (*name == '"')
			*o++ = '"';
		*o++ = *name;
	}
	*o++ = '"';
	return o;
}

/*
 * The statement with its long columns cut on the server, or NULL when
 * that is not possible (not a plain query, column names missing or not
 * unique) or not needed.  d is the description of the result.
 */
/* is result column i one that carries a file value (announced by the core) */
static int
is_sink(const int *sink, int nsink, int i)
{
	int			k;

	for (k = 0; k < nsink; k++)
		if (sink[k] == i)
			return 1;
	return 0;
}

static char *
wrap_long(pgc *c, const char *sql, PGresult *d, int *has_long, const int *sink, int nsink)
{
	int			n = PQnfields(d),
				i,
				j,
				nlong = 0;
	size_t		need = strlen(sql) + 64;
	char	   *w,
			   *o;

	*has_long = 0;
	for (i = 0; i < n; i++)
	{
		unsigned	t = (unsigned) PQftype(d, i);
		int			mod = PQfmod(d, i);
		int			islong;

		/* types with a size of their own are never long */
		islong = !(t == 16 || t == 20 || t == 21 || t == 23 || t == 26 || t == 700 || t == 701 ||
				   t == 1700 || t == 1082 || t == 1083 || t == 1114 || t == 1184 || t == 1042 ||
				   t == 19 || t == 2950) &&
			!(t == 1043 && mod >= 4 && mod - 4 <= LONGCAP) && !is_sink(sink, nsink, i);
		if (islong)
			nlong++;
		need += strlen(PQfname(d, i)) * 4 + 96;
	}
	if (nlong == 0)
		return NULL;
	*has_long = 1;
	if (!c->cut || !(starts_with(sql, "SELECT") || starts_with(sql, "WITH") ||
					 starts_with(sql, "VALUES") || starts_with(sql, "TABLE")))
		return NULL;
	for (i = 0; i < n; i++)
	{
		if (PQfname(d, i)[0] == '\0')
			return NULL;
		for (j = 0; j < i; j++)
			if (strcmp(PQfname(d, i), PQfname(d, j)) == 0)
				return NULL;
	}
	w = malloc(need);
	if (w == NULL)
		return NULL;
	o = w + sprintf(w, "SELECT ");
	for (i = 0; i < n; i++)
	{
		unsigned	t = (unsigned) PQftype(d, i);
		int			mod = PQfmod(d, i);
		int			islong = !(t == 16 || t == 20 || t == 21 || t == 23 || t == 26 || t == 700 || t == 701 ||
							   t == 1700 || t == 1082 || t == 1083 || t == 1114 || t == 1184 || t == 1042 ||
							   t == 19 || t == 2950) &&
			!(t == 1043 && mod >= 4 && mod - 4 <= LONGCAP) && !is_sink(sink, nsink, i);
		int			cap = (t == 1043 && mod >= 4) ? LONGCAP : c->text_len;

		if (i)
			*o++ = ',';
		if (!islong)
		{
			o = quote_ident(o, PQfname(d, i));
			continue;
		}
		if (t == 17)			/* bytea: the hex form of the first bytes */
		{
			o += sprintf(o, "E'\\\\x' || encode(substring(");
			o = quote_ident(o, PQfname(d, i));
			o += sprintf(o, " from 1 for %d), 'hex') AS ", cap / 2);
		}
		else
		{
			o += sprintf(o, "left(");
			o = quote_ident(o, PQfname(d, i));
			o += sprintf(o, "%s, %d) AS ", (t == 25 || t == 1043) ? "" : "::text", cap);
		}
		o = quote_ident(o, PQfname(d, i));
	}
	sprintf(o, " FROM (\n%s\n) xdb_q", sql);
	return w;
}

/* does the statement hold the word (letters in upper case), outside of names it is part of? */
static int
has_word(const char *sql, const char *word)
{
	size_t		i,
				l = strlen(word);
	const char *p;

	for (p = sql; *p; p++)
	{
		int			before = p > sql ? (unsigned char) p[-1] : ' ';

		if ((before >= 'A' && before <= 'Z') || (before >= 'a' && before <= 'z') ||
			(before >= '0' && before <= '9') || before == '_' || before == '"')
			continue;
		for (i = 0; i < l; i++)
			if ((p[i] & ~0x20) != word[i])
				break;
		if (i == l && !((p[l] >= 'A' && p[l] <= 'Z') || (p[l] >= 'a' && p[l] <= 'z') ||
						(p[l] >= '0' && p[l] <= '9') || p[l] == '_' || p[l] == '"'))
			return 1;
	}
	return 0;
}

/*
 * Is the statement one that returns its rows when it is run and that the
 * server does not take in DECLARE CURSOR?  These are the statements that
 * are no query and can return rows all the same: the call of a procedure
 * (a row with its OUT and INOUT parameters), INSERT, UPDATE, DELETE and
 * MERGE with RETURNING, FETCH from a cursor that a procedure opened, the
 * EXECUTE of a statement prepared on the server, SHOW and EXPLAIN.
 *
 * A statement that begins with WITH is a query, unless its WITH clause
 * changes data ("WITH x AS (INSERT ... RETURNING ...) SELECT ..."), which
 * DECLARE CURSOR does not take either.  The server tells: when the
 * statement names such a word, the DECLARE is prepared - not run.  Inside
 * a transaction its refusal must not cost the transaction.
 */
static int
runs_when_opened(pgc *c, const char *sql)
{
	static const char *const kw[] = {"CALL", "INSERT", "UPDATE", "DELETE", "MERGE", "FETCH",
		"EXECUTE", "SHOW", "EXPLAIN", NULL};
	int			i,
				intx,
				refused;
	char	   *q;
	PGresult   *v;

	for (i = 0; kw[i]; i++)
		if (starts_with(sql, kw[i]))
			return 1;
	if (!starts_with(sql, "WITH") ||
		!(has_word(sql, "INSERT") || has_word(sql, "UPDATE") || has_word(sql, "DELETE") || has_word(sql, "MERGE")))
		return 0;
	q = malloc(strlen(sql) + 48);
	if (q == NULL)
		return 0;
	sprintf(q, "DECLARE xdb_probe CURSOR FOR %s", sql);
	intx = PQtransactionStatus(c->pg) == PQTRANS_INTRANS;
	if (intx)
		run(c, "SAVEPOINT xdb_w", NULL);
	v = PQprepare(c->pg, "", q, 0, NULL);
	free(q);
	refused = PQresultStatus(v) != PGRES_COMMAND_OK && PQstatus(c->pg) == CONNECTION_OK &&
		PQresultErrorField(v, PG_DIAG_SQLSTATE) != NULL &&
		strcmp(PQresultErrorField(v, PG_DIAG_SQLSTATE), "0A000") == 0;
	if (intx && PQresultStatus(v) != PGRES_COMMAND_OK)
		run(c, "ROLLBACK TO SAVEPOINT xdb_w", NULL);
	PQclear(v);
	if (intx)
		run(c, "RELEASE SAVEPOINT xdb_w", NULL);
	return refused;
}

/* column i of a result or of a description, as the core wants it */
static void
col_of(const PGresult *r, int i, xbe_col *x)
{
	int			mod = PQfmod(r, i);

	snprintf(x->name, sizeof x->name, "%s", PQfname(r, i));
	switch ((unsigned) PQftype(r, i))
	{
		case 21:
			x->kind = XK_SMALLINT;
			break;
		case 16:
			x->kind = XK_BOOL;
			break;
		case 23:
		case 26:
			x->kind = XK_INTEGER;
			break;
		case 20:
			x->kind = XK_BIGINT;
			break;
		case 700:
		case 701:
			x->kind = XK_DOUBLE;
			break;
		case 1700:
			x->kind = XK_DECIMAL;
			if (mod >= 4)
			{
				x->prec = ((mod - 4) >> 16) & 0xFFFF;
				x->scale = (mod - 4) & 0xFFFF;
			}
			break;
		case 1082:
			x->kind = XK_DATE;
			break;
		case 1083:
			x->kind = XK_TIME;
			break;
		case 1114:
		case 1184:
			x->kind = XK_TIMESTAMP;
			break;
		case 1042:
			x->kind = XK_CHAR;
			x->len = mod >= 4 ? mod - 4 : 0;
			break;
		case 19:
			x->kind = XK_VARCHAR;
			x->len = 63;
			break;
		case 2950:			/* uuid */
			x->kind = XK_CHAR;
			x->len = 36;
			break;
		default:
			x->kind = XK_VARCHAR;
			x->len = ((unsigned) PQftype(r, i) == 1043 && mod >= 4) ? mod - 4 : 0;
			break;
	}
}

static int
pg_describe(void *vc, const char *sql, xbe_col **cols, int *ncols, void **stmt, xbe_err *e)
{
	pgc		   *c = vc;
	PGresult   *r;
	pgstmt	   *st;
	int			i,
				n;
	int			sink[LOB_MAXSINK];
	int			nsink = c->nsink;

	/* the file columns announced for this statement, and for no other */
	memcpy(sink, c->sink, sizeof sink);
	c->nsink = 0;
	*cols = NULL;
	*ncols = 0;
	*stmt = NULL;
	if (alive(c, e) < 0)
		return -1;
	r = PQprepare(c->pg, "", sql, 0, NULL);
	if (PQresultStatus(r) != PGRES_COMMAND_OK)
	{
		set_err(c, r, e);
		PQclear(r);
		recover(c, e);
		return -1;
	}
	PQclear(r);
	r = PQdescribePrepared(c->pg, "");
	if (PQresultStatus(r) != PGRES_COMMAND_OK)
	{
		set_err(c, r, e);
		PQclear(r);
		recover(c, e);
		return -1;
	}
	n = PQnfields(r);
	if (n > 0)
		*cols = calloc(n, sizeof(xbe_col));
	for (i = 0; i < n && *cols; i++)
		col_of(r, i, &(*cols)[i]);
	*ncols = n;
	st = calloc(1, sizeof(pgstmt));
	if (st && n > 0)
	{
		st->wsql = wrap_long(c, sql, r, &st->has_long, sink, nsink);
		if (st->wsql)
		{
			/*
			 * Let the server check the wrapped form now (a statement that is
			 * only valid at the top level would fail in it); inside a
			 * transaction a failure must not cost the transaction.
			 */
			int			intx = PQtransactionStatus(c->pg) == PQTRANS_INTRANS;
			PGresult   *v;

			if (intx)
				run(c, "SAVEPOINT xdb_w", NULL);
			v = PQprepare(c->pg, "", st->wsql, 0, NULL);
			if (PQresultStatus(v) != PGRES_COMMAND_OK || PQstatus(c->pg) != CONNECTION_OK)
			{
				free(st->wsql);
				st->wsql = NULL;
				if (intx)
					run(c, "ROLLBACK TO SAVEPOINT xdb_w", NULL);
			}
			PQclear(v);
			if (intx)
				run(c, "RELEASE SAVEPOINT xdb_w", NULL);
		}
	}
	PQclear(r);
	if (st)
	{
		st->runs = runs_when_opened(c, sql);
		st->ncols = n;
	}
	*stmt = st;
	return 0;
}

static void
pg_stmt_free(void *vc, void *stmt)
{
	pgstmt	   *st = stmt;

	if (st)
	{
		free(st->wsql);
		free(st);
	}
}

/*
 * The cursor of a statement that is no query (see runs_when_opened): the
 * statement runs now, once, and its rows wait here until they are fetched.
 * Nothing of it is left on the server, so neither the end of a transaction
 * nor a new connection takes the rows away.  A statement that returned no
 * rows leaves a cursor of no columns, which the core refuses.
 */
static int
run_at_open(pgc *c, const char *sql, int np, char **pv, void **cur, xbe_err *e)
{
	pgcur	   *k = calloc(1, sizeof(pgcur));
	PGresult   *r;

	if (k == NULL)
		return -1;
	r = PQexecParams(c->pg, sql, np, NULL, (const char *const *) pv, NULL, NULL, 0);
	if (is_copy(r))
	{
		end_copy(c, r);
		PQclear(r);
		free(k);
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "0A000");
		snprintf(e->msg, sizeof e->msg, "COPY to or from the client is not supported here");
		return -1;
	}
	if (PQresultStatus(r) != PGRES_COMMAND_OK && PQresultStatus(r) != PGRES_TUPLES_OK)
	{
		set_err(c, r, e);
		PQclear(r);
		free(k);
		recover(c, e);
		return -1;
	}
	k->res = r;
	k->gen = c->gen;
	k->epoch = -1;
	k->next = c->curs;
	c->curs = k;
	*cur = k;
	return 0;
}

/* the columns of the rows of a statement that ran at the open, as they came */
static int
pg_cursor_cols(void *vc, void *cur, xbe_col **cols, int *ncols, xbe_err *e)
{
	pgcur	   *k = cur;
	int			i,
				n;

	*cols = NULL;
	*ncols = 0;
	if (k == NULL || k->res == NULL || (n = PQnfields(k->res)) <= 0)
		return 0;
	*cols = calloc((size_t) n, sizeof(xbe_col));
	if (*cols == NULL)
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "57011");
		snprintf(e->msg, sizeof e->msg, "not enough memory for the columns of the statement");
		return -1;
	}
	for (i = 0; i < n; i++)
		col_of(k->res, i, &(*cols)[i]);
	*ncols = n;
	return 0;
}

static int
pg_open(void *vc, void *stmt, const char *sql, int np, char **pv, const int *pk,
		int hold, void **cur, xbe_err *e)
{
	pgc		   *c = vc;
	pgstmt	   *st = stmt;
	pgcur	   *k;
	char	   *q;
	PGresult   *r;

	if (alive(c, e) < 0)
		return -1;
	if (st ? st->runs : runs_when_opened(c, sql))
	{
		/*
		 * The server has said whether the statement returns rows.  One that
		 * returns none (a procedure without OUT parameters, an INSERT
		 * without RETURNING) is not run for an OPEN that must fail.  FETCH
		 * is the exception: prepared before its cursor exists it is
		 * described with no columns and returns rows all the same.
		 */
		if (st && st->ncols == 0 && !starts_with(sql, "FETCH"))
		{
			memset(e, 0, sizeof *e);
			snprintf(e->sqlstate, sizeof e->sqlstate, "07005");
			snprintf(e->msg, sizeof e->msg, "the statement returns no rows: there is nothing to fetch, and it was not run");
			return -1;
		}
		return run_at_open(c, sql, np, pv, cur, e);
	}
	if (st && st->wsql)
		sql = st->wsql;
	k = calloc(1, sizeof(pgcur));
	q = malloc(strlen(sql) + 96);
	if (k == NULL || q == NULL)
	{
		free(k);
		free(q);
		return -1;
	}
	snprintf(k->name, sizeof k->name, "xdb_c%d", ++c->seq);
	k->gen = c->gen;
	k->epoch = (!hold && c->sp) ? c->epoch : -1;
	k->small = st && st->has_long && st->wsql == NULL;
	sprintf(q, "DECLARE %s CURSOR %sFOR %s", k->name, hold ? "WITH HOLD " : "", sql);
	r = PQexecParams(c->pg, q, np, NULL, (const char *const *) pv, NULL, NULL, 0);
	free(q);
	if (PQresultStatus(r) != PGRES_COMMAND_OK)
	{
		set_err(c, r, e);
		PQclear(r);
		free(k);
		recover(c, e);
		return -1;
	}
	PQclear(r);
	k->next = c->curs;
	c->curs = k;
	*cur = k;
	return 0;
}

static void
pg_release(xbe_batch *b)
{
	if (b->owner)
		PQclear((PGresult *) b->owner);
	free(b->val);
	memset(b, 0, sizeof *b);
}

static int
pg_fetch(void *vc, void *cur, int maxrows, xbe_batch *b, xbe_err *e)
{
	pgc		   *c = vc;
	pgcur	   *k = cur;
	char		q[80];
	PGresult   *r;
	int			i,
				j;

	memset(b, 0, sizeof *b);
	if (k->res)
	{
		/* the rows are here already: the batch points into them and owns nothing but its table */
		int			left = PQntuples(k->res) - k->pos;

		b->nrows = left < maxrows ? left : maxrows;
		b->ncols = PQnfields(k->res);
		b->eof = b->nrows == left;
		b->release = pg_release;
		if (b->nrows > 0 && b->ncols > 0)
		{
			b->val = malloc(sizeof(char *) * (size_t) b->nrows * (size_t) b->ncols);
			if (b->val == NULL)
			{
				memset(b, 0, sizeof *b);
				return -1;
			}
			for (i = 0; i < b->nrows; i++)
				for (j = 0; j < b->ncols; j++)
					b->val[i * b->ncols + j] = PQgetisnull(k->res, k->pos + i, j) ? NULL :
						PQgetvalue(k->res, k->pos + i, j);
		}
		k->pos += b->nrows;
		return 0;
	}
	if (k->gen != c->gen || k->dead)
	{
		memset(e, 0, sizeof *e);
		snprintf(e->sqlstate, sizeof e->sqlstate, "24501");
		snprintf(e->msg, sizeof e->msg, k->dead ? "the cursor was closed by the failure of a statement: open it again" :
				 "the cursor was lost with the connection: open it again");
		return -1;
	}
	/* rows that may carry long values whole are read a few at a time */
	if (k->small && maxrows > 2)
		maxrows = 2;
	snprintf(q, sizeof q, "FETCH FORWARD %d FROM %s", maxrows, k->name);
	r = PQexec(c->pg, q);
	if (PQresultStatus(r) != PGRES_TUPLES_OK)
	{
		set_err(c, r, e);
		PQclear(r);
		recover(c, e);
		return -1;
	}
	b->nrows = PQntuples(r);
	b->ncols = PQnfields(r);
	b->eof = b->nrows < maxrows;
	b->owner = r;
	b->release = pg_release;
	if (b->nrows > 0)
	{
		b->val = malloc(sizeof(char *) * (size_t) b->nrows * (size_t) b->ncols);
		if (b->val == NULL)
		{
			pg_release(b);
			return -1;
		}
		for (i = 0; i < b->nrows; i++)
			for (j = 0; j < b->ncols; j++)
				b->val[i * b->ncols + j] = PQgetisnull(r, i, j) ? NULL : PQgetvalue(r, i, j);
	}
	return 0;
}

static void
pg_close(void *vc, void *cur)
{
	pgc		   *c = vc;
	pgcur	   *k = cur;
	pgcur	  **pp;
	char		q[64];

	for (pp = &c->curs; *pp; pp = &(*pp)->next)
		if (*pp == k)
		{
			*pp = k->next;
			break;
		}
	if (k->res)
		PQclear(k->res);		/* it ran at the open: nothing of it is on the server */
	else if (k->gen == c->gen && !k->dead && PQstatus(c->pg) == CONNECTION_OK &&
			 PQtransactionStatus(c->pg) != PQTRANS_INERROR)
	{
		snprintf(q, sizeof q, "CLOSE %s", k->name);
		run(c, q, NULL);
	}
	free(k);
}

static int	pg_txn(void *vc, int op, xbe_err *e);

static int
pg_txn(void *vc, int op, xbe_err *e)
{
	pgc		   *c = vc;
	int			rc;
	int			answered = 0;	/* the server answered the COMMIT with an error */

	memset(e, 0, sizeof *e);
	switch (op)
	{
		case XT_BEGIN:
			c->sp = 0;
			if (alive(c, e) < 0 && !e->rolled_back)
				return -1;
			c->in_txn = 1;
			rc = run(c, "BEGIN", e);
			break;
		case XT_COMMIT:
			c->sp = 0;
			{
				PGresult   *r = PQexec(c->pg, "COMMIT");

				rc = 0;
				if (PQresultStatus(r) != PGRES_COMMAND_OK)
				{
					answered = r != NULL && PQresultErrorField(r, PG_DIAG_SQLSTATE) != NULL;
					set_err(c, r, e);
					rc = -1;
				}
				else if (strcmp(PQcmdStatus(r), "COMMIT") != 0)
				{
					/*
					 * A transaction in which a statement failed and was not
					 * taken back to a savepoint: the server answers the
					 * COMMIT with ROLLBACK, without an error.
					 */
					snprintf(e->sqlstate, sizeof e->sqlstate, "25P02");
					snprintf(e->msg, sizeof e->msg,
							 "a statement of the transaction had failed, the server has rolled the transaction back");
					rc = -1;
				}
				PQclear(r);
			}
			c->in_txn = 0;
			break;
		case XT_ROLLBACK:
			c->sp = 0;
			rc = run(c, "ROLLBACK", e);
			c->in_txn = 0;
			break;
		case XT_SAVEPOINT:
			/* a transaction left in the failed state is taken back to the last savepoint first */
			if (c->sp && PQtransactionStatus(c->pg) == PQTRANS_INERROR)
				pg_txn(c, XT_ROLLBACK_TO, e);
			rc = run(c, c->sp ? "RELEASE SAVEPOINT xdb_sp; SAVEPOINT xdb_sp" : "SAVEPOINT xdb_sp", e);
			c->sp = 1;
			c->epoch++;
			break;
		case XT_ROLLBACK_TO:
			rc = c->sp ? run(c, "ROLLBACK TO SAVEPOINT xdb_sp", e) : 0;
			if (c->sp)
			{
				/* the server has closed the cursors declared since the savepoint */
				pgcur	   *k;

				for (k = c->curs; k; k = k->next)
					if (k->epoch == c->epoch)
						k->dead = 1;
				c->epoch++;
			}
			break;
		default:
			return -1;
	}
	if (rc < 0)
		recover(c, e);
	/*
	 * A COMMIT that the server answered with an error has not taken place,
	 * also when the server ended the session with that answer.  Without an
	 * answer nobody knows.
	 */
	if (answered)
		e->rolled_back = 1;
	return rc;
}

/* an empty query on an idle connection, with a limit on the wait */
static int
pg_ping(void *vc, int timeout)
{
	pgc		   *c = vc;
	PGresult   *r;

	if (PQstatus(c->pg) != CONNECTION_OK)
		return -1;
	if (xnet_dead(PQsocket(c->pg), 0))
		return -1;
	if (!PQsendQuery(c->pg, ""))
		return -1;
	for (;;)
	{
		while (PQisBusy(c->pg))
		{
			if (xnet_wait_readable(PQsocket(c->pg), timeout) <= 0 || !PQconsumeInput(c->pg))
				return -1;
		}
		r = PQgetResult(c->pg);
		if (r == NULL)
			break;
		PQclear(r);
	}
	return PQstatus(c->pg) == CONNECTION_OK ? 0 : -1;
}

/* ------------------------------------------- values from and to stream files */

static int
pg_lob_in_sql(void *vc, int kind, char *out, int outsize)
{
	/* first marker: the value itself, small; second: the large object it was staged in */
	snprintf(out, (size_t) outsize, "%s", kind == XLOB_CLOB ?
			 "convert_from(coalesce(?::bytea, lo_get(?::oid)), 'UTF8')" :
			 "coalesce(?::bytea, lo_get(?::oid))");
	return 2;
}

/*
 * The column in place of PUT_xLOB_TO_FILE(value, name): a bytea that
 * starts with a letter, the length of the file name as 8 hex digits and the
 * name.  Then follows, after D, the value itself; after S the OID of the
 * large object the server put it in, because it is longer than lob_direct;
 * after X only its length, where nothing can be staged.
 */
static int
pg_lob_out_sql(void *vc, int kind, int stage, char *pre, char *mid, char *post, xbe_err *e)
{
	pgc		   *c = vc;
	const char *hdr = "lpad(to_hex(octet_length(convert_to(xdb_f.p, 'UTF8'))), 8, '0') || xdb_f.p";
	int			can = stage && c && PQserverVersion(c->pg) >= 90400;

	snprintf(pre, XLOB_SQLPRE,
			 "(select case when xdb_f.v is null then null"
			 " when octet_length(xdb_f.v) <= %d then convert_to('D' || %s, 'UTF8') || xdb_f.v"
			 " else convert_to('%c' || %s || %s, 'UTF8') end"
			 " from (select %s(",
			 c ? c->lob_direct : LOB_DIRECT_KB * 1024, hdr, can ? 'S' : 'X', hdr,
			 can ? "lo_from_bytea(0, xdb_f.v)::text" : "octet_length(xdb_f.v)::text",
			 kind == XLOB_CLOB ? "convert_to(" : "");
	snprintf(mid, XLOB_SQLMID, "%s", kind == XLOB_CLOB ? ")::text, 'UTF8') as v, coalesce((" : ") as v, coalesce((");
	/* OFFSET 0: the value is worked out once per row, whatever the planner thinks */
	snprintf(post, XLOB_SQLMID, ")::text, '') as p offset 0) as xdb_f)");
	return 0;
}

static void
pg_lob_sinks(void *vc, const int *cols, int n)
{
	pgc		   *c = vc;

	c->nsink = n > LOB_MAXSINK ? LOB_MAXSINK : n;
	memcpy(c->sink, cols, sizeof(int) * (size_t) c->nsink);
}

static void
lob_err(xbe_err *e, const char *state, const char *msg)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", state);
	snprintf(e->msg, sizeof e->msg, "%s", msg);
}

/* remove a staged large object; nothing happens (and nothing fails) when it is gone */
static void
lob_unlink(pgc *c, unsigned oid)
{
	char		q[120];

	snprintf(q, sizeof q, "SELECT lo_unlink(oid) FROM pg_largeobject_metadata WHERE oid = %u", oid);
	run(c, q, NULL);
}

/* reads until buf holds want bytes or the file ends; -1 = error */
static int
lob_fill(xlob_src *src, char *buf, int want, xbe_err *e)
{
	int			have = 0;

	while (have < want)
	{
		int			n = src->read(src->ctx, buf + have, want - have, e);

		if (n < 0)
			return -1;
		if (n == 0)
			break;
		have += n;
	}
	return have;
}

static int
pg_lob_in(void *vc, int kind, xlob_src *src, char **slot, xbe_err *e)
{
	static const char hx[] = "0123456789abcdef";
	pgc		   *c = vc;
	int			direct = c->lob_direct;
	int			cap = direct + 1 > LOB_CHUNK ? direct + 1 : LOB_CHUNK;
	char	   *buf;
	int			n;
	unsigned	oid = 0;
	long long	off;
	PGresult   *r;
	Oid			t1[1] = {17};
	Oid			t3[3] = {26, 20, 17};
	const char *v[3];
	int			l[3],
				f[3];
	char		oidtxt[24],
				offtxt[24];

	slot[0] = slot[1] = NULL;
	if (alive(c, e) < 0)
		return -1;
	buf = malloc((size_t) cap);
	if (buf == NULL)
	{
		lob_err(e, "57011", "not enough memory for a file value");
		return -1;
	}
	n = lob_fill(src, buf, direct + 1, e);
	if (n < 0)
	{
		free(buf);
		return -1;
	}
	if (n <= direct)
	{
		/* small: the hex text of a bytea, in the statement */
		char	   *h = malloc((size_t) n * 2 + 3);
		int			i;

		if (h == NULL)
		{
			free(buf);
			lob_err(e, "57011", "not enough memory for a file value");
			return -1;
		}
		h[0] = '\\';
		h[1] = 'x';
		for (i = 0; i < n; i++)
		{
			h[2 + 2 * i] = hx[((unsigned char) buf[i]) >> 4];
			h[3 + 2 * i] = hx[((unsigned char) buf[i]) & 15];
		}
		h[2 + 2 * n] = '\0';
		free(buf);
		slot[0] = h;
		return 0;
	}
	/* large: into a large object, piece by piece */
	if (PQserverVersion(c->pg) < 90400 || c->nlob >= LOB_MAXSTAGED)
	{
		free(buf);
		lob_err(e, "0A000", c->nlob >= LOB_MAXSTAGED ? "too many large files in one statement" :
				"a file larger than lob_direct needs PostgreSQL 9.4 or later");
		return -1;
	}
	if (PQtransactionStatus(c->pg) == PQTRANS_IDLE)
	{
		/* no unit of work: one of our own around the statement */
		if (run(c, "BEGIN", e) < 0)
		{
			free(buf);
			recover(c, e);
			return -1;
		}
		c->lob_own = 1;
	}
	v[0] = buf;
	l[0] = n;
	f[0] = 1;
	r = PQexecParams(c->pg, "SELECT lo_from_bytea(0, $1)", 1, t1, v, l, f, 0);
	if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1)
	{
		set_err(c, r, e);
		PQclear(r);
		free(buf);
		recover(c, e);
		return -1;
	}
	oid = (unsigned) strtoul(PQgetvalue(r, 0, 0), NULL, 10);
	PQclear(r);
	c->lob[c->nlob++] = oid;
	off = n;
	snprintf(oidtxt, sizeof oidtxt, "%u", oid);
	for (;;)
	{
		n = lob_fill(src, buf, LOB_CHUNK, e);
		if (n < 0)
		{
			free(buf);
			return -1;
		}
		if (n == 0)
			break;
		snprintf(offtxt, sizeof offtxt, "%lld", off);
		v[0] = oidtxt;
		v[1] = offtxt;
		v[2] = buf;
		l[0] = l[1] = 0;
		l[2] = n;
		f[0] = f[1] = 0;
		f[2] = 1;
		r = PQexecParams(c->pg, "SELECT lo_put($1, $2, $3)", 3, t3, v, l, f, 0);
		if (PQresultStatus(r) != PGRES_TUPLES_OK)
		{
			set_err(c, r, e);
			PQclear(r);
			free(buf);
			recover(c, e);
			return -1;
		}
		PQclear(r);
		off += n;
	}
	free(buf);
	slot[1] = strdup(oidtxt);
	return slot[1] ? 0 : -1;
}

static void
pg_lob_in_done(void *vc, int ok)
{
	pgc		   *c = vc;
	int			i;

	if (PQstatus(c->pg) == CONNECTION_OK)
	{
		/*
		 * A failed transaction takes the large objects with it when it is
		 * rolled back; in a healthy one they are removed here, also when
		 * the statement did not run for a reason of ours.
		 */
		if (PQtransactionStatus(c->pg) == PQTRANS_INTRANS)
			for (i = 0; i < c->nlob; i++)
				lob_unlink(c, c->lob[i]);
		if (c->lob_own)
			run(c, (ok && PQtransactionStatus(c->pg) == PQTRANS_INTRANS) ? "COMMIT" : "ROLLBACK", NULL);
	}
	c->nlob = 0;
	c->lob_own = 0;
}

static int
hexv(int ch)
{
	return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
		ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

/* n bytes from hex text at *pp; -1 when the text ends or is not hex */
static int
unhex(const char **pp, unsigned char *out, int n)
{
	const char *p = *pp;
	int			i;

	for (i = 0; i < n; i++)
	{
		int			a = hexv((unsigned char) p[0]);
		int			b = a < 0 ? -1 : hexv((unsigned char) p[1]);

		if (b < 0)
			return -1;
		out[i] = (unsigned char) (a * 16 + b);
		p += 2;
	}
	*pp = p;
	return 0;
}

static int
pg_lob_out(void *vc, int kind, const char *v, xlob_dst *dst, xbe_err *e)
{
	pgc		   *c = vc;
	const char *p = v;
	unsigned char tag,
				ln[8];
	char		path[1100];
	unsigned char piece[16384];
	char		rest[40];
	int			plen = 0,
				i;

	if (p[0] != '\\' || p[1] != 'x')
		goto bad;
	p += 2;
	if (unhex(&p, &tag, 1) < 0 || unhex(&p, ln, 8) < 0)
		goto bad;
	for (i = 0; i < 8; i++)
	{
		int			d = hexv(ln[i]);

		if (d < 0)
			goto bad;
		plen = plen * 16 + d;
	}
	if (plen < 0 || plen >= (int) sizeof path)
	{
		lob_err(e, "428A1", "file name of a row is too long (reason 2)");
		return -1;
	}
	if (unhex(&p, (unsigned char *) path, plen) < 0)
		goto bad;
	path[plen] = '\0';
	if (tag == 'D')
	{
		if (dst->open(dst->ctx, path, e) < 0)
			return -1;
		for (;;)
		{
			int			n = 0;

			while (n < (int) sizeof piece && p[0] != '\0')
			{
				if (unhex(&p, piece + n, 1) < 0)
					goto bad;
				n++;
			}
			if (n == 0)
				break;
			if (dst->write(dst->ctx, (const char *) piece, n, e) < 0)
				return -1;
		}
		return 0;
	}
	/* what follows the name is a number in digits */
	for (i = 0; i < (int) sizeof rest - 1 && p[0] != '\0'; i++)
		if (unhex(&p, (unsigned char *) rest + i, 1) < 0)
			goto bad;
	rest[i] = '\0';
	if (tag == 'X')
	{
		lob_err(e, "0A000", "");
		snprintf(e->msg, sizeof e->msg, "a value of %s bytes for file \"%.300s\" is larger than lob_direct: "
				 "it needs a connection under commitment control", rest, path);
		return -1;
	}
	if (tag == 'S')
	{
		unsigned	oid = (unsigned) strtoul(rest, NULL, 10);
		long long	off = 0;
		Oid			t3[3] = {26, 20, 23};
		const char *pv[3];
		char		oidtxt[24],
					offtxt[24],
					lentxt[24];
		int			rc = 0;

		if (dst->open(dst->ctx, path, e) < 0)
		{
			lob_unlink(c, oid);
			return -1;
		}
		snprintf(oidtxt, sizeof oidtxt, "%u", oid);
		snprintf(lentxt, sizeof lentxt, "%d", LOB_CHUNK);
		pv[0] = oidtxt;
		pv[1] = offtxt;
		pv[2] = lentxt;
		for (;;)
		{
			PGresult   *r;
			int			n;

			snprintf(offtxt, sizeof offtxt, "%lld", off);
			r = PQexecParams(c->pg, "SELECT lo_get($1, $2, $3)", 3, t3, pv, NULL, NULL, 1);
			if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1)
			{
				set_err(c, r, e);
				PQclear(r);
				recover(c, e);
				return -1;		/* the failed transaction takes the large object with it */
			}
			n = PQgetlength(r, 0, 0);
			if (n > 0 && dst->write(dst->ctx, PQgetvalue(r, 0, 0), n, e) < 0)
				rc = -1;
			PQclear(r);
			off += n;
			if (rc < 0 || n < LOB_CHUNK)
				break;
		}
		lob_unlink(c, oid);
		return rc;
	}
bad:
	lob_err(e, "58004", "a file column came back from the server in a form the driver does not know");
	return -1;
}

static const xbe_lob pg_lob = {
	pg_lob_in_sql, pg_lob_out_sql, pg_lob_sinks, pg_lob_in, pg_lob_in_done, pg_lob_out, NULL
};

/* XDB CONNECTION INFO: what libpq keeps of the connection and of the server's start-up report */
static void
pg_info(void *vc, xbe_info_add add, void *ctx)
{
	static const char *const par[][2] = {
		{"server_encoding", "encoding"}, {"client_encoding", "client_encoding"},
		{"TimeZone", "time_zone"}, {"DateStyle", "date_style"},
		{"session_authorization", "session_user"}, {"is_superuser", "superuser"},
		{"in_hot_standby", "standby"}, {"default_transaction_read_only", "read_only_by_default"},
		{"standard_conforming_strings", "standard_conforming_strings"},
		{"application_name", "application_name"}
	};
	pgc		   *c = vc;
	PGconn	   *pg = c->pg;
	const char *p;
	char		v[128];
	int			lv = PQlibVersion();
	unsigned	i;

	snprintf(v, sizeof v, "PostgreSQL protocol %d", PQprotocolVersion(pg));
	add(ctx, "link", "protocol", v);
	add(ctx, "link", "encryption", PQsslInUse(pg) ? "on" : "none");
	snprintf(v, sizeof v, "%d", PQbackendPID(pg));
	add(ctx, "link", "session_id", v);
	switch (PQtransactionStatus(pg))
	{
		case PQTRANS_IDLE:
			p = "idle";
			break;
		case PQTRANS_INTRANS:
			p = "in a transaction";
			break;
		case PQTRANS_INERROR:
			p = "in a failed transaction";
			break;
		case PQTRANS_ACTIVE:
			p = "a statement is running";
			break;
		default:
			p = "connection is not usable";
			break;
	}
	add(ctx, "link", "state", p);
	snprintf(v, sizeof v, "libpq %d.%d", lv / 10000, lv % 10000);
	add(ctx, "link", "client_library", v);
	/* this backend opens a lost connection again by itself: the core adds these to its own */
	snprintf(v, sizeof v, "%d", c->n_reset);
	add(ctx, "count", "reconnects", v);
	snprintf(v, sizeof v, "%d", c->n_reset_txn);
	add(ctx, "count", "units_of_work_lost", v);
	snprintf(v, sizeof v, "%lld", (long long) c->t_reset);
	add(ctx, "count", "last_reconnect", v);

	add(ctx, "server", "product", "PostgreSQL");
	p = PQparameterStatus(pg, "server_version");
	add(ctx, "server", "version", p ? p : "");
	snprintf(v, sizeof v, "%d", PQserverVersion(pg));
	add(ctx, "server", "server_version_num", v);
	p = PQdb(pg);
	add(ctx, "server", "database", p ? p : "");
	for (i = 0; i < sizeof par / sizeof par[0]; i++)
		if ((p = PQparameterStatus(pg, par[i][0])) != NULL)
			add(ctx, "server", par[i][1], p);
	/* what the server said beside its answers (see pg_notice) */
	snprintf(v, sizeof v, "%d", c->n_notice);
	add(ctx, "server", "notices", v);
	if (c->notice[0])
		add(ctx, "server", "last_notice", c->notice);

	add(ctx, "options", "long_values", c->cut ? "cut" : "full");
	snprintf(v, sizeof v, "%d bytes", c->lob_direct);
	add(ctx, "options", "lob_direct", v);
}

const xbe_driver xbe_pg = {
	"pg", "$%d", "PGS", 1,
	pg_connect, pg_disconnect, pg_version,
	pg_exec, pg_describe, pg_stmt_free,
	pg_open, pg_fetch, pg_close, pg_txn, pg_ping,
	&pg_lob, pg_info,
	pg_cursor_cols
};

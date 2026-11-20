/*
 * xdbard.c - native ILE application requester driver (ARD), backend neutral.
 *
 * Db2 for i calls an ARD program for every SQL operation directed at an RDB
 * directory entry defined with RMTLOCNAME(*ARDPGM).  This module implements
 * that interface (IBM i "SQL Client Integration Exit Program", formats
 * ARCN0100 ... ARCC0100) and forwards the work to a backend chosen with
 * "backend =" in xdb.conf: PostgreSQL (xdbpg.c, the ILE port of libpq),
 * MySQL/MariaDB (xdbmy.c) or Microsoft SQL Server (xdbms.c).  The core
 * rewrites parameter markers, turns host variables into UTF-8 text and
 * result values from UTF-8 text into Db2 row format.
 *
 * Compiled LOCALETYPE(*LOCALEUTF) TGTCCSID(1208): every literal here is
 * UTF-8, every job-CCSID field is converted explicitly with ile_conv().
 * With XDB_LINUX_EMU the same source runs under a Linux test harness.
 *
 * Entry points: xdb_ard_entry() (the 8 ARD parameters) and
 * xdb_commit_exit() (commitment control exit program parameters).
 */
#ifdef __ILEC400__
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>
#include "xdb_be.h"
#include "xdbard.h"
#include "xdbprf.h"
#include "xdblob.h"

extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);
extern int	ile_job_ccsid(void);

#define JOB (ile_job_ccsid())
#define UTF8 1208
#define MAXSESS 16
#define MAXSTMT 64
#define MAXCOL 256
#define MAXPARM 256
#define FETCHROWS 200			/* rows asked from the server at a time, unless option fetch_rows says otherwise */
#define FETCHROWS_MAX 5000
/* Db2 mishandles ARD result blocks larger than 64K (phantom rows, SQLCODE -30000): stay below 32K */
#define BLOCKMAX 32000

/* SQL types (even = not null, odd = nullable) */
#define T_DATE 384
#define T_TIME 388
#define T_TIMESTAMP 392
#define T_VARCHAR 448
#define T_CHAR 452
#define T_LONGVARCHAR 456
#define T_VARGRAPHIC 464
#define T_GRAPHIC 468
#define T_FLOAT 480
#define T_DECIMAL 484
#define DECMAX 31				/* digits of a decimal column that Db2 takes from a driver */
#define T_ZONED 488
#define T_BIGINT 492
#define T_INTEGER 496
#define T_SMALLINT 500

typedef struct
{
	short		sqltype;
	short		sqllen;
	unsigned char sqlres[12];
	unsigned char *sqldata;
	short	   *sqlind;
	struct
	{
		short		length;
		unsigned char data[30];
	}			sqlname;
} xsqlvar;

typedef struct
{
	unsigned char sqldaid[8];
	int			sqldabc;
	short		sqln;
	short		sqld;
	xsqlvar		sqlvar[1];
} xsqlda;

typedef struct
{
	char		name[64];
	int			kind;			/* XK_* reported by the backend */
	int			sqltype;
	int			len;			/* bytes of data in a row (max for varchar) */
	int			prec;
	int			scale;
} xcol;

typedef struct
{
	int			used;
	int			section;
	char		pkg[20];
	char		stmtname[20];
	char		curname[20];
	char	   *sql;			/* UTF-8, markers in the backend's style */
	void	   *bstmt;			/* backend statement handle (may be NULL) */
	void	   *bcur;			/* backend cursor while is_open */
	int			ncols;
	xcol	   *cols;
	int			is_open;
	xbe_batch	buf;			/* read-ahead rows */
	int			bufrow;
	long		row_max;		/* bytes of the largest row the open cursor has delivered */
	int			row_seen;		/* ... and whether it has delivered one */
	int			row_asked;		/* rows asked from the server the last time (for the trace) */
	int			eof;
	int			block;
	int			nfetch;			/* fetch calls since open (trace) */
	unsigned	used_at;		/* for reuse of the least recently used slot */
	xbe_err		fetch_err;		/* error met after rows of the same block */
	int			has_fetch_err;
	int			virt;			/* answered by the driver itself: 1 XDB LAST ERROR, 2 XDB CONNECTION INFO */
	int			virt_done;
	int			lost;			/* the cursor went with a lost connection */
	int			reprep;			/* describe again on the new connection before use */
	int			late_cols;		/* the columns came with the open, not with the describe */
	xlob_plan  *lob;			/* file functions in the statement (xdblob.c), or NULL */
	char	   *sinkval;		/* results of PUT_xLOB_TO_FILE for the rows in buf */
} xstmt;

typedef struct
{
	int			used;
	int			actgrp;
	char		rdb[20];
	const xbe_driver *be;
	void	   *bc;				/* backend connection */
	int			autocommit;
	int			fetch_rows;		/* option fetch_rows: rows asked from the server at a time */
	int			in_txn;
	int			cmt_registered;
	int			cmt_wanted;
	int			cmt_gen;		/* identifies the commitment resource of this connection */
	int			fallback;		/* no commit resource: commit at statement boundaries */
	int			cmt_kind;		/* XDB_CMT_...: the part the resource has in a commit */
	int			cmt_handle;		/* the system's handle of the resource, to remove it */
	char		cmt_note[120];	/* the kinds the system did not take, and why */
	/*
	 * The unit of work as the program sees it, from its first statement to
	 * COMMIT or ROLLBACK.  in_txn is the server's side of it, which a lost
	 * connection ends early.
	 */
	int			uow_dirty;		/* it has changed something on the server */
	int			uow_set;		/* a SET of the session ran in the transaction: only a COMMIT keeps it */
	int			uow_lost;		/* ... and the server has lost that: it cannot be committed */
	char		uow_why[200];	/* how it was lost */
	int			voted_no;		/* the last commit ended with this connection's no: a rollback
								 * call that follows it has nothing left to do */
	int			savepoints;
	int			text_len;
	int			num_scale;
	int			char_ccsid;
	int			trim_char;
	char		datefmt[8];
	char		timefmt[8];
	char		trace[256];
	xstmt		st[MAXSTMT];
	unsigned	clock;
	/* to open the connection again when the server or the network dropped it */
	char	   *cn_info;
	char	   *cn_user;
	char	   *cn_pwd;
	time_t		last_use;
	int			idle_check;		/* seconds idle before the connection is tested, 0 = never */
	int			ping_timeout;
	/* what happened on this connection since CONNECT, for XDB CONNECTION INFO */
	time_t		t_connect;
	time_t		t_revive;		/* when the connection was last replaced */
	int			n_revive;		/* how often it was replaced */
	int			n_lost;			/* ... with a unit of work open */
	int			n_ping;			/* tests of an idle connection */
	long long	n_prep,
				n_exec,
				n_changed,		/* rows the executed statements reported */
				n_open,
				n_fetch,		/* fetch requests of Db2 */
				n_rows,			/* rows handed to Db2 */
				n_befetch,		/* fetches from the server */
				n_commit,
				n_cfail,		/* commits that did not take place on the server */
				n_rollback,
				n_err;
	int			joblog;			/* errors to the job log: 0 never, 1 long texts, 2 all */
	int			bad_type;		/* SQL type of the host variable refused last */
	int			in_len;			/* length of the input format of the request */
	const char *in_base;		/* and where it starts */
	/* file functions of the statement text read in this request, until a statement takes them */
	xlob_plan  *lob_new;
	int			lob_bad;		/* they are used wrongly: lob_err says how */
	xbe_err		lob_err;
} xsess;

static xsess sess[MAXSESS];
static int	cmt_seq;		/* last commitment resource number given out in this job */
static char gtrace[256];
static unsigned char job_blank = 0;

/* ---------------------------------------------------------------- utilities */

static void
tracef(const char *file, const char *fmt,...)
{
	FILE	   *f;
	va_list		ap;

	if (file == NULL || file[0] == '\0')
		return;
	f = fopen(file, "ab");
	if (f == NULL)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static int
g32(const char *p, int off)
{
	int			v;

	memcpy(&v, p + off, 4);
	return v;
}

static void
put16be(unsigned char *p, int v)
{
	p[0] = (unsigned char) ((v >> 8) & 0xFF);
	p[1] = (unsigned char) (v & 0xFF);
}

static unsigned char
blank(void)
{
	if (job_blank == 0)
	{
		char		b[4];

		if (ile_conv(UTF8, JOB, " ", 1, b, sizeof b) == 1)
			job_blank = (unsigned char) b[0];
		else
			job_blank = 0x40;
	}
	return job_blank;
}

#ifdef __ILEC400__
#include <qusrjobi.h>
#include <qmhsndpm.h>
#endif

/* numeric CCSID of the job (default CCSID when the job runs with 65535) */
static int
job_ccsid_num(void)
{
	static int	cached = 0;

	if (cached == 0)
	{
#ifdef __ILEC400__
		Qwc_JOBI0400_t ji;
		char		fmt[8],
					job[26],
					iid[16];
		struct
		{
			int			provided;
			int			avail;
			char		rest[32];
		}			ec;
		char		t[4];

		/* EBCDIC blanks/literals without relying on blank(): 0x40 is invariant */
		memset(job, 0x40, sizeof job);
		memset(iid, 0x40, sizeof iid);
		ile_conv(UTF8, 0, "*", 1, t, sizeof t);
		job[0] = t[0];
		ile_conv(UTF8, 0, "JOBI0400", 8, fmt, sizeof fmt);
		memset(&ec, 0, sizeof ec);
		ec.provided = sizeof ec;
		memset(&ji, 0, sizeof ji);
		QUSRJOBI(&ji, sizeof ji, fmt, job, iid, &ec);
		cached = ji.Coded_Char_Set_ID;
		if (cached == 65535 || cached <= 0)
			cached = ji.Default_Coded_Char_Set_Id;
		if (cached <= 0 || cached == 65535)
			cached = 37;
#else
		cached = ile_job_ccsid();
#endif
	}
	return cached;
}

/* fixed job-CCSID field -> trimmed UTF-8 string */
static void
fld(const char *p, int off, int len, char *out, int outsize)
{
	int			n = len;

	while (n > 0 && ((unsigned char) p[off + n - 1] == blank() || p[off + n - 1] == '\0'))
		n--;
	if (ile_conv(JOB, UTF8, p + off, n, out, outsize - 1) < 0)
		out[0] = '\0';
}

/* UTF-8 string -> fixed job-CCSID field, blank padded */
static void
setfld(char *p, int len, const char *s)
{
	char		tmp[512];
	int			n = ile_conv(UTF8, JOB, s, strlen(s), tmp, sizeof tmp);

	memset(p, blank(), len);
	if (n > 0)
		memcpy(p, tmp, n > len ? len : n);
}

/*
 * What Db2 does with the SQLCA of a driver (found by experiment, test XDBT6):
 *
 *   - SQLCODE and SQLSTATE that belong together in Db2 (-206 with 42703):
 *     Db2 shows its own message SQL0206 and takes SQLERRMC as that message's
 *     data, in the form message data has: every token with a two byte
 *     length in front.  Anything else there ends as "SQL system error".
 *   - a pair Db2 does not know (-438 with 23502): Db2 shows the text of
 *     the SQLSTATE class, "Integrity constraint violation - SQLSTATE 23502,
 *     SQLCODE -438", and does not look at SQLERRMC.
 *
 * So for the common errors the driver answers with Db2's own pair and the
 * tokens of that message (the name the server complained about), and for
 * the rest with a pair Db2 does not know and the server's text in SQLERRMC.
 * The complete text of the server is always in XDB LAST ERROR.
 */

static int	sqlca_raw = 0;		/* XDB SQLCA: SQLERRMC exactly as given */

/* the first piece of msg in quotes ("x", 'x' or `x`); *N when there is none */
static void
quoted_name(const char *msg, char *out, int size)
{
	const char *p;

	snprintf(out, size, "*N");
	for (p = msg; *p; p++)
		if (*p == '"' || *p == '\'' || *p == '`')
		{
			const char *q = strchr(p + 1, *p);
			int			n = q ? (int) (q - p - 1) : 0;

			if (n > 0)
			{
				if (n > size - 1)
					n = size - 1;
				memcpy(out, p + 1, n);
				out[n] = '\0';
			}
			return;
		}
}

/* the same, looking first behind the word "constraint" or "key" */
static void
constraint_name(const char *msg, char *out, int size)
{
	static const char *const w[] = {"constraint", "CONSTRAINT", "Constraint", " key ", NULL};
	int			i;

	for (i = 0; w[i]; i++)
	{
		const char *p = strstr(msg, w[i]);

		if (p)
		{
			quoted_name(p, out, size);
			if (strcmp(out, "*N") != 0)
				return;
		}
	}
	quoted_name(msg, out, size);
}

/* message data of Db2's own message for this pair; 0 = there is no plan for it */
static int
own_message_data(int code, const char *state, const char *msg, char *out)
{
	char		name[48],
				schema[40];
	const char *tok[5];
	int			nt = 0,
				n = 0,
				i;

#define PAIR(c, s) (code == (c) && strcmp(state, s) == 0)
	quoted_name(msg, name, sizeof name);
	snprintf(schema, sizeof schema, "*N");
	if (PAIR(-204, "42704") || PAIR(-601, "42710"))
	{
		char	   *dot = strrchr(name, '.');

		if (dot && dot[1] && dot != name)
		{
			*dot = '\0';
			snprintf(schema, sizeof schema, "%s", name);
			memmove(name, dot + 1, strlen(dot + 1) + 1);
		}
		tok[nt++] = name;
		tok[nt++] = schema;
		tok[nt++] = "N";
	}
	else if (PAIR(-206, "42703"))
	{
		tok[nt++] = name;
		tok[nt++] = "*N";
		tok[nt++] = "*N";
	}
	else if (PAIR(-104, "42601"))
	{
		tok[nt++] = name;
		tok[nt++] = "*N";
	}
	else if (PAIR(-803, "23505"))
	{
		constraint_name(msg, name, sizeof name);
		tok[nt++] = name;
		tok[nt++] = "*N";
		tok[nt++] = "*N";
		tok[nt++] = "*N";
	}
	else if (PAIR(-407, "23502"))
	{
		tok[nt++] = "*N";
		tok[nt++] = "*N";
		tok[nt++] = "*N";
		tok[nt++] = "*N";
		tok[nt++] = name;
	}
	else if (PAIR(-530, "23503"))
	{
		constraint_name(msg, name, sizeof name);
		tok[nt++] = name;
		tok[nt++] = "*N";
	}
	else if (PAIR(-404, "22001") || PAIR(-501, "24501") || PAIR(-518, "07003"))
		tok[nt++] = name;
	else if (PAIR(-551, "42501") || PAIR(-913, "57033"))
	{
		tok[nt++] = name;
		tok[nt++] = "*N";
		tok[nt++] = "N";
	}
	else if (PAIR(-517, "07005"))
	{
		tok[nt++] = "*N";
		tok[nt++] = name;
	}
	else if (PAIR(-452, "428A1"))
	{
		/*
		 * SQL0452 has two binary fields: the number of the host variable
		 * (none here: 0) and the reason code, which the texts of xdblob.c
		 * carry as "(reason n)".
		 */
		const char *r = strstr(msg, "(reason ");

		out[0] = out[1] = out[2] = 0;
		out[3] = (char) ((r && r[8] >= '0' && r[8] <= '9') ? r[8] - '0' : 0);
		return 4;
	}
	else
		return 0;
#undef PAIR
	for (i = 0; i < nt; i++)
	{
		char		t[64];
		int			l = ile_conv(UTF8, JOB, tok[i], strlen(tok[i]), t, sizeof t);

		if (l < 0)
			l = 0;
		if (n + 2 + l > 70)
			l = 70 - n - 2;
		if (l < 0)
			break;
		out[n] = (char) (l >> 8);
		out[n + 1] = (char) (l & 0xFF);
		memcpy(out + n + 2, t, l);
		n += 2 + l;
	}
	return n;
}


/* ------------------------------------------------------------------- SQLCA */

static void
sqlca_set(char *ca, int code, const char *state, const char *msg, int rows)
{
	int			v;
	short		ml = 0;
	char		tmp[80];

	memset(ca, 0, 136);
	setfld(ca, 8, "SQLCA");
	v = 136;
	memcpy(ca + 8, &v, 4);
	memcpy(ca + 12, &code, 4);
	memset(ca + 18, blank(), 70);
	if (msg && msg[0])
	{
		int			n = ile_conv(UTF8, JOB, msg, strlen(msg) > 70 ? 70 : strlen(msg), tmp, sizeof tmp);

		if (n > 70)
			n = 70;
		if (n > 0)
		{
			memcpy(ca + 18, tmp, n);
			ml = (short) n;
		}
	}
	if (!sqlca_raw)
	{
		char		md[70];
		int			n = own_message_data(code, state, msg ? msg : "", md);

		if (n > 0)
		{
			memset(ca + 18, blank(), 70);
			memcpy(ca + 18, md, n);
			ml = (short) n;
		}
	}
	memcpy(ca + 16, &ml, 2);
	setfld(ca + 88, 8, "XDB01000");
	memcpy(ca + 96 + 8, &rows, 4);	/* sqlerrd(3) */
	memset(ca + 120, blank(), 11);
	setfld(ca + 131, 5, state);
}

/* backend SQLSTATE -> SQLCODE (the SQLSTATE itself is passed through) */
static int
state2code(const char *st)
{
	static const struct
	{
		const char *st;
		int			code;
	}			map[] = {
		{"23505", -803}, {"42P01", -204}, {"42704", -204}, {"42703", -206}, {"42601", -104},
		{"23503", -530}, {"23502", -407}, {"22001", -404}, {"22012", -802},
		{"22003", -802}, {"42883", -440}, {"42804", -408}, {"40001", -913},
		{"40P01", -913}, {"57014", -952}, {"28P01", -30082}, {"28000", -30082},
		{"3D000", -30061}, {"42P07", -601}, {"42710", -601}, {"22007", -180},
		{"22008", -181}, {"42501", -551}, {"0A000", -84},
		{"42S02", -204}, {"42S22", -206}, {"42S01", -601}, {"42S21", -612},
		{"57033", -913}, {"08004", -30061}, {"07001", -313}, {"07005", -517},
		{"21S01", -117}, {"42000", -84}, {"22018", -420}, {"428A1", -452}, {NULL, 0}
	};
	int			i;

	for (i = 0; map[i].st; i++)
		if (strcmp(map[i].st, st) == 0)
			return map[i].code;
	if (strncmp(st, "08", 2) == 0)
		return -30080;
	return -438;
}


/* ------------------------------------------------------------ what went wrong */

/*
 * SQLERRMC has room for 70 bytes, and Db2 shows its own message text.  The
 * complete error of the database server is kept here for the statement
 * XDB LAST ERROR (one row: SQLCODE, SQLSTATE, NATIVE_ERROR, DATABASE, RDB,
 * MESSAGE_TEXT) and, when it would not fit in SQLERRMC, written to the job
 * log.  One per job: the most recent error of any connection.
 */
static struct
{
	int			sqlcode;
	char		sqlstate[6];
	int			native;
	char		dbms[16];
	char		rdb[20];
	char		msg[sizeof(((xbe_err *) 0)->msg)];
} last_err;

/* a diagnostic message in the job log (CPF9897: the text as it is) */
static void
joblog(const char *text)
{
#ifdef __ILEC400__
	char		data[600];
	char		msgid[7],
				msgf[20],
				mtype[10],
				stack[10],
				key[4];
	struct
	{
		int			provided;
		int			available;
		char		rest[32];
	}			ec;
	int			n = ile_conv(UTF8, JOB, text, strlen(text), data, sizeof data);

	if (n <= 0)
		return;
	setfld(msgid, 7, "CPF9897");
	setfld(msgf, 20, "QCPFMSG   *LIBL");
	setfld(mtype, 10, "*DIAG");
	setfld(stack, 10, "*");
	memset(&ec, 0, sizeof ec);
	ec.provided = sizeof ec;
	QMHSNDPM(msgid, msgf, data, n, mtype, stack, 0, key, &ec);
#else
	if (getenv("XDB_EMU_JOBLOG"))
		fprintf(stderr, "JOBLOG %s\n", text);
#endif
}

/* log: 0 never, 1 when the text does not fit in SQLERRMC, 2 always */
static void
err_keep(int sqlcode, const char *state, const xbe_err *e, const char *dbms, const char *rdb, int log)
{
	char		line[700];

	last_err.sqlcode = sqlcode;
	snprintf(last_err.sqlstate, sizeof last_err.sqlstate, "%s", state);
	last_err.native = e->native;
	snprintf(last_err.dbms, sizeof last_err.dbms, "%s", dbms ? dbms : "");
	snprintf(last_err.rdb, sizeof last_err.rdb, "%s", rdb ? rdb : "");
	snprintf(last_err.msg, sizeof last_err.msg, "%s", e->msg);
	if (log == 2 || (log == 1 && strlen(e->msg) > 70))
	{
		snprintf(line, sizeof line, "XDB %s (%s) SQLSTATE %s, error %d: %s", last_err.rdb, last_err.dbms,
				 state, e->native, e->msg);
		joblog(line);
	}
}

/* does sql consist of exactly these words (any case, any spacing, optional ;) */
static int
is_words(const char *sql, const char *const *w)
{
	int			i;

	if (sql == NULL)
		return 0;
	for (i = 0; w[i]; i++)
	{
		size_t		n = strlen(w[i]);
		size_t		k;

		while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r')
			sql++;
		for (k = 0; k < n; k++)
			if ((sql[k] | 0x20) != (w[i][k] | 0x20))
				return 0;
		sql += n;
	}
	while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r' || *sql == ';')
		sql++;
	return *sql == '\0';
}

/*
 * Two spellings: XDB LAST ERROR for programs, and a plain SELECT for
 * interactive SQL, which prepares only statements that start with SELECT
 * and runs everything else as EXECUTE IMMEDIATE (no result).
 */
static int
is_last_error(const char *sql)
{
	static const char *const a[] = {"XDB", "LAST", "ERROR", NULL};
	static const char *const b[] = {"SELECT", "*", "FROM", "XDB_LAST_ERROR", NULL};

	return is_words(sql, a) || is_words(sql, b);
}

static void
last_error_release(xbe_batch *b)
{
	free(b->owner);
	free(b->val);
	memset(b, 0, sizeof *b);
}

#define LAST_ERROR_COLS 6

/* the columns of XDB LAST ERROR */
static xbe_col *
last_error_cols(void)
{
	static const struct
	{
		const char *name;
		int			kind;
		int			len;
	}			d[LAST_ERROR_COLS] = {
		{"SQLCODE", XK_INTEGER, 0}, {"SQLSTATE", XK_CHAR, 5}, {"NATIVE_ERROR", XK_INTEGER, 0},
		{"DATABASE", XK_VARCHAR, 10}, {"RDB", XK_VARCHAR, 18}, {"MESSAGE_TEXT", XK_VARCHAR, 512}
	};
	xbe_col    *c = calloc(LAST_ERROR_COLS, sizeof *c);
	int			i;

	for (i = 0; c && i < LAST_ERROR_COLS; i++)
	{
		snprintf(c[i].name, sizeof c[i].name, "%s", d[i].name);
		c[i].kind = d[i].kind;
		c[i].len = d[i].len;
	}
	return c;
}

/* its one row; none when there was no error in this job yet */
static int
last_error_batch(xstmt *t, xbe_batch *b)
{
	char	   *text;
	size_t		size = sizeof last_err.msg + 128;

	memset(b, 0, sizeof *b);
	b->ncols = LAST_ERROR_COLS;
	b->eof = 1;
	if (t->virt_done || last_err.sqlstate[0] == '\0')
		return 0;
	t->virt_done = 1;
	text = malloc(size);
	b->val = calloc(LAST_ERROR_COLS, sizeof(char *));
	if (text == NULL || b->val == NULL)
	{
		free(text);
		free(b->val);
		b->val = NULL;
		return 0;
	}
	b->owner = text;
	b->release = last_error_release;
	b->val[0] = text;
	text += sprintf(text, "%d", last_err.sqlcode) + 1;
	b->val[1] = text;
	text += sprintf(text, "%s", last_err.sqlstate) + 1;
	b->val[2] = text;
	text += sprintf(text, "%d", last_err.native) + 1;
	b->val[3] = text;
	text += sprintf(text, "%s", last_err.dbms) + 1;
	b->val[4] = text;
	text += sprintf(text, "%s", last_err.rdb) + 1;
	b->val[5] = text;
	sprintf(text, "%s", last_err.msg);
	b->nrows = 1;
	return 0;
}

/*
 * The server has lost what this unit of work changed on it (the connection
 * went away, or the server rolled its transaction back).  The program is
 * told with SQLCODE -911 at the statement that finds it out; from then on
 * the unit of work cannot be committed, whatever follows in it.
 */
static void
uow_lose(xsess *s, const char *why)
{
	if (s->autocommit || s->fallback || !s->uow_dirty || s->uow_lost)
		return;
	s->uow_lost = 1;
	snprintf(s->uow_why, sizeof s->uow_why, "%s", why);
	tracef(s->trace, "  the unit of work has lost its changes on the server: a COMMIT will be refused");
}

static void
sqlca_be(char *ca, xsess *s, const xbe_err *e)
{
	char		state[8];
	int			code;

	snprintf(state, sizeof state, "%s", strlen(e->sqlstate) == 5 ? e->sqlstate : "58004");
	/* the same condition under the SQLSTATE Db2 uses for it */
	if (strcmp(state, "42P01") == 0 || strcmp(state, "42S02") == 0)
		strcpy(state, "42704");
	else if (strcmp(state, "42S22") == 0)
		strcpy(state, "42703");
	else if (strcmp(state, "42P07") == 0 || strcmp(state, "42S01") == 0)
		strcpy(state, "42710");
	else if (strcmp(state, "42000") == 0 && (strstr(e->msg, "syntax") || strstr(e->msg, "Syntax")))
		strcpy(state, "42601");
	if (e->rolled_back)			/* Db2's "unit of work rolled back" */
	{
		code = -911;
		sqlca_set(ca, -911, "40001", e->msg[0] ? e->msg : "transaction rolled back", 0);
		if (s)
			uow_lose(s, e->msg[0] ? e->msg : "the server rolled the transaction back");
	}
	else
	{
		code = state2code(state);
		sqlca_set(ca, code, state, e->msg[0] ? e->msg : "backend error", 0);
	}
	if (s)
	{
		int			log = s->joblog;

		s->n_err++;
		/* an error with a stream file (xdblob.c): its name is nowhere but in the text */
		if (log == 1 && strncmp(e->msg, "file ", 5) == 0)
			log = 2;
		tracef(s->trace, "  error %s (native %d): %s", state, e->native, e->msg);
		err_keep(code, strlen(e->sqlstate) == 5 ? e->sqlstate : state, e, s->be->name, s->rdb, log);
	}
}

/* ------------------------------------------------------------ configuration */

/*
 * A connection is described by a profile: a record of file XDBPRF (kept
 * with WRKXDBPRF) plus its options in XDBPRO, both in the library of this
 * service program.  An RDB without a profile there is looked up in the
 * older text file xdb.conf.  Either way the rest of the driver sees
 * key = value pairs through conf_get().
 */
#define PRF_NONE 0				/* not in the file: xdb.conf decides */
#define PRF_OK 1
#define PRF_DISABLED 2
#define PRF_SLOTS 4
#define PRF_MAXKV 80

typedef struct
{
	char		rdb[20];
	int			state;
	int			n;
	char	   *key[PRF_MAXKV];
	char	   *val[PRF_MAXKV];
} xprofile;

static xprofile prf[PRF_SLOTS];
static int	prf_next = 0;

static void
prf_put(xprofile *p, const char *key, const char *val)
{
	if (p->n >= PRF_MAXKV)
		return;
	p->key[p->n] = strdup(key);
	p->val[p->n] = strdup(val);
	if (p->key[p->n] && p->val[p->n])
		p->n++;
}

static const char *
prf_val(const xprofile *p, const char *key)
{
	int			i;

	for (i = 0; i < p->n; i++)
		if (strcmp(p->key[i], key) == 0)
			return p->val[i];
	return NULL;
}

static xprofile *
prf_of(const char *rdb)
{
	int			i;

	for (i = 0; i < PRF_SLOTS; i++)
		if (prf[i].state != PRF_NONE && strcmp(prf[i].rdb, rdb) == 0)
			return &prf[i];
	return NULL;
}

static void
prf_clear(xprofile *p)
{
	int			i;

	for (i = 0; i < p->n; i++)
	{
		if (strcmp(p->key[i], "password") == 0)
			memset(p->val[i], 0, strlen(p->val[i]));
		free(p->key[i]);
		free(p->val[i]);
	}
	memset(p, 0, sizeof *p);
}

/* the password is needed for the connect only */
static void
prf_forget(const char *rdb)
{
	xprofile   *p = prf_of(rdb);
	int			i;

	for (i = 0; p && i < p->n; i++)
		if (strcmp(p->key[i], "password") == 0)
			memset(p->val[i], 0, strlen(p->val[i]));
}

/* zoned field -> number */
static int
zoned(const char *p, int len)
{
	int			v = 0;
	int			i;

	for (i = 0; i < len; i++)
		v = v * 10 + (p[i] & 0x0F);
	return v;
}

/*
 * " key=value" for a connection string.  libpq wants \' inside quotes,
 * the other backends (xnet_kv) a doubled quote.
 */
static void
ci_add(char *ci, size_t size, int libpq, const char *key, const char *val)
{
	size_t		n = strlen(ci);
	int			quote = (val[0] == '\0' || strpbrk(val, " \t'\\") != NULL);
	const char *v;

	if (n + strlen(key) + 2 * strlen(val) + 6 >= size)
		return;
	if (n > 0)
		ci[n++] = ' ';
	n += (size_t) sprintf(ci + n, "%s=", key);
	if (quote)
		ci[n++] = '\'';
	for (v = val; *v; v++)
	{
		if (quote && libpq && (*v == '\'' || *v == '\\'))
			ci[n++] = '\\';
		else if (quote && !libpq && *v == '\'')
			ci[n++] = '\'';
		ci[n++] = *v;
	}
	if (quote)
		ci[n++] = '\'';
	ci[n] = '\0';
}

/* read the profile of rdb again; PRF_NONE when the files do not have it */
static int
prf_load(const char *rdb)
{
	xprofile   *p = prf_of(rdb);
	xdbprf_rec	r;
	xdbpro_rec *opt;
	char		lib[10];
	char		key[18];
	char		type[16],
				host[512],
				db[256],
				user[256],
				tls[16],
				k[64],
				v[1024],
				ci[4096];
	int			libpq,
				nopt,
				i,
				n;

	if (p)
		prf_clear(p);
	n = ile_conv(UTF8, JOB, rdb, strlen(rdb), v, sizeof v);
	if (n <= 0 || n > 18)
		return PRF_NONE;
	memset(key, blank(), sizeof key);
	memcpy(key, v, n);
	xdbprf_mylib(lib);
	if (xdbprf_read(lib, key, &r) != 0)
		return PRF_NONE;

	if (p == NULL)
	{
		p = &prf[prf_next];
		prf_next = (prf_next + 1) % PRF_SLOTS;
		prf_clear(p);
	}
	snprintf(p->rdb, sizeof p->rdb, "%s", rdb);
	fld(&r.sts, 0, 1, v, sizeof v);
	if (v[0] != 'A')
	{
		p->state = PRF_DISABLED;
		return p->state;
	}
	p->state = PRF_OK;

	fld(r.type, 0, sizeof r.type, type, sizeof type);
	for (i = 0; type[i]; i++)
		if (type[i] >= 'A' && type[i] <= 'Z')
			type[i] = (char) (type[i] + 32);
	libpq = (strcmp(type, "pg") == 0);
	prf_put(p, "backend", type);
	fld(lib, 0, 10, v, sizeof v);
	prf_put(p, "library", v);

	/* options first: one of them may replace a standard keyword */
	opt = malloc(sizeof(xdbpro_rec) * 40);
	nopt = opt ? xdbprf_options(lib, key, opt, 40) : 0;
	ci[0] = '\0';
	for (i = 0; i < nopt; i++)
	{
		char		kind[4];

		fld(opt[i].key, 0, sizeof opt[i].key, k, sizeof k);
		fld(opt[i].val, 0, sizeof opt[i].val, v, sizeof v);
		fld(&opt[i].kind, 0, 1, kind, sizeof kind);
		if (k[0] == '\0')
			continue;
		if (kind[0] == 'C')
			ci_add(ci, sizeof ci, libpq, k, v);
		else
			prf_put(p, k, v);
	}
	free(opt);
#define CI_HAS(name) (strstr(ci, name "=") == ci || strstr(ci, " " name "=") != NULL)

	fld(r.host, 0, sizeof r.host, host, sizeof host);
	fld(r.dbnm, 0, sizeof r.dbnm, db, sizeof db);
	fld(r.user, 0, sizeof r.user, user, sizeof user);
	fld(r.tls, 0, sizeof r.tls, tls, sizeof tls);
	if (host[0] && !CI_HAS("host"))
		ci_add(ci, sizeof ci, libpq, "host", host);
	if (zoned(r.port, sizeof r.port) > 0 && !CI_HAS("port"))
	{
		snprintf(v, sizeof v, "%d", zoned(r.port, sizeof r.port));
		ci_add(ci, sizeof ci, libpq, "port", v);
	}
	if (db[0] && !CI_HAS("dbname"))
		ci_add(ci, sizeof ci, libpq, "dbname", db);
	if (!CI_HAS("connect_timeout"))
	{
		n = zoned(r.tmo, sizeof r.tmo);
		snprintf(v, sizeof v, "%d", n > 0 ? n : 10);
		ci_add(ci, sizeof ci, libpq, "connect_timeout", v);
	}
	/* encryption: one setting for all types, in each backend's own words */
	i = strcmp(tls, "*OFF") == 0 ? 0 : strcmp(tls, "*REQUIRE") == 0 ? 2 : strcmp(tls, "*VERIFY") == 0 ? 3 : 1;
	if (libpq)
	{
		static const char *const m[] = {"disable", "prefer", "require", "verify-full"};

		if (!CI_HAS("sslmode"))
			ci_add(ci, sizeof ci, 1, "sslmode", m[i]);
	}
	else if (strcmp(type, "mysql") == 0)
	{
		static const char *const m[] = {"off", "prefer", "require", "verify"};

		if (!CI_HAS("tls"))
			ci_add(ci, sizeof ci, 0, "tls", m[i]);
	}
	else
	{
		static const char *const m[] = {"off", "prefer", "on", "on"};

		if (!CI_HAS("encrypt"))
		{
			ci_add(ci, sizeof ci, 0, "encrypt", m[i]);
			if (i == 3 && !CI_HAS("tls_verify"))
				ci_add(ci, sizeof ci, 0, "tls_verify", "on");
		}
	}
#undef CI_HAS
	prf_put(p, "conninfo", ci);
	if (user[0])
		prf_put(p, "user", user);

	fld(&r.pwds, 0, 1, v, sizeof v);
	if (v[0] == 'Y')
	{
		char		raw[700];
		int			ccsid = 0;

		n = xdbprf_getpwd(lib, key, raw, sizeof raw, &ccsid);
		if (n > 0)
		{
			n = ile_conv((ccsid <= 0 || ccsid == 65535) ? JOB : ccsid, UTF8, raw, n, v, sizeof v - 1);
			if (n > 0)
			{
				v[n] = '\0';
				prf_put(p, "password", v);
			}
			memset(v, 0, sizeof v);
		}
		else
			prf_put(p, "password_error", "1");
		memset(raw, 0, sizeof raw);
	}
	n = zoned(r.ccsid, sizeof r.ccsid);
	if (n > 0)
	{
		snprintf(v, sizeof v, "%d", n);
		prf_put(p, "ccsid", v);
	}
	n = zoned(r.txtl, sizeof r.txtl);
	if (n > 0)
	{
		snprintf(v, sizeof v, "%d", n);
		prf_put(p, "text_len", v);
	}
	snprintf(v, sizeof v, "%d", zoned(r.nscl, sizeof r.nscl));
	prf_put(p, "numeric_scale", v);
	fld(&r.trc, 0, 1, v, sizeof v);
	if (v[0] == 'Y')
	{
		snprintf(v, sizeof v, "/tmp/xdb_%s.log", rdb);
		prf_put(p, "trace", v);
	}
	return p->state;
}

static int	conf_file(const char *rdb, const char *key, char *out, int outsize);

/* 0 = found, anything else: not set */
static int
conf_get(const char *rdb, const char *key, char *out, int outsize)
{
	const xprofile *p = prf_of(rdb);
	const char *v;

	if (p == NULL)
		return conf_file(rdb, key, out, outsize);
	out[0] = '\0';
	v = prf_val(p, key);
	if (v == NULL)
		return 1;
	snprintf(out, outsize, "%s", v);
	return 0;
}

/* the text file xdb.conf: [rdb] sections of key = value lines */
static int
conf_file(const char *rdb, const char *key, char *out, int outsize)
{
	const char *path = getenv("XDB_CONF");
	FILE	   *f;
	char		line[2048];
	int			in = 0,
				found = 0;

	out[0] = '\0';
	f = fopen(path && path[0] ? path : "/QOpenSys/etc/xdb/xdb.conf", "rb");
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof line, f))
	{
		char	   *p = line,
				   *e;
		size_t		n = strlen(line);

		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
			line[--n] = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\0')
			continue;
		if (*p == '[')
		{
			e = strchr(p, ']');
			if (e)
				*e = '\0';
			in = (strcmp(p + 1, rdb) == 0 || strcmp(p + 1, "*") == 0);
			continue;
		}
		e = strchr(p, '=');
		if (!in || e == NULL)
			continue;
		*e = '\0';
		n = strlen(p);
		while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
			p[--n] = '\0';
		e++;
		while (*e == ' ' || *e == '\t')
			e++;
		if (strcmp(p, key) == 0)
		{
			snprintf(out, outsize, "%s", e);
			found = 1;
		}
	}
	fclose(f);
	return found ? 0 : 1;
}

/* ----------------------------------------------------------------- sessions */

static xsess *
sess_find(int actgrp, const char *rdb)
{
	int			i;

	for (i = 0; i < MAXSESS; i++)
		if (sess[i].used && sess[i].actgrp == actgrp && strcmp(sess[i].rdb, rdb) == 0)
			return &sess[i];
	return NULL;
}

static void
buf_free(xstmt *t)
{
	if (t->buf.release)
		t->buf.release(&t->buf);
	memset(&t->buf, 0, sizeof t->buf);
	t->bufrow = 0;
	free(t->sinkval);
	t->sinkval = NULL;
}

static void
cur_close(xsess *s, xstmt *t)
{
	buf_free(t);
	if (t->is_open && t->bcur && !t->lost)
		s->be->close(s->bc, t->bcur);
	t->bcur = NULL;
	t->is_open = 0;
	t->lost = 0;
}

static void
stmt_free(xsess *s, xstmt *t)
{
	cur_close(s, t);
	if (t->bstmt)
		s->be->stmt_free(s->bc, t->bstmt);
	free(t->sql);
	free(t->cols);
	xlob_plan_free(t->lob);
	memset(t, 0, sizeof *t);
}

static void
sess_close(xsess *s)
{
	int			i;

	if (s->bc)
	{
		for (i = 0; i < MAXSTMT; i++)
			if (s->st[i].used)
				stmt_free(s, &s->st[i]);
		s->be->disconnect(s->bc);
	}
	if (s->cn_pwd)
		memset(s->cn_pwd, 0, strlen(s->cn_pwd));
	free(s->cn_info);
	free(s->cn_user);
	free(s->cn_pwd);
	xlob_plan_free(s->lob_new);
	memset(s, 0, sizeof *s);
}

static xstmt *
stmt_by(xsess *s, int section, const char *pkg, const char *stmtname, const char *curname)
{
	int			i;
	xstmt	   *f = NULL;

	for (i = 0; i < MAXSTMT && f == NULL; i++)
	{
		xstmt	   *t = &s->st[i];

		if (!t->used)
			continue;
		if (curname && curname[0] && strcmp(t->curname, curname) == 0)
			f = t;
		else if (stmtname && stmtname[0] && strcmp(t->stmtname, stmtname) == 0 &&
				 (section == 0 || t->section == section))
			f = t;
	}
	if (f == NULL && !(stmtname && stmtname[0]))
		for (i = 0; i < MAXSTMT && f == NULL; i++)
			if (s->st[i].used && section != 0 && s->st[i].section == section &&
				strcmp(s->st[i].pkg, pkg) == 0)
				f = &s->st[i];
	if (f)
		f->used_at = ++s->clock;
	return f;
}

/* a free statement slot; when all are taken the least recently used closed one */
static xstmt *
stmt_new(xsess *s)
{
	int			i;
	xstmt	   *t = NULL;

	for (i = 0; i < MAXSTMT && t == NULL; i++)
		if (!s->st[i].used)
			t = &s->st[i];
	if (t == NULL)
	{
		for (i = 0; i < MAXSTMT; i++)
			if (!s->st[i].is_open && (t == NULL || s->st[i].used_at < t->used_at))
				t = &s->st[i];
		if (t == NULL)
			return NULL;
		stmt_free(s, t);
	}
	memset(t, 0, sizeof(xstmt));
	t->used = 1;
	t->used_at = ++s->clock;
	return t;
}

/* ------------------------------------------------- what the connection is */

/*
 * The statement XDB CONNECTION INFO (in interactive SQL: SELECT * FROM
 * XDB_CONNECTION_INFO) returns rows of AREA, NAME, VALUE with what the
 * driver knows about the connection it is asked on: where it leads, what
 * the server said about itself, the state of the session, the driver's own
 * counters, settings and limits.  It is for people who look for the cause
 * of a problem, and for programs that want to know what kind of server
 * they talk to.  Like XDB LAST ERROR it is answered here: the server is not
 * asked and the unit of work is not touched.
 *
 * The rows are not a promise: names come and go with the versions.
 */
#define CONN_INFO_COLS 3
#define CONN_INFO_ROWS 160
#define CI_AREA 12
#define CI_NAME 40
#define CI_VALUE 250

static const char *const built_at = __DATE__ " " __TIME__;

static int
is_conn_info(const char *sql)
{
	static const char *const a[] = {"XDB", "CONNECTION", "INFO", NULL};
	static const char *const b[] = {"SELECT", "*", "FROM", "XDB_CONNECTION_INFO", NULL};

	return is_words(sql, a) || is_words(sql, b);
}

static xbe_col *
conn_info_cols(void)
{
	static const struct
	{
		const char *name;
		int			len;
	}			d[CONN_INFO_COLS] = {{"AREA", CI_AREA}, {"NAME", CI_NAME}, {"VALUE", CI_VALUE}};
	xbe_col    *c = calloc(CONN_INFO_COLS, sizeof *c);
	int			i;

	for (i = 0; c && i < CONN_INFO_COLS; i++)
	{
		snprintf(c[i].name, sizeof c[i].name, "%s", d[i].name);
		c[i].kind = XK_VARCHAR;
		c[i].len = d[i].len;
	}
	return c;
}

/* the rows while they are collected: all text in one piece, a row is three offsets */
typedef struct
{
	char	   *text;
	size_t		len,
				cap;
	size_t		off[CONN_INFO_ROWS * CONN_INFO_COLS];
	int			n;
	int			bad;			/* no memory */
} xinfo;

static void
info_add(void *ctx, const char *area, const char *name, const char *value)
{
	static const int max[CONN_INFO_COLS] = {CI_AREA, CI_NAME, CI_VALUE};
	xinfo	   *x = ctx;
	const char *v[CONN_INFO_COLS];
	int			i;

	if (x->bad || x->n >= CONN_INFO_ROWS || name == NULL || name[0] == '\0')
		return;
	v[0] = area ? area : "";
	v[1] = name;
	v[2] = value ? value : "";
	for (i = 0; i < CONN_INFO_COLS; i++)
	{
		size_t		l = strlen(v[i]);

		/* no longer than the column, and no character cut in two */
		if (l > (size_t) max[i])
			for (l = max[i]; l > 0 && ((unsigned char) v[i][l] & 0xC0) == 0x80; l--)
				;
		if (x->len + l + 1 > x->cap)
		{
			size_t		nc = x->cap ? x->cap * 2 : 4096;
			char	   *nt;

			while (nc < x->len + l + 1)
				nc *= 2;
			nt = realloc(x->text, nc);
			if (nt == NULL)
			{
				x->bad = 1;
				return;
			}
			x->text = nt;
			x->cap = nc;
		}
		x->off[x->n * CONN_INFO_COLS + i] = x->len;
		memcpy(x->text + x->len, v[i], l);
		x->text[x->len + l] = '\0';
		x->len += l + 1;
	}
	x->n++;
}

static void
info_addf(xinfo *x, const char *area, const char *name, const char *fmt,...)
{
	char		v[400];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(v, sizeof v, fmt, ap);
	va_end(ap);
	info_add(x, area, name, v);
}

/* a point in time as the job sees it, "" for none */
static const char *
info_time(time_t t, char *out, int outsize)
{
	struct tm  *tm = t ? localtime(&t) : NULL;

	out[0] = '\0';
	if (tm)
		strftime(out, outsize, "%Y-%m-%d %H:%M:%S", tm);
	return out;
}

/* the rows of one area from a to x */
static void
info_take(xinfo *x, const xinfo *a, const char *area)
{
	int			i;

	for (i = 0; i < a->n; i++)
		if (strcmp(a->text + a->off[i * CONN_INFO_COLS], area) == 0)
			info_add(x, area, a->text + a->off[i * CONN_INFO_COLS + 1], a->text + a->off[i * CONN_INFO_COLS + 2]);
}

/* a number a backend reported, 0 when it did not */
static long long
info_num(const xinfo *a, const char *area, const char *name)
{
	int			i;

	for (i = 0; a && i < a->n; i++)
		if (strcmp(a->text + a->off[i * CONN_INFO_COLS], area) == 0 &&
			strcmp(a->text + a->off[i * CONN_INFO_COLS + 1], name) == 0)
			return atoll(a->text + a->off[i * CONN_INFO_COLS + 2]);
	return 0;
}

static void
conn_info_rows(xsess *s, xinfo *x)
{
	const char *type = s->be->name;
	xinfo	   *bx = calloc(1, sizeof *bx);
	char		v[300],
				lib[16];
	time_t		now = time(NULL);
	time_t		last;
	long long	nre,
				nlost;
	int			i,
				nst = 0,
				ncur = 0,
				nsess = 0;

	for (i = 0; i < MAXSTMT; i++)
	{
		nst += s->st[i].used && !s->st[i].virt;
		ncur += s->st[i].used && !s->st[i].virt && s->st[i].is_open;
	}
	for (i = 0; i < MAXSESS; i++)
		nsess += sess[i].used;

	info_add(x, "connection", "rdb", s->rdb);
	info_add(x, "connection", "database_type", strcmp(type, "pg") == 0 ? "PostgreSQL" :
			 strcmp(type, "mysql") == 0 ? "MySQL" : strcmp(type, "mssql") == 0 ? "SQL Server" : type);
	if (s->cn_info)
	{
		info_add(x, "connection", "host", xnet_kv(s->cn_info, "host", v, sizeof v) ? v : "");
		info_add(x, "connection", "port", xnet_kv(s->cn_info, "port", v, sizeof v) ? v : "(default)");
		info_add(x, "connection", "database", xnet_kv(s->cn_info, "dbname", v, sizeof v) ? v : "");
	}
	info_add(x, "connection", "user", s->cn_user ? s->cn_user : "");
	if (prf_of(s->rdb) && conf_get(s->rdb, "library", lib, sizeof lib) == 0)
		info_addf(x, "connection", "profile", "file XDBPRF in library %s", lib);
	else
		info_add(x, "connection", "profile", "text file xdb.conf");
	info_add(x, "connection", "connected", info_time(s->t_connect, v, sizeof v));
	info_addf(x, "connection", "activation_group", "%d", s->actgrp);

	/* the backend: the connection to the server, then the server */
	if (s->be->info && s->bc && bx)
	{
		s->be->info(s->bc, info_add, bx);
		info_take(x, bx, "link");
		info_take(x, bx, "server");
	}
	/* major * 100 + minor on every kind of server */
	info_addf(x, "server", "version_number", "%d", s->bc ? s->be->version(s->bc) : 0);
	/* connections opened again: by the core, and by a backend that does it itself */
	nre = s->n_revive + info_num(bx, "count", "reconnects");
	nlost = s->n_lost + info_num(bx, "count", "units_of_work_lost");
	last = (time_t) info_num(bx, "count", "last_reconnect");
	if (s->t_revive > last)
		last = s->t_revive;

	info_add(x, "session", "commit_mode", s->autocommit ? "every statement is committed at once" :
			 "commitment control");
	info_add(x, "session", "unit_of_work", s->uow_lost ? "its changes on the server are lost: COMMIT will fail" :
			 !s->in_txn ? "none" : s->uow_dirty ? "open, with changes" : "open");
	info_add(x, "session", "commit_resource", s->autocommit ? "not needed" :
			 s->fallback ? "refused by the system: commit after each statement" :
			 !s->cmt_registered ? "not registered yet" :
			 s->cmt_kind == XDB_CMT_LAST ? "last agent: a commit that fails here rolls back the whole unit of work" :
			 s->cmt_kind == XDB_CMT_ASKED ? "asked before the commit: a lost connection rolls back the whole unit of work" :
			 "only told: a commit that fails here does not stop the rest");
	if (s->cmt_note[0])
		info_add(x, "session", "commit_resource_refused", s->cmt_note);
	info_add(x, "session", "savepoint_per_statement", s->savepoints ? "on" : "off");
	info_addf(x, "session", "statements", "%d of %d", nst, MAXSTMT);
	info_addf(x, "session", "open_cursors", "%d", ncur);
	info_addf(x, "session", "seconds_since_last_statement", "%ld", (long) (now - s->last_use));
	info_add(x, "session", "date_format", s->datefmt);
	info_add(x, "session", "time_format", s->timefmt);

	info_addf(x, "counters", "reconnects", "%lld", nre);
	info_add(x, "counters", "last_reconnect", nre ? info_time(last, v, sizeof v) : "never");
	info_addf(x, "counters", "units_of_work_lost", "%lld", nlost);
	info_addf(x, "counters", "idle_checks", "%d", s->n_ping);
	info_addf(x, "counters", "statements_prepared", "%lld", s->n_prep);
	info_addf(x, "counters", "statements_executed", "%lld", s->n_exec);
	info_addf(x, "counters", "rows_changed", "%lld", s->n_changed);
	info_addf(x, "counters", "cursors_opened", "%lld", s->n_open);
	info_addf(x, "counters", "fetch_requests", "%lld", s->n_fetch);
	info_addf(x, "counters", "rows_fetched", "%lld", s->n_rows);
	info_addf(x, "counters", "fetches_from_server", "%lld", s->n_befetch);
	info_addf(x, "counters", "commits", "%lld", s->n_commit);
	info_addf(x, "counters", "commits_failed", "%lld", s->n_cfail);
	info_addf(x, "counters", "rollbacks", "%lld", s->n_rollback);
	info_addf(x, "counters", "errors", "%lld", s->n_err);

	info_addf(x, "options", "text_len", "%d", s->text_len);
	info_addf(x, "options", "numeric_scale", "%d", s->num_scale);
	if (s->char_ccsid)
		info_addf(x, "options", "ccsid", "%d", s->char_ccsid);
	else
		info_addf(x, "options", "ccsid", "%d (of the job)", job_ccsid_num());
	info_add(x, "options", "trim_char", s->trim_char ? "on" : "off");
	if (s->idle_check > 0)
		info_addf(x, "options", "idle_check", "%d seconds", s->idle_check);
	else
		info_add(x, "options", "idle_check", "off");
	info_addf(x, "options", "ping_timeout", "%d seconds", s->ping_timeout);
	info_addf(x, "options", "fetch_rows", "%d", s->fetch_rows);
	info_add(x, "options", "joblog", s->joblog == 2 ? "all" : s->joblog == 1 ? "long" : "off");
	info_add(x, "options", "trace", s->trace[0] ? s->trace : "off");
	if (bx)
	{
		info_take(x, bx, "options");	/* the ones only this kind of server has */
		free(bx->text);
		free(bx);
	}

	info_addf(x, "limits", "statements_per_connection", "%d", MAXSTMT);
	info_addf(x, "limits", "parameters_per_statement", "%d", MAXPARM);
	info_addf(x, "limits", "columns_per_result", "%d", MAXCOL);
	info_addf(x, "limits", "connections_per_job", "%d", MAXSESS);
	info_addf(x, "limits", "rows_per_exchange", "%d", s->fetch_rows);
	info_addf(x, "limits", "bytes_per_block", "%d", BLOCKMAX);

	info_add(x, "driver", "name", "XDBARDDRV");
	{
		char		raw[128];
		int			n = xdbprf_version(raw, sizeof raw);

		v[0] = '\0';
		if (n > 0)
			fld(raw, 0, n, v, sizeof v);
		info_add(x, "driver", "version", v[0] ? v : "not a package: built from the sources");
	}
	info_add(x, "driver", "built", built_at);
	xdbprf_mylib(lib);
	fld(lib, 0, 10, v, sizeof v);
	info_add(x, "driver", "library", v);
	info_addf(x, "driver", "job_ccsid", "%d", job_ccsid_num());
	info_addf(x, "driver", "connections_in_job", "%d of %d", nsess, MAXSESS);
}

static void
conn_info_release(xbe_batch *b)
{
	free(b->owner);
	free(b->val);
	memset(b, 0, sizeof *b);
}

/* all its rows in one batch */
static int
conn_info_batch(xsess *s, xstmt *t, xbe_batch *b)
{
	xinfo	   *x;
	int			i;

	memset(b, 0, sizeof *b);
	b->ncols = CONN_INFO_COLS;
	b->eof = 1;
	if (t->virt_done)
		return 0;
	t->virt_done = 1;
	x = calloc(1, sizeof *x);
	if (x == NULL)
		return 0;
	conn_info_rows(s, x);
	if (!x->bad && x->n > 0)
		b->val = calloc((size_t) x->n * CONN_INFO_COLS, sizeof(char *));
	if (b->val == NULL)
	{
		free(x->text);
		free(x);
		return 0;
	}
	for (i = 0; i < x->n * CONN_INFO_COLS; i++)
		b->val[i] = x->text + x->off[i];
	b->nrows = x->n;
	b->owner = x->text;
	b->release = conn_info_release;
	free(x);
	return 0;
}

/* ------------------------------------------------- connections that went away */

/*
 * A connection can be dropped while nobody uses it: by the server after its
 * idle timeout, by a firewall, by a restart.  The program that owns it still
 * expects it to be there.  So a connection that has been idle is tested
 * before a statement goes out on it, and replaced when it is gone; outside a
 * transaction the program notices nothing.
 */

/*
 * Opens a new connection in place of the one of s.  Returns -1 when that
 * fails (e says why), 1 when a unit of work was open and is lost, else 0.
 */
static int
sess_revive(xsess *s, xbe_err *e)
{
	xbe_opts	o;
	void	   *nb;
	int			had_txn = s->in_txn;
	int			i;

	memset(e, 0, sizeof *e);
	if (s->cn_info == NULL)
		return -1;
	memset(&o, 0, sizeof o);
	o.rdb = s->rdb;
	o.conninfo = s->cn_info;
	o.user = s->cn_user ? s->cn_user : "";
	o.password = s->cn_pwd ? s->cn_pwd : "";
	o.conf = conf_get;
	nb = s->be->connect(&o, e);
	if (nb == NULL)
	{
		tracef(s->trace, "  connection lost; a new one could not be opened: %s %s", e->sqlstate, e->msg);
		return -1;
	}
	/* cursors and prepared statements stayed with the old connection */
	for (i = 0; i < MAXSTMT; i++)
	{
		xstmt	   *t = &s->st[i];

		if (!t->used)
			continue;
		buf_free(t);
		if (t->is_open)
			t->lost = 1;
		t->bcur = NULL;
		if (t->bstmt)
		{
			s->be->stmt_free(s->bc, t->bstmt);
			t->bstmt = NULL;
			t->reprep = 1;
		}
	}
	s->be->disconnect(s->bc);
	s->bc = nb;
	if (had_txn)
		uow_lose(s, "the connection was lost");
	s->in_txn = 0;
	s->n_revive++;
	s->n_lost += had_txn ? 1 : 0;
	s->t_revive = time(NULL);
	tracef(s->trace, "  connection lost; reconnected%s", had_txn ? " (the open unit of work is lost)" : "");
	return had_txn ? 1 : 0;
}

/*
 * Before a statement: 0 = go on, -1 = ca is set and the statement must not
 * run (no connection, or the unit of work it belonged to was lost).
 */
static int
sess_check(xsess *s, char *ca)
{
	time_t		now = time(NULL);
	xbe_err		e;
	char		msg[400];
	int			r;

	if (s->be->ping == NULL || s->idle_check <= 0 || now - s->last_use < s->idle_check)
	{
		s->last_use = now;
		return 0;
	}
	r = s->be->ping(s->bc, s->ping_timeout);
	s->n_ping++;
	s->last_use = time(NULL);
	if (r == 0)
		return 0;
	r = sess_revive(s, &e);
	s->last_use = time(NULL);
	if (r < 0)
	{
		snprintf(msg, sizeof msg, "connection lost and not opened again: %.300s", e.msg);
		sqlca_set(ca, -30080, "08006", msg, 0);
		s->n_err++;
		return -1;
	}
	if (r > 0)
	{
		sqlca_set(ca, -911, "40001", "connection lost while idle; reconnected, the unit of work was rolled back", 0);
		s->n_err++;
		return -1;
	}
	return 0;
}

/*
 * After a statement failed with a connection error: when the connection is
 * really gone, open a new one now so that the next statement finds it.
 */
static void
sess_mend(xsess *s, xbe_err *e)
{
	xbe_err		e2;

	if (strncmp(e->sqlstate, "08", 2) != 0 || s->be->ping == NULL)
		return;
	if (s->be->ping(s->bc, s->ping_timeout) == 0)
		return;
	if (sess_revive(s, &e2) > 0)
		e->rolled_back = 1;
	s->last_use = time(NULL);
}

/* the backend's describe; it is told first which columns carry file values */
static int
be_describe(xsess *s, xstmt *t, xbe_col **cols, int *ncols, xbe_err *e)
{
	if (t->lob && t->lob->nout > 0 && s->be->lob)
	{
		int			c[XLOB_MAXOUT],
					i;

		for (i = 0; i < t->lob->nout; i++)
			c[i] = t->lob->out[i].col;
		s->be->lob->sinks(s->bc, c, t->lob->nout);
	}
	return s->be->describe(s->bc, t->sql, cols, ncols, &t->bstmt, e);
}

/* a statement prepared on a connection that was replaced since */
static void
stmt_reprep(xsess *s, xstmt *t)
{
	xbe_col    *cols = NULL;
	int			n = 0;
	xbe_err		e;

	if (t == NULL || !t->reprep || t->bstmt != NULL)
		return;
	t->reprep = 0;
	memset(&e, 0, sizeof e);
	if (be_describe(s, t, &cols, &n, &e) < 0)
		t->bstmt = NULL;
	free(cols);
}

/* ------------------------------------------------------------- transactions */

/* asks the server; 0 or -1, and err (when given) says what the server answered */
static int
txn_op(xsess *s, int op, xbe_err *err)
{
	xbe_err		e;
	int			rc;

	memset(&e, 0, sizeof e);
	rc = s->be->txn(s->bc, op, &e);
	if (rc < 0)
		tracef(s->trace, "  transaction request %d failed: %s %s", op, e.sqlstate, e.msg);
	if (err)
		*err = e;
	return rc;
}

/* the unit of work is over for the program: nothing of it is left to remember */
static void
uow_reset(xsess *s)
{
	s->in_txn = 0;
	s->uow_dirty = 0;
	s->uow_set = 0;
	s->uow_lost = 0;
	s->uow_why[0] = '\0';
}

/* cursors do not survive the end of the unit of work */
static void
cursors_end(xsess *s)
{
	int			i;

	for (i = 0; i < MAXSTMT; i++)
		if (s->st[i].used && s->st[i].is_open)
			cur_close(s, &s->st[i]);
}

/*
 * Called before a statement that may fail.  0 = go on; -1 = the statement
 * must not run and ca tells why: the server has lost the unit of work the
 * statement belongs to.
 */
static int
stmt_begin(xsess *s, char *ca)
{
	if (s->autocommit)
		return 0;
	if (s->fallback && s->in_txn)
	{
		int			i,
					open = 0;

		for (i = 0; i < MAXSTMT; i++)
			if (s->st[i].used && s->st[i].is_open)
				open = 1;
		if (!open)
		{
			txn_op(s, XT_COMMIT, NULL);
			uow_reset(s);
		}
	}
	if (!s->in_txn)
	{
		txn_op(s, XT_BEGIN, NULL);
		s->in_txn = 1;
		s->voted_no = 0;
	}
	else if (s->savepoints)
	{
		xbe_err		e;

		/*
		 * In a unit of work that is under way the savepoint is the first
		 * thing the server is asked.  A backend that finds the connection
		 * gone there and opens a new one has lost what the unit of work did
		 * before; without this the statement would run in a new transaction
		 * and nobody would know.
		 */
		if (txn_op(s, XT_SAVEPOINT, &e) < 0 && e.rolled_back)
		{
			sqlca_be(ca, s, &e);
			return -1;
		}
		return 0;
	}
	if (s->savepoints)
		txn_op(s, XT_SAVEPOINT, NULL);
	return 0;
}

static void
stmt_failed(xsess *s)
{
	if (!s->autocommit && s->in_txn && s->savepoints)
		txn_op(s, XT_ROLLBACK_TO, NULL);
}

/*
 * Words that tell that a statement which returns rows may also change
 * something, looked for outside literals, quoted names and comments.  A
 * unit of work that only read loses nothing with its connection, so its
 * COMMIT goes through; one that changed something must not.  In doubt the
 * answer is yes: a wrong yes costs a COMMIT that fails after a lost
 * connection although nothing was lost, a wrong no costs lost changes
 * nobody is told about.  What cannot be seen here: a function that changes
 * data and is called in a plain SELECT.
 */
static int
sql_may_change(const char *sql)
{
	static const char *const words[] = {
		"INSERT", "UPDATE", "DELETE", "MERGE", "INTO", "CALL", "EXEC", "EXECUTE", "NEXTVAL",
		"SETVAL", "CREATE", "DROP", "ALTER", "TRUNCATE", NULL
	};
	const char *p = sql;

	while (p && *p)
	{
		unsigned char c = (unsigned char) *p;

		if (c == '\'' || c == '"' || c == '`')
		{
			/* to the closing quote; a doubled one stays inside */
			for (p++; *p; p++)
				if ((unsigned char) *p == c)
				{
					if ((unsigned char) p[1] != c)
						break;
					p++;
				}
			if (*p)
				p++;
		}
		else if (c == '-' && p[1] == '-')
		{
			while (*p && *p != '\n')
				p++;
		}
		else if (c == '/' && p[1] == '*')
		{
			const char *e = strstr(p + 2, "*/");

			p = e ? e + 2 : p + strlen(p);
		}
		else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
		{
			const char *w = p;
			size_t		n;
			int			i;

			while (((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') || (*p >= '0' && *p <= '9') ||
				   *p == '_' || *p == '$' || *p == '#' || *p == '@')
				p++;
			n = (size_t) (p - w);
			for (i = 0; words[i]; i++)
				if (strlen(words[i]) == n)
				{
					size_t		k;

					for (k = 0; k < n && (w[k] & ~0x20) == words[i][k]; k++)
						;
					if (k == n)
						return 1;
				}
		}
		else if (c == '_' || c == '$' || c == '#' || c == '@' || (c >= '0' && c <= '9') || c >= 0x80)
		{
			/* a name or a number that does not start with a letter: none of the words */
			while (((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') || (*p >= '0' && *p <= '9') ||
				   *p == '_' || *p == '$' || *p == '#' || *p == '@' || (unsigned char) *p >= 0x80)
				p++;
		}
		else
			p++;
	}
	return 0;
}

static int	first_kw(const char *sql, const char *kw);

/* a statement ran in the unit of work: has it changed something? */
static void
uow_touch(xsess *s, const char *sql, int cursor)
{
	if (s->autocommit || s->uow_dirty)
		return;
	if (cursor ? sql_may_change(sql) : !(first_kw(sql, "SET") || first_kw(sql, "USE")))
	{
		s->uow_dirty = 1;
		tracef(s->trace, "  the unit of work has changes on the server now");
	}
	else if (!cursor)
		s->uow_set = 1;
}

/*
 * Is the connection there for a COMMIT?  A connection that was used a moment
 * ago is taken as alive, like before a statement; with always it is tested
 * anyway (unless option idle_check = 0 says never to test).
 */
static int
conn_alive(xsess *s, int always)
{
	if (s->be->ping == NULL || s->idle_check <= 0)
		return 1;
	if (!always && time(NULL) - s->last_use < s->idle_check)
		return 1;
	s->n_ping++;
	if (s->be->ping(s->bc, s->ping_timeout) != 0)
		return 0;
	s->last_use = time(NULL);
	return 1;
}

static void
uow_error(xbe_err *e, const char *state, const char *fmt, const char *detail)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", state);
	snprintf(e->msg, sizeof e->msg, fmt, detail);
}

/*
 * Before the commit, for a resource that is asked (XDB_CMT_ASKED): can this
 * connection commit?  0 = yes.  -1 = no, e says why, and the unit of work
 * is ended here, because the system rolls back the rest after a no.  The
 * connection is tested now, so that a server that went away is found while
 * everything can still be rolled back.  What stays open is the moment
 * between a yes and the commit itself.
 */
static int
uow_ready(xsess *s, xbe_err *e)
{
	int			gone = 0;

	memset(e, 0, sizeof *e);
	if (s->bc == NULL)
		return 0;
	if (s->uow_lost)
		uow_error(e, "40001", "COMMIT refused: what this unit of work changed on the server was lost earlier (%.200s)",
				  s->uow_why);
	else if (s->in_txn && s->uow_dirty && !conn_alive(s, 1))
	{
		uow_error(e, "40001", "COMMIT refused: the connection to the server is lost%s",
				  ", and what this unit of work changed there with it");
		s->n_lost++;
		gone = 1;
	}
	else
		return 0;
	cursors_end(s);
	if (gone || (s->in_txn && txn_op(s, XT_ROLLBACK, NULL) < 0))
		s->last_use = 0;		/* the next statement looks at the connection first */
	uow_reset(s);
	s->n_cfail++;
	tracef(s->trace, "COMMIT REFUSED");
	return -1;
}

/*
 * COMMIT on the server.  0 = what the unit of work changed there is
 * committed, or it changed nothing; -1 = it is not committed and e says
 * why.  Either way the unit of work is over afterwards.
 */
static int
uow_commit(xsess *s, xbe_err *e)
{
	int			rc = 0;

	memset(e, 0, sizeof *e);
	if (s->bc == NULL)
		return 0;
	cursors_end(s);
	if (s->uow_lost)
	{
		/* the program was told at the statement that found it; what it did afterwards must not stay */
		uow_error(e, "40001", "COMMIT refused: what this unit of work changed on the server was lost earlier (%.200s)",
				  s->uow_why);
		if (s->in_txn && txn_op(s, XT_ROLLBACK, NULL) < 0)
			s->last_use = 0;
		rc = -1;
	}
	else if (s->in_txn && !conn_alive(s, 0))
	{
		/* nothing is sent: the server cannot have committed, and the wait for an answer is spared */
		s->last_use = 0;
		if (s->uow_dirty)
		{
			uow_error(e, "40001", "COMMIT failed: the connection to the server was lost before it%s",
					  ", and what this unit of work changed there with it");
			s->n_lost++;
			rc = -1;
		}
		else
			tracef(s->trace, "  the unit of work only read: the lost connection takes nothing with it");
	}
	else if (s->in_txn)
	{
		xbe_err		b;

		rc = txn_op(s, XT_COMMIT, &b);
		if (rc < 0)
		{
			int			lost = strncmp(b.sqlstate, "08", 2) == 0;
			int			sure = b.rolled_back;	/* the server has said no: nothing is committed */

			if (!lost && s->be->ping && s->be->ping(s->bc, s->ping_timeout) != 0)
				lost = sure = 1;	/* an error and then no connection: the server's last words */
			if (!lost)
			{
				*e = b;
				snprintf(e->msg, sizeof e->msg, "COMMIT refused by the server: %.460s", b.msg);
				if (strlen(e->sqlstate) != 5)
					snprintf(e->sqlstate, sizeof e->sqlstate, "40000");
				txn_op(s, XT_ROLLBACK, NULL);	/* nothing may stay open on the server */
			}
			else
			{
				s->last_use = 0;	/* the next statement looks at the connection first */
				if (!s->uow_dirty)
				{
					tracef(s->trace, "  the unit of work only read: the lost connection takes nothing with it");
					rc = 0;
				}
				else if (sure)
					uow_error(e, "40001", "COMMIT failed: the server ended the session instead of committing (%.300s)",
							  b.msg);
				else
					uow_error(e, "08007", "COMMIT failed: the connection was lost while the server was asked, "
							  "whether it has committed is not known (%.300s)", b.msg);
				if (rc < 0)
					s->n_lost++;
			}
		}
	}
	uow_reset(s);
	if (rc == 0)
		s->n_commit++;
	else
		s->n_cfail++;
	tracef(s->trace, "%s", rc == 0 ? "COMMIT" : "COMMIT FAILED");
	return rc;
}

/* ROLLBACK on the server; a server that cannot be asked has lost the unit of work anyway */
static void
uow_rollback(xsess *s)
{
	if (s->bc == NULL)
		return;
	cursors_end(s);
	if (s->in_txn && (!conn_alive(s, 0) || txn_op(s, XT_ROLLBACK, NULL) < 0))
		s->last_use = 0;
	uow_reset(s);
	s->n_rollback++;
	tracef(s->trace, "ROLLBACK");
}

/*
 * A commit that did not take place on the server, for XDB LAST ERROR and
 * the job log.  told: the system was told in time, so the whole unit of work
 * is rolled back and the program's COMMIT fails.  Not told: the rest of the
 * unit of work is committed all the same, which is written to the job log
 * whatever option joblog says.
 */
static void
commit_failed(xsess *s, const xbe_err *e, int told)
{
	xbe_err		x = *e;
	size_t		n = strlen(x.msg);
	int			code = strcmp(x.sqlstate, "40001") == 0 ? -911 : state2code(x.sqlstate);

	snprintf(x.msg + n, sizeof x.msg - n, "%s", told ? "; the whole unit of work is rolled back" :
			 "; THE REST OF THE UNIT OF WORK IS COMMITTED (this connection could not stop the commit)");
	s->n_err++;
	tracef(s->trace, "  %s", x.msg);
	err_keep(code, x.sqlstate, &x, s->be->name, s->rdb, (told && s->joblog == 0) ? 0 : 2);
}

/*
 * The commitment resource has to be added from the caller's activation
 * group (this service program runs in its own), so the XDBARD program stub
 * does the QTNADDCR call: it asks xdb_commit_wanted() after every request
 * and reports back with xdb_commit_registered().  xdbard.h tells the kinds
 * of resource.
 */
static void
register_commit(xsess *s)
{
	if (!s->autocommit && !s->cmt_registered)
		s->cmt_wanted = 1;
}

static xsess *
sess_of(const char *in)
{
	char		rdb[20];

	fld(in, 8, 18, rdb, sizeof rdb);
	return sess_find(g32(in, 4), rdb);
}

static const char *
cmt_kind_name(int kind)
{
	return kind == XDB_CMT_LAST ? "last agent" : kind == XDB_CMT_ASKED ? "two-phase with prepare" :
		kind == XDB_CMT_TOLD ? "one-phase" : "none";
}

int
xdb_commit_wanted(const char *in, char *lib10, int *kind)
{
	xsess	   *s = sess_of(in);
	char		lib[32],
				val[16];

	if (s == NULL || !s->cmt_wanted || s->cmt_registered)
		return 0;
	/*
	 * The number goes into the resource (after activation group and RDB
	 * name), so the exit can tell this connection's resource from the one
	 * of an earlier connection with the same name.
	 */
	if (s->cmt_gen == 0)
		s->cmt_gen = ++cmt_seq;
	if (conf_get(s->rdb, "library", lib, sizeof lib) != 0)
		xdbprf_mylib(lib10);	/* the exit program is next to this service program */
	else
		setfld(lib10, 10, lib);
	/* option commit_vote = off: only told, as the driver was before it could vote */
	*kind = (conf_get(s->rdb, "commit_vote", val, sizeof val) == 0 && strcmp(val, "off") == 0) ?
		XDB_CMT_TOLD : XDB_CMT_LAST;
	return s->cmt_gen;
}

/* the system did not take a resource of this kind; the stub goes on with the next */
void
xdb_commit_refused(const char *in, int kind, const char *msgid, const char *data)
{
	xsess	   *s = sess_of(in);
	char		id[16];

	if (s == NULL)
		return;
	fld(msgid, 0, 7, id, sizeof id);
	tracef(s->trace, "  commit resource (%s) refused: %s (data %02X%02X%02X%02X %02X%02X%02X%02X)", cmt_kind_name(kind),
		   id, (unsigned char) data[0], (unsigned char) data[1], (unsigned char) data[2], (unsigned char) data[3],
		   (unsigned char) data[4], (unsigned char) data[5], (unsigned char) data[6], (unsigned char) data[7]);
	/* the message data of CPF8369 starts with its reason code, two bytes (13 = there is a last agent already) */
	{
		size_t		n = strlen(s->cmt_note);

		snprintf(s->cmt_note + n, sizeof s->cmt_note - n, "%s%s: %s reason %d", n ? "; " : "", cmt_kind_name(kind), id,
				 ((unsigned char) data[0] << 8) | (unsigned char) data[1]);
	}
}

void
xdb_commit_registered(const char *in, int kind, int handle)
{
	xsess	   *s = sess_of(in);

	if (s == NULL)
		return;
	s->cmt_registered = 1;
	s->cmt_wanted = 0;
	s->cmt_kind = kind;
	s->cmt_handle = handle;
	if (kind != XDB_CMT_NONE)
		tracef(s->trace, "  commit resource registered (%s)", cmt_kind_name(kind));
	else
	{
		tracef(s->trace, "  no commit resource: changes are committed at statement boundaries");
		s->fallback = 1;
		s->uow_lost = 0;
	}
}

/* where the removal of a resource is traced when its connection is closed already */
static char rm_trace[256];

int
xdb_commit_handle(const char *in, const char *fmtname)
{
	char		fmt[12];
	xsess	   *s;

	fld(fmtname, 0, 8, fmt, sizeof fmt);
	/* type 3 = the connection ends as part of a commit or rollback: no commit API may be called then */
	if (strcmp(fmt, "ARDI0100") != 0 || g32(in, 28) == 3)
		return 0;
	s = sess_of(in);
	if (s == NULL || s->cmt_kind == XDB_CMT_NONE)
		return 0;
	snprintf(rm_trace, sizeof rm_trace, "%s", s->trace);
	return s->cmt_handle;
}

void
xdb_commit_removed(int handle, int ok, const char *msgid)
{
	char		id[16];

	fld(msgid, 0, 7, id, sizeof id);
	if (ok)
		tracef(rm_trace, "  commit resource removed");
	else
		tracef(rm_trace, "  commit resource not removed now (%s): it goes at the next commit or rollback", id);
}

/* --------------------------------------------------------- statement text */

/* forget the file functions of a statement text that no statement took */
static void
lob_pending_clear(xsess *s)
{
	xlob_plan_free(s->lob_new);
	s->lob_new = NULL;
	s->lob_bad = 0;
}

/*
 * '?' and host-variable markers (:H) -> the backend's marker style
 * ("$%d", "?", "@P%d"), outside literals, quoted names and comments.
 */
static char *
rewrite_markers(const char *in, const char *fmt, int *nparams)
{
	size_t		l = strlen(in);
	char	   *out = malloc(l + 16 + l / 2 * 7);
	char	   *o = out;
	const char *p = in;
	int			n = 0;

	if (out == NULL)
		return NULL;
	while (*p)
	{
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			char		q = *p;

			*o++ = *p++;
			while (*p)
			{
				*o++ = *p;
				if (*p++ == q)
				{
					if (*p != q)
						break;
					*o++ = *p++;
				}
			}
			continue;
		}
		if (p[0] == '-' && p[1] == '-')
		{
			while (*p && *p != '\n')
				*o++ = *p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '*')
		{
			const char *e = strstr(p + 2, "*/");

			e = e ? e + 2 : p + strlen(p);
			memcpy(o, p, (size_t) (e - p));
			o += e - p;
			p = e;
			continue;
		}
		if (*p == '?')
		{
			o += sprintf(o, fmt, ++n);
			p++;
			continue;
		}
		if (p[0] == ':' && p[1] == 'H' && (p == in || p[-1] != ':') &&
			!((p[2] >= 'A' && p[2] <= 'Z') || (p[2] >= 'a' && p[2] <= 'z') ||
			  (p[2] >= '0' && p[2] <= '9') || p[2] == '_'))
		{
			const char *q = p + 2;

			o += sprintf(o, fmt, ++n);
			while (*q == ' ')
				q++;
			if (q[0] == ':' && q[1] == 'H')		/* indicator variable */
				p = q + 2;
			else
				p += 2;
			continue;
		}
		*o++ = *p++;
	}
	*o = '\0';
	while (o > out && (o[-1] == ' ' || o[-1] == ';' || o[-1] == '\n' || o[-1] == '\r'))
		*--o = '\0';
	if (nparams)
		*nparams = n;
	return out;
}

#ifdef XDB_LOBTEST
static int	lobt_strip_on(void);	/* after the experiment's switches, below */

static int
lobt_namechar(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		c == '_' || c == '$' || c == '#' || c == '@' || (c & 0x80);
}

/* end of a plain or quoted name starting at p; p itself when there is none */
static const char *
lobt_name_end(const char *p)
{
	if (*p == '"')
	{
		const char *q = strchr(p + 1, '"');

		return q ? q + 1 : p;
	}
	while (lobt_namechar((unsigned char) *p))
		p++;
	return p;
}

/* p after blanks, a period and blanks; NULL when there is no period */
static const char *
lobt_period(const char *p)
{
	while (*p == ' ')
		p++;
	if (*p != '.')
		return NULL;
	p++;
	while (*p == ' ')
		p++;
	return p;
}

/*
 * EXPERIMENT: "<rdb>.<schema>.<table>" -> "<schema>.<table>", in place,
 * outside literals, quoted names and comments.  Only a plain (unquoted)
 * first part equal to the RDB name, whatever its case, is taken away, and
 * only when a name, a period and another name follow.  Db2 writes the text
 * of a static statement with blanks around the periods ("RDB . S . T").
 */
static void
lobt_strip_rdb(char *sql, const char *rdb)
{
	size_t		rl = strlen(rdb);
	const char *p = sql;
	char	   *o = sql;

	while (rl > 0 && rdb[rl - 1] == ' ')
		rl--;
	if (rl == 0)
		return;
	while (*p)
	{
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			char		q = *p;

			*o++ = *p++;
			while (*p)
			{
				*o++ = *p;
				if (*p++ == q)
				{
					if (*p != q)
						break;
					*o++ = *p++;
				}
			}
			continue;
		}
		if (p[0] == '-' && p[1] == '-')
		{
			while (*p && *p != '\n')
				*o++ = *p++;
			continue;
		}
		if (p[0] == '/' && p[1] == '*')
		{
			const char *e = strstr(p + 2, "*/");

			e = e ? e + 2 : p + strlen(p);
			while (p < e)
				*o++ = *p++;
			continue;
		}
		if (lobt_namechar((unsigned char) *p))
		{
			const char *e = lobt_name_end(p);
			size_t		wl = (size_t) (e - p);
			const char *b = p;
			int			first;
			size_t		k;
			int			same;
			const char *n1;

			/* not a first part when a period (blanks allowed) stands before it */
			while (b > sql && b[-1] == ' ')
				b--;
			first = (b == sql || b[-1] != '.');
			n1 = lobt_period(e);
			same = (wl == rl && first && n1 != NULL);
			for (k = 0; same && k < rl; k++)
			{
				int			a = (unsigned char) p[k],
							c = (unsigned char) rdb[k];

				if (a >= 'a' && a <= 'z')
					a -= 32;
				if (c >= 'a' && c <= 'z')
					c -= 32;
				if (a != c)
					same = 0;
			}
			if (same)
			{
				const char *e1 = lobt_name_end(n1);
				const char *n2 = e1 > n1 ? lobt_period(e1) : NULL;

				if (n2 != NULL && lobt_name_end(n2) > n2)
				{
					p = n1;		/* drop "<rdb>." */
					continue;
				}
			}
			while (p < e)
				*o++ = *p++;
			continue;
		}
		*o++ = *p++;
	}
	*o = '\0';
}

/*
 * EXPERIMENT: the text of a static statement names its host variables
 * (": SCH", ": SCH : SCHIND") where a bound statement has :H.  A new string
 * with the backend's markers; the indicator variable of a host variable
 * (a second name right after the first) gets none.
 */
static char *
lobt_named_markers(const char *in, const char *fmt)
{
	size_t		l = strlen(in);
	char	   *out = malloc(l + 16 + l / 2 * 7);
	char	   *o = out;
	const char *p = in;
	int			n = 0;

	if (out == NULL)
		return NULL;
	while (*p)
	{
		if (*p == '\'' || *p == '"' || *p == '`')
		{
			char		q = *p;

			*o++ = *p++;
			while (*p)
			{
				*o++ = *p;
				if (*p++ == q)
				{
					if (*p != q)
						break;
					*o++ = *p++;
				}
			}
			continue;
		}
		if (*p == ':' && (p == in || p[-1] != ':') && p[1] != ':')
		{
			const char *q = p + 1;
			const char *e;

			while (*q == ' ')
				q++;
			e = lobt_name_end(q);
			if (e > q && *q != '"' && !(*q >= '0' && *q <= '9'))
			{
				o += sprintf(o, fmt, ++n);
				p = e;
				/* an indicator variable: blanks, a colon, a name */
				q = p;
				while (*q == ' ')
					q++;
				if (*q == ':' && q[1] != ':')
				{
					const char *q2 = q + 1;

					while (*q2 == ' ')
						q2++;
					e = lobt_name_end(q2);
					if (e > q2)
						p = e;
				}
				continue;
			}
		}
		*o++ = *p++;
	}
	*o = '\0';
	return out;
}
#endif

static char *
stmt_text(xsess *s, const char *in, int off, int len, int ccsid)
{
	char	   *u;
	char	   *r;
	int			n;

	if (off <= 0 || len <= 0)
		return NULL;
	u = malloc((size_t) len * 4 + 8);
	if (u == NULL)
		return NULL;
	n = ile_conv((ccsid == 0 || ccsid == 65535) ? JOB : ccsid, UTF8, in + off, len, u, (size_t) len * 4 + 4);
	if (n < 0)
	{
		free(u);
		return NULL;
	}
	u[n] = '\0';
#ifdef XDB_LOBTEST
	if (lobt_strip_on())		/* EXPERIMENT: three-part names */
	{
		lobt_strip_rdb(u, s->rdb);
		tracef(s->trace, "  EXPERIMENT strip: %.300s", u);
	}
#endif
	/* file functions (xdblob.c): their place in the text is taken by SQL of the backend */
	lob_pending_clear(s);
	{
		char	   *w = NULL;
		xlob_plan  *pl = NULL;
		int			rc = xlob_parse(u, s->be, s->bc, !s->autocommit, &w, &pl, &s->lob_err);

		if (rc < 0)
			s->lob_bad = 1;
		else if (rc > 0)
		{
			free(u);
			u = w;
			s->lob_new = pl;
		}
	}
	r = rewrite_markers(u, s->be->marker, NULL);
	free(u);
	return r;
}

/* ------------------------------------------------------ result description */

/* backend column descriptions -> Db2 types and row lengths */
static void
describe_cols(xsess *s, xstmt *t, const xbe_col *d, int n)
{
	int			i;

	free(t->cols);
	t->cols = NULL;
	t->ncols = n > MAXCOL ? MAXCOL : n;
	if (t->ncols == 0)
		return;
	t->cols = calloc(t->ncols, sizeof(xcol));
	for (i = 0; i < t->ncols; i++)
	{
		xcol	   *c = &t->cols[i];

		snprintf(c->name, sizeof c->name, "%s", d[i].name);
		c->kind = d[i].kind;
		switch (d[i].kind)
		{
			case XK_SMALLINT:
			case XK_BOOL:			/* boolean -> 0/1 */
				c->sqltype = T_SMALLINT;
				c->len = 2;
				break;
			case XK_INTEGER:
				c->sqltype = T_INTEGER;
				c->len = 4;
				break;
			case XK_BIGINT:
				c->sqltype = T_BIGINT;
				c->len = 8;
				break;
			case XK_DOUBLE:
				c->sqltype = T_FLOAT;
				c->len = 8;
				break;
			case XK_DECIMAL:
				c->sqltype = T_DECIMAL;
				if (d[i].prec > 0)
				{
					/*
					 * Db2 takes a packed decimal of DECMAX digits at most
					 * from a driver: with more, the DESCRIBE and the OPEN
					 * of the program end with SQL0804.  Servers have longer
					 * ones, and not only in tables: the SUM of a decimal
					 * column is numeric(38,s) on SQL Server and up to
					 * decimal(65,s) on MySQL.  Such a column is handed over
					 * with DECMAX digits and its scale; a value that needs
					 * more digits before the point than that leaves does
					 * not become another number, it is a value that cannot
					 * be converted (put_col), of its row only.
					 */
					c->prec = d[i].prec > DECMAX ? DECMAX : d[i].prec;
					c->scale = d[i].scale > c->prec ? c->prec : d[i].scale;
				}
				else
				{
					c->prec = DECMAX;
					c->scale = s->num_scale;
				}
				c->len = c->prec / 2 + 1;
				break;
			case XK_DATE:
				c->sqltype = T_DATE;
				c->len = 10;
				break;
			case XK_TIME:
				c->sqltype = T_TIME;
				c->len = 8;
				break;
			case XK_TIMESTAMP:
				c->sqltype = T_TIMESTAMP;
				c->len = 26;
				break;
			case XK_CHAR:
			default:
				c->sqltype = d[i].kind == XK_CHAR ? T_CHAR : T_VARCHAR;
				c->len = d[i].len > 0 ? d[i].len : s->text_len;
				if (c->len > 32000)
					c->len = 32000;
				break;
		}
	}
}

/* write an SQLDA describing the result columns; returns its length */
/*
 * EXPERIMENT (2026-11-14): can a driver hand large objects to Db2?  It
 * cannot: on 7.5 Db2 refuses every large object, locator and file
 * reference type in the SQLDA a driver returns (SQL0804, error type 2),
 * and a FETCH request carries no description of the program's variables.
 * The code is kept so that the answer can be checked again on a later
 * release; it is compiled only with DEFINE(XDB_LOBTEST) (build_xdb.sh
 * <kit> <lib> <ref> lobtest) and then acts only in a job that has data
 * area QTEMP/XDBLOBT (or, without it, environment variable XDB_LOBT).
 * Test programs: test/xdbt26.sqlrpgle (fetch) and
 * test/xdbt27.sqlrpgle (parameters).  In such a job the first column of
 * every query is described and returned as the data area says (read at
 * CONNECT):
 *   product=XXXvvrrm  product id given to Db2 at connect
 *   type=n     SQL type of the column (408 CLOB, 404 BLOB, 908 VARBINARY ...)
 *   da=1|2     1 = one SQLVAR, 2 = doubled SQLDA with SQLLONGLEN
 *   len=n      declared length      sqllen=n  SQLLEN of the first SQLVAR
 *   dlen=n     length of the value returned (letters A-Z, the last is #)
 *   row=0|1|2|4  value as it is / a 4 byte number only / with a 2 or 4 byte
 *              length in front
 *   ccsid=n    CCSID in the SQLVAR      ind=0  column not nullable
 * Second experiment, same switch (test/xdbt29*.sqlrpgle): three-part names.
 * Db2 sends "select ... from RDB.schema.table" of a program that never did
 * CONNECT to the driver as it is, over a connection it opens by itself; no
 * server knows the RDB name as the first part.
 *   strip=1    the driver takes "RDB." away where two more names follow
 * Without the define lt.on is never set and none of this runs.
 */
static struct
{
	int			on;
	int			type;
	int			da;
	int			len;
	int			sqllen;
	int			dlen;
	int			row;
	int			ccsid;
	int			ind;
	int			strip;
	char		product[12];
	char		spec[420];
}			lt;

#ifdef XDB_LOBTEST
static int
lobt_strip_on(void)
{
	return lt.on && lt.strip;
}
#endif

static void
lobt_load(void)
{
#ifdef XDB_LOBTEST
	char		j[420];
	char	   *tok;
	char		work[900];
	int			n = xdbprf_testspec(j, sizeof j);

	memset(&lt, 0, sizeof lt);
	lt.sqllen = -1;
	lt.ccsid = -1;
	lt.ind = 1;
	lt.da = 1;
	lt.row = 2;
	if (n <= 0)
		return;
	n = ile_conv(JOB, UTF8, j, n, lt.spec, sizeof lt.spec - 1);
	if (n <= 0)
		return;
	lt.spec[n] = '\0';
	lt.on = 1;
	snprintf(work, sizeof work, "%s", lt.spec);
	for (tok = strtok(work, " ;"); tok; tok = strtok(NULL, " ;"))
	{
		if (strncmp(tok, "product=", 8) == 0)
			snprintf(lt.product, sizeof lt.product, "%.8s", tok + 8);
		else if (strncmp(tok, "type=", 5) == 0)
			lt.type = atoi(tok + 5);
		else if (strncmp(tok, "da=", 3) == 0)
			lt.da = atoi(tok + 3);
		else if (strncmp(tok, "len=", 4) == 0)
			lt.len = atoi(tok + 4);
		else if (strncmp(tok, "sqllen=", 7) == 0)
			lt.sqllen = atoi(tok + 7);
		else if (strncmp(tok, "dlen=", 5) == 0)
			lt.dlen = atoi(tok + 5);
		else if (strncmp(tok, "row=", 4) == 0)
			lt.row = atoi(tok + 4);
		else if (strncmp(tok, "ccsid=", 6) == 0)
			lt.ccsid = atoi(tok + 6);
		else if (strncmp(tok, "ind=", 4) == 0)
			lt.ind = atoi(tok + 4);
		else if (strncmp(tok, "strip=", 6) == 0)
			lt.strip = atoi(tok + 6);
	}
	if (lt.dlen == 0)
		lt.dlen = lt.len;
#else
	lt.on = 0;
#endif
}

/* the test value of the first column into the row; -2 = no room */
static int
lobt_put(unsigned char *o, int room)
{
	static char letters[32];
	char		hash[4];
	int			p = 0,
				k;

	if (letters[0] == '\0')
		ile_conv(UTF8, JOB, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26, letters, sizeof letters);
	ile_conv(UTF8, JOB, "#", 1, hash, sizeof hash);
	if (lt.row == 1)
	{
		if (room < 4)
			return -2;
		o[0] = (unsigned char) (lt.dlen >> 24);
		o[1] = (unsigned char) (lt.dlen >> 16);
		o[2] = (unsigned char) (lt.dlen >> 8);
		o[3] = (unsigned char) lt.dlen;
		return 4;
	}
	if (room < lt.dlen + 4)
		return -2;
	if (lt.row == 2)
	{
		o[p++] = (unsigned char) (lt.dlen >> 8);
		o[p++] = (unsigned char) lt.dlen;
	}
	else if (lt.row == 4)
	{
		o[p++] = (unsigned char) (lt.dlen >> 24);
		o[p++] = (unsigned char) (lt.dlen >> 16);
		o[p++] = (unsigned char) (lt.dlen >> 8);
		o[p++] = (unsigned char) lt.dlen;
	}
	for (k = 0; k < lt.dlen; k++)
		o[p + k] = (unsigned char) letters[k % 26];
	if (lt.dlen > 0)
		o[p + lt.dlen - 1] = (unsigned char) hash[0];
	return p + lt.dlen;
}

static int
build_sqlda(xsess *s, xstmt *t, char *out, int outsize, int twice)
{
	int			n = t->ncols;
	int			ext = (lt.on && lt.type != 0 && lt.da >= 2 && n > 0);	/* EXPERIMENT */
	int			groups = (twice ? 2 : 1) + (ext ? 1 : 0);
	int			total = 16 + groups * n * (int) sizeof(xsqlvar);
	int			i;
	short		sh;

	if (total > outsize)
		return -1;
	memset(out, 0, total);
	setfld(out, 8, "SQLDA");
	if (ext)
	{
		/* 7th byte: the number of SQLVAR sets, '2' = doubled */
		char		d[4];

		ile_conv(UTF8, JOB, groups == 3 ? "3" : "2", 1, d, sizeof d);
		out[6] = d[0];
	}
	memcpy(out + 8, &total, 4);
	sh = (short) (groups * n);
	memcpy(out + 12, &sh, 2);
	sh = (short) n;
	memcpy(out + 14, &sh, 2);
	for (i = 0; i < n; i++)
	{
		unsigned char *v = (unsigned char *) out + 16 + i * sizeof(xsqlvar);
		xcol	   *c = &t->cols[i];
		char		nm[64];
		int			nl;

		sh = (short) (c->sqltype + 1);	/* always nullable */
		memcpy(v + offsetof(xsqlvar, sqltype), &sh, 2);
		sh = (short) (c->sqltype == T_DECIMAL ? c->prec * 256 + c->scale : c->len);
		memcpy(v + offsetof(xsqlvar, sqllen), &sh, 2);
		if (lt.on && lt.type != 0 && i == 0)
		{
			/* EXPERIMENT: the first column as the data area says */
			sh = (short) (lt.type + (lt.ind ? 1 : 0));
			memcpy(v + offsetof(xsqlvar, sqltype), &sh, 2);
			sh = (short) (lt.sqllen >= 0 ? lt.sqllen : ext ? 0 : lt.len > 32767 ? 32767 : lt.len);
			memcpy(v + offsetof(xsqlvar, sqllen), &sh, 2);
			put16be(v + offsetof(xsqlvar, sqldata) + 2, lt.ccsid >= 0 ? lt.ccsid : s->char_ccsid);
			if (ext)
			{
				int			ll = lt.len;

				memcpy(v + n * sizeof(xsqlvar), &ll, 4);	/* SQLLONGLEN of the second entry */
			}
		}
		else if (c->sqltype == T_CHAR || c->sqltype == T_VARCHAR)
			put16be(v + offsetof(xsqlvar, sqldata) + 2, s->char_ccsid);
		else if (c->sqltype == T_DATE || c->sqltype == T_TIME || c->sqltype == T_TIMESTAMP)
			put16be(v + offsetof(xsqlvar, sqldata) + 2, job_ccsid_num());	/* Db2: SQL0189 without it */
		nl = ile_conv(UTF8, JOB, c->name, strlen(c->name), nm, sizeof nm);
		if (nl < 0)
			nl = 0;
		if (nl > 30)
			nl = 30;
		sh = (short) nl;
		memcpy(v + offsetof(xsqlvar, sqlname), &sh, 2);
		memcpy(v + offsetof(xsqlvar, sqlname) + 2, nm, nl);
		if (twice)
		{
			/* labels: the last set */
			unsigned char *w = v + (groups - 1) * n * sizeof(xsqlvar);

			memcpy(w + offsetof(xsqlvar, sqlname), &sh, 2);
			memcpy(w + offsetof(xsqlvar, sqlname) + 2, nm, nl);
		}
	}
	return total;
}

/* ------------------------------------------------------- value conversions */

/* decimal text -> packed decimal; returns 0, or -1 on overflow/bad input */
static int
to_packed(const char *txt, int prec, int scale, unsigned char *out)
{
	char		dig[80];
	int			nd = prec | 1;	/* digits stored (odd) */
	int			neg = 0,
				i,
				ip = 0,
				fp = 0;
	const char *p = txt;
	const char *dot;
	char		ipart[80],
				fpart[80];

	while (*p == ' ')
		p++;
	if (*p == '-')
	{
		neg = 1;
		p++;
	}
	else if (*p == '+')
		p++;
	dot = strchr(p, '.');
	for (; *p && p != dot; p++)
	{
		if (*p < '0' || *p > '9' || ip >= 70)
			return -1;
		ipart[ip++] = *p;
	}
	if (dot)
		for (p = dot + 1; *p; p++)
		{
			if (*p < '0' || *p > '9')
				return -1;
			if (fp < scale)
				fpart[fp++] = *p;
		}
	while (fp < scale)
		fpart[fp++] = '0';
	i = 0;
	while (i < ip - 1 && ipart[i] == '0')
		i++;
	if (ip - i > prec - scale && !(ip - i == 1 && ipart[i] == '0'))
		return -1;
	memset(dig, '0', nd);
	memcpy(dig + nd - scale, fpart, scale);
	if (ip - i <= nd - scale)
		memcpy(dig + nd - scale - (ip - i), ipart + i, ip - i);
	memset(out, 0, nd / 2 + 1);
	for (i = 0; i < nd; i++)
	{
		int			d = dig[i] - '0';

		if (i % 2 == 0)
			out[i / 2] |= (unsigned char) (d << 4);
		else
			out[i / 2] |= (unsigned char) d;
	}
	out[nd / 2] |= neg ? 0x0D : 0x0F;
	return 0;
}

static void
from_packed(const unsigned char *in, int prec, int scale, int zoned, char *out)
{
	int			nd = zoned ? prec : (prec | 1);
	int			bytes = zoned ? prec : prec / 2 + 1;
	int			sign = zoned ? (in[bytes - 1] >> 4) : (in[bytes - 1] & 0x0F);
	char	   *o = out;
	int			i,
				started = 0;

	if (sign == 0x0D || sign == 0x0B)
		*o++ = '-';
	for (i = 0; i < nd; i++)
	{
		int			d = zoned ? (in[i] & 0x0F) : ((i % 2 == 0) ? (in[i / 2] >> 4) : (in[i / 2] & 0x0F));

		if (i == nd - scale)
		{
			if (!started)
				*o++ = '0';
			*o++ = '.';
			started = 1;
		}
		if (d != 0 || started || i == nd - scale - 1)
		{
			*o++ = (char) ('0' + d);
			started = 1;
		}
	}
	if (!started)
		*o++ = '0';
	*o = '\0';
}

/* one column value (UTF-8 text) -> row bytes; returns bytes written or -1 */
static int
put_col(xsess *s, xcol *c, const char *v, unsigned char *o, int room)
{
	char		tmp[64];
	char	   *conv;
	int			n;
	long long	ll;

	/* fixed-length types need their full length; VARCHAR is checked once its size is known */
	if (c->sqltype != T_VARCHAR && room < c->len)
		return -2;
	switch (c->sqltype)
	{
		case T_SMALLINT:
		case T_INTEGER:
		case T_BIGINT:
			if (c->kind == XK_BOOL)
				ll = (v[0] == 't' || v[0] == 'T' || v[0] == '1');
			else
				ll = strtoll(v, NULL, 10);
			for (n = c->len - 1; n >= 0; n--)
			{
				o[n] = (unsigned char) (ll & 0xFF);
				ll >>= 8;
			}
			return c->len;
		case T_FLOAT:
			{
				double		d = strtod(v, NULL);
				unsigned char b[8];
				int			one = 1;

				memcpy(b, &d, 8);
				if (*(char *) &one)
					for (n = 0; n < 8; n++)
						o[n] = b[7 - n];
				else
					memcpy(o, b, 8);
				return 8;
			}
		case T_DECIMAL:
			if (to_packed(v, c->prec, c->scale, o) < 0)
				return -1;
			return c->len;
		case T_DATE:
			if (strlen(v) < 10 || v[4] != '-')
				return -1;
			if (strcmp(s->datefmt, "*EUR") == 0)
				snprintf(tmp, sizeof tmp, "%.2s.%.2s.%.4s", v + 8, v + 5, v);
			else if (strcmp(s->datefmt, "*USA") == 0)
				snprintf(tmp, sizeof tmp, "%.2s/%.2s/%.4s", v + 5, v + 8, v);
			else
				snprintf(tmp, sizeof tmp, "%.10s", v);
			return ile_conv(UTF8, JOB, tmp, 10, (char *) o, room) == 10 ? 10 : -1;
		case T_TIME:
			if (strlen(v) < 8 || v[2] != ':')
				return -1;
			if (strcmp(s->timefmt, "*JIS") == 0)
				snprintf(tmp, sizeof tmp, "%.8s", v);
			else
				snprintf(tmp, sizeof tmp, "%.2s.%.2s.%.2s", v, v + 3, v + 6);
			return ile_conv(UTF8, JOB, tmp, 8, (char *) o, room) == 8 ? 8 : -1;
		case T_TIMESTAMP:
			{
				char		fr[8] = "000000";
				const char *f = strchr(v, '.');

				if (strlen(v) < 19 || v[4] != '-')
					return -1;
				if (f)
					for (n = 0; n < 6 && f[1 + n] >= '0' && f[1 + n] <= '9'; n++)
						fr[n] = f[1 + n];
				snprintf(tmp, sizeof tmp, "%.10s-%.2s.%.2s.%.2s.%.6s", v, v + 11, v + 14, v + 17, fr);
				return ile_conv(UTF8, JOB, tmp, 26, (char *) o, room) == 26 ? 26 : -1;
			}
		default:
			n = (int) strlen(v);
			conv = malloc((size_t) n * 2 + 8);
			if (conv == NULL)
				return -1;
			n = ile_conv(UTF8, s->char_ccsid, v, n, conv, (size_t) n * 2 + 4);
			if (n < 0)
			{
				free(conv);
				return -1;
			}
			if (n > c->len)
				n = c->len;
			if (c->sqltype == T_CHAR)
			{
				memset(o, s->char_ccsid == UTF8 ? ' ' : blank(), c->len);
				memcpy(o, conv, n);
				free(conv);
				return c->len;
			}
			if (room < n + 2)
			{
				free(conv);
				return -2;
			}
			put16be(o, n);
			memcpy(o + 2, conv, n);
			free(conv);
			return n + 2;
	}
}

/* host variable -> UTF-8 text parameter (malloc'ed), NULL for SQL NULL */
static char *
get_parm(xsess *s, xsqlvar *v, int *kind, int *err)
{
	int			type = v->sqltype & ~1;
	int			len = (unsigned short) v->sqllen;
	unsigned char *d = v->sqldata;
	int			ccsid = (v->sqlname.data[2] << 8) | v->sqlname.data[3];
	char	   *out;
	long long	ll = 0;
	int			i;

	*err = 0;
	switch (type)
	{
		case T_SMALLINT:
			*kind = XK_SMALLINT;
			break;
		case T_INTEGER:
			*kind = XK_INTEGER;
			break;
		case T_BIGINT:
			*kind = XK_BIGINT;
			break;
		case T_FLOAT:
			*kind = XK_DOUBLE;
			break;
		case T_DECIMAL:
		case T_ZONED:
			*kind = XK_DECIMAL;
			break;
		case T_DATE:
			*kind = XK_DATE;
			break;
		case T_TIME:
			*kind = XK_TIME;
			break;
		case T_TIMESTAMP:
			*kind = XK_TIMESTAMP;
			break;
		case T_CHAR:
			*kind = XK_CHAR;
			break;
		default:
			*kind = XK_VARCHAR;
			break;
	}
	if ((v->sqltype & 1) && v->sqlind && *v->sqlind < 0)
		return NULL;
	if (ccsid == 0 || ccsid == 65535)
		ccsid = JOB;
	switch (type)
	{
		case T_SMALLINT:
		case T_INTEGER:
		case T_BIGINT:
			ll = (d[0] & 0x80) ? -1 : 0;
			for (i = 0; i < len; i++)
				ll = (ll << 8) | d[i];
			out = malloc(32);
			snprintf(out, 32, "%lld", ll);
			return out;
		case T_FLOAT:
			{
				double		dv;
				float		fv;
				unsigned char b[8];
				int			one = 1;

				for (i = 0; i < len; i++)
					b[i] = *(char *) &one ? d[len - 1 - i] : d[i];
				if (len == 4)
				{
					memcpy(&fv, b, 4);
					dv = fv;
				}
				else
					memcpy(&dv, b, 8);
				out = malloc(40);
				snprintf(out, 40, "%.17g", dv);
				return out;
			}
		case T_DECIMAL:
		case T_ZONED:
			out = malloc(80);
			from_packed(d, (len >> 8) & 0xFF, len & 0xFF, type == T_ZONED, out);
			return out;
		case T_VARCHAR:
		case T_LONGVARCHAR:
			len = (d[0] << 8) | d[1];
			d += 2;
			/* FALLTHROUGH */
		case T_CHAR:
		case T_DATE:
		case T_TIME:
		case T_TIMESTAMP:
			out = malloc((size_t) len * 4 + 8);
			i = ile_conv(ccsid, UTF8, (char *) d, len, out, (size_t) len * 4 + 4);
			if (i < 0)
			{
				free(out);
				*err = 1;
				return NULL;
			}
			out[i] = '\0';
			if (type == T_CHAR && s->trim_char)
				while (i > 0 && out[i - 1] == ' ')
					out[--i] = '\0';
			if (type == T_DATE && i == 10 && out[2] == '.')		/* dd.mm.yyyy */
			{
				char		t[12];

				snprintf(t, sizeof t, "%.4s-%.2s-%.2s", out + 6, out + 3, out);
				strcpy(out, t);
			}
			else if (type == T_DATE && i == 10 && out[2] == '/')	/* mm/dd/yyyy */
			{
				char		t[12];

				snprintf(t, sizeof t, "%.4s-%.2s-%.2s", out + 6, out, out + 3);
				strcpy(out, t);
			}
			else if (type == T_TIME && i == 8 && out[2] == '.')
				out[2] = out[5] = ':';
			else if (type == T_TIMESTAMP && i >= 19 && out[10] == '-')
			{
				out[10] = ' ';
				out[13] = out[16] = ':';
			}
			return out;
		case T_GRAPHIC:
		case T_VARGRAPHIC:
			if (type == T_VARGRAPHIC)
			{
				len = (d[0] << 8) | d[1];
				d += 2;
			}
			out = malloc((size_t) len * 4 + 8);
			i = ile_conv(ccsid == JOB ? 1200 : ccsid, UTF8, (char *) d, (size_t) len * 2, out, (size_t) len * 4 + 4);
			if (i < 0)
			{
				free(out);
				*err = 1;
				return NULL;
			}
			out[i] = '\0';
			return out;
		default:
			/* a type the driver does not take (large objects and others) */
			s->bad_type = v->sqltype;
			*err = 1;
			return NULL;
	}
}

/* n bytes at p as hex digits, for the trace */
static const char *
hexof(const void *p, int n, char *out, int outsize)
{
	int			k;

	out[0] = '\0';
	for (k = 0; k < n && k * 2 + 3 <= outsize; k++)
		snprintf(out + k * 2, 3, "%02X", ((const unsigned char *) p)[k]);
	return out;
}

/*
 * Trace of a host variable the driver refuses: what Db2 passes for it.  A
 * large object has a second entry after the sqld first ones (length and
 * type name) when the SQLDA is doubled.
 */
static void
trace_bad_parm(xsess *s, const xsqlda *da, int dalen, int i)
{
	const xsqlvar *v = &da->sqlvar[i];
	char		hex[400];
	int			t = v->sqltype & ~1;
	int			second = 16 + (da->sqld + i) * (int) sizeof(xsqlvar);
	int			nd;

	if (s->trace[0] == '\0')
		return;
	tracef(s->trace, "  host variable %d: SQL type %d length %d is not supported", i + 1, v->sqltype, v->sqllen);
	tracef(s->trace, "    sqlda  %s sqldabc=%d sqln=%d sqld=%d (given length %d)",
		   hexof(da, 8, hex, sizeof hex), da->sqldabc, da->sqln, da->sqld, dalen);
	tracef(s->trace, "    sqlvar %s", hexof(v, sizeof(xsqlvar), hex, sizeof hex));
	/*
	 * Db2 gives the length of the first set of entries only; SQLDABC and
	 * the input length tell whether the second set is there all the same.
	 */
	tracef(s->trace, "    input format %d bytes, SQLDA at %d", s->in_len, (int) ((const char *) da - s->in_base));
	if (da->sqln >= 2 * da->sqld && da->sqldabc >= second + (int) sizeof(xsqlvar) &&
		(int) ((const char *) da - s->in_base) + second + (int) sizeof(xsqlvar) <= s->in_len)
		tracef(s->trace, "    second %s", hexof((const char *) da + second, sizeof(xsqlvar), hex, sizeof hex));
	else
		tracef(s->trace, "    second entry not within the input format");
	{
		/* what follows the SQLDA in the input format: the values, it seems */
		int			at = (int) ((const char *) da - s->in_base) + dalen;
		int			m = s->in_len - at;

		if (m > 64)
			m = 64;
		if (m > 0)
			tracef(s->trace, "    after the SQLDA, at %d of %d: %s", at, s->in_len,
				   hexof(s->in_base + at, m, hex, sizeof hex));
	}
	/* a locator is 4 bytes, a file reference 267, a large object 4 and the value */
	nd = (t >= 960 && t <= 969) ? 4 : (t >= 916 && t <= 925) ? 48 : 16;
	if (v->sqldata != NULL)
	{
		long		at = (long) ((const char *) v->sqldata - s->in_base);

		tracef(s->trace, "    data   %s", hexof(v->sqldata, nd, hex, sizeof hex));
		if (at >= 0 && at < s->in_len)
			tracef(s->trace, "    SQLDATA points into the input format, at %ld", at);
		else
			tracef(s->trace, "    SQLDATA points outside the input format");
	}
	else
		tracef(s->trace, "    data   (no pointer)");
}

static int
collect_parms(xsess *s, const char *in, int off, int len, char **vals, int *kinds, int *err)
{
	xsqlda	   *da;
	int			i,
				n;

	*err = 0;
	s->bad_type = 0;
	if (off <= 0 || len <= 0)
		return 0;
	da = (xsqlda *) (in + off);
	n = da->sqld;
	if (n > MAXPARM)
		n = MAXPARM;
	for (i = 0; i < n; i++)
	{
		int			e;

		vals[i] = get_parm(s, &da->sqlvar[i], &kinds[i], &e);
		if (e)
		{
			if (*err == 0 && s->bad_type != 0)
				trace_bad_parm(s, da, len, i);
			*err = i + 1;
		}
	}
	return n;
}

static void
free_parms(char **vals, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		free(vals[i]);
}

/* ---------------------------------------------------------------- handlers */

static const xbe_driver *
backend_by(const char *name)
{
	if (name[0] == '\0' || strcmp(name, "pg") == 0 || strcmp(name, "postgres") == 0 ||
		strcmp(name, "postgresql") == 0)
		return &xbe_pg;
#ifndef XDB_NO_MYSQL
	if (strcmp(name, "mysql") == 0 || strcmp(name, "mariadb") == 0)
		return &xbe_mysql;
#endif
#ifndef XDB_NO_MSSQL
	if (strcmp(name, "mssql") == 0 || strcmp(name, "sqlserver") == 0 || strcmp(name, "tds") == 0)
		return &xbe_mssql;
#endif
	return NULL;
}

/* first keyword of a statement is kw (upper case)? */
static int
first_kw(const char *sql, const char *kw)
{
	size_t		i,
				l = strlen(kw);

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
		if ((sql[i] & ~0x20) != kw[i])
			return 0;
	return !((sql[l] >= 'A' && sql[l] <= 'Z') || (sql[l] >= 'a' && sql[l] <= 'z') || sql[l] == '_');
}

/*
 * Why a connect failed goes to the job log whatever its length: Db2 shows
 * its own text for the SQLCODE, and without a connection XDB_LAST_ERROR
 * cannot be asked.  Option joblog = off keeps it out.
 */
static int
connect_log(const char *rdb)
{
	char		v[32];

	return (conf_get(rdb, "joblog", v, sizeof v) == 0 && strcmp(v, "off") == 0) ? 0 : 2;
}

/*
 * 1 = the user of the job, or one of its groups, is named in list (UTF-8,
 * names separated by blanks, or *ALL).  who gets the user of the job.
 */
static int
user_allowed(const char *list, char *who, int whosize)
{
	char		l[1100];
	char		w[10];
	int			n,
				ok;

	n = ile_conv(UTF8, JOB, list, strlen(list), l, sizeof l - 1);
	if (n < 0)
		n = 0;
	l[n] = '\0';
	ok = xdbprf_user_in(l, w);
	n = ile_conv(JOB, UTF8, w, 10, who, whosize - 1);
	if (n < 0)
		n = 0;
	who[n] = '\0';
	while (n > 0 && who[n - 1] == ' ')
		who[--n] = '\0';
	return ok;
}

static int
do_connect(const char *in, char *ca, char *out, int *outlen)
{
	char		rdb[20],
				user[64],
				pwd[128],
				val[1024],
				conninfo[2048];
	int			actgrp = g32(in, 4);
	xsess	   *s;
	int			i,
				ver;
	int			given,
				ulen,
				plen;
	const xbe_driver *be;
	xbe_opts	o;
	xbe_err		e;

	fld(in, 8, 18, rdb, sizeof rdb);
	fld(in, 76, 10, user, sizeof user);
	fld(in, 86, 10, pwd, sizeof pwd);
	lobt_load();				/* EXPERIMENT */
	s = sess_find(actgrp, rdb);
	if (s)
		sess_close(s);
	for (i = 0, s = NULL; i < MAXSESS; i++)
		if (!sess[i].used)
		{
			s = &sess[i];
			break;
		}
	/* the output format must be valid even when the connect fails */
	setfld(out, 3, "XDB");
	setfld(out + 3, 2, "00");
	setfld(out + 5, 2, "00");
	setfld(out + 7, 1, "0");
	setfld(out + 8, 10, user);
	setfld(out + 18, 1, "1");
	setfld(out + 19, 1, "0");
	*outlen = 20;
	if (s == NULL)
	{
		sqlca_set(ca, -30080, "08001", "too many connections", 0);
		return 0;
	}
	memset(s, 0, sizeof *s);
	if (prf_load(rdb) == PRF_DISABLED)
	{
		sqlca_set(ca, -30061, "08004", "connection profile is disabled (WRKXDBPRF)", 0);
		return 0;
	}
	if (conf_get(rdb, "conninfo", conninfo, sizeof conninfo) != 0)
	{
		sqlca_set(ca, -30061, "08004", "no connection profile for this RDB (WRKXDBPRF)", 0);
		return 0;
	}
	conf_get(rdb, "backend", val, sizeof val);
	be = backend_by(val);
	if (be == NULL)
	{
		sqlca_set(ca, -30061, "08004", "unknown database type in the connection profile", 0);
		return 0;
	}
	/* who may use the profile: user or group profiles, option allowed_users */
	if (conf_get(rdb, "allowed_users", val, sizeof val) == 0 && val[0] != '\0')
	{
		char		who[32];

		if (!user_allowed(val, who, sizeof who))
		{
			memset(&e, 0, sizeof e);
			snprintf(e.sqlstate, sizeof e.sqlstate, "28000");
			snprintf(e.msg, sizeof e.msg,
					 "user %s is not among the users allowed for connection profile %s (WRKXDBPRF)",
					 who[0] ? who : "of the job", rdb);
			if (conf_get(rdb, "trace", val, sizeof val) == 0)
				tracef(val, "ARCN0100 rdb=%s refused: %s", rdb, e.msg);
			sqlca_set(ca, -30082, "28000", e.msg, 0);
			err_keep(-30082, "28000", &e, be->name, rdb, connect_log(rdb));
			prf_forget(rdb);
			return 0;
		}
	}
	given = (user[0] != '\0' && pwd[0] != '\0');
	ulen = (int) strlen(user);
	plen = (int) strlen(pwd);
	if (user[0] == '\0' || pwd[0] == '\0')
	{
		/* no USER/USING on the CONNECT: Db2 passes the job user and no password */
		if (conf_get(rdb, "user", val, sizeof val) == 0)
			snprintf(user, sizeof user, "%.63s", val);
		else
			for (i = 0; user[i]; i++)
				if (user[i] >= 'A' && user[i] <= 'Z')
					user[i] = (char) (user[i] + 32);
		conf_get(rdb, "password", pwd, sizeof pwd);
		if (conf_get(rdb, "password_error", val, sizeof val) == 0)
		{
			sqlca_set(ca, -30082, "08001", "stored password of the profile cannot be read (XDBVLDL)", 0);
			return 0;
		}
	}
	else if (conf_get(rdb, "user_case", val, sizeof val) != 0 || strcmp(val, "asis") != 0)
		for (i = 0; user[i]; i++)
			if (user[i] >= 'A' && user[i] <= 'Z')
				user[i] = (char) (user[i] + 32);
	s->used = 1;
	s->actgrp = actgrp;
	s->be = be;
	snprintf(s->rdb, sizeof s->rdb, "%s", rdb);
	conf_get(rdb, "trace", s->trace, sizeof s->trace);
	snprintf(gtrace, sizeof gtrace, "%s", s->trace);
	s->text_len = conf_get(rdb, "text_len", val, sizeof val) == 0 ? atoi(val) : 1000;
	s->num_scale = conf_get(rdb, "numeric_scale", val, sizeof val) == 0 ? atoi(val) : 6;
	s->fetch_rows = conf_get(rdb, "fetch_rows", val, sizeof val) == 0 && atoi(val) > 0 ? atoi(val) : FETCHROWS;
	if (s->fetch_rows > FETCHROWS_MAX)
		s->fetch_rows = FETCHROWS_MAX;
	s->savepoints = be->needs_savepoints &&
		!(conf_get(rdb, "savepoints", val, sizeof val) == 0 && strcmp(val, "off") == 0);
	s->trim_char = !(conf_get(rdb, "trim_char", val, sizeof val) == 0 && strcmp(val, "off") == 0);
	s->char_ccsid = conf_get(rdb, "ccsid", val, sizeof val) == 0 ? atoi(val) : 0;
	s->autocommit = 1;
	snprintf(s->datefmt, sizeof s->datefmt, "*ISO");
	snprintf(s->timefmt, sizeof s->timefmt, "*ISO");
	memset(&o, 0, sizeof o);
	o.rdb = rdb;
	o.conninfo = conninfo;
	o.user = user;
	o.password = pwd;
	o.conf = conf_get;
	memset(&e, 0, sizeof e);
	s->bc = be->connect(&o, &e);
	if (s->bc)
	{
		s->t_connect = time(NULL);
		s->cn_info = strdup(conninfo);
		s->cn_user = strdup(user);
		s->cn_pwd = strdup(pwd);
		s->idle_check = conf_get(rdb, "idle_check", val, sizeof val) == 0 ? atoi(val) : 2;
		s->ping_timeout = conf_get(rdb, "ping_timeout", val, sizeof val) == 0 && atoi(val) > 0 ? atoi(val) : 5;
		s->last_use = time(NULL);
		s->joblog = 1;
		if (conf_get(rdb, "joblog", val, sizeof val) == 0)
			s->joblog = strcmp(val, "all") == 0 ? 2 : strcmp(val, "off") == 0 ? 0 : 1;
	}
	memset(pwd, 0, sizeof pwd);
	prf_forget(rdb);
	tracef(s->trace, "ARCN0100 rdb=%s backend=%s actgrp=%d user=%s -> %s (profile from %s)", rdb, be->name,
		   actgrp, user, s->bc ? "connected" : "FAILED", prf_of(rdb) ? "XDBPRF" : "xdb.conf");
	if (given)
		tracef(s->trace, "  USER and USING of the CONNECT are used: user of %d, password of %d characters",
			   ulen, plen);
	if (s->bc == NULL)
	{
		tracef(s->trace, "  %s (native %d): %s", e.sqlstate, e.native, e.msg);
		sqlca_set(ca, strncmp(e.sqlstate, "28", 2) == 0 ? -30082 : -30080,
				  strlen(e.sqlstate) == 5 ? e.sqlstate : "08001",
				  e.msg[0] ? e.msg : "connection failed", 0);
		err_keep(strncmp(e.sqlstate, "28", 2) == 0 ? -30082 : -30080,
				 strlen(e.sqlstate) == 5 ? e.sqlstate : "08001", &e, be->name, rdb, connect_log(rdb));
		memset(s, 0, sizeof *s);
		return 0;
	}
	sqlca_set(ca, 0, "00000", "", 0);
	/* product id seen by Db2: PGSvvrrm and the like, or product = XXXvvrrm in xdb.conf */
	if (lt.on)
		tracef(s->trace, "  EXPERIMENT QTEMP/XDBLOBT: %s", lt.spec);
	if ((lt.on && strlen(lt.product) == 8 && snprintf(val, sizeof val, "%s", lt.product) > 0) ||
		(conf_get(rdb, "product", val, sizeof val) == 0 && strlen(val) == 8))
	{
		setfld(out, 3, val);
		setfld(out + 3, 2, val + 3);
		setfld(out + 5, 2, val + 5);
		setfld(out + 7, 1, val + 7);
	}
	else
	{
		ver = be->version(s->bc);
		setfld(out, 3, be->product);
		snprintf(val, sizeof val, "%02d", (ver / 100) % 100);
		setfld(out + 3, 2, val);
		snprintf(val, sizeof val, "%02d", ver % 100);
		setfld(out + 5, 2, val);
		setfld(out + 7, 1, "0");
	}
	for (i = 0; user[i] && i < 60; i++)
		val[i] = (char) ((user[i] >= 'a' && user[i] <= 'z') ? user[i] - 32 : user[i]);
	val[i] = '\0';
	setfld(out + 8, 10, val);
	setfld(out + 18, 1, "1");	/* include bound statements at run time */
	setfld(out + 19, 1, "0");	/* not a protected conversation */
	*outlen = 20;
	return 0;
}

/* common statement header: isolation and formats decide the session mode */
static void
apply_header(xsess *s, const char *in)
{
	char		iso[12],
				df[12],
				tf[12];
	int			ccsid = g32(in, 76);

	fld(in, 82, 10, df, sizeof df);
	fld(in, 92, 10, tf, sizeof tf);
	fld(in, 102, 10, iso, sizeof iso);
	if (df[0] == '*')
		snprintf(s->datefmt, sizeof s->datefmt, "%.4s", df);
	if (tf[0] == '*')
		snprintf(s->timefmt, sizeof s->timefmt, "%.4s", tf);
	if (s->char_ccsid == 0)
		s->char_ccsid = (ccsid > 0 && ccsid != 65535) ? ccsid : job_ccsid_num();
	if (!s->cmt_registered)
		s->autocommit = (iso[0] == '\0' || strcmp(iso, "*NONE") == 0 || strcmp(iso, "*NC") == 0);
}

/* ------------------------------------------------- files (see xdblob.c) */

static void
lob_error(xbe_err *e, const char *state, const char *msg)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "%s", state);
	snprintf(e->msg, sizeof e->msg, "%s", msg);
}

/*
 * The file functions of a statement against its description: a value for
 * a file needs rows, a value from a file a statement without rows.  The
 * columns that write files are numbers (the bytes written) for Db2.
 */
static int
lob_check(const xlob_plan *pl, xbe_col *cols, int ncols, xbe_err *e)
{
	int			i;

	if (pl->nout > 0 && ncols == 0)
	{
		lob_error(e, "42601", pl->out[0].kind == XLOB_CLOB ?
				  "\"PUT_CLOB_TO_FILE\" must be a column of the result of a query" :
				  "\"PUT_BLOB_TO_FILE\" must be a column of the result of a query");
		return -1;
	}
	if (pl->nin > 0 && ncols > 0)
	{
		lob_error(e, "0A000", pl->in[0].kind == XLOB_CLOB ?
				  "\"GET_CLOB_FROM_FILE\" is not supported in a statement that returns rows" :
				  "\"GET_BLOB_FROM_FILE\" is not supported in a statement that returns rows");
		return -1;
	}
	for (i = 0; i < pl->nout; i++)
	{
		if (pl->out[i].col >= ncols || pl->out[i].col >= MAXCOL)
		{
			lob_error(e, "42601", "\"PUT_BLOB_TO_FILE\": the columns of the result could not be counted");
			return -1;
		}
		cols[pl->out[i].col].kind = XK_BIGINT;
		cols[pl->out[i].col].len = 0;
	}
	return 0;
}

/*
 * Before a statement with GET_xLOB_FROM_FILE runs: each file goes to the
 * backend, and the parameter list of the rewritten statement is put
 * together from the host variables (pv) and what the backend wants in
 * place of the files.  own collects what has to be freed afterwards.  The
 * files (xf, src: XLOB_MAXIN of each, *nxf opened) stay open, because a
 * backend may read them only when the statement is sent: lob_staged()
 * closes them.
 */
static int
lob_stage(xsess *s, const xlob_plan *pl, int np, char **pv, const int *pk,
		  char **fv, int *fk, char **own, int *nown, xlob_file *xf, xlob_src *src, int *nxf,
		  xbe_err *e)
{
	char	   *sv[XLOB_MAXIN][XLOB_SLOTS];
	int			f,
				i,
				k;

	*nown = 0;
	*nxf = 0;
	if (np != pl->nuser || pl->nmark > MAXPARM)
	{
		lob_error(e, "07001", "");
		snprintf(e->msg, sizeof e->msg, "the statement has %d host variables, %d were passed", pl->nuser, np);
		return -1;
	}
	memset(sv, 0, sizeof sv);
	for (f = 0; f < pl->nin; f++)
	{
		const xlob_in *in = &pl->in[f];
		const char *path = in->path_user >= 0 ? pv[in->path_user] : in->path;
		int			rc;

		if (xlob_src_open(&xf[f], path, in->kind, &src[f], e) < 0)
			return -1;
		*nxf = f + 1;
		rc = s->be->lob->in(s->bc, in->kind, &src[f], sv[f], e);
		for (k = 0; k < XLOB_SLOTS; k++)
			if (sv[f][k])
				own[(*nown)++] = sv[f][k];
		if (rc < 0)
			return -1;
	}
	for (i = 0; i < pl->nmark; i++)
	{
		const xlob_mark *m = &pl->mark[i];

		if (m->user >= 0)
		{
			fv[i] = pv[m->user];
			fk[i] = pk ? pk[m->user] : XK_VARCHAR;
		}
		else
		{
			fv[i] = sv[m->file][m->slot];
			fk[i] = XK_VARCHAR;
		}
	}
	return 0;
}

/* the statement has run (or not): the files of lob_stage are closed */
static void
lob_staged(xsess *s, xlob_file *xf, int nxf, int ok)
{
	int			f;

	for (f = 0; f < nxf; f++)
	{
		if (ok)
			tracef(s->trace, "  file %s: %lld bytes read", xf[f].path, xf[f].moved);
		xlob_src_close(&xf[f]);
	}
}

/*
 * After a fetch from the backend: the columns of PUT_xLOB_TO_FILE carry
 * what the backend needs to deliver the value.  Each goes to its file now,
 * once, and the column becomes the number of bytes written.
 */
static int
lob_sinks(xsess *s, xstmt *t, xbe_err *e)
{
	const xlob_plan *pl = t->lob;
	int			r,
				k;

	free(t->sinkval);
	t->sinkval = calloc((size_t) t->buf.nrows * (size_t) pl->nout, 24);
	if (t->sinkval == NULL)
	{
		lob_error(e, "57011", "not enough memory for the result of a file column");
		return -1;
	}
	for (r = 0; r < t->buf.nrows; r++)
		for (k = 0; k < pl->nout; k++)
		{
			const xlob_out *o = &pl->out[k];
			char	  **cell;
			xlob_file	xf;
			xlob_dst	dst;
			long long	bytes = 0;
			char	   *txt;
			int			rc;

			if (o->col >= t->buf.ncols)
				continue;
			cell = &t->buf.val[r * t->buf.ncols + o->col];
			if (*cell == NULL)
				continue;		/* a null value: no file */
			xlob_dst_init(&xf, o->kind, o->option, o->ccsid, &dst);
			rc = s->be->lob->out(s->bc, o->kind, *cell, &dst, e);
			if (rc == 0)
				rc = xlob_dst_close(&xf, 1, &bytes, e);
			else
			{
				xbe_err		e2;

				xlob_dst_close(&xf, 0, NULL, &e2);
			}
			if (rc < 0)
				return -1;
			txt = t->sinkval + ((size_t) r * (size_t) pl->nout + (size_t) k) * 24;
			snprintf(txt, 24, "%lld", bytes);
			*cell = txt;
			tracef(s->trace, "  file %s: %lld bytes written", xf.path, bytes);
		}
	return 0;
}

/*
 * One fetch from the backend for a statement with PUT_xLOB_TO_FILE.  A
 * backend with out_to writes the files itself while the row arrives: it
 * is told where they go, and what it leaves in the batch for such a column
 * becomes the number of bytes written.  The others return the row first.
 */
static int
lob_fetch(xsess *s, xstmt *t, xbe_err *e)
{
	const xlob_plan *pl = t->lob;
	const xbe_lob *bl = s->be->lob;
	xlob_file  *xf;
	xlob_dst	dst[XLOB_MAXOUT];
	int			frc,
				k,
				n = pl->nout;

	if (bl->out_to == NULL)
	{
		frc = s->be->fetch(s->bc, t->bcur, 1, &t->buf, e);
		if (frc >= 0 && t->buf.nrows > 0)
			frc = lob_sinks(s, t, e);
		return frc;
	}
	xf = malloc(sizeof(xlob_file) * (size_t) n);
	if (xf == NULL)
	{
		lob_error(e, "57011", "not enough memory for the result of a file column");
		return -1;
	}
	for (k = 0; k < n; k++)
		xlob_dst_init(&xf[k], pl->out[k].kind, pl->out[k].option, pl->out[k].ccsid, &dst[k]);
	bl->out_to(s->bc, dst, n);
	frc = s->be->fetch(s->bc, t->bcur, 1, &t->buf, e);
	bl->out_to(s->bc, NULL, 0);
	if (frc >= 0 && t->buf.nrows > 1)
	{
		/* cannot happen with a backend that keeps to its word */
		lob_error(e, "58004", "the server sent several rows where one with a file column was asked for");
		frc = -1;
	}
	for (k = 0; k < n; k++)
	{
		const xlob_out *o = &pl->out[k];
		long long	bytes = 0;
		xbe_err		e2;
		int			was = xf[k].isopen;

		if (frc < 0)
		{
			/* a file of the row that was written to its end stays; one cut short does not */
			xlob_dst_close(&xf[k], xf[k].complete, NULL, &e2);
			continue;
		}
		if (xlob_dst_close(&xf[k], 1, &bytes, e) < 0)
		{
			frc = -1;
			continue;
		}
		if (was && t->buf.nrows == 1 && o->col < t->buf.ncols && t->buf.val[o->col] != NULL)
		{
			/* the backend left 24 bytes there for the number */
			snprintf(t->buf.val[o->col], 24, "%lld", bytes);
			tracef(s->trace, "  file %s: %lld bytes written", xf[k].path, bytes);
		}
	}
	free(xf);
	return frc;
}

/* takes ownership of sql */
static xstmt *
do_prepare(xsess *s, const char *in, char *ca, int section, const char *pkg,
		   const char *stmtname, char *sql)
{
	xstmt	   *t = stmt_by(s, section, pkg, stmtname, NULL);
	xbe_col    *cols = NULL;
	int			ncols = 0;
	xbe_err		e;

	if (t == NULL)
		t = stmt_new(s);
	else
	{
		cur_close(s, t);
		if (t->bstmt)
			s->be->stmt_free(s->bc, t->bstmt);
		t->bstmt = NULL;
		free(t->sql);
		t->sql = NULL;
		xlob_plan_free(t->lob);
		t->lob = NULL;
		/* the slot now holds another statement: nothing of the old one applies */
		t->virt = 0;
		t->virt_done = 0;
		t->reprep = 0;
		t->has_fetch_err = 0;
		t->late_cols = 0;
	}
	if (t == NULL || sql == NULL)
	{
		free(sql);
		sqlca_set(ca, -129, "54004", "too many statements or empty statement", 0);
		return NULL;
	}
	if (s->lob_bad)
	{
		/* a file function used wrongly: the statement text says nothing useful to the server */
		free(sql);
		sqlca_be(ca, s, &s->lob_err);
		lob_pending_clear(s);
		stmt_free(s, t);
		return NULL;
	}
	t->sql = sql;
	t->lob = s->lob_new;
	s->lob_new = NULL;
	t->section = section;
	snprintf(t->pkg, sizeof t->pkg, "%s", pkg);
	snprintf(t->stmtname, sizeof t->stmtname, "%s", stmtname);
	if (is_last_error(sql) || is_conn_info(sql))
	{
		/* answered here: the server is not asked, the error is not replaced */
		t->virt = is_last_error(sql) ? 1 : 2;
		cols = t->virt == 1 ? last_error_cols() : conn_info_cols();
		describe_cols(s, t, cols, !cols ? 0 : t->virt == 1 ? LAST_ERROR_COLS : CONN_INFO_COLS);
		free(cols);
		sqlca_set(ca, 0, "00000", "", 0);
		return t;
	}
	if (sess_check(s, ca) < 0 || stmt_begin(s, ca) < 0)
	{
		stmt_free(s, t);
		return NULL;
	}
	s->n_prep++;
	memset(&e, 0, sizeof e);
	if (be_describe(s, t, &cols, &ncols, &e) < 0)
	{
		sess_mend(s, &e);
		sqlca_be(ca, s, &e);
		stmt_failed(s);
		stmt_free(s, t);
		return NULL;
	}
	if (t->lob && lob_check(t->lob, cols, ncols, &e) < 0)
	{
		free(cols);
		sqlca_be(ca, s, &e);
		stmt_failed(s);
		stmt_free(s, t);
		return NULL;
	}
	describe_cols(s, t, cols, ncols);
	free(cols);
	sqlca_set(ca, 0, "00000", "", 0);
	return t;
}

/*
 * The server has refused a statement because it runs it only outside a
 * transaction (PostgreSQL: CREATE DATABASE, VACUUM and the like), and has
 * done nothing.  Under commitment control the driver has a transaction open
 * on the server from the first statement of a unit of work on, and without
 * commitment control Db2 lets no statement through to a database that is not
 * Db2 for i (SQL5028): so no program could ever run such a statement.
 *
 * When the unit of work has nothing on this connection that its COMMIT or
 * ROLLBACK would still decide - no change, no open cursor - the transaction
 * of the driver is empty.  It is ended (committed, so that a SET of the
 * session made in it stays), the statement runs on its own and is final at
 * once, and the next statement begins a new transaction.  Otherwise the
 * program is told to end its unit of work first.
 *
 * 0 = the statement ran; -1 = e says why not (the first error, or the one
 * of the second run).
 */
static int
exec_alone(xsess *s, xstmt *t, const char *sql, int np, char **pv, const int *pk,
		   long long *rows, xbe_err *e)
{
	xbe_err		b;
	const char *has = NULL,
			   *todo = "COMMIT or ROLLBACK first";
	int			i;

	if (s->autocommit || !s->in_txn)
		return -1;				/* no transaction of ours is in the way */
	for (i = 0; i < MAXSTMT; i++)
		if (s->st[i].used && s->st[i].is_open)
			has = "an open cursor";
	if (s->uow_dirty || s->uow_lost)
		has = "changes";
	else if (has == NULL && s->uow_set && !s->savepoints)
	{
		/*
		 * Without a savepoint for each statement the refusal has spoiled the
		 * transaction: it can only be rolled back, and the SET would go with
		 * it unnoticed.
		 */
		has = "a SET of the session";
		todo = "it stays only with a COMMIT, so COMMIT after the SET";
	}
	if (has)
	{
		char		m[sizeof e->msg];

		snprintf(m, sizeof m, "%.230s - the server runs this statement only outside a transaction, "
				 "and this unit of work has %s on the connection: %s, then run it",
				 e->msg, has, todo);
		snprintf(e->msg, sizeof e->msg, "%s", m);
		tracef(s->trace, "  only outside a transaction, and the unit of work is not empty (%s): refused", has);
		return -1;
	}
	/* the failed statement goes back to its savepoint, then the empty transaction ends */
	stmt_failed(s);
	if (txn_op(s, s->savepoints ? XT_COMMIT : XT_ROLLBACK, &b) < 0)
	{
		txn_op(s, XT_ROLLBACK, NULL);
		uow_reset(s);
		tracef(s->trace, "  only outside a transaction, and the empty transaction could not be ended");
		return -1;
	}
	uow_reset(s);
	tracef(s->trace, "  only outside a transaction: the unit of work was empty, its transaction is ended "
		   "and the statement runs on its own");
	memset(e, 0, sizeof *e);
	*rows = 0;
	return s->be->exec(s->bc, t ? t->bstmt : NULL, sql, np, pv, pk, rows, e);
}

static void
do_execute(xsess *s, char *ca, xstmt *t, const char *sql, int np, char **pv, const int *pk,
		   char *out, int *outlen)
{
	long long	rows = 0;
	xbe_err		e;
	int			alone = 0;		/* the statement ran outside the unit of work */
	const xlob_plan *pl = t ? t->lob : s->lob_new;	/* file functions in the statement */
	int			rc;

	if (t == NULL && s->lob_bad)
	{
		sqlca_be(ca, s, &s->lob_err);
		lob_pending_clear(s);
		return;
	}
	if (pl && pl->nout > 0)
	{
		lob_error(&e, "42601", pl->out[0].kind == XLOB_CLOB ?
				  "\"PUT_CLOB_TO_FILE\" needs a cursor: PREPARE, DECLARE CURSOR, OPEN, FETCH" :
				  "\"PUT_BLOB_TO_FILE\" needs a cursor: PREPARE, DECLARE CURSOR, OPEN, FETCH");
		sqlca_be(ca, s, &e);
		return;
	}
	if (sess_check(s, ca) < 0)
		return;
	stmt_reprep(s, t);
	if (stmt_begin(s, ca) < 0)
		return;
	s->n_exec++;
	memset(&e, 0, sizeof e);
	if (pl)
	{
		/* the files go to the server first, in the unit of work of the statement */
		char	   *fv[MAXPARM];
		int			fk[MAXPARM];
		char	   *own[XLOB_MAXIN * XLOB_SLOTS];
		xlob_file  *xf = malloc(sizeof(xlob_file) * XLOB_MAXIN);
		xlob_src	src[XLOB_MAXIN];
		int			nown = 0,
					nxf = 0,
					i;

		if (xf == NULL)
		{
			lob_error(&e, "57011", "not enough memory for the files of the statement");
			rc = -1;
		}
		else
		{
			rc = lob_stage(s, pl, np, pv, pk, fv, fk, own, &nown, xf, src, &nxf, &e);
			if (rc == 0)
				rc = s->be->exec(s->bc, t ? t->bstmt : NULL, sql, pl->nmark, fv, fk, &rows, &e);
			s->be->lob->in_done(s->bc, rc == 0);
			lob_staged(s, xf, nxf, rc == 0);
			for (i = 0; i < nown; i++)
				free(own[i]);
			free(xf);
		}
	}
	else
	{
		rc = s->be->exec(s->bc, t ? t->bstmt : NULL, sql, np, pv, pk, &rows, &e);
		if (rc < 0 && e.alone)
		{
			rc = exec_alone(s, t, sql, np, pv, pk, &rows, &e);
			alone = !s->in_txn;	/* whatever came of it, no transaction is open now */
		}
	}
	if (rc < 0)
	{
		sess_mend(s, &e);
		sqlca_be(ca, s, &e);
		stmt_failed(s);
	}
	else
	{
		if (rows == 0 && (first_kw(sql, "UPDATE") || first_kw(sql, "DELETE")))
			sqlca_set(ca, 100, "02000", "", 0);
		else
			sqlca_set(ca, 0, "00000", "", rows > 2147483647 ? 2147483647 : (int) rows);
		tracef(s->trace, "  ok, %lld rows", rows);
		s->n_changed += rows > 0 ? rows : 0;
		if (!alone)
		{
			/* what ran on its own is final: the unit of work has nothing of it to commit */
			uow_touch(s, sql, 0);
			register_commit(s);
		}
	}
	setfld(out, 1, s->autocommit ? "0" : "1");
	*outlen = 1;
}

/* a host variable of a type the driver does not take: n counts from 1 */
static void
bad_parm(xsess *s, char *ca, int n)
{
	char		msg[160];
	int			t = s->bad_type & ~1;
	xbe_err		e;

	if (t == 404 || t == 408 || t == 412 || (t >= 916 && t <= 925) || (t >= 960 && t <= 969))
		snprintf(msg, sizeof msg,
				 "host variable %d is a large object (SQL type %d): not supported by XDB, pass a VARCHAR", n, t);
	else if (s->bad_type != 0)
		snprintf(msg, sizeof msg, "host variable %d: SQL type %d is not supported by XDB", n, t);
	else
		snprintf(msg, sizeof msg, "host variable %d cannot be converted", n);
	s->bad_type = 0;
	sqlca_set(ca, -804, "07002", msg, 0);
	/* Db2 shows its own text for -804: ours goes to XDB_LAST_ERROR and the job log */
	memset(&e, 0, sizeof e);
	snprintf(e.sqlstate, sizeof e.sqlstate, "07002");
	snprintf(e.msg, sizeof e.msg, "%s", msg);
	err_keep(-804, "07002", &e, s->be->name, s->rdb, s->joblog ? 2 : 0);
}

/* The output of AROC0100 for an open that worked: two flags, then the SQLDA. */
static void
open_output(xsess *s, xstmt *t, int block, char *out, int outsize, int *outlen)
{
	static xstmt nocols;
	int			off = 16;
	int			n;

	*outlen = 0;
	if (outsize < 32)
		return;
	n = build_sqlda(s, t ? t : &nocols, out + 16, outsize - 16, 0);
	if (n < 0)
		n = build_sqlda(s, &nocols, out + 16, outsize - 16, 0);
	memset(out, 0, 16);
	setfld(out, 1, block ? "1" : "0");
	setfld(out + 1, 1, s->autocommit ? "1" : "0");
	memcpy(out + 4, &off, 4);
	*outlen = n + 16;
}

/*
 * The open failed and ca holds the error.  Db2 takes the error only with
 * this output: the two flags and no SQLDA (offset 0).  With no output, or
 * with an SQLDA after the flags, the program gets CPD3E06 and SQL0969 in
 * place of the error (tried on 7.5, test XDBT24).
 */
static void
open_fail(xsess *s, char *ca, char *out, int outsize, int *outlen)
{
	int			code;

	memcpy(&code, ca + 12, 4);
	*outlen = 0;
	if (outsize >= 8)
	{
		memset(out, 0, 8);
		setfld(out, 1, "0");
		setfld(out + 1, 1, "0");
		*outlen = 8;
	}
	tracef(s->trace, "  open failed with sqlcode %d", code);
}

static void
do_open(xsess *s, const char *in, char *ca, char *out, int outsize, int *outlen)
{
	char		pkg[20],
				cur[20];
	int			section = g32(in, 72);
	xstmt	   *t;
	char	   *pv[MAXPARM];
	int			pk[MAXPARM];
	int			np,
				perr,
				n,
				rc;
	xbe_err		e;

	fld(in, 44, 18, pkg, sizeof pkg);
	fld(in, 148, 18, cur, sizeof cur);
	t = stmt_by(s, section, pkg, NULL, NULL);
	if (g32(in, 144) > 0)
	{
		/* bound DECLARE CURSOR text: keep what follows FOR */
		char	   *d = stmt_text(s, in, g32(in, 140), g32(in, 144), g32(in, 76));
		char	   *f = d ? strstr(d, " FOR ") : NULL;

		if (f == NULL && d)
			f = strstr(d, " for ");
#ifdef XDB_LOBTEST
		/* EXPERIMENT (three-part names): "DECLARE C1 CURSOR FOR S1", no section */
		if (lobt_strip_on() && f && strchr(f + 5, ' ') == NULL && t == NULL)
		{
			t = stmt_by(s, 0, pkg, f + 5, NULL);
			tracef(s->trace, "  EXPERIMENT cursor for statement name %s: %s", f + 5,
				   t ? "found" : "not found");
		}
#endif
		if (f && strchr(f + 5, ' '))
		{
			char	   *sql = strdup(f + 5);

#ifdef XDB_LOBTEST
			if (lobt_strip_on() && sql && section == 0)
			{
				char	   *m = lobt_named_markers(sql, s->be->marker);

				if (m)
				{
					free(sql);
					sql = m;
				}
				tracef(s->trace, "  EXPERIMENT static text: %.300s", sql);
			}
#endif
			free(d);
			t = do_prepare(s, in, ca, section, pkg, "", sql);
			if (t == NULL)
			{
				open_fail(s, ca, out, outsize, outlen);
				return;
			}
		}
		else
			free(d);
	}
	if (t == NULL || t->sql == NULL)
	{
		sqlca_set(ca, -514, "26501", "cursor statement not prepared", 0);
		open_fail(s, ca, out, outsize, outlen);
		return;
	}
	np = collect_parms(s, in, g32(in, 132), g32(in, 136), pv, pk, &perr);
	if (perr)
	{
		free_parms(pv, np);
		bad_parm(s, ca, perr);
		open_fail(s, ca, out, outsize, outlen);
		return;
	}
	cur_close(s, t);
	{
		int			i;

		/* a cursor name belongs to one statement at a time */
		for (i = 0; i < MAXSTMT; i++)
			if (s->st[i].used && &s->st[i] != t && strcmp(s->st[i].curname, cur) == 0)
			{
				cur_close(s, &s->st[i]);
				s->st[i].curname[0] = '\0';
			}
	}
	snprintf(t->curname, sizeof t->curname, "%s", cur);
	if (t->virt)
	{
		free_parms(pv, np);
		t->bcur = NULL;
		t->virt_done = 0;
		goto opened;
	}
	if (sess_check(s, ca) < 0)
	{
		free_parms(pv, np);
		open_fail(s, ca, out, outsize, outlen);
		return;
	}
	stmt_reprep(s, t);
	if (stmt_begin(s, ca) < 0)
	{
		free_parms(pv, np);
		open_fail(s, ca, out, outsize, outlen);
		return;
	}
	memset(&e, 0, sizeof e);
	if (t->lob && t->lob->nout > 0)
	{
		/*
		 * A backend may have put the two arguments of PUT_xLOB_TO_FILE in
		 * its own order: the host variables follow the markers of the
		 * statement as it is now.
		 */
		const xlob_plan *pl = t->lob;
		char	   *fv[MAXPARM];
		int			fk[MAXPARM];
		int			i;

		rc = 0;
		if (np != pl->nuser || pl->nmark > MAXPARM)
		{
			lob_error(&e, "07001", "");
			snprintf(e.msg, sizeof e.msg, "the statement has %d host variables, %d were passed", pl->nuser, np);
			rc = -1;
		}
		for (i = 0; rc == 0 && i < pl->nmark; i++)
		{
			if (pl->mark[i].user < 0)
			{
				lob_error(&e, "0A000", "a value from a file is not supported in a statement that returns rows");
				rc = -1;
				break;
			}
			fv[i] = pv[pl->mark[i].user];
			fk[i] = pk[pl->mark[i].user];
		}
		if (rc == 0)
			rc = s->be->open(s->bc, t->bstmt, t->sql, pl->nmark, fv, fk, s->autocommit, &t->bcur, &e);
	}
	else
		rc = s->be->open(s->bc, t->bstmt, t->sql, np, pv, pk, s->autocommit, &t->bcur, &e);
	free_parms(pv, np);
	if (rc == 0 && s->be->cursor_cols)
	{
		xbe_col    *cols = NULL;
		int			nc = 0;

		/*
		 * The statement ran at the open (the call of a procedure): the
		 * backend may know the columns of the rows only now.  When the
		 * describe told none, these are the columns, and Db2 takes them
		 * from the SQLDA that goes back with this open.  When it told
		 * some, they stand, but rows with another number of columns
		 * cannot be handed out as what was described.
		 */
		rc = s->be->cursor_cols(s->bc, t->bcur, &cols, &nc, &e);
		if (rc == 0 && nc > 0)
		{
			if (t->ncols == 0 || t->late_cols)
			{
				describe_cols(s, t, cols, nc);
				t->late_cols = 1;
				tracef(s->trace, "  the server told the %d columns of the statement when it ran", t->ncols);
			}
			else if (nc != t->ncols)
			{
				memset(&e, 0, sizeof e);
				snprintf(e.sqlstate, sizeof e.sqlstate, "07008");
				snprintf(e.msg, sizeof e.msg, "the statement was described with %d columns and returned rows of %d: "
						 "its first result set is not always the same", t->ncols, nc);
				rc = -1;
			}
		}
		free(cols);
		if (rc < 0)
			s->be->close(s->bc, t->bcur);
	}
	if (rc == 0 && t->ncols == 0)
	{
		/* it ran and has no rows to give: a cursor of no columns is none */
		s->be->close(s->bc, t->bcur);
		memset(&e, 0, sizeof e);
		snprintf(e.sqlstate, sizeof e.sqlstate, "07005");
		snprintf(e.msg, sizeof e.msg, "the statement ran and returned no rows: there is nothing to fetch");
		rc = -1;
	}
	if (rc < 0)
	{
		sess_mend(s, &e);
		sqlca_be(ca, s, &e);
		stmt_failed(s);
		t->bcur = NULL;
		open_fail(s, ca, out, outsize, outlen);
		return;
	}
opened:
	t->is_open = 1;
	if (!t->virt)
	{
		s->n_open++;
		uow_touch(s, t->sql, 1);
	}
	t->eof = 0;
	t->bufrow = 0;
	t->nfetch = 0;
	t->row_max = 0;
	t->row_seen = 0;
	t->row_asked = 0;
	t->has_fetch_err = 0;
	t->block = (in[130] != '\0' && ((unsigned char) in[130] & 0x0F) == 1);
	register_commit(s);
	n = build_sqlda(s, t, out + 16, outsize - 16, 0);
	if (n < 0)
	{
		sqlca_set(ca, -840, "54004", "too many columns for the output buffer", 0);
		cur_close(s, t);
		open_fail(s, ca, out, outsize, outlen);
		return;
	}
	open_output(s, t, t->block, out, outsize, outlen);
	sqlca_set(ca, 0, "00000", "", 0);
	tracef(s->trace, "  open %s, %d columns, block=%d (flag x%02X) bufsize=%d outlen=%d iso=%s",
		   cur, t->ncols, t->block, (unsigned char) in[130], outsize, *outlen,
		   s->autocommit ? "none" : "txn");
	if (lt.on)
		tracef(s->trace, "  AROC0100 input format is %d bytes, SQLDA of the host variables at %d, length %d",
			   s->in_len, g32(in, 132), g32(in, 136));
}

/* memory one batch of rows is meant to take, and what it may take at the most */
#define BATCH_BYTES 4000000L
#define BATCH_BYTES_MAX 64000000L
/* what a value costs beside its bytes: its pointer and its allocation */
#define VALUE_COST 48

/*
 * Rows asked from the backend at a time: what option fetch_rows says (200
 * without it), fewer when the rows are long, so that one batch stays within
 * a few megabytes whatever the option says.  More rows at a time are fewer
 * exchanges with the server, which counts when the server is far away; the
 * blocks handed to Db2 are not touched by it.
 *
 * Before the cursor has delivered a row nothing is known but what the rows
 * can be at the most (the lengths the columns are described with): the first
 * batch is sized by that.  From then on the largest row the cursor has
 * really delivered decides - a query that describes its columns far wider
 * than its values are (CAST(... AS CHAR(12000)) around a short text) would
 * otherwise crawl in batches of a few dozen rows - and what the rows can be
 * at the most still sets a ceiling, at a higher mark, because the rows to
 * come may be longer than those seen.  Where a server sends values longer
 * than described (MySQL sends a long text whole and the driver cuts it), the
 * rows seen make the batches smaller than the description would.
 */
static int
fetch_rows(const xsess *s, const xstmt *t)
{
	long		est = 16,
				n;
	int			i;

	/* a row that writes files may carry megabytes: one at a time */
	if (t->lob && t->lob->nout > 0)
		return 1;
	for (i = 0; i < t->ncols; i++)
		est += (t->cols[i].sqltype == T_CHAR || t->cols[i].sqltype == T_VARCHAR) ?
			(long) t->cols[i].len * 2 + 8 : 24;
	if (!t->row_seen)
		n = BATCH_BYTES / est;
	else
	{
		n = BATCH_BYTES / (t->row_max > 64 ? t->row_max : 64);
		if (n > BATCH_BYTES_MAX / est)
			n = BATCH_BYTES_MAX / est;
	}
	return n < 1 ? 1 : n > s->fetch_rows ? s->fetch_rows : (int) n;
}

/* the rows of a batch that has just come from the server: how large is the largest? */
static void
rows_measure(xstmt *t)
{
	const xbe_batch *b = &t->buf;
	int			r,
				c;

	if (b->val == NULL)
		return;
	for (r = 0; r < b->nrows; r++)
	{
		long		size = 0;

		for (c = 0; c < b->ncols; c++)
		{
			const char *v = b->val[r * b->ncols + c];

			size += VALUE_COST + (v ? (long) strlen(v) : 0);
		}
		if (size > t->row_max)
			t->row_max = size;
		t->row_seen = 1;
	}
}

static void
do_fetch(xsess *s, const char *in, char *ca, char *out, int outsize, int *outlen)
{
	char		cur[20];
	xstmt	   *t;
	unsigned char *o = (unsigned char *) out + 8;
	int			room = outsize - 8;
	int			rows = 0;
	int			off = 8;

	fld(in, 112, 18, cur, sizeof cur);
	t = stmt_by(s, 0, "", NULL, cur);
	if (lt.on)
	{
		/* EXPERIMENT: all that Db2 sends with a FETCH, after the common header */
		char		hx[200];
		int			k,
					m = s->in_len - 112;

		if (m > 80)
			m = 80;
		hx[0] = '\0';
		for (k = 0; k < m; k++)
			sprintf(hx + 2 * k, "%02X", (unsigned char) in[112 + k]);
		tracef(s->trace, "ARFC0100 input format is %d bytes; after the header (112): %s", s->in_len, hx);
	}
	memset(out, 0, 8);
	setfld(out + 4, 1, "0");
	*outlen = 5;
	if (t == NULL || !t->is_open)
	{
		sqlca_set(ca, -501, "24501", "cursor not open", 0);
		return;
	}
	if (t->lost && !t->has_fetch_err)
	{
		sqlca_set(ca, -501, "24501", "the cursor was closed when the connection was lost", 0);
		return;
	}
	if (!t->virt)
		s->n_fetch++;
	for (;;)
	{
		int			j,
					used = 0,
					bad = 0;

		if (t->buf.val == NULL || t->bufrow >= t->buf.nrows)
		{
			xbe_err		e;
			int			frc;

			buf_free(t);
			if (t->has_fetch_err && rows == 0)
			{
				/* the error that ended the previous block */
				t->has_fetch_err = 0;
				sqlca_be(ca, s, &t->fetch_err);
				return;
			}
			if (t->eof)
				break;
			memset(&e, 0, sizeof e);
			if (t->virt == 2)
				frc = conn_info_batch(s, t, &t->buf);
			else if (t->virt)
				frc = last_error_batch(t, &t->buf);
			else
			{
				s->n_befetch++;
				if (t->lob && t->lob->nout > 0)
					frc = lob_fetch(s, t, &e);
				else
				{
					int			want = fetch_rows(s, t);

					if (want != t->row_asked)
					{
						if (t->row_seen)
							tracef(s->trace, "  %d rows asked from the server at a time now: the largest row so far has %ld bytes",
								   want, t->row_max);
						else
							tracef(s->trace, "  %d rows asked from the server at a time", want);
						t->row_asked = want;
					}
					frc = s->be->fetch(s->bc, t->bcur, want, &t->buf, &e);
					if (frc == 0)
						rows_measure(t);
				}
			}
			if (frc < 0)
			{
				buf_free(t);
				t->eof = 1;
				sess_mend(s, &e);
				/* the statement failed: in a transaction, back to its savepoint */
				stmt_failed(s);
				if (rows == 0)
				{
					sqlca_be(ca, s, &e);
					return;
				}
				t->fetch_err = e;	/* rows already in the block go out first */
				t->has_fetch_err = 1;
				break;
			}
			if (t->buf.eof || t->buf.nrows == 0)
				t->eof = 1;
			if (t->buf.nrows == 0)
				break;
		}
		for (j = 0; j < t->ncols; j++)
		{
			const char *v = j < t->buf.ncols ? t->buf.val[t->bufrow * t->buf.ncols + j] : NULL;
			int			n;

			if (room - used < 1)
			{
				bad = 2;
				break;
			}
			if (lt.on && lt.type != 0 && j == 0)
			{
				/* EXPERIMENT: the value of the first column comes from here */
				if (lt.ind)
					o[used++] = 0x00;
				n = lobt_put(o + used, room - used);
				if (n == -2)
				{
					bad = 2;
					break;
				}
				used += n;
				continue;
			}
			if (v == NULL)
			{
				o[used++] = 0xFF;
				continue;
			}
			o[used] = 0x00;
			n = put_col(s, &t->cols[j], v, o + used + 1, room - used - 1);
			if (n == -2)
			{
				bad = 2;
				break;
			}
			if (n < 0)
				o[used++] = 0xFE;	/* conversion error */
			else
				used += n + 1;
		}
		if (bad == 2)
		{
			if (rows == 0)
			{
				sqlca_set(ca, -30020, "58009", "row larger than the output buffer", 0);
				return;
			}
			break;				/* row stays buffered for the next call */
		}
		if (rows > 0 && (int) (o - (unsigned char *) out) + used > BLOCKMAX)
			break;				/* block is full: this row goes into the next call */
		o += used;
		room -= used;
		rows++;
		if (!t->virt)
			s->n_rows++;
		t->bufrow++;
		if (!t->block)
			break;
	}
	if (rows == 0)
	{
		sqlca_set(ca, 100, "02000", "", 0);
		return;
	}
	memcpy(out, &off, 4);
	*outlen = (int) (o - (unsigned char *) out);
	sqlca_set(ca, 0, "00000", "", rows);
	if (t->nfetch++ < 3)
	{
		char		hx[130];
		int			k;

		for (k = 0; k < 60 && k < *outlen; k++)
			sprintf(hx + 2 * k, "%02X", (unsigned char) out[k]);
		tracef(s->trace, "ARFC0100 %s call %d: bufsize=%d rows=%d outlen=%d data=%s",
			   cur, t->nfetch, outsize, rows, *outlen, hx);
	}
}

/*
 * Test statement "XDB SQLCA <sqlcode> <sqlstate> <text>": nothing goes to
 * the server, the given SQLCA comes back.  It shows how Db2 treats an
 * SQLCODE returned by an ARD (some are turned into SQL0901).
 */
static int
test_sqlca(const char *sql, char *ca)
{
	int			code;
	char		state[8],
				text[80];

	if (sql == NULL || strlen(sql) < 12 || (sql[0] | 0x20) != 'x' || (sql[1] | 0x20) != 'd' ||
		(sql[2] | 0x20) != 'b' || sql[3] != ' ' || (sql[4] | 0x20) != 's' || (sql[5] | 0x20) != 'q' ||
		(sql[6] | 0x20) != 'l' || (sql[7] | 0x20) != 'c' || (sql[8] | 0x20) != 'a' || sql[9] != ' ')
		return 0;
	text[0] = '\0';
	if (sscanf(sql + 10, "%d %5s %79[^\n]", &code, state, text) < 2)
		return 0;
	sqlca_raw = 1;
	sqlca_set(ca, code, state, text, 0);
	sqlca_raw = 0;
	{
		/* "/FF/" in the text stands for the token separator X'FF' */
		char		mk[8];
		int			ml = ile_conv(1208, ile_job_ccsid(), "/FF/", 4, mk, sizeof mk);
		short		l;
		int			i;

		memcpy(&l, ca + 16, 2);
		for (i = 0; ml == 4 && i + 4 <= l; i++)
			if (memcmp(ca + 18 + i, mk, 4) == 0)
			{
				ca[18 + i] = (char) 0xFF;
				memmove(ca + 19 + i, ca + 22 + i, (size_t) (l - i - 4));
				l -= 3;
			}
		memcpy(ca + 16, &l, 2);
	}
	{
		/*
		 * "/V/" starts a token that gets a two byte length in front of it,
		 * the form message data has for a varying length field.
		 */
		char		mk[8];
		char		in[70],
					o[70];
		int			ml = ile_conv(1208, ile_job_ccsid(), "/V/", 3, mk, sizeof mk);
		short		l;
		int			i = 0,
					n = 0,
					start,
					seen = 0;

		memcpy(&l, ca + 16, 2);
		memcpy(in, ca + 18, 70);
		while (ml == 3 && i < l && n < 70)
		{
			if (i + 3 <= l && memcmp(in + i, mk, 3) == 0)
			{
				short		tl;

				seen = 1;
				i += 3;
				start = i;
				while (i < l && !(i + 3 <= l && memcmp(in + i, mk, 3) == 0))
					i++;
				tl = (short) (i - start);
				if (n + 2 + tl > 70)
					break;
				memcpy(o + n, &tl, 2);
				memcpy(o + n + 2, in + start, tl);
				n += 2 + tl;
			}
			else
				o[n++] = in[i++];
		}
		if (seen)
		{
			memset(ca + 18, blank(), 70);
			memcpy(ca + 18, o, n);
			l = (short) n;
			memcpy(ca + 16, &l, 2);
		}
	}
	return 1;
}

/* ------------------------------------------------------------- entry point */

void
xdb_ard_entry(int *level, char *in, int *inlen, char *fmtname,
			  char *ca, int *ccsid, char *out, int *outlen)
{
	char		fmt[12],
				rdb[20],
				pkg[20],
				name[20];
	int			outsize = g32(in, 0);
	int			actgrp = g32(in, 4);
	xsess	   *s;

	fld(fmtname, 0, 8, fmt, sizeof fmt);
	fld(in, 8, 18, rdb, sizeof rdb);
	*outlen = 0;
	*ccsid = job_ccsid_num();
	if (strcmp(fmt, "ARCN0100") == 0)
	{
		do_connect(in, ca, out, outlen);
		return;
	}
	s = sess_find(actgrp, rdb);
	if (s == NULL)
	{
		tracef(gtrace, "%s rdb=%s actgrp=%d: no session", fmt, rdb, actgrp);
		sqlca_set(ca, -900, "08003", "connection does not exist", 0);
		return;
	}
	s->in_len = *inlen;
	s->in_base = in;
	lob_pending_clear(s);
	if (strcmp(fmt, "ARDI0100") == 0)
	{
		tracef(s->trace, "ARDI0100 type=%d", g32(in, 28));
		if (g32(in, 28) != 3)
		{
			xbe_err		e;

			/* what is still open goes: rolled back, or committed where every statement is */
			if (s->fallback)
				uow_commit(s, &e);
			else
				uow_rollback(s);
		}
		sess_close(s);
		sqlca_set(ca, 0, "00000", "", 0);
		return;
	}
	if (strcmp(fmt, "ARBB0100") == 0 || strcmp(fmt, "ARBS0100") == 0 || strcmp(fmt, "AREB0100") == 0)
	{
		tracef(s->trace, "%s (package bind accepted, statements are taken at run time)", fmt);
		sqlca_set(ca, 0, "00000", "", 0);
		return;
	}
	if (strcmp(fmt, "ARDT0100") == 0)
	{
		tracef(s->trace, "ARDT0100 not supported");
		sqlca_set(ca, -84, "42612", "DESCRIBE TABLE not supported by XDB", 0);
		return;
	}
	fld(in, 44, 18, pkg, sizeof pkg);
	if (strcmp(fmt, "ARCC0100") == 0 || strcmp(fmt, "ARDS0100") == 0)
	{
		xstmt	   *t;

		fld(in, 80, 18, name, sizeof name);
		tracef(s->trace, "%s %s section=%d", fmt, name, g32(in, 72));
		if (fmt[2] == 'C')
		{
			t = stmt_by(s, 0, "", NULL, name);
			if (t && t->is_open)
				cur_close(s, t);
			sqlca_set(ca, 0, "00000", "", 0);
		}
		else
		{
			t = stmt_by(s, g32(in, 72), pkg, name, NULL);
			if (t == NULL)
			{
				/*
				 * A DESCRIBE of a statement whose PREPARE failed: SQL0518.
				 * Db2 looks into the output of this format also when nothing
				 * is returned.  What it finds must not be what was left there
				 * (seen: MCH1210 in the job log, SQL30020, and the connection
				 * is ended), and an SQLDA that is returned with the error is
				 * refused too (CPD3E06, SQL0969).  So: nothing returned, and
				 * the place cleared.
				 */
				char		m[80];

				if (outsize >= 16)
					memset(out, 0, 16);
				*outlen = 0;
				snprintf(m, sizeof m, "statement \"%s\" is not prepared", name[0] ? name : "*N");
				sqlca_set(ca, -518, "07003", m, 0);
			}
			else
			{
				*outlen = build_sqlda(s, t, out, outsize, 1);
				if (*outlen < 0)
					*outlen = 0;
				sqlca_set(ca, 0, "00000", "", 0);
			}
		}
		return;
	}
	apply_header(s, in);
	if (strcmp(fmt, "ARPS0100") == 0 || strcmp(fmt, "ARPD0100") == 0)
	{
		char	   *sql = stmt_text(s, in, g32(in, 112), g32(in, 116), g32(in, 76));
		xstmt	   *t;

		fld(in, 120, 18, name, sizeof name);
		tracef(s->trace, "%s stmt=%s section=%d: %s", fmt, name, g32(in, 72), sql ? sql : "(null)");
		if (test_sqlca(sql, ca))
		{
			free(sql);
			return;
		}
		t = do_prepare(s, in, ca, g32(in, 72), pkg, name, sql);
		if (t && fmt[3] == 'D')
		{
			*outlen = build_sqlda(s, t, out, outsize, 1);
			if (*outlen < 0)
				*outlen = 0;
		}
		return;
	}
	if (strcmp(fmt, "ARXI0100") == 0)
	{
		char	   *sql = stmt_text(s, in, g32(in, 112), g32(in, 116), g32(in, 76));

		tracef(s->trace, "ARXI0100 section=%d: %s", g32(in, 72), sql ? sql : "(null)");
		setfld(out, 1, "0");
		*outlen = 1;
		if (test_sqlca(sql, ca))
		{
			free(sql);
			return;
		}
#ifdef XDB_LOBTEST
		/* EXPERIMENT (three-part names): Db2 sends its SQL path first */
		if (lobt_strip_on() && sql && strncmp(sql, "SET PATH ", 9) == 0)
		{
			tracef(s->trace, "  EXPERIMENT: SET PATH answered without the server");
			sqlca_set(ca, 0, "00000", "", 0);
			free(sql);
			return;
		}
#endif
		if (sql == NULL)
			sqlca_set(ca, -104, "42601", "empty statement", 0);
		else
			do_execute(s, ca, NULL, sql, 0, NULL, NULL, out, outlen);
		free(sql);
		return;
	}
	if (strcmp(fmt, "ARXP0100") == 0 || strcmp(fmt, "ARXB0100") == 0)
	{
		char	   *pv[MAXPARM];
		int			pk[MAXPARM];
		int			perr,
					np;
		char	   *sql = NULL;
		xstmt	   *t = NULL;

		if (fmt[3] == 'P')
		{
			fld(in, 128, 18, name, sizeof name);
			t = stmt_by(s, g32(in, 72), pkg, name, NULL);
			np = collect_parms(s, in, g32(in, 112), g32(in, 116), pv, pk, &perr);
		}
		else
		{
			sql = stmt_text(s, in, g32(in, 140), g32(in, 144), g32(in, 76));
			np = collect_parms(s, in, g32(in, 132), g32(in, 136), pv, pk, &perr);
		}
		tracef(s->trace, "%s section=%d parms=%d: %s", fmt, g32(in, 72), np,
			   t ? t->sql : (sql ? sql : "(not found)"));
#ifdef XDB_LOBTEST
		if (lobt_strip_on() && t == NULL && sql == NULL)
		{
			char		hx[1300];
			int			m = s->in_len - 112;

			if (m > 600)
				m = 600;
			tracef(s->trace, "  EXPERIMENT %s statement name [%s], input format %d bytes; after 112: %s",
				   fmt, name, s->in_len, m > 0 ? hexof(in + 112, m, hx, sizeof hx) : "");
		}
#endif
		/* the output format must be there also when the statement is not run */
		setfld(out, 1, "0");
		*outlen = 1;
		if (perr)
			bad_parm(s, ca, perr);
		else if (t == NULL && sql == NULL)
			sqlca_set(ca, -518, "07003", "statement not prepared", 0);
		else
			do_execute(s, ca, t, t ? t->sql : sql, np, pv, pk, out, outlen);
		free_parms(pv, np);
		free(sql);
		return;
	}
	if (strcmp(fmt, "AROC0100") == 0)
	{
		tracef(s->trace, "AROC0100 section=%d", g32(in, 72));
		do_open(s, in, ca, out, outsize, outlen);
		return;
	}
	if (strcmp(fmt, "ARFC0100") == 0)
	{
		do_fetch(s, in, ca, out, outsize, outlen);
		return;
	}
	tracef(s->trace, "%s not supported", fmt);
	sqlca_set(ca, -84, "42612", "request not supported by XDB", 0);
}

/*
 * Commitment control exit program of the resource of a connection.
 * info = activation group (4) + RDB name (18) + resource number (4); the
 * action wanted is at status + 4.  The answer, always complete: length 7,
 * commit vote, classify result, changes ended.
 *
 *   A  (last agent)  everything else is prepared and nothing is committed:
 *      commit on the server now.  Vote 1 = done, the system commits the
 *      rest; vote 2 = not done, the system rolls the rest back and the
 *      program's COMMIT fails.
 *   P  (asked first)  can this connection commit?  Vote 1 or 2.
 *   C  commit; the outcome is decided and a failure here cannot change it.
 *   R  roll back.
 * Other actions (classify, savepoints) are answered without doing anything.
 */
void
xdb_commit_exit(char *info, char *status, char *ret)
{
	char		rdb[20],
				act[4];
	int			actgrp,
				gen;
	xsess	   *s;
	xbe_err		e;
	int			seven = 7;

	memcpy(ret, &seven, 4);
	setfld(ret + 4, 1, "1");
	setfld(ret + 5, 1, "0");
	setfld(ret + 6, 1, "0");
	memcpy(&actgrp, info, 4);
	fld(info, 4, 18, rdb, sizeof rdb);
	memcpy(&gen, info + 22, 4);
	fld(status, 4, 1, act, sizeof act);
	s = sess_find(actgrp, rdb);
	if (s == NULL)
	{
		setfld(ret + 6, 1, "1");	/* connection is gone: remove the resource */
		return;
	}
	if (gen != 0 && gen != s->cmt_gen)
	{
		/*
		 * The resource of an earlier connection with the same name, which
		 * has ended; the present connection has its own. Remove it, or
		 * every connect and disconnect would leave one more behind.
		 */
		setfld(ret + 6, 1, "1");
		tracef(s->trace, "commit exit: resource %d of an ended connection, removed", gen);
		return;
	}
	tracef(s->trace, "commit exit action=%s", act);
	switch (act[0])
	{
		case 'A':
			s->voted_no = 0;
			if (uow_commit(s, &e) < 0)
			{
				setfld(ret + 4, 1, "2");
				commit_failed(s, &e, 1);
				s->voted_no = 1;
			}
			break;
		case 'P':
			s->voted_no = 0;
			if (uow_ready(s, &e) < 0)
			{
				setfld(ret + 4, 1, "2");
				commit_failed(s, &e, 1);
				s->voted_no = 1;
			}
			break;
		case 'C':
			s->voted_no = 0;
			if (uow_commit(s, &e) < 0)
				commit_failed(s, &e, 0);
			break;
		case 'R':
			if (s->voted_no && !s->in_txn && !s->uow_dirty)
			{
				/* the rollback this connection's own no has caused: done when it voted */
				s->voted_no = 0;
				tracef(s->trace, "  (the unit of work ended with the vote)");
				break;
			}
			s->voted_no = 0;
			uow_rollback(s);
			break;
		default:
			break;
	}
}

/* used by the program stubs when an exception escapes the driver */
void
xdb_fail_sqlca(char *ca)
{
	sqlca_set(ca, -901, "58004", "XDB internal error (see job log)", 0);
	tracef(gtrace, "INTERNAL ERROR: exception in the ARD");
}

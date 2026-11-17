/*
 * lobparse.c - Linux test of xlob_parse() (xdb/xdblob.c): how statements
 * with the file functions are read and rewritten.  A stand-in backend puts
 * FILEIN(?,?) in place of a GET function and FILEOUT<s>(value ;; name) in
 * place of a PUT function, so that the result can be read.
 *   build: gcc -DXDB_LINUX_EMU -I xdb -o lobparse test/lobparse.c xdb/xdblob.c
 *   run:   ./lobparse > out; diff out test/lobparse.expected
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xdblob.h"

int
ile_conv(int from, int to, const char *in, size_t inlen, char *out, size_t outsize)
{
	return -1;
}

static int
t_in_sql(void *c, int kind, char *out, int outsize)
{
	snprintf(out, (size_t) outsize, "FILEIN%s(?,?)", kind == XLOB_CLOB ? "TEXT" : "");
	return 2;
}

static int
t_out_sql(void *c, int kind, int stage, char *pre, char *mid, char *post, xbe_err *e)
{
	snprintf(pre, XLOB_SQLPRE, "FILEOUT%s%s(", kind == XLOB_CLOB ? "TEXT" : "", stage ? "" : "NOSTAGE");
	snprintf(mid, XLOB_SQLMID, " ;; ");
	snprintf(post, XLOB_SQLMID, ")");
	return 0;
}

/* a backend that wants the file name in front of the value */
static int
n_in_sql(void *c, int kind, char *out, int outsize)
{
	snprintf(out, (size_t) outsize, "?");
	return 1;
}

static int
n_out_sql(void *c, int kind, int stage, char *pre, char *mid, char *post, xbe_err *e)
{
	snprintf(pre, XLOB_SQLPRE, "NAMED(");
	snprintf(mid, XLOB_SQLMID, " ;; ");
	snprintf(post, XLOB_SQLMID, ")");
	return 1;
}

/* a server that cannot write files */
static int
x_out_sql(void *c, int kind, int stage, char *pre, char *mid, char *post, xbe_err *e)
{
	memset(e, 0, sizeof *e);
	snprintf(e->sqlstate, sizeof e->sqlstate, "0A000");
	snprintf(e->msg, sizeof e->msg, "this server is too old for that");
	return -1;
}

static const xbe_lob t_lob = {t_in_sql, t_out_sql, NULL, NULL, NULL, NULL, NULL};
static const xbe_lob n_lob = {n_in_sql, n_out_sql, NULL, NULL, NULL, NULL, NULL};
static const xbe_lob x_lob = {n_in_sql, x_out_sql, NULL, NULL, NULL, NULL, NULL};
static xbe_driver with = {.name = "test",.marker = "$%d",.lob = &t_lob};
static xbe_driver namefirst = {.name = "namefirst",.marker = "?",.lob = &n_lob};
static xbe_driver tooold = {.name = "old",.marker = "?",.lob = &x_lob};
static xbe_driver without = {.name = "plain",.marker = "$%d"};

static void
show(const char *sql, const xbe_driver *be, int stage)
{
	char	   *out = NULL;
	xlob_plan  *pl = NULL;
	xbe_err		e;
	int			rc,
				i;

	memset(&e, 0, sizeof e);
	rc = xlob_parse(sql, be, NULL, stage, &out, &pl, &e);
	printf("IN : %s\n", sql);
	if (rc == 0)
		printf("     (no file function)\n");
	else if (rc < 0)
		printf("ERR: %s %s\n", e.sqlstate, e.msg);
	else
	{
		printf("OUT: %s\n", out);
		printf("     host variables %d, markers", pl->nuser);
		for (i = 0; i < pl->nmark; i++)
			if (pl->mark[i].user >= 0)
				printf(" h%d", pl->mark[i].user);
			else
				printf(" f%d.%d", pl->mark[i].file, pl->mark[i].slot);
		printf("\n");
		for (i = 0; i < pl->nin; i++)
			if (pl->in[i].path_user >= 0)
				printf("     file %d in : %s, name in host variable %d\n", i,
					   pl->in[i].kind == XLOB_CLOB ? "text" : "bytes", pl->in[i].path_user);
			else
				printf("     file %d in : %s, name [%s]\n", i,
					   pl->in[i].kind == XLOB_CLOB ? "text" : "bytes", pl->in[i].path);
		for (i = 0; i < pl->nout; i++)
			printf("     file %d out: %s, column %d, option %d, ccsid %d\n", i,
				   pl->out[i].kind == XLOB_CLOB ? "text" : "bytes", pl->out[i].col,
				   pl->out[i].option, pl->out[i].ccsid);
	}
	printf("\n");
	free(out);
	xlob_plan_free(pl);
}

int
main(void)
{
	static const char *const ok[] = {
		/* nothing to do */
		"select * from t where a = ?",
		"select 'get_blob_from_file(x)' as s, \"PUT_BLOB_TO_FILE\" from t -- put_blob_to_file(a,b)",
		"select myschema.get_blob_from_file('x'), put_blob_to_file from t",
		"update t set get_blob_from_file = 1",
		/* values from files */
		"insert into doc (id, body) values(?, get_blob_from_file(?))",
		"insert into doc (id, body) values(:H, GET_BLOB_FROM_FILE(:H :H))",
		"update doc set note = Get_Clob_From_File ( '/home/x/it''s.txt' , 1 ) where id = ? and k = ?",
		"insert into doc values(?, get_blob_from_file('/a'), ?, get_clob_from_file(?), ?)",
		"call store(get_blob_from_file(?), 'x?', ?) /* ? */",
		/* values to files */
		"select id, put_blob_to_file(body, '/tmp/d' || id || '.pdf') from doc where id > ?",
		"SELECT PUT_CLOB_TO_FILE(note, ?, 32, 1252) AS n, id FROM doc WHERE k = ? ORDER BY id",
		"select distinct on (a, f(?)) a, put_blob_to_file(b, :H), put_clob_to_file(c, 'x', 8) x from t",
		"with w as (select a, b from t where k = ?) select a, count(*) * 2, put_blob_to_file(max(b), ?) from w group by a",
		"select put_blob_to_file(substring(b from 1 for 10), concat('/t/', a, ')')) /* c */ , a from t",
		NULL
	};
	static const char *const bad[] = {
		/* the rows of the second query would carry no file name */
		"select put_blob_to_file(b, '/x') from t union all select put_blob_to_file(b, '/y') from u",
		"select a, put_clob_to_file(b, '/x') from t where k = 1 except select a, b from u",
		"with w as (select 1 union select 2) select put_blob_to_file(b, '/x') from t, w intersect select b from u",
		/* read as a query by this module; the driver refuses it when it sees that no rows come back */
		"insert into u select put_blob_to_file(b, 'x') from t",
		"select a, coalesce(put_blob_to_file(b, 'x'), 0) from t",
		"select a from t where put_blob_to_file(b, 'x') > 0",
		"select *, put_blob_to_file(b, 'x') from t",
		"select t.*, put_blob_to_file(b, 'x') from t",
		"update t set a = put_clob_to_file(b, 'x')",
		"select put_blob_to_file(b) from t",
		"select put_blob_to_file(b, 'x', 4) from t",
		"select put_blob_to_file(b, 'x', 16, 1208) from t",
		"select put_clob_to_file(b, 'x', 16, 0) from t",
		"select put_blob_to_file(get_blob_from_file('a'), 'x') from t",
		"select put_blob_to_file(b, 'x' from t",
		"insert into t values(get_blob_from_file('/a' || ?))",
		"insert into t values(get_blob_from_file(name))",
		"insert into t values(get_blob_from_file())",
		"insert into t values(get_clob_from_file('a', 2))",
		NULL
	};
	int			i;

	for (i = 0; ok[i]; i++)
		show(ok[i], &with, 1);
	printf("---- used wrongly\n\n");
	for (i = 0; bad[i]; i++)
		show(bad[i], &with, 1);
	printf("---- no transaction, and a backend without file support\n\n");
	show("select put_blob_to_file(b, 'x') from t", &with, 0);
	show("insert into t values(get_blob_from_file('a'))", &without, 1);
	show("select put_clob_to_file(b, 'x') from t", &without, 1);
	printf("---- a backend that takes the file name before the value, and a server that cannot\n\n");
	show("select id, put_blob_to_file(body, '/tmp/d' || id) from doc where id > ?", &namefirst, 1);
	show("select put_clob_to_file(f(note, ?, ?), ?, 32, 1252) as n, ? from doc where k = ?", &namefirst, 1);
	show("select put_blob_to_file(body, ?), put_blob_to_file(g(?), concat(?, ?)) from doc", &namefirst, 1);
	show("select put_blob_to_file(f(?), '/x') from doc where k = ?", &namefirst, 1);
	show("insert into doc values(?, get_blob_from_file(?), get_clob_from_file('/a'))", &namefirst, 1);
	show("select put_blob_to_file(b, 'x') from t", &tooold, 1);
	show("insert into t values(get_blob_from_file('a'))", &tooold, 1);
	return 0;
}

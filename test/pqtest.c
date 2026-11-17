/*
 * pqtest.c - libpq smoke test for the ILE port (same source on Linux).
 * usage: pqtest "<conninfo>" <outfile>
 * Output is UTF-8; compare ILE output with the Linux reference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libpq-fe.h"

#if defined(__ILEC400__)
extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);
#endif

static FILE *out;
static int	fails = 0;

static void
check(PGconn *c, PGresult *r, ExecStatusType want, const char *what)
{
	if (PQresultStatus(r) != want)
	{
		fprintf(out, "FAIL %s: %s / %s", what, PQresStatus(PQresultStatus(r)),
				PQerrorMessage(c));
		fails++;
	}
	else
		fprintf(out, "ok   %s\n", what);
}

static void
dump(PGresult *r)
{
	int			i,
				j;

	for (j = 0; j < PQnfields(r); j++)
		fprintf(out, "%s%s(%u)", j ? "|" : "  cols: ", PQfname(r, j),
				(unsigned) PQftype(r, j));
	fprintf(out, "\n");
	for (i = 0; i < PQntuples(r); i++)
	{
		fprintf(out, "  row%d: ", i);
		for (j = 0; j < PQnfields(r); j++)
			fprintf(out, "%s%s", j ? "|" : "",
					PQgetisnull(r, i, j) ? "<null>" : PQgetvalue(r, i, j));
		fprintf(out, "\n");
	}
}

int
main(int argc, char **argv)
{
	char		conninfo[1024];
	char		path[512];
	PGconn	   *c;
	PGresult   *r;
	const char *pv[3];

	if (argc < 3)
		return 2;
#if defined(__ILEC400__)
	{
		/* CL passes short literals blank-padded to 32 bytes, not terminated */
		char		raw[2][33];
		int			a,
					n;

		for (a = 0; a < 2; a++)
		{
			memcpy(raw[a], argv[a + 1], 32);
			raw[a][32] = '\0';
			n = (int) strlen(raw[a]);
			while (n > 0 && (unsigned char) raw[a][n - 1] == 0x40)
				raw[a][--n] = '\0';
		}
		if (ile_conv(0, 1208, raw[0], strlen(raw[0]), conninfo, sizeof conninfo - 1) < 0 ||
			ile_conv(0, 1208, raw[1], strlen(raw[1]), path, sizeof path - 1) < 0)
			return 3;
	}
#else
	snprintf(conninfo, sizeof conninfo, "%s", argv[1]);
	snprintf(path, sizeof path, "%s", argv[2]);
#endif
	if (conninfo[0] == '/')
	{
		/* first argument is a file holding the connection string (UTF-8) */
		FILE	   *cf = fopen(conninfo, "rb");
		size_t		n;

		if (!cf)
			return 5;
		n = fread(conninfo, 1, sizeof conninfo - 1, cf);
		fclose(cf);
		conninfo[n] = '\0';
		while (n > 0 && (conninfo[n - 1] == '\n' || conninfo[n - 1] == '\r' || conninfo[n - 1] == ' '))
			conninfo[--n] = '\0';
	}
	out = fopen(path, "wb");
	if (!out)
		return 4;
	fprintf(out, "libpq %d\n", PQlibVersion());
	c = PQconnectdb(conninfo);
	if (PQstatus(c) != CONNECTION_OK)
	{
		fprintf(out, "FAIL connect: %s", PQerrorMessage(c));
		fclose(out);
		return 1;
	}
	fprintf(out, "ok   connect server=%d proto=%d enc=%s ssl=%d\n",
			PQserverVersion(c) / 10000, PQprotocolVersion(c),
			PQparameterStatus(c, "client_encoding"), PQsslInUse(c));

	r = PQexec(c, "create temp table t (id int primary key, name varchar(40), "
			   "amt numeric(11,2), d date, ts timestamp, flag boolean, note text)");
	check(c, r, PGRES_COMMAND_OK, "create table");
	PQclear(r);

	pv[0] = "1";
	pv[1] = "Citt\xC3\xA0 di Bergamo \xE2\x82\xAC";	/* a-grave, euro sign */
	pv[2] = "1234567.89";
	r = PQexecParams(c, "insert into t values ($1, $2, $3, date '2026-11-12', "
					 "timestamp '2026-11-12 18:30:00.123456', true, null)",
					 3, NULL, pv, NULL, NULL, 0);
	check(c, r, PGRES_COMMAND_OK, "insert params");
	fprintf(out, "  cmdtuples=%s\n", PQcmdTuples(r));
	PQclear(r);

	r = PQexec(c, "insert into t select g, 'row ' || g, g * 1.5, date '2026-01-01' + g, "
			   "null, g % 2 = 0, repeat('x', g) from generate_series(2, 5) g");
	check(c, r, PGRES_COMMAND_OK, "insert bulk");
	PQclear(r);

	r = PQprepare(c, "s1", "select id, name, amt, d, ts, flag, note from t "
				  "where id <= $1 order by id", 1, NULL);
	check(c, r, PGRES_COMMAND_OK, "prepare");
	PQclear(r);
	r = PQdescribePrepared(c, "s1");
	check(c, r, PGRES_COMMAND_OK, "describe");
	fprintf(out, "  nparams=%d paramtype=%u nfields=%d fmod(amt)=%d\n", PQnparams(r),
			(unsigned) PQparamtype(r, 0), PQnfields(r), PQfmod(r, 2));
	PQclear(r);
	pv[0] = "3";
	r = PQexecPrepared(c, "s1", 1, pv, NULL, NULL, 0);
	check(c, r, PGRES_TUPLES_OK, "exec prepared");
	dump(r);
	PQclear(r);

	r = PQexec(c, "insert into t values (1, 'dup', 0, null, null, null, null)");
	fprintf(out, "%s duplicate key -> sqlstate %s\n",
			PQresultStatus(r) == PGRES_FATAL_ERROR ? "ok  " : "FAIL",
			PQresultErrorField(r, PG_DIAG_SQLSTATE) ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : "?");
	PQclear(r);

	r = PQexec(c, "begin");
	PQclear(r);
	r = PQexec(c, "delete from t");
	fprintf(out, "  deleted=%s\n", PQcmdTuples(r));
	PQclear(r);
	r = PQexec(c, "rollback");
	PQclear(r);
	r = PQexec(c, "select count(*), sum(amt), max(length(note)) from t");
	check(c, r, PGRES_TUPLES_OK, "after rollback");
	dump(r);
	PQclear(r);

	PQfinish(c);
	fprintf(out, "RESULT %d failure(s)\n", fails);
	fclose(out);
	return fails ? 1 : 0;
}

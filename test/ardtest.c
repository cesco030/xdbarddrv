/*
 * ardtest.c - Linux harness: drives xdb_ard_entry() the way Db2 for i does.
 * usage: ardtest <isolation: *NONE|*CHG> [noblock]
 * env: XDB_CONF, XDB_EMU_JOBCCSID, XDB_TEST_RDB (default PGTEST),
 *      XDB_TEST_DIALECT = pg | mysql | mssql (default pg)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "xdbard.h"

extern int	ile_conv(int from, int to, const char *in, size_t inlen, char *out, size_t outsize);
extern int	ile_job_ccsid(void);

#include "ardharn.h"

int
main(int argc, char **argv)
{
	int			n,
				v;
	static xsqlvar pv[4];
	static unsigned char pint[4] = {0, 0, 0, 1};
	static unsigned char pdec[6] = {0x00, 0x12, 0x34, 0x56, 0x78, 0x9D};	/* -1234567.89 dec(11,2) */
	static char pchr[20],
				pdat[10];
	static short ind0 = 0;
	char		t[4];

	iso = argc > 1 ? argv[1] : "*NONE";
	if (getenv("XDB_TEST_RDB"))
		rdbname = getenv("XDB_TEST_RDB");
	if (getenv("XDB_TEST_DIALECT"))
		dia = strcmp(getenv("XDB_TEST_DIALECT"), "mysql") == 0 ? 1 :
			strcmp(getenv("XDB_TEST_DIALECT"), "mssql") == 0 ? 2 :
			strcmp(getenv("XDB_TEST_DIALECT"), "mock") == 0 ? 3 : 0;
	ile_conv(1208, ile_job_ccsid(), " ", 1, t, sizeof t);
	bl = (unsigned char) t[0];

	hdr(0);
	J(in + 76, 10, "");
	J(in + 86, 10, "");
	J(in + 96, 8, "QSQ07050");
	call("ARCN0100", 104);

	hdr(1);
	n = text(138, 112, D("create temp table t (id int primary key, name varchar(30), amt numeric(11,2), d date, ts timestamp, f float8, note text, c char(5))",
						 "create temporary table t (id int primary key, name varchar(30), amt decimal(11,2), d date, ts datetime(6), f double, note text, c char(5))",
						 "create table #t (id int primary key, name varchar(30), amt numeric(11,2), d datetime, ts datetime, f float, note text, c char(5))"));
	call("ARXI0100", n);

	/* prepare + execute with host variables */
	hdr(2);
	J(in + 120, 18, "S1");
	n = text(138, 112, D("INSERT INTO t (id, amt, name, d) VALUES(?, ?, ?, ?)",
						 "INSERT INTO t (id, amt, name, d) VALUES(?, ?, ?, ?)",
						 "INSERT INTO #t (id, amt, name, d) VALUES(?, ?, ?, ?)"));
	call("ARPS0100", n);
	hdr(2);
	J(in + 128, 18, "S1");
	memset(pv, 0, sizeof pv);
	pv[0].sqltype = 496;
	pv[0].sqllen = 4;
	pv[0].sqldata = pint;
	pv[1].sqltype = 485;
	pv[1].sqllen = 11 * 256 + 2;
	pv[1].sqldata = pdec;
	pv[1].sqlind = &ind0;
	J(pchr, 20, "Citt\xC3\xA0 alta");
	pv[2].sqltype = 452;
	pv[2].sqllen = 20;
	pv[2].sqldata = (unsigned char *) pchr;
	pv[2].sqlname.length = 8;
	J(pdat, 10, "31.12.2026");
	pv[3].sqltype = 384;
	pv[3].sqllen = 10;
	pv[3].sqldata = (unsigned char *) pdat;
	memcpy(in + 160, "SQLDA   ", 8);
	{
		short		sh = 4;

		memcpy(in + 160 + 12, &sh, 2);
		memcpy(in + 160 + 14, &sh, 2);
	}
	memcpy(in + 176, pv, sizeof pv);
	v = 160;
	memcpy(in + 112, &v, 4);
	v = 16 + sizeof pv;
	memcpy(in + 116, &v, 4);
	call("ARXP0100", 176 + sizeof pv);
	call("ARXP0100", 176 + sizeof pv);	/* duplicate key */

	hdr(3);
	n = text(138, 112, D("insert into t select g, 'row ' || g, g * 1.5, date '2026-01-01' + g, timestamp '2026-11-12 18:30:00.5', g / 3.0, repeat('x', g), 'ab' from generate_series(2, 4) g",
						 "insert into t select g, concat('row ', g), g * 1.5, date_add('2026-01-01', interval g day), '2026-11-12 18:30:00.5', g / 3.0, repeat('x', g), 'ab' from (select 2 g union all select 3 union all select 4) x",
						 "insert into #t select g, 'row ' + cast(g as varchar(10)), g * 1.5, dateadd(day, g, '20260101'), '2026-11-12T18:30:00.500', g / 3.0, replicate('x', g), 'ab' from (select 2 g union all select 3 union all select 4) x"));
	call("ARXI0100", n);

	/* prepare+describe, open, fetch, close */
	hdr(4);
	J(in + 120, 18, "S2");
	n = text(138, 112, D("SELECT id, name, amt, d, ts, f, note, c, amt * 2 AS dbl FROM t WHERE id <= ? ORDER BY id",
						 "SELECT id, name, amt, d, ts, f, note, c, amt * 2 AS dbl FROM t WHERE id <= ? ORDER BY id",
						 "SELECT id, name, amt, d, ts, f, note, c, amt * 2 AS dbl FROM #t WHERE id <= ? ORDER BY id"));
	if (call("ARPD0100", n) == 0)
		dump_sqlda(out);
	hdr(4);
	in[130] = argc > 2 ? 0xF0 : 0xF1;
	if (ile_job_ccsid() == 1208)
		in[130] = argc > 2 ? '0' : '1';
	J(in + 148, 18, "C1");
	pint[3] = 3;
	memset(pv, 0, sizeof pv);
	pv[0].sqltype = 496;
	pv[0].sqllen = 4;
	pv[0].sqldata = pint;
	memcpy(in + 176, "SQLDA   ", 8);
	{
		short		sh = 1;

		memcpy(in + 176 + 12, &sh, 2);
		memcpy(in + 176 + 14, &sh, 2);
	}
	memcpy(in + 192, pv, sizeof(xsqlvar));
	v = 176;
	memcpy(in + 132, &v, 4);
	v = 16 + sizeof(xsqlvar);
	memcpy(in + 136, &v, 4);
	if (call("AROC0100", 192 + sizeof(xsqlvar)) == 0)
	{
		memcpy(&v, out + 4, 4);
		memcpy(sda, out + v, outlen - v);
		printf("  block=%c hold=%c\n", out[0] & 0x0F ? '1' : '0', out[1] & 0x0F ? '1' : '0');
	}
	for (n = 0; n < 6; n++)
	{
		hdr(4);
		J(in + 112, 18, "C1");
		if (call("ARFC0100", 130) != 0)
			break;
		dump_rows();
	}
	hdr(4);
	J(in + 80, 18, "C1");
	call("ARCC0100", 98);

	hdr(5);
	n = text(138, 112, "select * from no_such_table");
	call("ARXI0100", n);
	hdr(6);
	n = text(138, 112, D("update t set note = 'after error' where id = 2",
						 "update t set note = 'after error' where id = 2",
						 "update #t set note = 'after error' where id = 2"));
	call("ARXI0100", n);
	if (strcmp(iso, "*NONE") != 0)
	{
		char		info[80],
					status[132],
					ret[16];

		memset(info, 0, sizeof info);
		v = 7;
		memcpy(info, &v, 4);
		J(info + 4, 18, rdbname);
		memset(status, 0, sizeof status);
		J(status + 4, 1, "R");
		xdb_commit_exit(info, status, ret);
		printf("rollback through the commit exit\n");
		hdr(7);
		n = text(138, 112, D("select count(*) from t", "select count(*) from t", "select count(*) from #t"));
		call("ARXI0100", n);
	}
	hdr(0);
	v = 2;
	memcpy(in + 28, &v, 4);
	call("ARDI0100", 32);
	return 0;
}

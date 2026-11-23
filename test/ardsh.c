/*
 * ardsh.c - scriptable Linux harness for the ARD: one request per line.
 *   C                 connect            D             disconnect
 *   ISO *CHG|*NONE    isolation for the next requests
 *   X sql             execute immediate
 *   P name sql        prepare
 *   E name a|b|c      execute prepared (i:5 = integer, n: = NULL, else VARCHAR; host
 *                     variables of the other types: @s:5 SMALLINT, @b:5 BIGINT, @f:1.5
 *                     DOUBLE, @r:1.5 REAL, @p15,4:12.5 DECIMAL(15,4), @z15,4:12.5 zoned,
 *                     @c10:abc CHAR(10), @D:2026-01-31 DATE, @T:13:45:10 TIME,
 *                     @Z:2026-01-31-13.45.10.123456 TIMESTAMP, @g:text VARGRAPHIC,
 *                     @v:text VARCHAR (also an empty one);
 *                     the value @null is the NULL value of that type, @D:@null)
 *   Q sql             prepare, describe, open, fetch everything, close
 *   R sql             the same in one fixed statement slot (as STRSQL does)
 *   O cur sql         open a cursor and leave it open (a|b params after " ## ")
 *   F cur             fetch one block         K cur    close
 *   A cur [## a|b]    open the cursor again without preparing its statement, as a
 *                     program does with a cursor it opens more than once
 *   COMMIT / ROLLBACK what the system does for them: the commit exit of every
 *                     connection, in the order of a commit (see ardharn.h)
 *   RDB name          the connection the next requests are for
 *   SLEEP n           wait n seconds         ! command   run a shell command
 * env: XDB_CONF, XDB_EMU_JOBCCSID, XDB_TEST_RDB, XDB_SH_MAXROWS (rows printed),
 *      XDB_SH_FLOAT17 (floating point values with all their digits),
 *      XDB_EMU_CMT (the kind of commitment resource the emulated system takes)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include "xdbard.h"

extern int	ile_conv(int from, int to, const char *in, size_t inlen, char *out, size_t outsize);
extern int	ile_job_ccsid(void);

#include "ardharn.h"

static int	sect = 10;
static struct
{
	char		name[20];
	int			section;
	char		sda[8192];
	short		told;			/* columns the describe told */
}			curs[16];
static short told_cols;
static struct
{
	char		name[20];
	int			section;
	short		told;
}			last;				/* the cursor of the last O, for A */
static int	nrows_total,
			maxprint = 5;

/* parameters "a|b|c" -> SQLDA at in+off; returns end offset */
static unsigned char pbuf[40000];
static short pind[32];
static int
parms(const char *spec, int off, int offfld)
{
	static xsqlvar pv[32];
	static char tmp[20000];
	char	   *tok;
	int			n = 0,
				used = 0,
				v;

	memset(pv, 0, sizeof pv);
	snprintf(tmp, sizeof tmp, "%s", spec);
	for (tok = strtok(tmp, "|"); tok && n < 32; tok = strtok(NULL, "|"), n++)
	{
		pind[n] = 0;
		pv[n].sqlind = &pind[n];
		if (strncmp(tok, "i:", 2) == 0)
		{
			int			x = atoi(tok + 2);

			pv[n].sqltype = 497;
			pv[n].sqllen = 4;
			pbuf[used] = (unsigned char) (x >> 24);
			pbuf[used + 1] = (unsigned char) (x >> 16);
			pbuf[used + 2] = (unsigned char) (x >> 8);
			pbuf[used + 3] = (unsigned char) x;
			pv[n].sqldata = pbuf + used;
			used += 4;
		}
		else if (tok[0] == '@' && tok[1] && strchr(tok, ':'))
		{
			/* a host variable of another type, laid out as the system passes it */
			const char *val = strchr(tok, ':') + 1;
			char		k = tok[1];
			int			l,
						i;

			if (strcmp(val, "@null") == 0)
			{
				/* the NULL value of that type: the indicator says so, the data is not looked at */
				pind[n] = -1;
				val = "0";
			}
			pv[n].sqldata = pbuf + used;
			if (k == 's' || k == 'b')
			{
				long long	x = atoll(val);

				l = k == 's' ? 2 : 8;
				pv[n].sqltype = k == 's' ? 501 : 493;
				pv[n].sqllen = (short) l;
				for (i = 0; i < l; i++)
					pbuf[used + i] = (unsigned char) (x >> (8 * (l - 1 - i)));
			}
			else if (k == 'f' || k == 'r')
			{
				double		dv = atof(val);
				float		fv = (float) dv;
				unsigned char *src = k == 'f' ? (unsigned char *) &dv : (unsigned char *) &fv;
				int			one = 1;

				l = k == 'f' ? 8 : 4;
				pv[n].sqltype = 481;
				pv[n].sqllen = (short) l;
				for (i = 0; i < l; i++)
					pbuf[used + i] = *(char *) &one ? src[l - 1 - i] : src[i];
			}
			else if (k == 'p' || k == 'z')
			{
				/* digits of the value, scaled, right aligned; the sign in the last nibble or zone */
				int			prec = atoi(tok + 2),
							scale = strchr(tok, ',') && strchr(tok, ',') < val ? atoi(strchr(tok, ',') + 1) : 0;
				char		dg[80];
				int			neg = val[0] == '-',
							nd = 0,
							frac = -1;
				const char *q;

				memset(dg, '0', sizeof dg);
				for (q = val + neg; *q; q++)
					if (*q == '.')
						frac = 0;
					else if (frac < 0 || frac < scale)
					{
						if (nd < 70)
							dg[nd++] = *q;
						if (frac >= 0)
							frac++;
					}
				for (i = frac < 0 ? 0 : frac; i < scale && nd < 70; i++)
					dg[nd++] = '0';
				pv[n].sqltype = k == 'p' ? 485 : 489;
				pv[n].sqllen = (short) (prec * 256 + scale);
				if (k == 'p')
				{
					l = prec / 2 + 1;
					memset(pbuf + used, 0, (size_t) l);
					for (i = 0; i < prec; i++)
					{
						int			d = i < nd ? dg[nd - 1 - i] - '0' : 0;
						int			nib = 1 + i;	/* nibbles from the right, 0 is the sign */

						pbuf[used + l - 1 - nib / 2] |= (unsigned char) (nib % 2 ? d << 4 : d);
					}
					pbuf[used + l - 1] |= neg ? 0x0D : 0x0F;
				}
				else
				{
					l = prec;
					for (i = 0; i < prec; i++)
						pbuf[used + l - 1 - i] = (unsigned char) (0xF0 | (i < nd ? dg[nd - 1 - i] - '0' : 0));
					if (neg)
						pbuf[used + l - 1] = (unsigned char) (0xD0 | (pbuf[used + l - 1] & 0x0F));
				}
			}
			else if (k == 'v')
			{
				/* VARCHAR, said so: the way to pass an empty one, or text that looks like another form */
				static char c[20000];

				l = ile_conv(1208, ile_job_ccsid(), val, strlen(val), c, sizeof c);
				if (l < 0)
					l = 0;
				pv[n].sqltype = 449;
				pv[n].sqllen = (short) l;
				pbuf[used] = (unsigned char) (l >> 8);
				pbuf[used + 1] = (unsigned char) l;
				memcpy(pbuf + used + 2, c, (size_t) l);
				l += 2;
			}
			else if (k == 'g')
			{
				l = ile_conv(1208, 1200, val, strlen(val), (char *) pbuf + used + 2, 24000);
				if (l < 0)
					l = 0;
				pv[n].sqltype = 465;
				pv[n].sqllen = (short) (l / 2);
				pbuf[used] = (unsigned char) ((l / 2) >> 8);
				pbuf[used + 1] = (unsigned char) (l / 2);
				l += 2;
			}
			else
			{
				/* CHAR(n), DATE, TIME, TIMESTAMP: text in the CCSID of the job */
				static char c[4000];
				int			want = k == 'c' ? atoi(tok + 2) : k == 'D' ? 10 : k == 'T' ? 8 : 26;

				l = ile_conv(1208, ile_job_ccsid(), val, strlen(val), c, sizeof c);
				if (l < 0)
					l = 0;
				if (k == 'c' || l > want)
				{
					char		blank;

					ile_conv(1208, ile_job_ccsid(), " ", 1, &blank, 1);
					for (i = l; i < want; i++)
						c[i] = blank;
					l = want;
				}
				pv[n].sqltype = k == 'c' ? 453 : k == 'D' ? 385 : k == 'T' ? 389 : 393;
				pv[n].sqllen = (short) l;
				memcpy(pbuf + used, c, (size_t) l);
			}
			used += l;
		}
		else if (tok[0] == 't' && strchr(tok, ':'))
		{
			/* t<sqltype>:  a host variable of any SQL type, 8 bytes of data */
			pv[n].sqltype = (short) atoi(tok + 1);
			pv[n].sqllen = 8;
			memset(pbuf + used, 0, 8);
			pv[n].sqldata = pbuf + used;
			used += 8;
		}
		else if (strncmp(tok, "n:", 2) == 0)
		{
			pv[n].sqltype = 449;
			pv[n].sqllen = 10;
			pv[n].sqldata = pbuf + used;
			pind[n] = -1;
			used += 12;
		}
		else
		{
			static char c[20000];
			int			l = ile_conv(1208, ile_job_ccsid(), tok, strlen(tok), c, sizeof c);

			pv[n].sqltype = 449;
			pv[n].sqllen = (short) l;
			pbuf[used] = (unsigned char) (l >> 8);
			pbuf[used + 1] = (unsigned char) l;
			memcpy(pbuf + used + 2, c, l);
			pv[n].sqldata = pbuf + used;
			used += l + 2;
		}
	}
	/*
	 * A NULL host variable as Db2 hands it to the driver (seen on IBM i 7.5,
	 * for an EXECUTE and for an OPEN): no data pointer, and an indicator
	 * pointer that cannot be used.  Here that is an address that ends the
	 * harness when it is read.
	 */
	for (v = 0; v < n; v++)
		if (pind[v] < 0)
		{
			pv[v].sqldata = NULL;
			pv[v].sqlind = (short *) 8;
		}
	off = (off + 15) & ~15;
	memcpy(in + off, "SQLDA   ", 8);
	{
		short		sh = (short) n;

		memcpy(in + off + 12, &sh, 2);
		memcpy(in + off + 14, &sh, 2);
	}
	memcpy(in + off + 16, pv, n * sizeof(xsqlvar));
	memcpy(in + offfld, &off, 4);
	v = 16 + n * (int) sizeof(xsqlvar);
	memcpy(in + offfld + 4, &v, 4);
	return off + v;
}

/* sql = NULL: the statement of the section is prepared already, parms = " ## a|b" or NULL */
static int
open_cur(const char *name, int section, char *sql, char *parms_of_reopen)
{
	char	   *pp = sql ? strstr(sql, " ## ") : parms_of_reopen;
	int			n,
				v;
	short		now;

	if (pp)
		*pp = '\0';
	if (sql != NULL)
	{
		hdr(section);
		J(in + 120, 18, name);
		n = text(256, 112, sql);
		if (call("ARPD0100", n) != 0)
			return -1;
		dump_sqlda(out);
		memcpy(&told_cols, out + 14, 2);
	}
	hdr(section);
	in[130] = ile_job_ccsid() == 1208 ? '1' : 0xF1;
	J(in + 148, 18, name);
	n = 192;
	if (pp)
		n = parms(pp + 4, 192, 132);
	if (call("AROC0100", n) != 0)
		return -1;
	memcpy(&v, out + 4, 4);
	memcpy(sda, out + v, outlen - v);
	/* columns the describe did not tell: the rows of a procedure on MySQL */
	memcpy(&now, sda + 14, 2);
	if (now != told_cols)
	{
		printf("  the open tells %d columns:\n", now);
		dump_sqlda(sda);
	}
	return 0;
}

static int
count_rows(void)
{
	/* rows are counted by sqlerrd(3) of the fetch */
	int			r;

	memcpy(&r, ca + 96 + 8, 4);
	return r;
}

int
main(int argc, char **argv)
{
	static char line[16384];
	int			n,
				v,
				i;

	/* line by line, so that what the driver writes to stderr stays where it belongs */
	setvbuf(stdout, NULL, _IOLBF, 0);
	iso = "*NONE";
	if (getenv("XDB_TEST_RDB"))
		rdbname = getenv("XDB_TEST_RDB");
	if (getenv("XDB_SH_MAXROWS"))
		maxprint = atoi(getenv("XDB_SH_MAXROWS"));
	{
		char		t[4];

		ile_conv(1208, ile_job_ccsid(), " ", 1, t, sizeof t);
		bl = (unsigned char) t[0];
	}
	while (fgets(line, sizeof line, stdin))
	{
		char	   *a = line + strlen(line);
		char		w1[32],
					w2[32];
		char	   *rest;

		while (a > line && (a[-1] == '\n' || a[-1] == '\r'))
			*--a = '\0';
		if (line[0] == '\0' || line[0] == '#')
			continue;
		printf("> %.100s\n", line);
		w1[0] = w2[0] = '\0';
		sscanf(line, "%31s %31s", w1, w2);
		rest = strchr(line, ' ');
		rest = rest ? rest + 1 : "";
		if (strcmp(w1, "C") == 0)
		{
			hdr(0);
			J(in + 76, 10, w2);
			J(in + 86, 10, strlen(rest) > strlen(w2) ? rest + strlen(w2) + 1 : "");
			J(in + 96, 8, "QSQ07050");
			call("ARCN0100", 104);
		}
		else if (strcmp(w1, "D") == 0)
		{
			hdr(0);
			v = 2;
			memcpy(in + 28, &v, 4);
			call("ARDI0100", 32);
		}
		else if (strcmp(w1, "ISO") == 0)
		{
			static char isob[16];

			snprintf(isob, sizeof isob, "%s", w2);
			iso = isob;
		}
		else if (strcmp(w1, "X") == 0)
		{
			hdr(++sect);
			n = text(138, 112, rest);
			call("ARXI0100", n);
			memcpy(&v, ca + 104, 4);
			printf("  rows=%d\n", v);
		}
		else if (strcmp(w1, "P") == 0)
		{
			hdr(++sect);
			J(in + 120, 18, w2);
			n = text(138, 112, rest + strlen(w2) + 1);
			call("ARPS0100", n);
		}
		else if (strcmp(w1, "E") == 0)
		{
			hdr(0);
			J(in + 128, 18, w2);
			n = parms(strlen(rest) > strlen(w2) ? rest + strlen(w2) + 1 : "", 160, 112);
			call("ARXP0100", n);
			memcpy(&v, ca + 104, 4);
			printf("  rows=%d\n", v);
		}
		else if (strcmp(w1, "Q") == 0 || strcmp(w1, "R") == 0)
		{
			int			total = 0,
						calls = 0;

			/* R: always the same statement slot, as STRSQL does */
			if (w1[0] == 'R')
				sect = 9;
			else
				++sect;
			dump_done = 0;
			if (open_cur("QC", sect, rest, NULL) == 0)
			{
				for (;;)
				{
					char		f[8];
					int			code;

					hdr(sect);
					J(in + 112, 18, "QC");
					J(f, 8, "ARFC0100");
					xdb_ard_entry(&level, in, &n, f, ca, &ccsid, out, &outlen);
					memcpy(&code, ca + 12, 4);
					if (code != 0)
					{
						char		st[16],
									msg[128];
						short		ml;

						memcpy(&ml, ca + 16, 2);
						U(ca + 131, 5, st);
						M(ca, msg);
						printf("ARFC0100 -> sqlcode %d sqlstate %s %s\n", code, st, msg);
						break;
					}
					calls++;
					dump_limit = maxprint;
					dump_rows();
					total += count_rows();
				}
				printf("  total rows=%d in %d fetch calls\n", total, calls);
				hdr(sect);
				J(in + 80, 18, "QC");
				call("ARCC0100", 98);
			}
		}
		else if (strcmp(w1, "O") == 0)
		{
			for (i = 0; i < 16 && curs[i].name[0]; i++)
				;
			if (i < 16 && open_cur(w2, ++sect, rest + strlen(w2) + 1, NULL) == 0)
			{
				snprintf(curs[i].name, sizeof curs[i].name, "%s", w2);
				curs[i].section = sect;
				curs[i].told = told_cols;
				memcpy(curs[i].sda, sda, sizeof sda);
			}
			if (i < 16)
			{
				/* the statement stays prepared when the open fails or the cursor is closed */
				snprintf(last.name, sizeof last.name, "%s", w2);
				last.section = sect;
				last.told = told_cols;
			}
		}
		else if (strcmp(w1, "A") == 0)
		{
			static char pb[4096];

			if (strcmp(last.name, w2) != 0)
			{
				printf("  A is for the cursor of the last O\n");
				continue;
			}
			for (i = 0; i < 16 && strcmp(curs[i].name, w2) != 0; i++)
				;
			if (i == 16)
				for (i = 0; i < 16 && curs[i].name[0]; i++)
					;
			pb[0] = '\0';
			if (strstr(rest, "## "))
				snprintf(pb, sizeof pb, " %s", strstr(rest, "## "));
			told_cols = last.told;
			if (i < 16 && open_cur(w2, last.section, NULL, pb[0] ? pb : NULL) == 0)
			{
				snprintf(curs[i].name, sizeof curs[i].name, "%s", w2);
				curs[i].section = last.section;
				curs[i].told = last.told;
				memcpy(curs[i].sda, sda, sizeof sda);
			}
		}
		else if (strcmp(w1, "F") == 0 || strcmp(w1, "K") == 0)
		{
			for (i = 0; i < 16 && strcmp(curs[i].name, w2) != 0; i++)
				;
			if (i == 16)
			{
				printf("  no such cursor\n");
				continue;
			}
			hdr(curs[i].section);
			if (w1[0] == 'F')
			{
				J(in + 112, 18, w2);
				memcpy(sda, curs[i].sda, sizeof sda);
				if (call("ARFC0100", 130) == 0)
				{
					printf("  rows=%d\n", count_rows());
					dump_limit = 2;
					dump_done = 0;
					dump_rows();
				}
			}
			else
			{
				J(in + 80, 18, w2);
				call("ARCC0100", 98);
				curs[i].name[0] = '\0';
			}
		}
		else if (strcmp(w1, "COMMIT") == 0 || strcmp(w1, "ROLLBACK") == 0)
		{
			/* what the system does for the program's COMMIT or ROLLBACK, on all connections */
			harn_end(w1[0] == 'C');
			for (i = 0; i < 16; i++)
				curs[i].name[0] = '\0';
		}
		else if (strcmp(w1, "SLEEP") == 0)
			sleep((unsigned) atoi(w2));
		else if (strcmp(w1, "!") == 0)
		{
			/* a shell command: something that happens outside the program */
			fflush(stdout);
			if (system(rest) != 0)
				printf("  (the command failed)\n");
		}
		else if (strcmp(w1, "RDB") == 0)
		{
			/* the connection the next requests are for (as SET CONNECTION, or before a further C) */
			static char names[8][20];
			static int	nn;

			snprintf(names[nn % 8], sizeof names[0], "%s", w2);
			rdbname = names[nn++ % 8];
		}
		else
			printf("  ?\n");
	}
	return 0;
}

/* ardharn.h - helpers shared by the Linux ARD harnesses (ardtest, ardsh) */
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

static char in[65536] __attribute__((aligned(16)));
static char out[65536] __attribute__((aligned(16)));
static char ca[136];
static int	harn_cmt_gen;		/* number of the commitment resource last registered */
static int	outlen,
			ccsid,
			level = 1;
static const char *iso;
static const char *rdbname = "PGTEST";
static int	dia;				/* 0 pg, 1 mysql, 2 mssql, 3 mssql backend on tdsmock */

/* statement text for the dialect under test */
static const char *
D(const char *pg, const char *my, const char *ms)
{
	/* dialect 3 = the SQL Server backend against test/tdsmock.py (sqlite SQL) */
	if (dia == 3 && strstr(ms, "dateadd"))
		return "insert into #t select g, 'row ' || g, g * 1.5, date('2026-01-01', '+' || g || ' day'), '2026-11-12 18:30:00.500', g / 3.0, substr('xxxxxxxx', 1, g), 'ab' from (select 2 g union all select 3 union all select 4) x";
	return dia == 1 ? my : dia >= 2 ? ms : pg;
}
static unsigned char bl;

static void
J(char *dst, int len, const char *s)
{
	char		t[256];
	int			n = ile_conv(1208, ile_job_ccsid(), s, strlen(s), t, sizeof t);

	memset(dst, bl, len);
	memcpy(dst, t, n > len ? len : n);
}

static void
U(const char *src, int len, char *dst)
{
	int			n = ile_conv(ile_job_ccsid(), 1208, src, len, dst, 255);

	dst[n < 0 ? 0 : n] = 0;
	while (n > 0 && dst[n - 1] == ' ')
		dst[--n] = 0;
}

/*
 * SQLERRMC as text.  When it holds message data (tokens with a two byte
 * length in front, as Db2's own messages want them) the tokens are shown
 * as {a|b|c}.
 */
static void
M(const char *ca, char *dst)
{
	short		ml;
	const unsigned char *p = (const unsigned char *) ca + 18;
	int			i = 0,
				n = 0;

	memcpy(&ml, ca + 16, 2);
	while (ml >= 2 && i + 2 <= ml)
	{
		int			l = (p[i] << 8) | p[i + 1];

		if (i + 2 + l > ml)
			break;
		i += 2 + l;
		n++;
	}
	if (n == 0 || i != ml)
	{
		U(ca + 18, ml, dst);
		return;
	}
	strcpy(dst, "{");
	for (i = 0; i < ml;)
	{
		int			l = (p[i] << 8) | p[i + 1];
		char		t[128];

		U((const char *) p + i + 2, l, t);
		if (i > 0)
			strcat(dst, "|");
		strcat(dst, t);
		i += 2 + l;
	}
	strcat(dst, "}");
}

/*
 * The commitment resources of this process, as the system would keep them
 * for one commitment definition, and its part in a commit: the resources
 * that are asked first (P), then the last agent (A), then the outcome for
 * the others (C or R).  XDB_EMU_CMT limits what the emulated system takes:
 * asked = no last agent, told = one-phase only, none = no resource at all.
 */
static struct
{
	char		rdb[20];
	int			gen;
	int			kind;
	int			handle;
	int			live;
}			harn_res[64];
static int	harn_nres;

static void
harn_register(int gen, int kind)
{
	const char *lim = getenv("XDB_EMU_CMT");
	int			max = lim == NULL ? XDB_CMT_LAST : strcmp(lim, "asked") == 0 ? XDB_CMT_ASKED :
		strcmp(lim, "told") == 0 ? XDB_CMT_TOLD : strcmp(lim, "none") == 0 ? XDB_CMT_NONE : XDB_CMT_LAST;
	int			i,
				last = 0;
	char		id[8];
	static const char data[8] = {0, 13, 0, 0, 0, 0, 0, 0};

	for (i = 0; i < harn_nres; i++)
		if (harn_res[i].live && harn_res[i].kind == XDB_CMT_LAST)
			last = 1;
	J(id, 7, "CPF8369");
	for (; kind > XDB_CMT_NONE; kind--)
	{
		if (kind <= max && !(kind == XDB_CMT_LAST && last))
			break;
		xdb_commit_refused(in, kind, id, data);
	}
	if (kind != XDB_CMT_NONE && harn_nres < 64)
	{
		snprintf(harn_res[harn_nres].rdb, sizeof harn_res[0].rdb, "%s", rdbname);
		harn_res[harn_nres].gen = gen;
		harn_res[harn_nres].kind = kind;
		harn_res[harn_nres].handle = harn_nres + 1;
		harn_res[harn_nres].live = 1;
		harn_nres++;
	}
	harn_cmt_gen = gen;
	xdb_commit_registered(in, kind, kind != XDB_CMT_NONE ? harn_nres : 0);
}

/* one request to the driver, with what the program stub does around it */
static void
harn_request(char *f, int inlen)
{
	char		lib[10];
	int			kind = 0,
				gen,
				gone = xdb_commit_handle(in, f);

	xdb_ard_entry(&level, in, &inlen, f, ca, &ccsid, out, &outlen);
	if (gone)
	{
		char		id[8];

		J(id, 7, "       ");
		harn_res[gone - 1].live = 0;
		xdb_commit_removed(gone, 1, id);
	}
	gen = xdb_commit_wanted(in, lib, &kind);
	if (gen)
		harn_register(gen, kind);
}

/* one call of the exit program for resource i; gives the vote back ('1', '2', '3') */
static char
harn_exit(int i, const char *action)
{
	char		info[80],
				status[132],
				ret[16],
				t[4];
	int			v = 7;

	memset(info, 0, sizeof info);
	memcpy(info, &v, 4);
	J(info + 4, 18, harn_res[i].rdb);
	memcpy(info + 22, &harn_res[i].gen, 4);
	memset(status, 0, sizeof status);
	J(status + 4, 1, action);
	memset(ret, 0, sizeof ret);
	xdb_commit_exit(info, status, ret);
	memcpy(&v, ret, 4);
	if (v != 7)
		printf("  THE EXIT ANSWERED LENGTH %d\n", v);
	U(ret + 6, 1, t);
	if (t[0] == '1' && action[0] != 'P')
	{
		harn_res[i].live = 0;
		printf("  resource %d (%s) has ended and is removed\n", harn_res[i].gen, harn_res[i].rdb);
	}
	U(ret + 4, 1, t);
	return t[0];
}

/* COMMIT (1) or ROLLBACK (0) of the program; 0 = done as asked, -1 = the commit became a rollback */
static int
harn_end(int commit)
{
	int			i,
				n = harn_nres,
				failed = !commit;
	char		vote[64];

	memset(vote, 0, sizeof vote);
	for (i = 0; i < n && !failed; i++)
		if (harn_res[i].live && harn_res[i].kind == XDB_CMT_ASKED)
		{
			vote[i] = harn_exit(i, "P");
			if (vote[i] == '2')
			{
				printf("  %s votes against the commit\n", harn_res[i].rdb);
				failed = 1;
			}
		}
	for (i = 0; i < n && !failed; i++)
		if (harn_res[i].live && harn_res[i].kind == XDB_CMT_LAST)
		{
			vote[i] = harn_exit(i, "A");
			if (vote[i] == '2')
			{
				printf("  %s (last agent) has not committed\n", harn_res[i].rdb);
				failed = 1;
			}
		}
	for (i = 0; i < n; i++)
	{
		if (!harn_res[i].live)
			continue;
		/* the last agent has done its part when it was asked; who voted read-only is left alone */
		if ((harn_res[i].kind == XDB_CMT_LAST && vote[i]) || vote[i] == '3')
			continue;
		harn_exit(i, failed ? "R" : "C");
	}
	if (commit && failed)
		printf("  COMMIT FAILED: the unit of work is rolled back\n");
	return (commit && failed) ? -1 : 0;
}

static void
hdr(int section)
{
	int			v;

	memset(in, 0, sizeof in);
	v = sizeof out;
	memcpy(in, &v, 4);
	v = 7;
	memcpy(in + 4, &v, 4);
	J(in + 8, 18, rdbname);
	J(in + 26, 18, "TESTLIB");
	J(in + 44, 18, "TESTPGM");
	memcpy(in + 72, &section, 4);
	v = ile_job_ccsid();
	memcpy(in + 76, &v, 4);
	J(in + 80, 1, "'");
	J(in + 81, 1, ".");
	J(in + 82, 10, "*EUR");
	J(in + 92, 10, "*ISO");
	J(in + 102, 10, iso);
}

static int
call(const char *fmt, int inlen)
{
	char		f[8],
				st[16],
				msg[128];
	int			code;
	short		ml;

	J(f, 8, fmt);
	memset(out, 0x55, 4096);
	harn_request(f, inlen);
	memcpy(&code, ca + 12, 4);
	memcpy(&ml, ca + 16, 2);
	U(ca + 131, 5, st);
	M(ca, msg);
	printf("%s -> sqlcode %d sqlstate %s outlen %d %s\n", fmt, code, st, outlen, msg);
	return code;
}

static int
text(int off, int offfld, const char *sql)
{
	char		t[16384];
	int			n = ile_conv(1208, ile_job_ccsid(), sql, strlen(sql), t, sizeof t);

	if (n < 0)
	{
		printf("  (statement text is not convertible to the job CCSID)\n");
		n = 0;
	}
	memcpy(in + off, t, n);
	memcpy(in + offfld, &off, 4);
	memcpy(in + offfld + 4, &n, 4);
	return off + n;
}

static void
dump_sqlda(const char *p)
{
	short		n,
				i,
				t,
				l;
	char		nm[64];

	memcpy(&n, p + 14, 2);
	for (i = 0; i < n; i++)
	{
		const unsigned char *v = (const unsigned char *) p + 16 + i * sizeof(xsqlvar);
		short		nl;

		memcpy(&t, v, 2);
		memcpy(&l, v + 2, 2);
		memcpy(&nl, v + offsetof(xsqlvar, sqlname), 2);
		U((const char *) v + offsetof(xsqlvar, sqlname) + 2, nl, nm);
		printf("  col %s type %d len %d ccsid %d\n", nm, t, (unsigned short) l,
			   (v[offsetof(xsqlvar, sqldata) + 2] << 8) | v[offsetof(xsqlvar, sqldata) + 3]);
	}
}

/* decode rows using the SQLDA returned by open (kept in sda) */
static char sda[8192];
static int	dump_limit = -1,
			dump_done;
static void
dump_rows(void)
{
	int			off;
	const unsigned char *p,
			   *end = (const unsigned char *) out + outlen;
	short		n,
				i;

	memcpy(&off, out, 4);
	if (off == 0)
		return;
	p = (const unsigned char *) out + off;
	memcpy(&n, sda + 14, 2);
	while (p < end)
	{
		if (dump_limit >= 0 && dump_done >= dump_limit)
			return;
		dump_done++;
		printf("  row:");
		for (i = 0; i < n; i++)
		{
			const unsigned char *v = (const unsigned char *) sda + 16 + i * sizeof(xsqlvar);
			short		t,
						l;
			char		b[1100];
			int			k;
			long long	ll = 0;

			memcpy(&t, v, 2);
			memcpy(&l, v + 2, 2);
			t &= ~1;
			if (*p++ & 0x80)
			{
				printf(" <%s>", p[-1] == 0xFF ? "null" : "conv-error");
				continue;
			}
			switch (t)
			{
				case 500:
				case 496:
				case 492:
					k = t == 500 ? 2 : t == 496 ? 4 : 8;
					ll = (p[0] & 0x80) ? -1 : 0;
					for (int q = 0; q < k; q++)
						ll = (ll << 8) | p[q];
					printf(" %lld", ll);
					p += k;
					break;
				case 480:
					{
						unsigned char r[8];
						double		d;

						for (k = 0; k < 8; k++)
							r[k] = p[7 - k];
						memcpy(&d, r, 8);
						printf(" %g", d);
						p += 8;
						break;
					}
				case 484:
					k = ((l >> 8) & 0xFF) / 2 + 1;
					printf(" x'");
					for (int q = 0; q < k; q++)
						printf("%02X", p[q]);
					printf("'");
					p += k;
					break;
				case 448:
					k = (p[0] << 8) | p[1];
					U((const char *) p + 2, k, b);
					printf(" [%s]", b);
					p += 2 + k;
					break;
				default:
					U((const char *) p, l, b);
					printf(" [%s]", b);
					p += l;
					break;
			}
		}
		printf("\n");
	}
}


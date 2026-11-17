/*
 * xdbprf_emu.c - stand-in for xdbprfc.c on Linux: profiles come from the
 * text file named by XDB_PRF_EMU, one item per line, fields separated by |
 *
 *   P|rdb|type|sts|host|port|dbname|user|tls|timeout|ccsid|textlen|scale|trace
 *   O|rdb|keyword|kind|value
 *   W|rdb|password
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xdbprf.h"

extern int	ile_conv(int from, int to, const char *in, size_t inlen, char *out, size_t outsize);
extern int	ile_job_ccsid(void);

/* UTF-8 text -> field in the (emulated) job CCSID, blank padded */
static void
put(char *dst, int len, const char *src)
{
	char		t[1024];
	char		b[4];
	int			n = ile_conv(1208, ile_job_ccsid(), src, strlen(src), t, sizeof t);

	ile_conv(1208, ile_job_ccsid(), " ", 1, b, sizeof b);
	memset(dst, b[0], len);
	if (n > 0)
		memcpy(dst, t, n > len ? len : n);
}

static void
putnum(char *dst, int len, const char *src)
{
	char		t[16];

	snprintf(t, sizeof t, "%0*d", len, atoi(src));
	put(dst, len, t);
}

static int
split(char *line, char **f, int max)
{
	int			n = 0;
	char	   *p = line;

	line[strcspn(line, "\r\n")] = '\0';
	while (n < max)
	{
		f[n++] = p;
		p = strchr(p, '|');
		if (p == NULL)
			break;
		*p++ = '\0';
	}
	return n;
}

static int
same(const char *rdb18, const char *name)
{
	char		k[18];

	put(k, 18, name);
	return memcmp(k, rdb18, 18) == 0;
}

void
xdbprf_mylib(char *lib10)
{
	put(lib10, 10, "EMULIB");
}

int
xdbprf_read(const char *lib10, const char *rdb18, xdbprf_rec *out)
{
	const char *path = getenv("XDB_PRF_EMU");
	FILE	   *fp = path ? fopen(path, "r") : NULL;
	char		line[1024];
	char	   *f[16];
	int			rc = 1;

	if (fp == NULL)
		return -1;
	while (fgets(line, sizeof line, fp))
		if (split(line, f, 16) == 14 && f[0][0] == 'P' && same(rdb18, f[1]))
		{
			put((char *) out, sizeof *out, "");
			put(out->rdb, 18, f[1]);
			put(out->type, 10, f[2]);
			put(&out->sts, 1, f[3]);
			put(out->host, 120, f[4]);
			putnum(out->port, 5, f[5]);
			put(out->dbnm, 60, f[6]);
			put(out->user, 60, f[7]);
			put(&out->pwds, 1, "N");
			put(out->tls, 10, f[8]);
			putnum(out->tmo, 3, f[9]);
			putnum(out->ccsid, 5, f[10]);
			putnum(out->txtl, 5, f[11]);
			putnum(out->nscl, 2, f[12]);
			put(&out->trc, 1, f[13]);
			rc = 0;
		}
	fclose(fp);
	if (rc == 0)
	{
		char		pw[64];

		if (xdbprf_getpwd(lib10, rdb18, pw, sizeof pw, NULL) > 0)
			put(&out->pwds, 1, "Y");
	}
	return rc;
}

int
xdbprf_options(const char *lib10, const char *rdb18, xdbpro_rec *out, int max)
{
	const char *path = getenv("XDB_PRF_EMU");
	FILE	   *fp = path ? fopen(path, "r") : NULL;
	char		line[1024];
	char	   *f[16];
	int			n = 0;

	if (fp == NULL)
		return 0;
	while (n < max && fgets(line, sizeof line, fp))
		if (split(line, f, 5) == 5 && f[0][0] == 'O' && same(rdb18, f[1]))
		{
			put((char *) &out[n], sizeof out[n], "");
			put(out[n].rdb, 18, f[1]);
			put(out[n].key, 20, f[2]);
			put(&out[n].kind, 1, f[3]);
			put(out[n].val, 200, f[4]);
			n++;
		}
	fclose(fp);
	return n;
}

int
xdbprf_getpwd(const char *lib10, const char *rdb18, char *out, int outsize, int *ccsid)
{
	const char *path = getenv("XDB_PRF_EMU");
	FILE	   *fp = path ? fopen(path, "r") : NULL;
	char		line[1024];
	char	   *f[16];
	int			n = -1;

	if (fp == NULL)
		return -1;
	while (fgets(line, sizeof line, fp))
		if (split(line, f, 3) == 3 && f[0][0] == 'W' && same(rdb18, f[1]))
		{
			snprintf(out, outsize, "%s", f[2]);
			n = (int) strlen(out);
			if (ccsid)
				*ccsid = 1208;
		}
	fclose(fp);
	return n;
}

int
xdbprf_setpwd(const char *lib10, const char *rdb18, const char *pwd, int len)
{
	return -1;
}

int
xdbprf_delpwd(const char *lib10, const char *rdb18)
{
	return -1;
}

/*
 * Who the job runs under comes from the environment here: XDB_EMU_USER
 * (default TESTUSER) and XDB_EMU_GROUPS (names separated by blanks).
 */
static int
emu_names(char names[][10], int max)
{
	char		all[400];
	char	   *tok;
	int			n = 0;

	snprintf(all, sizeof all, "%s %s", getenv("XDB_EMU_USER") ? getenv("XDB_EMU_USER") : "TESTUSER",
			 getenv("XDB_EMU_GROUPS") ? getenv("XDB_EMU_GROUPS") : "");
	for (tok = strtok(all, " "); tok && n < max; tok = strtok(NULL, " "))
		put(names[n++], 10, tok);
	return n;
}

int
xdbprf_user_in(const char *list, char *who10)
{
	char		names[20][10];
	char		star[10];
	int			n = emu_names(names, 20);
	char		up[1100];
	const char *p;
	int			any = 0,
				i;
	char		blank[4];

	ile_conv(1208, ile_job_ccsid(), " ", 1, blank, sizeof blank);
	put(star, 10, "*ALL");
	memset(who10, blank[0], 10);
	if (n > 0)
		memcpy(who10, names[0], 10);
	/* the list arrives in the job CCSID: back to UTF-8 to fold the case */
	i = ile_conv(ile_job_ccsid(), 1208, list, strlen(list), up, sizeof up - 1);
	up[i < 0 ? 0 : i] = '\0';
	for (i = 0; up[i]; i++)
		if (up[i] >= 'a' && up[i] <= 'z')
			up[i] = (char) (up[i] - 32);
	p = up;
	while (*p)
	{
		char		w[32];
		char		wj[10];
		int			l = 0;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		while (*p && *p != ' ')
		{
			if (l < 30)
				w[l] = *p;
			l++;
			p++;
		}
		w[l < 30 ? l : 30] = '\0';
		any = 1;
		if (l > 10)
			continue;
		put(wj, 10, w);
		if (memcmp(wj, star, 10) == 0)
			return 1;
		for (i = 0; i < n; i++)
			if (memcmp(names[i], wj, 10) == 0)
				return 1;
	}
	return any ? 0 : 1;
}

int
xdbprf_whoami(char *out, int outsize)
{
	return snprintf(out, outsize, "%s", "emulation");
}

int
xdbprf_names(const char *prof10, char *out, int outsize)
{
	(void) prof10;
	return snprintf(out, outsize, "%s", "emulation");
}

/* the version of the package: environment variable XDB_EMU_VERSION here */
int
xdbprf_version(char *out, int outsize)
{
	const char *e = getenv("XDB_EMU_VERSION");
	int			n;

	out[0] = '\0';
	if (e == NULL || *e == '\0')
		return 0;
	n = ile_conv(1208, ile_job_ccsid(), e, strlen(e), out, outsize - 1);
	if (n < 0)
		n = 0;
	out[n] = '\0';
	return n;
}

/* EXPERIMENT (large objects): the switch comes from XDB_EMU_LOBT here */
#ifdef XDB_LOBTEST
int
xdbprf_testspec(char *out, int outsize)
{
	const char *e = getenv("XDB_EMU_LOBT");
	int			n;

	if (e == NULL)
		return 0;
	n = ile_conv(1208, ile_job_ccsid(), e, strlen(e), out, outsize - 1);
	if (n < 0)
		n = 0;
	out[n] = '\0';
	return n;
}
#endif

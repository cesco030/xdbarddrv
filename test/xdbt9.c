/*
 * XDBT9 - profile access on IBM i: library, password round trip in the
 * validation list, profile record and options.  Uses the profile
 * XDBSELFTEST (xdbt9.sql adds it, the build script removes it again).
 * Result lines go to /tmp/xdbt9.out.
 */
#include <stdio.h>
#include <string.h>
#include "xdbprf.h"

int
main(void)
{
	FILE	   *f = fopen("/tmp/xdbt9.out", "w");
	char		lib[11];
	char		rdb[19];
	char		out[200];
	xdbprf_rec	r;
	xdbpro_rec	o[8];
	int			ccsid = -1;
	int			n,
				i,
				rc;
	const char *pw = "Test-pw_123 x";

	if (f == NULL)
		return 1;
	memset(lib, 0, sizeof lib);
	xdbprf_mylib(lib);
	fprintf(f, "library=[%s]\n", lib);
	memset(rdb, ' ', 18);
	rdb[18] = '\0';
	memcpy(rdb, "XDBSELFTEST", 11);

	rc = xdbprf_setpwd(lib, rdb, pw, (int) strlen(pw));
	fprintf(f, "setpwd rc=%d\n", rc);
	n = xdbprf_getpwd(lib, rdb, out, sizeof out, &ccsid);
	fprintf(f, "getpwd len=%d ccsid=%d same=%d\n", n, ccsid, n > 0 && strcmp(out, pw) == 0);
	rc = xdbprf_setpwd(lib, rdb, "second", 6);
	n = xdbprf_getpwd(lib, rdb, out, sizeof out, &ccsid);
	fprintf(f, "replace rc=%d len=%d same=%d\n", rc, n, n > 0 && strcmp(out, "second") == 0);
	rc = xdbprf_delpwd(lib, rdb);
	fprintf(f, "delpwd rc=%d\n", rc);
	n = xdbprf_getpwd(lib, rdb, out, sizeof out, &ccsid);
	fprintf(f, "getpwd after delete len=%d\n", n);
	rc = xdbprf_delpwd(lib, rdb);
	fprintf(f, "delpwd again rc=%d\n", rc);

	rc = xdbprf_read(lib, rdb, &r);
	fprintf(f, "read rc=%d size=%d\n", rc, (int) sizeof r);
	if (rc == 0)
		fprintf(f, "  type=[%.10s] sts=%c host=[%.20s] port=%.5s db=[%.10s] user=[%.10s] pwds=%c tls=[%.10s]"
				" tmo=%.3s ccsid=%.5s txtl=%.5s nscl=%.2s trc=%c\n",
				r.type, r.sts, r.host, r.port, r.dbnm, r.user, r.pwds, r.tls,
				r.tmo, r.ccsid, r.txtl, r.nscl, r.trc);
	n = xdbprf_options(lib, rdb, o, 8);
	fprintf(f, "options n=%d size=%d\n", n, (int) sizeof o[0]);
	for (i = 0; i < n; i++)
		fprintf(f, "  [%.20s] %c [%.30s]\n", o[i].key, o[i].kind, o[i].val);
	memcpy(rdb, "XDBNOSUCHPROFILE  ", 18);
	fprintf(f, "read missing rc=%d options=%d\n", xdbprf_read(lib, rdb, &r), xdbprf_options(lib, rdb, o, 8));
	fclose(f);
	return 0;
}

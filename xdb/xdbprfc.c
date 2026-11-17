/*
 * xdbprfc.c - XDB connection profiles: record and password access on IBM i.
 *
 * Compiled with the defaults (job CCSID literals, no UTF-8 locale): names
 * given to the system are EBCDIC here.  Bound into the XDB service program
 * (reads) and into the maintenance program XDBPRFM (password store).
 *
 * Passwords are entries of the validation list XDBVLDL, stored with the
 * attribute QsyEncryptData = 1 so that the system can give them back; that
 * needs system value QRETSVRSEC = 1, and *USE plus *AUTLMGT authority to
 * the list (the XDB service program adopts its owner's authority for it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <recio.h>
#include <qsyvldl.h>
#include <qsyrusri.h>
#include <qwcrdtaa.h>
#include <mih/matpgmnm.h>
#include "xdbprf.h"

typedef struct
{
	char		name[10];
	char		lib[10];
} qualname;

typedef struct
{
	Qsy_Attr_Info_T info;
	Qsy_Attr_Descr_T d;
} attr1;

void
xdbprf_mylib(char *lib10)
{
	/* the template of MATPGMNM, format 0; the pointer aligns it */
	union
	{
		void	   *align;
		_Packed struct
		{
			int			size;
			int			used;
			int			format;
			char		reserved[4];
			char		ctx_type[2];
			char		ctx_name[30];
			char		pgm_type[2];
			char		pgm_name[30];
		}			t;
	}			u;

	memset(&u, 0, sizeof u);
	u.t.size = sizeof u.t;
	_MATPGMNM((_MATPGMNM_Template_T *) &u.t);
	memcpy(lib10, u.t.ctx_name, 10);
}

/* "LIB/FILE" for _Ropen */
static void
qualified(char *out, const char *lib10, const char *file)
{
	int			n = 10;

	while (n > 0 && (lib10[n - 1] == ' ' || lib10[n - 1] == '\0'))
		n--;
	memcpy(out, lib10, n);
	out[n] = '/';
	strcpy(out + n + 1, file);
}

int
xdbprf_read(const char *lib10, const char *rdb18, xdbprf_rec *out)
{
	char		name[32];
	_RFILE	   *f;
	_RIOFB_T   *fb;
	int			rc;

	qualified(name, lib10, "XDBPRF");
	f = _Ropen(name, "rr");
	if (f == NULL)
		return -1;
	fb = _Rreadk(f, out, sizeof *out, __KEY_EQ, (void *) rdb18, 18);
	rc = (fb->num_bytes == sizeof *out) ? 0 : 1;
	_Rclose(f);
	return rc;
}

int
xdbprf_options(const char *lib10, const char *rdb18, xdbpro_rec *out, int max)
{
	char		name[32];
	_RFILE	   *f;
	_RIOFB_T   *fb;
	int			n = 0;

	qualified(name, lib10, "XDBPRO");
	f = _Ropen(name, "rr");
	if (f == NULL)
		return 0;
	fb = _Rreadk(f, &out[0], sizeof out[0], __KEY_EQ, (void *) rdb18, 18);
	while (fb->num_bytes == sizeof out[0])
	{
		n++;
		if (n >= max)
			break;
		fb = _Rreadk(f, &out[n], sizeof out[0], __KEY_NEXTEQ, (void *) rdb18, 18);
	}
	_Rclose(f);
	return n;
}

static void
vldl_name(qualname *q, const char *lib10)
{
	memcpy(q->name, "XDBVLDL   ", 10);
	memcpy(q->lib, lib10, 10);
}

/* the entry id is the RDB name, compared byte for byte in any job */
static void
entry_id(Qsy_Entry_ID_Info_T *id, const char *rdb18)
{
	memset(id, 0, sizeof *id);
	id->Entry_ID_Len = 18;
	id->Entry_ID_CCSID = 65535;
	memcpy(id->Entry_ID, rdb18, 18);
}

/* 0 = removed, otherwise errno (ENOREC when there was no entry) */
static int
remove_entry(const char *lib10, const char *rdb18)
{
	qualname	q;
	Qsy_Entry_ID_Info_T id;

	vldl_name(&q, lib10);
	entry_id(&id, rdb18);
	errno = 0;
	if (QsyRemoveValidationLstEntry((void *) &q, &id) != 0)
		return errno ? errno : -1;
	return 0;
}

int
xdbprf_delpwd(const char *lib10, const char *rdb18)
{
	int			rc = remove_entry(lib10, rdb18);

	return rc == ENOREC ? 0 : rc;
}

int
xdbprf_setpwd(const char *lib10, const char *rdb18, const char *pwd, int len)
{
	qualname	q;
	Qsy_Entry_ID_Info_T id;
	Qsy_Entry_Encr_Data_Info_T enc;
	Qsy_Entry_Data_Info_T data;
	attr1		a;
	int			rc;

	if (len <= 0 || len > QSY_MAX_ENCR_LEN)
		return EINVAL;
	/* replace: remove an entry that may be there, then add */
	remove_entry(lib10, rdb18);
	vldl_name(&q, lib10);
	entry_id(&id, rdb18);
	memset(&enc, 0, sizeof enc);
	enc.Encr_Data_Len = len;
	enc.Encr_Data_CCSID = 0;	/* job CCSID: kept with the entry */
	memcpy(enc.Encr_Data, pwd, len);
	memset(&data, 0, sizeof data);
	data.Entry_Data_Len = 3;
	data.Entry_Data_CCSID = 0;
	memcpy(data.Entry_Data, "XDB", 3);
	memset(&a, 0, sizeof a);
	a.info.Number_Attrs = 1;
	a.d.Attr_Location = 0;		/* kept in the validation list */
	a.d.Attr_Type = 0;			/* system defined attribute */
	a.d.Attr_ID = "QsyEncryptData";
	a.d.Attr_Data_Info.Attr_VLDL.Attr_CCSID = -1;
	a.d.Attr_Data_Info.Attr_VLDL.Attr_Len = 1;
	a.d.Attr_Data_Info.Attr_VLDL.Attr_Value = "1";	/* can be given back */
	errno = 0;
	rc = QsyAddValidationLstEntry((void *) &q, &id, &enc, &data, &a);
	memset(&enc, 0, sizeof enc);
	if (rc != 0)
		return errno ? errno : -1;
	return 0;
}

/*
 * QsyFindValidationLstEntryAttrs with the attribute QsyEncryptData gives
 * the entry back with its data-to-encrypt field in the clear (when the
 * entry was stored with the attribute set to 1 and QRETSVRSEC is 1); the
 * attribute buffer itself only receives that 0 or 1.
 */
int
xdbprf_getpwd(const char *lib10, const char *rdb18, char *out, int outsize, int *ccsid)
{
	qualname	q;
	Qsy_Entry_ID_Info_T id;
	Qsy_Rtn_Vld_Lst_Ent_T ent;
	attr1		a;
	struct
	{
		Qsy_Rtn_VLDL_Attr_T h;
		char		data[16];
	}			r;
	int			n;

	vldl_name(&q, lib10);
	entry_id(&id, rdb18);
	memset(&a, 0, sizeof a);
	memset(&r, 0, sizeof r);
	memset(&ent, 0, sizeof ent);
	a.info.Number_Attrs = 1;
	a.d.Attr_Location = 0;
	a.d.Attr_Type = 0;
	a.d.Attr_ID = "QsyEncryptData";
	a.d.Attr_Data_Info.Attr_VLDL.Attr_CCSID = -1;
	a.d.Attr_Data_Info.Attr_VLDL.Attr_Len = sizeof r;
	a.d.Attr_Data_Info.Attr_VLDL.Attr_Value = &r;
	if (QsyFindValidationLstEntryAttrs((void *) &q, &id, &ent, &a.info) != 0)
		return -1;
	n = ent.Encr_Data_Info.Encr_Data_Len;
	if (r.h.Attr_Len != 1 || r.data[0] != '1' || n <= 0 || n >= outsize || n > QSY_MAX_ENCR_LEN)
	{
		memset(&ent, 0, sizeof ent);
		return -1;
	}
	memcpy(out, ent.Encr_Data_Info.Encr_Data, n);
	out[n] = '\0';
	if (ccsid)
		*ccsid = (int) ent.Encr_Data_Info.Encr_Data_CCSID;
	memset(&ent, 0, sizeof ent);
	return n;
}

/* ------------------------------------------------- who may use a profile */

/*
 * A user profile (*CURRENT = the user the job runs under now), then its
 * group profile and its supplemental groups: names of 10, blank padded.
 * Returns how many names were stored (0 when the system does not tell).
 * QSYRUSRI format USRI0200: user at 8, group profile at 43, offset to the
 * supplemental groups at 96, their number at 100.
 */
static int
names_of(const char *prof10, char names[][10], int max)
{
	char		buf[1024];
	struct
	{
		int			provided;
		int			avail;
		char		msgid[7];
		char		rsv;
		char		data[64];
	}			ec;
	int			n = 0,
				off = 0,
				cnt = 0,
				i;

	memset(&ec, 0, sizeof ec);
	ec.provided = sizeof ec;
	memset(buf, 0, sizeof buf);
	QSYRUSRI(buf, sizeof buf, "USRI0200", (char *) prof10, &ec);
	if (ec.avail > 0)
		return 0;
	memcpy(names[n++], buf + 8, 10);
	if (n < max && buf[43] != ' ' && buf[43] != '\0' && memcmp(buf + 43, "*NONE", 5) != 0)
		memcpy(names[n++], buf + 43, 10);
	memcpy(&off, buf + 96, 4);
	memcpy(&cnt, buf + 100, 4);
	for (i = 0; i < cnt && n < max && off >= 104 && off + (i + 1) * 10 <= (int) sizeof buf; i++)
		memcpy(names[n++], buf + off + i * 10, 10);
	return n;
}

int
xdbprf_user_in(const char *list, char *who10)
{
	char		names[20][10];
	int			n = names_of("*CURRENT  ", names, 20);
	const char *p = list;
	int			any = 0;

	memset(who10, ' ', 10);
	if (n > 0)
		memcpy(who10, names[0], 10);
	while (*p)
	{
		char		w[10];
		int			l = 0,
					i;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		memset(w, ' ', 10);
		while (*p && *p != ' ')
		{
			if (l < 10)
				w[l] = (char) toupper((unsigned char) *p);
			l++;
			p++;
		}
		any = 1;
		if (l == 4 && memcmp(w, "*ALL", 4) == 0)
			return 1;
		if (l > 10)
			continue;
		for (i = 0; i < n; i++)
			if (memcmp(names[i], w, 10) == 0)
				return 1;
	}
	/* a list that names nobody the system knows about refuses; no list allows */
	return any ? 0 : 1;
}

int
xdbprf_whoami(char *out, int outsize)
{
	return xdbprf_names(NULL, out, outsize);
}

/* the same for any user profile the job may look at: for the tests only */
int
xdbprf_names(const char *prof10, char *out, int outsize)
{
	char		names[20][10];
	int			n = names_of(prof10 ? prof10 : "*CURRENT  ", names, 20);
	int			i,
				l = 0;

	out[0] = '\0';
	for (i = 0; i < n && l + 12 < outsize; i++)
	{
		int			k = 10;

		while (k > 0 && names[i][k - 1] == ' ')
			k--;
		if (i > 0)
			out[l++] = ' ';
		memcpy(out + l, names[i], k);
		l += k;
		out[l] = '\0';
	}
	return l;
}

/* ------------------------------------------------- version of the package */

int
xdbprf_version(char *out, int outsize)
{
	struct
	{
		int			avail;
		int			ret;
		char		type[10];
		char		lib[10];
		int			len;
		int			dec;
		char		val[120];
	}			r;
	struct
	{
		int			provided;
		int			avail;
		char		msgid[7];
		char		rsv;
		char		data[64];
	}			ec;
	char		name[20];
	int			n;

	out[0] = '\0';
	memcpy(name, "XDBVER    ", 10);
	xdbprf_mylib(name + 10);
	memset(&ec, 0, sizeof ec);
	ec.provided = sizeof ec;
	memset(&r, 0, sizeof r);
	QWCRDTAA(&r, sizeof r, name, -1, 100, &ec);
	if (ec.avail > 0 || r.len <= 0)
		return 0;
	n = r.len > 100 ? 100 : r.len;
	while (n > 0 && r.val[n - 1] == ' ')
		n--;
	if (n > outsize - 1)
		n = outsize - 1;
	memcpy(out, r.val, n);
	out[n] = '\0';
	return n;
}

/* ------------------------------------------- EXPERIMENT: large objects */

#ifdef XDB_LOBTEST
int
xdbprf_testspec(char *out, int outsize)
{
	struct
	{
		int			avail;
		int			ret;
		char		type[10];
		char		lib[10];
		int			len;
		int			dec;
		char		val[420];
	}			r;
	struct
	{
		int			provided;
		int			avail;
		char		msgid[7];
		char		rsv;
		char		data[64];
	}			ec;
	int			n;

	memset(&ec, 0, sizeof ec);
	ec.provided = sizeof ec;
	memset(&r, 0, sizeof r);
	QWCRDTAA(&r, sizeof r, "XDBLOBT   QTEMP     ", -1, 400, &ec);
	if (ec.avail > 0 || r.len <= 0)
	{
		/*
		 * No data area: environment variable XDB_LOBT of the job.  The jobs
		 * that a QSH script starts inherit it, so a compile can be tested.
		 */
		const char *e = getenv("XDB_LOBT");

		if (e == NULL || *e == '\0')
			return 0;
		n = (int) strlen(e);
		if (n > outsize - 1)
			n = outsize - 1;
		if (n > 400)
			n = 400;
		memcpy(out, e, n);
		out[n] = '\0';
		return n;
	}
	n = r.len < outsize - 1 ? r.len : outsize - 1;
	if (n > 400)
		n = 400;
	memcpy(out, r.val, n);
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
		n--;
	out[n] = '\0';
	return n;
}
#endif

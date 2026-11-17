/*
 * XDBPRFT - connection test of an XDB profile, called by XDBPRFM (option 9).
 *
 *   parameters: RDB name char(18), result '1'/'0' char(1), four lines char(62)
 *
 * Connects to the RDB through the SQL call level interface, reports what
 * happened and disconnects.  (An embedded SQL program can only CONNECT to a
 * remote database when it was precompiled for one, SQL0862; the call level
 * interface has no such tie.)  Runs in its own activation group, so the
 * connection ends with the call whatever happens.
 *
 * CRTCMOD ... TGTCCSID(*JOB), CRTPGM ... BNDSRVPGM((QSYS/QSQCLI)) ACTGRP(*NEW)
 */
#include <stdio.h>
#include <string.h>
#include <sqlcli.h>

#define LINE 62

static void
put(char *line, const char *text)
{
	size_t		n = strlen(text);

	memset(line, ' ', LINE);
	memcpy(line, text, n > LINE ? LINE : n);
}

int
main(int argc, char *argv[])
{
	char		rdb[19];
	char		text[700];
	char		name[64];
	char		ver[64];
	SQLCHAR		state[8];
	SQLINTEGER	native = 0;
	SQLINTEGER	nocommit = SQL_TXN_NO_COMMIT;
	SQLSMALLINT len = 0;
	SQLHENV		env = 0;
	SQLHDBC		dbc = 0;
	SQLRETURN	rc;
	int			i;

	if (argc < 7)
		return 1;
	memcpy(rdb, argv[1], 18);
	rdb[18] = '\0';
	for (i = 17; i >= 0 && rdb[i] == ' '; i--)
		rdb[i] = '\0';
	argv[2][0] = '0';
	for (i = 3; i <= 6; i++)
		put(argv[i], "");

	rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
	if (rc == SQL_SUCCESS)
		rc = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
	if (rc != SQL_SUCCESS)
	{
		put(argv[3], "The SQL call level interface could not be started.");
		return 0;
	}
	SQLSetConnectAttr(dbc, SQL_ATTR_COMMIT, &nocommit, 0);

	rc = SQLConnect(dbc, (SQLCHAR *) rdb, SQL_NTS, NULL, SQL_NTS, NULL, SQL_NTS);
	if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
	{
		argv[2][0] = '1';
		put(argv[3], "Connected.");
		name[0] = ver[0] = '\0';
		SQLGetInfo(dbc, SQL_DBMS_NAME, name, sizeof name, &len);
		SQLGetInfo(dbc, SQL_DBMS_VER, ver, sizeof ver, &len);
		/* Db2 passes the version on as vvrrm */
		if (strlen(ver) == 5 && strspn(ver, "0123456789") == 5)
			sprintf(text, "Server version %.2s.%.2s.%.1s", ver, ver + 2, ver + 4);
		else
			sprintf(text, "Server: %.25s %.25s", name, ver);
		put(argv[4], text);
		SQLDisconnect(dbc);
	}
	else
	{
		char		msg[600];

		state[0] = '\0';
		msg[0] = '\0';
		SQLGetDiagRec(SQL_HANDLE_DBC, dbc, 1, state, &native, (SQLCHAR *) msg, sizeof msg, &len);
		put(argv[3], "The connection failed.");
		sprintf(text, "SQLCODE %d   SQLSTATE %.5s", (int) native, (char *) state);
		put(argv[4], text);
		if (native == -950)
		{
			put(argv[5], "There is no RDB directory entry with this name.");
			put(argv[6], "Option 8 on the list adds it.");
		}
		else
		{
			/* the message tokens carry the text of the database server */
			text[0] = '\0';
			len = 0;
			SQLGetDiagField(SQL_HANDLE_DBC, dbc, 1, SQL_DIAG_MESSAGE_TOKENS, text, sizeof text - 1, &len);
			if (len > 0 && len < (SQLSMALLINT) sizeof text)
				text[len] = '\0';
			if (text[0] == '\0' || text[0] == ' ')
				strcpy(text, msg);
			for (i = 0; text[i]; i++)
				if ((unsigned char) text[i] < 0x40 || (unsigned char) text[i] == 0xFF)
					text[i] = ' ';
			put(argv[5], text);
			if (strlen(text) > LINE)
				put(argv[6], text + LINE);
		}
	}
	SQLFreeHandle(SQL_HANDLE_DBC, dbc);
	SQLFreeHandle(SQL_HANDLE_ENV, env);
	return 0;
}

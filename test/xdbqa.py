#!/usr/bin/env python3
# xdbqa.py - what the quality tests of the PostgreSQL and MySQL backends share
# (pgtypes.py, pgparams.py, pgrows.py, mytypes.py, myparams.py, myrows.py,
# sqltext.py, dbfuzz.py).  Not a test by itself.
#
# The server a test talks to comes from the environment, so that the same
# test runs against any version:
#   PostgreSQL       XDB_PG_PORT (5433)  XDB_PG_DB (postgres)  XDB_PG_USER (postgres)
#   MySQL / MariaDB  XDB_MY_PORT (3306)  XDB_MY_DB (xdbtest)   XDB_MY_USER (xdbn)
#   SQL Server       XDB_MS_PORT (14340) XDB_MS_DB (xdbtest)   XDB_MS_USER (xdbtest)
#                    (sqltext.py only; the other tests of SQL Server are ms*.py)
#   all on 127.0.0.1 (XDB_PG_HOST / XDB_MY_HOST / XDB_MS_HOST), password xdbpw, without TLS
#   (XDB_PG_TLS / XDB_MY_TLS / XDB_MS_TLS = *PREFER or *REQUIRE for a connection with it: a
#   MySQL login with caching_sha2_password, the only kind MySQL 9 has, needs it the first time).
# The driver is driven through ardsh of the Linux emulation; what it is
# compared with is written by the test itself or read by another client
# (psycopg2, PyMySQL, python-tds): pip install psycopg2-binary pymysql python-tds.
import os
import subprocess
import struct
from decimal import Decimal, getcontext

getcontext().prec = 100     # the numbers compared have up to 65 digits: none may be rounded on the way


class Target:
    def __init__(self, kind, rdb='QA', workdir=None):
        self.kind = kind                    # 'pg', 'mysql' or 'mssql'
        self.rdb = rdb
        p, port, db, user = {'pg': ('XDB_PG_', '5433', 'postgres', 'postgres'), 'mysql': ('XDB_MY_', '3306', 'xdbtest', 'xdbn'),
                             'mssql': ('XDB_MS_', '14340', 'xdbtest', 'xdbtest')}[kind]
        self.host = os.environ.get(p + 'HOST', '127.0.0.1')
        self.port = int(os.environ.get(p + 'PORT', port))
        self.db = os.environ.get(p + 'DB', db)
        self.user = os.environ.get(p + 'USER', user)
        self.tls = os.environ.get(p + 'TLS', '*OFF')        # *OFF, *PREFER, *REQUIRE: the TLS column of the profile
        self.pw = 'xdbpw'
        self.dir = workdir or '/tmp/xdb_qa_%s_%d_%d' % (kind, self.port, os.getpid())
        os.makedirs(self.dir, exist_ok=True)
        self._client = None
        self.profile()

    def profile(self, options=None, ccsid=0, text_len=1000, port=None, scale=6, driver=None):
        """the emulated connection profile: options is {keyword: value} for the connection string of the
        database client, driver the same for the options the driver itself reads (fetch_rows, init_sql, ...)"""
        with open(self.dir + '/profile.emu', 'w') as f:
            f.write('P|%s|%s|A|%s|%d|%s|%s|%s|10|%d|%d|%d|N\nW|%s|%s\n' % (
                self.rdb, {'pg': 'PG', 'mysql': 'MYSQL', 'mssql': 'MSSQL'}[self.kind], self.host, port or self.port, self.db, self.user,
                self.tls, ccsid, text_len, scale, self.rdb, self.pw))
            for k, v in (options or {}).items():
                f.write('O|%s|%s|C|%s\n' % (self.rdb, k, v))
            for k, v in (driver or {}).items():
                f.write('O|%s|%s|D|%s\n' % (self.rdb, k, v))

    def run(self, emu, lines, maxrows=5, ccsid='1144', timeout=1800, env=None):
        """the lines through ardsh; gives its whole output"""
        e = dict(os.environ, XDB_SH_FLOAT17='1', XDB_EMU_JOBCCSID=ccsid, XDB_PRF_EMU=self.dir + '/profile.emu',
                 XDB_CONF='/dev/null', XDB_SH_MAXROWS=str(maxrows), XDB_TEST_RDB=self.rdb)
        e.update(env or {})
        p = subprocess.run([emu + '/ardsh'], input=('\n'.join(lines) + '\n').encode('utf-8'), env=e,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return p.stdout.decode('utf-8', 'replace')

    def client(self):
        """a connection of the other client, every statement committed by itself"""
        if self._client is None:
            if self.kind == 'pg':
                import psycopg2
                self._client = psycopg2.connect(host=self.host, port=self.port, dbname=self.db, user=self.user, password=self.pw)
                self._client.autocommit = True
            elif self.kind == 'mssql':
                import pytds
                self._client = pytds.connect(self.host, self.db, self.user, self.pw, port=self.port, autocommit=True)
            else:
                import pymysql
                self._client = pymysql.connect(host=self.host, port=self.port, database=self.db, user=self.user, password=self.pw,
                                               autocommit=True, charset='utf8mb4')
        return self._client

    def sql(self, statement, args=None):
        """one statement by the other client; gives the rows, or raises its error"""
        cur = self.client().cursor()
        cur.execute(statement, args)
        try:
            return cur.fetchall()
        except Exception:
            return []

    def version(self):
        """(numbers of the version, its text)"""
        if self.kind == 'mssql':
            r = self.sql("select cast(serverproperty('ProductMajorVersion') as int), cast(serverproperty('ProductMinorVersion') as int), @@version")[0]
            return (r[0], r[1]), r[2]
        t = self.sql('select version()')[0][0]
        if self.kind == 'pg':
            n = int(self.sql("select current_setting('server_version_num')")[0][0])
            return (n // 10000, n // 100 % 100) if n < 100000 else (n // 10000, n % 10000), t
        v = t.split('-')[0].split('.')
        return (int(v[0]), int(v[1]), int(v[2]) if len(v) > 2 and v[2].isdigit() else 0), t


def blocks(out):
    """the requests of an ardsh output: [{cmd, cols [(name, type, len, ccsid)], rows [text], err, code, affected}]"""
    res = []
    for line in out.split('\n'):
        if line.startswith('> '):
            res.append({'cmd': line[2:], 'cols': [], 'rows': [], 'err': None, 'code': 0, 'affected': None, 'state': ''})
        elif res:
            b = res[-1]
            if line.startswith('  col '):
                p = line.split()
                # "  col <name> type <t> len <l> ccsid <c>": a name can hold blanks
                i = p.index('type', 2) if 'type' in p[2:] else 2
                b['cols'].append((' '.join(p[1:i]), int(p[i + 1]), int(p[i + 3]), int(p[i + 5])))
            elif line.startswith('  row: '):
                b['rows'].append(line[7:])
            elif line.startswith('  rows='):
                b['affected'] = int(line[7:])
            elif line.startswith('AR') and ' -> sqlcode ' in line:
                p = line.split()
                code = int(p[3])
                if code < 0 and b['err'] is None:
                    b['err'] = line[line.index('sqlcode'):][:260]
                    b['code'] = code
                    b['state'] = p[5]
    return res


def split(row):
    """the values of a row line: numbers, x'..' and <null> have no blanks; [text] can"""
    out = []
    p = 0
    while p < len(row):
        if row[p] == ' ':
            p += 1
        elif row[p] == '[':
            e = row.find('] ', p)
            e = len(row) - 1 if e < 0 else e
            out.append(row[p:e + 1])
            p = e + 1
        else:
            e = row.find(' ', p)
            e = len(row) if e < 0 else e
            out.append(row[p:e])
            p = e
    return out


def text_of(r):
    return r[1:-1] if r is not None and r.startswith('[') and r.endswith(']') else r


def packed(r, scale):
    """x'0000123F' of a DECIMAL column -> Decimal"""
    h = r[2:-1]
    d = Decimal(int(h[:-1] or '0')).scaleb(-scale)
    return -d if h[-1] in 'DB' else d


NAMES = {449: 'VARCHAR', 453: 'CHAR', 485: 'DECIMAL', 497: 'INTEGER', 501: 'SMALLINT', 493: 'BIGINT', 481: 'DOUBLE',
         385: 'DATE', 389: 'TIME', 393: 'TIMESTAMP'}


def coltype(col):
    """(name, type, len, ccsid) -> 'DECIMAL(18,4)'"""
    _, t, l, _ = col
    if t == 485:
        return 'DECIMAL(%d,%d)' % (l >> 8, l & 255)
    if t in (449, 453):
        return '%s(%d)' % (NAMES[t], l)
    return NAMES.get(t, str(t))


def same(kind, col, got, want):
    """does the value the driver handed over (as ardsh prints it) say what `want` says?
    kind: int (want int), dec (Decimal: compared at the scale of the column), float (float), real (float, as 4 bytes),
    reals (the same, and handed over as the shortest number that gives those 4 bytes back),
    date ('YYYY-MM-DD'), time ('HH:MM:SS'), ts ('YYYY-MM-DD HH:MM:SS[.ffffff]'), text (str), null, bad (a value Db2
    cannot hold: it must arrive as "could not be converted"), want None = NULL"""
    if want is None:
        return got == '<null>'
    if kind == 'bad':
        return got == '<conv-error>'
    if got in ('<null>', '<conv-error>'):
        return False
    t = col[1]
    try:
        if kind == 'int':
            return t in (497, 501, 493) and int(got) == want
        if kind == 'dec':
            if t != 485:
                return False
            sc = col[2] & 255
            return packed(got, sc) == Decimal(want)
        if kind == 'float':
            return t == 481 and float(got) == want
        if kind == 'real':
            return t == 481 and struct.pack('f', float(got)) == struct.pack('f', want)
        if kind == 'reals':
            # a single-precision value as the shortest number that gives it back: 0.1, not 0.100000001
            f4 = struct.unpack('f', struct.pack('f', want))[0]
            return t == 481 and float(got) == float(float_text(f4, single=True))
        g = text_of(got)
        if kind == 'date':
            return t == 385 and '%s-%s-%s' % (g[6:10], g[3:5], g[0:2]) == want
        if kind == 'time':
            return t == 389 and g.replace('.', ':') == want
        if kind == 'ts':
            w = want if '.' in want else want + '.'
            w = (w + '000000')[:26]
            return t == 393 and g[:10] + ' ' + g[11:].replace('.', ':', 2) == w
        if kind == 'text':
            return g.rstrip(' ') == want.rstrip(' ') if t == 453 else g == want
    except (ValueError, IndexError):
        return False
    return False


def float_text(v, single=False):
    """a floating point host variable written out: the shortest text that gives the same number back,
    15 to 17 digits for a DOUBLE, 6 to 9 for a REAL (single = True; v is then the value the 4 bytes hold)"""
    if single:
        for d in range(6, 10):
            t = '%.*g' % (d, v)
            if struct.pack('f', float(t)) == struct.pack('f', v):
                return t
        return '%.9g' % v
    for d in (15, 16, 17):
        t = '%.*g' % (d, v)
        if float(t) == v:
            return t
    return '%.17g' % v

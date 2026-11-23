#!/usr/bin/env python3
# msrows.py - many rows of every shape from a SQL Server, each checked
# against what it must be.
#
# usage: python3 msrows.py <emu dir> [rows (5000)] [-v]
#   env XDB_MS_PORT (14340), XDB_MS_DB (xdbtest), XDB_MS_USER (xdbtest); the
#   password of the test login is xdbpw.
#
# One query makes the rows on the server from their number alone: integers,
# decimals, floating point, dates and times, text of every length from
# nothing to its limit (in the code page of the server and in Unicode),
# bytes, long values, and NULLs in a pattern.  The values are of different
# lengths in every row, so that over some thousand rows every part of a
# row falls on the border between two packets of the server at some point,
# and between two blocks of rows the driver hands to Db2.
#
# What every value must be is computed here from the number of the row,
# not asked of the server.  The same rows are read
#   - through a cursor of the server (a plain SELECT) and directly (the
#     same SELECT inside sp_executesql, which is not opened as a cursor),
#   - with 1, 200 and 3000 rows asked of the server at a time,
#   - at the protocol level of today and at the one of SQL Server 2000.
import os
import subprocess
import sys
from decimal import Decimal, getcontext

getcontext().prec = 100     # no number that is compared may be rounded on the way
from datetime import datetime, timedelta

EMU = sys.argv[1]
args = [a for a in sys.argv[2:] if not a.startswith('-')]
N = int(args[0]) if args else 5000
VERBOSE = '-v' in sys.argv[2:]
PORT = int(os.environ.get('XDB_MS_PORT', '14340'))
DB = os.environ.get('XDB_MS_DB', 'xdbtest')
USER = os.environ.get('XDB_MS_USER', 'xdbtest')
D = '/tmp/xdb_msrows'
os.makedirs(D, exist_ok=True)


def select(old):
    # the old protocol has no datetime2 and no (max) types: the query leaves them to the new one
    d2 = "cast('2021-03-04 05:06:07' as datetime)" if old else "dateadd(microsecond, i * 37 % 1000000, cast('2021-03-04 05:06:07' as datetime2(6)))"
    lng = "cast(case when i % 50 = 0 then replicate('L', 7000) else 'short' end as varchar(8000))" if old else \
        "case when i % 50 = 0 then replicate(cast('L' as varchar(max)), 20000 + i) else cast('short' as varchar(max)) end"
    return ("select i, cast(i * 7919 % 100003 - 50000 as int) as c_int, cast(i as bigint) * 1000000007 as c_big, "
            "cast(i as decimal(18,4)) / 7 as c_dec, cast(i as float) / 3 as c_flt, "
            "dateadd(ms, i * 1237 % 86400000, dateadd(day, i % 3000, cast('2020-01-01' as datetime))) as c_dt, "
            + d2 + " as c_d2, "
            "replicate(char(65 + i % 26), i % 201) as c_v, replicate(nchar(224 + i % 30), i % 101) as c_nv, "
            "case when i % 3 = 0 then null else i end as c_n1, case when i % 5 = 0 then null else cast(i as varchar(10)) end as c_n2, "
            "case when i % 7 = 0 then null else cast(i as decimal(9,2)) end as c_n3, "
            "cast(replicate(char(1 + i % 255), i % 41) as varbinary(40)) as c_bin, "
            + lng + " as c_long, cast(i % 2 as bit) as c_bit, cast(i % 256 as tinyint) as c_tiny, "
            "cast(case when i % 11 = 0 then null else replicate('n', i % 17) end as nvarchar(20)) as c_n4 "
            "from (select top " + str(N) + " row_number() over (order by (select null)) as i from sys.all_columns a cross join sys.all_columns b) n "
            "order by i")


def expect(i, old):
    """the values of row i as the harness prints them"""
    dec = (Decimal(i) / Decimal(7)).quantize(Decimal('0.000001'), rounding='ROUND_DOWN')    # decimal(18,4) / int -> scale 6 on the server
    dt = datetime(2020, 1, 1) + timedelta(days=i % 3000, milliseconds=i * 1237 % 86400000)
    # a datetime keeps 1/300 s: the server rounds the milliseconds to .000, .003, .007
    ms = dt.microsecond // 1000
    ticks = (ms * 3 + 5) // 10                  # 1/300 s, rounded
    dt = dt.replace(microsecond=0)
    frac = [0, 3, 7][ticks % 3] + (ticks // 3) * 10     # 1000 when the value rounds up to the next second
    d2 = datetime(2021, 3, 4, 5, 6, 7) + (timedelta(0) if old else timedelta(microseconds=i * 37 % 1000000))
    return {
        'i': i,
        'c_int': i * 7919 % 100003 - 50000,
        'c_big': i * 1000000007,
        'c_dec': dec,
        'c_flt': i / 3,
        'c_dt': (dt, frac),
        'c_d2': d2,
        'c_v': chr(65 + i % 26) * (i % 201),
        'c_nv': chr(224 + i % 30) * (i % 101),
        'c_n1': None if i % 3 == 0 else i,
        'c_n2': None if i % 5 == 0 else str(i),
        'c_n3': None if i % 7 == 0 else Decimal(i),
        'c_bin': ('%02X' % (1 + i % 255)) * (i % 41),
        # a long value is cut at the text length of the profile, 32000 here
        'c_long': ('L' * (7000 if old else min(20000 + i, 32000))) if i % 50 == 0 else 'short',
        'c_bit': i % 2,
        'c_tiny': i % 256,
        'c_n4': None if i % 11 == 0 else 'n' * (i % 17),
    }


NAMES = ['i', 'c_int', 'c_big', 'c_dec', 'c_flt', 'c_dt', 'c_d2', 'c_v', 'c_nv', 'c_n1', 'c_n2', 'c_n3', 'c_bin', 'c_long', 'c_bit', 'c_tiny', 'c_n4']


def split(row):
    """the values of a "  row: ..." line: numbers, x'..', [text] (text may hold blanks), <null>"""
    out = []
    p = 0
    while p < len(row):
        if row[p] == ' ':
            p += 1
        elif row[p] == '[':
            e = row.index(']', p)       # the test has no ']' in its values
            out.append(row[p:e + 1])
            p = e + 1
        else:
            e = row.find(' ', p)
            e = len(row) if e < 0 else e
            out.append(row[p:e])
            p = e
    return out


def packed(r, scale):
    h = r[2:-1]
    d = Decimal(int(h[:-1] or '0')).scaleb(-scale)
    return -d if h[-1] in 'DB' else d


def check(vals, exp, scales):
    for k, name in enumerate(NAMES):
        g, w = vals[k], exp[name]
        if w is None:
            ok = g == '<null>'
        elif g == '<null>' or g == '<conv-error>':
            ok = False
        elif name in ('i', 'c_int', 'c_big', 'c_n1', 'c_bit', 'c_tiny'):
            ok = int(g) == w
        elif name in ('c_dec', 'c_n3'):
            ok = packed(g, scales[name]) == w
        elif name == 'c_flt':
            ok = float(g) == w
        elif name == 'c_dt':
            dt, frac = w
            ok = g == '[%s.%03d000]' % (dt.strftime('%Y-%m-%d-%H.%M.%S'), frac % 1000) if frac < 1000 else None
            if ok is None:      # the milliseconds rounded up to the next second
                dt2 = dt + timedelta(seconds=1)
                ok = g == '[%s.000000]' % dt2.strftime('%Y-%m-%d-%H.%M.%S')
        elif name == 'c_d2':
            ok = g == '[%s.%06d]' % (w.strftime('%Y-%m-%d-%H.%M.%S'), w.microsecond)
        elif name == 'c_long' and len(w) > 1000:
            ok = g == '[%s... %d bytes]' % (w[:60], len(w))
        else:
            ok = g == '[%s]' % w
        if not ok:
            return '%s: got %s, it is %s' % (name, g[:70], str(w)[:70])
    return None


def run(label, old, fetch_rows, direct):
    with open(D + '/profile.emu', 'w') as f:
        f.write('P|MSRW|MSSQL|A|127.0.0.1|%d|%s|%s|*OFF|10|0|32000|6|N\nW|MSRW|xdbpw\n' % (PORT, DB, USER))
        if old:
            f.write('O|MSRW|tds|C|7.1\n')
        if fetch_rows:
            f.write('O|MSRW|fetch_rows|D|%d\n' % fetch_rows)
    sql = select(old)
    if direct:
        sql = "exec sp_executesql N'%s'" % sql.replace("'", "''")
    env = dict(os.environ, XDB_SH_FLOAT17='1', XDB_EMU_JOBCCSID='1144', XDB_PRF_EMU=D + '/profile.emu', XDB_CONF='/dev/null',
               XDB_SH_MAXROWS='1000000', XDB_TEST_RDB='MSRW')
    out = subprocess.run([EMU + '/ardsh'], input=('C\nQ %s\nD\n' % sql).encode(), env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=1800).stdout.decode('utf-8', 'replace')
    rows = []
    scales = {}
    calls = '?'
    err = None
    for line in out.split('\n'):
        if line.startswith('  row: '):
            rows.append(line[7:])
        elif line.startswith('  col '):
            p = line.split()
            if p[3] == '485':
                scales[p[1].lower()] = int(p[5]) & 255
        elif 'total rows=' in line:
            calls = line.strip()
        elif line.startswith('AR') and 'sqlcode -' in line and err is None:
            err = line[:200]
        elif 'runtime error' in line or 'AddressSanitizer' in line:
            err = err or line[:200]
    bad = 0
    first = None
    if err:
        bad += 1
        first = err
    if len(rows) != N:
        bad += 1
        first = first or '%d rows arrived, %d were made' % (len(rows), N)
    for n, row in enumerate(rows[:N]):
        vals = split(row)
        what = 'the row has %d values' % len(vals) if len(vals) != len(NAMES) else check(vals, expect(n + 1, old), scales)
        if what:
            bad += 1
            first = first or 'row %d, %s' % (n + 1, what)
            if VERBOSE:
                print('   row %d, %s' % (n + 1, what))
    print('%-58s %s%s' % (label, 'ok, %d rows (%s)' % (len(rows), calls) if not bad else 'DIFF in %d places; first: ' % bad, '' if not bad else first))
    return bad


total = 0
for old in (False, True):
    for direct in (False, True):
        for fr in (0, 1, 3000):
            if fr == 1 and N > 2000:
                continue    # one exchange for every row: only with a short run
            total += run('TDS %s, %s, %s rows at a time' % ('7.1' if old else '7.4', 'directly' if direct else 'through a cursor', fr or 'default (200)'),
                         old, fr, direct)
sys.exit(1 if total else 0)

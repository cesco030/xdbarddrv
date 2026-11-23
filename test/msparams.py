#!/usr/bin/env python3
# msparams.py - what a host variable becomes on a SQL Server, checked against
# the server itself.
#
# usage: python3 msparams.py <emu dir> [7.1] [-v]
#   env XDB_MS_PORT (14340), XDB_MS_DB (xdbtest), XDB_MS_USER (xdbtest); the
#   password of the test login is xdbpw.  Needs python-tds (pip install
#   python-tds): the rows the driver is compared with are written and read
#   by that client, not by the driver.
#
# For every type of column (TARGETS) there is a table xp_<n> (id, src, c).
# Every host variable (PARAMS) is inserted into each of them twice: by the
# driver as a parameter (src 'P'), and by the other client as a literal of
# the type the driver is meant to send (src 'L').  The two rows must be the
# same, byte for byte, or both inserts must have failed.  A column of type
# sql_variant keeps the type of what it was given: there the type, the
# precision, the scale and the length are compared as well.
#
# The date, time and timestamp variables are run twice, without and with
# option datetime2 of the profile.
import os
import subprocess
import struct
import sys

import pytds

EMU = sys.argv[1]
OLD = '7.1' in sys.argv[2:]
VERBOSE = '-v' in sys.argv[2:]
PORT = int(os.environ.get('XDB_MS_PORT', '14340'))
DB = os.environ.get('XDB_MS_DB', 'xdbtest')
USER = os.environ.get('XDB_MS_USER', 'xdbtest')
D = '/tmp/xdb_msparams'
os.makedirs(D, exist_ok=True)

TARGETS = ['tinyint', 'smallint', 'int', 'bigint', 'bit', 'decimal(18,4)', 'decimal(38,10)', 'numeric(31,0)', 'money', 'smallmoney',
           'float', 'real', 'date', 'time(0)', 'time(7)', 'datetime', 'smalldatetime', 'datetime2(6)', 'datetime2(7)',
           'datetimeoffset(7)', 'char(10)', 'varchar(50)', 'nchar(10)', 'nvarchar(50)', 'varchar(max)', 'nvarchar(max)', 'text',
           'ntext', 'uniqueidentifier', 'xml', 'varbinary(50)', 'sql_variant']
if OLD:
    # SQL Server 2000 has none of these
    TARGETS = [t for t in TARGETS if t not in ('date', 'time(0)', 'time(7)', 'datetime2(6)', 'datetime2(7)', 'datetimeoffset(7)',
                                               'varchar(max)', 'nvarchar(max)', 'xml')]


def q(s):
    return "N'" + s.replace("'", "''") + "'"


def fl(x):
    r = repr(float(x))
    return r if 'e' in r else r + 'e0'


def real_text(x):
    """the text the driver writes for a REAL: 6 to 9 digits, the fewest that give the same 4 bytes back"""
    f = struct.unpack('f', struct.pack('f', float(x)))[0]
    for d in range(6, 10):
        t = '%.*g' % (d, f)
        if struct.pack('f', float(t)) == struct.pack('f', f):
            return t
    return '%.9g' % f


P = []      # (label, host variable for ardsh, literal without option datetime2, literal with it or None, group)


def add(label, spec, lit, lit2=None, group='n'):
    P.append((label, spec, lit, lit2, group))


for v in (0, 1, -1, 2147483647, -2147483648):
    add('INTEGER %d' % v, 'i:%d' % v, 'cast(%d as int)' % v)
for v in (32767, -32768):
    add('SMALLINT %d' % v, '@s:%d' % v, 'cast(%d as int)' % v)
for v in (255, 9223372036854775807, -9223372036854775808):
    add('BIGINT %d' % v, '@b:%d' % v, 'cast(%d as bigint)' % v)
for v in ('0', '1.5', '-1.5', '0.1', '1.7976931348623157e308', '2.2250738585072014e-308', '123456789012345680', '1e-5'):
    add('DOUBLE %s' % v, '@f:%s' % v, 'cast(%s as float)' % fl(v))
for v in ('1.5', '0.1', '3.4028235e38'):
    # a REAL goes as the shortest number that gives the same REAL back (0.1, not 0.100000001490116)
    add('REAL %s' % v, '@r:%s' % v, 'cast(%s as float)' % fl(real_text(v)))
for spec, v, s in (('p15,4', '12.5', 4), ('p15,4', '-99999999999.9999', 4), ('p15,4', '0', 4), ('p5,2', '-0.01', 2),
                   ('p31,0', '9' * 31, 0), ('p31,31', '0.' + '1234567890' * 3 + '1', 31), ('p31,5', '-' + '9' * 26 + '.99999', 5),
                   ('z7,2', '12345.67', 2), ('z7,2', '-0.5', 2), ('p40,2', '1.5', 2), ('p40,0', '9' * 39, 0), ('p9,0', '3', 0)):
    add('%s(%s) %s' % ('DECIMAL' if spec[0] == 'p' else 'NUMERIC', spec[1:], v[:24]), '@%s:%s' % (spec, v), 'cast(%s as decimal(38,%d))' % (v, s))
# the blanks at the end of a CHAR variable are not sent (option trim_char, on unless the profile says off)
add('CHAR(10) abc', '@c10:abc', "cast(N'abc' as varchar(8000))")
add('CHAR(5) with accents', '@c5:Città', "cast(N'Città' as varchar(8000))")
add('CHAR(3) blank', '@c3: ', "cast(N'' as varchar(8000))")
for lab, v in (('abc', 'abc'), ('empty', ''), ('trailing blanks', 'ab  '), ('accents and the euro', 'Città è già 5 €'), ('a quote', "O'Brien"),
               ('a number', '42'), ('a decimal number', '12.50'), ('a date', '2026-01-31'), ('a timestamp', '2026-01-31 13:45:10.123'),
               ('a time', '13:45:10'), ('a uuid', '6F9619FF-8B86-D011-B42D-00C04FC964FF'), ('xml', '<a b="1">t</a>'),
               ('8000 characters', 'x' * 8000), ('8001 characters', 'y' * 8001), ('12000 characters', 'z' * 12000)):
    add('VARCHAR %s' % lab, '@v:%s' % v, 'cast(%s as %s)' % (q(v), 'text' if len(v) > 8000 else 'varchar(8000)'))
for lab, v in (('abc', 'abc'), ('Cyrillic, euro, kanji', 'Я€日'), ('outside the basic plane', '\U0001f600!'),
               ('4000 characters', 'я' * 4000), ('2001 characters', 'я' * 2001), ('accents only', 'Città')):
    # every character in the code page of the server: varchar; otherwise nvarchar, ntext over 4000 bytes of UTF-8
    wide = any(ord(ch) > 255 for ch in v)
    typ = ('ntext' if len(v.encode('utf-8')) > 4000 else 'nvarchar(4000)') if wide else 'varchar(8000)'
    add('VARGRAPHIC %s' % lab, '@g:%s' % v, 'cast(%s as %s)' % (q(v), typ))
for v in ('2026-01-31', '0001-01-01', '9999-12-31', '1753-01-01', '1752-12-31', '2000-02-29', '1900-01-01'):
    add('DATE %s' % v, '@D:%s' % v, "cast('%s' as %s)" % (v, 'datetime' if OLD else 'date'), None, 'd')
for v in ('00:00:00', '23:59:59', '13:45:10', '24:00:00'):
    add('TIME %s' % v, '@T:%s' % v, "cast('%s' as varchar(8000))" % v, "cast('%s' as time(7))" % v, 'd')
for v, dt in (('2026-01-31-13.45.10.123456', 1), ('2026-01-31-13.45.10.000000', 1), ('0001-01-01-00.00.00.000000', 0),
              ('9999-12-31-23.59.59.999999', 1), ('9999-12-31-23.59.59.997000', 1), ('1753-01-01-00.00.00.000000', 1),
              ('1752-12-31-23.59.59.999999', 1), ('2026-12-31-23.59.59.999999', 1), ('2026-01-31-13.45.10.001500', 1),
              ('2026-01-31-13.45.10.001700', 1), ('2026-01-31-13.45.10.005000', 1), ('1899-12-31-23.59.59.990000', 1)):
    iso = v[:10] + ' ' + v[11:].replace('.', ':', 2)
    two = "cast('%s' as datetime2(7))" % iso
    # a datetime where a datetime can hold the value; the rest goes as datetime2 (not on the old protocol, which has none)
    add('TIMESTAMP %s' % v, '@Z:%s' % v, 'cast(%s as datetime)' % two if (dt or OLD) else two, two, 'd')
add('NULL VARCHAR', 'n:', 'cast(null as varchar(8000))')
for lab, spec, typ, typ2 in (('SMALLINT', '@s', 'int', None), ('BIGINT', '@b', 'bigint', None), ('DOUBLE', '@f', 'float', None),
                             ('DECIMAL', '@p15,4', 'decimal(38,0)', None), ('CHAR', '@c10', 'varchar(8000)', None),
                             ('VARGRAPHIC', '@g', 'varchar(8000)', None), ('DATE', '@D', 'datetime' if OLD else 'date', None),
                             ('TIME', '@T', 'varchar(8000)', 'time(7)'), ('TIMESTAMP', '@Z', 'datetime', 'datetime2(7)')):
    add('NULL %s' % lab, '%s:@null' % spec, 'cast(null as %s)' % typ, 'cast(null as %s)' % typ2 if typ2 else None,
        'd' if typ2 or lab == 'DATE' else 'n')

ID2 = 1000      # the ids of the second run


def runs():
    """(first id, option datetime2, the parameters of the run)"""
    out = [(0, 0, P)]
    if not OLD:
        out.append((ID2, 1, [p for p in P if p[4] == 'd']))
    return out


db = pytds.connect('127.0.0.1', DB, USER, 'xdbpw', port=PORT, autocommit=True)
cur = db.cursor()
for t in range(len(TARGETS)):
    cur.execute("if object_id('dbo.xp_%d') is not null drop table dbo.xp_%d" % (t, t))
    cur.execute('create table dbo.xp_%d (id int not null, src char(1) not null, c %s null, primary key (id, src))' % (t, TARGETS[t]))

# the literals, by the other client
lit_err = {}
for base, dt2, plist in runs():
    for t in range(len(TARGETS)):
        for i, p in enumerate(plist):
            lit = p[3] if (dt2 and p[3]) else p[2]
            try:
                cur.execute("insert into dbo.xp_%d (id, src, c) values (%d, 'L', %s)" % (t, base + i, lit))
            except pytds.Error as e:
                lit_err[(t, base + i)] = '%s %s' % (getattr(e, 'number', ''), str(e.args[0] if e.args else e).split('\n')[0][:110])

# the parameters, by the driver: every statement by itself, so that one that fails costs no other
par_err = {}
for base, dt2, plist in runs():
    with open(D + '/profile.emu', 'w') as f:
        f.write('P|MSPA|MSSQL|A|127.0.0.1|%d|%s|%s|*OFF|10|0|1000|6|N\nW|MSPA|xdbpw\n' % (PORT, DB, USER))
        if OLD:
            f.write('O|MSPA|tds|C|7.1\n')
        if dt2:
            f.write('O|MSPA|datetime2|C|on\n')
    script = ['ISO *NONE', 'C']
    order = []
    for t in range(len(TARGETS)):
        script.append("P s%d insert into dbo.xp_%d (id, src, c) values (?, 'P', ?)" % (t, t))
        for i, p in enumerate(plist):
            script.append('E s%d i:%d|%s' % (t, base + i, p[1]))
            order.append((t, base + i))
    script.append('D')
    env = dict(os.environ, XDB_EMU_JOBCCSID='1144', XDB_PRF_EMU=D + '/profile.emu', XDB_CONF='/dev/null', XDB_TEST_RDB='MSPA')
    out = subprocess.run([EMU + '/ardsh'], input=('\n'.join(script) + '\n').encode(), env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=1800).stdout.decode('utf-8', 'replace')
    n = -1
    for line in out.split('\n'):
        if line.startswith('> E s'):
            n += 1
        elif n >= 0 and line.startswith('ARXP') and 'sqlcode -' in line and order[n] not in par_err:
            par_err[order[n]] = line[12:150]
    if n + 1 != len(order):
        print('the harness answered %d requests of %d' % (n + 1, len(order)))
        print(out[-1500:])
        sys.exit(2)


def read(t):
    typ = TARGETS[t]
    x = 'cast(c as varchar(max))' if typ == 'text' else 'cast(c as nvarchar(max))' if typ in ('ntext', 'xml') else 'c'
    if OLD and typ in ('text', 'ntext'):
        x = 'cast(c as %s)' % ('varchar(8000)' if typ == 'text' else 'nvarchar(4000)')
    y = 'convert(nvarchar(120), c, 2)' if typ.startswith('varbinary') else \
        'convert(nvarchar(120), %s, 3)' % x if typ in ('float', 'real') else \
        'convert(nvarchar(120), %s, 121)' % x if typ.startswith('date') or typ == 'smalldatetime' else 'convert(nvarchar(120), %s)' % x
    ext = ", cast(sql_variant_property(c, 'BaseType') as varchar(30)), cast(sql_variant_property(c, 'Precision') as int), " \
          "cast(sql_variant_property(c, 'Scale') as int), cast(sql_variant_property(c, 'MaxLength') as int)" if typ == 'sql_variant' else ''
    cur.execute('select id, src, case when c is null then 1 else 0 end, convert(%s, %s), %s, datalength(c)%s from dbo.xp_%d' % (
        'varbinary(8000)' if OLD else 'varbinary(max)', x, y, ext, t))
    rows = {}
    for r in cur.fetchall():
        rows[(r[0], r[1])] = tuple(r[2:])
    return rows


bad = 0
both_ok = both_fail = 0
other = {}
kinds = {}
for t in range(len(TARGETS)):
    rows = read(t)
    for base, dt2, plist in runs():
        for i, p in enumerate(plist):
            k = (t, base + i)
            a, b = rows.get((base + i, 'P')), rows.get((base + i, 'L'))
            what = None
            if a is None and b is None:
                both_fail += 1
                if k not in par_err or k not in lit_err:
                    what = 'no row and no error (%s / %s)' % (par_err.get(k), lit_err.get(k))
                else:
                    # the same refusal?  The text the driver hands over is the start of the server's
                    pm = par_err[k].split(' outlen ')[-1].split(' ', 1)[-1].strip()
                    lm = lit_err[k].split(' ', 1)[-1].strip()
                    if pm[:40] != lm[:40]:
                        other.setdefault((pm[:60], lm[:60]), []).append('%s -> %s' % (p[0], TARGETS[t]))
            elif a is None:
                what = 'the server takes the literal, the parameter failed: %s' % par_err.get(k, 'no error told')
            elif b is None:
                what = 'the parameter was taken (%s), the literal fails: %s' % (a[2], lit_err.get(k, '?'))
            elif a != b:
                what = 'parameter %s, literal %s' % (str(a[2:])[:80], str(b[2:])[:80])
            else:
                both_ok += 1
                if TARGETS[t] == 'sql_variant' and a[4] is not None:
                    kinds.setdefault((p[0].split(' ')[0], dt2), set()).add('%s%s' % (a[4], '(%d,%d)' % (a[5], a[6]) if a[4] == 'decimal' else ''))
            if what:
                bad += 1
            if what or VERBOSE:
                print('%-4s %-34s -> %-17s%s %s' % ('DIFF' if what else 'ok', p[0][:34], TARGETS[t], ' [datetime2=on]' if dt2 else '',
                                                    what or (a[2] if a else 'refused by both: %s' % par_err.get(k, '')[:70])))
for (lab, dt2), v in sorted(kinds.items()):
    print('  a %-10s%s arrives as %s' % (lab, ' with datetime2=on' if dt2 else '', ', '.join(sorted(v))))
for (pm, lm), where in sorted(other.items()):
    if VERBOSE:
        print('  refused by both, in other words (%d times, as %s): the parameter "%s", the literal "%s"' % (len(where), where[0], pm, lm))
if other:
    print('  %d of the refusals are worded otherwise than the server words them for the literal (-v lists them)' % sum(len(w) for w in other.values()))
for t in range(len(TARGETS)):
    cur.execute('drop table dbo.xp_%d' % t)
print('%d host variables into %d column types, TDS %s: %d the same as the literal, %d refused by both, %d differ' % (
    len(P), len(TARGETS), '7.1' if OLD else '7.4', both_ok, both_fail, bad))
sys.exit(1 if bad else 0)

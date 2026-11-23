#!/usr/bin/env python3
# pgtypes.py - every data type of a PostgreSQL server through the driver,
# against what the value is.
#
# usage: python3 pgtypes.py <emu dir> [utf8] [hostile] [-v]
#   the server: see xdbqa.py (XDB_PG_PORT, XDB_PG_DB, XDB_PG_USER).
#   utf8     the profile asks for character data in CCSID 1208
#   hostile  the login used gets session defaults a driver must not trip
#            over (DateStyle German, IntervalStyle sql_standard, bytea_output
#            escape, extra_float_digits 0, another time zone) for the run;
#            they are taken off again at the end
#
# A value is written here as the server's literal and as what it is: a
# number, a date, a text.  For the types that arrive as text because Db2
# has no such type (interval, arrays, json, ranges, ...) the value is
# compared with the text the server itself makes of it, read by psycopg2.
# Values Db2 cannot hold (a date in the year 20000, infinity, NaN) must
# arrive as "could not be converted", not as another value.
import sys
from decimal import Decimal

import xdbqa

EMU = sys.argv[1]
UTF8 = 'utf8' in sys.argv[2:]
HOSTILE = 'hostile' in sys.argv[2:]
VERBOSE = '-v' in sys.argv[2:]
T = xdbqa.Target('pg')
T.profile(ccsid=1208 if UTF8 else 0)
ver, vtext = T.version()

C = []      # (label, expression, kind, want)   kind 'srv': the text the server makes of it


def add(typ, lit, kind, want, label=None):
    C.append((label or '%s %s' % (typ, lit), 'cast(%s as %s)' % (lit, typ), kind, want))


for v in (0, 32767, -32768):
    add('smallint', str(v), 'int', v)
for v in (0, 2147483647, -2147483648):
    add('integer', str(v), 'int', v)
for v in (9223372036854775807, -9223372036854775808):
    add('bigint', str(v), 'int', v)
add('boolean', "'t'", 'int', 1)
add('boolean', "'f'", 'int', 0)
add('oid', '4294967295', 'int', 4294967295)
for typ, v in (('numeric(9,2)', '12.50'), ('numeric(9,2)', '-9999999.99'), ('numeric(18,0)', '999999999999999999'),
               ('numeric(31,10)', '123456789012345678901.0123456789'), ('numeric(31,31)', '0.' + '1234567890' * 3 + '1'),
               ('numeric(5,0)', '0')):
    add(typ, "'%s'" % v, 'dec', v)
# without a precision: DECIMAL(31, numeric scale of the profile, 6)
add('numeric', "'12.5'", 'dec', '12.5')
add('numeric', "'-0.000001'", 'dec', '-0.000001')
add('numeric', "'1234567890123456789012345.123456'", 'dec', '1234567890123456789012345.123456')
# more digits than Db2 takes from a driver: 31
add('numeric(38,4)', "'1234567890123456789012345678901234.5678'", 'bad', 1)
add('numeric(38,4)', "'123456789012345678901234567.5678'", 'dec', '123456789012345678901234567.5678')
add('numeric', "'NaN'", 'bad', 1)
for v in ('0', '1.5', '-1.5', '0.1', '0.30000000000000004', '1.7976931348623157e308', '2.2250738585072014e-308', '123456789012345680',
          '5e-324'):
    add('double precision', "'%s'" % v, 'float', float(v))
for v in ('1.5', '0.1', '3.4028235e38', '16777217'):
    add('real', "'%s'" % v, 'real', float(v))
add('double precision', "'NaN'", 'bad', 1)
add('double precision', "'Infinity'", 'bad', 1)
add('real', "'-Infinity'", 'bad', 1)
for v in ('2026-01-31', '0001-01-01', '9999-12-31', '2000-02-29', '1582-10-10'):
    add('date', "'%s'" % v, 'date', v)
add('date', "'20000-01-01'", 'bad', 1)
add('date', "'0044-03-15 BC'", 'bad', 1)
add('date', "'infinity'", 'bad', 1)
add('date', "'-infinity'", 'bad', 1)
for v, w in (('00:00:00', '00:00:00'), ('23:59:59', '23:59:59'), ('13:45:10.987654', '13:45:10'), ('24:00:00', '24:00:00')):
    add('time', "'%s'" % v, 'time', w)
for v in ('2026-01-31 13:45:10.123456', '0001-01-01 00:00:00', '9999-12-31 23:59:59.999999', '2026-01-31 00:00:00'):
    add('timestamp', "'%s'" % v, 'ts', v)
add('timestamp(0)', "'2026-01-31 13:45:10.7'", 'ts', '2026-01-31 13:45:11')
add('timestamp', "'infinity'", 'bad', 1)
add('timestamp', "'20000-01-01 00:00:00'", 'bad', 1)
add('timestamp', "'0044-03-15 12:00:00 BC'", 'bad', 1)
# a timestamp with time zone arrives as the local time of the session: said in the zone of the value
add('timestamp with time zone', "'2026-01-31 13:45:10.5+00'", 'srvts', None)
add('timestamp with time zone', "'2026-07-31 23:59:59.999999-11'", 'srvts', None)
add('char(10)', "'abc'", 'text', 'abc')
add('char(5)', "'Größ'", 'text', 'Größ')
add('varchar(50)', "'x'", 'text', 'x')
add('varchar(50)', "''", 'text', '')
add('varchar(20)', "'Città èéù €'", 'text', 'Città èéù €')
add('text', "'short'", 'text', 'short')
add('text', "repeat('y', 600)", 'text', 'y' * 600, 'text of 600')
add('text', "repeat('z', 5000)", 'text', 'z' * 1000, 'text of 5000, cut at the text length 1000')
add('name', "'pg_class'", 'text', 'pg_class')
add('"char"', "'x'", 'text', 'x')
add('uuid', "'6f9619ff-8b86-d011-b42d-00c04fc964ff'", 'text', '6f9619ff-8b86-d011-b42d-00c04fc964ff')
add('bytea', "E'\\\\001\\\\002\\\\377'", 'text', '\\x0102ff')
add('bytea', "''", 'text', '\\x' if ver >= (9, 0) else '')
for typ, lit in (('interval', "'1 year 2 mons 3 days 04:05:06.789'"), ('interval', "'-1 day'"), ('interval', "'0'"),
                 ('time with time zone', "'13:45:10+02'"), ('integer[]', "'{1,2,NULL,4}'"), ('text[]', "'{\"a b\",c,\"d,e\"}'"),
                 ('inet', "'192.168.1.10/24'"), ('cidr', "'10.0.0.0/8'"), ('macaddr', "'08:00:2b:01:02:03'"),
                 ('point', "'(1.5,-2)'"), ('box', "'((0,0),(1,1))'"), ('bit(8)', "B'10101010'"), ('bit varying(16)', "B'101'"),
                 ('money', "'1234.56'"), ('tsvector', "'a fat cat'"), ('regclass', "'pg_class'"), ('xid', "'42'")):
    add(typ, lit, 'srv', None)
if ver >= (9, 2):
    add('json', "'{\"a\": 1, \"b\": [true, null]}'", 'srv', None)
    add('int4range', "'[1,10)'", 'srv', None)
    add('tstzrange', "'[2026-01-01 00:00+00,2026-02-01 00:00+00)'", 'srv', None)
if ver >= (9, 4):
    add('jsonb', "'{\"b\": 1, \"a\": \"x\"}'", 'srv', None)
    add('pg_lsn', "'16/B374D848'", 'srv', None)
if ver >= (10, 0):
    add('macaddr8', "'08:00:2b:01:02:03:04:05'", 'srv', None)
if ver >= (14, 0):
    add('int4multirange', "'{[1,3),[5,8)}'", 'srv', None)
    add('numeric', "'Infinity'", 'bad', 1)
# NULL of the types Db2 has a type for
for typ in ('smallint', 'integer', 'bigint', 'boolean', 'numeric(9,2)', 'numeric', 'real', 'double precision', 'date', 'time',
            'timestamp', 'timestamp with time zone', 'char(10)', 'varchar(50)', 'text', 'bytea', 'uuid', 'interval', 'integer[]'):
    C.append(('%s NULL' % typ, 'cast(null as %s)' % typ, 'null', None))
C.append(("a character outside the job's character set", "chr(1071) || chr(26085) || chr(128512) || '!'", 'text',
          '\u042f\u65e5\U0001f600!' if UTF8 else '\x1a\x1a\x1a!'))
C.append(('enum value', "(select 'sad'::xdb_qa_mood)", 'text', 'sad'))
C.append(('domain over numeric(8,2)', "(select '12.5'::xdb_qa_money)", 'dec', '12.5'))
C.append(('row value', "row(1, 'a b', null::int)", 'srv', None))

HOST = {'DateStyle': 'German, DMY', 'IntervalStyle': 'sql_standard', 'bytea_output': 'escape', 'extra_float_digits': '0',
        'TimeZone': 'Asia/Kolkata', 'lc_monetary': 'C'}
if ver < (9, 0):
    del HOST['bytea_output']
try:
    T.sql('drop type if exists xdb_qa_mood cascade')
    T.sql('drop domain if exists xdb_qa_money cascade')
    T.sql("create type xdb_qa_mood as enum ('sad', 'ok', 'happy')")
    T.sql('create domain xdb_qa_money as numeric(8,2)')
    if HOSTILE:
        for k, v in HOST.items():
            T.sql('alter role "%s" set %s = \'%s\'' % (T.user, k, v))
    out = T.run(EMU, ['C'] + ['Q select %s as v' % c[1] for c in C] + ['D'])
    # what the server says the values are, in a session set as the driver sets its own
    T._client = None
    for s in ("set datestyle to 'ISO'", "set intervalstyle to 'postgres'", "set bytea_output to 'hex'"):
        try:
            T.sql(s)
        except Exception:
            T.client().rollback() if not T.client().autocommit else None
    srv = {}
    for c in C:
        if c[2] in ('srv', 'srvts'):
            try:
                srv[c[0]] = T.sql('select (%s)::text' % c[1])[0][0]
            except Exception as e:
                srv[c[0]] = None
finally:
    for k in HOST:
        if HOSTILE:
            T.sql('alter role "%s" reset %s' % (T.user, k))
    T._client = None
    T.sql('drop type if exists xdb_qa_mood cascade')
    T.sql('drop domain if exists xdb_qa_money cascade')

B = [b for b in xdbqa.blocks(out) if b['cmd'].startswith('Q ')]
if len(B) != len(C):
    print('the harness answered %d requests of %d' % (len(B), len(C)))
    print(out[-1500:])
    sys.exit(2)
bad = 0
for c, b in zip(C, B):
    label, expr, kind, want = c
    what = None
    if b['err'] or not b['rows'] or not b['cols']:
        what = 'no value: %s' % (b['err'] or 'no row')
    else:
        got, col = b['rows'][0], b['cols'][0]
        if kind == 'srv':
            if srv[label] is None:
                continue        # the server itself has no such value or type
            w = srv[label]
            ok = xdbqa.same('text', col, got, w)
        elif kind == 'srvts':
            # 'YYYY-MM-DD HH:MM:SS[.ffffff]+zz' -> the local date and time
            w = srv[label]
            w = w[:w.rindex('+') if '+' in w[19:] else w.rindex('-')]
            ok = xdbqa.same('ts', col, got, w)
        elif kind == 'null':
            ok = got == '<null>'
        elif kind == 'dec' and col[1] == 485:
            # at the scale of the column: digits beyond it are cut off
            sc = col[2] & 255
            q = Decimal(want).quantize(Decimal(1).scaleb(-sc), rounding='ROUND_DOWN') if Decimal(want).as_tuple().exponent < -sc else Decimal(want)
            ok = xdbqa.packed(got, sc) == q if got not in ('<null>', '<conv-error>') else False
            w = want
        else:
            ok = xdbqa.same(kind, col, got, want)
            w = want
        if not ok:
            what = 'got %s as %s, it is %s' % (got[:80], xdbqa.coltype(col), 'a value Db2 cannot hold' if kind == 'bad' else repr(w)[:80])
    if what:
        bad += 1
    if what or VERBOSE:
        print('%-4s %-62s %s' % ('DIFF' if what else 'ok', label[:62], what or '%s as %s' % (b['rows'][0][:50], xdbqa.coltype(b['cols'][0]))))
print('PostgreSQL %d.%d, %d values%s%s: %d differ' % (ver[0], ver[1], len(C), ', character data in UTF-8' if UTF8 else '',
                                                    ', hostile session defaults' if HOSTILE else '', bad))
sys.exit(1 if bad else 0)

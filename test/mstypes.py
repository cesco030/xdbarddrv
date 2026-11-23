#!/usr/bin/env python3
# Linux test of the data type mapping against a real Microsoft SQL Server, through the
# emulation of the driver.  It checks itself: every value is read twice from the server,
# once as a column of its type - the way a program gets it - and once as the text the server
# itself makes of it (CONVERT), and the two must say the same.  Nothing is made on the server.
#   usage: python3 mstypes.py <emu dir with ardsh> [7.1]
#   server: XDB_MS_PORT (default 14340), XDB_MS_DB (xdbtest), XDB_MS_USER (xdbtest), password
#   xdbpw, on 127.0.0.1.  Not for Babelfish or the mock: they answer some of this differently.
#   With 7.1 the connection keeps to the protocol of SQL Server 2000, where the server sends
#   the types that came later (date, time, datetime2, datetimeoffset) as text.
# Output: one line for each value that differs or fails, then the counts; exit status 1 when
# something differs.  -v prints every case.
import os
import struct
import subprocess
import sys
from decimal import Decimal, getcontext

getcontext().prec = 100     # no number that is compared may be rounded on the way

EMU = sys.argv[1]
OLD = '7.1' in sys.argv[2:]
UTF8 = 'utf8' in sys.argv[2:]       # the profile asks for character data in CCSID 1208
VERBOSE = '-v' in sys.argv[2:]
D = '/tmp/xdb_mstypes'
os.makedirs(D, exist_ok=True)
with open(D + '/profile.emu', 'w') as f:
    f.write('P|MSTY|MSSQL|A|127.0.0.1|%s|%s|%s|*OFF|10|%d|1000|6|N\nW|MSTY|xdbpw\n' % (
        os.environ.get('XDB_MS_PORT', '14340'), os.environ.get('XDB_MS_DB', 'xdbtest'), os.environ.get('XDB_MS_USER', 'xdbtest'),
        1208 if UTF8 else 0))
    if OLD:
        f.write('O|MSTY|tds|C|7.1\n')

CASES = []      # (label, value expression, expression of its text, kind)


def add(typ, lits, kind, text=None):
    for lit in lits:
        v = 'cast(%s as %s)' % (lit, typ)
        CASES.append(('%s %s' % (typ, lit), v, (text or 'convert(varchar(400), %s)') % v, kind))
    v = 'cast(null as %s)' % typ
    CASES.append(('%s NULL' % typ, v, (text or 'convert(varchar(400), %s)') % v, kind))


add('tinyint', ['0', '255'], 'int')
add('smallint', ['-32768', '32767'], 'int')
add('int', ['-2147483648', '2147483647', '0'], 'int')
add('bigint', ['-9223372036854775808', '9223372036854775807'], 'int')
add('bit', ['0', '1'], 'int')
add('decimal(9,2)', ['0', '12.5', '-9999999.99'], 'dec')
add('decimal(18,0)', ['999999999999999999', '-1'], 'dec')
add('decimal(31,10)', ["'123456789012345678901.0123456789'", "'-0.0000000001'"], 'dec')
add('numeric(38,10)', ["'123456789012345678901.0123456789'", "'-999999999999999999999.9999999999'"], 'dec')
add('decimal(38,0)', ["'9999999999999999999999999999999'", "'-9999999999999999999999999999999'"], 'dec')
add('decimal(38,0)', ["'10000000000000000000000000000000'"], 'over')
add('decimal(38,38)', ["'0.12345678901234567890123456789012345678'"], 'dec')
add('money', ['922337203685477.5807', '-922337203685477.5808', '0.0001'], 'dec', 'convert(varchar(400), %s, 2)')
add('smallmoney', ['214748.3647', '-214748.3648'], 'dec', 'convert(varchar(400), %s, 2)')
add('float', ['0', '1.5', '0.1', '-2.2250738585072014E-308', '1.7976931348623157E+308', '123456789.123456789', '1e15'], 'float', 'convert(varchar(400), %s, 3)')
add('real', ['0.1', '3.40282347E+38', '-1.17549435E-38', '16777216'], 'real', 'convert(varchar(400), cast(%s as float), 3)')
add('date', ["'0001-01-01'", "'9999-12-31'", "'2026-02-28'"], 'date', 'convert(varchar(400), %s, 23)')
add('time(0)', ["'23:59:59'", "'00:00:00'"], 'time', 'convert(varchar(400), %s, 108)')
add('time(7)', ["'23:59:59.9999999'", "'13:45:10.1234567'"], 'time', 'convert(varchar(400), %s, 108)')
# a datetime as the server shows it, in milliseconds (its 1/300 s inside are .997 for .9966667)
add('datetime', ["'1753-01-01 00:00:00'", "'9999-12-31 23:59:59.997'", "'2026-01-31 13:45:10.123'"], 'ts', 'convert(varchar(400), %s, 121)')
add('smalldatetime', ["'1900-01-01 00:00'", "'2079-06-06 23:59'"], 'ts', 'convert(varchar(400), %s, 121)')
for n in (0, 3, 6, 7):
    add('datetime2(%d)' % n, ["'0001-01-01 00:00:00'", "'9999-12-31 23:59:59.9999999'" if n == 7 else "'9999-12-31 23:59:59'", "'2026-01-31 13:45:10.1234567'"],
        'ts', 'convert(varchar(400), cast(%s as datetime2(7)), 121)')
add('datetimeoffset(7)', ["'2026-01-31 13:45:10.1234567 +05:30'", "'0001-01-01 00:00:00 +00:00'", "'9999-12-31 23:59:59.9999999 +14:00'"], 'text',
    'convert(varchar(400), %s, 121)')
add('datetimeoffset(0)', ["'2026-01-31 13:45:10 -08:00'"], 'text', 'convert(varchar(400), %s, 121)')
add('datetimeoffset(3)', ["'2026-01-31 23:45:10.127 +02:00'", "'2026-12-31 23:59:59.999 -00:30'"], 'text', 'convert(varchar(400), %s, 121)')
add('char(10)', ["'abc'", "''", "'0123456789'"], 'text')
add('varchar(50)', ["'x'", "''", "'ab  '", "'Citt' + char(224) + ' ' + char(232) + char(233) + char(249)", "replicate('y', 50)"], 'text')
add('nchar(5)', ["N'ab'", "N'Gr' + nchar(246) + nchar(223)"], 'text')
add('nvarchar(50)', ["N'x'", "N''", "nchar(224) + nchar(8364) + nchar(1071) + nchar(26085)", "nchar(55357) + nchar(56832) + N'!'"], 'text', 'convert(nvarchar(400), %s)')
add('varchar(max)', ["'short'", "replicate('x', 200)"], 'text')
add('nvarchar(max)', ["N'short'", "replicate(nchar(232), 200)"], 'text', 'convert(nvarchar(400), %s)')
add('text', ["'old text'"], 'text', 'convert(varchar(400), %s)')
add('ntext', ["N'old ' + nchar(232)"], 'text', 'convert(nvarchar(400), %s)')
add('xml', ["'<a b=\"1\">t</a>'"], 'text', 'convert(nvarchar(400), %s)')
add('uniqueidentifier', ["'6F9619FF-8B86-D011-B42D-00C04FC964FF'", "'00000000-0000-0000-0000-000000000000'"], 'text')
add('binary(4)', ['0x0A0B', '0x00000000'], 'hex', 'convert(varchar(400), %s, 2)')
add('varbinary(20)', ['0x', '0x00FF10', '0x0102030405060708090A0B0C0D0E0F1011121314'], 'hex', 'convert(varchar(400), %s, 2)')
add('varbinary(max)', ['0x0102', "cast(replicate('a', 100) as varbinary(max))"], 'hex', 'convert(varchar(400), %s, 2)')
add('image', ['0x0405'], 'hex', 'convert(varchar(400), cast(%s as varbinary(max)), 2)')
add('rowversion', ['0x0000000000000FA1'], 'hex', 'convert(varchar(400), cast(%s as varbinary(8)), 2)')
add('hierarchyid', ["'/1/2/'", "'/'"], 'hex', 'convert(varchar(400), cast(%s as varbinary(max)), 2)')
for lab, inner, kind, text in (
        ('int', '123', 'text', None), ('bigint', 'cast(-5 as bigint)', 'text', None), ('varchar', "'abc'", 'text', None),
        ('nvarchar', "N'Gr' + nchar(246)", 'text', 'convert(nvarchar(400), %s)'), ('decimal', 'cast(1.50 as decimal(9,2))', 'text', None),
        ('money', 'cast(12.3456 as money)', 'text', 'convert(varchar(400), cast(%s as money), 2)'), ('bit', 'cast(1 as bit)', 'text', None),
        ('float', 'cast(1.5 as float)', 'vfloat', 'convert(varchar(400), cast(%s as float), 3)'),
        ('date', "cast('2026-01-31' as date)", 'text', 'convert(varchar(400), cast(%s as date), 23)'),
        ('datetime', "cast('2026-01-31 13:45:10.123' as datetime)", 'vts', 'convert(varchar(400), cast(%s as datetime), 121)'),
        ('datetime2', "cast('2026-01-31 13:45:10.1234567' as datetime2(7))", 'vts', 'convert(varchar(400), cast(%s as datetime2(7)), 121)'),
        ('time', "cast('13:45:10.5' as time(3))", 'text', 'convert(varchar(400), cast(%s as time(3)), 114)'),
        ('uniqueidentifier', "cast('6F9619FF-8B86-D011-B42D-00C04FC964FF' as uniqueidentifier)", 'text', None),
        ('varbinary', 'cast(0x0102FF as varbinary(10))', 'hex', 'convert(varchar(400), cast(%s as varbinary(10)), 2)')):
    v = 'cast(%s as sql_variant)' % inner
    CASES.append(('sql_variant of %s' % lab, v, (text or 'convert(varchar(400), %s)') % v, kind))
CASES.append(('sql_variant NULL', 'cast(null as sql_variant)', 'convert(varchar(400), cast(null as sql_variant))', 'text'))
# json and vector came with SQL Server 2025; a client that does not ask for them otherwise gets them as text
env0 = dict(os.environ, XDB_EMU_JOBCCSID='1144', XDB_PRF_EMU=D + '/profile.emu', XDB_CONF='/dev/null', XDB_TEST_RDB='MSTY')
ver = subprocess.run([EMU + '/ardsh'], input=b"C\nQ select cast(serverproperty('ProductMajorVersion') as int) as v\nD\n", env=env0,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120).stdout.decode('utf-8', 'replace')
major = [int(l[7:]) for l in ver.split('\n') if l.startswith('  row: ') and l[7:].strip().isdigit()]
if major and major[0] >= 17:
    add('json', ["'{\"a\":1,\"b\":[true,null]}'"], 'text', 'convert(nvarchar(400), %s)')
    add('vector(3)', ["'[1,2.5,-3]'"], 'text', 'convert(nvarchar(400), %s)')
CASES.append(('geography point', 'geography::Point(45.5, 9.2, 4326)', 'convert(varchar(400), cast(geography::Point(45.5, 9.2, 4326) as varbinary(max)), 2)', 'hex'))

script = 'C\n' + ''.join('Q select %s as v\nQ select %s as v\n' % (c[1], c[2]) for c in CASES) + 'D\n'
env = dict(os.environ, XDB_SH_FLOAT17='1', XDB_EMU_JOBCCSID='1144', XDB_PRF_EMU=D + '/profile.emu', XDB_CONF='/dev/null', XDB_SH_MAXROWS='5', XDB_TEST_RDB='MSTY')
out = subprocess.run([EMU + '/ardsh'], input=script.encode(), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900).stdout.decode('utf-8', 'replace')

blocks = []
for line in out.split('\n'):
    if line.startswith('> Q '):
        blocks.append({'col': None, 'row': None, 'err': None})
    elif blocks:
        b = blocks[-1]
        if line.startswith('  col ') and b['col'] is None:
            p = line.split()
            b['col'] = (int(p[3]), int(p[5]))
        elif line.startswith('  row: ') and b['row'] is None:
            b['row'] = line[7:]
        elif line.startswith('AR') and 'sqlcode -' in line and b['err'] is None:
            b['err'] = line[:150]


def text_of(r):
    return r[1:-1] if r is not None and r.startswith('[') and r.endswith(']') else r


def packed(r, scale):
    h = r[2:-1]
    d = Decimal(int(h[:-1] or '0')).scaleb(-scale)
    return -d if h[-1] in 'DB' else d


def same(kind, col, got, want):
    """got: what the driver handed over; want: the text of the server"""
    if want == '<null>' or got == '<null>':
        return got == want
    w = text_of(want)
    typ, ln = col
    if kind == 'over':
        return got == '<conv-error>'
    if kind == 'int':
        return typ in (497, 501, 493) and int(got) == int(w)
    if kind == 'dec':
        if typ != 485:
            return False
        sc = ln & 255
        q = Decimal(w)
        q = (q * (Decimal(10) ** sc)).to_integral_value(rounding='ROUND_DOWN') / (Decimal(10) ** sc)
        return packed(got, sc) == q
    if kind in ('float', 'real', 'vfloat'):
        g = float(text_of(got)) if kind == 'vfloat' else float(got)
        if kind == 'real':
            return struct.pack('f', g) == struct.pack('f', float(w))
        return g == float(w)
    g = text_of(got)
    if typ != 449 or kind in ('text', 'hex', 'vts'):
        if kind == 'date':
            g = '%s-%s-%s' % (g[6:10], g[3:5], g[0:2])
        elif kind == 'time':
            g = g.replace('.', ':')
        elif kind == 'ts':
            g = g[:10] + ' ' + g[11:].replace('.', ':', 2)
            w = (w + '.000000' if '.' not in w else w + '000000')[:26]
        elif kind == 'vts':
            # a date and time inside a sql_variant: as text, with the digits the server shows
            return g.replace('T', ' ')[:len(w)] == w or g == w
        elif kind == 'hex':
            return g.upper() == w.upper()
        return g.rstrip(' ') == w.rstrip(' ')
    return None     # a date or time type that arrived as text: compared by the caller


# What some values are, written here and not asked of the server: characters
# the job (CCSID 1144) does not have arrive as its substitution character,
# and whole with a profile that asks for CCSID 1208.
KNOWN = {
    "nvarchar(50) nchar(224) + nchar(8364) + nchar(1071) + nchar(26085)": ('\u00e0\u20ac\u042f\u65e5', '\u00e0\u20ac\x1a\x1a'),
    "nvarchar(50) nchar(55357) + nchar(56832) + N'!'": ('\U0001f600!', '\x1a!'),
    "nchar(5) N'Gr' + nchar(246) + nchar(223)": ('Gr\u00f6\u00df', 'Gr\u00f6\u00df'),
    "varchar(50) 'Citt' + char(224) + ' ' + char(232) + char(233) + char(249)": ('Citt\u00e0 \u00e8\u00e9\u00f9', 'Citt\u00e0 \u00e8\u00e9\u00f9'),
    "nvarchar(max) replicate(nchar(232), 200)": ('\u00e8' * 200, '\u00e8' * 200),
    "ntext N'old ' + nchar(232)": ('old \u00e8', 'old \u00e8'),
    "sql_variant of nvarchar": ('Gr\u00f6', 'Gr\u00f6'),
}
known_seen = 0

names = {449: 'VARCHAR', 453: 'CHAR', 485: 'DECIMAL', 497: 'INTEGER', 501: 'SMALLINT', 493: 'BIGINT', 481: 'DOUBLE', 385: 'DATE', 389: 'TIME', 393: 'TIMESTAMP'}
bad = 0
astext = 0
if len(blocks) != 2 * len(CASES):
    print('the harness answered %d requests of %d' % (len(blocks), 2 * len(CASES)))
    print(out[-1500:])
    sys.exit(2)
for i, c in enumerate(CASES):
    a, o = blocks[2 * i], blocks[2 * i + 1]
    what = None
    if o['err'] or o['row'] is None:
        what = 'the server gave no text for it: %s' % (o['err'] or 'no row')
    elif a['err'] or a['row'] is None:
        what = 'no value: %s' % (a['err'] or 'no row')
    else:
        r = same(c[3], a['col'], a['row'], o['row'])
        if r is None:
            # the old protocol: the type arrives as the text the server makes by itself
            astext += 1
            r = a['row'] != '<conv-error>' and len(text_of(a['row'])) >= 8
            if not r:
                what = 'as text: got %s' % a['row']
        elif not r:
            what = 'got %s as %s(%d), the server says %s' % (a['row'][:90], names.get(a['col'][0], a['col'][0]), a['col'][1], o['row'][:90])
        if not what and c[0] in KNOWN:
            known_seen += 1
            if text_of(a['row']).rstrip(' ') != KNOWN[c[0]][0 if UTF8 else 1]:
                what = 'got %s, it is %r' % (a['row'][:90], KNOWN[c[0]][0 if UTF8 else 1][:60])
    if what:
        bad += 1
    if what or VERBOSE:
        print('%-4s %-58s %s' % ('DIFF' if what else 'ok', c[0][:58], what or '%s as %s' % (a['row'][:60], names.get(a['col'][0], a['col'][0]))))
if known_seen != len(KNOWN):
    print('DIFF only %d of the %d values known here were looked at' % (known_seen, len(KNOWN)))
    bad += 1
print('%d values of %d types, TDS %s%s: %d differ%s' % (len(CASES), len(set(c[0].split(' ')[0] for c in CASES)), '7.1' if OLD else '7.4',
                                                         ', character data in UTF-8' if UTF8 else '', bad,
                                                         ', %d arrived as text (types the old protocol does not have)' % astext if astext else ''))
sys.exit(1 if bad else 0)

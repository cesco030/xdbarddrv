#!/usr/bin/env python3
# tdsfuzz.py - damaged answers of a real SQL Server, for the SQL Server
# backend of the driver.
#
# usage: python3 tdsfuzz.py <emu dir> <first seed> <rounds> [workers] [7.1] [keep=<dir>]
#   env XDB_MS_PORT (14340), XDB_MS_DB (xdbtest), XDB_MS_USER (xdbtest); the
#   password of the test login is xdbpw.  <emu dir> should hold a build with
#   the sanitizers (XDB_EXTRA="-fsanitize=address,undefined ...").
#
# Every round runs the same script through ardsh, which connects to a
# proxy of this program; the proxy passes everything on to the server and
# back, and damages one packet (now and then two) of what the server
# answers, in a way the seed of the round decides: bits flipped, bytes set
# to the values a parser trips over, lengths made huge, a piece cut out or
# doubled, the packet header changed, the connection closed in the middle.
# After the damage the proxy closes the connection as soon as nothing has
# moved for a while, as a line that died would.  A connection the driver
# makes after that is passed on untouched: the server is still there.
#
# What the driver may do with a damaged answer: report an error, or hand
# over what it understood.  What it may not do: read or write memory it
# does not own, run into undefined behaviour, ask for absurd amounts of
# memory and fall over, or go on for ever.  A round is
#   ok     the harness ended by itself, whatever it printed
#   CRASH  a sanitizer report, a signal, or an exit code of a fault
#   HANG   the harness was still running after the time limit
# The output and the seed of every round that is not ok are kept in the
# keep directory; the same seed gives the same damage again.
import os
import random
import select
import signal
import socket
import subprocess
import sys
import threading
import time

EMU = sys.argv[1]
SEED0 = int(sys.argv[2])
ROUNDS = int(sys.argv[3])
rest = sys.argv[4:]
WORKERS = int(rest[0]) if rest and rest[0].isdigit() else 4
OLD = '7.1' in rest
KEEP = [a[5:] for a in rest if a.startswith('keep=')]
KEEP = KEEP[0] if KEEP else '/tmp/xdb_tdsfuzz'
PORT = int(os.environ.get('XDB_MS_PORT', '14340'))
DB = os.environ.get('XDB_MS_DB', 'xdbtest')
USER = os.environ.get('XDB_MS_USER', 'xdbtest')
LIMIT = 40          # seconds a round may take
IDLE = 1.5          # seconds without a byte, after the damage, until the line is closed
os.makedirs(KEEP, exist_ok=True)

SCRIPT = """C
Q select cast(1 as tinyint) a, cast(2 as smallint) b, 3 c, cast(4 as bigint) d, cast(1 as bit) e, cast(1.5 as decimal(9,2)) f, cast(2.5 as money) g, cast(1.5 as float) h, cast(1.5 as real) i, cast('2026-01-31' as date) j, cast('13:45:10.123' as time(3)) k, getdate() l, sysdatetime() m, sysdatetimeoffset() n, cast('abc' as char(5)) o, cast('abc' as varchar(10)) p, N'ab' + nchar(8364) q, newid() r, cast(0x0102 as varbinary(4)) s, cast('x' as varchar(max)) t, cast(N'y' as nvarchar(max)) u, cast('<a/>' as xml) v, cast(1 as sql_variant) w, cast(null as int) x, cast(12345678901234567890.5 as decimal(38,4)) y, cast('2026-01-31 10:00' as smalldatetime) z
Q select top 200 name, object_id, create_date, cast(null as varchar(10)) n1, cast(null as int) n2, cast(null as datetime) n3, cast(null as int) n4, cast(null as bigint) n5, cast(null as decimal(9,2)) n6, cast(null as nvarchar(max)) n7, cast(null as uniqueidentifier) n8, cast(null as date) n9 from sys.all_objects order by object_id
Q select replicate(cast('x' as varchar(max)), 20000) as big, replicate(cast(N'y' as nvarchar(max)), 9000) as nbig, cast(replicate(cast(0x41 as varbinary(max)), 5000) as varbinary(max)) as bbig
Q select cast(cast('2026-01-31 13:45:10.1234567' as datetime2(7)) as sql_variant) a, cast(cast('13:45:10.5' as time(3)) as sql_variant) b, cast(cast(1.5 as float) as sql_variant) c, cast(N'v' as sql_variant) d, cast(cast(12.5 as money) as sql_variant) e, cast(newid() as sql_variant) f, cast(0x0102 as sql_variant) g, cast(cast('2026-01-31' as date) as sql_variant) h
X create table #f (id int identity primary key, v varchar(20) null, n decimal(9,2) null, d datetime null)
P a insert into #f (v, n, d) values (?, ?, ?)
E a abc|@p9,2:12.5|@Z:2026-01-31-13.45.10.123456
E a n:|@p9,2:-1|@D:2026-01-31
E a @g:x|@f:1.5|n:
Q select * from #f order by id
O c1 select id, v from #f where id >= ? order by id ## i:1
F c1
K c1
Q select 1/0 as z
Q select cast('x' as int) as z
Q select * from no_such_table_xdb
X insert into #f (id, v) values (1, 'dup')
X print 'a message'
Q exec sp_executesql N'select 1 as a; select 2 as b, 3 as c'
Q exec sp_helpdb
Q select top 3 name, object_id from sys.objects for xml auto
X create table #doc (id int, body varbinary(max) null, note nvarchar(max) null)
X insert into #doc values (1, cast(replicate(cast(0x41 as varbinary(max)), 30000) as varbinary(max)), replicate(cast(nchar(232) as nvarchar(max)), 20000))
Q select id, put_blob_to_file(body, '@W@/out.bin') as b, put_clob_to_file(note, '@W@/out.txt') as c from #doc
X insert into #doc (id, body, note) values (2, get_blob_from_file('@W@/in.bin'), get_clob_from_file('@W@/in.txt'))
Q select id, datalength(body) as lb, datalength(note) as ln from #doc order by id
X create procedure #pa @a int, @b int, @t int output as begin set @t = @a + @b; select @t as s; return 7 end
Q exec #pa 1, 2, 0
X declare @t int; exec #pa 3, 4, @t output
Q select @@spid as spid, @@trancount as tc, db_name() as db
Q XDB CONNECTION INFO
COMMIT
X update #f set v = 'z' where id = 1
ROLLBACK
Q select count(*) as n from #f
D
"""

TOKENS = [0x81, 0xD1, 0xD2, 0xFD, 0xFE, 0xFF, 0xAA, 0xAB, 0xE3, 0xAC, 0x79, 0xA9, 0xAD, 0xA4, 0xA5, 0x88, 0xED, 0xEE, 0xAE, 0x01]
EDGE = [0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0x26, 0xA7, 0xE7, 0xA5, 0xF1, 0x62, 0x6C, 0x29, 0x2A, 0x2B, 0x23, 0x63, 0x22, 0xF0]

lock = threading.Lock()
stats = {'ok': 0, 'CRASH': 0, 'HANG': 0, 'damaged': 0, 'kinds': {}}


def damage(rng, pkt):
    """one packet of the server (header and payload) -> (bytes to send, close after it, what was done)"""
    hdr, pay = bytearray(pkt[:8]), bytearray(pkt[8:])
    kind = rng.choice(['flip', 'flip', 'set', 'set', 'token', 'len', 'len', 'cut', 'cutclose', 'dup', 'drop', 'hdr', 'junk', 'close',
                       'zero', 'ones'])
    close = False
    n = len(pay)
    if kind in ('flip', 'set', 'token', 'len', 'cut', 'cutclose', 'dup', 'drop', 'zero', 'ones') and n == 0:
        kind = 'hdr'
    if kind == 'flip':
        for _ in range(rng.randint(1, 8)):
            i = rng.randrange(n)
            pay[i] ^= 1 << rng.randrange(8)
    elif kind == 'set':
        for _ in range(rng.randint(1, 4)):
            pay[rng.randrange(n)] = rng.choice(EDGE)
    elif kind == 'token':
        for _ in range(rng.randint(1, 2)):
            pay[rng.randrange(n)] = rng.choice(TOKENS)
    elif kind == 'len':
        i = rng.randrange(n)
        v = rng.choice([b'\xff\xff', b'\xff\xff\xff\xff', b'\xff\xff\xff\x7f', b'\x00\x00', b'\x00\x80', b'\xfe\xff\xff\xff\xff\xff\xff\xff',
                        b'\xff\xff\xff\xff\xff\xff\xff\xff', b'\x00\x00\x00\x80', b'\xff\x7f', b'\x01\x00\x00\x00'])
        pay[i:i + len(v)] = v
        del pay[n:]
    elif kind in ('cut', 'cutclose'):
        del pay[rng.randrange(n):]
        close = kind == 'cutclose'
    elif kind == 'dup':
        i = rng.randrange(n)
        j = min(n, i + rng.randint(1, 64))
        pay[i:i] = pay[i:j]
    elif kind == 'drop':
        i = rng.randrange(n)
        del pay[i:min(n, i + rng.randint(1, 64))]
    elif kind == 'zero':
        i = rng.randrange(n)
        j = min(n, i + rng.randint(1, 32))
        pay[i:j] = bytes(j - i)
    elif kind == 'ones':
        i = rng.randrange(n)
        j = min(n, i + rng.randint(1, 32))
        pay[i:j] = b'\xff' * (j - i)
    elif kind == 'junk':
        pay = bytearray(rng.getrandbits(8) for _ in range(rng.randint(0, 600)))
    elif kind == 'close':
        return b'', True, kind
    if kind == 'hdr':
        what = rng.choice(['type', 'status', 'short', 'long', 'tiny', 'huge'])
        kind = 'hdr-' + what
        if what == 'type':
            hdr[0] = rng.choice([0x00, 0x01, 0x03, 0x04, 0x0E, 0x10, 0x12, 0x17, 0xFF])
        elif what == 'status':
            hdr[1] ^= rng.choice([0x01, 0x02, 0x08, 0xFF])
        else:
            ln = {'short': max(8, len(pkt) - rng.randint(1, 40)), 'long': min(65535, len(pkt) + rng.randint(1, 4000)),
                  'tiny': rng.randint(0, 8), 'huge': 65535}[what]
            hdr[2], hdr[3] = ln >> 8, ln & 255
            return bytes(hdr) + bytes(pay), False, kind
    ln = min(65535, 8 + len(pay))
    hdr[2], hdr[3] = ln >> 8, ln & 255
    return bytes(hdr) + bytes(pay), close, kind


def proxy(lsock, rng, state):
    """the connections of the driver, one after the other, passed on to the server; the first one gets the damage"""
    lsock.settimeout(0.3)
    first = True
    while not state.get('stop'):
        try:
            cli, _ = lsock.accept()
        except socket.timeout:
            continue
        except OSError:
            return
        relay(cli, rng, state, first)
        first = False


def relay(cli, rng, state, first):
    try:
        srv = socket.create_connection(('127.0.0.1', PORT), timeout=10)
    except OSError:
        cli.close()
        state['noserver'] = True
        return
    cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    srv.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = b''
    seen = 0
    target = state['target'] if first else -1
    again = state['again']
    done = False
    last = time.time()
    try:
        while not state.get('stop'):
            r, _, _ = select.select([cli, srv], [], [], 0.25)
            if not r:
                if done and time.time() - last > IDLE:
                    break
                continue
            last = time.time()
            if cli in r:
                d = cli.recv(65536)
                if not d:
                    break
                srv.sendall(d)
            if srv in r:
                d = srv.recv(65536)
                if not d:
                    break
                buf += d
                while len(buf) >= 8:
                    ln = (buf[2] << 8) | buf[3]
                    if ln < 8:
                        ln = 8
                    if len(buf) < ln:
                        break
                    pkt, buf = buf[:ln], buf[ln:]
                    if target >= 0 and (seen == target or (again and seen == target + again)):
                        out, close, kind = damage(rng, pkt)
                        state['kinds'].append(kind)
                        done = True
                        if out:
                            cli.sendall(out)
                        if close:
                            raise ConnectionAbortedError
                    else:
                        cli.sendall(pkt)
                    seen += 1
    except (OSError, ConnectionAbortedError):
        pass
    finally:
        if first:
            state['packets'] = seen
        else:
            state['again_conns'] = state.get('again_conns', 0) + 1
        for s in (cli, srv):
            try:
                s.close()
            except OSError:
                pass


def one_round(seed, wdir, npackets):
    rng = random.Random(seed)
    state = {'target': rng.randrange(max(1, npackets)), 'again': rng.choice([0, 0, 0, 1, 2, 5]), 'kinds': [], 't0': time.time()}
    if npackets == 0:
        state['target'] = -1    # the clean run that counts the packets
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', 0))
    ls.listen(4)
    port = ls.getsockname()[1]
    with open(wdir + '/profile.emu', 'w') as f:
        f.write('P|FUZZ|MSSQL|A|127.0.0.1|%d|%s|%s|*OFF|10|0|1000|6|N\nW|FUZZ|xdbpw\n' % (port, DB, USER))
        if OLD:
            f.write('O|FUZZ|tds|C|7.1\n')
        if rng.random() < 0.3:
            f.write('O|FUZZ|fetch_rows|D|%d\n' % rng.choice([1, 7, 50, 3000]))
    th = threading.Thread(target=proxy, args=(ls, rng, state), daemon=True)
    th.start()
    env = dict(os.environ, XDB_EMU_JOBCCSID='1144', XDB_PRF_EMU=wdir + '/profile.emu', XDB_CONF='/dev/null', XDB_TEST_RDB='FUZZ',
               XDB_SH_MAXROWS='3', LD_LIBRARY_PATH=EMU,
               ASAN_OPTIONS=os.environ.get('XDB_FUZZ_ASAN', 'detect_leaks=0:exitcode=97:allocator_may_return_null=1:max_allocation_size_mb=512'),
               UBSAN_OPTIONS='halt_on_error=1:exitcode=96:print_stacktrace=1')
    if not os.path.exists(wdir + '/in.bin'):
        with open(wdir + '/in.bin', 'wb') as f:
            f.write(bytes(range(256)) * 300)
        with open(wdir + '/in.txt', 'w', encoding='utf-8') as f:
            f.write('una riga di testo con accenti: \u00e0\u00e8\u00ec\u00f2\u00f9 \u20ac\n' * 900)
    p = subprocess.Popen([EMU + '/ardsh'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                         start_new_session=True)
    verdict = 'ok'
    try:
        out, _ = p.communicate(SCRIPT.replace('@W@', wdir).encode(), timeout=LIMIT)
    except subprocess.TimeoutExpired:
        verdict = 'HANG'
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
        out, _ = p.communicate()
    state['stop'] = True
    th.join(5)
    ls.close()
    text = out.decode('utf-8', 'replace')
    if verdict == 'ok' and (p.returncode != 0 or 'ERROR: AddressSanitizer' in text or 'runtime error:' in text or 'LeakSanitizer' in text):
        verdict = 'CRASH'
    if 'failed to allocate' in text:
        state['bigalloc'] = True    # the driver asked for more memory than the limit of the run and went on without it
    state['rc'] = p.returncode
    return verdict, state, text


def worker(k, seeds, npackets):
    wdir = '%s/w%d' % (KEEP, k)
    os.makedirs(wdir, exist_ok=True)
    for seed in seeds:
        verdict, state, text = one_round(seed, wdir, npackets)
        with lock:
            stats[verdict] += 1
            if state['kinds']:
                stats['damaged'] += 1
            if state.get('again_conns'):
                stats['revived'] = stats.get('revived', 0) + 1
            for kind in state['kinds']:
                stats['kinds'][kind] = stats['kinds'].get(kind, 0) + 1
            if state.get('bigalloc'):
                stats['bigalloc'] = stats.get('bigalloc', 0) + 1
                with open('%s/seed%d_bigalloc.txt' % (KEEP, seed), 'w') as f:
                    f.write('seed %d, packet %d of %d, damage %s\n' % (seed, state['target'], npackets, state['kinds']))
                    f.write(text[-12000:])
            if verdict != 'ok' or state.get('noserver'):
                name = '%s/seed%d_%s.txt' % (KEEP, seed, verdict)
                with open(name, 'w') as f:
                    f.write('seed %d, packet %d of %d, damage %s, exit code %s\n' % (seed, state['target'], npackets, state['kinds'], state.get('rc')))
                    f.write(text[-12000:])
                print('%s seed %d (packet %d, %s) -> %s' % (verdict, seed, state['target'], ','.join(state['kinds']), name), flush=True)


# a clean round first: how many packets does the server send, and does the script run?
v, st, text = one_round(-1, KEEP, 0)
NP = st.get('packets', 0)
if v != 'ok' or NP < 10 or 'sqlcode -30080' in text or st.get('noserver'):
    print('the clean round did not work (%s, %d packets):' % (v, NP))
    print(text[-2500:])
    sys.exit(2)
print('clean round: %d packets from the server, %d lines of output' % (NP, text.count('\n')), flush=True)
t0 = time.time()
seeds = list(range(SEED0, SEED0 + ROUNDS))
ths = [threading.Thread(target=worker, args=(k, seeds[k::WORKERS], NP)) for k in range(WORKERS)]
for t in ths:
    t.start()
for t in ths:
    t.join()
print('%d rounds (seeds %d to %d), TDS %s, %d with damage delivered: %d ok, %d CRASH, %d HANG, in %d s' % (
    ROUNDS, SEED0, SEED0 + ROUNDS - 1, '7.1' if OLD else '7.4', stats['damaged'], stats['ok'], stats['CRASH'], stats['HANG'], time.time() - t0))
print('rounds in which the driver connected again after the damage: %d' % stats.get('revived', 0))
print('rounds in which the driver asked for more than 512 MB at once (refused, and it went on): %d' % stats.get('bigalloc', 0))
print('damage: ' + ', '.join('%s %d' % kv for kv in sorted(stats['kinds'].items())))
sys.exit(1 if stats['CRASH'] or stats['HANG'] else 0)

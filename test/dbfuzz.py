#!/usr/bin/env python3
# dbfuzz.py - damaged answers of a real PostgreSQL, MySQL or MariaDB server,
# for the two backends of the driver that are not SQL Server (that one has
# tdsfuzz.py, which works the same way).
#
# usage: python3 dbfuzz.py <emu dir> pg|mysql <first seed> <rounds> [workers] [keep=<dir>] [aim=<from>-<to>]
#   aim: damage only messages of that part of the script, in hundredths of
#   all the messages of a clean round (aim=70-100: the last three tenths,
#   where the file functions are); without it half of the rounds aim at the
#   first 60 messages, the sign-on and the first results.
#   the server: see xdbqa.py.  <emu dir> should hold a build with the
#   sanitizers, of the driver and of the client library of PostgreSQL:
#     XDB_EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer"
#     sh build/build_emu.sh <kit> <emu dir>; sh build/build_ard_emu.sh <kit> <emu dir>
#
# Every round runs the same script through ardsh, which connects to a proxy
# of this program; the proxy passes everything on to the server and back,
# and damages one message of what the server answers (now and then two),
# in a way the seed of the round decides: bits flipped, bytes set to the
# values a parser trips over, lengths made huge, a piece cut out or
# doubled, the message header changed, the connection closed in the
# middle.  After the damage the proxy closes the connection as soon as
# nothing has moved for a while, as a line that died would; a connection
# the driver makes after that is passed on untouched.
#
# What the driver may do with a damaged answer: report an error, or hand
# over what it understood.  What it may not do: read or write memory it
# does not own, run into undefined behaviour, ask for absurd amounts of
# memory and fall over, or go on for ever.  A round is
#   ok     the harness ended by itself, whatever it printed
#   CRASH  a sanitizer report, a signal, or an exit code of a fault
#   HANG   the harness was still running after the time limit
# The output, the seed and the damaged messages (first 400 bytes, in
# hexadecimal) of every round that is not ok are kept in the keep
# directory; the same seed gives the same damage again.
import os
import random
import select
import signal
import socket
import subprocess
import sys
import threading
import time

import xdbqa

EMU = sys.argv[1]
KIND = sys.argv[2]
SEED0 = int(sys.argv[3])
ROUNDS = int(sys.argv[4])
rest = sys.argv[5:]
WORKERS = int(rest[0]) if rest and rest[0].isdigit() else 4
KEEP = [a[5:] for a in rest if a.startswith('keep=')]
KEEP = KEEP[0] if KEEP else '/tmp/xdb_dbfuzz_%s' % KIND
AIM = [a[4:] for a in rest if a.startswith('aim=')]
AIM = tuple(int(x) for x in AIM[0].split('-')) if AIM else None
LIMIT = 60          # seconds a round may take
IDLE = 1.5          # seconds without a byte, after the damage, until the line is closed
os.makedirs(KEEP, exist_ok=True)
T0 = xdbqa.Target(KIND, workdir=KEEP + '/main')
ver, vtext = T0.version()

PG_SCRIPT = """C
Q select 1::smallint a, 2 b, 3::bigint c, true d, 1.5::numeric(9,2) e, 2.5::float8 f, 1.5::real g, date '2026-01-31' h, time '13:45:10' i, now()::timestamp j, now() k, 'abc'::char(5) l, 'abc'::varchar(10) m, 'ab' || chr(8364) n, '6f9619ff-8b86-d011-b42d-00c04fc964ff'::uuid o, E'\\\\001\\\\002'::bytea p, repeat('x', 3000)::text q, interval '1 day' r, array[1,2,3] s, null::int t, 12345678901234567890.5::numeric u
Q select g as i, 'name ' || g as name, g::numeric / 7 as n, null::text as n1, case when g % 3 = 0 then null else g end as n2, now() + g * interval '1 second' as ts from generate_series(1, 300) g
Q select repeat('x', 20000) as big, repeat('y', 9000) as nbig
X create temporary table xdb_f (id serial primary key, v varchar(20), n numeric(9,2), d timestamp)
P a insert into xdb_f (v, n, d) values (?, ?, ?)
E a abc|@p9,2:12.5|@Z:2026-01-31-13.45.10.123456
E a n:|@p9,2:-1|@D:2026-01-31
E a @g:x|@f:1.5|n:
Q select * from xdb_f order by id
O c1 select id, v from xdb_f where id >= ? order by id ## i:1
F c1
K c1
Q select 1/0 as z
Q select 'x'::int as z
Q select * from no_such_table_xdb
X insert into xdb_f (id, v) values (1, 'dup')
Q select $$a ? b$$ as v, ? as p ## x
X create or replace function pg_temp.xdb_ff(a int, out total int) language plpgsql as $f$ begin total := a + 1; raise notice 'hello'; end $f$
Q select * from pg_temp.xdb_ff(5)
X create temporary table xdb_doc (id int, body bytea, note text)
X insert into xdb_doc values (1, decode(repeat('41', 30000), 'hex'), repeat(chr(232), 20000))
Q select id, put_blob_to_file(body, '@W@/out.bin') as b, put_clob_to_file(note, '@W@/out.txt') as c from xdb_doc
X insert into xdb_doc (id, body, note) values (2, get_blob_from_file('@W@/in.bin'), get_clob_from_file('@W@/in.txt'))
Q select id, length(body) as lb, length(note) as ln from xdb_doc order by id
Q XDB CONNECTION INFO
COMMIT
X update xdb_f set v = 'z' where id = 1
ROLLBACK
Q select count(*) as n from xdb_f
D
"""

MY_SCRIPT = """C
Q select cast(1 as signed) a, 2 b, 12345678901 c, cast(1.5 as decimal(9,2)) e, 2.5e0 f, date('2026-01-31') h, time('13:45:10') i, now() j, cast('abc' as char(5)) l, concat('ab', char(0xE282AC using utf8)) n, unhex('0102FF') p, repeat('x', 3000) q, null t, cast(12345678901234567890.5 as decimal(30,1)) u, b'101' v
Q select i, concat('name ', i) as name, cast(i as decimal(18,4)) / 7 as n, null as n1, if(i % 3 = 0, null, i) as n2, now() + interval i second as ts from xdb_fz_n where i <= 300 order by i
Q select repeat('x', 20000) as big, repeat('y', 9000) as nbig
X create temporary table xdb_f (id int auto_increment primary key, v varchar(20), n decimal(9,2), d datetime)
P a insert into xdb_f (v, n, d) values (?, ?, ?)
E a abc|@p9,2:12.5|@Z:2026-01-31-13.45.10.123456
E a n:|@p9,2:-1|@D:2026-01-31
E a @g:x|@f:1.5|n:
E a i:5|i:7|n:
Q select * from xdb_f order by id
O c1 select id, v from xdb_f where id >= ? order by id ## i:1
F c1
K c1
Q select 1 as z from no_such_table_xdb
Q select cast('x' as decimal(40,50)) as z
X insert into xdb_f (id, v) values (1, 'dup')
Q select 'a\\\\'b ?' as v, ? as p ## x
X drop procedure if exists xdb_fp@K@
X create procedure xdb_fp@K@(in a int, out t int) begin set t = a + 1; select a as x, 'r' as y; select 'second' as z; end
Q call xdb_fp@K@(1, @t)
X call xdb_fp@K@(2, @t)
Q select @t as t
Q show variables like 'version%'
X create temporary table xdb_doc (id int, body longblob, note longtext)
X insert into xdb_doc values (1, repeat(char(65), 30000), repeat(convert(char(232) using latin1), 20000))
Q select id, put_blob_to_file(body, '@W@/out.bin') as b, put_clob_to_file(note, '@W@/out.txt') as c from xdb_doc
X insert into xdb_doc (id, body, note) values (2, get_blob_from_file('@W@/in.bin'), get_clob_from_file('@W@/in.txt'))
Q select id, length(body) as lb, char_length(note) as ln from xdb_doc order by id
Q XDB CONNECTION INFO
COMMIT
X update xdb_f set v = 'z' where id = 1
ROLLBACK
Q select count(*) as n from xdb_f
X drop procedure if exists xdb_fp@K@
D
"""
SCRIPT = PG_SCRIPT if KIND == 'pg' else MY_SCRIPT

EDGE = [0x00, 0x01, 0x7F, 0x80, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0x54, 0x44, 0x45, 0x5A, 0x43, 0x31, 0x32, 0x6E]
PG_TYPES = b'RSKZTDCENA123ntsIGHWdcvV'

lock = threading.Lock()
stats = {'ok': 0, 'CRASH': 0, 'HANG': 0, 'damaged': 0, 'kinds': {}}


def frame_len(buf):
    """bytes of the first whole message of the server in buf, None when it is not all there yet"""
    if KIND == 'pg':
        if len(buf) < 5:
            return None
        ln = 1 + int.from_bytes(buf[1:5], 'big')
        ln = max(ln, 5)
    else:
        if len(buf) < 4:
            return None
        ln = 4 + int.from_bytes(buf[0:3], 'little')
    return ln if len(buf) >= ln else None


HDR = 5 if KIND == 'pg' else 4


def with_len(hdr, pay):
    """the header with the length of the payload it now has"""
    hdr = bytearray(hdr)
    if KIND == 'pg':
        hdr[1:5] = (len(pay) + 4).to_bytes(4, 'big')
    else:
        hdr[0:3] = min(len(pay), 0xFFFFFF).to_bytes(3, 'little')
    return bytes(hdr)


def damage(rng, pkt):
    """one message of the server (header and payload) -> (bytes to send, close after it, what was done)"""
    hdr, pay = bytearray(pkt[:HDR]), bytearray(pkt[HDR:])
    kind = rng.choice(['flip', 'flip', 'set', 'set', 'len', 'len', 'cut', 'cutclose', 'dup', 'drop', 'hdr', 'hdr', 'junk', 'close',
                       'zero', 'ones'])
    close = False
    n = len(pay)
    if kind in ('flip', 'set', 'len', 'cut', 'cutclose', 'dup', 'drop', 'zero', 'ones') and n == 0:
        kind = 'hdr'
    if kind == 'flip':
        for _ in range(rng.randint(1, 8)):
            pay[rng.randrange(n)] ^= 1 << rng.randrange(8)
    elif kind == 'set':
        for _ in range(rng.randint(1, 4)):
            pay[rng.randrange(n)] = rng.choice(EDGE)
    elif kind == 'len':
        i = rng.randrange(n)
        v = rng.choice([b'\xff\xff', b'\xff\xff\xff\xff', b'\x7f\xff\xff\xff', b'\x00\x00', b'\x80\x00', b'\xfe\xff\xff\xff\xff\xff\xff\xff\xff',
                        b'\xfd\xff\xff\xff', b'\xfc\xff\xff', b'\x00\x00\x00\x80', b'\xff\xff\xff\x7f', b'\x40\x00\x00\x00', b'\xfb'])
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
        what = rng.choice(['type', 'short', 'long', 'tiny', 'huge', 'neg'])
        kind = 'hdr-' + what
        if what == 'type':
            if KIND == 'pg':
                hdr[0] = rng.choice(PG_TYPES)
            else:
                hdr[3] = rng.choice([0, 1, 7, 255, (hdr[3] + 2) & 255])     # the sequence number
        else:
            ln = {'short': max(0, len(pay) - rng.randint(1, 40)), 'long': len(pay) + rng.randint(1, 4000), 'tiny': rng.randint(0, 3),
                  'huge': 0x3FFFFFFF if KIND == 'pg' else 0xFFFFFF, 'neg': 0x7FFFFFFF if KIND == 'pg' else 0xFFFFFE}[what]
            if KIND == 'pg':
                hdr[1:5] = ((ln + 4) & 0xFFFFFFFF if what != 'neg' else 0xFFFFFFF0).to_bytes(4, 'big')
            else:
                hdr[0:3] = min(ln, 0xFFFFFF).to_bytes(3, 'little')
            return bytes(hdr) + bytes(pay), False, kind
    return with_len(hdr, pay) + bytes(pay), close, kind


def relay(cli, rng, state, first):
    try:
        srv = socket.create_connection((T0.host, T0.port), timeout=10)
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
                while True:
                    ln = frame_len(buf)
                    if ln is None:
                        break
                    pkt, buf = buf[:ln], buf[ln:]
                    if target >= 0 and (seen == target or (again and seen == target + again)):
                        out, close, kind = damage(rng, pkt)
                        state['kinds'].append(kind)
                        state['bytes'].append('message %d (%s): the server sent %s\n  delivered %s%s' % (
                            seen, kind, pkt[:400].hex(), out[:400].hex(), ', then the connection was closed' if close else ''))
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


def proxy(lsock, rng, state):
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


def one_round(seed, k, npackets):
    wdir = '%s/w%d' % (KEEP, k)
    os.makedirs(wdir, exist_ok=True)
    rng = random.Random(seed)
    # the first messages (sign-on) and the many rows are both worth a look: half of the rounds aim at the first 60
    lim = max(1, npackets)
    tgt = rng.randrange(min(60, lim)) if rng.random() < 0.5 else rng.randrange(lim)
    if AIM:
        tgt = rng.randrange(lim * AIM[0] // 100, max(lim * AIM[0] // 100 + 1, lim * AIM[1] // 100))
    state = {'target': tgt if npackets else -1, 'again': rng.choice([0, 0, 0, 1, 2, 5]), 'kinds': [], 'bytes': [], 't0': time.time()}
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', 0))
    ls.listen(8)
    T = xdbqa.Target(KIND, workdir=wdir)
    drv = {'fetch_rows': rng.choice([1, 7, 50, 3000])} if rng.random() < 0.3 else {}
    T.profile(port=ls.getsockname()[1], driver=drv)
    if not os.path.exists(wdir + '/in.bin'):
        with open(wdir + '/in.bin', 'wb') as f:
            f.write(bytes(range(256)) * 300)
        with open(wdir + '/in.txt', 'w', encoding='utf-8') as f:
            f.write('una riga di testo con accenti: àèìòù €\n' * 900)
    th = threading.Thread(target=proxy, args=(ls, rng, state), daemon=True)
    th.start()
    env = dict(os.environ, XDB_EMU_JOBCCSID='1144', XDB_PRF_EMU=wdir + '/profile.emu', XDB_CONF='/dev/null', XDB_TEST_RDB=T.rdb,
               XDB_SH_MAXROWS='3', LD_LIBRARY_PATH=EMU,
               ASAN_OPTIONS=os.environ.get('XDB_FUZZ_ASAN', 'detect_leaks=0:exitcode=97:allocator_may_return_null=1:max_allocation_size_mb=512'),
               UBSAN_OPTIONS='halt_on_error=1:exitcode=96:print_stacktrace=1')
    p = subprocess.Popen([EMU + '/ardsh'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                         start_new_session=True)
    verdict = 'ok'
    try:
        out, _ = p.communicate(SCRIPT.replace('@W@', wdir).replace('@K@', str(k)).encode(), timeout=LIMIT)
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
        state['bigalloc'] = True
    state['rc'] = p.returncode
    return verdict, state, text


def worker(k, seeds, npackets):
    for seed in seeds:
        verdict, state, text = one_round(seed, k, npackets)
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
                    f.write('seed %d, message %d of %d, damage %s\n' % (seed, state['target'], npackets, state['kinds']))
                    f.write(text[-12000:])
            if verdict != 'ok' or state.get('noserver'):
                name = '%s/seed%d_%s.txt' % (KEEP, seed, verdict)
                with open(name, 'w') as f:
                    f.write('seed %d, message %d of %d, damage %s, exit code %s\n' % (seed, state['target'], npackets, state['kinds'], state.get('rc')))
                    f.write('\n'.join(state['bytes']) + '\n')
                    f.write(text[-12000:])
                print('%s seed %d (message %d, %s) -> %s' % (verdict, seed, state['target'], ','.join(state['kinds']), name), flush=True)


if KIND == 'mysql':
    # the numbers the script reads its rows from
    T0.sql('create table if not exists xdb_fz_n (i int not null primary key)')
    if T0.sql('select count(*) from xdb_fz_n')[0][0] < 300:
        T0.sql('delete from xdb_fz_n')
        T0.sql('insert into xdb_fz_n values ' + ','.join('(%d)' % i for i in range(1, 301)))
v, st, text = one_round(-1, 99, 0)
NP = st.get('packets', 0)
if v != 'ok' or NP < 10 or 'sqlcode -30080' in text or st.get('noserver'):
    print('the clean round did not work (%s, %d messages):' % (v, NP))
    print(text[-2500:])
    sys.exit(2)
name = 'PostgreSQL %d.%d' % ver if KIND == 'pg' else '%s %s' % ('MariaDB' if 'MariaDB' in vtext else 'MySQL', '.'.join(str(x) for x in ver))
print('%s, clean round: %d messages from the server, %d lines of output, %d errors the script asks for' % (
    name, NP, text.count('\n'), text.count('sqlcode -')), flush=True)
t0 = time.time()
seeds = list(range(SEED0, SEED0 + ROUNDS))
ths = [threading.Thread(target=worker, args=(k, seeds[k::WORKERS], NP)) for k in range(WORKERS)]
for t in ths:
    t.start()
for t in ths:
    t.join()
print('%s: %d rounds (seeds %d to %d), %d with damage delivered: %d ok, %d CRASH, %d HANG, in %d s' % (
    name, ROUNDS, SEED0, SEED0 + ROUNDS - 1, stats['damaged'], stats['ok'], stats['CRASH'], stats['HANG'], time.time() - t0))
print('rounds in which the driver connected again after the damage: %d; in which it asked for more than 512 MB at once: %d' % (
    stats.get('revived', 0), stats.get('bigalloc', 0)))
print('damage: ' + ', '.join('%s %d' % kv for kv in sorted(stats['kinds'].items())))
sys.exit(1 if stats['CRASH'] or stats['HANG'] else 0)

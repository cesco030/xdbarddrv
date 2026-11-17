#!/bin/sh
# Linux test of the file functions with the MySQL backend (xdb/xdblob.c, xdb/xdbmy.c): values
# between stream files and the server, through the emulation of the driver.
#   usage: sh lobtest_my.sh <emu dir with ardsh>     needs python3 and the MySQL of the other
#   tests (127.0.0.1:3306, database xdbtest, user xdbn, password xdbpw) with max_allowed_packet
#   at its default of 64 MB.  Compare with lobtest_my.expected.
# A file travels as the value of a parameter, in pieces of 1 MB; a file column is written while
# its row is read from the line.  The server limits a value to max_allowed_packet.
# Work files: /tmp/xdb_lobm.  XDB_LOBT_TRACE=Y writes the trace of the driver to /tmp/xdb_LOBMY.log.
E=$1
D=/tmp/xdb_lobm
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_TEST_RDB=LOBMY XDB_CONF=/dev/null XDB_SH_MAXROWS=20
export XDB_EMU_JOBLOG=1		# what the driver would write to the job log comes as "JOBLOG ..."
{
  echo "P|LOBMY|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*OFF|10|0|1000|6|${XDB_LOBT_TRACE:-N}"
  echo "W|LOBMY|xdbpw"
} > $P
python3 - $D <<'PY'
import random, sys, os
d = sys.argv[1]
random.seed(20261115)
def bin(name, n):
    open(os.path.join(d, name), 'wb').write(random.randbytes(n))
bin('small.bin', 1000)
bin('piece.bin', 1024 * 1024)       # exactly one piece
bin('over.bin', 1024 * 1024 + 1)    # one byte more
bin('big.bin', 20 * 1024 * 1024 + 123)  # the row that brings it back is longer than one packet (16 MB)
open(os.path.join(d, 'empty.bin'), 'wb').close()
with open(os.path.join(d, 'toobig.bin'), 'wb') as f:
    f.truncate(68 * 1024 * 1024)    # more than max_allowed_packet
line = 'Riga àèìòù € ñ ü — 日本語 Ω 😀 𝄞\n'
open(os.path.join(d, 'small.txt'), 'w', encoding='utf-8').write(line * 3)
open(os.path.join(d, 'big.txt'), 'w', encoding='utf-8').write(line * 60000)
open(os.path.join(d, 'latin.txt'), 'w', encoding='utf-8').write('Città è perché\n' * 4)
open(os.path.join(d, 'bad.txt'), 'wb').write(b'abc\xff\xfedef')
open(os.path.join(d, 'cut.txt'), 'wb').write('perch'.encode() + b'\xc3')
for f in os.listdir(d):
    if f.startswith('out') or f.startswith('app'):
        os.remove(os.path.join(d, f))
os.makedirs(os.path.join(d, 'dir.bin'), exist_ok=True)
PY
run() { # title, then the requests on standard input
  echo "== $1"
  $E/ardsh 2>&1 | grep -E "^(>|AR|JOBLOG|  row|  col|  rows|  total)" | cut -c1-170
}
same() { if cmp -s "$D/$1" "$D/$2"; then echo "   $2 = $1"; else echo "   $2 DIFFERS from $1"; fi; }
md5() { echo "   md5 of $1: $(md5sum < $D/$1 | cut -c1-32)"; }

run "values from files, then back to files" <<EOF2
ISO *CHG
C
X create temporary table xdb_doc (id int primary key, body longblob, note longtext)
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/small.bin'))
P s1 insert into xdb_doc (id, body, note) values(?, get_blob_from_file(?), get_clob_from_file(?))
E s1 i:2|$D/piece.bin|$D/small.txt
E s1 i:3|$D/over.bin|$D/big.txt
E s1 i:4|$D/big.bin|$D/latin.txt
E s1 i:5|$D/empty.bin|$D/empty.bin
Q select id, length(body) as bytes, md5(body) as body_md5, char_length(note) as chars, md5(note) as note_md5 from xdb_doc order by id
Q select id, put_blob_to_file(body, concat('$D/out', id, '.bin')), put_clob_to_file(note, concat('$D/out', id, '.txt')) as text_bytes from xdb_doc order by id
ROLLBACK
D
EOF2
md5 small.bin; md5 piece.bin; md5 over.bin; md5 big.bin; md5 small.txt; md5 big.txt; md5 latin.txt
same small.bin out1.bin; same piece.bin out2.bin; same over.bin out3.bin; same big.bin out4.bin; same empty.bin out5.bin
same small.txt out2.txt; same big.txt out3.txt; same latin.txt out4.txt; same empty.bin out5.txt
ls $D/out1.txt 2>/dev/null || echo "   out1.txt was not written (the value is null)"

run "file options, CCSID of a text file, name in a host variable, a long column beside it" <<EOF2
ISO *CHG
C
X create temporary table xdb_doc (id int primary key, body longblob, note longtext)
X insert into xdb_doc values(1, get_blob_from_file('$D/over.bin'), get_clob_from_file('$D/latin.txt'))
O c1 select put_blob_to_file(body, ?, 8) as new_file, repeat('x', 3000) as long_text from xdb_doc ## $D/out_new.bin
F c1
K c1
O c1 select put_blob_to_file(body, ?, 8) as new_file from xdb_doc ## $D/out_new.bin
F c1
K c1
Q select put_clob_to_file(note, '$D/app.txt', 32) as first, put_clob_to_file(note, '$D/app2.txt', 32, 1252) as cp1252 from xdb_doc
Q select put_clob_to_file(note, '$D/app.txt', 32) as second, put_clob_to_file(note, '$D/app2.txt', 32, 1252) as cp1252 from xdb_doc
Q select put_clob_to_file(concat(note, repeat('è', 70000)), '$D/out_euro.txt', 16, 1252) as long_cp1252 from xdb_doc
Q select put_blob_to_file(?, concat(?, '.bin')) as from_markers, ? as third ## bytes of a host variable|$D/out_marker|3
ROLLBACK
D
EOF2
same over.bin out_new.bin
echo "   app.txt has $(wc -c < $D/app.txt) bytes, app2.txt $(wc -c < $D/app2.txt), out_euro.txt $(wc -c < $D/out_euro.txt)"
echo "   app2.txt as CP1252: $(iconv -f CP1252 -t UTF-8 $D/app2.txt | head -1)"
echo "   out_marker.bin: $(cat $D/out_marker.bin)"

run "more shapes: markers after the file, many rows, an aggregate, the way of interactive SQL, no commitment control" <<EOF2
ISO *CHG
C
X create temporary table xdb_doc (id int primary key, body longblob, note longtext)
X insert into xdb_doc (id) values(1)
X insert into xdb_doc (id) values(2)
P s1 update xdb_doc set body = get_blob_from_file(?), note = ? where id = ?
E s1 $D/over.bin|first|i:1
E s1 $D/small.bin|second|i:2
Q select id, length(body) as bytes, note from xdb_doc order by id
Q with recursive g(i) as (select 1 union all select i + 1 from g where i < 300) select put_clob_to_file(concat(i, char(10)), '$D/app_many.txt', 32) as n, repeat('y', 900) as filler from g
Q select put_blob_to_file(max(body), '$D/out_agg.bin') as agg_bytes, count(*) as n from xdb_doc
R select id, put_blob_to_file(body, concat('$D/out r ', id, ' è.bin')) as "bytes written" from xdb_doc order by id
ROLLBACK
D
ISO *NONE
C
X create temporary table xdb_doc2 (id int primary key, body longblob)
X insert into xdb_doc2 (id, body) values(1, get_blob_from_file('$D/over.bin'))
Q select put_blob_to_file(body, '$D/out_none.bin') as without_commit from xdb_doc2
X drop temporary table xdb_doc2
D
EOF2
same over.bin "out r 1 è.bin"; same small.bin "out r 2 è.bin"; same over.bin out_none.bin
echo "   app_many.txt: $(wc -l < $D/app_many.txt) lines, in order: $(if seq 1 300 | cmp -s - $D/app_many.txt; then echo yes; else echo NO; fi)"
echo "   out_agg.bin: $(wc -c < $D/out_agg.bin) bytes"

# a row is read from the line in packets of 16 MB - 1; a row of just that length ends with an empty one
N1=$((16777215 - 6 - ${#D} - 15)); N2=$((33554430 - 11 - ${#D} - 15))
run "rows of exactly one and two packets, one byte less and one more" <<EOF2
ISO *CHG
C
Q select put_blob_to_file(repeat('x', $((N1 - 1))), '$D/out_p1a.bin') as b, 'after' as next_column
Q select put_blob_to_file(repeat('x', $N1), '$D/out_p1b.bin') as b, 'after' as next_column
Q select put_blob_to_file(repeat('x', $((N1 + 1))), '$D/out_p1c.bin') as b, 'after' as next_column
Q select put_blob_to_file(repeat('x', $((N2 - 1))), '$D/out_p2a.bin') as b, 'after' as next_column
Q select put_blob_to_file(repeat('x', $N2), '$D/out_p2b.bin') as b, 'after' as next_column
Q select put_blob_to_file(repeat('x', $((N2 + 1))), '$D/out_p2c.bin') as b, 'after' as next_column
ROLLBACK
D
EOF2
python3 - $D <<'PY'
import sys, os
for f in ('out_p1a.bin', 'out_p1b.bin', 'out_p1c.bin', 'out_p2a.bin', 'out_p2b.bin', 'out_p2c.bin'):
    d = open(os.path.join(sys.argv[1], f), 'rb').read()
    print('   %s: only x: %s' % (f, set(d) == {120}))
PY

run "what goes wrong: files" <<EOF2
ISO *CHG
C
X create temporary table xdb_doc (id int primary key, body longblob, note longtext)
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/nofile.bin'))
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/dir.bin'))
X insert into xdb_doc (id, body) values(1, get_blob_from_file(''))
P s1 insert into xdb_doc (id, body) values(?, get_blob_from_file(?))
E s1 i:1|n:
E s1 i:1
X insert into xdb_doc (id, note) values(8, get_clob_from_file('$D/bad.txt'))
X insert into xdb_doc (id, note) values(9, get_clob_from_file('$D/cut.txt'))
E s1 i:1|$D/over.bin
E s1 i:1|$D/small.bin
E s1 i:2|$D/small.bin
Q select id, length(body) as bytes, md5(body) as body_md5 from xdb_doc order by id
Q select put_blob_to_file(body, '$D/nodir/x.bin') from xdb_doc where id = 1
Q select put_blob_to_file(body, '$D/dir.bin') from xdb_doc where id = 1
Q select put_blob_to_file(body, null) from xdb_doc where id = 1
Q select put_blob_to_file(body, repeat('a', 2000)) from xdb_doc where id = 1
Q select put_blob_to_file(body, '$D/out_x.bin', 8), put_blob_to_file(body, '$D/nodir/y.bin') from xdb_doc where id = 1
O c1 select id, put_blob_to_file(body, concat('$D/out_y', id, '.bin'), 8) as new_file from xdb_doc order by id
F c1
K c1
O c1 select id, put_blob_to_file(body, concat('$D/out_y', 3 - id, '.bin'), 8) as new_file from xdb_doc order by id
F c1
F c1
K c1
ROLLBACK
D
EOF2
md5 small.bin
same over.bin out_x.bin
echo "   (out_x.bin is the first column of a row whose second column failed: complete, so it stays)"
same over.bin out_y1.bin; same small.bin out_y2.bin
echo "   (the second cursor fails at its first row, because out_y2.bin exists; a failed fetch ends a cursor)"

run "what goes wrong: the limit of the server, the statement" <<EOF2
ISO *CHG
C
X create temporary table xdb_doc (id int primary key, body longblob, note longtext)
P s1 insert into xdb_doc (id, body) values(?, get_blob_from_file(?))
E s1 i:1|$D/toobig.bin
E s1 i:1|$D/small.bin
Q select id, length(body) as bytes from xdb_doc
Q select put_blob_to_file(repeat('x', 70000000), '$D/out_big1.bin') as too_big
Q select put_blob_to_file(repeat('x', 67108860), '$D/out_big2.bin') as too_big_with_its_name
Q select put_blob_to_file(body, '$D/out_after.bin') as still_works from xdb_doc
X select put_clob_to_file(note, '$D/out_i.txt') from xdb_doc
Q select id from xdb_doc where body = get_blob_from_file('$D/small.bin')
Q select put_blob_to_file(body, '$D/out_u.bin') from xdb_doc union all select body from xdb_doc
Q select id, put_blob_to_file(body, '$D/out_o.bin') as b from xdb_doc order by 2
ROLLBACK
D
EOF2
same small.bin out_after.bin
ls $D/out_big1.bin $D/out_big2.bin $D/out_i.txt $D/out_u.bin 2>/dev/null | sed 's/^/   left behind: /'
echo "   files in $D beginning with out: $(ls $D | grep -c '^out')"

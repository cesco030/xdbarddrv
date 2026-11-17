# QSH script: test of the file functions of the driver on IBM i with a MySQL, a SQL Server or
# a PostgreSQL database (XDBT32).
# usage: sh x32.sh <kitdir> <lib> <rdb> <MYSQL|MSSQL|PG> [<database> [<MB>]]
#   database: only for MySQL, when the connection has no database of its own: where the
#   temporary table is kept.  Left out or '', the test takes the first database of the server
#   that lets the user create a temporary table, and says which.
#   MB: size of the large file, 3 by default (the file has that many times 1048577 bytes).
#   The server holds it in the temporary table and once more while it is fetched; MySQL
#   takes no value longer than its max_allowed_packet.  On PostgreSQL a value above option
#   lob_direct (4 MB) is written to and deleted from the large objects of the server on
#   its way in and again on its way out.  Above 16 MB the large file and its copy are
#   removed here at the end.
#   On the server only a temporary table is used and the unit of work is rolled back.
#   Files: /tmp/xdbt32_<rdb> (two files of random bytes are made there with the dd of PASE,
#   the test writes the others).
#   result: /tmp/xdbt32_<rdb>.out    log: <kitdir>/out/x32.log
#   Read the result: every file written back must be equal to the one it came from, the
#   checksums of the server must be those worked out here, and the text count must be 2.
K=$1; LIB=$2; RDB=$3; KIND=$4; DB=$5; MB=${6:-3}
mkdir -p $K/out
L=$K/out/x32.log
T=/tmp/xdbt32_$RDB
O=$T.out
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo "start $RDB $KIND" > $L
rm -f $O
mkdir -p $T
for f in in_small.bin in_big.bin out_text.txt out_280.txt back_1.bin back_2.bin back_3.bin back_11.txt back_12.txt a.tmp b.tmp; do rm -f $T/$f; done
/QOpenSys/usr/bin/dd if=/dev/urandom of=$T/in_small.bin bs=1000 count=1 > /dev/null 2>&1
/QOpenSys/usr/bin/dd if=/dev/urandom of=$T/in_big.bin bs=1048577 count=$MB > /dev/null 2>&1
if ! test -s $T/in_big.bin; then echo "THE INPUT FILES COULD NOT BE MADE (dd of PASE, /dev/urandom)" >> $L; echo end >> $L; exit 1; fi
if ! system "CRTSQLRPGI OBJ($LIB/XDBT32) SRCSTMF('$K/test/xdbt32.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB test: file functions, three kinds of server')" > $K/out/XDBT32.lst 2>&1; then
  echo "XDBT32 COMPILE FAILED (see out/XDBT32.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] " $K/out/XDBT32.lst | head -25 >> $L
  echo end >> $L; exit 1
fi
echo "XDBT32 compiled" >> $L
system "CALL PGM($LIB/XDBT32) PARM('$RDB' '$KIND' '$T' '$O' '$DB')" 2>&1 | grep -E "$MSG|XDB " | head -40 >> $L
for pair in in_small.bin:back_1.bin in_big.bin:back_2.bin in_small.bin:back_3.bin out_text.txt:back_11.txt; do
  x=${pair%%:*}; y=${pair##*:}
  if cmp -s $T/$x $T/$y; then echo "   $y = $x" >> $O; else echo "   $y DIFFERS from $x" >> $O; fi
done
# the file of CCSID 280 and its copy in UTF-8: both through grep, which reads each in its own CCSID
grep "" $T/out_280.txt > $T/a.tmp
grep "" $T/back_12.txt > $T/b.tmp
if test -s $T/a.tmp && cmp -s $T/a.tmp $T/b.tmp; then echo "   back_12.txt = out_280.txt (as text)" >> $O; else echo "   back_12.txt DIFFERS from out_280.txt (as text)" >> $O; fi
for f in in_small.bin in_big.bin out_text.txt out_280.txt back_1.bin back_2.bin back_3.bin back_11.txt back_12.txt; do
  if test -e $T/$f; then echo "   $f: $(ls -l $T/$f | awk '{print $5}') bytes, CCSID $(attr $T/$f CCSID)" >> $O; else echo "   $f: not there" >> $O; fi
done
if test -e $T/nodir; then echo "   nodir EXISTS" >> $O; fi
if test $MB -gt 16; then rm -f $T/in_big.bin $T/back_2.bin; echo "   in_big.bin and back_2.bin removed after the test" >> $O; fi
echo end >> $L

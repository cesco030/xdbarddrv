# QSH script: test of the file functions of the driver on IBM i (XDBT30): large values between
# stream files and a PostgreSQL database.
# usage: sh x30.sh <kitdir> <lib> <rdb>
#   On the server only a temporary table is used and the unit of work is rolled back.  Values
#   larger than option lob_direct pass through large objects of the server, which the driver
#   removes again in the same unit of work.
#   Pass 1 runs with the profile as it is (a value of 4 MB travels in one piece).  For pass 2
#   the profile <rdb> of <lib> gets option lob_direct = 64 for the time of the test, so that
#   values of a few hundred KB take the staged way; the option is taken away again at the end,
#   whatever happens.  A profile that has the option already is left alone: pass 2 is skipped.
#   Files: /tmp/xdbt30/p1 and /tmp/xdbt30/p2.   result: /tmp/xdbt30.out   log: <kitdir>/out/x30.log
#   The result is compared with test/xdbt30.expected, without the two lines that count the
#   large objects of the server; the log says whether it is the same.  The message texts in
#   it are those of an English job.
K=$1; LIB=$2; RDB=$3
mkdir -p $K/out
L=$K/out/x30.log
O=/tmp/xdbt30.out
T=/tmp/xdbt30
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo start > $L
rm -f $O
if ! system "CRTSQLRPGI OBJ($LIB/XDBT30) SRCSTMF('$K/test/xdbt30.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB test: values between files and the server')" > $K/out/XDBT30.lst 2>&1; then
  echo "XDBT30 COMPILE FAILED (see out/XDBT30.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] " $K/out/XDBT30.lst | head -25 >> $L
  echo end >> $L; exit 1
fi
echo "XDBT30 compiled" >> $L
pass() { # directory, rows of the large value, title
  mkdir -p $T/$1
  for f in a.bin b.bin c1.bin c2.bin new.bin null.bin t1208.txt t280.txt t1252.txt u1208.txt app.txt; do rm -f $T/$1/$f; done
  echo "== $3" >> $O
  echo "-- $3" >> $L
  system "CALL PGM($LIB/XDBT30) PARM('$RDB' '$T/$1' '$2' '$O')" 2>&1 | grep -E "$MSG|XDB " | head -40 >> $L
  for pair in a.bin:c1.bin b.bin:c2.bin a.bin:new.bin; do
    x=${pair%%:*}; y=${pair##*:}
    if cmp -s $T/$1/$x $T/$1/$y; then echo "   $y = $x" >> $O; else echo "   $y DIFFERS from $x" >> $O; fi
  done
  for f in a.bin b.bin t1208.txt t280.txt t1252.txt u1208.txt app.txt; do
    echo "   $f: $(ls -l $T/$1/$f | awk '{print $5}') bytes, CCSID $(attr $T/$1/$f CCSID)" >> $O
  done
  if test -e $T/$1/null.bin; then echo "   null.bin EXISTS" >> $O; else echo "   null.bin was not written" >> $O; fi
}
sql() { system "RUNSQL SQL('$1') COMMIT(*NONE) NAMING(*SQL)" >> $L 2>&1; }
pass p1 250000 "pass 1: profile as it is"
W="where PORDB = ''$RDB'' and POKEY = ''lob_direct''"
if sql "insert into $LIB.XDBPRO (PORDB, POKEY, POKIND, POVAL, POCHGU, POCHGT) values(''$RDB'', ''lob_direct'', ''D'', ''64'', user, current timestamp)"; then
  echo "option lob_direct = 64 added" >> $L
  pass p2 200000 "pass 2: lob_direct = 64, the large values are staged"
  sql "delete from $LIB.XDBPRO $W" && echo "option removed" >> $L || echo "OPTION NOT REMOVED: delete it with WRKXDBPRF option 7" >> $L
else
  echo "the profile has option lob_direct already (or the insert failed): pass 2 not run" >> $L
  echo "== pass 2 not run: the profile has option lob_direct already" >> $O
fi
# both through grep, which reads each file in its own CCSID
grep -v "large objects on the server" $O > $T/got.txt
grep "" $K/test/xdbt30.expected > $T/want.txt
if cmp -s $T/got.txt $T/want.txt; then
  echo "result as expected (test/xdbt30.expected)" >> $L
else
  echo "RESULT DIFFERS from test/xdbt30.expected: see $O" >> $L
fi
echo end >> $L

# QSH script: sample and test XDBINFO - prints what the statement XDB CONNECTION INFO tells
# about a connection: where it leads, the server, the session, the driver's counters, settings
# and limits.  Nothing is made or changed on the server (one "select 1" is run).
# usage: sh xinfo.sh <kitdir> <lib> <rdb> [<rdb> ...]
#   Each name is a connection profile with its RDB directory entry.
#   result: /tmp/xdbinfo_<rdb>.out for each name    log: <kitdir>/out/xinfo.log
#   The program XDBINFO stays in <lib>.
K=$1; LIB=$2; shift 2
mkdir -p $K/out
L=$K/out/xinfo.log
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo "start $(date)" > $L
if system "CRTSQLRPGI OBJ($LIB/XDBINFO) SRCSTMF('$K/test/xdbinfo.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB sample: what a connection is')" > $K/out/XDBINFO.lst 2>&1; then
  echo "XDBINFO compiled" >> $L
  for RDB in "$@"; do
    O=/tmp/xdbinfo_$RDB.out
    rm -f $O
    echo "--- $RDB" >> $L
    system "CALL PGM($LIB/XDBINFO) PARM('$RDB' '$O')" 2>&1 | grep -E "$MSG|XDB " | head -20 >> $L
    if test -s $O; then echo "$O written, $(wc -l < $O) lines" >> $L; else echo "NO OUTPUT for $RDB" >> $L; fi
  done
else
  echo "XDBINFO COMPILE FAILED (see out/XDBINFO.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] |MSG ID|$MSG" $K/out/XDBINFO.lst | head -40 >> $L
fi
echo "end $(date)" >> $L

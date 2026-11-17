# QSH script: builds module XDBALW and its batch test XDBT25 in <lib> and runs the test.
# usage: sh x25.sh <kitdir> <lib>     result: /tmp/xdbt25.out   log: <kitdir>/out/x25.log
K=$1; LIB=$2
mkdir -p $K/out
L=$K/out/x25.log
echo start > $L
S=/QSYS.LIB/$LIB.LIB/QXDBSRC.FILE
st() { if system "$2" > $K/out/$1.lst 2>&1; then echo "$1 OK" >> $L; rm -f $K/out/$1.lst; return 0; fi
  echo "$1 FAILED (see out/$1.lst)" >> $L
  grep -E "RNF[0-9]+ +[234]0|SQL[0-9]+ +[234]0|CPD[0-9A-F]{4}|CPF[0-9A-F]{4}|MCH[0-9]{4}|RNS9" $K/out/$1.lst | head -40 >> $L
  return 1; }
liblist -a $LIB > /dev/null 2>&1
st SRC_XDBALWH "CPYFRMSTMF FROMSTMF('$K/prf/xdbalwh.rpgle') TOMBR('$S/XDBALWH.MBR') MBROPT(*REPLACE)" &&
system "CHGPFM FILE($LIB/QXDBSRC) MBR(XDBALWH) SRCTYPE(RPGLE) TEXT('XDB allowed users: prototypes')" > /dev/null 2>&1
st SRC_XDBALW "CPYFRMSTMF FROMSTMF('$K/prf/xdbalw.rpgle') TOMBR('$S/XDBALW.MBR') MBROPT(*REPLACE)" &&
system "CHGPFM FILE($LIB/QXDBSRC) MBR(XDBALW) SRCTYPE(RPGLE) TEXT('XDB allowed users: rules')" > /dev/null 2>&1
st XDBALW_MOD "CRTRPGMOD MODULE($LIB/XDBALW) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBALW) DBGVIEW(*SOURCE) REPLACE(*YES)" &&
st XDBT25_MOD "CRTSQLRPGI OBJ($LIB/XDBT25) SRCSTMF('$K/test/xdbt25.sqlrpgle') OBJTYPE(*MODULE) COMMIT(*NONE) OPTION(*SQL) DBGVIEW(*NONE) REPLACE(*YES)" &&
st XDBT25 "CRTPGM PGM($LIB/XDBT25) MODULE($LIB/XDBT25 $LIB/XDBALW) ACTGRP(*NEW) TEXT('XDB test: rules of the allowed users')" || { echo end >> $L; exit 1; }
system "DLTMOD MODULE($LIB/XDBT25)" > /dev/null 2>&1
rm -f /tmp/xdbt25.out
system "CALL PGM($LIB/XDBT25) PARM('/tmp/xdbt25.out')" > $K/out/XDBT25.run 2>&1; echo "XDBT25 run rc=$?" >> $L
grep -E "CPF[0-9A-F]{4}|MCH[0-9]{4}|RNX[0-9]{4}|RNQ[0-9]{4}|SQL[0-9]{4}" $K/out/XDBT25.run | grep -v CPF9801 | head -12 >> $L
echo end >> $L

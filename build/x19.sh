# QSH script: builds module XDBSETUP and the batch driver XDBT19 in <lib>, then runs the setup checks
# on <checklib>.   usage: sh x19.sh <kitdir> <lib> <checklib> <owner or -> <mode C|A|O> [nobuild]
K=$1; LIB=$2; CL=$3; OWN=$4; MODE=$5
if test "$OWN" = "-"; then OWN=" "; fi
mkdir -p $K/out
L=$K/out/x19.log
echo start > $L
S=/QSYS.LIB/$LIB.LIB/QXDBSRC.FILE
st() { if system "$2" > $K/out/$1.lst 2>&1; then echo "$1 OK" >> $L; rm -f $K/out/$1.lst; return 0; fi
  echo "$1 FAILED (see out/$1.lst)" >> $L
  grep -E "RNF[0-9]+ +[234]0|SQL[0-9]+ +[234]0|CPD[0-9A-F]{4}|CPF[0-9A-F]{4}|MCH[0-9]{4}|RNS9" $K/out/$1.lst | head -40 >> $L
  return 1; }
if test -z "$6"; then
  liblist -a $LIB > /dev/null 2>&1
  st SRC_XDBSETUPH "CPYFRMSTMF FROMSTMF('$K/prf/xdbsetuph.rpgle') TOMBR('$S/XDBSETUPH.MBR') MBROPT(*REPLACE)" &&
  system "CHGPFM FILE($LIB/QXDBSRC) MBR(XDBSETUPH) SRCTYPE(RPGLE) TEXT('XDB setup: prototypes')" > /dev/null 2>&1
  st SRC_XDBSETUP "CPYFRMSTMF FROMSTMF('$K/prf/xdbsetup.rpgle') TOMBR('$S/XDBSETUP.MBR') MBROPT(*REPLACE)" &&
  system "CHGPFM FILE($LIB/QXDBSRC) MBR(XDBSETUP) SRCTYPE(RPGLE) TEXT('XDB setup: checks and repairs')" > /dev/null 2>&1
  st XDBSETUP_MOD "CRTRPGMOD MODULE($LIB/XDBSETUP) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBSETUP) DBGVIEW(*SOURCE) REPLACE(*YES)" &&
  st XDBT19_MOD "CRTSQLRPGI OBJ($LIB/XDBT19) SRCSTMF('$K/test/xdbt19.sqlrpgle') OBJTYPE(*MODULE) COMMIT(*NONE) OPTION(*SQL) DBGVIEW(*NONE) REPLACE(*YES)" &&
  st XDBT19 "CRTPGM PGM($LIB/XDBT19) MODULE($LIB/XDBT19 $LIB/XDBSETUP) ACTGRP(*NEW) TEXT('XDB test: setup checks in batch')" || { echo end >> $L; exit 1; }
fi
rm -f /tmp/xdbt19.out
system "CALL PGM($LIB/XDBT19) PARM('$CL' '$OWN' '$MODE' '/tmp/xdbt19.out')" > $K/out/XDBT19.run 2>&1; echo "XDBT19 run rc=$?" >> $L
grep -E "CPF[0-9A-F]{4}|MCH[0-9]{4}|RNX[0-9]{4}|RNQ[0-9]{4}|SQL[0-9]{4}" $K/out/XDBT19.run | head -12 >> $L
echo end >> $L

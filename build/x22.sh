# QSH script: builds XDBT22 (test of the allowed users check) in <lib> and runs it.
# usage: sh x22.sh <kitdir> <lib> [<user profile> ...]
#   the user profiles are only read: their names are written to /tmp/xdbt22.out to compare the
#   reading of group and supplemental groups with DSPUSRPRF.   log: <kitdir>/out/x22.log
K=$1; LIB=$2
mkdir -p $K/out
L=$K/out/x22.log
echo start > $L
st() { if system "$2" > $K/out/$1.lst 2>&1; then echo "$1 OK" >> $L; rm -f $K/out/$1.lst; return 0; fi
  echo "$1 FAILED (see out/$1.lst)" >> $L
  grep -E "CZM[0-9]{4}|CPD[0-9A-F]{4}|CPF[0-9A-F]{4}|MCH[0-9]{4}" $K/out/$1.lst | head -30 >> $L
  return 1; }
st XDBPRFC_T "CRTCMOD MODULE($LIB/XDBT22P) SRCSTMF('$K/xdb/xdbprfc.c') TGTCCSID(*JOB) INCDIR('$K/xdb') DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) REPLACE(*YES)" &&
st XDBT22_MOD "CRTCMOD MODULE($LIB/XDBT22) SRCSTMF('$K/test/xdbt22.c') TGTCCSID(*JOB) SYSIFCOPT(*IFSIO) INCDIR('$K/xdb') DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) REPLACE(*YES)" &&
st XDBT22 "CRTPGM PGM($LIB/XDBT22) MODULE($LIB/XDBT22 $LIB/XDBT22P) ACTGRP(*NEW) TEXT('XDB test: allowed users')" || { echo end >> $L; exit 1; }
system "DLTMOD MODULE($LIB/XDBT22)" > /dev/null 2>&1
system "DLTMOD MODULE($LIB/XDBT22P)" > /dev/null 2>&1
P="'/tmp/xdbt22.out'"
shift; shift
for u in "$@"; do P="$P '$u'"; done
rm -f /tmp/xdbt22.out
system "CALL PGM($LIB/XDBT22) PARM($P)" > $K/out/XDBT22.run 2>&1; echo "XDBT22 run rc=$?" >> $L
grep -E "CPF[0-9A-F]{4}|MCH[0-9]{4}|CEE[0-9]{4}" $K/out/XDBT22.run | head -12 >> $L
echo end >> $L

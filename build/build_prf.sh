# QSH script: creates the objects of the XDB connection profiles in <lib>.
#   source file QXDBSRC (members copied from <kitdir>/prf), files XDBPRF XDBPRO XDBKWD,
#   validation list XDBVLDL, display file XDBPRFD, programs XDBPRFM XDBPRFT (C sources stay in
#   the kit), command WRKXDBPRF with its help panel group (build_hlp.sh).
# Existing XDBPRF, XDBPRO and XDBVLDL are kept: profiles and passwords are never dropped.
# usage: sh build_prf.sh <kitdir> <lib> [<ref>]   log: <kitdir>/out/prf.log, listings <kitdir>/out/<name>.lst
# <ref>: see build_hlp.sh (library the command names; *LIBL for a package)
K=$1; LIB=$2; R=$3
mkdir -p $K/out
L=$K/out/prf.log
echo start > $L
liblist -a $LIB > /dev/null 2>&1
S=/QSYS.LIB/$LIB.LIB/QXDBSRC.FILE
exists() { system "CHKOBJ OBJ($LIB/$1) OBJTYPE($2)" > /dev/null 2>&1; }
step() { # name, command: listing in out/<name>.lst, the error lines of a failure in the log
  if system "$2" > $K/out/$1.lst 2>&1; then
    echo "$1 OK" >> $L; rm -f $K/out/$1.lst; return 0
  fi
  echo "$1 FAILED (see out/$1.lst)" >> $L
  grep -E "CPD[0-9A-F]|CPF[0-9A-F]|RNF[0-9]+ +[234]0|RNS9|SQL[0-9]+ +[234]0|CZM[0-9]|CZS[0-9]|SQL[0-9]+:|MCH[0-9]" $K/out/$1.lst | head -30 >> $L
  return 1
}
member() { # member, source type, stream file, text
  step "SRC_$1" "CPYFRMSTMF FROMSTMF('$K/prf/$3') TOMBR('$S/$1.MBR') MBROPT(*REPLACE)" &&
    system "CHGPFM FILE($LIB/QXDBSRC) MBR($1) SRCTYPE($2) TEXT('$4')" > /dev/null 2>&1
}

exists QXDBSRC '*FILE' || step QXDBSRC "CRTSRCPF FILE($LIB/QXDBSRC) RCDLEN(112) TEXT('XDB - sources of the connection profiles')"
member XDBPRF    PF       xdbprf.pf        'XDB connection profiles'
member XDBPRO    PF       xdbpro.pf        'XDB profile options'
member XDBKWD    PF       xdbkwd.pf        'XDB option keywords'
member XDBPRFD   DSPF     xdbprfd.dspf     'XDB work with connection profiles'
member XDBPRFM   RPGLE    xdbprfm.rpgle    'XDB work with connection profiles'
member XDBSETUPH RPGLE    xdbsetuph.rpgle  'XDB setup: prototypes'
member XDBSETUP  RPGLE    xdbsetup.rpgle   'XDB setup: checks and repairs'
member XDBALWH   RPGLE    xdbalwh.rpgle    'XDB allowed users: prototypes'
member XDBALW    RPGLE    xdbalw.rpgle     'XDB allowed users: rules'
member WRKXDBPRF CMD      wrkxdbprf.cmd    'Work with XDB connection profiles'

exists XDBPRF '*FILE' && echo "XDBPRF kept" >> $L ||
  step XDBPRF "CRTPF FILE($LIB/XDBPRF) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBPRF) AUT(*USE)"
exists XDBPRO '*FILE' && echo "XDBPRO kept" >> $L ||
  step XDBPRO "CRTPF FILE($LIB/XDBPRO) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBPRO) AUT(*USE)"
exists XDBVLDL '*VLDL' && echo "XDBVLDL kept" >> $L ||
  step XDBVLDL "CRTVLDL VLDL($LIB/XDBVLDL) AUT(*EXCLUDE) TEXT('XDB - passwords of the connection profiles')"
exists XDBKWD '*FILE' && system "DLTF FILE($LIB/XDBKWD)" > /dev/null 2>&1
step XDBKWD "CRTPF FILE($LIB/XDBKWD) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBKWD) AUT(*USE)" &&
  step XDBKWD_DATA "RUNSQLSTM SRCSTMF('$K/prf/xdbkwd.sql') COMMIT(*NONE) NAMING(*SQL) DFTRDBCOL($LIB)"

step XDBPRFD "CRTDSPF FILE($LIB/XDBPRFD) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBPRFD) RSTDSP(*YES) REPLACE(*YES)"
step XDBPRFC "CRTCMOD MODULE($LIB/XDBPRFC) SRCSTMF('$K/xdb/xdbprfc.c') TGTCCSID(*JOB) INCDIR('$K/xdb') DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) REPLACE(*YES)"
step XDBSETUP_MOD "CRTRPGMOD MODULE($LIB/XDBSETUP) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBSETUP) DBGVIEW(*SOURCE) REPLACE(*YES)"
step XDBALW_MOD "CRTRPGMOD MODULE($LIB/XDBALW) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBALW) DBGVIEW(*SOURCE) REPLACE(*YES)"
step XDBPRFM_MOD "CRTRPGMOD MODULE($LIB/XDBPRFM) SRCFILE($LIB/QXDBSRC) SRCMBR(XDBPRFM) DBGVIEW(*SOURCE) REPLACE(*YES)" &&
  step XDBPRFM "CRTPGM PGM($LIB/XDBPRFM) MODULE($LIB/XDBPRFM $LIB/XDBSETUP $LIB/XDBALW $LIB/XDBPRFC) ACTGRP(XDBPRFM) TEXT('XDB - work with connection profiles and setup')"
step XDBPRFT_MOD "CRTCMOD MODULE($LIB/XDBPRFT) SRCSTMF('$K/prf/xdbprft.c') TGTCCSID(*JOB) DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) REPLACE(*YES)" &&
  step XDBPRFT "CRTPGM PGM($LIB/XDBPRFT) MODULE($LIB/XDBPRFT) BNDSRVPGM((QSYS/QSQCLI)) ACTGRP(*NEW) TEXT('XDB - connection test of a profile')"
# the command and its help panel group
sh $K/build/build_hlp.sh $K $LIB "$R"; cat $K/out/hlp.log >> $L

# self test of the file and password access
step XDBT9_DATA "RUNSQLSTM SRCSTMF('$K/test/xdbt9.sql') COMMIT(*NONE) NAMING(*SQL) DFTRDBCOL($LIB)"
rm -f /tmp/xdbt9.out
step XDBT9_MOD "CRTCMOD MODULE($LIB/XDBT9) SRCSTMF('$K/test/xdbt9.c') TGTCCSID(*JOB) INCDIR('$K/xdb') SYSIFCOPT(*IFS64IO) DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) REPLACE(*YES)" &&
  step XDBT9 "CRTPGM PGM($LIB/XDBT9) MODULE($LIB/XDBT9 $LIB/XDBPRFC) ACTGRP(*NEW) TEXT('XDB test: profile access')" &&
  step XDBT9_RUN "CALL PGM($LIB/XDBT9)"
step XDBT9_CLEAN "RUNSQLSTM SRCSTMF('$K/test/xdbt9_clean.sql') COMMIT(*NONE) NAMING(*SQL) DFTRDBCOL($LIB)"
echo end >> $L

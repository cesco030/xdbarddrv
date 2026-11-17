# QSH script: help panel group of command WRKXDBPRF, and the command itself with the help attached.
# Called by build_prf.sh; can be run alone after a change of prf/wrkxdbprf.pnlgrp.
# usage: sh build_hlp.sh <kitdir> <lib> [<ref>]   log: <kitdir>/out/hlp.log, listings <kitdir>/out/<name>.lst
# <ref>: library the command names for its program and its help; left out = <lib>, *LIBL = for a package
K=$1; LIB=$2; R=$3
if test -z "$R"; then R=$LIB; fi
mkdir -p $K/out
H=$K/out/hlp.log
echo "help start" > $H
S=/QSYS.LIB/$LIB.LIB/QXDBSRC.FILE
hstep() { # name, command: listing in out/<name>.lst, the error lines of a failure in the log
  if system "$2" > $K/out/$1.lst 2>&1; then
    echo "$1 OK" >> $H; rm -f $K/out/$1.lst; return 0
  fi
  echo "$1 FAILED (see out/$1.lst)" >> $H
  grep -E "CPD[0-9A-F]+|CPF[0-9A-F]+" $K/out/$1.lst | head -30 >> $H
  return 1
}
# The help shows the restore command of an update, and RSTOBJ needs the name of the library the
# package was saved from. The source names the library the published packages are saved from
# (the one line of build/savlib.txt); a package built in another library names that one.
P=$K/prf/wrkxdbprf.pnlgrp
DOCLIB=$(cat $K/build/savlib.txt)
if test "$R" = "*LIBL"; then
  if test "$LIB" != "$DOCLIB"; then
    rm -f $K/out/wrkxdbprf.pnlgrp
    sed "s/SAVLIB($DOCLIB)/SAVLIB($LIB)/g" $P > $K/out/wrkxdbprf.pnlgrp
    P=$K/out/wrkxdbprf.pnlgrp
  fi
  echo "help names SAVLIB($LIB) in $(grep -c "SAVLIB($LIB)" $P) commands" >> $H
fi
hstep SRC_WRKXDBPRFH "CPYFRMSTMF FROMSTMF('$P') TOMBR('$S/WRKXDBPRFH.MBR') MBROPT(*REPLACE)" &&
  system "CHGPFM FILE($LIB/QXDBSRC) MBR(WRKXDBPRFH) SRCTYPE(PNLGRP) TEXT('Help for command WRKXDBPRF')" > /dev/null 2>&1
hstep SRC_WRKXDBPRF "CPYFRMSTMF FROMSTMF('$K/prf/wrkxdbprf.cmd') TOMBR('$S/WRKXDBPRF.MBR') MBROPT(*REPLACE)" &&
  system "CHGPFM FILE($LIB/QXDBSRC) MBR(WRKXDBPRF) SRCTYPE(CMD) TEXT('Work with XDB connection profiles')" > /dev/null 2>&1
hstep WRKXDBPRFH "CRTPNLGRP PNLGRP($LIB/WRKXDBPRF) SRCFILE($LIB/QXDBSRC) SRCMBR(WRKXDBPRFH) REPLACE(*YES) TEXT('Help for command WRKXDBPRF')"
# every command of the kit is created with its help panel group
hstep WRKXDBPRF "CRTCMD CMD($LIB/WRKXDBPRF) PGM($R/XDBPRFM) SRCFILE($LIB/QXDBSRC) SRCMBR(WRKXDBPRF) HLPPNLGRP($R/WRKXDBPRF) HLPID(*CMD) REPLACE(*YES) TEXT('Work with XDB connection profiles')"
echo "help end" >> $H

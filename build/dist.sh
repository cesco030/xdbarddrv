# QSH script: builds the XDB package for other systems.
#   usage: sh dist.sh <kitdir> <buildlib> <version>
#   run in a job that has <buildlib> first in its library list, for example
#   SBMJOB CMD(QSH CMD('sh .../build/dist.sh <kitdir> XDBBUILD 0.9.0')) INLLIBL(XDBBUILD QGPL QTEMP)
#
# Everything is compiled new into <buildlib> with the references made through the library list
# (*LIBL): the Setup display of WRKXDBPRF binds them to the library they are restored into.
# The package holds no profiles and no passwords: the profile files go in as empty models
# (XDBPRFMDL, XDBPROMDL), the password list not at all, and the driver without adoption.
# The licence and the notices (doc/LICENSE, doc/THIRD-PARTY-NOTICES.txt) go in as the members
# of source file XDBLICENSE, so the objects save file carries them by itself.
#
# <buildlib> is created when it is missing. When it exists it is CLEARED, but only if it holds
# data area XDBBUILD, which this script puts there: a library that is not a build library of
# this script is never touched.
#
# The package does not depend on the system it is built on more than it must: the public
# authority of every object is set here (not left to system value QCRTAUT), and the build goes
# through also for a user without special authority who may not create validation lists.
# The objects still name the system, the user and the library that made them, and RSTOBJ needs
# the name of that library. Help and README name the library the published packages are saved
# from (its name is the one line of build/savlib.txt); built in another library, the restore
# commands in the help of the command and in the README beside the save files name that one
# (build_hlp.sh, and below).
#
# Environment:  XDB_NOSRC=1   no sources save file and no copy of the sources (a build from a
#                             clone of the repository: the clone is the source tree)
#               LANG          must name a locale of the CCSID of the job (checked below); it does
#                             by itself where the system values QCCSID and QLOCALE fit together
#
# Result in <kitdir>/../dist:  xdb-<version>-objects.savf   objects (RSTOBJ ... SAVLIB(<buildlib>))
#                              xdb-<version>-sources.savf   sources (RST from /tmp/xdb-src)
#                              xdb-src/                     the same sources as a tree
#                              WRKXDBPRF-help.html, README.md, dist.log
K=$1; B=$2; V=$3
if test -z "$K" || test -z "$B" || test -z "$V"; then echo "usage: sh dist.sh <kitdir> <buildlib> <version>"; exit 1; fi
D=$K/../dist
S=/tmp/xdb-src
mkdir -p $D $K/out
L=$D/dist.log
VER="$V $(date +%Y-%m-%d)"
echo "XDB package $VER, build library $B, start $(date)" > $L
# The scripts read their logs with patterns that use ^ $ and |. Those characters sit at other
# code points from one EBCDIC code page to the next, and a search goes by the locale of the job
# (LANG) while the scripts are read in its CCSID. When the two do not fit, the searches find
# nothing and the log would be silent about failures: checked before anything is touched.
if test "$(echo ab | grep -c -E '^ab$|^x')" != "1"; then
  echo "The locale of this job does not fit its CCSID: a search with the characters ^ \$ | finds nothing." >> $L
  echo "Set LANG to a locale of the CCSID of the job (CCSID 1144, for example: export LANG=/QSYS.LIB/IT_IT.LOCALE) and run again. Nothing was done." >> $L
  exit 1
fi
# the modules that are not compiled as UTF-8 take the CCSID of this job: the log says which
system "DSPJOB OPTION(*DFNA) OUTPUT(*PRINT)" > $K/out/dist.tmp 2>&1
grep -i "character set" $K/out/dist.tmp | grep -v -i default | sed 's/  */ /g;s/\. //g' | head -1 >> $L
ok() { system "CHKOBJ OBJ($B/$1) OBJTYPE($2)" > /dev/null 2>&1; }
do1() { # label, command
  if system "$2" > $K/out/dist.tmp 2>&1; then echo "$1 -> OK" >> $L; return 0; fi
  echo "$1 -> FAILED" >> $L
  grep -E "CPF[0-9A-F]{4}|CPD[0-9A-F]{4}|CPA[0-9A-F]{4}|SQL[0-9]{4}" $K/out/dist.tmp | head -6 >> $L
  return 1
}

# ---- the build library
if system "CHKOBJ OBJ(QSYS/$B) OBJTYPE(*LIB)" > /dev/null 2>&1; then
  if ok XDBBUILD '*DTAARA'; then
    do1 "clear build library $B" "CLRLIB LIB($B)" || exit 1
  else
    echo "Library $B exists and is not a build library of this script (no data area XDBBUILD). Nothing was done." >> $L
    exit 1
  fi
else
  do1 "create build library $B" "CRTLIB LIB($B) TEXT('XDB build library - dist.sh replaces its contents')" || exit 1
fi
do1 "mark it" "CRTDTAARA DTAARA($B/XDBBUILD) TYPE(*CHAR) LEN(50) VALUE('XDB build library') TEXT('Marks a library that dist.sh may clear')"
liblist -a $B > /dev/null 2>&1

# ---- compile and bind everything, references through the library list
echo "--- libpq" >> $L
sh $K/build/build_ile.sh $K $B 10
echo "modules compiled: $(grep -c '^OK ' $K/out/build.log), failed: $(grep -c '^FAIL ' $K/out/build.log)" >> $L
grep '^FAIL ' $K/out/build.log >> $L
sh $K/build/bind_ile.sh $K $B
grep -E " OK$|FAILED|CPF|CPD5D0|not created" $K/out/bind.log | grep -v CPD5CDC >> $L
echo "--- driver" >> $L
sh $K/build/build_xdb.sh $K $B '*LIBL'
grep -E " OK$|FAILED|not created|adopts" $K/out/xdb.log >> $L
echo "--- profiles, setup, command, help" >> $L
sh $K/build/build_prf.sh $K $B '*LIBL'
grep -E " OK$|FAILED|kept|left out|names SAVLIB" $K/out/prf.log | grep -v "^SRC_" >> $L

MISSING=""
for o in XDB:SRVPGM LIBPQ:SRVPGM XDBARD:PGM XDBCMT:PGM XDBPRFM:PGM XDBPRFT:PGM WRKXDBPRF:CMD WRKXDBPRF:PNLGRP XDBPRFD:FILE XDBKWD:FILE XDBPRF:FILE XDBPRO:FILE; do
  ok ${o%%:*} "*${o##*:}" || MISSING="$MISSING ${o%%:*}"
done
if test -n "$MISSING"; then echo "BUILD INCOMPLETE, missing:$MISSING - no package made" >> $L; exit 1; fi

# ---- what makes it a package
echo "--- package" >> $L
do1 "driver without adoption (setup switches it on)" "CHGSRVPGM SRVPGM($B/XDB) USRPRF(*USER)"
do1 "XDBPRF becomes the model XDBPRFMDL" "RNMOBJ OBJ($B/XDBPRF) OBJTYPE(*FILE) NEWOBJ(XDBPRFMDL)"
do1 "XDBPRO becomes the model XDBPROMDL" "RNMOBJ OBJ($B/XDBPRO) OBJTYPE(*FILE) NEWOBJ(XDBPROMDL)"
do1 "models are empty" "CLRPFM FILE($B/XDBPRFMDL)" && do1 "models are empty" "CLRPFM FILE($B/XDBPROMDL)"
do1 "text of XDBPRFMDL" "CHGOBJD OBJ($B/XDBPRFMDL) OBJTYPE(*FILE) TEXT('XDB - model of XDBPRF, the setup copies it')"
do1 "text of XDBPROMDL" "CHGOBJD OBJ($B/XDBPROMDL) OBJTYPE(*FILE) TEXT('XDB - model of XDBPRO, the setup copies it')"
if ok XDBVLDL '*VLDL'; then do1 "no password list in the package" "DLTVLDL VLDL($B/XDBVLDL)"; else echo "no password list in the package -> OK (none was made)" >> $L; fi
do1 "version $VER" "CRTDTAARA DTAARA($B/XDBVER) TYPE(*CHAR) LEN(50) VALUE('$VER') TEXT('XDB - version of this package')"
# the licence and the notices travel inside the objects save file too: source file XDBLICENSE
# (CCSID 37, the texts are plain ASCII)
if test -f $K/doc/LICENSE && test -f $K/doc/THIRD-PARTY-NOTICES.txt; then
  do1 "file XDBLICENSE for the licence texts" "CRTSRCPF FILE($B/XDBLICENSE) RCDLEN(112) CCSID(37) TEXT('XDB - licence and notices (LICENSE, NOTICES)')" &&
  do1 "member LICENSE" "CPYFRMSTMF FROMSTMF('$K/doc/LICENSE') TOMBR('/QSYS.LIB/$B.LIB/XDBLICENSE.FILE/LICENSE.MBR') MBROPT(*REPLACE)" &&
  do1 "member NOTICES" "CPYFRMSTMF FROMSTMF('$K/doc/THIRD-PARTY-NOTICES.txt') TOMBR('/QSYS.LIB/$B.LIB/XDBLICENSE.FILE/NOTICES.MBR') MBROPT(*REPLACE)"
  system "CHGPFM FILE($B/XDBLICENSE) MBR(LICENSE) SRCTYPE(TXT) TEXT('Licence of XDBARDDRV (MIT)')" > /dev/null 2>&1
  system "CHGPFM FILE($B/XDBLICENSE) MBR(NOTICES) SRCTYPE(TXT) TEXT('Notices of the PostgreSQL code in LIBPQ')" > /dev/null 2>&1
  echo "  XDBLICENSE members: $(ls /QSYS.LIB/$B.LIB/XDBLICENSE.FILE 2>/dev/null | tr '\n' ' ')" >> $L
else
  echo "NOT COPIED LICENSE and THIRD-PARTY-NOTICES.txt into file XDBLICENSE (missing in doc/) - no package made" >> $L; exit 1
fi
for o in XDBARD XDBCMT; do
  system "DSPPGM PGM($B/$o) DETAIL(*SRVPGM)" > $K/out/dist.tmp 2>&1
  echo "  $o bound to: $(grep -E ' XDB ' $K/out/dist.tmp | sed 's/  */ /g' | cut -c1-20)" >> $L
done
system "DSPSRVPGM SRVPGM($B/XDB) DETAIL(*SRVPGM)" > $K/out/dist.tmp 2>&1
echo "  XDB bound to: $(grep -E ' LIBPQ ' $K/out/dist.tmp | sed 's/  */ /g' | cut -c1-20)" >> $L

# the public authority of what is saved, whatever QCRTAUT is here: use for everybody, as the
# packages always had it; the dictionary and the two models are read only
AUTBAD=0
for o in XDB:SRVPGM LIBPQ:SRVPGM XDBARD:PGM XDBCMT:PGM XDBPRFM:PGM XDBPRFT:PGM WRKXDBPRF:CMD WRKXDBPRF:PNLGRP XDBPRFD:FILE XDBVER:DTAARA XDBLICENSE:FILE; do
  system "GRTOBJAUT OBJ($B/${o%%:*}) OBJTYPE(*${o##*:}) USER(*PUBLIC) AUT(*CHANGE) REPLACE(*YES)" > /dev/null 2>&1 || AUTBAD=$((AUTBAD+1))
done
for o in XDBKWD XDBPRFMDL XDBPROMDL; do
  system "GRTOBJAUT OBJ($B/$o) OBJTYPE(*FILE) USER(*PUBLIC) AUT(*USE) REPLACE(*YES)" > /dev/null 2>&1 || AUTBAD=$((AUTBAD+1))
done
if test $AUTBAD -eq 0; then echo "public authority of the 14 objects set -> OK" >> $L; else echo "public authority of the 14 objects set -> FAILED for $AUTBAD" >> $L; fi

# ---- objects save file
do1 "save file for the objects" "CRTSAVF FILE($B/XDBOBJ) TEXT('XDB $V - objects')" || exit 1
do1 "save the objects" "SAVOBJ OBJ(XDB LIBPQ XDBARD XDBCMT XDBPRFM XDBPRFT WRKXDBPRF XDBPRFD XDBKWD XDBPRFMDL XDBPROMDL XDBVER XDBLICENSE) LIB($B) DEV(*SAVF) SAVF($B/XDBOBJ) OBJTYPE(*SRVPGM *PGM *CMD *PNLGRP *FILE *DTAARA) DTACPR(*YES)" || exit 1
grep -E "CPC37[0-9A-F]{2}" $K/out/dist.tmp >> $L
do1 "objects save file to the IFS" "CPYTOSTMF FROMMBR('/QSYS.LIB/$B.LIB/XDBOBJ.FILE') TOSTMF('$D/xdb-$V-objects.savf') STMFOPT(*REPLACE) CVTDTA(*NONE)"

# ---- sources: only what build/dist.files names, staged in a neutral directory
if test -n "$XDB_NOSRC"; then
echo "--- sources: left out (XDB_NOSRC)" >> $L
else
echo "--- sources" >> $L
rm -rf $S $D/xdb-src
mkdir -p $S
N=0
while read f; do
  if test -z "$f"; then continue; fi
  case $f in */*) mkdir -p $S/$(dirname $f) ;; esac
  if cp -R $K/$f $S/$f; then N=$((N+1)); else echo "NOT COPIED $f" >> $L; fi
done < $K/build/dist.files
rm -f $S/prf/*_profile.sql   # profiles of the place where the kit is developed are not part of the package
cp $K/doc/README.md $S/README.md
# the licence and the notices of the code of other authors go with every copy
for f in THIRD-PARTY-NOTICES.txt LICENSE; do if test -f $K/doc/$f; then cp $K/doc/$f $S/$f; fi; done
echo "entries copied: $N, files: $(find $S -type f | wc -l)" >> $L
do1 "save file for the sources" "CRTSAVF FILE($B/XDBSRC) TEXT('XDB $V - sources (IFS)')" &&
do1 "save the sources" "SAV DEV('/QSYS.LIB/$B.LIB/XDBSRC.FILE') OBJ(('$S')) SUBTREE(*ALL) DTACPR(*YES)" &&
do1 "sources save file to the IFS" "CPYTOSTMF FROMMBR('/QSYS.LIB/$B.LIB/XDBSRC.FILE') TOSTMF('$D/xdb-$V-sources.savf') STMFOPT(*REPLACE) CVTDTA(*NONE)"
cp -R $S $D/xdb-src
fi

# ---- documentation
# RSTOBJ needs the name of the library the objects were saved from: the README that goes beside
# the save files names this build library (the sources name the one of build/savlib.txt)
DOCLIB=$(cat $K/build/savlib.txt)
rm -f $D/README.md
if test "$B" = "$DOCLIB"; then
  cp $K/doc/README.md $D/README.md
else
  touch -C 819 $D/README.md
  sed "s/SAVLIB($DOCLIB)/SAVLIB($B)/g" $K/doc/README.md >> $D/README.md
fi
n=$(grep -c "SAVLIB($B)" $D/README.md)
if test "$n" = "2"; then echo "README beside the save files: SAVLIB($B) in 2 commands -> OK" >> $L
else echo "README beside the save files: SAVLIB($B) in $n commands, 2 expected -> FAILED (build/savlib.txt names $DOCLIB)" >> $L; fi
for f in THIRD-PARTY-NOTICES.txt LICENSE; do rm -f $D/$f; if test -f $K/doc/$f; then cp $K/doc/$f $D/$f; fi; done
if test ! -f $D/THIRD-PARTY-NOTICES.txt; then echo "NOT COPIED THIRD-PARTY-NOTICES.txt (run build/gen_notices.py)" >> $L; fi
if test ! -f $D/LICENSE; then echo "NOT COPIED LICENSE (doc/LICENSE is missing)" >> $L; fi
rm -f $D/WRKXDBPRF-help.html
do1 "help text as a page" "GENCMDDOC CMD($B/WRKXDBPRF) TODIR('$D') GENOPT(*HTML) REPLACE(*YES)" && mv $D/${B}_WRKXDBPRF.html $D/WRKXDBPRF-help.html
rm -f $K/out/dist.tmp
echo "--- result" >> $L
ls -l $D | grep -E "savf|html|README|NOTICES|LICENSE" | sed 's/  */ /g' | cut -d' ' -f5- >> $L
echo "end $(date)" >> $L

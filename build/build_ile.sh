# QSH script: compile the libpq kit into ILE modules.
# usage: sh build_ile.sh <kitdir> <lib> <optimize> [module-filter-regex]
K=$1; LIB=$2; OPT=$3; FLT=$4
mkdir -p $K/out
LOG=$K/out/build.log
echo "start $(date) lib=$LIB opt=$OPT" > $LOG
INC="'$K/ile/compat' '$K/ile' '$K/pg/src/include' '$K/pg/src/interfaces/libpq' '$K/pg/src/port' '$K/pg/src/common'"
while read mod src; do
  if [ -n "$FLT" ]; then echo $mod | grep -q -E "$FLT" || continue; fi
  if system "CRTCMOD MODULE($LIB/$mod) SRCSTMF('$K/$src') LOCALETYPE(*LOCALEUTF) TGTCCSID(1208) SYSIFCOPT(*IFS64IO) LANGLVL(*EXTENDED) DEFINE('FRONTEND' 'USE_PRIVATE_ENCODING_FUNCS' 'JSONAPI_USE_PQEXPBUFFER') INCDIR($INC) OPTIMIZE($OPT) DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) FLAG(30) REPLACE(*YES)" > $K/out/$mod.lst 2>&1; then
    echo "OK $mod" >> $LOG; rm -f $K/out/$mod.lst
  else
    echo "FAIL $mod $src" >> $LOG
  fi
done < $K/build/modules.txt
echo "end $(date)" >> $LOG

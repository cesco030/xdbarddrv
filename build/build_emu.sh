#!/bin/sh
# Linux gate: build the kit with the ILE feature set (XDB_LINUX_EMU).
# usage: build_emu.sh <kitdir> <outdir>
#   XDB_EXTRA in the environment is added to the compiler options, for a
#   build with the sanitizers (see test/dbfuzz.py).
set -e
K=$1; O=$2
mkdir -p "$O/obj"
INC="-I$K/ile -I$K/pg/src/include -I$K/pg/src/interfaces/libpq -I$K/pg/src/port -I$K/pg/src/common"
DEF="-DXDB_LINUX_EMU -DFRONTEND -DUSE_PRIVATE_ENCODING_FUNCS -DJSONAPI_USE_PQEXPBUFFER -DSO_MAJOR_VERSION=5 -D_REENTRANT -D_GNU_SOURCE"
# -fwrapv -fno-strict-aliasing: as PostgreSQL's own build has them (its code counts on both)
CFL="-std=c99 -O1 -fPIC -fwrapv -fno-strict-aliasing -Wall -Wno-unused -Wmissing-prototypes -Werror=implicit-function-declaration $XDB_EXTRA"
fail=0
for f in $(cat "$K/build/files.txt") ; do
  o="$O/obj/$(basename "$f" .c).o"
  gcc $CFL $DEF $INC -c "$K/pg/$f" -o "$o" 2> "$o.log" || { echo "FAIL $f"; head -5 "$o.log"; fail=1; }
done
for x in ile_shim ile_chklocale ile_bitutils; do gcc $CFL $DEF $INC -c "$K/ile/$x.c" -o "$O/obj/$x.o" || fail=1; done
[ $fail = 0 ] || exit 1
gcc -shared $XDB_EXTRA -o "$O/libpq_emu.so" "$O"/obj/*.o -lpthread -lm
echo "built $O/libpq_emu.so ($(ls "$O"/obj/*.o | wc -l) objects)"

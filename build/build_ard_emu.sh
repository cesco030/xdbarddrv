#!/bin/sh
# Linux gate for the ARD: build the driver (all backends) and the harness.
# usage: build_ard_emu.sh <kitdir> <outdir>   (libpq_emu.so must be in <outdir>)
set -e
K=$1; O=$2
INC="-I$K/xdb -I$K/ile -I$K/pg/src/interfaces/libpq -I$K/pg/src/include"
CFL="-std=gnu99 -O1 -g -Wall -Wno-unused -Wmissing-prototypes -Werror=implicit-function-declaration -DXDB_LINUX_EMU -D_GNU_SOURCE $XDB_EXTRA"
SRC="xdbard xdblob xdbpg xdbmy xdbnet xdbtls"
[ -f "$K/xdb/xdbms.c" ] && SRC="$SRC xdbms" || CFL="$CFL -DXDB_NO_MSSQL"
OBJ=""
for x in $SRC; do gcc $CFL $INC -c "$K/xdb/$x.c" -o "$O/$x.o"; OBJ="$OBJ $O/$x.o"; done
gcc $CFL $INC -c "$K/test/xdbprf_emu.c" -o "$O/xdbprf_emu.o"; OBJ="$OBJ $O/xdbprf_emu.o"
for t in ardtest ardsh; do
  gcc $CFL $INC -I"$K/test" -o "$O/$t" "$K/test/$t.c" $OBJ -L"$O" -lpq_emu -lssl -lcrypto -Wl,-rpath,"$O"
done
echo "built $O/ardtest $O/ardsh"

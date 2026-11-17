#!/bin/bash
# Babelfish for PostgreSQL (TDS test server) - build from the GitHub sources
# Everything is kept under one directory: $XDB_BBF_DIR, or /opt/bbf when that is not set.
B=${XDB_BBF_DIR:-/opt/bbf}; I=$B/inst
export DEBIAN_FRONTEND=noninteractive
step() { echo "=== $1 $(date +%T)"; }
step apt
apt-get install -y -q flex libicu-dev libxml2-dev libossp-uuid-dev uuid-dev libxslt1-dev pkg-config gperf > $B/apt.log 2>&1; echo "apt rc=$?"
step clone
[ -d pg ] || git clone -q --depth 1 -b BABEL_5_6_0__PG_17_10 https://github.com/babelfish-for-postgresql/postgresql_modified_for_babelfish pg
[ -d ext ] || git clone -q --depth 1 -b BABEL_5_6_0 https://github.com/babelfish-for-postgresql/babelfish_extensions ext
[ -d antlr4 ] || git clone -q --depth 1 -b 4.9.3 https://github.com/antlr/antlr4 antlr4
[ -s /usr/local/lib/antlr-4.9.3-complete.jar ] || curl -sSL -o /usr/local/lib/antlr-4.9.3-complete.jar https://raw.githubusercontent.com/antlr/website-antlr4/gh-pages/download/antlr-4.9.3-complete.jar
ls -la /usr/local/lib/antlr-4.9.3-complete.jar
step engine
if [ ! -x $I/bin/postgres ]; then
  cd $B/pg && ./configure --prefix=$I --without-readline --with-libxml --with-uuid=ossp --with-icu --with-openssl CFLAGS="-O1" > $B/configure.log 2>&1; echo "configure rc=$?"
  make -j3 > $B/make_pg.log 2>&1; echo "make rc=$?"
  make install > $B/inst_pg.log 2>&1; echo "install rc=$?"
  make -C contrib -j3 install > $B/contrib.log 2>&1; echo "contrib rc=$?"
fi
step antlr-runtime
if [ ! -f /usr/local/lib/libantlr4-runtime.so.4.9.3 ]; then
  mkdir -p $B/antlr4/runtime/Cpp/build && cd $B/antlr4/runtime/Cpp/build
  cmake .. -DANTLR_JAR_LOCATION=/usr/local/lib/antlr-4.9.3-complete.jar -DCMAKE_INSTALL_PREFIX=/usr/local -DWITH_DEMO=False -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O1" > $B/antlr_cmake.log 2>&1; echo "cmake rc=$?"
  make -j3 > $B/antlr_make.log 2>&1; echo "make rc=$?"
  make install > $B/antlr_inst.log 2>&1; echo "install rc=$?"
  cp /usr/local/lib/libantlr4-runtime.so.4.9.3 $I/lib/
fi
step extensions
export PG_CONFIG=$I/bin/pg_config PG_SRC=$B/pg cmake=$(which cmake)
for e in babelfishpg_money babelfishpg_common babelfishpg_tds babelfishpg_tsql; do
  cd $B/ext/contrib/$e
  make -j3 > $B/ext_$e.log 2>&1; echo "$e make rc=$?"
  make install >> $B/ext_$e.log 2>&1; echo "$e install rc=$?"
done
step DONE
B=${XDB_BBF_DIR:-/opt/bbf}; I=$B/inst
step() { echo "=== $1 $(date +%T)"; }
step antlr-4.13.2
rm -rf antlr4 && git clone -q --depth 1 -b 4.13.2 https://github.com/antlr/antlr4 antlr4 2>/dev/null
curl -sSL -o /usr/local/lib/antlr-4.13.2-complete.jar https://raw.githubusercontent.com/antlr/website-antlr4/gh-pages/download/antlr-4.13.2-complete.jar
ls -la /usr/local/lib/antlr-4.13.2-complete.jar
rm -f /usr/local/lib/libantlr4-runtime.* ; rm -rf /usr/local/include/antlr4-runtime
mkdir -p $B/antlr4/runtime/Cpp/build && cd $B/antlr4/runtime/Cpp/build
cmake .. -DANTLR_JAR_LOCATION=/usr/local/lib/antlr-4.13.2-complete.jar -DCMAKE_INSTALL_PREFIX=/usr/local -DWITH_DEMO=False -DANTLR_BUILD_CPP_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O1" > $B/antlr_cmake.log 2>&1; echo "cmake rc=$?"
make -j4 > $B/antlr_make.log 2>&1; echo "make rc=$?"
make install > $B/antlr_inst.log 2>&1; echo "install rc=$?"
cp /usr/local/lib/libantlr4-runtime.so.4.13.2 $I/lib/
step tsql
export PG_CONFIG=$I/bin/pg_config PG_SRC=$B/pg cmake=$(which cmake)
cd $B/ext/contrib/babelfishpg_tsql && git clean -fdxq . 
make -j4 > $B/ext_babelfishpg_tsql.log 2>&1; echo "tsql make rc=$?"
make install >> $B/ext_babelfishpg_tsql.log 2>&1; echo "tsql install rc=$?"
step DONE
B=${XDB_BBF_DIR:-/opt/bbf}; I=$B/inst
export PG_CONFIG=$I/bin/pg_config PG_SRC=$B/pg cmake=$(which cmake)
unset JAVA_TOOL_OPTIONS; echo skip-gen
cd $B/ext/contrib/babelfishpg_tsql
make -j4 > $B/ext_babelfishpg_tsql.log 2>&1; echo "tsql make rc=$? $(date +%T)"
make install >> $B/ext_babelfishpg_tsql.log 2>&1; echo "tsql install rc=$?"
echo DONE
# creates and starts the Babelfish instance: PostgreSQL on 5544, TDS on 14333
B=${XDB_BBF_DIR:-/opt/bbf}; I=$B/inst; D=$B/data; U=postgres
export LD_LIBRARY_PATH=$I/lib:/usr/local/lib
if [ ! -d $D ]; then
  mkdir -p $D && chown -R $U $B/data && chmod 755 $(dirname $B) $B
  su $U -c "LD_LIBRARY_PATH=$LD_LIBRARY_PATH $I/bin/initdb -D $D -E UTF8 --locale=C.UTF-8 -A trust" > $B/initdb.log 2>&1 || { tail -3 $B/initdb.log; exit 1; }
  cat >> $D/postgresql.conf <<X
listen_addresses = '127.0.0.1'
port = 5544
shared_preload_libraries = 'babelfishpg_tds'
babelfishpg_tds.port = 14333
babelfishpg_tds.listen_addresses = '127.0.0.1'
unix_socket_directories = '/tmp'
X
  echo "host all all 127.0.0.1/32 md5" > $D/pg_hba.conf.new; echo "local all all trust" >> $D/pg_hba.conf.new; cp $D/pg_hba.conf.new $D/pg_hba.conf; chown $U $D/pg_hba.conf
  FIRST=1
fi
su $U -c "LD_LIBRARY_PATH=$LD_LIBRARY_PATH $I/bin/pg_ctl -D $D -l /tmp/bbf.log -w start" | tail -1
if [ -n "$FIRST" ]; then
  P="$I/bin/psql -h /tmp -p 5544 -U $U -v ON_ERROR_STOP=1"
  su $U -c "LD_LIBRARY_PATH=$LD_LIBRARY_PATH $P -d postgres -c \"CREATE USER babelfish_user WITH SUPERUSER CREATEDB CREATEROLE PASSWORD 'xdbpw' INHERIT\" -c \"CREATE DATABASE babelfish_db OWNER babelfish_user\""
  su $U -c "LD_LIBRARY_PATH=$LD_LIBRARY_PATH $P -d babelfish_db -c 'CREATE EXTENSION IF NOT EXISTS babelfishpg_tds CASCADE' -c 'GRANT ALL ON SCHEMA sys to babelfish_user' -c \"ALTER SYSTEM SET babelfishpg_tsql.database_name = 'babelfish_db'\" -c \"ALTER SYSTEM SET babelfishpg_tsql.migration_mode = 'single-db'\" -c 'SELECT pg_reload_conf()' -c \"CALL sys.initialize_babelfish('babelfish_user')\"" 2>&1 | tail -4
fi

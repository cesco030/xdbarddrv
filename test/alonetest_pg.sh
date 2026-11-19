#!/bin/sh
# Linux test of the statements a PostgreSQL server runs only outside a transaction (CREATE
# DATABASE, DROP DATABASE, VACUUM and the like), through the emulation of the driver.
#   usage: sh alonetest_pg.sh <emu dir with ardsh>     needs the PostgreSQL of the other tests
#   (127.0.0.1:5433, user postgres, password xdbpw; the user creates and drops the database
#   xdb_al_db, the schema xdb_al and the table xdb_al_t there).  Compare with
#   alonetest_pg.expected.
# Under commitment control the driver has a transaction open on the server from the first
# statement of a unit of work on, and the server refuses such a statement in it (SQLSTATE
# 25001); a program without commitment control gets no statement through Db2 to a database
# that is not Db2 for i (SQL5028).  So the driver runs such a statement on its own when the
# unit of work has nothing on the connection that a COMMIT or ROLLBACK would still decide.
# What is tried:
#   - as the first statement of a unit of work and after statements that only read: it runs;
#   - what it did is final at once: a ROLLBACK does not take a new database away;
#   - a SET of the session made before it in the same unit of work stays;
#   - with changes in the unit of work, or with an open cursor: refused with a text that says
#     what to do, and the unit of work and the cursor go on;
#   - prepared and executed; opened as a cursor (the server takes it for no query: not run);
#   - an error of the second run (the database is there already) is the error the program gets;
#   - without a savepoint for each statement (option savepoints = off) the empty transaction
#     can only be rolled back: it runs too, but not after a SET of the session, which would
#     be lost with the transaction;
#   - SET TRANSACTION after the first query has the same SQLSTATE and is left alone;
#   - without commitment control (the emulation can, Db2 cannot) nothing is different.
# Work files: /tmp/xdb_alone.  XDB_ALONE_TRACE=Y writes the traces to /tmp/xdb_ALONEPG.log
# and /tmp/xdb_ALONEPN.log.
E=$1
D=/tmp/xdb_alone
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=20
{
  echo "P|ALONEPG|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|${XDB_ALONE_TRACE:-N}"
  echo "W|ALONEPG|xdbpw"
  echo "P|ALONEPN|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|${XDB_ALONE_TRACE:-N}"
  echo "W|ALONEPN|xdbpw"
  echo "O|ALONEPN|savepoints|D|off"
} > $P
run() { # rdb, title, then the requests on standard input
  echo "== $2"
  XDB_TEST_RDB=$1 $E/ardsh 2>&1 | grep -E "^(>|AR|  row|  col|  rows|  total|  the open)" | cut -c1-290
}

run ALONEPG "the objects (DROP DATABASE is such a statement too)" <<'EOF2'
C
ISO *CHG
X drop database if exists xdb_al_db
X drop schema if exists xdb_al cascade
X drop table if exists xdb_al_t
X create schema xdb_al
X create table xdb_al.t (n int)
X create table xdb_al_t (n int)
COMMIT
D
EOF2

run ALONEPG "the first statement of a unit of work, and after one that only read" <<'EOF2'
C
ISO *CHG
X vacuum xdb_al_t
Q select 1 as one
X vacuum xdb_al_t
Q select count(*) as n from xdb_al_t
COMMIT
D
EOF2

run ALONEPG "a database: there at once, a ROLLBACK does not take it away; the error of the second run" <<'EOF2'
C
ISO *CHG
X create database xdb_al_db
Q select datname from pg_database where datname = 'xdb_al_db'
ROLLBACK
Q select datname from pg_database where datname = 'xdb_al_db'
X create database xdb_al_db
Q select 2 as in_step
X drop database xdb_al_db
Q select count(*) as left_over from pg_database where datname = 'xdb_al_db'
COMMIT
D
EOF2

run ALONEPG "a SET of the session made before it stays" <<'EOF2'
C
ISO *CHG
X set search_path to xdb_al, public
X vacuum t
Q show search_path
COMMIT
Q show search_path
COMMIT
D
EOF2

run ALONEPG "with changes in the unit of work: refused, and the unit of work goes on" <<'EOF2'
C
ISO *CHG
X insert into xdb_al_t values (1)
X vacuum xdb_al_t
Q XDB LAST ERROR
Q select count(*) as n from xdb_al_t
ROLLBACK
Q select count(*) as n from xdb_al_t
X vacuum xdb_al_t
COMMIT
D
EOF2

run ALONEPG "with an open cursor: refused, and the cursor goes on" <<'EOF2'
C
ISO *CHG
O c1 select g from generate_series(1, 3) g
X vacuum xdb_al_t
Q XDB LAST ERROR
F c1
K c1
X vacuum xdb_al_t
COMMIT
D
EOF2

run ALONEPG "prepared, executed twice, refused after a change, executed after the ROLLBACK" <<'EOF2'
C
ISO *CHG
P s1 vacuum xdb_al_t
E s1
E s1
X insert into xdb_al_t values (5)
E s1
ROLLBACK
E s1
Q select count(*) as n from xdb_al_t
COMMIT
D
EOF2

run ALONEPG "opened as a cursor: no query for the server, and it is not run" <<'EOF2'
C
ISO *CHG
Q vacuum xdb_al_t
Q select 3 as in_step
COMMIT
D
EOF2

run ALONEPG "SET TRANSACTION after the first query: the same SQLSTATE, left alone" <<'EOF2'
C
ISO *CHG
Q select 1 as one
X set transaction isolation level serializable
Q XDB LAST ERROR
Q select 4 as in_step
COMMIT
D
EOF2

run ALONEPN "without a savepoint for each statement (option savepoints = off)" <<'EOF2'
C
ISO *CHG
X vacuum xdb_al_t
Q select 1 as one
X vacuum xdb_al_t
X set search_path to xdb_al, public
X vacuum t
Q XDB LAST ERROR
ROLLBACK
X set search_path to xdb_al, public
COMMIT
X vacuum t
Q show search_path
COMMIT
X insert into xdb_al_t values (7)
X vacuum xdb_al_t
Q select count(*) as n from xdb_al_t
ROLLBACK
Q select count(*) as n from xdb_al_t
COMMIT
D
EOF2

run ALONEPG "without commitment control (the emulation can, Db2 cannot)" <<'EOF2'
C
X vacuum xdb_al_t
X insert into xdb_al_t values (9)
X vacuum xdb_al_t
Q select count(*) as n from xdb_al_t
X delete from xdb_al_t
D
EOF2

run ALONEPG "the objects go again" <<'EOF2'
C
ISO *CHG
X drop table xdb_al_t
X drop schema xdb_al cascade
COMMIT
Q select count(*) as left_over from pg_class where relname = 'xdb_al_t'
COMMIT
D
EOF2

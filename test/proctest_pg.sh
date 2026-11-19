#!/bin/sh
# Linux test of what a PostgreSQL server returns from statements that are no query
# (xdb/xdbpg.c), through the emulation of the driver: the row with the OUT parameters of a
# procedure, rows through a cursor that a procedure or a function opened, RETURNING, SHOW,
# EXPLAIN.
#   usage: sh proctest_pg.sh <emu dir with ardsh>     needs the PostgreSQL of the other tests
#   (127.0.0.1:5433, user postgres, password xdbpw; version 11 or later for the procedures;
#   the user creates and drops the objects xdb_pt_* there).  Compare with proctest_pg.expected.
# The server takes only a query in DECLARE CURSOR, and DECLARE CURSOR is how the driver opens
# a cursor there.  A statement that is no query and returns rows all the same runs when its
# cursor is opened, and its rows are kept by the driver.  What is tried:
#   - a function that returns a table is a query, as it always was;
#   - a cursor on CALL: one row with the OUT and INOUT parameters; opened again; a procedure
#     without such parameters has nothing to fetch (SQLSTATE 07005) and has run;
#   - rows from a procedure or a function: it opens a cursor and hands back its name, FETCH
#     reads it in the same unit of work.  Prepared before the cursor exists the FETCH is
#     described with no columns, and they come with the open;
#   - INSERT, UPDATE and DELETE with RETURNING, also in a WITH clause; a WITH that only names
#     such a word stays a query; SHOW; EXPLAIN;
#   - ROLLBACK undoes what such a statement did; without a unit of work each one is committed
#     on its own and the cursor of a procedure does not outlive the CALL.
# Work files: /tmp/xdb_procp.  XDB_PROCT_TRACE=Y writes the trace to /tmp/xdb_PROCPG.log.
E=$1
D=/tmp/xdb_procp
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_TEST_RDB=PROCPG XDB_CONF=/dev/null XDB_SH_MAXROWS=20
{
  echo "P|PROCPG|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|${XDB_PROCT_TRACE:-N}"
  echo "W|PROCPG|xdbpw"
} > $P
run() { # title, then the requests on standard input
  echo "== $1"
  $E/ardsh 2>&1 | grep -E "^(>|AR|  row|  col|  rows|  total|  the open)" | cut -c1-170
}

run "the objects" <<'EOF2'
C
X drop procedure if exists xdb_pt_add(int, int, int)
X drop procedure if exists xdb_pt_none(int)
X drop procedure if exists xdb_pt_cur(int, refcursor)
X drop function if exists xdb_pt_curf(int, refcursor)
X drop function if exists xdb_pt_rows(int)
X drop table if exists xdb_pt_log
X create table xdb_pt_log (id serial primary key, n int)
X create procedure xdb_pt_add(in a int, in b int, inout total int) language plpgsql as $$ begin total := a + b; insert into xdb_pt_log (n) values (total); end $$
X create procedure xdb_pt_none(in a int) language plpgsql as $$ begin insert into xdb_pt_log (n) values (a); end $$
X create procedure xdb_pt_cur(in p_n int, inout c refcursor) language plpgsql as $$ begin open c for select g as n, g::bigint * g as sq, 'row ' || g as label from generate_series(p_n, p_n + 2) g; end $$
X create function xdb_pt_curf(p_n int, c refcursor) returns refcursor language plpgsql as $$ begin open c for select g as n, cast(g / 7.0 as numeric(9,3)) as part, date '2026-01-01' + g as day, g % 2 = 0 as even from generate_series(p_n, p_n + 2) g; return c; end $$
X create function xdb_pt_rows(p_n int) returns table (n int, sq bigint, label text) language sql as $$ select g, g::bigint * g, 'row ' || g from generate_series(p_n, p_n + 1) g $$
D
EOF2

run "a function that returns a table is a query" <<'EOF2'
C
Q select * from xdb_pt_rows(7)
O c1 select n, label from xdb_pt_rows(?) where sq > ? ## i:200|i:40000
F c1
K c1
D
EOF2

run "a cursor on CALL: the row with the OUT and INOUT parameters" <<'EOF2'
C
Q call xdb_pt_add(5, 6, null)
O c1 call xdb_pt_add(?, ?, null) ## i:40|i:2
F c1
F c1
K c1
A c1 ## i:1000|i:234
F c1
K c1
O c2 call xdb_pt_none(?) ## i:9
Q XDB LAST ERROR
Q select n from xdb_pt_log order by id
P s1 call xdb_pt_add(?, ?, null)
E s1 i:1|i:1
Q select 1 as in_step
D
EOF2

run "rows from a procedure or a function: it opens a cursor, FETCH reads it" <<'EOF2'
ISO *CHG
C
X call xdb_pt_cur(5, 'xdb_c_a')
Q fetch all from xdb_c_a
X close xdb_c_a
P s1 call xdb_pt_cur(?, 'xdb_c_b')
E s1 i:100
O c1 fetch all from xdb_c_b
F c1
K c1
X close xdb_c_b
E s1 i:200
A c1
F c1
K c1
Q call xdb_pt_cur(7, null)
Q fetch all from "<unnamed portal 1>"
Q select xdb_pt_curf(30, 'xdb_c_f')
Q fetch 2 from xdb_c_f
Q fetch all from xdb_c_f
Q fetch all from xdb_c_f
O c3 fetch all from xdb_c_late
X call xdb_pt_cur(50, 'xdb_c_late')
A c3
F c3
K c3
COMMIT
Q fetch all from xdb_c_late
Q select 2 as in_step
COMMIT
D
EOF2

run "RETURNING, also in a WITH clause; SHOW; EXPLAIN" <<'EOF2'
C
X delete from xdb_pt_log
X alter sequence xdb_pt_log_id_seq restart with 1
Q insert into xdb_pt_log (n) values (77), (78) returning id, n
O c1 insert into xdb_pt_log (n) values (?) returning id ## i:79
F c1
K c1
A c1 ## i:80
F c1
K c1
Q update xdb_pt_log set n = n + 100 where n >= 79 returning id, n
Q with gone as (delete from xdb_pt_log where n = 78 returning id, n) select count(*) as deleted, max(n) as n from gone
Q with t as (select 'the word insert in a text' as s) select s from t
Q delete from xdb_pt_log where n = -1 returning id
Q select id, n from xdb_pt_log order by id
Q show transaction_read_only
Q explain (costs off) select 1
Q insert into xdb_pt_log (id, n) values (1, 1) returning id
Q select 3 as in_step
D
EOF2

run "in a unit of work ROLLBACK undoes it; without one each statement is committed" <<'EOF2'
ISO *CHG
C
Q insert into xdb_pt_log (n) values (5001) returning n
Q call xdb_pt_add(5000, 2, null)
Q select count(*) as inside from xdb_pt_log where n >= 5000
ROLLBACK
Q select count(*) as left_over from xdb_pt_log where n >= 5000
COMMIT
D
ISO *NONE
C
X call xdb_pt_cur(5, 'xdb_c_a')
Q fetch all from xdb_c_a
Q insert into xdb_pt_log (n) values (6001) returning n
Q select count(*) as kept from xdb_pt_log where n = 6001
D
EOF2

run "tidy" <<'EOF2'
C
X drop procedure if exists xdb_pt_add(int, int, int)
X drop procedure if exists xdb_pt_none(int)
X drop procedure if exists xdb_pt_cur(int, refcursor)
X drop function if exists xdb_pt_curf(int, refcursor)
X drop function if exists xdb_pt_rows(int)
X drop table if exists xdb_pt_log
D
EOF2

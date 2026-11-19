#!/bin/sh
# Linux test of stored procedures of a MySQL server (xdb/xdbmy.c), through the emulation of the
# driver: a procedure that is executed, one whose rows are fetched through a cursor on its
# CALL, and the statements whose columns the server tells only when they run.
#   usage: sh proctest_my.sh <emu dir with ardsh>     needs the MySQL of the other tests
#   (127.0.0.1:3306, database xdbtest, user xdbn, password xdbpw; the user creates and drops
#   the procedures xdb_pt_* there).  Compare with proctest_my.expected.
# What is tried:
#   - values go in as parameter markers; a value comes out through a session variable;
#   - a cursor on a CALL: the PREPARE of a CALL tells no columns, the server describes the
#     rows when the procedure has run, and the driver hands that description to Db2 with the
#     answer to the OPEN (the harness prints it as "the open tells n columns");
#   - the same cursor opened again, the first of two result sets, a procedure without a
#     result set, one that fails after its rows: after each the connection answers the next
#     statement (a driver that leaves part of the server's answer unread does not);
#   - EXPLAIN and the SHOW statements that are described late too.  No cursor is asked for
#     these: MySQL 8.0 ends abnormally when a prepared EXPLAIN is run with one;
#   - a table that got a column after the statement was prepared.
# Work files: /tmp/xdb_proct.  XDB_PROCT_TRACE=Y writes the trace to /tmp/xdb_PROCMY.log.
E=$1
D=/tmp/xdb_proct
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_TEST_RDB=PROCMY XDB_CONF=/dev/null XDB_SH_MAXROWS=20
{
  echo "P|PROCMY|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*OFF|10|0|1000|6|${XDB_PROCT_TRACE:-N}"
  echo "W|PROCMY|xdbpw"
} > $P
run() { # title, then the requests on standard input
  echo "== $1"
  $E/ardsh 2>&1 | grep -E "^(>|AR|  row|  col|  rows|  total|  the open)" | cut -c1-170
}

run "the procedures" <<'EOF2'
C
X drop procedure if exists xdb_pt_add
X drop procedure if exists xdb_pt_fill
X drop procedure if exists xdb_pt_rows
X drop procedure if exists xdb_pt_two
X drop procedure if exists xdb_pt_none
X drop procedure if exists xdb_pt_fail
X drop table if exists xdb_pt_log
X create table xdb_pt_log (n int)
X create procedure xdb_pt_add(in a int, in b int, out total int) begin set total = a + b; end
X create procedure xdb_pt_fill(in p_n int) begin drop temporary table if exists xdb_pt_out; create temporary table xdb_pt_out (n int, sq bigint, label varchar(40)); insert into xdb_pt_out values (p_n, p_n * p_n, concat('row ', p_n)), (p_n + 1, (p_n + 1) * (p_n + 1), concat('row ', p_n + 1)); end
X create procedure xdb_pt_rows(in p_n int) begin insert into xdb_pt_log values (p_n); select n, n * n as sq, concat('row ', n) as label, cast(n / 7 as decimal(9,3)) as part, date_add('2026-01-01', interval n day) as day from (select p_n as n union all select p_n + 1 union all select p_n + 2) t; end
X create procedure xdb_pt_two(in p_n int) begin select p_n as first_n, 'first' as which; select p_n + 1 as second_n, 'second' as which, 'x' as more; end
X create procedure xdb_pt_none(in p_n int) begin set @xdb_pt_seen = p_n; end
X create procedure xdb_pt_fail(in p_n int) begin select p_n as n; signal sqlstate '45000' set message_text = 'xdb_pt_fail says no'; end
D
EOF2

run "values in by markers, a value out through a session variable, rows through a temporary table" <<'EOF2'
C
P s1 call xdb_pt_add(?, ?, @xdb_pt_total)
E s1 i:40|i:2
Q select cast(@xdb_pt_total as char) as total
E s1 i:1000|i:234
Q select cast(@xdb_pt_total as char) as total
X call xdb_pt_add(5, 6, @xdb_pt_total)
Q select cast(@xdb_pt_total as char) as total
P s2 call xdb_pt_fill(?)
E s2 i:7
Q select n, sq, label from xdb_pt_out order by n
D
EOF2

run "a cursor on a CALL: the rows of the procedure" <<'EOF2'
C
Q call xdb_pt_rows(5)
Q select 1 as in_step
O c1 call xdb_pt_rows(?) ## i:200
F c1
F c1
K c1
A c1 ## i:300
F c1
K c1
A c1 ## i:400
A c1 ## i:500
F c1
K c1
Q select count(*) as calls, min(n) as first_n, max(n) as last_n from xdb_pt_log
D
EOF2

run "executed only: the rows are read and dropped" <<'EOF2'
C
X call xdb_pt_rows(1)
P s1 call xdb_pt_rows(?)
E s1 i:2
E s1 i:3
Q select 2 as in_step
D
EOF2

run "two result sets (the first is fetched), no result set, an error after the rows" <<'EOF2'
C
Q call xdb_pt_two(7)
Q select 3 as in_step
O c2 call xdb_pt_none(?) ## i:9
Q XDB LAST ERROR
Q select @xdb_pt_seen as seen
O c3 call xdb_pt_fail(?) ## i:4
Q select 4 as in_step
A c3 ## i:5
Q select 5 as in_step
D
EOF2

run "in a unit of work: the rows are there after the OPEN, ROLLBACK undoes what the procedure did" <<'EOF2'
ISO *CHG
C
X delete from xdb_pt_log
COMMIT
O c1 call xdb_pt_rows(?) ## i:10
F c1
K c1
Q select count(*) as logged from xdb_pt_log
ROLLBACK
Q select count(*) as logged from xdb_pt_log
O c1 call xdb_pt_rows(?) ## i:20
COMMIT
Q select count(*) as logged from xdb_pt_log
COMMIT
D
EOF2

run "statements the server describes when they run: EXPLAIN, some SHOW" <<'EOF2'
C
Q explain select 1
Q select 6 as in_step
Q show create table xdb_pt_log
Q show tables like 'xdb_pt%'
Q select 7 as in_step
D
EOF2

run "a table that got a column after the statement was prepared" <<'EOF2'
C
X create temporary table xdb_pt_chg (a int)
X insert into xdb_pt_chg values (1)
O c5 select * from xdb_pt_chg
F c5
K c5
X alter table xdb_pt_chg add column b varchar(10) default 'new', add column c int default 7
A c5
Q XDB LAST ERROR
Q select 8 as in_step
O c5 select * from xdb_pt_chg
F c5
K c5
D
EOF2

run "tidy" <<'EOF2'
C
X drop procedure if exists xdb_pt_add
X drop procedure if exists xdb_pt_fill
X drop procedure if exists xdb_pt_rows
X drop procedure if exists xdb_pt_two
X drop procedure if exists xdb_pt_none
X drop procedure if exists xdb_pt_fail
X drop table if exists xdb_pt_log
D
EOF2

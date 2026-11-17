**free
// XDBLOST - a server connection ends inside a unit of work: what does COMMIT answer?
//
// A unit of work that changes rows on a server and in the local database is right only
// when both parts are committed or neither.  The dangerous moment: the connection to the
// server ends while the program does something else (the server closes a session that
// waits, a firewall forgets the connection, the server is restarted) and the program
// gives COMMIT without having sent another statement to that server.  The rows on the
// server are gone with the session, so the COMMIT has to fail and the local rows must
// not stay.  This program makes that happen and prints what it gets.
//
// How a session is made to end: the program tells the server to close a session that
// waits for more than 2 seconds (PostgreSQL: idle_in_transaction_session_timeout, MySQL:
// wait_timeout; both are settings of the session and end with it) and then waits 5.
//
// One server (the other name *NONE), cases A to E:
//   A  nothing is lost: a row on the server, a row in the local table, COMMIT
//   B  rows on both, the session ends, COMMIT
//   C  the server is only read, a local row, the session ends, COMMIT
//   D  rows on both, the session ends, ROLLBACK
//   E  rows on both, the session ends, one more statement for the server (it is told
//      that the unit of work is lost), and the program gives COMMIT all the same
// Two servers (both names), cases F to I:
//   F  nothing is lost: rows on both servers and locally, COMMIT
//   G  rows everywhere, the session of the second server ends, COMMIT
//   H  rows everywhere, the session of the first server ends, COMMIT
//   I  the second server is only read, rows on the first and locally, the session of
//      the second ends, COMMIT
// After each case the rows of that case are counted in every table, and the last line of
// the case says whether the result is right.
//
// What is right: in B, E, G and H the COMMIT fails with SQLCODE -175 (SQLSTATE 58028) and
// no row of the case is left anywhere; the program reads the reason from the driver with
// XDB LAST ERROR and gives ROLLBACK.  In C and I the COMMIT goes through: a connection
// that was only read loses nothing.  D is a ROLLBACK like any other.  The job log has the
// messages of the system for each COMMIT that failed (CPF9897 from the driver, CPD83EC,
// CPD83D7, CPF83DB, SQL0175).
//
// On the servers only one temporary table each is made; it ends with the session, which
// is also how a lost row shows: the table is not there any more.
//
//   CALL XDBLOST PARM('<PostgreSQL>' '<MySQL>' '<library>' '<output file>' '<database>')
//   The first two are names of connection profiles (command WRKXDBPRF) with their RDB
//   directory entries, or *NONE.  library: where the local table XDBLOSTC is, which must
//   be journaled (xlost.sh makes it).  database: only for MySQL when its connection has
//   no database: where the temporary table is kept; blank = the first database of the
//   server that lets this user make one.
//
// compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

dcl-pr sleep uns(10) extproc('sleep');
  seconds uns(10) value;
end-pr;

dcl-s lines varchar(400) dim(300);
dcl-s nl int(10) inz(0);
dcl-s localRdb char(18);
dcl-s curRdb char(18);
dcl-s locTab varchar(40);
dcl-s q varchar(1) inz('''');
dcl-s wrong int(10) inz(0);

// the servers of this run
dcl-s cRdb char(18) dim(2);
dcl-s cKind char(2) dim(2);
dcl-s cTab varchar(90) dim(2);
dcl-s nc int(10) inz(0);

// one line of the result with the SQLCODE of the statement before it; gives the SQLCODE back
dcl-proc note;
  dcl-pi *n int(10);
    what varchar(200) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(180) inz('');

  code = sqlcod;
  state = sqlstt;
  exec sql set option closqlcsr = *endmod;
  if code < 0;
    exec sql get diagnostics condition 1 :text = message_text;
  endif;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = what + ' -> ' + %char(code) + ' ' + state;
    if text <> '';
      lines(nl) += '  [' + text + ']';
    endif;
  endif;
  return code;
end-proc;

dcl-proc say;
  dcl-pi *n;
    what varchar(400) const;
  end-pi;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = what;
  endif;
end-proc;

dcl-proc nameOf;
  dcl-pi *n varchar(12);
    kind char(2) const;
  end-pi;
  if kind = 'PG';
    return 'PostgreSQL';
  endif;
  return 'MySQL';
end-proc;

// makes connection k (0 = the local database) the current one
dcl-proc use;
  dcl-pi *n;
    k int(10) const;
  end-pi;
  if k = 0;
    curRdb = localRdb;
  else;
    curRdb = cRdb(k);
  endif;
  exec sql set connection :curRdb;
  if sqlcod < 0;
    note('      SET CONNECTION');
  endif;
end-proc;

// a statement without values on the current connection; its SQLCODE.  Printed when it
// fails, or always when always is on.
dcl-proc exe;
  dcl-pi *n int(10);
    label varchar(120) const;
    stm varchar(400) const;
    always ind const;
  end-pi;
  dcl-s s varchar(400);
  dcl-s code int(10);

  s = stm;
  exec sql execute immediate :s;
  code = sqlcod;
  if code < 0 or always;
    note(label);
  endif;
  return code;
end-proc;

// a query with one row and one number on the current connection; -1 when it fails
// (printed only when loud is on)
dcl-proc one;
  dcl-pi *n int(20);
    label varchar(100) const;
    stm varchar(400) const;
    loud ind const;
  end-pi;
  dcl-s s varchar(400);
  dcl-s n int(20) inz(-1);
  dcl-s nn int(5) inz(0);

  exec sql declare c1 cursor for q1;
  s = stm;
  exec sql prepare q1 from :s;
  if sqlcod < 0;
    if loud;
      note(label + ' (prepare)');
    endif;
    return -1;
  endif;
  exec sql open c1;
  if sqlcod < 0;
    if loud;
      note(label + ' (open)');
    endif;
    return -1;
  endif;
  exec sql fetch c1 into :n :nn;
  if sqlcod < 0;
    if loud;
      note(label + ' (fetch)');
    endif;
    exec sql close c1;
    return -1;
  endif;
  exec sql close c1;
  return n;
end-proc;

// what the driver knows about the current connection: some rows of XDB CONNECTION INFO
dcl-proc counters;
  dcl-pi *n;
    label varchar(60) const;
  end-pi;
  dcl-s s varchar(60) inz('XDB CONNECTION INFO');
  dcl-s a varchar(12);
  dcl-s n varchar(40);
  dcl-s v varchar(250);
  dcl-s ai int(5);
  dcl-s ni int(5);
  dcl-s vi int(5);
  dcl-s line varchar(330) inz('');

  exec sql declare c2 cursor for q2;
  exec sql prepare q2 from :s;
  if sqlcod < 0;
    note(label + ' (XDB CONNECTION INFO)');
    return;
  endif;
  exec sql open c2;
  dow sqlcod = 0;
    exec sql fetch c2 into :a :ai, :n :ni, :v :vi;
    if sqlcod = 0 and (n = 'commit_resource' or n = 'commit_resource_refused');
      say(label + ' ' + n + ' = ' + v);
    elseif sqlcod = 0 and (n = 'reconnects' or n = 'units_of_work_lost' or n = 'commits'
                           or n = 'commits_failed' or n = 'rollbacks');
      if %len(line) + %len(n) + %len(v) + 4 <= 330;
        line += ' ' + n + ' = ' + v + ';';
      endif;
    endif;
  enddo;
  exec sql close c2;
  say(label + line);
end-proc;

// the error the driver kept last, in full
dcl-proc lastError;
  dcl-s s varchar(60) inz('XDB LAST ERROR');
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s native int(10);
  dcl-s dbms varchar(16);
  dcl-s rdb varchar(18);
  dcl-s msg varchar(300);
  dcl-s i1 int(5);
  dcl-s i2 int(5);
  dcl-s i3 int(5);
  dcl-s i4 int(5);
  dcl-s i5 int(5);
  dcl-s i6 int(5);

  exec sql declare c3 cursor for q3;
  exec sql prepare q3 from :s;
  if sqlcod < 0;
    return;
  endif;
  exec sql open c3;
  if sqlcod = 0;
    exec sql fetch c3 into :code :i1, :state :i2, :native :i3, :dbms :i4, :rdb :i5, :msg :i6;
    if sqlcod = 0;
      say('      the driver about it (XDB LAST ERROR): ' + %char(code) + ' ' + state + ' '
          + %trim(rdb) + ': ' + msg);
    endif;
  endif;
  exec sql close c3;
end-proc;

// The temporary table of connection k, made when it is not there.  The first time the
// place is found (cTab), later the same statement makes it again in a new session.
dcl-proc tempTable;
  dcl-pi *n ind;
    k int(10) const;
    wantDb varchar(64) const;
  end-pi;
  dcl-s cols varchar(40) inz(' (id int, txt varchar(40))');
  dcl-s s varchar(400);
  dcl-s db varchar(64);
  dcl-s dbs varchar(64) dim(40);
  dcl-s ndb int(10) inz(0);
  dcl-s i int(10);

  if cKind(k) = 'PG';
    cTab(k) = 'xdblost_c';
    return exe('      temporary table' : 'create temp table if not exists ' + cTab(k) + cols
               : *off) = 0;
  endif;
  if cTab(k) <> '';
    return exe('      temporary table' : 'create temporary table if not exists ' + cTab(k)
               + cols : *off) = 0;
  endif;
  if wantDb <> '';
    cTab(k) = '`' + wantDb + '`.xdblost_c';
    return exe('      temporary table' : 'create temporary table if not exists ' + cTab(k)
               + cols : *off) = 0;
  endif;
  // MySQL keeps a temporary table in a database: the one of the connection, or the first
  // of the server that lets this user make one
  s = 'select schema_name from information_schema.schemata where schema_name not in ('
    + q + 'information_schema' + q + ', ' + q + 'mysql' + q + ', ' + q
    + 'performance_schema' + q + ', ' + q + 'sys' + q + ') order by schema_name';
  exec sql declare c6 cursor for q6;
  exec sql prepare q6 from :s;
  exec sql open c6;
  dow sqlcod = 0 and ndb < %elem(dbs);
    exec sql fetch c6 into :db;
    if sqlcod = 0;
      ndb += 1;
      dbs(ndb) = db;
    endif;
  enddo;
  exec sql close c6;
  cTab(k) = 'xdblost_c';
  s = 'create temporary table if not exists ' + cTab(k) + cols;
  exec sql execute immediate :s;
  if sqlcod = 0;
    return *on;
  endif;
  for i = 1 to ndb;
    cTab(k) = '`' + dbs(i) + '`.xdblost_c';
    s = 'create temporary table if not exists ' + cTab(k) + cols;
    exec sql execute immediate :s;
    if sqlcod = 0;
      say('   the MySQL connection has no database: temporary table in database number '
          + %char(i) + ' of the ' + %char(ndb) + ' the server lists');
      return *on;
    endif;
  endfor;
  note('   temporary table on MySQL');
  cTab(k) = '';
  return *off;
end-proc;

// one row of this case on connection k (0 = local)
dcl-proc put;
  dcl-pi *n;
    k int(10) const;
    id int(10) const;
  end-pi;
  use(k);
  if k = 0;
    exe('      row into the local table' : 'insert into ' + locTab + ' values (' + %char(id)
        + ', ' + q + 'case' + q + ')' : *off);
  else;
    exe('      row on ' + nameOf(cKind(k)) : 'insert into ' + cTab(k) + ' values ('
        + %char(id) + ', ' + q + 'case' + q + ')' : *off);
  endif;
end-proc;

// the server of connection k is told to end a session that waits, and the program waits
dcl-proc endSession;
  dcl-pi *n;
    k int(10) const;
  end-pi;
  use(k);
  if cKind(k) = 'PG';
    exe('      SET idle_in_transaction_session_timeout' :
        'set idle_in_transaction_session_timeout = 2000' : *off);
  else;
    exe('      SET wait_timeout' : 'set session wait_timeout = 2' : *off);
  endif;
  sleep(5);
  say('      (the session on ' + nameOf(cKind(k)) + ' was told to end after 2 idle seconds;'
      + ' 5 seconds went by)');
end-proc;

// the rows of this case on connection k; -1 = the table is not there (the session ended)
dcl-proc rowsOf;
  dcl-pi *n int(20);
    k int(10) const;
    id int(10) const;
  end-pi;
  dcl-s n int(20);

  use(k);
  if k = 0;
    n = one('      count' : 'select count(*) from ' + locTab + ' where id = ' + %char(id)
            : *on);
  else;
    n = one('      count' : 'select count(*) from ' + cTab(k) + ' where id = ' + %char(id)
            : *off);
  endif;
  exec sql commit;
  return n;
end-proc;

dcl-proc rowsText;
  dcl-pi *n varchar(80);
    n int(20) const;
  end-pi;
  if n < 0;
    return 'gone (the session ended and its temporary table with it)';
  endif;
  return %char(n);
end-proc;

// One case.  act: what each connection does - index 1 = the local database, 2 and 3 =
// the servers: W a row is written, R rows are read, - nothing.
// lose: the server whose session ends (0 none).  again: one more statement for that
// server before the end.  toCommit: COMMIT or ROLLBACK at the end.
// wantFail: must the COMMIT fail; wantRows: the rows that must be there afterwards on
// each connection, -1 = gone with the session, -2 = either.
dcl-proc aCase;
  dcl-pi *n;
    letter char(1) const;
    title varchar(200) const;
    id int(10) const;
    act char(3) const;
    lose int(10) const;
    again ind const;
    toCommit ind const;
    wantFail ind const;
    wantLocal int(10) const;
    want1 int(10) const;
    want2 int(10) const;
    wantDb varchar(64) const;
  end-pi;
  dcl-s k int(10);
  dcl-s code int(10);
  dcl-s n int(20);
  dcl-s got int(20) dim(3);
  dcl-s want int(10) dim(3);
  dcl-s ok ind inz(*on);
  dcl-s failed ind;

  say(letter + '  ' + title);
  // a clean start: every table is there, nothing is open
  for k = 1 to nc;
    use(k);
    tempTable(k : wantDb);
  endfor;
  exec sql commit;
  if sqlcod < 0;
    note('      COMMIT before the case');
  endif;

  // the servers first, then the local table
  for k = 1 to nc;
    if %subst(act : k + 1 : 1) = 'W';
      put(k : id);
    elseif %subst(act : k + 1 : 1) = 'R';
      use(k);
      n = one('      read on ' + nameOf(cKind(k)) : 'select count(*) from ' + cTab(k) : *on);
    endif;
  endfor;
  if %subst(act : 1 : 1) = 'W';
    put(0 : id);
  endif;

  if lose > 0;
    endSession(lose);
    if again;
      use(lose);
      exe('      one more row on ' + nameOf(cKind(lose)) : 'insert into ' + cTab(lose)
          + ' values (' + %char(id) + ', ' + q + 'again' + q + ')' : *on);
    endif;
  endif;

  use(0);
  if toCommit;
    exec sql commit;
    code = note('      COMMIT');
  else;
    exec sql rollback;
    code = note('      ROLLBACK');
  endif;
  failed = (code < 0);
  if failed;
    if lose > 0;
      use(lose);
      lastError();
    endif;
    // what a program does after a COMMIT that failed
    use(0);
    exec sql rollback;
    note('      ROLLBACK after the failure');
  endif;

  got(1) = rowsOf(0 : id);
  want(1) = wantLocal;
  want(2) = want1;
  want(3) = want2;
  say('      rows of this case afterwards: local table ' + rowsText(got(1)));
  for k = 1 to nc;
    got(k + 1) = rowsOf(k : id);
    say('      rows of this case afterwards: ' + nameOf(cKind(k)) + ' ' + rowsText(got(k + 1)));
  endfor;
  for k = 1 to nc + 1;
    if want(k) <> -2 and got(k) <> want(k);
      ok = *off;
    endif;
  endfor;
  if failed <> wantFail;
    ok = *off;
  endif;
  if ok;
    say('      RIGHT');
  else;
    wrong += 1;
    if toCommit and not failed and wantFail;
      say('      WRONG: the COMMIT reported no error, but the rows on the server are lost'
          + ' and the others are committed');
    else;
      say('      WRONG: not what it should be');
    endif;
  endif;

  // the rows of the case are taken away again where the table still is
  use(0);
  exe('      cleaning the local table' : 'delete from ' + locTab : *off);
  exec sql commit;
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBLOST');
    pPg char(32) const;
    pMy char(32) const;
    pLib char(32) const;
    pFile char(32) const;
    pDb char(32) const options(*nopass);
  end-pi;
  dcl-s fname varchar(60);
  dcl-s wline varchar(400);
  dcl-s wantDb varchar(64) inz('');
  dcl-s s varchar(400);
  dcl-s k int(10);
  dcl-s i int(10);
  dcl-s tmp char(18);
  dcl-s n1 varchar(12);
  dcl-s n2 varchar(12);

  fname = %trim(pFile);
  if %parms() >= 5;
    wantDb = %trim(pDb);
  endif;
  locTab = %trim(pLib) + '.xdblostc';

  // the local database first: its name is needed for SET CONNECTION
  exec sql connect reset;
  note('CONNECT RESET (the local database)');
  exec sql values current server into :localRdb;
  exe('cleaning the local table' : 'delete from ' + locTab : *off);
  exec sql commit;

  if %trim(pPg) <> '*NONE';
    nc += 1;
    cRdb(nc) = pPg;
    cKind(nc) = 'PG';
  endif;
  if %trim(pMy) <> '*NONE';
    nc += 1;
    cRdb(nc) = pMy;
    cKind(nc) = 'MY';
  endif;
  for k = 1 to nc;
    curRdb = cRdb(k);
    exec sql connect to :curRdb;
    if note('CONNECT TO ' + %trim(curRdb) + ' (' + nameOf(cKind(k)) + ')') < 0;
      nc = 0;
      leave;
    endif;
    cTab(k) = '';
    if not tempTable(k : wantDb);
      nc = 0;
      leave;
    endif;
    exec sql commit;
  endfor;

  if nc = 1;
    n1 = nameOf(cKind(1));
    say('--- one server (' + n1 + ') and the local database in one unit of work');
    aCase('A' : 'nothing is lost: a row on ' + n1 + ', a local row, COMMIT'
          : 1 : 'WW-' : 0 : *off : *on : *off : 1 : 1 : 0 : wantDb);
    aCase('B' : 'a row on ' + n1 + ', a local row, the session ends, COMMIT'
          : 2 : 'WW-' : 1 : *off : *on : *on : 0 : -1 : 0 : wantDb);
    aCase('C' : n1 + ' is only read, a local row, the session ends, COMMIT'
          : 3 : 'WR-' : 1 : *off : *on : *off : 1 : -2 : 0 : wantDb);
    aCase('D' : 'a row on ' + n1 + ', a local row, the session ends, ROLLBACK'
          : 4 : 'WW-' : 1 : *off : *off : *off : 0 : -1 : 0 : wantDb);
    aCase('E' : 'a row on ' + n1 + ', a local row, the session ends, one more statement'
          + ' for ' + n1 + ', COMMIT all the same'
          : 5 : 'WW-' : 1 : *on : *on : *on : 0 : -2 : 0 : wantDb);
  elseif nc = 2;
    n1 = nameOf(cKind(1));
    n2 = nameOf(cKind(2));
    say('--- two servers (' + n1 + ' first, then ' + n2
        + ') and the local database in one unit of work');
    aCase('F' : 'nothing is lost: rows on both servers and a local row, COMMIT'
          : 6 : 'WWW' : 0 : *off : *on : *off : 1 : 1 : 1 : wantDb);
    aCase('G' : 'rows everywhere, the session on ' + n2 + ' ends, COMMIT'
          : 7 : 'WWW' : 2 : *off : *on : *on : 0 : 0 : -1 : wantDb);
    aCase('H' : 'rows everywhere, the session on ' + n1 + ' ends, COMMIT'
          : 8 : 'WWW' : 1 : *off : *on : *on : 0 : -1 : 0 : wantDb);
    aCase('I' : n2 + ' is only read, rows on ' + n1 + ' and locally, the session on ' + n2
          + ' ends, COMMIT'
          : 9 : 'WWR' : 2 : *off : *on : *off : 1 : 1 : -2 : wantDb);

  endif;

  if nc > 0;
    for k = 1 to nc;
      use(k);
      counters('the driver about ' + nameOf(cKind(k)) + ':');
    endfor;
    exec sql commit;
    if wrong = 0;
      say('=== every case is right');
    else;
      say('=== ' + %char(wrong) + ' cases are WRONG');
    endif;
  endif;

  // ---- the end: back to the local database, then every server connection is ended
  exec sql connect reset;
  exec sql rollback;
  for k = 1 to nc;
    tmp = cRdb(k);
    exec sql disconnect :tmp;
    note('DISCONNECT from ' + nameOf(cKind(k)));
  endfor;
  s = 'call qsys2.ifs_write(path_name => ?, line => ?, overwrite => ' + q + 'APPEND' + q
    + ', end_of_line => ' + q + 'LF' + q + ')';
  exec sql prepare w1 from :s;
  for i = 1 to nl;
    wline = lines(i);
    exec sql execute w1 using :fname, :wline;
  endfor;
  exec sql commit;
  return;
end-proc;

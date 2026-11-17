**free
// XDBINFO - what a connection is: a sample and a test of the statement XDB CONNECTION INFO.
//
// The driver answers that statement itself, on any connection, without asking the server:
// rows of AREA, NAME and VALUE that tell where the connection leads, what the server said
// about itself, the state of the session, the driver's own counters, settings and limits.
// In interactive SQL it is written  SELECT * FROM XDB_CONNECTION_INFO.
// It is meant for finding the cause of a problem and for a program that wants to know what
// kind of server it talks to.  The rows can change from one version of the driver to the
// next: take the ones you need by name and expect that a name may be missing.
//
// This program prints all rows of a new connection, runs one query and a COMMIT, prints the
// session and the counters again, and shows how a program picks single values.
//
//   CALL XDBINFO PARM('<rdb>' '<output file>')
//
// compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

// one block of rows of the statement
dcl-ds info qualified dim(100);
  area varchar(12);
  name varchar(40);
  value varchar(250);
end-ds;

dcl-s lines varchar(320) dim(400);
dcl-s nl int(10) inz(0);
// values a program may want: taken from the rows by name
dcl-s dbType varchar(40) inz('');
dcl-s dbVersion varchar(100) inz('');
dcl-s verNumber int(10) inz(0);
dcl-s uowOpen ind inz(*off);
dcl-s reconnects int(10) inz(0);

// one line of the result with the SQLCODE of the statement before it; gives the SQLCODE back
dcl-proc note;
  dcl-pi *n int(10);
    what varchar(200) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(200) inz('');

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
    what varchar(320) const;
  end-pi;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = what;
  endif;
end-proc;

// Runs the statement and prints the rows of the areas named in areas (all when empty).
// The values this program wants are kept on the way.
dcl-proc showInfo;
  dcl-pi *n;
    title varchar(80) const;
    areas varchar(80) const;
  end-pi;
  dcl-s stm varchar(40) inz('XDB CONNECTION INFO');
  dcl-s want int(10) inz(100);
  dcl-s got int(10);
  dcl-s total int(10) inz(0);
  dcl-s i int(10);
  dcl-s atEnd ind inz(*off);
  dcl-s a char(11);
  dcl-s n char(30);

  exec sql declare ci cursor for ciStmt;
  exec sql prepare ciStmt from :stm;
  if sqlcod < 0;
    note(title + ' (prepare)');
    return;
  endif;
  exec sql open ci;
  if sqlcod < 0;
    note(title + ' (open)');
    return;
  endif;
  say('--- ' + title);
  dow not atEnd;
    exec sql fetch ci for :want rows into :info;
    if sqlcod < 0;
      note(title + ' (fetch)');
      leave;
    endif;
    got = sqlerrd(3);
    atEnd = (sqlcod = 100);
    for i = 1 to got;
      if areas = '' or %scan(info(i).area : areas) > 0;
        a = info(i).area;
        n = info(i).name;
        say('  ' + a + n + info(i).value);
      endif;
      // single values, by area and name
      if info(i).area = 'connection' and info(i).name = 'database_type';
        dbType = info(i).value;
      elseif info(i).area = 'server' and info(i).name = 'version';
        dbVersion = info(i).value;
      elseif info(i).area = 'server' and info(i).name = 'version_number';
        monitor;
          verNumber = %int(info(i).value);
        on-error;
          verNumber = 0;
        endmon;
      elseif info(i).area = 'session' and info(i).name = 'unit_of_work';
        uowOpen = (info(i).value = 'open');
      elseif info(i).area = 'counters' and info(i).name = 'reconnects';
        monitor;
          reconnects = %int(info(i).value);
        on-error;
          reconnects = 0;
        endmon;
      endif;
    endfor;
    total += got;
  enddo;
  exec sql close ci;
  say('  (' + %char(total) + ' rows in all)');
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBINFO');
    pRdb char(32) const;
    pFile char(32) const;
  end-pi;
  dcl-s rdb char(18);
  dcl-s fname varchar(60);
  dcl-s wline varchar(320);
  dcl-s s varchar(300);
  dcl-s one int(10);
  dcl-s oneNull int(5);
  dcl-s i int(10);
  dcl-s q varchar(1) inz('''');

  rdb = pRdb;
  fname = %trim(pFile);

  exec sql connect to :rdb;
  if note('CONNECT TO ' + %trim(rdb)) < 0;
    exsr report;
    return;
  endif;

  showInfo('a new connection: everything' : '');

  // a query every kind of server takes, then a COMMIT: the counters and the session tell
  s = 'select 1';
  exec sql prepare q1 from :s;
  exec sql declare c1 cursor for q1;
  exec sql open c1;
  exec sql fetch c1 into :one :oneNull;
  note('one query (select 1)');
  exec sql close c1;
  showInfo('after one query: session and counters' : 'session counters');
  if uowOpen;
    say('  so a unit of work is open: COMMIT or ROLLBACK before DISCONNECT');
  else;
    say('  so no unit of work is open on the server');
  endif;
  exec sql commit;
  note('COMMIT');
  showInfo('after the COMMIT: session and counters' : 'session counters');
  if uowOpen;
    say('  so a unit of work is open: COMMIT or ROLLBACK before DISCONNECT');
  else;
    say('  so no unit of work is open on the server');
  endif;

  say('--- what a program can take from it');
  say('  kind of server: ' + dbType);
  say('  its version: ' + dbVersion + ' (as a number: ' + %char(verNumber) + ')');
  say('  times the connection was opened again: ' + %char(reconnects));
  if dbType = 'PostgreSQL';
    say('  so text is put together with ||, and a row generator is generate_series');
  elseif dbType = 'MySQL';
    say('  so text is put together with CONCAT(), and names can stand in backticks');
  elseif dbType = 'SQL Server';
    say('  so text is put together with +, and a temporary table is named #name');
  endif;

  exec sql commit;
  exec sql disconnect :rdb;
  note('DISCONNECT');
  exsr report;
  return;

  begsr report;
    exec sql connect reset;
    s = 'call qsys2.ifs_write(path_name => ?, line => ?, overwrite => ' + q + 'APPEND' + q
      + ', end_of_line => ' + q + 'LF' + q + ')';
    exec sql prepare w1 from :s;
    for i = 1 to nl;
      wline = lines(i);
      exec sql execute w1 using :fname, :wline;
    endfor;
    exec sql commit;
  endsr;
end-proc;

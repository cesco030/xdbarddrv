**free
// XDBDEMO - embedded SQL on a PostgreSQL database through the XDB driver.
//
// Lists the tables of one PostgreSQL schema, biggest first, on QSYSPRT:
// name, live rows, size in MB and the time of the last automatic analyze.
//
//   CALL MYLIB/XDBDEMO PARM('MYPG')                schema "public"
//   CALL MYLIB/XDBDEMO PARM('MYPG' 'pg_catalog')   another schema (case sensitive)
//
// MYPG stands for the name of a connection profile of type PG (command
// WRKXDBPRF) and of its RDB directory entry (option 8 of that command).
//
// Compile:
//   CRTSQLRPGI OBJ(MYLIB/XDBDEMO) SRCSTMF('.../xdbdemo.sqlrpgle')
//              COMMIT(*CHG) RDB(MYPG) OPTION(*SQL) CVTCCSID(*JOB)
//
// The statement goes to PostgreSQL as written, so it is PostgreSQL syntax,
// with ? where a host variable is used. PREPARE + OPEN ... USING is the
// form to use: static SQL with :hostvar inside the statement does not
// compile against this driver yet.

ctl-opt main(pMain) dftactgrp(*no) actgrp(*new) option(*srcstmt : *nodebugio);

dcl-f qsysprt printer(132) usropn;

// one print line; numbers are right aligned in their columns
dcl-ds line len(132) qualified;
  name char(40);
  liveRows char(19);
  sizeMb char(18);
  *n char(2);
  analyzed char(26);
end-ds;

// ---------------------------------------------------------------- main

dcl-proc pMain;
  dcl-pi *n extpgm('XDBDEMO');
    pRdb char(32) const;
    pSchema char(32) const options(*nopass);
  end-pi;

  // rows and their null indicators, filled 50 at a time by FETCH
  dcl-ds rows dim(50) qualified;
    name varchar(63);
    liveRows int(20);
    sizeMb packed(13 : 2);
    analyzed timestamp;
  end-ds;
  dcl-ds inds dim(50) qualified;
    col int(5) dim(4);
  end-ds;
  dcl-s rdb char(18);
  dcl-s schema varchar(63) inz('public');
  dcl-s minRows int(20) inz(0);
  dcl-s wanted int(10) inz(50);
  dcl-s got int(10);
  dcl-s total int(10) inz(0);
  dcl-s i int(10);
  dcl-s atEnd ind inz(*off);
  dcl-s stmt varchar(500);

  exec sql set option datfmt = *iso, closqlcsr = *endmod;

  rdb = pRdb;
  if %parms() >= 2;
    schema = %trim(pSchema);
  endif;

  open qsysprt;

  exec sql connect to :rdb;
  if failed('CONNECT');
    close qsysprt;
    return;
  endif;

  stmt = 'select relname, n_live_tup, '
       + 'cast(pg_total_relation_size(relid) / 1048576.0 as numeric(13, 2)), '
       + 'last_autoanalyze '
       + 'from pg_stat_user_tables '
       + 'where schemaname = ? and n_live_tup >= ? '
       + 'order by 3 desc';
  exec sql prepare tabsStmt from :stmt;
  if failed('PREPARE');
    exec sql rollback;
    exec sql connect reset;
    exec sql disconnect :rdb;
    close qsysprt;
    return;
  endif;
  exec sql declare tabs cursor for tabsStmt;

  exec sql open tabs using :schema, :minRows;
  if not failed('OPEN');
    print('Tables of schema ' + schema);
    clear line;
    line.name = 'Table';
    evalr line.liveRows = 'Rows';
    evalr line.sizeMb = 'Size MB';
    line.analyzed = 'Last auto analyze';
    write qsysprt line;

    dow not atEnd;
      // one FETCH brings up to 50 rows in a single exchange with the server
      exec sql fetch tabs for :wanted rows into :rows :inds;
      if failed('FETCH');
        leave;
      endif;
      got = sqlerrd(3);
      atEnd = (sqlcode = 100);

      for i = 1 to got;
        clear line;
        line.name = rows(i).name;
        evalr line.liveRows = %editc(%dec(rows(i).liveRows : 15 : 0) : '1');
        evalr line.sizeMb = %editc(rows(i).sizeMb : '1');
        if inds(i).col(4) < 0;
          line.analyzed = 'never';
        else;
          line.analyzed = %char(rows(i).analyzed);
        endif;
        write qsysprt line;
      endfor;
      total += got;
    enddo;

    print(%char(total) + ' tables');
    exec sql close tabs;
  endif;

  // end the unit of work, go back to the local database, then end the
  // PostgreSQL connection
  exec sql commit;
  exec sql connect reset;
  exec sql disconnect :rdb;

  close qsysprt;
  return;
end-proc;

// ------------------------------------------------------------- helpers

// True when the last SQL statement failed; the error goes on the report.
// GET DIAGNOSTICS gives Db2's message for it, with the name the server
// complained about. The statement XDB LAST ERROR (prepare, open, fetch)
// returns the complete text of the server and its own error number.
dcl-proc failed;
  dcl-pi *n ind;
    what varchar(20) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(110);

  if sqlcode >= 0;
    return *off;
  endif;
  code = sqlcode;
  state = sqlstate;
  exec sql get diagnostics condition 1 :text = MESSAGE_TEXT;
  print(what + ' failed: SQLCODE ' + %char(code) + ' SQLSTATE ' + state);
  print('  ' + text);
  return *on;
end-proc;

dcl-proc print;
  dcl-pi *n;
    text varchar(132) const;
  end-pi;

  dcl-ds full len(132) qualified;
    text char(132);
  end-ds;

  full.text = text;
  write qsysprt full;
end-proc;

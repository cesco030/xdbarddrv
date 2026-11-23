**free
// XDBT32 - the file functions of XDB on a MySQL, SQL Server or PostgreSQL database: bytes from two
//   stream files into a temporary table and back to files, text made on the server to a
//   UTF-8 file and to a file of CCSID 280, read back from both and compared on the server,
//   and a few things that have to fail.  Only a temporary table is used; the unit of work is
//   rolled back.  The files <dir>/in_small.bin and <dir>/in_big.bin must exist (x32.sh makes
//   them); what is written goes to <dir>/back_<id>.bin, back_<id>.txt, out_text.txt,
//   out_280.txt.  The script compares the files afterwards.
//   CALL XDBT32 PARM('<rdb>' 'MYSQL', 'MSSQL' or 'PG' '<dir>' '<output file>' '<database>')
//     (literals of 32 at most).  database: where MySQL keeps the temporary table when the
//     connection has no database of its own; blank = the first one of the server that lets
//     this user create a temporary table.  A temporary table is seen by this session only.
// compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

dcl-s lines varchar(400) dim(80);
dcl-s nl int(10) inz(0);

dcl-proc note;
  dcl-pi *n;
    what varchar(200) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(300) inz('');

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

// the time since t0, in milliseconds
dcl-proc elapsed;
  dcl-pi *n varchar(20);
    t0 timestamp const;
  end-pi;
  return %char(%div(%diff(%timestamp() : t0 : *mseconds) : 1000)) + ' ms';
end-proc;

// a query with one row and one number
dcl-proc num;
  dcl-pi *n int(20);
    label varchar(100) const;
    stm varchar(3000) const;
  end-pi;
  dcl-s s varchar(3000);
  dcl-s n int(20) inz(-1);
  dcl-s ni int(5) inz(0);
  dcl-s t0 timestamp;

  exec sql declare c1 cursor for q1;
  t0 = %timestamp();
  s = stm;
  exec sql prepare q1 from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return -1;
  endif;
  exec sql open c1;
  if sqlcod < 0;
    note(label + ' (open)');
    return -1;
  endif;
  exec sql fetch c1 into :n :ni;
  if sqlcod < 0;
    note(label + ' (fetch)');
    exec sql close c1;
    return -1;
  endif;
  exec sql close c1;
  say(label + ': ' + %char(n) + '   (' + elapsed(t0) + ')');
  return n;
end-proc;

// a query with one row and one text
dcl-proc txt;
  dcl-pi *n;
    label varchar(100) const;
    stm varchar(3000) const;
  end-pi;
  dcl-s s varchar(3000);
  dcl-s v varchar(200) inz('');
  dcl-s vi int(5) inz(0);

  exec sql declare c4 cursor for q4;
  s = stm;
  exec sql prepare q4 from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return;
  endif;
  exec sql open c4;
  if sqlcod < 0;
    note(label + ' (open)');
    return;
  endif;
  exec sql fetch c4 into :v :vi;
  if sqlcod < 0;
    note(label + ' (fetch)');
    exec sql close c4;
    return;
  endif;
  exec sql close c4;
  say(label + ': ' + v);
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBT32');
    pRdb char(32) const;
    pKind char(32) const;
    pDir char(32) const;
    pFile char(32) const;
    pDb char(32) const;
  end-pi;
  dcl-s rdb char(18);
  dcl-s dbs varchar(64) dim(40);
  dcl-s ndb int(10) inz(0);
  dcl-s db varchar(64);
  dcl-s cols varchar(80) inz(' (id int primary key, body longblob, note longtext)');
  dcl-s dir varchar(40);
  dcl-s fname varchar(60);
  dcl-s wline varchar(400);
  dcl-s my ind;
  dcl-s pg ind;
  dcl-s lo0 int(20) inz(0);
  dcl-s lo9 int(20) inz(0);
  dcl-s tab varchar(90);
  dcl-s t1 varchar(400);
  dcl-s t2 varchar(400);
  dcl-s s varchar(3000);
  dcl-s path varchar(120);
  dcl-s pre varchar(120);
  dcl-s i int(10);
  dcl-s id int(10);
  dcl-s n1 int(20);
  dcl-s n2 int(20);
  dcl-s i1 int(5);
  dcl-s i2 int(5);
  dcl-s hash varchar(64);
  dcl-s h8 varchar(64);
  dcl-s hi int(5);
  dcl-s t0 timestamp;
  dcl-s q varchar(1) inz('''');

  rdb = pRdb;
  dir = %trim(pDir);
  fname = %trim(pFile);
  my = (%trim(pKind) = 'MYSQL');
  pg = (%trim(pKind) = 'PG');

  exec sql connect to :rdb;
  note('connect');
  if sqlcod < 0;
    exsr report;
    return;
  endif;

  // the text of the tests: accents, the euro sign and a character beyond the 16 bit
  // range in t1; in t2 only what CCSID 280 has.  Lines end with a line feed.
  if my;
    tab = 'xdbt32';
    t1 = 'repeat(convert(unhex(' + q + '43697474C3A0207065726368C3A920E282AC20F09F98800A'
       + q + ') using utf8mb4), 20000)';
    t2 = 'repeat(convert(unhex(' + q + '43697474C3A0207065726368C3A90A' + q
       + ') using utf8mb4), 20000)';
    txt('server' : 'select concat(version(), ' + q + ', max_allowed_packet ' + q
        + ', @@max_allowed_packet)');
    // a temporary table needs a database: the one of the connection, the one asked
    // for, or the first that lets this user create one
    db = %trim(pDb);
    if db = '';
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
      s = 'create temporary table xdbt32' + cols;
      exec sql execute immediate :s;
      if sqlcod < 0;
        tab = '';
        for i = 1 to ndb;
          s = 'create temporary table `' + dbs(i) + '`.xdbt32' + cols;
          exec sql execute immediate :s;
          if sqlcod = 0;
            tab = '`' + dbs(i) + '`.xdbt32';
            say('the connection has no database: temporary table in database number '
                + %char(i) + ' of ' + %char(ndb) + ' the server lists, ' + dbs(i));
            leave;
          endif;
        endfor;
        if tab = '';
          note('no database of ' + %char(ndb) + ' takes a temporary table; the last one said');
          exec sql rollback;
          exec sql disconnect :rdb;
          exsr report;
          return;
        endif;
      endif;
    else;
      tab = '`' + db + '`.xdbt32';
      s = 'create temporary table ' + tab + cols;
      exec sql execute immediate :s;
    endif;
    s = '';
  elseif pg;
    tab = 'xdbt32';
    t1 = 'repeat(' + q + 'Citt' + q + ' || chr(224) || ' + q + ' perch' + q + ' || chr(233) || '
       + q + ' ' + q + ' || chr(8364) || ' + q + ' ' + q
       + ' || chr(128512) || chr(10), 20000)';
    t2 = 'repeat(' + q + 'Citt' + q + ' || chr(224) || ' + q + ' perch' + q
       + ' || chr(233) || chr(10), 20000)';
    txt('server' : 'select ' + q + 'PostgreSQL ' + q + ' || current_setting(' + q
        + 'server_version' + q + ')');
    s = 'create temp table xdbt32 (id int primary key, body bytea, note text)';
  else;
    tab = '#xdbt32';
    t1 = 'replicate(cast(N' + q + 'Citt' + q + ' + nchar(224) + N' + q + ' perch' + q
       + ' + nchar(233) + N' + q + ' ' + q + ' + nchar(8364) + N' + q + ' ' + q
       + ' + nchar(55357) + nchar(56832) + nchar(10) as nvarchar(max)), 20000)';
    t2 = 'replicate(cast(N' + q + 'Citt' + q + ' + nchar(224) + N' + q + ' perch' + q
       + ' + nchar(233) + nchar(10) as nvarchar(max)), 20000)';
    txt('server' : 'select cast(serverproperty(' + q + 'ProductVersion' + q
        + ') as varchar(30)) + ' + q + ' ' + q + ' + cast(serverproperty(' + q + 'Edition'
        + q + ') as varchar(60))');
    s = 'create table #xdbt32 (id int primary key, body varbinary(max), note nvarchar(max))';
  endif;
  if s <> '';
    exec sql execute immediate :s;
  endif;
  note('temporary table ' + tab);
  if pg;
    lo0 = num('large objects on the server before' :
              'select count(*) from pg_largeobject_metadata');
  endif;

  // ---- bytes: from files into the table
  s = 'insert into ' + tab + ' (id, body) values(?, get_blob_from_file(?))';
  exec sql prepare s1 from :s;
  note('prepare the insert with GET_BLOB_FROM_FILE');
  id = 1;
  path = dir + '/in_small.bin';
  exec sql execute s1 using :id, :path;
  note('row 1 from in_small.bin');
  id = 2;
  path = dir + '/in_big.bin';
  t0 = %timestamp();
  exec sql execute s1 using :id, :path;
  note('row 2 from in_big.bin (' + elapsed(t0) + ')');
  path = dir + '/in_small.bin';
  exec sql execute s1 using :id, :path;
  note('row 2 once more: a duplicate key after the file was sent');
  id = 3;
  path = dir + '/nofile.bin';
  exec sql execute s1 using :id, :path;
  note('row 3 from a file that does not exist');
  path = dir + '/in_small.bin';
  exec sql execute s1 using :id, :path;
  note('row 3 from in_small.bin: the statement still works');

  // ---- what the server has
  if my;
    // SHA-256 there: MySQL 9 has MD5() only in a component that is not installed with it
    s = 'select id, length(body), sha2(body, 256) from ' + tab
      + ' where body is not null order by id';
  elseif pg;
    s = 'select id, octet_length(body), md5(body) from xdbt32 where body is not null order by id';
  else;
    s = 'select id, datalength(body), lower(convert(varchar(32), hashbytes(' + q + 'MD5' + q
      + ', cast(substring(body, 1, 8000) as varbinary(8000))), 2)) from #xdbt32'
      + ' where body is not null order by id';
  endif;
  exec sql declare c2 cursor for q2;
  exec sql prepare q2 from :s;
  exec sql open c2;
  if sqlcod < 0;
    note('rows of the table (open)');
  else;
    dow 1 = 1;
      exec sql fetch c2 into :id, :n1 :i1, :hash :hi;
      if sqlcod <> 0;
        if sqlcod < 0;
          note('rows of the table (fetch)');
        endif;
        leave;
      endif;
      if my;
        say('  row ' + %char(id) + ': ' + %char(n1) + ' bytes, sha256 ' + hash);
      elseif pg;
        say('  row ' + %char(id) + ': ' + %char(n1) + ' bytes, md5 ' + hash);
      else;
        say('  row ' + %char(id) + ': ' + %char(n1) + ' bytes, md5 of the first 8000 ' + hash);
      endif;
    enddo;
    exec sql close c2;
  endif;
  if not my and not pg;
    // the whole value can be hashed from SQL Server 2016 on; before that this fails
    txt('  row 2, md5 of all of it' : 'select lower(convert(varchar(32), hashbytes(' + q
        + 'MD5' + q + ', body), 2)) from #xdbt32 where id = 2');
  endif;

  // ---- text: made on the server, to a UTF-8 file and to a file of CCSID 280
  s = 'select put_clob_to_file(' + t1 + ', ' + q + dir + '/out_text.txt' + q
    + '), put_clob_to_file(' + t2 + ', ' + q + dir + '/out_280.txt' + q + ', 16, 280) from '
    + tab + ' where id = 1';
  exec sql declare c5 cursor for q5;
  exec sql prepare q5 from :s;
  note('prepare the query with PUT_CLOB_TO_FILE');
  exec sql open c5;
  if sqlcod < 0;
    note('text to files (open)');
  else;
    exec sql fetch c5 into :n1 :i1, :n2 :i2;
    if sqlcod < 0;
      note('text to files (fetch)');
    else;
      say('text to files: out_text.txt ' + %char(n1) + ' bytes, out_280.txt ' + %char(n2)
          + ' bytes');
    endif;
    exec sql close c5;
  endif;

  // ---- text: from the two files into the table, compared with what the server made
  s = 'insert into ' + tab + ' (id, note) values(?, get_clob_from_file(?))';
  exec sql prepare s2 from :s;
  id = 11;
  path = dir + '/out_text.txt';
  exec sql execute s2 using :id, :path;
  note('row 11 from out_text.txt');
  id = 12;
  path = dir + '/out_280.txt';
  exec sql execute s2 using :id, :path;
  note('row 12 from out_280.txt');
  if my;
    num('texts in the table that are what the server made (2 = both)' :
        'select count(*) from ' + tab + ' where (id = 11 and sha2(note, 256) = sha2(' + t1
        + ', 256)) or (id = 12 and sha2(note, 256) = sha2(' + t2 + ', 256))');
    num('characters in row 11' : 'select char_length(note) from ' + tab + ' where id = 11');
  elseif pg;
    num('texts in the table that are what the server made (2 = both)' :
        'select count(*) from xdbt32 where (id = 11 and note = ' + t1
        + ') or (id = 12 and note = ' + t2 + ')');
    num('characters in row 11' : 'select char_length(note) from xdbt32 where id = 11');
  else;
    num('texts in the table that are what the server made (2 = both)' :
        'select count(*) from #xdbt32 where (id = 11 and cast(note as varbinary(max)) = cast('
        + t1 + ' as varbinary(max))) or (id = 12 and cast(note as varbinary(max)) = cast('
        + t2 + ' as varbinary(max)))');
    num('bytes of row 11 on the server (UTF-16)' :
        'select datalength(note) from #xdbt32 where id = 11');
  endif;

  // ---- everything back to files, the names made from a host variable and the row
  if my;
    s = 'select id, put_blob_to_file(body, concat(?, id, ' + q + '.bin' + q
      + ')), put_clob_to_file(note, concat(?, id, ' + q + '.txt' + q
      + ')) from ' + tab + ' order by id';
  elseif pg;
    s = 'select id, put_blob_to_file(body, ?::text || id::text || ' + q + '.bin' + q
      + '), put_clob_to_file(note, ?::text || id::text || ' + q + '.txt' + q
      + ') from xdbt32 order by id';
  else;
    s = 'select id, put_blob_to_file(body, ? + cast(id as varchar(10)) + ' + q + '.bin' + q
      + '), put_clob_to_file(note, ? + cast(id as varchar(10)) + ' + q + '.txt' + q
      + ') from #xdbt32 order by id';
  endif;
  pre = dir + '/back_';
  exec sql declare c3 cursor for q3;
  exec sql prepare q3 from :s;
  note('prepare the query that writes all rows');
  t0 = %timestamp();
  exec sql open c3 using :pre, :pre;
  if sqlcod < 0;
    note('rows to files (open)');
  else;
    dow 1 = 1;
      exec sql fetch c3 into :id, :n1 :i1, :n2 :i2;
      if sqlcod <> 0;
        if sqlcod < 0;
          note('rows to files (fetch)');
        endif;
        leave;
      endif;
      wline = '  row ' + %char(id) + ':';
      if i1 < 0;
        wline += ' no bytes,';
      else;
        wline += ' ' + %char(n1) + ' bytes to back_' + %char(id) + '.bin,';
      endif;
      if i2 < 0;
        wline += ' no text';
      else;
        wline += ' ' + %char(n2) + ' bytes to back_' + %char(id) + '.txt';
      endif;
      say(wline);
    enddo;
    exec sql close c3;
    say('rows to files: ' + elapsed(t0));
  endif;

  // ---- a file that cannot be written
  num('a file in a directory that does not exist' : 'select put_blob_to_file(body, ' + q + dir
      + '/nodir/x.bin' + q + ') from ' + tab + ' where id = 1');

  if pg;
    lo9 = num('large objects on the server after' :
              'select count(*) from pg_largeobject_metadata');
    say('large objects left behind by this run: ' + %char(lo9 - lo0));
  endif;
  exec sql rollback;
  note('rollback');
  if my;
    s = 'drop temporary table if exists ' + tab;
    exec sql execute immediate :s;
    note('drop the temporary table');
    exec sql commit;
  endif;
  exec sql disconnect :rdb;
  exsr report;
  return;

  begsr report;
    exec sql connect reset;
    // the same checksums of the files, worked out here by Db2
    for i = 1 to 2;
      if i = 1;
        path = dir + '/in_small.bin';
      else;
        path = dir + '/in_big.bin';
      endif;
      hash = '';
      h8 = '';
      if my;
        exec sql select lower(hex(hash_sha256(line))),
                        lower(hex(hash_sha256(substr(line, 1, min(8000, length(line))))))
                   into :hash, :h8
                   from table(qsys2.ifs_read_binary(path_name => :path))
                   fetch first 1 row only;
      else;
        exec sql select lower(hex(hash_md5(line))),
                        lower(hex(hash_md5(substr(line, 1, min(8000, length(line))))))
                   into :hash, :h8
                   from table(qsys2.ifs_read_binary(path_name => :path))
                   fetch first 1 row only;
      endif;
      if sqlcod < 0;
        note('checksum of ' + path);
      elseif my;
        say('here, ' + path + ': sha256 ' + hash + ', sha256 of the first 8000 ' + h8);
      else;
        say('here, ' + path + ': md5 ' + hash + ', md5 of the first 8000 ' + h8);
      endif;
    endfor;
    for i = 1 to nl;
      wline = lines(i);
      exec sql call qsys2.ifs_write(path_name => :fname, line => :wline,
                                    overwrite => 'APPEND', end_of_line => 'LF');
    endfor;
    exec sql commit;
  endsr;
end-proc;

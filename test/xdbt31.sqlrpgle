**free
// XDBT31 - one large value between a stream file and an XDB database, of any size: made on
//   the server, written to a file with PUT_BLOB_TO_FILE, read back with GET_BLOB_FROM_FILE
//   into a temporary table, and compared there with what the server made (MD5).  The value
//   is written once and read once, so a value above option lob_direct passes twice through
//   the staging of the backend.  The unit of work is rolled back; the file stays.
//   CALL XDBT31 PARM('<rdb>' '<file>' '<rows>' '<output file>')   (literals of 32 at most)
//     rows: the value has 16 bytes per row (1310720 = 20 MB)
//   The statements are PostgreSQL's (generate_series, string_agg, decode).
// compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

dcl-s lines varchar(400) dim(30);
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
    label varchar(80) const;
    stm varchar(1000) const;
  end-pi;
  dcl-s s varchar(1000);
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
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = label + ': ' + %char(n) + '   (' + elapsed(t0) + ')';
  endif;
  return n;
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBT31');
    pRdb char(32) const;
    pPath char(32) const;
    pRows char(32) const;
    pFile char(32) const;
  end-pi;
  dcl-s rdb char(18);
  dcl-s path varchar(40);
  dcl-s big varchar(12);
  dcl-s fname varchar(60);
  dcl-s wline varchar(400);
  dcl-s i int(10);
  dcl-s id int(10) inz(1);
  dcl-s s varchar(1000);
  dcl-s gen varchar(200);
  dcl-s lo0 int(20);
  dcl-s lo9 int(20);
  dcl-s t0 timestamp;
  dcl-s q varchar(1) inz('''');

  rdb = pRdb;
  path = %trim(pPath);
  big = %trim(pRows);
  fname = %trim(pFile);
  gen = 'decode(string_agg(md5(i::text), ' + q + q + ' order by i), ' + q + 'hex' + q + ')';

  exec sql connect to :rdb;
  note('connect');
  if sqlcod < 0;
    exsr report;
    return;
  endif;
  s = 'create temp table xdbt31 (id int primary key, body bytea)';
  exec sql execute immediate :s;
  note('temporary table');
  lo0 = num('large objects on the server before' : 'select count(*) from pg_largeobject_metadata');

  num('bytes written to the file' : 'select put_blob_to_file(' + gen + ', ' + q + path + q
      + ') from generate_series(1, ' + big + ') as g(i)');

  s = 'insert into xdbt31 (id, body) values(?, get_blob_from_file(?))';
  exec sql prepare s1 from :s;
  t0 = %timestamp();
  exec sql execute s1 using :id, :path;
  note('insert from the file (' + elapsed(t0) + ')');

  num('bytes in the table' : 'select octet_length(body) from xdbt31 where id = 1');
  num('the value in the table is what the server made (1 = yes)' :
      'select count(*) from xdbt31 where id = 1 and md5(body) = (select md5(' + gen
      + ') from generate_series(1, ' + big + ') as g(i))');

  lo9 = num('large objects on the server after' : 'select count(*) from pg_largeobject_metadata');
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = 'large objects left behind by this run: ' + %char(lo9 - lo0);
  endif;
  exec sql rollback;
  note('rollback');
  exec sql disconnect :rdb;
  exsr report;
  return;

  begsr report;
    exec sql connect reset;
    for i = 1 to nl;
      wline = lines(i);
      exec sql call qsys2.ifs_write(path_name => :fname, line => :wline,
                                    overwrite => 'APPEND', end_of_line => 'LF');
    endfor;
    exec sql commit;
  endsr;
end-proc;

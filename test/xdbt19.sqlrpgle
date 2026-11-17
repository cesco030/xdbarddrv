**free
// XDBT19 - batch driver of the setup module XDBSETUP: what WRKXDBPRF shows
// and does on its Setup display, without a display.
//   CALL XDBT19 PARM(library owner mode output)
//   mode C = check only
//        A = run what is to do, except everything about the owner
//        O = as A, and give driver and password list to the owner (the
//            owner profile itself is never created from here)
//   owner blank = the default of the setup
// Result in the stream file named by output.
// compile: CRTSQLRPGI OBJTYPE(*MODULE) COMMIT(*NONE), bound with XDBSETUP
ctl-opt main(pMain);

/copy QXDBSRC,XDBSETUPH

dcl-s lines varchar(300) dim(300);
dcl-s nl int(10) inz(0);

dcl-proc add;
  dcl-pi *n;
    text varchar(300) const;
  end-pi;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = text;
  endif;
end-proc;

dcl-proc hexOf;
  dcl-pi *n varchar(120);
    part char(60) const;
  end-pi;
  dcl-s h varchar(120);
  dcl-s p char(60);
  p = part;
  exec sql set option closqlcsr = *endmod;
  exec sql values hex(cast(:p as char(60) for bit data)) into :h;
  if sqlcod <> 0;
    return 'hex failed ' + %char(sqlcod);
  endif;
  return h;
end-proc;

dcl-proc show;
  dcl-pi *n;
    title varchar(60) const;
    lib char(10) const;
    owner char(10) const;
  end-pi;
  dcl-ds items likeds(xsItem_t) dim(XS_MAXITEMS);
  dcl-s count int(10);
  dcl-s i int(10);
  dcl-s j int(10);

  xs_check(lib : owner : items : count);
  add('--- ' + title + ' (' + %char(count) + ' lines)');
  for i = 1 to count;
    add(items(i).state + ' ' + items(i).id + ' ' + items(i).text + ' ' + %trim(items(i).status));
    if items(i).note <> '';
      add('     note: ' + items(i).note);
    endif;
    for j = 1 to items(i).ncmd;
      if items(i).manual;
        add('     shown only: ' + items(i).cmd(j));
      else;
        add('     cmd: ' + items(i).cmd(j));
      endif;
    endfor;
  endfor;
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBT19');
    pLib char(10) const;
    pOwner char(10) const;
    pMode char(1) const;
    pOut char(32) const;
  end-pi;
  dcl-ds items likeds(xsItem_t) dim(XS_MAXITEMS);
  dcl-s count int(10);
  dcl-s i int(10);
  dcl-s j int(10);
  dcl-s raw char(240);
  dcl-s msgId char(7);
  dcl-s msgText varchar(132);
  dcl-s wline varchar(300);
  dcl-s wpath varchar(32);
  dcl-s wmode varchar(10) inz('REPLACE');

  add('library ' + %trim(pLib) + ', owner asked [' + %trim(pOwner) + '], mode ' + pMode);
  add('version: ' + xs_version(pLib));
  add('default owner: ' + xs_defaultOwner(pLib));
  if xs_missing(pLib);
    add('profile files or dictionary missing: the list cannot be shown yet');
  else;
    add('profile files and dictionary are there');
  endif;
  add('lines to do or failing: ' + %char(xs_open(pLib)));

  raw = xs_rawSrvpgm(pLib : 'XDB');
  add('service program XDB, raw: owner [' + %subst(raw : 29 : 10) + '] at 138 ['
      + %subst(raw : 138 : 1) + '] at 214 [' + %subst(raw : 214 : 1) + ']');
  add('  000 ' + hexOf(%subst(raw : 1 : 60)));
  add('  060 ' + hexOf(%subst(raw : 61 : 60)));
  add('  120 ' + hexOf(%subst(raw : 121 : 60)));
  add('  180 ' + hexOf(%subst(raw : 181 : 60)));
  raw = xs_rawCmd(pLib : 'WRKXDBPRF');
  add('command WRKXDBPRF, raw: program [' + %subst(raw : 29 : 20) + '] help ['
      + %subst(raw : 169 : 20) + ']');
  add('  000 ' + hexOf(%subst(raw : 1 : 60)));
  add('  060 ' + hexOf(%subst(raw : 61 : 60)));
  add('  120 ' + hexOf(%subst(raw : 121 : 60)));
  add('  180 ' + hexOf(%subst(raw : 181 : 60)));

  show('before' : pLib : pOwner);

  if pMode = 'A' or pMode = 'O';
    xs_check(pLib : pOwner : items : count);
    if items(1).state = 'F';
      add('--- the library cannot be used: nothing is run');
      count = 0;
    endif;
    for i = 1 to count;
      if items(i).state = 'T' and not items(i).manual and items(i).id <> 'OWNPRF'
         and (items(i).id <> 'OWNER' or pMode = 'O');
        add('--- doing ' + %trim(items(i).id));
        for j = 1 to items(i).ncmd;
          add('  run: ' + items(i).cmd(j));
          if xs_run(items(i).cmd(j) : msgId : msgText);
            add('   -> OK');
          else;
            add('   -> ' + msgId + ' ' + msgText);
          endif;
        endfor;
        // later lines depend on earlier ones: look again
        xs_check(pLib : pOwner : items : count);
      endif;
    endfor;
    show('after' : pLib : pOwner);
  endif;

  wpath = %trim(pOut);
  for i = 1 to nl;
    wline = lines(i);
    exec sql call qsys2.ifs_write(path_name => :wpath, line => :wline,
                                  overwrite => :wmode, end_of_line => 'LF');
    wmode = 'APPEND';
  endfor;
  return;
end-proc;

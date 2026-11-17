**free
// XDBSETUP - checks and repairs of an XDB installation (module, no main).
//
// The objects of XDB can be restored into any library. This module looks
// at one library and tells what is in order and what is still to do so
// that the driver works from there without the library list:
//   - the library is in the system ASP (Db2 calls the driver in every job)
//   - profile files and password list exist (they are never part of a
//     package: a restore cannot overwrite profiles or passwords)
//   - the programs are bound to the service programs of this library
//   - the command points to this library
//   - the driver and the password list belong to a profile made for it,
//     and the driver adopts its authority to read the passwords
// For everything to do it gives the CL commands. Nothing is changed here
// except through xs_run, one command at a time, which the caller shows to
// the user first. System values are only reported.
//
// Bound into XDBPRFM (display) and XDBT19 (batch test).

ctl-opt nomain option(*srcstmt : *nodebugio);

/copy QXDBSRC,XDBSETUPH

dcl-ds pgmSts psds qualified;
  msgId char(7) pos(40);
  msgData char(80) pos(91);
end-ds;

dcl-ds errDs qualified template;
  provided int(10) inz(116);
  avail int(10) inz(0);
  msgId char(7);
  rsv char(1);
  data char(100);
end-ds;

dcl-pr QUSROBJD extpgm('QUSROBJD');
  rcv char(90) options(*varsize);
  rcvLen int(10) const;
  fmt char(8) const;
  obj char(20) const;
  type char(10) const;
  err likeds(errDs);
end-pr;
dcl-pr QUSCRTUS extpgm('QUSCRTUS');
  name char(20) const;
  attr char(10) const;
  size int(10) const;
  init char(1) const;
  aut char(10) const;
  text char(50) const;
  replace char(10) const;
  err likeds(errDs);
end-pr;
dcl-pr QUSPTRUS extpgm('QUSPTRUS');
  name char(20) const;
  ptr pointer;
  err likeds(errDs);
end-pr;
dcl-pr QBNLPGMI extpgm('QBNLPGMI');
  space char(20) const;
  fmt char(8) const;
  pgm char(20) const;
  err likeds(errDs);
end-pr;
dcl-pr QBNLSPGM extpgm('QBNLSPGM');
  space char(20) const;
  fmt char(8) const;
  pgm char(20) const;
  err likeds(errDs);
end-pr;
dcl-pr QBNRSPGM extpgm('QBNRSPGM');
  rcv char(400) options(*varsize);
  rcvLen int(10) const;
  fmt char(8) const;
  pgm char(20) const;
  err likeds(errDs);
end-pr;
dcl-pr QCDRCMDI extpgm('QCDRCMDI');
  rcv char(400) options(*varsize);
  rcvLen int(10) const;
  fmt char(8) const;
  cmd char(20) const;
  err likeds(errDs);
end-pr;
dcl-pr QSYCUSRS extpgm('QSYCUSRS');
  answer char(1);
  user char(10) const;
  auts char(10) const;
  count int(10) const;
  callLvl int(10) const;
  err likeds(errDs);
end-pr;
dcl-pr QSYRUSRA extpgm('QSYRUSRA');
  rcv char(100) options(*varsize);
  rcvLen int(10) const;
  fmt char(8) const;
  user char(10) const;
  obj char(20) const;
  type char(10) const;
  err likeds(errDs);
end-pr;
dcl-pr QWCRSVAL extpgm('QWCRSVAL');
  rcv char(100) options(*varsize);
  rcvLen int(10) const;
  count int(10) const;
  names char(10) const;
  err likeds(errDs);
end-pr;
dcl-pr QMHRTVM extpgm('QMHRTVM');
  rcv char(300) options(*varsize);
  rcvLen int(10) const;
  fmt char(8) const;
  msgId char(7) const;
  msgf char(20) const;
  data char(80) const;
  dataLen int(10) const;
  replace char(10) const;
  rtnCtl char(10) const;
  err likeds(errDs);
end-pr;
dcl-pr QMHSNDPM extpgm('QMHSNDPM');
  msgId char(7) const;
  msgf char(20) const;
  data char(256) const;
  dataLen int(10) const;
  type char(10) const;
  stack char(10) const;
  counter int(10) const;
  key char(4);
  err likeds(errDs);
end-pr;
dcl-pr QCMDEXC extpgm('QCMDEXC');
  cmd char(300) const;
  len packed(15 : 5) const;
end-pr;

dcl-c USRSPC 'XDBSETUP  QTEMP     ';

// data area XDBVER of the library looked at (a data area cannot be
// defined inside a procedure)
dcl-s verName char(21);
dcl-s verData char(50) dtaara(verName);

// ------------------------------------------------------------- helpers

// *on = the object exists; owner and ASP number are given back
dcl-proc objInfo;
  dcl-pi *n ind;
    lib char(10) const;
    name char(10) const;
    type char(10) const;
    owner char(10);
    asp int(10);
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-ds rcv qualified;
    returned int(10);
    avail int(10);
    name char(10);
    lib char(10);
    type char(10);
    rtnLib char(10);
    asp int(10);
    owner char(10);
    rest char(28);
  end-ds;

  owner = *blanks;
  asp = 0;
  callp(e) QUSROBJD(rcv : %size(rcv) : 'OBJD0100' : name + lib : type : ec);
  if %error();
    return *off;
  endif;
  if ec.avail > 0;
    // not authorized to it: it is there
    return (ec.msgId = 'CPF9802' or ec.msgId = 'CPF9821' or ec.msgId = 'CPF9822');
  endif;
  owner = rcv.owner;
  asp = rcv.asp;
  return *on;
end-proc;

// library of service program srvpgm as bound into program or service
// program name of library lib, *LIBL when it is bound through the library
// list; '' when it is not bound or not found
dcl-proc boundLib;
  dcl-pi *n varchar(10);
    lib char(10) const;
    name char(10) const;
    kind char(1) const;
    srvpgm char(10) const;
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-s p pointer;
  dcl-s pe pointer;
  dcl-ds head based(p) qualified;
    filler char(124);
    listOffset int(10);
    listSize int(10);
    count int(10);
    entrySize int(10);
  end-ds;
  dcl-ds entry based(pe) qualified;
    pgm char(10);
    pgmLib char(10);
    spName char(10);
    spLib char(10);
  end-ds;
  dcl-s i int(10);

  callp(e) QUSCRTUS(USRSPC : 'XDB' : 65536 : x'00' : '*ALL' : 'XDB setup' : '*YES' : ec);
  if %error() or ec.avail > 0;
    return '';
  endif;
  ec.avail = 0;
  if kind = 'S';
    callp(e) QBNLSPGM(USRSPC : 'SPGL0200' : name + lib : ec);
  else;
    callp(e) QBNLPGMI(USRSPC : 'PGML0200' : name + lib : ec);
  endif;
  if %error() or ec.avail > 0;
    return '';
  endif;
  ec.avail = 0;
  callp(e) QUSPTRUS(USRSPC : p : ec);
  if %error() or ec.avail > 0 or p = *null;
    return '';
  endif;
  for i = 1 to head.count;
    pe = p + head.listOffset + (i - 1) * head.entrySize;
    if entry.spName = srvpgm;
      // bound through the library list: the library comes back empty
      if entry.spLib = *blanks or %subst(entry.spLib : 1 : 1) = x'00';
        return '*LIBL';
      endif;
      return %trim(entry.spLib);
    endif;
  endfor;
  return '';
end-proc;

// U = the service program runs with the authority of the user only,
// O = it adopts the authority of its owner, ' ' = not found
dcl-proc adoption;
  dcl-pi *n char(1);
    lib char(10) const;
    name char(10) const;
  end-pi;
  dcl-s raw char(240);

  raw = xs_rawSrvpgm(lib : name);
  if raw = *blanks;
    return ' ';
  endif;
  return %subst(raw : 138 : 1);
end-proc;

dcl-proc publicAuthority;
  dcl-pi *n char(10);
    lib char(10) const;
    name char(10) const;
    type char(10) const;
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-s rcv char(100);

  callp(e) QSYRUSRA(rcv : %size(rcv) : 'USRA0100' : '*PUBLIC' : name + lib : type : ec);
  if %error() or ec.avail > 0;
    return *blanks;
  endif;
  return %subst(rcv : 9 : 10);
end-proc;

dcl-proc hasSpecial;
  dcl-pi *n ind;
    aut char(10) const;
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-s answer char(1) inz('N');

  callp(e) QSYCUSRS(answer : '*CURRENT' : aut : 1 : 0 : ec);
  if %error() or ec.avail > 0;
    return *off;
  endif;
  return (answer = 'Y');
end-proc;

// first character of a system value, ' ' when it cannot be read
dcl-proc systemValue;
  dcl-pi *n char(1);
    name char(10) const;
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-ds rcv qualified;
    count int(10);
    offset int(10);
    rest char(92);
  end-ds;
  dcl-s buf char(100) based(pBuf);
  dcl-s pBuf pointer;

  clear rcv;
  pBuf = %addr(rcv);
  callp(e) QWCRSVAL(rcv : %size(rcv) : 1 : name : ec);
  if %error() or ec.avail > 0 or rcv.count < 1 or rcv.offset < 8 or rcv.offset > 80;
    return ' ';
  endif;
  // entry: name 10, type 1, status 1, length 4, then the value
  return %subst(buf : rcv.offset + 17 : 1);
end-proc;

dcl-proc addCmd;
  dcl-pi *n;
    item likeds(xsItem_t);
    cmd varchar(200) const;
  end-pi;
  if item.ncmd < XS_MAXCMDS;
    item.ncmd += 1;
    item.cmd(item.ncmd) = cmd;
  endif;
end-proc;

// ---------------------------------------------------------- the checks

dcl-proc xs_defaultOwner export;
  dcl-pi *n char(10);
    lib char(10) const;
  end-pi;
  dcl-s owner char(10);
  dcl-s asp int(10);

  if objInfo(lib : 'XDB' : '*SRVPGM' : owner : asp)
     and adoption(lib : 'XDB') = 'O'
     and owner <> *blanks and %subst(owner : 1 : 1) <> 'Q';
    return owner;
  endif;
  return 'XDBOWNER';
end-proc;

dcl-proc xs_missing export;
  dcl-pi *n ind;
    lib char(10) const;
  end-pi;
  dcl-s owner char(10);
  dcl-s asp int(10);

  return not (objInfo(lib : 'XDBPRF' : '*FILE' : owner : asp)
              and objInfo(lib : 'XDBPRO' : '*FILE' : owner : asp)
              and objInfo(lib : 'XDBKWD' : '*FILE' : owner : asp));
end-proc;

dcl-proc xs_open export;
  dcl-pi *n int(10);
    lib char(10) const;
  end-pi;
  dcl-ds items likeds(xsItem_t) dim(XS_MAXITEMS);
  dcl-s count int(10);
  dcl-s i int(10);
  dcl-s n int(10) inz(0);

  xs_check(lib : *blanks : items : count);
  for i = 1 to count;
    if items(i).state = 'T' or items(i).state = 'F';
      n += 1;
    endif;
  endfor;
  return n;
end-proc;

dcl-proc xs_version export;
  dcl-pi *n varchar(50);
    lib char(10) const;
  end-pi;

  verName = %trim(lib) + '/XDBVER';
  in(e) verData;
  if %error() or verData = *blanks;
    return 'not a package (built here)';
  endif;
  return %trim(verData);
end-proc;

dcl-proc xs_check export;
  dcl-pi *n;
    lib char(10) const;
    ownerIn char(10) const;
    items likeds(xsItem_t) dim(XS_MAXITEMS);
    count int(10);
  end-pi;
  dcl-s owner char(10);
  dcl-s L varchar(10);
  dcl-s W varchar(10);
  dcl-s n int(10) inz(0);
  dcl-s o char(10);
  dcl-s asp int(10);
  dcl-s b varchar(10) dim(3);
  dcl-s v char(1);
  dcl-s cppLib char(10);
  dcl-s hlpLib char(10);
  dcl-s raw char(240);
  dcl-s haveList ind;
  dcl-s haveDriver ind;
  dcl-s i int(10);

  L = %trim(lib);
  owner = ownerIn;
  if owner = *blanks;
    owner = xs_defaultOwner(lib);
  endif;
  W = %trim(owner);
  clear items;

  // ---- the library
  n += 1;
  items(n).id = 'LIB';
  items(n).text = 'Library is in the system ASP';
  if lib = 'QTEMP';
    items(n).state = 'F';
    items(n).status = 'QTEMP cannot be used';
  elseif not objInfo('QSYS' : lib : '*LIB' : o : asp);
    items(n).state = 'F';
    items(n).status = 'Library not found';
  elseif asp > 32;
    items(n).state = 'F';
    items(n).status = 'Independent ASP ' + %char(asp);
    items(n).note = 'Db2 calls the driver in every job, also in jobs that do not have '
                  + 'this ASP. Restore into a library of the system ASP.';
  else;
    items(n).state = 'O';
    items(n).status = 'OK';
  endif;

  // ---- the person who installs
  n += 1;
  items(n).id = 'AUT';
  items(n).text = 'Your special authorities';
  if hasSpecial('*SECADM') and hasSpecial('*ALLOBJ');
    items(n).state = 'O';
    items(n).status = 'OK';
  else;
    items(n).state = 'W';
    items(n).status = '*SECADM or *ALLOBJ missing';
    items(n).note = 'The steps for the owner profile need a security officer. '
                  + 'Option 5 shows the commands to hand over.';
  endif;

  // ---- system value, reported only
  n += 1;
  items(n).id = 'SYSVAL';
  items(n).text = 'System value QRETSVRSEC';
  v = systemValue('QRETSVRSEC');
  if v = '1';
    items(n).state = 'O';
    items(n).status = 'OK';
  else;
    items(n).state = 'W';
    items(n).status = 'Passwords cannot be stored';
    items(n).note = 'The system keeps passwords for other servers only when this value '
                  + 'is 1. Setup does not change system values.';
    items(n).manual = *on;
    addCmd(items(n) : 'CHGSYSVAL SYSVAL(QRETSVRSEC) VALUE(''1'')');
  endif;

  // ---- what a package brings
  n += 1;
  items(n).id = 'KWD';
  items(n).text = 'Option dictionary XDBKWD';
  if objInfo(lib : 'XDBKWD' : '*FILE' : o : asp);
    items(n).state = 'O';
    items(n).status = 'OK';
  else;
    items(n).state = 'F';
    items(n).status = 'Missing';
    items(n).note = 'Restore the objects of the package again.';
  endif;

  // ---- profile files: made from the models, never restored
  n += 1;
  items(n).id = 'FILES';
  items(n).text = 'Profile files XDBPRF and XDBPRO';
  items(n).state = 'O';
  items(n).status = 'OK';
  if not objInfo(lib : 'XDBPRF' : '*FILE' : o : asp);
    if objInfo(lib : 'XDBPRFMDL' : '*FILE' : o : asp);
      addCmd(items(n) : 'CRTDUPOBJ OBJ(XDBPRFMDL) FROMLIB(' + L + ') OBJTYPE(*FILE) TOLIB('
                      + L + ') NEWOBJ(XDBPRF) DATA(*NO)');
    else;
      items(n).state = 'F';
    endif;
  endif;
  if not objInfo(lib : 'XDBPRO' : '*FILE' : o : asp);
    if objInfo(lib : 'XDBPROMDL' : '*FILE' : o : asp);
      addCmd(items(n) : 'CRTDUPOBJ OBJ(XDBPROMDL) FROMLIB(' + L + ') OBJTYPE(*FILE) TOLIB('
                      + L + ') NEWOBJ(XDBPRO) DATA(*NO)');
    else;
      items(n).state = 'F';
    endif;
  endif;
  if items(n).state = 'F';
    items(n).status = 'Model file missing';
    items(n).note = 'XDBPRFMDL and XDBPROMDL come with the package. Restore it again.';
    items(n).ncmd = 0;
  elseif items(n).ncmd > 0;
    items(n).state = 'T';
    items(n).status = 'To do: create empty';
    items(n).note = 'The files are created empty from the models of the package.';
  endif;

  // ---- password list
  n += 1;
  items(n).id = 'VLDL';
  items(n).text = 'Password list XDBVLDL';
  haveList = objInfo(lib : 'XDBVLDL' : '*VLDL' : o : asp);
  if haveList;
    items(n).state = 'O';
    items(n).status = 'OK';
  else;
    items(n).state = 'T';
    items(n).status = 'To do: create';
    items(n).note = 'The passwords of the profiles are kept encrypted in this list. '
                  + 'Nobody but its owner gets authority to it.';
    addCmd(items(n) : 'CRTVLDL VLDL(' + L + '/XDBVLDL) AUT(*EXCLUDE) '
                    + 'TEXT(''XDB - passwords of the connection profiles'')');
  endif;

  // ---- bindings
  n += 1;
  items(n).id = 'BIND';
  items(n).text = 'Driver bound to this library';
  b(1) = boundLib(lib : 'XDB' : 'S' : 'LIBPQ');
  b(2) = boundLib(lib : 'XDBARD' : 'P' : 'XDB');
  b(3) = boundLib(lib : 'XDBCMT' : 'P' : 'XDB');
  haveDriver = (b(1) <> '' and b(2) <> '' and b(3) <> '');
  if not haveDriver;
    items(n).state = 'F';
    items(n).status = 'Driver objects missing';
    items(n).note = 'XDB, LIBPQ, XDBARD and XDBCMT come with the package. Restore it again.';
  else;
    if b(1) <> L;
      addCmd(items(n) : 'UPDSRVPGM SRVPGM(' + L + '/XDB) MODULE(*NONE) SRVPGMLIB(' + L + ')');
    endif;
    if b(2) <> L;
      addCmd(items(n) : 'UPDPGM PGM(' + L + '/XDBARD) MODULE(*NONE) SRVPGMLIB(' + L + ')');
    endif;
    if b(3) <> L;
      addCmd(items(n) : 'UPDPGM PGM(' + L + '/XDBCMT) MODULE(*NONE) SRVPGMLIB(' + L + ')');
    endif;
    if items(n).ncmd = 0;
      items(n).state = 'O';
      items(n).status = 'OK';
    else;
      items(n).state = 'T';
      for i = 1 to 3;
        if b(i) <> L;
          if b(i) = '*LIBL';
            items(n).status = 'To do: now the library list';
          else;
            items(n).status = 'To do: now ' + b(i);
          endif;
          leave;
        endif;
      endfor;
      items(n).note = 'The driver then finds its parts without the library list. Jobs '
                    + 'that are connected now keep the present objects until they end.';
    endif;
  endif;

  // ---- the command
  n += 1;
  items(n).id = 'CMD';
  items(n).text = 'Command WRKXDBPRF';
  raw = xs_rawCmd(lib : 'WRKXDBPRF');
  if raw = *blanks;
    items(n).state = 'W';
    items(n).status = 'Command not found';
  else;
    cppLib = %subst(raw : 39 : 10);
    hlpLib = %subst(raw : 179 : 10);
    if cppLib = lib and hlpLib = lib;
      items(n).state = 'O';
      items(n).status = 'OK';
    else;
      items(n).state = 'T';
      if cppLib = '*LIBL';
        items(n).status = 'To do: now the library list';
      else;
        items(n).status = 'To do: now ' + %trim(cppLib);
      endif;
      items(n).note = 'The command and its help then work without the library list.';
      addCmd(items(n) : 'CHGCMD CMD(' + L + '/WRKXDBPRF) PGM(' + L + '/XDBPRFM) HLPPNLGRP('
                      + L + '/WRKXDBPRF)');
    endif;
  endif;

  // ---- the owner profile
  n += 1;
  items(n).id = 'OWNPRF';
  items(n).text = 'Owner profile ' + W;
  if objInfo('QSYS' : owner : '*USRPRF' : o : asp);
    items(n).state = 'O';
    items(n).status = 'OK';
  else;
    items(n).state = 'T';
    items(n).status = 'To do: create';
    items(n).note = 'A profile nobody can sign on with and without special authority. '
                  + 'It only owns the driver and the password list.';
    addCmd(items(n) : 'CRTUSRPRF USRPRF(' + W + ') PASSWORD(*NONE) USRCLS(*USER) '
                    + 'SPCAUT(*NONE) INLMNU(*SIGNOFF) LMTCPB(*YES) '
                    + 'TEXT(''XDB - owner of the driver and of the password list'')');
  endif;

  // ---- ownership and adoption
  n += 1;
  items(n).id = 'OWNER';
  items(n).text = 'Driver and passwords owned by it';
  if not objInfo(lib : 'XDB' : '*SRVPGM' : o : asp);
    items(n).state = 'F';
    items(n).status = 'Driver objects missing';
  else;
    if o <> owner;
      addCmd(items(n) : 'CHGOBJOWN OBJ(' + L + '/XDB) OBJTYPE(*SRVPGM) NEWOWN(' + W
                      + ') CUROWNAUT(*REVOKE)');
    endif;
    if haveList;
      if objInfo(lib : 'XDBVLDL' : '*VLDL' : o : asp) and o <> owner;
        addCmd(items(n) : 'CHGOBJOWN OBJ(' + L + '/XDBVLDL) OBJTYPE(*VLDL) NEWOWN(' + W
                        + ') CUROWNAUT(*REVOKE)');
      endif;
      if publicAuthority(lib : 'XDBVLDL' : '*VLDL') <> '*EXCLUDE';
        addCmd(items(n) : 'GRTOBJAUT OBJ(' + L + '/XDBVLDL) OBJTYPE(*VLDL) USER(*PUBLIC) '
                        + 'AUT(*EXCLUDE)');
      endif;
    endif;
    if adoption(lib : 'XDB') <> 'O';
      addCmd(items(n) : 'CHGSRVPGM SRVPGM(' + L + '/XDB) USRPRF(*OWNER)');
    endif;
    if items(n).ncmd = 0 and haveList;
      items(n).state = 'O';
      items(n).status = 'OK';
    else;
      items(n).state = 'T';
      items(n).status = 'To do';
      if not haveList;
        items(n).status = 'To do: after the password list';
      endif;
      items(n).note = 'The driver reads the passwords with the authority of this owner; '
                    + 'the users of the databases need no authority to the list.';
    endif;
  endif;

  count = n;
end-proc;

// ------------------------------------------------------------- running

dcl-proc xs_run export;
  dcl-pi *n ind;
    cmd varchar(200) const;
    msgId char(7);
    msgText varchar(132);
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-s key char(4);
  dcl-s line varchar(256);
  dcl-s rcv char(300);
  dcl-s len int(10) based(pLen);
  dcl-s pLen pointer;
  dcl-s id char(7);
  dcl-s data char(80);

  msgId = *blanks;
  msgText = '';
  line = 'XDB setup: ' + cmd;
  callp(e) QMHSNDPM('CPF9897' : 'QCPFMSG   *LIBL' : line : %len(line) : '*INFO' : '*'
                    : 1 : key : ec);
  callp(e) QCMDEXC(cmd : %len(cmd));
  if not %error();
    return *on;
  endif;
  id = pgmSts.msgId;
  data = pgmSts.msgData;
  msgId = id;
  ec.avail = 0;
  callp(e) QMHRTVM(rcv : %size(rcv) : 'RTVM0100' : id : 'QCPFMSG   *LIBL' : data : %size(data)
                   : '*YES' : '*NO' : ec);
  if not %error() and ec.avail = 0;
    pLen = %addr(rcv) + 8;
    if len > 0 and len <= 132;
      msgText = %subst(rcv : 25 : len);
    elseif len > 132;
      msgText = %subst(rcv : 25 : 132);
    endif;
  endif;
  return *off;
end-proc;

// ------------------------------------------------- raw answers (tests)

dcl-proc xs_rawSrvpgm export;
  dcl-pi *n char(240);
    lib char(10) const;
    name char(10) const;
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-s rcv char(400);

  rcv = *blanks;
  callp(e) QBNRSPGM(rcv : %size(rcv) : 'SPGI0100' : name + lib : ec);
  if %error() or ec.avail > 0;
    return *blanks;
  endif;
  return %subst(rcv : 1 : 240);
end-proc;

dcl-proc xs_rawCmd export;
  dcl-pi *n char(240);
    lib char(10) const;
    name char(10) const;
  end-pi;
  dcl-ds ec likeds(errDs) inz(*likeds);
  dcl-s rcv char(400);

  rcv = *blanks;
  callp(e) QCDRCMDI(rcv : %size(rcv) : 'CMDI0100' : name + lib : ec);
  if %error() or ec.avail > 0;
    return *blanks;
  endif;
  return %subst(rcv : 1 : 240);
end-proc;

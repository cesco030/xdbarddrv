**free
// XDBSETUPH - prototypes of module XDBSETUP (checks and repairs of an XDB
// installation). Copied into XDBPRFM and into the batch test XDBT19.

dcl-c XS_MAXITEMS 12;
// the line about the owner of the objects has one command for each object of a package
dcl-c XS_MAXCMDS 14;

// One line of the setup list.
//   state  O = in order, T = to do (setup can do it), W = remark,
//          F = the installation cannot work like this
//   manual *on = the commands are shown only, setup does not run them
dcl-ds xsItem_t qualified template;
  id char(8);
  text char(40);
  state char(1);
  status char(30);
  note varchar(136);
  manual ind;
  ncmd int(10);
  cmd varchar(200) dim(XS_MAXCMDS);
end-ds;

// all checks of the installation in library lib; owner blank = the default
dcl-pr xs_check extproc('xs_check');
  lib char(10) const;
  owner char(10) const;
  items likeds(xsItem_t) dim(XS_MAXITEMS);
  count int(10);
end-pr;

// the owner profile setup proposes: the present owner when the driver
// already adopts the authority of a profile of its own, else XDBOWNER
dcl-pr xs_defaultOwner char(10) extproc('xs_defaultOwner');
  lib char(10) const;
end-pr;

// *on when the files without which the profiles cannot be shown are missing
dcl-pr xs_missing ind extproc('xs_missing');
  lib char(10) const;
end-pr;

// number of lines in state T or F
dcl-pr xs_open int(10) extproc('xs_open');
  lib char(10) const;
end-pr;

// version of the package (data area XDBVER), or a text when there is none
dcl-pr xs_version varchar(50) extproc('xs_version');
  lib char(10) const;
end-pr;

// runs one command; it is written to the job log first. *off = failed,
// then msgId and msgText say why.
dcl-pr xs_run ind extproc('xs_run');
  cmd varchar(200) const;
  msgId char(7);
  msgText varchar(132);
end-pr;

// for the tests: the raw answers of two system interfaces
dcl-pr xs_rawSrvpgm char(240) extproc('xs_rawSrvpgm');
  lib char(10) const;
  name char(10) const;
end-pr;
dcl-pr xs_rawCmd char(240) extproc('xs_rawCmd');
  lib char(10) const;
  name char(10) const;
end-pr;

**free
// XDBALWH - prototypes of module XDBALW (the rules of the allowed users of
// an XDB connection profile). Copied into XDBPRFM and into the batch test
// XDBT25.

// '' when name is *ALL or a user profile of this system (a group profile
// is one too), otherwise the message to show
dcl-pr alw_nameError varchar(78) extproc('alw_nameError');
  name char(10) const;
end-pr;

// '' when list (names separated by blanks) can be the value of option
// allowed_users: user profiles of this system, or *ALL alone
dcl-pr alw_listError varchar(78) extproc('alw_listError');
  list varchar(200) const;
end-pr;

// The value of the option as the three names of the profile display; no
// value gives *ALL. Returns *on when the option holds more than the
// display shows: more than three names, or a word longer than a name.
dcl-pr alw_toNames ind extproc('alw_toNames');
  value varchar(200) const;
  names char(10) dim(3);
end-pr;

// Checks the three names typed. They come back in upper case and left
// adjusted, with *ALL in the first when nothing was typed. Returns '' when
// they can be saved, otherwise the message; bad is then the number of the
// field for the cursor. loaded and more are what alw_toNames gave when the
// profile was read: names that were not touched are not checked again, and
// a list the display does not show whole cannot be changed from it.
dcl-pr alw_check varchar(78) extproc('alw_check');
  names char(10) dim(3);
  loaded char(10) dim(3) const;
  more ind const;
  bad int(10);
end-pr;

// The value to store for the names; '' when they say *ALL (no option)
dcl-pr alw_value varchar(200) extproc('alw_value');
  names char(10) dim(3) const;
end-pr;

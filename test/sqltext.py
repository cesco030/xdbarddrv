#!/usr/bin/env python3
# sqltext.py - statements whose text has question marks, colons, quotes and
# comments in the places where a driver that looks for parameter markers
# can go wrong.
#
# usage: python3 sqltext.py <emu dir> pg|mysql|mssql [-v]
#   the server: see xdbqa.py.
#
# The driver finds the markers of a statement itself (it numbers them for
# PostgreSQL and counts them for all).  A question mark inside a literal, a
# quoted name, a comment or a dollar-quoted body is no marker; where the
# string ends depends on the server and its settings (backslashes).  Every
# case says what the row must be; "error" where the server itself refuses.
import sys

import xdbqa

EMU = sys.argv[1]
KIND = sys.argv[2]
VERBOSE = '-v' in sys.argv[3:]
T = xdbqa.Target(KIND)
ver, vtext = T.version()
maria = 'MariaDB' in vtext

C = []      # (label, statement, host variables or None, the row it must give as a list of texts, or 'error')


def add(label, stm, parms, want):
    C.append((label, stm, parms, want))


add('no marker', "select 'a' as v", None, ['a'])
add('one marker', "select ? as v", 'x', ['x'])
add('a question mark in a literal', "select 'a?b' as v, ? as p", 'x', ['a?b', 'x'])
add('a doubled quote in a literal', "select 'it''s ?' as v, ? as p", 'x', ["it's ?", 'x'])
add('a marker between two literals', "select '?' as a, ? as p, '?' as b", 'x', ['?', 'x', '?'])
add('a comment to the end of the line', "select ? as p -- is this one?", 'x', ['x'])
add('a comment in the middle', "select /* one? two? */ ? as p", 'x', ['x'])
add('a comment with a quote in it', "select /* don't */ ? as p", 'x', ['x'])
add('a line comment with a quote in it', "select ? as p -- don't", 'x', ['x'])
add('two markers, the same value twice', "select ? as a, ? as b", 'x|y', ['x', 'y'])
add('a literal that ends with a backslash', "select 'a\\\\' as v, ? as p" if KIND == 'mysql' else "select 'a\\' as v, ? as p", 'x', ['a\\', 'x'])
if KIND == 'mssql':
    add('a name in brackets with a question mark', 'select 1 as [a?b], ? as p', 'x', ['1', 'x'])
    add('a name in brackets with a closing bracket', 'select [a]]?b], ? as p from (select 2 as [a]]?b]) t', 'x', ['2', 'x'])
    add('a quoted name with a question mark', 'select "a?b", ? as p from (select 1 as "a?b") t', 'x', ['1', 'x'])
    add('a national literal with a question mark', "select N'a?b' as v, ? as p", 'x', ['a?b', 'x'])
    add('a comment inside a comment', "select /* a /* b ? */ c ? */ ? as p", 'x', ['x'])
    add('two dashes are a comment, blank or not', "select 5 as v, ? as p --?", 'x', ['5', 'x'])
    add('a bracket inside a literal is no name', "select '[?' as v, ? as p", 'x', ['[?', 'x'])
    add('a quote inside a name in brackets', "select [it's ?], ? as p from (select 3 as [it's ?]) t", 'x', ['3', 'x'])
    add('a temporary table with number signs', "create table #xdb_qa_h (id int, v varchar(10))", None, 'done')
    add('... a row into it', "insert into #xdb_qa_h (id, v) values (?, ?)", 'i:1|a#?', 'done')
    add('... and back', "select v, ? as p from #xdb_qa_h where id = ?", 'x|i:1', ['a#?', 'x'])
elif KIND == 'pg':
    add('a quoted name with a question mark', 'select "a?b", ? as p from (select 1 as "a?b") t', 'x', ['1', 'x'])
    add('a cast and a marker', "select '5'::int as v, ?::int as p", '7', ['5', '7'])
    add('an array slice', "select (array[1,2,3])[1:2]::text as v, ? as p", 'x', ['{1,2}', 'x'])
    add('a dollar-quoted text with a question mark', "select $$a ? b$$ as v, ? as p", 'x', ['a ? b', 'x'])
    add('a dollar-quoted text with a tag and a quote', "select $q$it's ?$q$ as v, ? as p", 'x', ["it's ?", 'x'])
    add('a dollar-quoted text with a dollar in it', "select $a$1 $ 2 $b$ ?$a$ as v, ? as p", 'x', ['1 $ 2 $b$ ?', 'x'])
    add('an escape string with a quote', "select E'it\\'s ?' as v, ? as p", 'x', ["it's ?", 'x'])
    add('an escape string with a backslash at its end', "select E'a\\\\' as v, ? as p", 'x', ['a\\', 'x'])
    add('a comment inside a comment', "select /* a /* b ? */ c ? */ ? as p", 'x', ['x'])
    add('a marker right after a dollar-quoted text', "select $$x$$||? as v", 'y', ['xy'])
    add('a parameter number of the server in the text', "select $1::text as v", 'x', ['x'])
    if ver >= (9, 4):
        add('the operator ? written ??', "select ('{\"a\":1}'::jsonb ?? 'a')::int as v, ? as p", 'x', ['1', 'x'])
        add('the operators ??| and ??&', "select ('{\"a\":1}'::jsonb ??| array['a','b'])::int as v, ('{\"a\":1}'::jsonb ??& array['a'])::int as w", None, ['1', '1'])
    add('a function body with question marks and quotes',
        "create or replace function xdb_qa_f(a int) returns text language plpgsql as $f$ begin /* what's this? */ return 'is it ' || a || '?'; end $f$",
        None, 'done')
    add('the function called with a marker', "select xdb_qa_f(?::int) as v", '5', ['is it 5?'])
    if (8, 2) <= ver < (19, 0):     # the setting came with 8.2 and can no longer be switched off from 19 on
        add('standard_conforming_strings off: a backslash quote', "set standard_conforming_strings to off", None, 'done')
        add('... then a literal with an escaped quote', "select 'it\\'s ?' as v, ? as p", 'x', ["it's ?", 'x'])
        add('... and back on', "set standard_conforming_strings to on", None, 'done')
        add('with it on, a backslash is a character', "select 'a\\' as v, ? as p", 'x', ['a\\', 'x'])
else:
    add('a backslash quote in a literal', "select 'it\\'s ?' as v, ? as p", 'x', ["it's ?", 'x'])
    add('a double-quoted literal with a question mark', 'select "a ? b" as v, ? as p', 'x', ['a ? b', 'x'])
    add('a double-quoted literal with a backslash quote', 'select "say \\"what?\\"" as v, ? as p', 'x', ['say "what?"', 'x'])
    add('a name in backticks with a question mark', 'select `a?b`, ? as p from (select 1 as `a?b`) t', 'x', ['1', 'x'])
    add('a comment with a number sign', "select ? as p # is this one?", 'x', ['x'])
    add('a number sign comment with a quote', "select ? as p # don't", 'x', ['x'])
    add('a variable assignment', "select @xdb_qa := 5 as v, ? as p", 'x', ['5', 'x'])
    add('a versioned comment', "select /*!40001 SQL_NO_CACHE */ ? as p", 'x', ['x'])
    add('two dashes without a blank are no comment', "select 5--2 as v, ? as p", 'x', ['7', 'x'])
    add('NO_BACKSLASH_ESCAPES: a backslash is a character', "set session sql_mode = concat(@@sql_mode, ',NO_BACKSLASH_ESCAPES')", None, 'done')
    add('... then a literal that ends with a backslash', "select 'a\\' as v, ? as p", 'x', ['a\\', 'x'])
    add('... and off again', "set session sql_mode = replace(@@sql_mode, 'NO_BACKSLASH_ESCAPES', '')", None, 'done')
    add('ANSI_QUOTES: double quotes are names', "set session sql_mode = concat(@@sql_mode, ',ANSI_QUOTES')", None, 'done')
    add('... a quoted name with a question mark', 'select "a?b", ? as p from (select 1 as "a?b") t', 'x', ['1', 'x'])
    add('... and off again', "set session sql_mode = replace(@@sql_mode, 'ANSI_QUOTES', '')", None, 'done')
    add('a limit from a host variable', "select 1 as v from (select 1 union all select 2 union all select 3) t limit ?", 'i:2', ['1'])
    # a numeric host variable is a number on the server, not its text
    add('a BIGINT in arithmetic keeps all its digits', "select cast(? + 1 as char) as v", '@b:9007199254740993', ['9007199254740994'])
    add('a DECIMAL in arithmetic stays exact', "select cast(? * 3 = 0.3 as char) as v", '@p15,4:0.1', ['1'])
    add('a BIGINT above 2^53 finds its own row only',
        "select count(*) as n from (select 9007199254740992 as id union all select 9007199254740993) t where id = ?", '@b:9007199254740993', ['1'])
    add('a table with a BIT(1) and an ENUM column', "create temporary table xdb_qa_b (id int, f bit(1), e enum('x','y','z'))", None, 'done')
    add('0 and 1 into the BIT column', "insert into xdb_qa_b (id, f, e) values (?, ?, ?)", 'i:1|i:0|i:2', 'done')
    add('... and once more', "insert into xdb_qa_b (id, f, e) values (?, ?, ?)", 'i:2|i:1|i:3', 'done')
    add('0 is 0 there, and 2 the second member', "select cast(f + 0 as char) as f, e from xdb_qa_b where id = ?", 'i:1', ['0', 'y'])
    add('1 is 1 there, and 3 the third member', "select cast(f + 0 as char) as f, e from xdb_qa_b where id = ?", 'i:2', ['1', 'z'])
    add('the BIT column found by a number', "select cast(count(*) as char) as n from xdb_qa_b where f = ?", 'i:1', ['1'])
    # one statement run with a number, then with text, then with a number: the text must arrive as text
    add('a table with a latin1 column', "create temporary table xdb_qa_l (id int, v varchar(20) character set latin1)", None, 'done')
    add('a number into it', "insert into xdb_qa_l (id, v) values (1, ?)", 'i:5', 'same')
    add('accented text into it, by the same statement', "insert into xdb_qa_l (id, v) values (1, ?)", '@v:Citt\u00e0', 'same')
    add('the accented letter is one byte there', "select hex(v) as h from xdb_qa_l where v <> '5'", None, ['43697474E0'])

# markers whose type the statement leaves open, inside a unit of work: the server refuses them
# ("could not determine data type of parameter $1": "select ?" before version 10, "? is null" on
# all), the driver declares them as text and prepares again - and what the unit of work did
# before must still be there
C2 = []
if KIND == 'pg':
    C2.append(('a table and a row, in a unit of work', "create temporary table xdb_qa_o (id int, v varchar(10))", None, 'done'))
    C2.append(('... the row', "insert into xdb_qa_o values (1, 'kept')", None, 'done'))
    C2.append(('a marker of open type in the select list', "select ? as v", 'x', ['x']))
    C2.append(('a marker of open type that is null', "select (? is null)::int as v", 'n:', ['1']))
    C2.append(('... and one that is not', "select (? is null)::int as v", 'x', ['0']))
    C2.append(('two of them and one the server can type', "select case when ? is null then 'none' else 'some' end as a, ? as b, v from xdb_qa_o where id = ?",
               'n:|y|i:1', ['none', 'y', 'kept']))
    C2.append(('a number where the type is open arrives as text', "select ? || '!' as v, pg_typeof(?)::text as t", 'i:5|i:5', ['5!', 'text']))
    C2.append(('an insert that depends on one', "insert into xdb_qa_o (id, v) select 2, 'two' where ? is not null", 'x', 'done'))
    C2.append(('the first row is still there, and the second', "select count(*)::int as n, min(v) as a, max(v) as b from xdb_qa_o", None, ['2', 'kept', 'two']))
    C2.append(('a statement that is wrong is still refused', "select ? as v from no_such_table_xdb", 'x', 'error'))
    C2.append(('... and the unit of work goes on', "select count(*)::int as n from xdb_qa_o", None, ['2']))


def run_cases(cases, iso):
    script = ['ISO ' + iso, 'C']
    prepared = None
    for label, stm, parms, want in cases:
        stm = stm.replace('\n', ' ')
        if want == 'same':
            # one prepared statement, executed with host variables of changing types
            if prepared != stm:
                script.append('P qa' + str(len(script)) + ' ' + stm)
                prepared = stm
                pname = 'qa' + str(len(script) - 1)
            script.append('E %s %s' % (pname, parms))
        elif want == 'done' and parms:
            script.append('P qa%d %s' % (len(script), stm))
            script.append('E qa%d %s' % (len(script) - 1, parms))
        elif want == 'done':
            script.append('X ' + stm)
        else:
            script.append('Q ' + stm + (' ## ' + parms if parms else ''))
    if iso != '*NONE':
        script.append('ROLLBACK')
    script.append('D')
    out = T.run(EMU, script, maxrows=3)
    B = [b for b in xdbqa.blocks(out) if b['cmd'][:2] in ('X ', 'Q ', 'E ')]
    if len(B) != len(cases):
        print('the harness answered %d requests of %d' % (len(B), len(cases)))
        print(out[-1500:])
        sys.exit(2)
    bad = 0
    for (label, stm, parms, want), b in zip(cases, B):
        what = None
        if want == 'error':
            if not b['err']:
                what = 'was taken, should be refused'
        elif b['err']:
            what = b['err'][:150]
        elif want not in ('done', 'same'):
            got = [xdbqa.text_of(v) for v in xdbqa.split(b['rows'][0])] if b['rows'] else None
            if got != want:
                what = 'got %s, it is %s' % (got, want)
        if what:
            bad += 1
        if what or VERBOSE:
            print('%-4s %-52s %s' % ('DIFF' if what else 'ok', label[:52], what or ''))
            if what:
                print('       %s' % stm[:150])
    return bad


bad = run_cases(C, '*NONE')
if C2:
    bad += run_cases(C2, '*CHG')
if KIND == 'pg':
    try:
        T.sql('drop function if exists xdb_qa_f(int)')
    except Exception:
        pass
print('%s %s: %d statements, %d differ' % ({'pg': 'PostgreSQL', 'mssql': 'SQL Server'}.get(KIND, 'MariaDB' if maria else 'MySQL'), '.'.join(str(x) for x in ver[:2]),
                                           len(C) + len(C2), bad))
sys.exit(1 if bad else 0)

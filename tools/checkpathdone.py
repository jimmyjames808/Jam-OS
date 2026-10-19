#!/usr/bin/env python3
"""Check that every function of the POSIX call layer that makes a path
ends it (user/posix/sys/files.h, "A path's reference";
docs/DIRHANDLE-PLAN.md, DL0 and DL1):

    checkpathdone.py [--selftest] [file.c ...]

A struct path made by path_get, path_get_at, path_set, path_of,
path_in_dir or file_where_path holds the directory description a relative path starts
from (`at`), and path_done lets it go. A function that makes one and
never calls path_done leaks the description and its channel, one of the
volume's 256, every time it runs. So each function in user/posix/sys/*.c
(or the files given) that calls one of those must call path_done too,
or say in its comment or body that its caller ends the path it makes:
"(path_done: the caller's)" (stat_at, the place hooks). The functions
that define the makers themselves are not their callers.

A function is the repository's shape: its name and parameters at the
start of a line, its body from a line holding only "{" to one holding
only "}". Prints file:line (the first call) for each function that
fails, and exits 1 if any did. Run from the repository root (`make
check` does)."""
import glob
import re
import sys

MAKERS = ('path_get', 'path_get_at', 'path_set', 'path_of', 'path_in_dir', 'file_where_path')
MAKER_CALL = re.compile(r'\b(' + '|'.join(MAKERS) + r')\s*\(')
DONE_CALL = re.compile(r'\bpath_done\s*\(')
EXEMPT = '(path_done: the caller\'s)'
NAME = re.compile(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(')


def strip_strings(line):
    """line without its string and character literals' contents."""
    return re.sub(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'', '""', line)


def functions(lines):
    """(name, first line number, comment above, body lines with numbers)
    for each function in lines."""
    out = []
    i = 0
    while i < len(lines):
        if lines[i].rstrip() != '{':
            i += 1
            continue
        start = i - 1   # the signature ends on the line above the brace
        while start > 0 and not NAME.match(lines[start]):
            start -= 1
        m = NAME.match(lines[start])
        name = m.group(1) if m else '?'
        comment = []
        c = start - 1
        if c >= 0 and lines[c].rstrip().endswith('*/'):
            while c >= 0:
                comment.insert(0, lines[c])
                if lines[c].lstrip().startswith('/*'):
                    break
                c -= 1
        body = []
        j = i + 1
        while j < len(lines) and lines[j].rstrip() != '}':
            body.append((j + 1, lines[j]))
            j += 1
        out.append((name, start + 1, comment, body))
        i = j + 1
    return out


def check_text(path, text):
    """The complaints for one file's text."""
    bad = []
    for name, _, comment, body in functions(text.split('\n')):
        if name in MAKERS:
            continue
        calls = [(n, l) for n, l in body if MAKER_CALL.search(strip_strings(l))]
        if not calls:
            continue
        said = EXEMPT in ''.join(comment) or any(EXEMPT in l for _, l in body)
        if said or any(DONE_CALL.search(strip_strings(l)) for _, l in body):
            continue
        n, l = calls[0]
        maker = MAKER_CALL.search(strip_strings(l)).group(1)
        bad.append(f'{path}:{n}: {name} makes a path ({maker}) and never calls path_done '
                   f'(files.h: "A path\'s reference"; or say "{EXEMPT}")')
    return bad


def selftest():
    good = '''
static long ok(long fd)
{
    struct path p;
    long r = path_get(fd, "x", &p);
    path_done(&p);
    return r;
}

/* Makes it for the caller. (path_done: the caller's) */
static long theirs(struct path *p)
{
    return path_set("/data", p);
}

long path_set(const char *abs, struct path *out)
{
    return path_get(AT_FDCWD, abs, out);
}

static void words(void)
{
    puts("path_get(");
}
'''
    leaky = '''
static long leak(long fd)
{
    struct path p;
    return path_of(d, &p);
}
'''
    if check_text('good.c', good):
        print('checkpathdone: selftest: a good file was refused:', check_text('good.c', good))
        return 1
    found = check_text('leaky.c', leaky)
    if len(found) != 1 or not found[0].startswith('leaky.c:5: leak '):
        print('checkpathdone: selftest: the leak was not found:', found)
        return 1
    return 0


def main(argv):
    if argv[1:] == ['--selftest']:
        return selftest()
    files = argv[1:] or sorted(glob.glob('user/posix/sys/*.c'))
    bad = []
    for path in files:
        with open(path, encoding='utf-8') as f:
            bad += check_text(path, f.read())
    for line in bad:
        print('checkpathdone: ' + line)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

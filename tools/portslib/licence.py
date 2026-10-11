"""The licences a recipe may name: SPDX expressions over a list of
identifiers the builder accepts (docs/M13.7-PLAN.md section 12).

A recipe's `licence` line is an SPDX licence expression: identifiers
joined by AND and OR, an exception after WITH, parentheses. Only the
identifiers below are taken. The list is short on purpose: a port under a
licence that isn't here is a licence someone has to read first
(CONTRIBUTING.md: permissive licences anywhere, GPL and LGPL only in a
separate program with its source beside it), and adding it is a one-line
change a review sees. `LicenseRef-` names and the deprecated `+` suffix
are refused for the same reason.
"""
import re

# Permissive: may be linked into anything, Jam OS's own code included.
PERMISSIVE = {
    "0BSD", "Apache-2.0", "BSD-1-Clause", "BSD-2-Clause", "BSD-3-Clause", "BSL-1.0",
    "CC0-1.0", "curl", "HPND", "ISC", "Libpng", "libpng-2.0", "MirOS", "MIT", "MIT-0",
    "NCSA", "OFL-1.1", "PostgreSQL", "PSF-2.0", "Python-2.0", "Unlicense", "Vim", "X11",
    "X11-distribute-modifications-variant", "Zlib", "bzip2-1.0.6", "blessing",
}
# Copyleft: only in a separate program (CONTRIBUTING.md), its complete
# source shipped beside it.
COPYLEFT = {
    "GPL-2.0-only", "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later",
    "LGPL-2.1-only", "LGPL-2.1-or-later", "LGPL-3.0-only", "LGPL-3.0-or-later",
    "MPL-2.0",
}
EXCEPTIONS = {
    "Autoconf-exception-3.0", "Bison-exception-2.2", "Classpath-exception-2.0",
    "GCC-exception-3.1", "LLVM-exception",
}
KNOWN = PERMISSIVE | COPYLEFT

TOKEN = re.compile(r"\s*(\(|\)|[A-Za-z0-9.+-]+)")


def _tokens(text):
    out, at = [], 0
    while at < len(text):
        m = TOKEN.match(text, at)
        if not m:
            if text[at:].strip():
                raise ValueError("unexpected '%s'" % text[at:].strip()[:20])
            break
        out.append(m.group(1))
        at = m.end()
    return out


class _Parser:
    """A recursive-descent reader of one expression, into a small tree:
    ("id", name, exception or None), ("and", [..]), ("or", [..])."""

    def __init__(self, tokens):
        self.t, self.i = tokens, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self):
        tok = self.peek()
        if tok is None:
            raise ValueError("the expression ends too soon")
        self.i += 1
        return tok

    def expr(self):
        parts = [self.term()]
        while self.peek() == "OR":
            self.take()
            parts.append(self.term())
        return parts[0] if len(parts) == 1 else ("or", parts)

    def term(self):
        parts = [self.factor()]
        while self.peek() == "AND":
            self.take()
            parts.append(self.factor())
        return parts[0] if len(parts) == 1 else ("and", parts)

    def factor(self):
        tok = self.take()
        if tok == "(":
            inner = self.expr()
            if self.take() != ")":
                raise ValueError("a '(' without its ')'")
            return inner
        if tok in ("AND", "OR", "WITH", ")"):
            raise ValueError("'%s' where a licence was expected" % tok)
        if tok.startswith("LicenseRef-") or tok.endswith("+"):
            raise ValueError("'%s': name a licence on the list (tools/portslib/licence.py)"
                             % tok)
        if tok not in KNOWN:
            raise ValueError("'%s' is not a licence the builder accepts "
                             "(tools/portslib/licence.py; SPDX identifiers, case matters)" % tok)
        exc = None
        if self.peek() == "WITH":
            self.take()
            exc = self.take()
            if exc not in EXCEPTIONS:
                raise ValueError("'%s' is not an exception the builder accepts" % exc)
        return ("id", tok, exc)


def parse(text):
    """The expression's tree; ValueError saying what is wrong."""
    tokens = _tokens(text)
    if not tokens:
        raise ValueError("no licence")
    p = _Parser(tokens)
    tree = p.expr()
    if p.peek() is not None:
        raise ValueError("'%s' after a whole expression" % p.peek())
    return tree


def _avoidable(tree, pred):
    """Whether a choice of OR branches gives a licence set with no
    identifier for which pred is true."""
    kind = tree[0]
    if kind == "id":
        return not pred(tree[1])
    if kind == "or":
        return any(_avoidable(t, pred) for t in tree[1])
    return all(_avoidable(t, pred) for t in tree[1])


def binds_lgpl(text):
    """True when every reading of the expression includes an LGPL licence:
    a static link of it leaves the duty to let users relink."""
    return not _avoidable(parse(text), lambda n: n.startswith("LGPL-"))


def copyleft(text):
    """True when every reading of the expression includes a copyleft
    licence: the package's complete source must be shipped beside it."""
    return not _avoidable(parse(text), lambda n: n in COPYLEFT)

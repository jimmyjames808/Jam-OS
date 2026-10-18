#!/usr/bin/env python3
"""Check that the POSIX call layer and libjam, as libc.a holds them, never
reach the C library's allocator (docs/CR-PLAN.md, LH):

    checklayerheap.py <nm> <libmusl.a> <object>...

The layer allocates from its own heap (user/posix/sys/heap.c: jam_malloc
and the rest), never from malloc: a program may replace malloc with its
own, which may hold its lock while it calls into the layer, and a signal
handler's calls may come while the program is inside it. musl's own
allocator (__libc_malloc and the rest, which musl's internals call) is no
better: in a program that keeps it, it is the program's malloc.

So every object given (the layer's, before their renaming, and libjam's
as libc.a has them) is checked: none may name an allocator function, nor
any of musl's that reaches one. A member of libmusl.a reaches the
allocator if it defines an allocator function, names one, or names a
function that a member reaching it defines, and so on (a symbol several
members define counts if any of them reaches: the over-cautious way).
Names the given objects define among themselves are theirs, not musl's.

ALLOWED is the one exception: __malloc_atfork, by which fork.c takes and
gives malloc's lock around vfork and clone as musl's fork does (it
allocates nothing).

As a check of the check, strdup and fopen, which allocate, must be found
reaching it. Prints each object that reaches it, with the chain, and
exits 1 if there is any. Run from the repository root (`make check`
does)."""
import subprocess
import sys

ALLOCATOR = set("""
malloc calloc realloc free aligned_alloc posix_memalign memalign valloc pvalloc
reallocarray malloc_usable_size
__libc_malloc __libc_malloc_impl __libc_calloc __libc_realloc __libc_free
""".split())

ALLOWED = {'__malloc_atfork'}

MUST_REACH = ('strdup', 'fopen')


def symbols(nm, paths):
    """[(name, defined set, undefined set)] for each object (or archive
    member) in nm's output, in order."""
    out = subprocess.run([nm] + paths, capture_output=True, text=True, check=True).stdout
    objs = []
    for line in out.splitlines():
        if not line.strip():
            continue
        if line.endswith(':') and ' ' not in line:
            objs.append((line[:-1], set(), set()))
            continue
        parts = line.split()
        if not objs or len(parts) < 2:
            continue
        kind, name = parts[-2], parts[-1]
        if kind in 'Uw':
            objs[-1][2].add(name)
        elif kind.upper() in 'TDBRVWCGSI':
            objs[-1][1].add(name)
    return objs


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    nm, musl, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    members = symbols(nm, [musl])
    definers = {}
    for i, (_, defs, _) in enumerate(members):
        for s in defs:
            definers.setdefault(s, []).append(i)

    # How each member that reaches the allocator does (a chain of names),
    # grown until nothing more is found.
    how = {}
    for i, (_, defs, undefs) in enumerate(members):
        mine = sorted(defs & ALLOCATOR) or sorted(undefs & ALLOCATOR)
        if mine:
            how[i] = [mine[0]]
    grew = True
    while grew:
        grew = False
        for i, (_, _, undefs) in enumerate(members):
            if i in how:
                continue
            for s in sorted(undefs):
                j = next((j for j in definers.get(s, ()) if j in how), None)
                if j is not None:
                    how[i] = [s] + how[j]
                    grew = True
                    break

    def symbol_reaches(s):
        if s in ALLOCATOR:
            return [s]
        j = next((j for j in definers.get(s, ()) if j in how), None)
        return None if j is None else [s] + how[j]

    for s in MUST_REACH:
        if not symbol_reaches(s):
            sys.exit(f'checklayerheap: {s} not found reaching the allocator in {musl}: '
                     'the check itself is broken')

    objs = symbols(nm, paths)
    ours = set().union(*(defs for _, defs, _ in objs))
    bad = 0
    for name, _, undefs in objs:
        for s in sorted(undefs - ours - ALLOWED):
            chain = symbol_reaches(s)
            if chain:
                print(f'{name}: {" -> ".join(chain)}')
                bad += 1
    if bad:
        print(f'checklayerheap: {bad} reference(s) to the C library\'s allocator from the '
              'layer or libjam (allocate with jam_malloc & co.: user/posix/sys/heap.c)')
        sys.exit(1)


if __name__ == '__main__':
    main()

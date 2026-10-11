# libc-test (ports/libc-test/recipe): the one program, by files/Makefile.
# gen.py says which tests are built (expected-failures.txt marks the
# dynamic-linking ones `skip`).

build() {
    tests=$(python3 "$portdir/files/gen.py" tests "$srcdir" "$portdir/files/expected-failures.txt" | tr '\n' ' ')
    mkdir _build
    cd _build
    "$make" -j"$jobs" -f "$portdir/files/Makefile" SRC="$srcdir" PORTFILES="$portdir/files" \
        CC="$CC" OBJCOPY="$OBJCOPY" TESTS="$tests"
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp _build/libc-test "$destdir$prefix/bin/libc-test"
}

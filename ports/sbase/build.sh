# sbase (ports/sbase/recipe): the three multi-call programs. Which tools
# each holds is the recipe's `multicall` lines (the one place); the rules
# are files/Makefile's, the dispatching main files/mkbox.sh's.
boxes="sbase-plain sbase-ro sbase-rw"

# The names on program $1's multicall lines, in the recipe's order.
tools() {
    awk -v p="$1" '$1 == "multicall" && $2 == p {
        for (i = 3; i <= NF && $i !~ /^#/; i++) printf "%s ", $i
    }' "$portdir/recipe"
}

build() {
    set --
    for b in $boxes; do
        set -- "$@" "TOOLS_$b=$(tools "bin/$b")"
    done
    mkdir _build
    cd _build
    "$make" -j"$jobs" -f "$portdir/files/Makefile" SRC="$srcdir" PORTFILES="$portdir/files" \
        BOXES="$boxes" CC="$CC" AR="$AR" "$@"
}

install() {
    mkdir -p "$destdir$prefix/bin"
    for b in $boxes; do
        cp "_build/$b" "$destdir$prefix/bin/$b"
    done
}

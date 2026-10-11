# Lua (ports/lua/recipe): lua.c and the libraries, the files of upstream's
# src/Makefile (CORE_O, LIB_O, LUA_O) in its order, compiled with our flags
# (see the recipe).
objs="lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser
      lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib
      lmathlib loadlib loslib lstrlib ltablib lutf8lib linit lua"

build() {
    cd src
    for f in $objs; do
        $CC -std=gnu99 -O2 -w -DLUA_USE_POSIX -c "$f.c" -o "$f.o"
    done
    # The output's name picks the program's list (the compiler wrapper).
    $CC -o lua $(for f in $objs; do printf '%s.o ' "$f"; done)
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp src/lua "$destdir$prefix/bin/lua"
}

# libffi's build: upstream's configure and make as the `configure` style
# runs them, then libffi's own ffi_call tests linked into one program,
# ffi-tests (override/ffi-tests.c has the driver and says how they run).
# Style custom because the tests are not a part of upstream's install.

build() {
    # --disable-exec-static-tramp: static trampolines read /proc/self/maps and
    # map a table of themselves; closures are not used, and Jam OS has no /proc.
    # --disable-builddir builds in the source directory (the install step
    # below runs there); --disable-multi-os-directory puts the library in
    # lib/, not lib64/.
    ./configure --host=x86_64-linux-musl --prefix="$prefix" \
        --disable-shared --enable-static --disable-builddir --disable-docs \
        --disable-multi-os-directory --disable-exec-static-tramp
    "$make" -j"$jobs"
    build_tests
}

# Every ffi_call test of libffi.call and libffi.complex, as an object with its
# main, exit and abort renamed (override/ffi-tests.c): the closure tests
# (cls_*) need executable memory and are left out, and so are the tests for
# Windows' and 32-bit x86's calling conventions, which are empty on x86-64
# but are not about anything Jam OS has.
build_tests() {
    t=testsuite/jamos
    mkdir -p "$t"
    : > "$t/ffi-tests-list.h"
    objs=
    for f in testsuite/libffi.call/*.c testsuite/libffi.complex/*.c; do
        n=${f##*/}
        n=${n%.c}
        id=$(printf '%s' "$n" | tr -c 'A-Za-z0-9\n' _)   # i128-1 -> i128_1
        case $n in
        cls_*|align_stdcall|closure_thiscall_fastcall_pop) continue ;;
        esac
        "$CC" -O2 -std=gnu11 -I. -Iinclude -I"${f%/*}" -Dmain="ffitest_${id}_main" \
            -Dexit=ffitest_exit -Dabort=ffitest_abort \
            -Wno-attributes -Wno-format -c "$f" -o "$t/$n.o"
        # The tests share helper names (return_c, many...): only the renamed
        # main stays visible.
        "$OBJCOPY" --keep-global-symbol="ffitest_${id}_main" "$t/$n.o"
        printf 'T(%s, "%s")\n' "$id" "$n" >> "$t/ffi-tests-list.h"
        objs="$objs $t/$n.o"
    done
    "$CC" -O2 -std=gnu11 -I"$t" -c testsuite/ffi-tests.c -o "$t/ffi-tests.o"
    # shellcheck disable=SC2086 # the object files are words on purpose
    "$CC" -o ffi-tests "$t/ffi-tests.o" $objs .libs/libffi.a -lm
}

install() {
    "$make" install DESTDIR="$destdir"
    mkdir -p "$destdir$prefix/bin"
    cp ffi-tests "$destdir$prefix/bin/ffi-tests"
}

# llvm-runtimes (ports/llvm-runtimes/recipe): two CMake builds of LLVM's
# runtimes/ directory with the port's clang drivers (tools/jamos-clang,
# jamos-clang++, which compile for x86_64-unknown-linux-musl against the
# port's sysroot): first compiler-rt's builtins and crt objects, then
# libunwind, libc++abi and libc++, which the first's builtins could serve.
# CMake never links a test program (there is nothing to link one with
# yet: these are the runtimes), so its checks are compiles only, and the
# answers a compile can't give are given here. The code is position-
# independent, as libc.a's: a program links anywhere (Chromium's above
# 4 GiB, its recipe's image-base), static and non-PIE either way.
cmake_common() {
    cmake -G Ninja -S "$srcdir/runtimes" -B "$1" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_ASM_COMPILER="$CC" \
        -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-linux-musl \
        -DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-linux-musl \
        -DCMAKE_ASM_COMPILER_TARGET=x86_64-unknown-linux-musl \
        -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" -DCMAKE_NM="$NM" -DCMAKE_STRIP="$STRIP" \
        -DCMAKE_OBJCOPY="$OBJCOPY" -DCMAKE_OBJDUMP="$OBJDUMP" \
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCMAKE_FIND_ROOT_PATH="$JAMOS_SYSROOT" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-musl \
        -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF \
        "$@"
}

build() {
    for d in compiler-rt libunwind libcxxabi libcxx; do
        cp "$srcdir/$d/LICENSE.TXT" "$srcdir/LICENSE-$d.TXT"
    done
    # compiler-rt: the builtins and crtbegin/crtend only (no sanitizers,
    # profiling or fuzzing: nothing on Jam OS runs them yet).
    cmake_common "$srcdir/_build-crt" -DLLVM_ENABLE_RUNTIMES=compiler-rt \
        -DCOMPILER_RT_BUILD_BUILTINS=ON -DCOMPILER_RT_BUILD_CRT=ON \
        -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF \
        -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF \
        -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF \
        -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
    cmake --build "$srcdir/_build-crt" --parallel "$jobs"

    # libunwind, libc++abi, libc++: static, on musl, over compiler-rt.
    # __cxa_thread_atexit_impl is glibc's (musl has none): told so, or the
    # link-less check would say yes and libc++abi would call it for every
    # thread_local with a destructor; libc++abi then keeps its own list.
    cmake_common "$srcdir/_build-rt" -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
        -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_STATIC=ON \
        -DLIBUNWIND_USE_COMPILER_RT=ON -DLIBUNWIND_INCLUDE_TESTS=OFF \
        -DLIBUNWIND_INSTALL_HEADERS=ON \
        -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_STATIC=ON \
        -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
        -DLIBCXXABI_ENABLE_STATIC_UNWINDER=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
        -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
        -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON \
        -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON \
        -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF \
        -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
        -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF
    cmake --build "$srcdir/_build-rt" --parallel "$jobs"
}

install() {
    DESTDIR=$destdir cmake --install "$srcdir/_build-crt"
    DESTDIR=$destdir cmake --install "$srcdir/_build-rt"
    # compiler-rt installs under its own names (lib/linux/..-x86_64): the
    # names tools/jamos-clang links by.
    lib=$destdir$prefix/lib
    for f in libclang_rt.builtins clang_rt.crtbegin clang_rt.crtend; do
        case $f in lib*) ext=a ;; *) ext=o ;; esac
        found=$(find "$destdir" -name "$f-x86_64.$ext" -o -name "$f.$ext" | head -1)
        [ -n "$found" ] || { echo "llvm-runtimes: compiler-rt installed no $f" >&2; exit 1; }
        [ "$found" = "$lib/$f.$ext" ] || mv "$found" "$lib/$f.$ext"
    done
}

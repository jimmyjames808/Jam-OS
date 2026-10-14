# cr-rust-hello (ports/cr-rust-hello/recipe): cargo from Chromium's Rust
# ($JAMOS_RUST), with the standard library built from its rust-src and the
# crates vendored beside it (-Zbuild-std; RUSTC_BOOTSTRAP lets the release
# driver take that unstable flag), for x86_64-unknown-linux-musl. The link
# is the port's compiler driver (jamos-clang, $CC): musl's libc and
# libunwind come from the sysroot (-C link-self-contained=no keeps rustc's
# own copies and start files out), the program is static and not
# position-independent (Jam OS runs ET_EXEC only), and its list is the
# link argument the driver takes.
build() {
    target=x86_64-unknown-linux-musl
    library=$JAMOS_RUST/lib/rustlib/src/rust/library
    export RUSTC_BOOTSTRAP=1 RUSTC=$JAMOS_RUST/bin/rustc
    export CARGO_HOME=$TMPDIR/cargo-home CARGO_TARGET_DIR=$srcdir/target
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER=$CC
    export RUSTFLAGS="-C link-self-contained=no -C relocation-model=static \
-C link-arg=--jamos-wants=$portdir/wants/cr-rust-hello \
--remap-path-prefix=$srcdir=/usr/src/cr-rust-hello --remap-path-prefix=$JAMOS_RUST=/rust"
    "$JAMOS_RUST/bin/cargo" build --release --offline -j "$jobs" --target $target \
        -Zbuild-std=std,panic_abort \
        --config "source.crates-io.replace-with='vendored-sources'" \
        --config "source.vendored-sources.directory='$library/vendor'"
}
install() {
    mkdir -p "$destdir$prefix/bin" "$destdir$prefix/share/cr-rust-hello"
    cp target/x86_64-unknown-linux-musl/release/cr-rust-hello "$destdir$prefix/bin/cr-rust-hello"
    cp expected.txt expected-panic.txt "$destdir$prefix/share/cr-rust-hello/"
}

# hello-wayland's build: upstream's Makefile wants ImageMagick for cat.h and
# the host's pkg-config for the scanner, so the same three steps are done
# here with the cross tools: cat.h from cat.png (files/cathdr.py, byte for byte
# what ImageMagick makes), the xdg-shell client code from wayland-protocols'
# XML in the sysroot (wayland-scanner, the host build), and one link.

build() {
    python3 "$portdir/files/cathdr.py" cat.png cat.h
    xml=$($PKG_CONFIG --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header "$xml" xdg-shell-client-protocol.h
    wayland-scanner private-code "$xml" xdg-shell-protocol.c
    # shellcheck disable=SC2046 # pkg-config's words on purpose
    "$CC" -std=c11 -O2 -Wall -Wno-unused-parameter -o hello-wayland \
        main.c xdg-shell-protocol.c shm.c $($PKG_CONFIG --cflags --libs wayland-client)
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp hello-wayland "$destdir$prefix/bin/hello-wayland"
}

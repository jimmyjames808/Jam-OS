# linux-uapi (ports/linux-uapi/recipe): the kernel's headers_install for
# x86, as Alpine's linux-headers builds it. The kernel's make chooses its
# own compilers (HOSTCC=gcc for scripts/unifdef; nothing is built for the
# target), so the port's CC is not used.
build() {
    $make -s ARCH=x86 HOSTCC=gcc -j"$jobs" headers
}
install() {
    hdr=$destdir$prefix
    $make -s ARCH=x86 HOSTCC=gcc INSTALL_HDR_PATH="$hdr" headers_install
    rm -rf "$hdr/include/drm"
    find "$hdr" \( -name .install -o -name ..install.cmd \) -exec rm -f {} +
    # Two names that differ only in case can't both live on FAT32: drop the
    # one with capitals (the netfilter targets: xt_MARK.h beside xt_mark.h).
    (cd "$hdr" && find include -type f | sort) |
        awk '{ k = tolower($0); if (k in seen) { print (seen[k] ~ /[A-Z]/ ? seen[k] : $0) } else seen[k] = $0 }' |
        while IFS= read -r f; do
            rm -f "$hdr/$f"
        done
}

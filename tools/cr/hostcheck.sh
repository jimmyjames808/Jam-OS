#!/bin/bash
# The PC check (docs/CR-PLAN.md section 15, track CT1): before anything of
# Chromium's is fetched, say whether this machine (the PC's WSL) can build
# it: free disk under the build directory, WSL's memory cap and swap, CPU
# threads, the kernel, and the host tools the build and the toolchain
# need. Every number is printed; a shortfall is a FAIL line and exit 1,
# so a track stops with the numbers in hand (CR-PLAN's Q1 says what the
# owner may change on the Windows side: .wslconfig's memory and swap).
#
#   tools/cr/hostcheck.sh [outdir]
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh \
#       tools/cr/hostcheck.sh build/cr/hostcheck
#
# outdir (under build/, so remote-test.sh brings it back) gets the report
# as hostcheck.txt. The wants, from the plan's budget, can be lowered for a
# smaller job (the toolchain alone needs a few GB): CR_DISK_GB (150 free
# under CR_DIR), CR_MEM_GB (22: WSL reports a little under its 24 GB cap's
# worth; the owner's PC shows 23), CR_SWAP_GB (8: below it a WARN, not a
# FAIL: the plan asks 16, and jobs can be capped instead), CR_THREADS (16).
# CR_DIR is the build tree's home (default ~/cr, made if missing: its file
# system is what counts).
set -u
out=${1:-}
want_disk=${CR_DISK_GB:-150}
want_mem=${CR_MEM_GB:-22}
want_swap=${CR_SWAP_GB:-8}
want_threads=${CR_THREADS:-16}
dir=${CR_DIR:-$HOME/cr}
# The ports builder's venv (Meson, Ninja, CMake) is on PATH as tools/ports.py puts it.
tools=${JAMOS_TOOLS:-$HOME/.cache/jamos/tools}
PATH=$tools/bin:$PATH

fails=0
report=$(mktemp "${TMPDIR:-/tmp}/hostcheck.XXXXXX")
trap 'rm -f "$report"' EXIT
say() { printf '%s\n' "$*" | tee -a "$report"; }
ok() { say "ok    $*"; }
warn() { say "WARN  $*"; }
fail() { say "FAIL  $*"; fails=$((fails + 1)); }

say "hostcheck: $(uname -s) $(uname -m)"
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
    fail "not a Linux x86_64 host: Chromium's prebuilt clang and Rust are Linux x86_64 binaries"
fi

# Disk: free space on the file system that holds the build tree.
mkdir -p "$dir" 2>/dev/null
free_gb=$(df -Pk "$dir" 2>/dev/null | awk 'NR == 2 { printf "%d", $4 / 1048576 }')
if [ -z "$free_gb" ]; then
    fail "disk: can't read df for $dir"
elif [ "$free_gb" -ge "$want_disk" ]; then
    ok "disk: $free_gb GB free under $dir (want $want_disk)"
else
    fail "disk: $free_gb GB free under $dir, want $want_disk"
fi

# Memory and swap, as WSL's cap gives them.
mem_gb=$(awk '/^MemTotal:/ { printf "%d", $2 / 1048576 + 0.5 }' /proc/meminfo 2>/dev/null)
swap_gb=$(awk '/^SwapTotal:/ { printf "%d", $2 / 1048576 + 0.5 }' /proc/meminfo 2>/dev/null)
avail_gb=$(awk '/^MemAvailable:/ { printf "%d", $2 / 1048576 }' /proc/meminfo 2>/dev/null)
if [ -z "$mem_gb" ]; then
    fail "memory: no /proc/meminfo"
elif [ "$mem_gb" -ge "$want_mem" ]; then
    ok "memory: $mem_gb GB ($avail_gb GB available now; want $want_mem)"
else
    fail "memory: $mem_gb GB, want $want_mem (.wslconfig: memory=24GB; or cap the jobs at 12)"
fi
if [ -n "$swap_gb" ] && [ "$swap_gb" -ge "$want_swap" ]; then
    ok "swap: $swap_gb GB (want $want_swap)"
else
    warn "swap: ${swap_gb:-?} GB, want $want_swap (.wslconfig: swap=16GB): a big link may need fewer jobs"
fi

threads=$(nproc 2>/dev/null || echo 0)
if [ "$threads" -ge "$want_threads" ]; then
    ok "threads: $threads (want $want_threads)"
else
    fail "threads: $threads, want $want_threads"
fi
ok "kernel: $(uname -r)"

# Host tools: the toolchain's (tools/cr/toolchain.sh, ports/llvm-runtimes,
# ports/linux-uapi) and Chromium's build's (CR-PLAN section 15). Node.js:
# the tarball's own prebuilt (third_party/node) or the system's, as Alpine
# links it: missing is a WARN until CB1 says which the tarball needs.
need() {   # need <what it is for> <command> [version option]
    local what=$1 cmd=$2 vopt=${3:---version} path ver
    path=$(command -v "$cmd" 2>/dev/null)
    if [ -z "$path" ]; then
        fail "tool: $cmd missing ($what)"
        return
    fi
    ver=$("$path" $vopt 2>&1 < /dev/null | head -1)
    ok "tool: $cmd ${ver:-} ($path)"
}
need "everything" python3
need "the toolchain's downloads" curl
need "the toolchain's downloads" sha256sum
need "the tarballs" xz
need "the runtimes' build" cmake
need "the runtimes' and Chromium's build" ninja
need "the UAPI headers' install" make
need "the UAPI headers' install" gcc
need "the UAPI headers' install" rsync
need "Chromium's build" gperf
need "Chromium's build" pkg-config
need "Chromium's build" bison
need "Chromium's build" flex
need "Chromium's build" git
if command -v node > /dev/null 2>&1; then
    ok "tool: node $(node --version 2>&1 | head -1)"
else
    warn "tool: node missing (Chromium's tarball may bring its own: CB1 says)"
fi
cmake_ver=$(cmake --version 2>/dev/null | awk 'NR == 1 { print $3 }')
if [ -n "$cmake_ver" ]; then
    major=${cmake_ver%%.*}
    rest=${cmake_ver#*.}
    minor=${rest%%.*}
    if [ "$major" -lt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -lt 20 ]; }; then
        fail "tool: cmake $cmake_ver, LLVM's runtimes want 3.20 or newer"
    fi
fi

# What is already fetched (tools/cr/toolchain.sh).
cache=${JAMOS_TOOLCHAINS:-$HOME/.cache/jamos/toolchains}
if [ -d "$cache" ]; then
    say "info  toolchains in $cache: $(ls "$cache" 2>/dev/null | tr '\n' ' ')"
    say "info  $(du -sh "$cache" 2>/dev/null | awk '{ print $1 }') used there"
fi

if [ "$fails" -gt 0 ]; then
    say "hostcheck: FAIL ($fails shortfall(s))"
else
    say "hostcheck: PASS"
fi
if [ -n "$out" ]; then
    mkdir -p "$out"
    cp "$report" "$out/hostcheck.txt"
fi
[ "$fails" -eq 0 ]

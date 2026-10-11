# CMake's toolchain file for a port cross-built for Jam OS (tools/ports.py
# gives it to every `style cmake` build; docs/M13.7-PLAN.md section 5).
#
# The compiler and the binutils are the port's own (build/ports/<name>/bin,
# first on PATH, under the x86_64-linux-musl- names: tools/jamos-cc with
# the port's sysroot). The system is "Linux" because musl's headers are
# Linux's; jamos-cc defines __jamos__ and not __linux__, so a port's
# Linux-only code is not taken. Everything is static. find_package,
# find_library and find_path look only in the port's sysroot (what it
# `needs`), never on the Mac; programs (code generators) come from the Mac.
# A try_run can't run on the Mac: a port answers its own in its recipe
# (`cmake -D<VAR>=<answer>`).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-linux-musl-gcc)
# No C++ for Jam OS yet: this one refuses, so a port that needs it fails.
set(CMAKE_CXX_COMPILER x86_64-linux-musl-g++)
set(CMAKE_AR x86_64-linux-musl-ar CACHE FILEPATH "ar")
set(CMAKE_RANLIB x86_64-linux-musl-ranlib CACHE FILEPATH "ranlib")
set(CMAKE_STRIP x86_64-linux-musl-strip CACHE FILEPATH "strip")

set(CMAKE_FIND_ROOT_PATH $ENV{JAMOS_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "static only")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(CMAKE_SKIP_RPATH ON)

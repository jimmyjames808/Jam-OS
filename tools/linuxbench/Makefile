# linuxbench: one static x86_64 Linux program (README.md here). From the
# repository's root:
#     make -C tools/linuxbench          build it: gcc on an x86_64 Linux,
#                                       else a Linux cross compiler if one
#                                       is installed (below), else say how
#     make -C tools/linuxbench docker   build it in Docker's gcc image (the
#                                       first run downloads the image)
# The program is build/linuxbench/linuxbench. It is not part of Jam OS's
# build: the top-level Makefile never runs this one.

SRCS    := basic.c cpus.c cross.c files.c ipc.c main.c method.c sysinfo.c
HDRS    := lb.h
OUTDIR  := ../../build/linuxbench
BIN     := $(OUTDIR)/linuxbench
CFLAGS  := -std=gnu17 -O2 -Wall -Wextra -D_GNU_SOURCE -pthread
LDFLAGS := -static -pthread

HOST := $(shell uname -s)-$(shell uname -m)
ifeq ($(HOST),Linux-x86_64)
LINUX_CC ?= gcc
else
# The first Linux x86_64 compiler found: musl-cross or GNU cross gcc, or zig.
LINUX_CC ?= $(firstword \
    $(foreach c,x86_64-linux-musl-gcc x86_64-linux-gnu-gcc,\
        $(if $(shell command -v $(c) 2>/dev/null),$(c))) \
    $(if $(shell command -v zig 2>/dev/null),zig cc -target x86_64-linux-musl))
endif

.PHONY: all docker clean

all: $(BIN)

$(BIN): $(SRCS) $(HDRS) Makefile
ifeq ($(strip $(LINUX_CC)),)
	@echo "linuxbench: no Linux x86_64 compiler here. Either:"
	@echo "  - make -C tools/linuxbench docker   (Docker Desktop running; it downloads gcc's image)"
	@echo "  - install one (e.g. 'brew install zig', or a musl-cross gcc) and run make again"
	@echo "  - or build it on the PC's live Linux (tools/linuxbench/README.md, step 4):"
	@echo "      gcc -std=gnu17 -O2 -D_GNU_SOURCE -static -pthread -o linuxbench *.c"
	@exit 1
else
	@mkdir -p $(OUTDIR)
	$(LINUX_CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	@echo "linuxbench: built $@ (copy it to the SanDisk: README.md, step 2)"
endif

# The repository mounted at /src; x86_64 even on an Apple-silicon Mac
# (emulated: slow, but it is one small build).
docker:
	docker run --rm --platform linux/amd64 -v "$(abspath ../..)":/src \
	    -w /src/tools/linuxbench gcc:14 make LINUX_CC=gcc

clean:
	rm -f $(BIN)

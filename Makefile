# Jam OS build.  `make` builds the kernel, the user programs and the bootfs
# image, `make image` the bootable USB image, `make run` boots it in QEMU
# (q35 + UEFI + xHCI USB boot, like a PC). `make syscalls` regenerates the
# system call glue after abi/syscalls.def changes.

CROSS   ?= x86_64-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AR      := $(CROSS)ar
STRIP   := $(CROSS)strip
OBJDUMP := $(CROSS)objdump

# `make KTESTS=0` leaves out the in-kernel tests (kernel/test) and compiles
# the DBG_HOOK injection points away, building into build/noktests. The
# boot menu's "All tests" and "Benchmark" entries need the default KTESTS=1.
KTESTS  ?= 1
BUILD   := $(if $(filter 0,$(KTESTS)),build/noktests,build)

# The network's default mode (ARCHITECTURE.md "Networking"): what a boot
# with no `vlan=` word does. A build of the public tree sends plain
# untagged Ethernet; a network that needs a VLAN sets JAMOS_VLAN in a
# local.mk at the top of the tree (git-ignored; local.mk.example shows
# how), e.g. `JAMOS_VLAN := 21`, or on the command line
# (`make JAMOS_VLAN=21`). `none` (or `untagged`) is untagged; anything but
# that or a VLAN id 1..4094 stops the build. The environment is not asked:
# an exported JAMOS_VLAN nobody remembers must not change a build.
-include local.mk
NET_ORIGIN := $(origin JAMOS_VLAN)
ifeq ($(NET_ORIGIN),undefined)
JAMOS_VLAN := none
NET_FROM   := $(if $(wildcard local.mk),local.mk sets no JAMOS_VLAN,no local.mk)
else ifeq ($(NET_ORIGIN),file)
NET_FROM   := local.mk
else ifeq ($(NET_ORIGIN),command line)
NET_FROM   := make command line
else
$(error JAMOS_VLAN comes from local.mk or the make command line, not the $(NET_ORIGIN))
endif
ifneq ($(filter none untagged,$(strip $(JAMOS_VLAN))),)
NET_DEFAULT := CMDLINE_VLAN_UNTAGGED
NET_TEXT    := net untagged
NET_SAY     := untagged
else
# (the case patterns in parentheses: an unbalanced ")" would end $(shell))
NET_ID := $(shell n='$(strip $(subst ',,$(JAMOS_VLAN)))'; case "$$n" in (''|0*|*[!0-9]*) ;; \
            (*) [ "$$n" -le 4094 ] 2>/dev/null && echo "$$n" ;; esac)
ifeq ($(NET_ID),)
$(error JAMOS_VLAN must be none or a VLAN id 1..4094, not "$(JAMOS_VLAN)" ($(NET_FROM)))
endif
NET_DEFAULT := $(NET_ID)
NET_TEXT    := net vlan$(NET_ID)
NET_SAY     := VLAN $(NET_ID)
endif
KERNEL  := $(BUILD)/jamos.elf
KERNEL_DEBUG := $(BUILD)/jamos.debug.elf
IMAGE   := $(BUILD)/jamos.img
BOOTFS  := $(BUILD)/bootfs.img
# The Mac's signing tool (tools/jamos-sign.c, below): one for both builds.
SIGN_TOOL := build/host/jamos-sign
# The PC's check of a boot menu, on the Mac (tools/menucheck.c, below).
MENU_TOOL := build/host/menucheck
# The page-hash table with libos's and the kernel's SHA-256, on the Mac
# (tools/pagetable.c, below).
PAGETABLE_TOOLS := build/host/pagetable-libos build/host/pagetable-kernel
# libfun's smooth text on the Mac: the preview and its sanitised check
# (tools/fontpreview.c, below).
FONT_TOOL  := build/host/fontpreview
FONT_CHECK := build/host/fontcheck
# The partition table library on the Mac, sanitised (tools/gptcheck.c, below).
GPT_CHECK  := build/host/gptcheck
# The ext library on the Mac, sanitised (tools/extcheck.c, below).
EXT_CHECK  := build/host/extcheck
# The host tool for ext volumes (tools/jamext, below): the image's /data.
JAMEXT     := build/host/jamext
# The ports formats' readers on the Mac, sanitised (tools/pkgcheck.c, below).
PKG_CHECK  := build/host/pkgcheck
ROOTPORT_CHECK := build/host/rootportcheck
# The kernel's AVL tree on the Mac, sanitised (tools/avlcheck.c, below).
AVL_CHECK  := build/host/avlcheck
# The kernel's interval tree on the Mac, sanitised (tools/itreecheck.c, below).
ITREE_CHECK := build/host/itreecheck
# The files a stick has once `update -w` has written a build: its own and
# the previous one (what boot/limine.conf may name; `make check`).
STICK_FILES := /boot/jamos.elf /boot/bootfs.img /boot/prev-jamos.elf /boot/prev-bootfs.img
# The ESP ends at 256 MiB (it starts at 1 MiB: 255 MiB, FAT32 with 2 KiB
# clusters); the data partition (/data, "JAMOS-DATA") fills the rest
# of the image: 64 MiB in QEMU, the rest of the stick once
# tools/write-usb.sh has grown it. `update` keeps three builds on the ESP
# while it writes one (the stick's, the previous one and the new one:
# user/services/init/espwrite.c), so a build may take a third of it
# (mkbootfs.py's budget, from ESP_END_MIB; the image's own check after its
# files are in: mkbootfs.py --esp-check). Sticks made before 2026-10-12
# have a 63 MiB ESP, too small for three of today's builds: `update`
# refuses to write such a stick (nothing on it changes) and says so;
# `make usb` remakes it.
ESP_END_MIB := 256
IMAGE_MIB := $(shell echo $$(( $(ESP_END_MIB) + 64 )))

# The data partition's filesystem (docs/M13.6-PLAN.md, "Ext by default"):
# ext (MBR type 0x83, made by jamext) unless asked for FAT32 (type 0x0C,
# mformat) with `make image DATA_FS=fat`, or JAMOS_DATA_FS=fat in the
# environment (the PC's builds forward JAMOS_*): the owner's choice, that
# every data volume Jam OS makes is ext and FAT only when asked. The test
# scripts that are about FAT make a FAT image of their own
# (tools/datafs.sh's data_fat_image). The ESP is FAT32 either way: the
# firmware reads only FAT.
# The ext volume is the same bytes every time (DATA_SEED: its uuid and its
# inodes' generations; DATA_TIME: every time written, SOURCE_DATE_EPOCH
# when that is set). DATA_DIR, if it is a directory, is copied into an ext
# /data before the first boot (jamext import): the owner's
# ~/.config/jamos/data holds the files a stick needs (etc/authorized_keys),
# since the Mac can't read an ext stick afterwards. `make usb` of an ext
# image makes the stick's volume again at the stick's size and copies
# DATA_DIR in once more (tools/stick-data.sh). The stamp rebuilds the image
# when any of it changes.
DATA_FS   ?= $(if $(JAMOS_DATA_FS),$(JAMOS_DATA_FS),ext)
ifneq ($(filter-out ext fat,$(DATA_FS)),)
$(error DATA_FS is ext or fat, not "$(DATA_FS)")
endif
DATA_DIR  ?= $(HOME)/.config/jamos/data
DATA_TIME ?= $(if $(SOURCE_DATE_EPOCH),$(SOURCE_DATE_EPOCH),1792000000)
DATA_SEED ?= 1
DATA_STAMP := $(IMAGE).data-fs

LIMINE  := third_party/limine
OVMF_DIR ?= $(shell brew --prefix qemu 2>/dev/null)/share/qemu

# Flag stamps. make doesn't see a compile command change, so a build kept
# from before a flag changed would keep its objects: stale ones that look
# up to date. Every group of objects that is built with one set of flags
# depends on a stamp file, $(FLAGDIR)/<name>, holding that set. A stamp is
# written again only when its text differs, so a changed flag builds
# exactly the group that has it, and a tree with nothing changed builds
# nothing (the stamp's rule runs on every make, FORCE, but touches the
# file only on a difference; it reads it with the shell's `read`, no
# program started: the build asks about a hundred stamps each time).
#   $(call FLAG_STAMP,<name>,<variable>)   the stamp <name>, holding the
#       variable's text (given as a name, as flags may hold commas);
#       the group's rules list $(FLAGDIR)/<name> among their prerequisites.
#   $(call PROG_FLAGS,<name>,<objects>)    the objects are compiled with
#       PROG_CFLAGS := $(PF_<name>), on top of the group's flags, and have
#       a stamp of it, pf-<name>.
# The compiler's name and version (the path of its libgcc) are in every
# compile stamp: a new compiler builds everything again. (The stamps'
# rules come before `all`'s: it is named the default goal here.)
.DEFAULT_GOAL := all
FLAGDIR := $(BUILD)/flags
TOOLID   = $(CC) $(LIBGCC)
define FLAG_STAMP
$(FLAGDIR)/$(1): FORCE
	@old=; [ -f $$@ ] && IFS= read -r old < $$@; \
	 [ "$$$$old" = '$$($(2))' ] || { mkdir -p $$(@D); printf '%s\n' '$$($(2))' > $$@; }
endef
define PROG_FLAGS
$(2): PROG_CFLAGS := $$(PF_$(1))
$(2): $(FLAGDIR)/pf-$(1)
$(call FLAG_STAMP,pf-$(1),PF_$(1))
endef

# -Wvla: no variable-length arrays (CODING-GUIDE.md "Recursion and the
# stack"). -Wframe-larger-than: a kernel thread has a 64 KiB stack; the
# largest frame today is sys_channel_call's, about 2.4 KiB (its message
# buffer), so 3 KiB lets nothing grow much past it unnoticed.
CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -Wframe-larger-than=3072 \
          -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-PIC -fno-pie -fno-omit-frame-pointer -fno-lto \
          -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
          -mno-red-zone -mcmodel=kernel \
          -Ikernel/include -Ithird_party/limine-protocol/include \
          -MMD -MP
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
           -T kernel/linker.ld

ifeq ($(KTESTS),0)
CFLAGS += -DJAM_NO_KTESTS
C_SRCS := $(shell find kernel -name '*.c' -not -path 'kernel/test/*')
else
C_SRCS := $(shell find kernel -name '*.c')
endif
S_SRCS := $(shell find kernel -name '*.S')
OBJS   := $(C_SRCS:%.c=$(BUILD)/%.o) $(S_SRCS:%.S=$(BUILD)/%.S.o)

# The panic screen's glyphs (<jam/panictext.h>): a crashed kernel runs no
# font code, so the font tool (tools/panicglyphs.c, in build/host/fontpreview,
# below) bakes them on the Mac into a table the kernel links.
PANIC_GLYPHS := $(BUILD)/gen/panicglyphs.c
OBJS         += $(PANIC_GLYPHS:.c=.o)

NET_STAMP := $(BUILD)/net-default.txt

# The kernel's flags: its compile (every object, ksyms and the panic glyphs
# too) and its link.
FLAGS_kernel      = $(TOOLID) $(CFLAGS) | $(ASFLAGS)
FLAGS_kernel-link = $(LD) $(LDFLAGS)
$(eval $(call FLAG_STAMP,kernel,FLAGS_kernel))
$(eval $(call FLAG_STAMP,kernel-link,FLAGS_kernel-link))

# Drivers (the rules are further down, after the user programs'): every
# drivers/<name>/*.c, and every test driver's drivers/test/<name>/*.c, is
# the program drv/<name> in bootfs (libos + user/lib/driver_user.c).
# drivers/lib/ is no driver: code several drivers link (DRV_LIB_<name>).
DRIVER_DIRS := $(sort $(filter-out drivers/lib,$(patsubst %/,%,$(dir $(wildcard drivers/*/*.c \
                                                                       drivers/test/*/*.c)))))
DRIVERS     := $(sort $(notdir $(DRIVER_DIRS)))
ifneq ($(words $(DRIVERS)),$(words $(DRIVER_DIRS)))
$(error two driver directories share a name: $(DRIVER_DIRS))
endif

.PHONY: all image run debug clean font usb flash syscalls idl wl keymap check compdb includes \
        ports FORCE

all: $(KERNEL) $(BOOTFS) $(SIGN_TOOL) $(MENU_TOOL) $(BUILD)/fontpreview.png

# kernel/main.c gets the network's default (JAMOS_NET_DEFAULT, above) and
# is built again whenever it changes: the stamp is rewritten only then.
# Every build says which default it has.
$(BUILD)/kernel/main.o: CFLAGS += -DJAMOS_NET_DEFAULT=$(NET_DEFAULT)
$(BUILD)/kernel/main.o: $(NET_STAMP)
$(NET_STAMP): FORCE
	@mkdir -p $(BUILD)
	@echo "network default: $(NET_SAY) ($(NET_FROM))"
	@echo "$(NET_TEXT)" > $@.new; if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

# System call glue. tools/gensyscalls.py turns abi/syscalls.def into the
# numbers, the kernel dispatch table and the user wrappers. The output is
# committed (so it can be read and grepped like any source); every build
# first checks that it matches the .def, so a table change can't be half
# applied. `make syscalls` regenerates it.
SYSCALL_GEN := kernel/include/jam/syscall_nums.h kernel/include/jam/syscall_impl.h \
               kernel/abi/syscall_table.c user/lib/syscalls.S user/include/jam_syscalls.h
SYSCALLS_OK := $(BUILD)/syscalls.ok

$(SYSCALLS_OK): abi/syscalls.def tools/gensyscalls.py kernel/include/jam/abi.h $(SYSCALL_GEN)
	@mkdir -p $(BUILD)
	python3 tools/gensyscalls.py check
	@touch $@

syscalls:
	python3 tools/gensyscalls.py gen

# Protocols. tools/genidl.py turns abi/idl/<name>.idl into the header
# drivers/include/idl/<name>.h (message structs, client stubs, server
# dispatch; all static inline over <jam/driver.h>). Committed and checked
# like the syscall glue; `make idl` regenerates.
IDL_SRCS := $(wildcard abi/idl/*.idl)
IDL_GEN  := drivers/include/idl/common.h $(IDL_SRCS:abi/idl/%.idl=drivers/include/idl/%.h)
IDL_OK   := $(BUILD)/idl.ok

$(IDL_OK): $(IDL_SRCS) abi/idl/ids.txt tools/genidl.py $(wildcard $(IDL_GEN))
	@mkdir -p $(BUILD)
	python3 tools/genidl.py check
	@touch $@

idl:
	python3 tools/genidl.py gen

# Wayland (docs/HISTORY.md, G1). tools/genwl.py turns upstream's interface files,
# third_party/wayland-protocols/<name>.xml (and Jam OS's own, abi/wayland/<name>.xml),
# into user/include/jwl/<protocol>.h
# (versions, opcodes, enums, typed stubs) and user/lib/jwl_<protocol>.c (the
# tables libjwl's codec reads); the protocol is the file's name with '_' for
# '-'. Committed and checked like the IDL; `make wl` regenerates.
WL_XML    := $(wildcard third_party/wayland-protocols/*.xml) $(wildcard abi/wayland/*.xml)
WL_PROTOS := $(subst -,_,$(basename $(notdir $(WL_XML))))
WL_TABLES := $(WL_PROTOS:%=user/lib/jwl_%.c)
WL_GEN    := $(WL_PROTOS:%=user/include/jwl/%.h) $(WL_TABLES)
WL_OK     := $(BUILD)/wl.ok

$(WL_OK): $(WL_XML) tools/genwl.py $(wildcard $(WL_GEN))
	@mkdir -p $(BUILD)
	python3 tools/genwl.py check
	@touch $@

wl:
	python3 tools/genwl.py gen

# Keyboard layouts. tools/genkeymap.py turns abi/keymap/ (keys.txt, and a
# file per layout) into user/lib/keymap_keys.c and user/lib/keymap_<layout>.c
# (<keymap.h>: the C tables and the XKB keymap). Committed like the IDL;
# `make check` fails if they are stale; `make keymap` makes them again.
keymap:
	python3 tools/genkeymap.py gen

$(OBJS): | $(SYSCALLS_OK) $(IDL_OK)

# Two-pass link: stage 1 has an empty symbol table; its function addresses
# become the table linked into the final kernel. .ksyms is the last section,
# so nothing moves between the passes (gensyms.py verify checks this).
$(BUILD)/ksyms_empty.c: tools/gensyms.py
	@mkdir -p $(BUILD)
	: > $(BUILD)/empty.nm
	python3 tools/gensyms.py gen $(BUILD)/empty.nm $@

$(BUILD)/jamos.stage1.elf: $(OBJS) $(BUILD)/ksyms_empty.o kernel/linker.ld $(FLAGDIR)/kernel-link
	$(LD) $(LDFLAGS) $(OBJS) $(BUILD)/ksyms_empty.o -o $@

$(BUILD)/ksyms.c: $(BUILD)/jamos.stage1.elf tools/gensyms.py
	$(CROSS)nm -n --defined-only $< > $(BUILD)/stage1.nm
	python3 tools/gensyms.py gen $(BUILD)/stage1.nm $@

# The linked kernel keeps its debug info in $(KERNEL_DEBUG) (for gdb);
# $(KERNEL), what the ESP, `update` and the stored kernel get, is it
# without (--strip-debug: about 5 of its 7 MB, which nothing reads at run
# time). Its .symtab stays (update-server.py reads jamos_version there);
# the panic screen's names are .ksyms, a loaded section, untouched.
$(KERNEL_DEBUG): $(OBJS) $(BUILD)/ksyms.o kernel/linker.ld $(FLAGDIR)/kernel-link
	$(LD) $(LDFLAGS) $(OBJS) $(BUILD)/ksyms.o -o $@
	$(CROSS)nm -n --defined-only $@ > $(BUILD)/final.nm
	python3 tools/gensyms.py verify $(BUILD)/stage1.nm $(BUILD)/final.nm

$(KERNEL): $(KERNEL_DEBUG)
	$(STRIP) --strip-debug $< -o $@

$(BUILD)/ksyms_empty.o $(BUILD)/ksyms.o: $(BUILD)/%.o: $(BUILD)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(PANIC_GLYPHS): $(FONT_TOOL)
	@mkdir -p $(dir $@)
	$(FONT_TOOL) --panic $@
$(PANIC_GLYPHS:.c=.o): $(PANIC_GLYPHS)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# (after the compile rules above: $< is each one's own first prerequisite)
$(OBJS) $(BUILD)/ksyms_empty.o $(BUILD)/ksyms.o: $(FLAGDIR)/kernel

# User programs (user/): the same cross compiler, freestanding, static at
# 0x400000 (user/linker.ld). SSE is fine here; the red zone too. User code
# sees only the kernel headers in UINC_HDRS, copied into their own include
# directory, so kernel internals are not on its include path at all.
USER_CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla \
               -ffreestanding -fno-stack-protector -fno-stack-check \
               -fno-PIC -fno-pie -fno-omit-frame-pointer -fno-lto \
               -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mcmodel=small \
               -Iuser/include -I$(BUILD)/uinc -Idrivers/include -MMD -MP
USER_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T user/linker.ld
LIBGCC      := $(shell $(CC) -print-libgcc-file-name)
# Their stamps: the compile flags (every object of user/, libos, libfun, the
# drivers' shared files, FatFs, lwIP and the rest below that is compiled
# with them; a group with more flags has a stamp of its own too) and the
# link's (every program, driver and bigprog).
FLAGS_user      = $(TOOLID) $(USER_CFLAGS)
FLAGS_user-link = $(LD) $(USER_LDFLAGS) $(LIBGCC)
$(eval $(call FLAG_STAMP,user,FLAGS_user))
$(eval $(call FLAG_STAMP,user-link,FLAGS_user-link))
UINC_HDRS   := abi.h bootfs.h startup.h status.h syscall_nums.h
UINC        := $(UINC_HDRS:%=$(BUILD)/uinc/jam/%)
UOBJ        := $(BUILD)/uobj
LIBOS_SRCS  := $(filter-out user/lib/crt0.S user/lib/driver_crt.c,\
                             $(wildcard user/lib/*.c user/lib/*.S))
LIBOS_OBJS  := $(LIBOS_SRCS:%=$(UOBJ)/%.o)
# dr_mp3 (third_party/dr_mp3, vendored unmodified) is compiled once, from
# user/lib/mp3port/dr_mp3_impl.c (its configuration; the directory also
# holds the string.h and stdlib.h it includes), into libos for <mp3.h>
# (user/lib/mp3.c). A program that never calls mp3_open doesn't link it.
DRMP3_OBJ   := $(UOBJ)/user/lib/mp3port/dr_mp3_impl.c.o
LIBOS_OBJS  += $(DRMP3_OBJ)
PF_drmp3 := -Iuser/lib/mp3port -Ithird_party/dr_mp3
$(eval $(call PROG_FLAGS,drmp3,$(DRMP3_OBJ)))
PF_mp3   := -Ithird_party/dr_mp3
$(eval $(call PROG_FLAGS,mp3,$(UOBJ)/user/lib/mp3.c.o))
# Monocypher (third_party/monocypher, vendored unmodified): Ed25519 as RFC
# 8032 has it, for signed updates (<update.h>, user/lib/updsig.c) and the
# SSH server's host key (init's ssh.c). Into libos; only a program that
# checks a signature or makes a key (init, utest) links it in.
# tools/jamos-sign, the Mac's side, is built from the same files.
MONO_DIR    := third_party/monocypher/src
MONO_SRCS   := $(MONO_DIR)/monocypher.c $(MONO_DIR)/optional/monocypher-ed25519.c
MONO_HDRS   := $(MONO_DIR)/monocypher.h $(MONO_DIR)/optional/monocypher-ed25519.h
MONO_OBJS   := $(MONO_SRCS:%=$(UOBJ)/%.o)
LIBOS_OBJS  += $(MONO_OBJS)
PF_mono := -I$(MONO_DIR) -I$(MONO_DIR)/optional
$(eval $(call PROG_FLAGS,mono,$(MONO_OBJS) $(UOBJ)/user/lib/updsig.c.o $(UOBJ)/user/lib/pkgfmt.c.o))
# The programs, by role: user/services/<name> (init, console, devmgr, ...),
# user/apps/<name> (the apps, on libfun) and user/tests/<name>. Every
# directory there with a .c file is the program bin/<name> in bootfs, except
# user/apps/fun, which is the apps library, and the POSIX tests
# (POSIX_TESTS: user/posix/posix.mk builds them with jamos-cc). A
# program's sources are its *.c and one level of subdirectories
# (user/services/shell/cmd/*.c).
LIBFUN_DIR  := user/apps/fun
POSIX_TESTS := user/tests/jittest
USER_DIRS   := $(filter-out $(LIBFUN_DIR) $(POSIX_TESTS),$(patsubst %/,%,$(sort $(dir \
                   $(wildcard user/services/*/*.c user/apps/*/*.c user/tests/*/*.c)))))
USER_PROGS  := $(notdir $(USER_DIRS))
ifneq ($(words $(sort $(USER_PROGS))),$(words $(USER_DIRS)))
$(error two user program directories share a name: $(USER_DIRS))
endif
prog_dir     = $(filter %/$(1),$(USER_DIRS))
prog_srcs    = $(wildcard $(call prog_dir,$(1))/*.c $(call prog_dir,$(1))/*/*.c)
prog_objs    = $(patsubst %,$(UOBJ)/%.o,$(call prog_srcs,$(1)))
USER_OBJS   := $(LIBOS_OBJS) $(patsubst %,$(UOBJ)/%.o,$(wildcard $(LIBFUN_DIR)/*.c)) \
               $(UOBJ)/user/lib/crt0.S.o $(UOBJ)/user/lib/driver_crt.c.o \
               $(foreach p,$(USER_PROGS),$(call prog_objs,$(p)))

# Copies named one by one (static pattern rules, not .SECONDARY pattern
# targets): GNU make 3.81, the Mac's, spins forever under -j when an
# order-only prerequisite is a .SECONDARY file a pattern rule would make
# and it doesn't exist yet (a new header or IDL file).
$(UINC): $(BUILD)/uinc/jam/%.h: kernel/include/jam/%.h
	@mkdir -p $(dir $@)
	cp $< $@

$(UOBJ)/%.c.o: %.c $(FLAGDIR)/user | $(UINC) $(SYSCALLS_OK) $(IDL_OK) $(WL_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(PROG_CFLAGS) -c $< -o $@

$(UOBJ)/%.S.o: %.S $(FLAGDIR)/user | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UOBJ)/libos.a: $(LIBOS_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# libjam.a: libos without its C library (heap, string functions, the printf
# family, environ and the call of main), for POSIX programs, which get
# those from musl (docs/M13-PLAN.md section 2; <os.h>). The same objects
# as libos's: libos is libjam plus LIBJAM_LIBC. `make check` links it
# against a stub C library (tools/libjam-stub.c, tools/checklibjam.py):
# every symbol libjam needs from a C library is one the stub has, and none
# that libjam defines is a C library's.
LIBJAM_LIBC := user/lib/heap.c user/lib/string.c user/lib/stdio.c user/lib/startmain.c
LIBJAM_OBJS := $(filter-out $(LIBJAM_LIBC:%=$(UOBJ)/%.o),$(LIBOS_OBJS))

$(UOBJ)/libjam.a: $(LIBJAM_OBJS)
	rm -f $@
	$(AR) rcs $@ $^
all: $(UOBJ)/libjam.a

LIBJAM_LINK := $(BUILD)/libjam-link
$(LIBJAM_LINK): $(UOBJ)/libjam.a tools/libjam-stub.c tools/checklibjam.py $(FLAGDIR)/user \
                $(FLAGDIR)/user-link
	$(CC) $(filter-out -I% -MMD -MP,$(USER_CFLAGS)) -c tools/libjam-stub.c -o $@-stub.o
	$(LD) $(USER_LDFLAGS) --fatal-warnings -e _start $@-stub.o \
	    --whole-archive $(UOBJ)/libjam.a --no-whole-archive $(LIBGCC) -o $@
	python3 tools/checklibjam.py $(CROSS)nm $(UOBJ)/libjam.a
check: $(LIBJAM_LINK)

# libfun (user/apps/fun, <fun.h>): the screen, drawing, text, keys and
# thread pool of the apps, which link it before libos.
# fbbench (user/tests) measures libfun's own drawing code too, and
# fractal (user/tests) draws with it on every CPU; the
# compositor (user/services) blends and draws with it; utest tests its
# window (utest/funwin.c) against the fake compositor, and wlapps shows
# the apps' windows composed headless on the borrowed screen; wltest's
# libfun modes (wltest/fun.c) test the mouse as an app sees it; the console
# draws its windows' text in libfun's smooth font (cellpaint.c).
FUN_PROGS   := $(notdir $(filter user/apps/%,$(USER_DIRS))) fbbench fractal compositor utest \
               wlapps wltest console
LIBFUN_OBJS := $(patsubst %,$(UOBJ)/%.o,$(wildcard $(LIBFUN_DIR)/*.c))

$(UOBJ)/libfun.a: $(LIBFUN_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# libfun's smooth text (<fun.h>, font.c): stb_truetype
# (third_party/stb_truetype, vendored unmodified, compiled in ttf.c) and
# the Inter and JetBrains Mono faces (third_party/inter,
# third_party/jetbrains-mono), which fontdata.c's .incbin links in as they
# are.
# build/host/fontpreview (below) builds the same files for the Mac.
FONT_FACES  := third_party/inter/Inter-Regular.ttf third_party/inter/Inter-Medium.ttf \
               third_party/jetbrains-mono/JetBrainsMono-Regular.ttf \
               third_party/jetbrains-mono/JetBrainsMono-Bold.ttf
FONT_SRCS   := $(addprefix $(LIBFUN_DIR)/,font.c fontdraw.c fontdata.c ttf.c utf8.c alpha.c)
FONT_STB_OBJS := $(UOBJ)/$(LIBFUN_DIR)/ttf.c.o $(UOBJ)/$(LIBFUN_DIR)/font.c.o
PF_stb := -Ithird_party/stb_truetype
$(eval $(call PROG_FLAGS,stb,$(FONT_STB_OBJS)))
$(UOBJ)/$(LIBFUN_DIR)/fontdata.c.o: $(FONT_FACES)

# FatFs (third_party/fatfs, vendored unmodified), built into the fat
# service only. Its configuration is Jam OS's
# (user/services/fat/ffport/ffconf.h), but ff.h includes "ffconf.h" from
# its own directory first, where the vendored default sits; so the sources
# are copied into $(BUILD)/fatfs and compiled there, with ffport on the
# include path (ffconf.h, and the <string.h> ff.c wants).
FATFS_SRC   := third_party/fatfs/source
FATFS_PORT  := user/services/fat/ffport
FATFS_STAGE := $(BUILD)/fatfs
FATFS_HDRS  := $(FATFS_STAGE)/ff.h $(FATFS_STAGE)/diskio.h
FATFS_OBJS  := $(FATFS_STAGE)/ff.o $(FATFS_STAGE)/ffunicode.o
FATFS_INC   := -I$(FATFS_STAGE) -I$(FATFS_PORT)

$(FATFS_HDRS): $(FATFS_STAGE)/%.h: $(FATFS_SRC)/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(FATFS_OBJS:.o=.c): $(FATFS_STAGE)/%.c: $(FATFS_SRC)/%.c
	@mkdir -p $(dir $@)
	cp $< $@

$(eval $(call FLAG_STAMP,fatfs,FATFS_INC))
$(FATFS_STAGE)/%.o: $(FATFS_STAGE)/%.c $(FATFS_HDRS) $(FLAGDIR)/user $(FLAGDIR)/fatfs \
                    | $(UINC) $(SYSCALLS_OK)
	$(CC) $(USER_CFLAGS) $(FATFS_INC) -c $< -o $@

# lwIP (third_party/lwip, vendored unmodified: only the files netstack
# needs), built into bin/netstack and, for its in-process tests, utest.
# Its configuration is user/services/netstack/port: lwipopts.h, arch/cc.h
# and the C library headers lwIP includes (libos has what they declare).
LWIP_DIR    := third_party/lwip/src
LWIP_PORT   := user/services/netstack/port
LWIP_INC    := -I$(LWIP_DIR)/include -I$(LWIP_PORT)
LWIP_SRCS   := $(wildcard $(LWIP_DIR)/core/*.c $(LWIP_DIR)/core/ipv4/*.c $(LWIP_DIR)/netif/*.c)
LWIP_OBJS   := $(LWIP_SRCS:%.c=$(BUILD)/lwip/%.o)

$(eval $(call FLAG_STAMP,lwip,LWIP_INC))
$(LWIP_OBJS): $(BUILD)/lwip/%.o: %.c $(FLAGDIR)/user $(FLAGDIR)/lwip | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(LWIP_INC) -c $< -o $@

# The compositor's cursor set (docs/design/cursors.svg): tools/cursorgen.c
# draws it on the Mac into build/gen/cursorset.c, which the compositor
# links (and utest, which compares what the compositor draws with it), and
# build/cursors.png, the set four times as big, to look at.
CURSOR_GEN := build/host/cursorgen
CURSOR_SET := $(BUILD)/gen/cursorset.c
CURSOR_OBJ := $(UOBJ)/$(CURSOR_SET).o
$(CURSOR_GEN): tools/cursorgen.c tools/png.c tools/fontpreview.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O2 -Wall -Wextra -Werror -o $@ tools/cursorgen.c tools/png.c -lm
$(CURSOR_SET): $(CURSOR_GEN)
	@mkdir -p $(dir $@)
	$(CURSOR_GEN) $@ $(BUILD)/cursors.png
PF_cursor := -iquote user/services/compositor -I$(LIBFUN_DIR)
$(eval $(call PROG_FLAGS,cursor,$(CURSOR_OBJ)))

# What a program links, includes and needs made first beyond its own
# directory and libos.
EXTRA_OBJS_compositor := $(CURSOR_OBJ)
EXTRA_OBJS_netstack   := $(LWIP_OBJS)
EXTRA_CFLAGS_netstack := $(LWIP_INC)
# fat compiles ff.c itself, inside user/services/fat/ffx.c (its additions).
EXTRA_OBJS_fat   := $(FATFS_STAGE)/ffunicode.o
EXTRA_CFLAGS_fat := $(FATFS_INC)
EXTRA_DEPS_fat   := $(FATFS_HDRS) $(FATFS_STAGE)/ff.c
# The boot splash: pl_mpeg (third_party/pl_mpeg, vendored unmodified, one
# header) and the <string.h>/<stdlib.h> it includes, which are libos's
# (user/apps/splash/port).
EXTRA_CFLAGS_splash := -Ithird_party/pl_mpeg -Iuser/apps/splash/port
# jamjar's album covers, decoded by its helper bin/jamcover: stb_image
# (third_party/stb_image, vendored unmodified, PNG and JPEG only) and the
# <stdlib.h>/<string.h> it includes, which are libos's
# (user/apps/jamcover/port).
EXTRA_CFLAGS_jamcover := -Ithird_party/stb_image -Iuser/apps/jamcover/port
# init makes the SSH server's host key with Monocypher (init/ssh.c).
EXTRA_CFLAGS_init := -I$(MONO_DIR) -I$(MONO_DIR)/optional
# utest tests the music player's folder walk and spectrum (utest/music.c):
# the player's own objects, linked in, and its header; and the RTL8125
# probe's transmit-register guard (utest/netframe.c: drivers/rtl8125/notx.h);
# and netstack's core with lwIP, driven in-process over a fake edge
# (utest/netstack.c: its stack.h and ctl.h, no lwIP header; nettcp.c and
# tcpabuse.c: its TCP connections, tcp.c, the same way), the DHCP
# client's and the resolver's cores (utest/dhcp*.c, dns*.c), and the
# network drivers' netdev server over a fake card (utest/netsrv.c:
# drivers/lib/netserver.c), bin/dns's sockets and askers over a fake
# netstack (utest/dnsd.c), bin/sntp's request and checks (utest/sntp.c),
# and the RTL8125 driver's guard over a fake chip (utest/rtlguard.c:
# guard.c and the files it calls, built as user code), and FatFs's structs
# as fat's build sees them (utest/fatlayout.c: "ff.h" with fat's
# ffconf.h, on the quote path only, so ffport's <string.h> stays fat's),
# and the compositor's boxes and regions (utest/comp_region.c: region.c
# and its comp.h), and its window manager on its scene (utest/compwm.c:
# wm.h, no protocol in those files; the test plays the seat), with
# title.c's title bar boxes and the masks (mask.c) and libfun they draw
# with, and the desktop's logic around them (utest/compdesk*.c: screens,
# the animations' clock, the strip's layout, the menus, popovers and
# notifications, desk.c; none of their drawing), and where a positioner
# puts a popup (utest/compplace.c: xdgplace.c), and the console's window
# mode's pure parts (utest/conwin.c, conpaint.c, consel.c:
# wlinput.c, cellpaint.c, view.c, select.c and their console.h), and the
# shell's bracketed paste (utest/shpaste.c: sh_paste.c and sh_paste.h), and
# how a /data program is checked (utest/shimage.c: libos's dataprog.c, which
# the shell and init's /svc/exec share), and /svc/exec's client
# (utest/execsvc.c: exectest's client.c), and init's approvals of a
# package's programs (utest/pkgctl.c: init's pkgapprove.c over a RAM
# filesystem, manifests signed with the vectors' public test key by
# Monocypher), and bin/pkg's transaction code (utest/pkgplan.c, pkgtxn.c,
# pkgcut.c: user/services/pkg's fsx, db, plan, unpack, journal and txn over
# the same RAM filesystem, cut at every change).
NETSTACK_CORE      := $(patsubst %,$(UOBJ)/user/services/netstack/%.o,stack.c ctl.c tcp.c \
                        port/sys_arch.c)
EXTRA_OBJS_utest   := $(UOBJ)/user/services/music/spectrum.c.o $(UOBJ)/user/services/music/tracks.c.o \
                      $(NETSTACK_CORE) $(LWIP_OBJS) \
                      $(UOBJ)/user/services/dhcp/msg.c.o $(UOBJ)/user/services/dhcp/client.c.o \
                      $(UOBJ)/user/services/dns/msg.c.o $(UOBJ)/user/services/dns/cache.c.o \
                      $(UOBJ)/user/services/dns/resolver.c.o $(UOBJ)/drivers/lib/netserver.c.o \
                      $(UOBJ)/user/services/dns/socks.c.o $(UOBJ)/user/services/dns/askers.c.o \
                      $(UOBJ)/user/services/sntp/ntp.c.o \
                      $(patsubst %,$(UOBJ)/drivers/rtl8125/%.c.o,guard regs chip tx) \
                      $(patsubst %,$(UOBJ)/user/services/compositor/%.c.o,region scene wm wmtile wmkeys \
                        wmgrab wmsave deco title mask screens anim strip menus popover notify desk \
                        cursors xdgplace) \
                      $(CURSOR_OBJ) $(patsubst %,$(UOBJ)/user/services/console/%.c.o,wlinput \
                        cellpaint view select) $(UOBJ)/user/services/shell/sh_paste.c.o \
                      $(UOBJ)/user/tests/exectest/client.c.o $(UOBJ)/libfun.a \
                      $(UOBJ)/user/services/init/pkgapprove.c.o \
                      $(patsubst %,$(UOBJ)/user/services/pkg/%.c.o,fsx db plan unpack journal txn)
EXTRA_CFLAGS_utest := -iquote user/services/music -iquote drivers/rtl8125 \
                      -iquote user/services/compositor -iquote user/services/console \
                      -iquote user/services/shell -iquote user/tests/exectest \
                      -iquote user/services/netstack -iquote user/services/dhcp \
                      -iquote user/services/dns -iquote user/services/sntp \
                      -iquote $(FATFS_STAGE) -iquote $(FATFS_PORT) -Iuser/apps/fun \
                      -iquote user/services/init -iquote user/services/pkg \
                      -I$(MONO_DIR) -I$(MONO_DIR)/optional
EXTRA_DEPS_utest   := $(FATFS_HDRS)
# The ports formats' vectors (ports/vectors/, tools/pkgvec2c.py), linked in
# for utest/pkgfmt.c: the same bytes `make check` reads on the Mac.
PKGVEC_C   := $(BUILD)/gen/pkgvectors.c
PKGVEC_OBJ := $(UOBJ)/$(PKGVEC_C).o
$(PKGVEC_C): tools/pkgvec2c.py $(wildcard ports/vectors/*)
	@mkdir -p $(dir $@)
	python3 tools/pkgvec2c.py ports/vectors $@
PF_pkgvec := -iquote user/tests/utest
$(eval $(call PROG_FLAGS,pkgvec,$(PKGVEC_OBJ)))
EXTRA_OBJS_utest   += $(PKGVEC_OBJ)
# execwide is exectest's body with a wider list (user/tests/exectest/exectest.h).
EXTRA_OBJS_execwide   := $(UOBJ)/user/tests/exectest/probe.c.o \
                         $(UOBJ)/user/tests/exectest/client.c.o
EXTRA_CFLAGS_execwide := -iquote user/tests/exectest
# bigprog's table (user/tests/bigprog/table.S): 16 pages in bootfs's copy.
EXTRA_OBJS_bigprog    := $(UOBJ)/user/tests/bigprog/table.S.o

# The shell's `disk check` runs the ext library's checker: the objects bin/extfs
# builds from user/services/extfs/ext (the same files, one build).
EXTRA_OBJS_shell   := $(patsubst %,$(UOBJ)/%.o,$(wildcard user/services/extfs/ext/*.c))
EXTRA_CFLAGS_shell := -Iuser/services/extfs/ext

# $(BUILD)/user/<prog> keeps its debug info (for gdb); bootfs gets a copy
# without it ($(BUILD)/user/<prog>.bootfs), symbols kept for backtraces.
# PROG_CFLAGS: the program's own directory on the "..." include path (so
# "sh.h" works from shell/cmd/), for an app the apps library's <fun.h>,
# and the program's EXTRA_CFLAGS.
define USER_PROG
PF_$(1) := -iquote $(call prog_dir,$(1)) $(if $(filter $(1),$(FUN_PROGS)),-I$(LIBFUN_DIR)) \
           $(EXTRA_CFLAGS_$(1))
$(call PROG_FLAGS,$(1),$(call prog_objs,$(1)))
$(call prog_objs,$(1)): | $(EXTRA_DEPS_$(1))

$(BUILD)/user/$(1): $(UOBJ)/user/lib/crt0.S.o $(call prog_objs,$(1)) $(EXTRA_OBJS_$(1)) \
                    $(if $(filter $(1),$(FUN_PROGS)),$(UOBJ)/libfun.a) $(UOBJ)/libos.a \
                    user/linker.ld $(FLAGDIR)/user-link
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) $(UOBJ)/user/lib/crt0.S.o $(call prog_objs,$(1)) $(EXTRA_OBJS_$(1)) \
	    $(if $(filter $(1),$(FUN_PROGS)),$(UOBJ)/libfun.a) $(UOBJ)/libos.a $(LIBGCC) -o $$@

$(BUILD)/user/$(1).bootfs: $(BUILD)/user/$(1)
	$(STRIP) --strip-debug $$< -o $$@
endef
$(foreach p,$(USER_PROGS),$(eval $(call USER_PROG,$(p))))

# The big program (tools/bigprog-test.sh; docs/CR-PLAN.md section 6, CP1):
# bin/bigprog linked with a table of BIGPROG_PAGES pages (76,800: 300 MiB,
# a program as big as Chromium's) instead of 16. Generated only when a
# test asks for it (`make build/bigprog/bigprog`), stripped of its debug
# information, never in bootfs or the tree.
BIGPROG_PAGES ?= 76800
$(eval $(call FLAG_STAMP,bigprog,BIGPROG_PAGES))
$(BUILD)/bigprog/table.o: user/tests/bigprog/table.S $(FLAGDIR)/user $(FLAGDIR)/bigprog \
                          | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -DBIG_PAGES=$(BIGPROG_PAGES) -c $< -o $@
$(BUILD)/bigprog/bigprog: $(UOBJ)/user/lib/crt0.S.o $(call prog_objs,bigprog) \
                          $(BUILD)/bigprog/table.o $(UOBJ)/libos.a user/linker.ld \
                          $(FLAGDIR)/user-link
	$(LD) $(USER_LDFLAGS) -s $(UOBJ)/user/lib/crt0.S.o $(call prog_objs,bigprog) \
	    $(BUILD)/bigprog/table.o $(UOBJ)/libos.a $(LIBGCC) -o $@

# The same table in a POSIX program (tools/bigpkg.c, built by
# jamos-cc with the list `posix` and `mount /jam rw`): tools/pkg-test.sh's big
# package, installed by `pkg` onto /jam and run from there. Generated when a
# test asks for it (`make build/bigpkg/bigpkg`), never in bootfs or the tree.
$(BUILD)/bigpkg/bigpkg: tools/bigpkg.c $(BUILD)/bigprog/table.o tools/jamos-cc \
                        $(SYSROOT_LIBS) $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	printf 'posix\nmount /jam rw\nmount /pkg r\nsvc exec\n' > $(BUILD)/bigpkg/list
	JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc -O2 -Wall -Werror \
	    --jamos-wants=$(BUILD)/bigpkg/list -o $@ $< $(BUILD)/bigprog/table.o

# ---- drivers (ARCHITECTURE.md "The migration rule") -------------------------
# A driver sees nothing but <jam/driver.h> (+ <jam/abi.h>, <jam/status.h>,
# <jam/task.h>, libos's cooperative tasks, <jam/netframe.h>, pure
# functions over a network frame's bytes, and <jam/netdev.h>, the netdev
# rings' layout and index code; and <jam/netserver.h>, the netdev server
# whose code, drivers/lib/netserver.c, a network driver links into its own
# object: DRV_LIB_<name> below; and <jam/sha256.h>, drivers/lib/sha256.c,
# the same way; and <jam/parttable.h>, the partition table library,
# drivers/lib/parttable*.c, for disk drivers), the generated <idl/*.h>
# and the compiler's freestanding headers
# (stdint/stddef/stdbool/stdarg): -nostdinc drops every other include path,
# and DRV_INC holds copies of just those files. -fno-builtin: no call is
# assumed to be a C library function. tools/checkdriver.py then fails the
# build if a driver object uses any symbol those headers don't declare (see
# its header for the exact list), so declaring a libos or kernel function
# yourself doesn't work either. `make check` proves the check rejects what
# it should. A driver's files are compiled with the user flags, linked into
# one object (ld -r), checked, and linked with crt0, driver_crt (main ->
# driver_main) and libos into drv/<name>.
DRV_SURFACE := drivers/include/jam/driver.h drivers/include/jam/task.h kernel/include/jam/abi.h \
               kernel/include/jam/status.h
DRV_INC     := $(BUILD)/driver-include
DRV_HDRS    := $(DRV_INC)/jam/driver.h $(DRV_INC)/jam/task.h $(DRV_INC)/jam/abi.h \
               $(DRV_INC)/jam/status.h $(DRV_INC)/jam/netframe.h $(DRV_INC)/jam/netdev.h \
               $(DRV_INC)/jam/netserver.h $(DRV_INC)/jam/sha256.h $(DRV_INC)/jam/parttable.h \
               $(IDL_GEN:drivers/include/%=$(DRV_INC)/%)
DRV_ISOLATE := -nostdinc -isystem $(shell $(CC) -print-file-name=include) -I$(DRV_INC) -fno-builtin
DRV_CFLAGS  := $(filter-out -I%,$(USER_CFLAGS)) $(DRV_ISOLATE)
# DRV_LIB_<name>: the drivers/lib/ files driver <name> links into its own
# object (checked by checkdriver.py with the rest of it).
DRV_LIB_rtl8125 := netserver
DRV_LIB_e1000e  := netserver
DRV_LIB_nvme    := sha256 parttable parttable_gpt parttable_make
DRV_OBJS     = $(patsubst %.c,$(BUILD)/udrv/%.o,$(wildcard $(filter %/$(1),$(DRIVER_DIRS))/*.c)) \
               $(patsubst %,$(BUILD)/udrv/drivers/lib/%.o,$(DRV_LIB_$(1)))

$(DRV_INC)/jam/driver.h $(DRV_INC)/jam/task.h $(DRV_INC)/jam/netframe.h $(DRV_INC)/jam/netdev.h \
        $(DRV_INC)/jam/netserver.h $(DRV_INC)/jam/sha256.h $(DRV_INC)/jam/parttable.h: \
        $(DRV_INC)/jam/%.h: drivers/include/jam/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(DRV_INC)/jam/abi.h $(DRV_INC)/jam/status.h: $(DRV_INC)/jam/%.h: kernel/include/jam/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(filter $(DRV_INC)/idl/%,$(DRV_HDRS)): $(DRV_INC)/idl/%.h: drivers/include/idl/%.h | $(IDL_OK)
	@mkdir -p $(dir $@)
	cp $< $@

FLAGS_drv = $(TOOLID) $(DRV_CFLAGS)
$(eval $(call FLAG_STAMP,drv,FLAGS_drv))
$(BUILD)/udrv/drivers/%.o: drivers/%.c $(FLAGDIR)/drv | $(DRV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(DRV_CFLAGS) -c $< -o $@

define DRIVER
$(BUILD)/udrv/$(1).o: $(call DRV_OBJS,$(1)) tools/checkdriver.py $(DRV_SURFACE)
	$(LD) -r -o $$@.r $(call DRV_OBJS,$(1))
	python3 tools/checkdriver.py $(CROSS)nm $(1) $$@.r -- $(DRV_SURFACE)
	mv $$@.r $$@

$(BUILD)/drv/$(1): $(UOBJ)/user/lib/crt0.S.o $(UOBJ)/user/lib/driver_crt.c.o $(BUILD)/udrv/$(1).o \
                   $(UOBJ)/libos.a user/linker.ld $(FLAGDIR)/user-link
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) $(UOBJ)/user/lib/crt0.S.o $(UOBJ)/user/lib/driver_crt.c.o \
	    $(BUILD)/udrv/$(1).o $(UOBJ)/libos.a $(LIBGCC) -o $$@

$(BUILD)/drv/$(1).bootfs: $(BUILD)/drv/$(1)
	$(STRIP) --strip-debug $$< -o $$@
endef
$(foreach d,$(DRIVERS),$(eval $(call DRIVER,$(d))))

# The partition table library (<jam/parttable.h>), as a disk driver links
# it (DRV_LIB_<name> := parttable parttable_gpt parttable_make: the cut-short
# prepare rule in parttable.c uses the maker's bytes), checked on its own by
# `make check` so it keeps to the driver rules before any driver uses it.
PT_LIB     := parttable parttable_gpt parttable_make
PT_LIB_OBJ := $(BUILD)/udrv/parttable-lib.o
$(PT_LIB_OBJ): $(patsubst %,$(BUILD)/udrv/drivers/lib/%.o,$(PT_LIB)) tools/checkdriver.py \
               $(DRV_SURFACE)
	$(LD) -r -o $@.r $(patsubst %,$(BUILD)/udrv/drivers/lib/%.o,$(PT_LIB))
	python3 tools/checkdriver.py $(CROSS)nm parttable $@.r -- $(DRV_SURFACE)
	mv $@.r $@

# `make check`: the generated code is current (and the IDL and Wayland
# generators pass their own tests: genidl's selftest, tools/genwl-tests/), the driver check still
# rejects what it must (tools/checkdriver-tests/: a kernel include, a
# kmalloc call, a call into another driver, ...) and accepts a clean one,
# the Markdown docs still match the tree (tools/checkdocs.py), the
# RTL8125 listen-only probe has no way to transmit (tools/checknotx.sh),
# boot/init.cfg's parts add up to its whole run (tools/init-test.sh --check),
# the NVMe driver writes only a drive it found to be Jam OS's (tools/checknvme.py), and the
# partition table library keeps to the driver rules and passes
# tools/gptcheck.c (its cases, mutations and a short fuzz pass), and the
# ports' recipes pass their lint and the ports builder its self-test
# (tools/ports.py: offline, fake ports built with the real sysroot), and the
# ext library builds for Jam OS and passes tools/extcheck.c (Linux's
# fixtures, broken checksums, the feature table, a fuzz pass), the host tool
# jamext builds and round-trips a directory, files, links and a volume at
# an offset, each image checked by extcheck (tools/jamext-test.sh), the
# stick's images (ext /data by default, FAT when asked, the flashing tool's
# data step: tools/image-test.sh), and the syscall scan (tools/cr/syscall-scan.py) passes a clean program and
# catches a `syscall` of a program's own.
check: all
	python3 tools/gensyscalls.py check
	python3 tools/genidl.py check
	CC="$(CC)" python3 tools/genidl.py selftest
	python3 tools/genwl.py check
	HOSTCC="$(HOSTCC)" python3 tools/genwl.py selftest
	python3 tools/genkeymap.py check
	CC="$(CC)" NM="$(CROSS)nm" CFLAGS="$(DRV_CFLAGS)" SURFACE="$(DRV_SURFACE)" \
	    OUT="$(BUILD)/checkdriver-tests" sh tools/checkdriver-selftest.sh
	python3 tools/checkdocs.py
	python3 tools/sortincludes.py
	sh tools/checkaudio.sh
	sh tools/checknotx.sh
	sh tools/init-test.sh --check
	python3 tools/checknvme.py
	python3 tools/netpeer.py --selftest
	python3 tools/pcap-vlan-check.py --selftest
	python3 tools/serial-feed.py --selftest
	python3 tools/checkwants.py --selftest
	python3 tools/mkbootfs.py --selftest
	python3 tools/ports.py lint
	$(MAKE) -s $(PAGETABLE_TOOLS)
	python3 tools/ports.py selftest
	python3 tools/ports.py vectors
	python3 tools/ports-server.py --self-test
	CROSS=$(CROSS) python3 tools/cr/syscall-scan.py --selftest
	@for d in $$(cd third_party && ls -d */ | tr -d /); do \
	    case " $(THIRD_PARTY_NOTICED) " in *" $$d "*) ;; \
	    *) echo "licences: third_party/$$d has no notice in the boot image (LICENCES in the Makefile, or a port's recipe)"; exit 1;; esac; \
	done
	python3 tools/fatcheck.py --selftest
	$(SIGN_TOOL) self-test
	$(MENU_TOOL) boot/limine.conf $(STICK_FILES)
	$(MAKE) -s $(FONT_CHECK)
	$(FONT_CHECK) --check
	$(MAKE) -s $(PT_LIB_OBJ) $(GPT_CHECK)
	python3 tools/mkgpt.py --cases $(BUILD)/gptcheck
	$(GPT_CHECK) $(BUILD)/gptcheck
	$(MAKE) -s $(ROOTPORT_CHECK)
	$(ROOTPORT_CHECK)
	$(MAKE) -s $(AVL_CHECK)
	$(AVL_CHECK)
	$(MAKE) -s $(ITREE_CHECK)
	$(ITREE_CHECK)
	$(MAKE) -s $(EXT_CHECK) $(EXT_FIXTURES) $(EXT_CROSS)
	$(EXT_CHECK) tools/extfix $(BUILD)/extfix
	$(MAKE) -s $(JAMEXT)
	sh tools/jamext-test.sh $(JAMEXT) $(EXT_CHECK) $(BUILD)/jamext-test $(BUILD)/extfix
	sh tools/image-test.sh $(BUILD)/image-test
	$(MAKE) -s $(PKG_CHECK)
	python3 tools/ports.py pkgfuzz $(BUILD)/pkgfuzz
	$(PKG_CHECK) ports/vectors $(BUILD)/pkgfuzz

# The boot splash's video: boot/splash.mpg, committed. It is made from the
# owner's animation (tools/mksplash.sh), which lives outside the repository
# in SPLASH_SRC; it is made again only when both of those files are there
# and one is newer than it (without them the rule has no prerequisites: the
# committed file is used as it is).
SPLASH_SRC   ?= $(HOME)/Movies/motion-graphics/jamos-boot
SPLASH_PAIR  := $(SPLASH_SRC)/jamos-boot.mov $(SPLASH_SRC)/jamos-boot.wav
SPLASH_FILES := $(if $(filter 2,$(words $(wildcard $(SPLASH_PAIR)))),$(SPLASH_PAIR))

boot/splash.mpg: $(SPLASH_FILES)
	$(if $(SPLASH_FILES),tools/mksplash.sh $(SPLASH_SRC) $@)

# build.txt in bootfs: the git commit the build was made from ("git
# 2079f35", "-dirty" when tracked files had changes), which `version` and
# `update` show and tools/update-server.py puts in the manifest, then the
# network's default mode ("net vlan21", "net untagged"), which init's
# update check and tools/flash-usb.sh read, then the ABI stamp ("abi
# 23f43a3c93e81a21": tools/ports.py abi, the hash of the system calls and
# protocols a ported program carries the call layer for: bin/pkg refuses a
# package of another stamp, `update` says when a new build has another,
# docs/M13.7-PLAN.md section 11). Written again only when it changes, so an
# unchanged tree packs nothing anew.
BUILD_INFO := $(BUILD)/build.txt
$(BUILD_INFO): $(NET_STAMP) FORCE
	@mkdir -p $(BUILD)
	@h=$$(git rev-parse --short=7 HEAD 2>/dev/null) || h=0000000; \
	 [ $$h = 0000000 ] || git diff --quiet HEAD -- 2>/dev/null || h=$$h-dirty; \
	 a=$$(python3 tools/ports.py abi) || exit 1; \
	 printf 'git %s\n%s\nabi %s\n' "$$h" "$(NET_TEXT)" "$$a" > $@.new; \
	 if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi
FORCE:

# update.pub in bootfs: the public half of the owner's update key
# (`build/host/jamos-sign keygen` makes it, once, in ~/.config/jamos;
# UPDATE_KEY names another file), which init checks every fetched build's
# signature with (<update.h>). Without the file the build has no key, and
# init refuses every `update`. Copied again only when it changes (or comes,
# or goes), so an unchanged key packs nothing anew.
UPDATE_KEY ?= $(HOME)/.config/jamos/update.pub
UPDATE_PUB := $(BUILD)/update.pub
$(UPDATE_PUB): FORCE
	@mkdir -p $(BUILD)
	@if [ -f "$(UPDATE_KEY)" ]; then cp "$(UPDATE_KEY)" $@.new; else : > $@.new; fi; \
	 if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

# ports.pub in bootfs: the public half of the owner's ports key, which
# signs every package, index and vulnerability list `make ports` makes
# (docs/M13.7-PLAN.md section 7). `build/host/jamos-sign keygen --ports`
# makes the pair, once, in ~/.config/jamos (the secret, ports.key, stays
# there: never in the tree); PORTS_KEY names another public file, and
# `PORTS_KEY=` builds without one. A build without it (a public build, or
# a machine without the key) takes no package: pkg refuses every
# repository. Copied again only when it changes, comes or goes.
PORTS_KEY ?= $(HOME)/.config/jamos/ports.pub
PORTS_PUB := $(BUILD)/ports.pub
$(PORTS_PUB): FORCE
	@mkdir -p $(BUILD)
	@if [ -f "$(PORTS_KEY)" ]; then cp "$(PORTS_KEY)" $@.new; else : > $@.new; fi; \
	 if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

# bootfs: the files init and the tests need before USB and FAT32 work,
# loaded by Limine as a module (boot/limine.conf: module_path).
BOOTFS_FILES := $(foreach p,$(USER_PROGS),bin/$(p)=$(BUILD)/user/$(p).bootfs) \
                $(foreach d,$(DRIVERS),drv/$(d)=$(BUILD)/drv/$(d).bootfs) init.cfg=boot/init.cfg \
                splash.mpg=boot/splash.mpg build.txt=$(BUILD_INFO) \
                $(if $(wildcard $(UPDATE_KEY)),update.pub=$(UPDATE_PUB)) \
                $(if $(wildcard $(PORTS_KEY)),ports.pub=$(PORTS_PUB))

# Every program's list (<wants.h>) is checked first: what the build
# approves for each boot-image program (tools/checkwants.py). The boot
# image may take what `update` takes (UPDATE_FILE_MAX) and a third of the
# ESP beside the kernel, whichever is less (tools/mkbootfs.py: `update`
# keeps three builds on the ESP), so it is packed again when the kernel
# changes.
$(BOOTFS): $(USER_PROGS:%=$(BUILD)/user/%.bootfs) $(DRIVERS:%=$(BUILD)/drv/%.bootfs) boot/init.cfg \
           boot/splash.mpg $(BUILD_INFO) $(UPDATE_PUB) $(PORTS_PUB) tools/mkbootfs.py \
           tools/checkwants.py user/include/os.h $(KERNEL) $(LIMINE)/BOOTX64.EFI boot/limine.conf \
           user/include/update.h user/services/init/espwrite.c
	python3 tools/checkwants.py $(foreach p,$(USER_PROGS),$(call prog_dir,$(p))=$(BUILD)/user/$(p).bootfs)
	python3 tools/mkbootfs.py --esp-mib $$(( $(ESP_END_MIB) - 1 )) --kernel $(KERNEL) \
	    --esp-file $(LIMINE)/BOOTX64.EFI --esp-file boot/limine.conf $@ $(BOOTFS_FILES) @$(PORTS_LIST)

image: $(IMAGE)

$(DATA_STAMP): FORCE
	@mkdir -p $(BUILD)
	@{ echo "$(DATA_FS) $(DATA_TIME) $(DATA_SEED)"; \
	   if [ $(DATA_FS) = ext ] && [ -d "$(DATA_DIR)" ]; then \
	       (cd "$(DATA_DIR)" && find . -type f | LC_ALL=C sort | while read -r f; do \
	           echo "$$f"; cksum < "$$f"; done); \
	   fi; } > $@.new; \
	 if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi

$(IMAGE): $(KERNEL) $(BOOTFS) boot/limine.conf tools/mkimage.py tools/fat-label.py tools/mkbootfs.py \
          $(DATA_STAMP) $(if $(filter ext,$(DATA_FS)),$(JAMEXT))
	python3 tools/mkimage.py $@ $(ESP_END_MIB) $(IMAGE_MIB) --data $(DATA_FS)
	mformat -i $@@@1M -T $$(( ($(ESP_END_MIB) - 1) * 2048 )) -F -v JAMOS ::
	python3 tools/fat-label.py $@ 1 --fix
ifeq ($(DATA_FS),ext)
	$(JAMEXT) -q --seed $(DATA_SEED) --time $(DATA_TIME) --part 2 mkfs --label JAMOS-DATA $@ 2T \
	    || { rm -f $@; exit 1; }
	@if [ -d "$(DATA_DIR)" ]; then \
	    echo "data: $(DATA_DIR) goes into /data"; \
	    $(JAMEXT) -q --seed $(DATA_SEED) --time $(DATA_TIME) --part 2 import $@ "$(DATA_DIR)" \
	        || { rm -f $@; exit 1; }; \
	fi
	$(JAMEXT) -q --part 2 check $@ > /dev/null || { rm -f $@; exit 1; }
else
	mformat -i $@@@$(ESP_END_MIB)M -T $$(( ($(IMAGE_MIB) - $(ESP_END_MIB)) * 2048 )) -F \
	    -v JAMOS-DATA ::
	python3 tools/fat-label.py $@ $(ESP_END_MIB) --fix
endif
	mmd -i $@@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@@@1M $(LIMINE)/BOOTX64.EFI ::/EFI/BOOT/
	mcopy -i $@@@1M boot/limine.conf ::/boot/limine/
	mcopy -i $@@@1M $(KERNEL) ::/boot/
	mcopy -i $@@@1M $(BOOTFS) ::/boot/bootfs.img
	python3 tools/mkbootfs.py --esp-check $@ $(KERNEL) $(BOOTFS) || { rm -f $@; exit 1; }

# Writable copy of the UEFI variable store (the firmware code image is read-only).
$(BUILD)/ovmf-vars.fd:
	@mkdir -p $(BUILD)
	cp $(OVMF_DIR)/edk2-i386-vars.fd $@

QEMU_FLAGS := -M q35 -m 2G -smp 4 -cpu max \
    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_DIR)/edk2-x86_64-code.fd \
    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=usbstick,format=raw,file=$(IMAGE) \
    -device usb-storage,bus=xhci.0,drive=usbstick,bootindex=0 \
    -device usb-kbd,bus=xhci.0 \
    -device edu,dma_mask=0xffffffff \
    -nic none \
    -serial stdio -no-reboot

run: $(IMAGE) $(BUILD)/ovmf-vars.fd
	qemu-system-x86_64 $(QEMU_FLAGS)

# Same, but wait for gdb on :1234 (attach with
# `x86_64-elf-gdb build/jamos.debug.elf -ex "target remote :1234"`).
debug: $(IMAGE) $(BUILD)/ovmf-vars.fd
	qemu-system-x86_64 $(QEMU_FLAGS) -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

# jamos-sign (tools/jamos-sign.c): the update key and each manifest's
# signature, on the Mac, from the same Monocypher the PC checks with.
HOSTCC ?= cc
$(SIGN_TOOL): tools/jamos-sign.c $(MONO_SRCS) $(MONO_HDRS) \
              third_party/monocypher/tests/vectors-ed25519.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(MONO_DIR) -I$(MONO_DIR)/optional \
	    -o $@ tools/jamos-sign.c $(MONO_SRCS)

# pagetable (tools/pagetable.c): a program's page-hash table as the PC
# computes it, with libos's SHA-256 and with the kernel's, each built as it
# is against a stand-in for the one header of Jam OS's it includes (its
# memcpy): `make check` compares their tables with the packager's.
build/host/shim/os.h build/host/shim/jam/string.h:
	@mkdir -p $(dir $@)
	@printf '#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n' > $@
build/host/pagetable-libos: tools/pagetable.c user/lib/sha256.c user/include/sha256.h \
                            build/host/shim/os.h
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Werror -Ibuild/host/shim -Iuser/include \
	    -o $@ tools/pagetable.c user/lib/sha256.c
build/host/pagetable-kernel: tools/pagetable.c kernel/lib/sha256.c kernel/include/jam/sha256.h \
                             build/host/shim/jam/string.h
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Werror -DPAGETABLE_KERNEL -Ibuild/host/shim \
	    -Ikernel/include -o $@ tools/pagetable.c kernel/lib/sha256.c

# The package repository (docs/M13.7-PLAN.md section 7): every package
# port built, packaged and signed with the ports key (~/.config/jamos/
# ports.key, or JAMOS_PORTS_KEY), with the index, VULNS and the sources,
# in build/repo for tools/ports-server.py. Fetches what isn't cached.
ports: all
	python3 tools/ports.py repo

# menucheck (tools/menucheck.c): init's boot menu check (user/lib/bootmenu.c,
# the same file), on the Mac.
$(MENU_TOOL): tools/menucheck.c user/lib/bootmenu.c user/include/bootmenu.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Werror -Iuser/include \
	    -o $@ tools/menucheck.c user/lib/bootmenu.c

# fontpreview (tools/fontpreview.c): libfun's smooth text on the Mac, from
# the same files (FONT_SRCS, above). `make` draws build/fontpreview.png
# with it (the title bars and sample text); fontcheck is the same program
# built with ASan and UBSan, whose --check `make check` runs. x86-64 code
# (Rosetta on Apple silicon): <os.h>'s inline functions are x86.
HOST_X86     := $(if $(filter arm64,$(shell uname -m)),-arch x86_64)
FONT_TOOL_SRCS := tools/fontpreview.c tools/fontcheck.c tools/panicglyphs.c tools/png.c \
                  $(FONT_SRCS)
FONT_TOOL_DEPS := $(FONT_TOOL_SRCS) tools/fontpreview.h kernel/include/jam/panictext.h \
                  $(LIBFUN_DIR)/fun.h $(LIBFUN_DIR)/internal.h $(FONT_FACES) third_party/stb_truetype/stb_truetype.h
FONT_TOOL_CFLAGS := -std=gnu17 -Wall -Wextra -Werror -Wvla $(HOST_X86) -D_FORTIFY_SOURCE=0 -Iuser/include \
                    -Ikernel/include -Idrivers/include -I$(LIBFUN_DIR) -Ithird_party/stb_truetype
$(FONT_TOOL): $(FONT_TOOL_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(FONT_TOOL_CFLAGS) -O2 -o $@ $(FONT_TOOL_SRCS)
$(FONT_CHECK): $(FONT_TOOL_DEPS)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(FONT_TOOL_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $@ $(FONT_TOOL_SRCS)
# gptcheck (tools/gptcheck.c): the partition table library (the same
# files a disk driver links) with ASan and UBSan, against tools/mkgpt.py's
# images and thousands of mutated tables; `make check` runs it.
GPT_SRCS := tools/gptcheck.c tools/gptmutate.c tools/gptmake.c drivers/lib/parttable.c \
            drivers/lib/parttable_gpt.c drivers/lib/parttable_make.c
$(GPT_CHECK): $(GPT_SRCS) tools/gptcheck.h drivers/include/jam/parttable.h \
              drivers/lib/parttable_int.h kernel/include/jam/status.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -Ikernel/include -Idrivers/include -Idrivers/lib -o $@ $(GPT_SRCS)

# rootportcheck (tools/rootportcheck.c): usb-bus's root port handling (the
# driver's own rootport.c) against a model of PORTSC with the controller's
# event rule (xHCI 4.19.2), which QEMU doesn't have; `make check` runs it.
$(ROOTPORT_CHECK): tools/rootportcheck.c drivers/usb-bus/rootport.c drivers/usb-bus/usbbus.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -Ikernel/include -Idrivers/include -o $@ \
	    tools/rootportcheck.c drivers/usb-bus/rootport.c

# avlcheck (tools/avlcheck.c): the kernel's AVL tree (kernel/lib/avl.c, the
# same file) against a sorted array doing the same steps, with its
# summaries checked from scratch after each; `make check` runs it.
$(AVL_CHECK): tools/avlcheck.c kernel/lib/avl.c kernel/include/jam/avl.h kernel/include/jam/list.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -Ikernel/include -o $@ tools/avlcheck.c kernel/lib/avl.c

# itreecheck (tools/itreecheck.c): the kernel's interval tree
# (kernel/lib/itree.c over kernel/lib/avl.c, the same files) against an array
# of the same ranges searched one by one; `make check` runs it.
$(ITREE_CHECK): tools/itreecheck.c kernel/lib/itree.c kernel/lib/avl.c kernel/include/jam/itree.h \
                kernel/include/jam/avl.h kernel/include/jam/list.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -Ikernel/include -o $@ tools/itreecheck.c kernel/lib/itree.c \
	    kernel/lib/avl.c

# pkgcheck (tools/pkgcheck.c): the ports formats' readers (user/lib/pkg*.c, the
# very files bin/pkg and init link) and utest's checks of them
# (user/tests/utest/pkg*.c), with ASan and UBSan, over PO3's vectors
# (ports/vectors, CASES), mutated packages, indexes and texts, and the texts
# the Python reference reader was asked about (`tools/ports.py pkgfuzz`);
# `make check` runs it.
PKG_LIB   := $(addprefix user/lib/,pkgfmt.c pkglist.c pkgmanifest.c pkgtar.c pkgindex.c \
                                    pkgjournal.c)
PKG_TEST  := $(addprefix user/tests/utest/,pkgcases.c pkgfuzz.c pkgjnl.c pkgmisc.c)
PKG_SHIM  := build/host/pkgshim/os.h
$(PKG_SHIM):
	@mkdir -p $(dir $@)
	@printf '#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n' > $@
$(PKG_CHECK): tools/pkgcheck.c $(PKG_LIB) $(PKG_TEST) user/lib/sha256.c user/lib/pkgint.h \
              user/include/pkgfmt.h user/include/sha256.h user/tests/utest/pkgcases.h \
              user/tests/utest/pkgcases_int.h $(MONO_SRCS) $(MONO_HDRS) $(PKG_SHIM)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -Wvla -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -Ibuild/host/pkgshim -Iuser/include -Ikernel/include \
	    -Iuser/lib -Iuser/tests/utest -I$(MONO_DIR) -I$(MONO_DIR)/optional -o $@ \
	    tools/pkgcheck.c $(PKG_LIB) $(PKG_TEST) user/lib/sha256.c $(MONO_SRCS)

# extcheck (tools/extcheck.c and the other tools/ext*.c): the ext library (the
# extfs service's format code, user/services/extfs/ext, the same files)
# with ASan and UBSan, against the images Linux's mke2fs made
# (tools/extfix/make.sh made them; committed gzipped, unpacked here) and
# thousands of mutated ones, and its writes (names, links, rename, orphans,
# mkfs) on volumes held in memory, each checked against a volume's
# invariants (tools/extinv.c); `make check` runs it. The library is also
# compiled as Jam OS compiles a program's files ($(EXT_CROSS)); bin/extfs
# (user/services/extfs, the service) builds it in as its own subdirectory.
EXT_DIR      := user/services/extfs/ext
EXT_SRCS     := $(wildcard $(EXT_DIR)/*.c)
EXT_HDRS     := $(wildcard $(EXT_DIR)/*.h)
EXT_FIXTURES := $(patsubst tools/extfix/%.img.gz,$(BUILD)/extfix/%.img,\
                           $(wildcard tools/extfix/*.img.gz))
EXT_CROSS    := $(patsubst $(EXT_DIR)/%.c,$(BUILD)/extcross/%.o,$(EXT_SRCS))
EXT_TOOL     := tools/extcheck.c tools/extbreak.c tools/extmutate.c tools/extinv.c \
                tools/extvol.c tools/extmkfs.c tools/extnames.c tools/extbig.c \
                tools/extwrite.c tools/extfile.c tools/extverify.c tools/extcut.c tools/extjnl.c \
                tools/extnstorm.c tools/extdetect.c
$(EXT_CHECK): $(EXT_TOOL) tools/extcheck.h tools/extvol.h tools/extwrite.h tools/extjnl.h \
              $(EXT_SRCS) $(EXT_HDRS) drivers/lib/sha256.c drivers/include/jam/sha256.h \
              kernel/include/jam/status.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O1 -g -Wall -Wextra -Werror -Wvla -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -Idrivers/include -Ikernel/include -I$(EXT_DIR) -o $@ \
	    $(EXT_TOOL) $(EXT_SRCS) drivers/lib/sha256.c
$(EXT_FIXTURES): $(BUILD)/extfix/%.img: tools/extfix/%.img.gz
	@mkdir -p $(dir $@)
	gzip -dc $< > $@
$(EXT_CROSS): $(BUILD)/extcross/%.o: $(EXT_DIR)/%.c $(EXT_HDRS) $(FLAGDIR)/user | $(UINC)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# jamext (tools/jamext): the same library as a host tool for the Mac and any
# Unix, so images of an ext volume are made and read without e2fsprogs:
# `jamext mkimage` builds one from a directory, put/get/ls/ln/check work on
# an image or a partition of a disk image. `make check` makes images with it
# and has extcheck (the sanitised build above) check them
# (tools/jamext-test.sh). Plain -O2: it is a build tool, not a test.
JAMEXT_SRCS  := $(wildcard tools/jamext/*.c)
$(JAMEXT): $(JAMEXT_SRCS) tools/jamext/jamext.h $(EXT_SRCS) $(EXT_HDRS) \
           kernel/include/jam/status.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -Ikernel/include -I$(EXT_DIR) \
	    -Itools/jamext -o $@ $(JAMEXT_SRCS) $(EXT_SRCS)

$(BUILD)/fontpreview.png: $(FONT_TOOL)
	$(FONT_TOOL) $@

# Write the image to a USB stick. Refuses anything that isn't external.
usb: $(IMAGE) $(JAMEXT)
	JAMEXT=$(JAMEXT) DATA_DIR="$(DATA_DIR)" tools/write-usb.sh $(IMAGE) $(DEV)

# Update a stick that already boots Jam OS: the kernel, bootfs and
# limine.conf onto its ESP; nothing erased, /data untouched. DEV is optional
# (the one external disk with Jam OS's layout).
flash: $(IMAGE)
	tools/flash-usb.sh $(KERNEL) $(BOOTFS) boot/limine.conf $(DEV)

font:
	python3 tools/bdf2c.py third_party/spleen/spleen-8x16.bdf kernel/dev/font_8x16.c
	(echo "/* User space's copy of kernel/dev/font_8x16.c, written by \`make font\`. Its own object"; \
	 echo " * in libos.a: only the programs that draw text (<font.h>) link it in. */"; \
	 cat kernel/dev/font_8x16.c) > user/lib/font_8x16.c
	python3 tools/bdf2c.py third_party/spleen/spleen-8x16.bdf user/lib/font_latin.c --latin

clean:
	rm -rf $(BUILD)

# (the objects a program is linked with beside its own, EXTRA_OBJS: utest's
# drivers' files and the cursor set, bigprog's table, ... and the ext
# library's cross build: their headers too)
-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d) $(FATFS_OBJS:.o=.d) $(LWIP_OBJS:.o=.d) \
         $(patsubst %.o,%.d,$(foreach d,$(DRIVERS),$(call DRV_OBJS,$(d)))) \
         $(patsubst %.o,%.d,$(filter %.o,$(foreach p,$(USER_PROGS),$(EXTRA_OBJS_$(p))))) \
         $(EXT_CROSS:.o=.d)

# POSIX on musl: musl, the call layer, the sysroot, posixtest (its own file).
include user/posix/posix.mk

# The ports in the boot image (ports/, docs/M13.7-PLAN.md): every recipe
# with `place boot` (sbase, Lua, libc-test) is built by tools/ports.py with
# jamos-cc against the sysroot, from its vendored source in third_party/.
# The builder decides what is out of date (its stamp: the recipe, the
# port's files, the source, the sysroot), so this rule always asks it, and
# writes $(PORTS_LIST), the `name=file` lines mkbootfs packs, only when
# they changed: an unchanged boot image is packed anew by nobody. A new
# `place boot` recipe needs no edit here.
PORTS_OUT  := $(BUILD)/ports
PORTS_LIST := $(PORTS_OUT)/bootfs.list
$(PORTS_LIST): $(SYSROOT_LIBS) $(SYSROOT_HDRS) tools/jamos-cc FORCE
	@mkdir -p $(PORTS_OUT)
	JAMOS_BASE_SYSROOT=$(SYSROOT) CROSS=$(CROSS) python3 tools/ports.py --out $(PORTS_OUT) \
	    boot -o $@
$(BOOTFS): $(PORTS_LIST)

# The notices of the other third_party/ code in the image (their licences
# ask for them to travel with it; the ports' come from their stages):
# /boot/share/licences/<name>/, as <bootfs name>=<file>. `make check` stops
# on a third_party/ directory that is neither here nor a port's source.
LICENCES := musl/COPYRIGHT=third_party/musl/COPYRIGHT \
            fatfs/LICENSE.txt=third_party/fatfs/LICENSE.txt \
            lwip/COPYING=third_party/lwip/COPYING \
            monocypher/LICENCE.md=third_party/monocypher/LICENCE.md \
            dr_mp3/LICENSE=third_party/dr_mp3/LICENSE \
            pl_mpeg/LICENSE=third_party/pl_mpeg/LICENSE \
            stb_image/LICENSE=third_party/stb_image/LICENSE \
            stb_truetype/LICENSE=third_party/stb_truetype/LICENSE \
            spleen/LICENSE=third_party/spleen/LICENSE \
            inter/LICENSE.txt=third_party/inter/LICENSE.txt \
            jetbrains-mono/OFL.txt=third_party/jetbrains-mono/OFL.txt \
            limine/LICENSE=third_party/limine/LICENSE \
            limine-protocol/LICENSE=third_party/limine-protocol/LICENSE \
            wayland/COPYING=third_party/wayland-protocols/COPYING-wayland \
            wayland-protocols/COPYING=third_party/wayland-protocols/COPYING-wayland-protocols \
            tinyssh/LICENSE.md=third_party/tinyssh/LICENSE.md
BOOTFS_FILES += $(foreach l,$(LICENCES),share/licences/$(firstword $(subst =, ,$(l)))=$(lastword $(subst =, ,$(l))))
$(BOOTFS): $(foreach l,$(LICENCES),$(lastword $(subst =, ,$(l))))
THIRD_PARTY_NOTICED := $(sort $(foreach l,$(LICENCES),$(word 2,$(subst /, ,$(lastword $(subst =, ,$(l)))))) \
                       $(shell awk '$$1 == "vendored" { print $$2 }' ports/*/recipe))

# Transitional: tinyssh (X2) is still a make fragment in user/ports/, until
# it is a recipe; nothing else is.
include user/ports/ports.mk
# Jam OS's own commands as programs (the shell's, built again for POSIX shells).
include user/sysbox/sysbox.mk

# compile_commands.json for editors (VS Code IntelliSense, clangd): the real
# build's flags for every file, from a dry run (tools/compdb.py).
compdb:
	python3 tools/compdb.py

# #include lines in the CODING-GUIDE order (tools/sortincludes.py; `make
# check` fails when a run is out of order, this target fixes them).
includes:
	python3 tools/sortincludes.py --fix

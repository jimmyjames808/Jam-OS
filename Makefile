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
KERNEL  := $(BUILD)/jamos.elf
IMAGE   := $(BUILD)/jamos.img
BOOTFS  := $(BUILD)/bootfs.img
IMAGE_MIB := 128
# The ESP ends at 64 MiB (it starts at 1 MiB); the data partition (/data,
# FAT32 "JAMOS-DATA") fills the rest of the image: 64 MiB in QEMU, the
# rest of the stick once tools/write-usb.sh has grown it.
ESP_END_MIB := 64

LIMINE  := third_party/limine
OVMF_DIR ?= $(shell brew --prefix qemu 2>/dev/null)/share/qemu

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

.PHONY: all image run debug clean font usb flash syscalls idl check compdb includes FORCE

all: $(KERNEL) $(BOOTFS)

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

$(IDL_OK): $(IDL_SRCS) tools/genidl.py $(wildcard $(IDL_GEN))
	@mkdir -p $(BUILD)
	python3 tools/genidl.py check
	@touch $@

idl:
	python3 tools/genidl.py gen

$(OBJS): | $(SYSCALLS_OK) $(IDL_OK)

# Two-pass link: stage 1 has an empty symbol table; its function addresses
# become the table linked into the final kernel. .ksyms is the last section,
# so nothing moves between the passes (gensyms.py verify checks this).
$(BUILD)/ksyms_empty.c: tools/gensyms.py
	@mkdir -p $(BUILD)
	: > $(BUILD)/empty.nm
	python3 tools/gensyms.py gen $(BUILD)/empty.nm $@

$(BUILD)/jamos.stage1.elf: $(OBJS) $(BUILD)/ksyms_empty.o kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) $(BUILD)/ksyms_empty.o -o $@

$(BUILD)/ksyms.c: $(BUILD)/jamos.stage1.elf tools/gensyms.py
	$(CROSS)nm -n --defined-only $< > $(BUILD)/stage1.nm
	python3 tools/gensyms.py gen $(BUILD)/stage1.nm $@

$(KERNEL): $(OBJS) $(BUILD)/ksyms.o kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) $(BUILD)/ksyms.o -o $@
	$(CROSS)nm -n --defined-only $@ > $(BUILD)/final.nm
	python3 tools/gensyms.py verify $(BUILD)/stage1.nm $(BUILD)/final.nm

$(BUILD)/ksyms_empty.o $(BUILD)/ksyms.o: $(BUILD)/%.o: $(BUILD)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

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
$(DRMP3_OBJ): PROG_CFLAGS := -Iuser/lib/mp3port -Ithird_party/dr_mp3
$(UOBJ)/user/lib/mp3.c.o: PROG_CFLAGS := -Ithird_party/dr_mp3
# The programs, by role: user/services/<name> (init, console, devmgr, ...),
# user/apps/<name> (the apps, on libfun) and user/tests/<name>. Every
# directory there with a .c file is the program bin/<name> in bootfs, except
# user/apps/fun, which is the apps library. A program's sources are its
# *.c and one level of subdirectories (user/services/shell/cmd/*.c).
LIBFUN_DIR  := user/apps/fun
USER_DIRS   := $(filter-out $(LIBFUN_DIR),$(patsubst %/,%,$(sort $(dir \
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

$(UOBJ)/%.c.o: %.c | $(UINC) $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(PROG_CFLAGS) -c $< -o $@

$(UOBJ)/%.S.o: %.S | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UOBJ)/libos.a: $(LIBOS_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# libfun (user/apps/fun, <fun.h>): the screen, drawing, text, keys and
# thread pool of the apps, which link it before libos.
FUN_PROGS   := $(notdir $(filter user/apps/%,$(USER_DIRS)))
LIBFUN_OBJS := $(patsubst %,$(UOBJ)/%.o,$(wildcard $(LIBFUN_DIR)/*.c))

$(UOBJ)/libfun.a: $(LIBFUN_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

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

$(FATFS_STAGE)/%.o: $(FATFS_STAGE)/%.c $(FATFS_HDRS) | $(UINC) $(SYSCALLS_OK)
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

$(LWIP_OBJS): $(BUILD)/lwip/%.o: %.c | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(LWIP_INC) -c $< -o $@

# What a program links, includes and needs made first beyond its own
# directory and libos.
EXTRA_OBJS_netstack   := $(LWIP_OBJS)
EXTRA_CFLAGS_netstack := $(LWIP_INC)
EXTRA_OBJS_fat   := $(FATFS_OBJS)
EXTRA_CFLAGS_fat := $(FATFS_INC)
EXTRA_DEPS_fat   := $(FATFS_HDRS)
# The boot splash: pl_mpeg (third_party/pl_mpeg, vendored unmodified, one
# header) and the <string.h>/<stdlib.h> it includes, which are libos's
# (user/apps/splash/port).
EXTRA_CFLAGS_splash := -Ithird_party/pl_mpeg -Iuser/apps/splash/port
# jamjar's album covers, decoded by its helper bin/jamcover: stb_image
# (third_party/stb_image, vendored unmodified, PNG and JPEG only) and the
# <stdlib.h>/<string.h> it includes, which are libos's
# (user/apps/jamcover/port).
EXTRA_CFLAGS_jamcover := -Ithird_party/stb_image -Iuser/apps/jamcover/port
# utest tests the music player's folder walk and spectrum (utest/music.c):
# the player's own objects, linked in, and its header; and the RTL8125
# probe's transmit-register guard (utest/netframe.c: drivers/rtl8125/notx.h);
# and netstack's core with lwIP, driven in-process over a fake edge
# (utest/netstack.c: its stack.h and ctl.h, no lwIP header), the DHCP
# client's and the resolver's cores (utest/dhcp*.c, dns*.c), and the
# network drivers' netdev server over a fake card (utest/netsrv.c:
# drivers/lib/netserver.c), bin/dns's sockets and askers over a fake
# netstack (utest/dnsd.c), bin/sntp's request and checks (utest/sntp.c),
# and the RTL8125 driver's guard over a fake chip (utest/rtlguard.c:
# guard.c and the files it calls, built as user code).
NETSTACK_CORE      := $(patsubst %,$(UOBJ)/user/services/netstack/%.o,stack.c ctl.c port/sys_arch.c)
EXTRA_OBJS_utest   := $(UOBJ)/user/services/music/spectrum.c.o $(UOBJ)/user/services/music/tracks.c.o \
                      $(NETSTACK_CORE) $(LWIP_OBJS) \
                      $(UOBJ)/user/services/dhcp/msg.c.o $(UOBJ)/user/services/dhcp/client.c.o \
                      $(UOBJ)/user/services/dns/msg.c.o $(UOBJ)/user/services/dns/cache.c.o \
                      $(UOBJ)/user/services/dns/resolver.c.o $(UOBJ)/drivers/lib/netserver.c.o \
                      $(UOBJ)/user/services/dns/socks.c.o $(UOBJ)/user/services/dns/askers.c.o \
                      $(UOBJ)/user/services/sntp/ntp.c.o \
                      $(patsubst %,$(UOBJ)/drivers/rtl8125/%.c.o,guard regs chip tx)
EXTRA_CFLAGS_utest := -iquote user/services/music -iquote drivers/rtl8125 \
                      -iquote user/services/netstack -iquote user/services/dhcp \
                      -iquote user/services/dns -iquote user/services/sntp

# $(BUILD)/user/<prog> keeps its debug info (for gdb); bootfs gets a copy
# without it ($(BUILD)/user/<prog>.bootfs), symbols kept for backtraces.
# PROG_CFLAGS: the program's own directory on the "..." include path (so
# "sh.h" works from shell/cmd/), for an app the apps library's <fun.h>,
# and the program's EXTRA_CFLAGS.
define USER_PROG
$(call prog_objs,$(1)): PROG_CFLAGS := -iquote $(call prog_dir,$(1)) \
                                       $(if $(filter $(1),$(FUN_PROGS)),-I$(LIBFUN_DIR)) \
                                       $(EXTRA_CFLAGS_$(1))
$(call prog_objs,$(1)): | $(EXTRA_DEPS_$(1))

$(BUILD)/user/$(1): $(UOBJ)/user/lib/crt0.S.o $(call prog_objs,$(1)) $(EXTRA_OBJS_$(1)) \
                    $(if $(filter $(1),$(FUN_PROGS)),$(UOBJ)/libfun.a) $(UOBJ)/libos.a \
                    user/linker.ld
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) $(UOBJ)/user/lib/crt0.S.o $(call prog_objs,$(1)) $(EXTRA_OBJS_$(1)) \
	    $(if $(filter $(1),$(FUN_PROGS)),$(UOBJ)/libfun.a) $(UOBJ)/libos.a $(LIBGCC) -o $$@

$(BUILD)/user/$(1).bootfs: $(BUILD)/user/$(1)
	$(STRIP) --strip-debug $$< -o $$@
endef
$(foreach p,$(USER_PROGS),$(eval $(call USER_PROG,$(p))))

# ---- drivers (ARCHITECTURE.md "The migration rule") -------------------------
# A driver sees nothing but <jam/driver.h> (+ <jam/abi.h>, <jam/status.h>,
# <jam/task.h>, libos's cooperative tasks, <jam/netframe.h>, pure
# functions over a network frame's bytes, and <jam/netdev.h>, the netdev
# rings' layout and index code; and <jam/netserver.h>, the netdev server
# whose code, drivers/lib/netserver.c, a network driver links into its own
# object: DRV_LIB_<name> below), the generated <idl/*.h>
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
               $(DRV_INC)/jam/netserver.h $(IDL_GEN:drivers/include/%=$(DRV_INC)/%)
DRV_ISOLATE := -nostdinc -isystem $(shell $(CC) -print-file-name=include) -I$(DRV_INC) -fno-builtin
DRV_CFLAGS  := $(filter-out -I%,$(USER_CFLAGS)) $(DRV_ISOLATE)
# DRV_LIB_<name>: the drivers/lib/ files driver <name> links into its own
# object (checked by checkdriver.py with the rest of it).
DRV_LIB_rtl8125 := netserver
DRV_LIB_e1000e  := netserver
DRV_OBJS     = $(patsubst %.c,$(BUILD)/udrv/%.o,$(wildcard $(filter %/$(1),$(DRIVER_DIRS))/*.c)) \
               $(patsubst %,$(BUILD)/udrv/drivers/lib/%.o,$(DRV_LIB_$(1)))

$(DRV_INC)/jam/driver.h $(DRV_INC)/jam/task.h $(DRV_INC)/jam/netframe.h $(DRV_INC)/jam/netdev.h \
        $(DRV_INC)/jam/netserver.h: \
        $(DRV_INC)/jam/%.h: drivers/include/jam/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(DRV_INC)/jam/abi.h $(DRV_INC)/jam/status.h: $(DRV_INC)/jam/%.h: kernel/include/jam/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(filter $(DRV_INC)/idl/%,$(DRV_HDRS)): $(DRV_INC)/idl/%.h: drivers/include/idl/%.h | $(IDL_OK)
	@mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/udrv/drivers/%.o: drivers/%.c | $(DRV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(DRV_CFLAGS) -c $< -o $@

define DRIVER
$(BUILD)/udrv/$(1).o: $(call DRV_OBJS,$(1)) tools/checkdriver.py $(DRV_SURFACE)
	$(LD) -r -o $$@.r $(call DRV_OBJS,$(1))
	python3 tools/checkdriver.py $(CROSS)nm $(1) $$@.r -- $(DRV_SURFACE)
	mv $$@.r $$@

$(BUILD)/drv/$(1): $(UOBJ)/user/lib/crt0.S.o $(UOBJ)/user/lib/driver_crt.c.o $(BUILD)/udrv/$(1).o \
                   $(UOBJ)/libos.a user/linker.ld
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) $(UOBJ)/user/lib/crt0.S.o $(UOBJ)/user/lib/driver_crt.c.o \
	    $(BUILD)/udrv/$(1).o $(UOBJ)/libos.a $(LIBGCC) -o $$@

$(BUILD)/drv/$(1).bootfs: $(BUILD)/drv/$(1)
	$(STRIP) --strip-debug $$< -o $$@
endef
$(foreach d,$(DRIVERS),$(eval $(call DRIVER,$(d))))

# `make check`: the generated code is current, the driver check still
# rejects what it must (tools/checkdriver-tests/: a kernel include, a
# kmalloc call, a call into another driver, ...) and accepts a clean one,
# the Markdown docs still match the tree (tools/checkdocs.py), and the
# RTL8125 listen-only probe has no way to transmit (tools/checknotx.sh).
check: all
	python3 tools/gensyscalls.py check
	python3 tools/genidl.py check
	CC="$(CC)" NM="$(CROSS)nm" CFLAGS="$(DRV_CFLAGS)" SURFACE="$(DRV_SURFACE)" \
	    OUT="$(BUILD)/checkdriver-tests" sh tools/checkdriver-selftest.sh
	python3 tools/checkdocs.py
	python3 tools/sortincludes.py
	sh tools/checkaudio.sh
	sh tools/checknotx.sh
	python3 tools/netpeer.py --selftest
	python3 tools/pcap-vlan-check.py --selftest
	python3 tools/checkwants.py --selftest

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
# `update` show and tools/update-server.py puts in the manifest. Written
# again only when it changes, so an unchanged tree packs nothing anew.
BUILD_INFO := $(BUILD)/build.txt
$(BUILD_INFO): FORCE
	@mkdir -p $(BUILD)
	@h=$$(git rev-parse --short=7 HEAD 2>/dev/null) || h=0000000; \
	 [ $$h = 0000000 ] || git diff --quiet HEAD -- 2>/dev/null || h=$$h-dirty; \
	 printf 'git %s\n' "$$h" > $@.new; \
	 if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; fi
FORCE:

# bootfs: the files init and the tests need before USB and FAT32 work,
# loaded by Limine as a module (boot/limine.conf: module_path).
BOOTFS_FILES := $(foreach p,$(USER_PROGS),bin/$(p)=$(BUILD)/user/$(p).bootfs) \
                $(foreach d,$(DRIVERS),drv/$(d)=$(BUILD)/drv/$(d).bootfs) init.cfg=boot/init.cfg \
                splash.mpg=boot/splash.mpg build.txt=$(BUILD_INFO)

# Every program's list (<wants.h>) is checked first: what the build
# approves for each boot-image program (tools/checkwants.py).
$(BOOTFS): $(USER_PROGS:%=$(BUILD)/user/%.bootfs) $(DRIVERS:%=$(BUILD)/drv/%.bootfs) boot/init.cfg \
           boot/splash.mpg $(BUILD_INFO) tools/mkbootfs.py tools/checkwants.py user/include/os.h
	python3 tools/checkwants.py $(foreach p,$(USER_PROGS),$(call prog_dir,$(p))=$(BUILD)/user/$(p).bootfs)
	python3 tools/mkbootfs.py $@ $(BOOTFS_FILES)

image: $(IMAGE)

$(IMAGE): $(KERNEL) $(BOOTFS) boot/limine.conf tools/mkimage.py tools/fat-label.py
	python3 tools/mkimage.py $@ $(ESP_END_MIB) $(IMAGE_MIB)
	mformat -i $@@@1M -T $$(( ($(ESP_END_MIB) - 1) * 2048 )) -F -v JAMOS ::
	mformat -i $@@@$(ESP_END_MIB)M -T $$(( ($(IMAGE_MIB) - $(ESP_END_MIB)) * 2048 )) -F \
	    -v JAMOS-DATA ::
	python3 tools/fat-label.py $@ 1 --fix
	python3 tools/fat-label.py $@ $(ESP_END_MIB) --fix
	mmd -i $@@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@@@1M $(LIMINE)/BOOTX64.EFI ::/EFI/BOOT/
	mcopy -i $@@@1M boot/limine.conf ::/boot/limine/
	mcopy -i $@@@1M $(KERNEL) ::/boot/
	mcopy -i $@@@1M $(BOOTFS) ::/boot/bootfs.img

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
# `x86_64-elf-gdb build/jamos.elf -ex "target remote :1234"`).
debug: $(IMAGE) $(BUILD)/ovmf-vars.fd
	qemu-system-x86_64 $(QEMU_FLAGS) -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

# Write the image to a USB stick. Refuses anything that isn't external.
usb: $(IMAGE)
	tools/write-usb.sh $(IMAGE) $(DEV)

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

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d) $(FATFS_OBJS:.o=.d) $(LWIP_OBJS:.o=.d) \
         $(patsubst %.o,%.d,$(foreach d,$(DRIVERS),$(call DRV_OBJS,$(d))))

# compile_commands.json for editors (VS Code IntelliSense, clangd): the real
# build's flags for every file, from a dry run (tools/compdb.py).
compdb:
	python3 tools/compdb.py

# #include lines in the CODING-GUIDE order (tools/sortincludes.py; `make
# check` fails when a run is out of order, this target fixes them).
includes:
	python3 tools/sortincludes.py --fix

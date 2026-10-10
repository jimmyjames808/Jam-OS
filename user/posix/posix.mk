# POSIX on musl (docs/M13-PLAN.md sections 1 and 14), included at the end
# of the Makefile: musl's C library, built by these rules (not musl's
# configure) from third_party/musl, unmodified, less its dynamic linker
# (ldso/, src/ldso) and the files our overrides replace
# (user/posix/musl/OVERRIDES); the Linux call layer (user/posix/sys/) and
# the start (user/posix/crt/); the sysroot, build/sysroot, that
# tools/jamos-cc compiles and links against; and posixtest
# (user/posix/test/), built with jamos-cc, as bin/posixtest in bootfs.
#
#   build/sysroot/include   musl's headers (its two generated ones made
#                           by the same sed steps as musl's Makefile) and
#                           Jam OS's extensions (user/posix/include)
#   build/sysroot/lib       libc.a (musl, the overrides, the call layer
#                           and libjam), crt1.o, crti.o, crtn.o,
#                           posix.ld, and musl's empty libm.a & co. so
#                           -lm links
#
# musl is compiled once per clean build (about 1,300 files); the
# override check (tools/checkoverrides.py) runs first, so a changed
# upstream file behind an override stops the build.

MUSL       := third_party/musl
POSIX      := user/posix
MUSL_OUT   := $(BUILD)/musl
POSIX_OUT  := $(BUILD)/posix
SYSROOT    := $(BUILD)/sysroot
MUSL_OVR   := $(POSIX)/musl/OVERRIDES

# The overrides: the upstream files left out, and ours built in their place
# (a header is found first on the include path instead; crt1 is crt1.o).
MUSL_OVR_UP   := $(shell awk '!/^\#/ && NF == 3 { print $$1 }' $(MUSL_OVR))
MUSL_OVR_OURS := $(shell awk '!/^\#/ && NF == 3 { print $$3 }' $(MUSL_OVR))
MUSL_OVR_SRCS := $(filter-out %.h %/crt1.c,$(MUSL_OVR_OURS))
MUSL_OVR_OBJS := $(MUSL_OVR_SRCS:%=$(MUSL_OUT)/ovr/%.o)

# musl's sources as its Makefile picks them: src/*/*.c and mallocng, an
# arch file (src/<dir>/x86_64/<name>.[csS]) replacing the generic
# <name>.c beside it.
MUSL_DIRS := $(filter-out $(MUSL)/src/ldso,$(wildcard $(MUSL)/src/*)) $(MUSL)/src/malloc/mallocng
MUSL_BASE := $(wildcard $(addsuffix /*.c,$(MUSL_DIRS)))
MUSL_ARCH := $(wildcard $(addsuffix /x86_64/*.c,$(MUSL_DIRS)) \
                        $(addsuffix /x86_64/*.s,$(MUSL_DIRS)) $(addsuffix /x86_64/*.S,$(MUSL_DIRS)))
MUSL_SRCS := $(filter-out $(addsuffix .c,$(subst /x86_64/,/,$(basename $(MUSL_ARCH)))),$(MUSL_BASE)) \
             $(MUSL_ARCH)
MUSL_SRCS := $(sort $(filter-out $(addprefix $(MUSL)/,$(MUSL_OVR_UP)),$(MUSL_SRCS)))
MUSL_OBJS := $(patsubst $(MUSL)/%,$(MUSL_OUT)/obj/%.o,$(MUSL_SRCS))

MUSL_GENH := $(MUSL_OUT)/include/bits/alltypes.h $(MUSL_OUT)/include/bits/syscall.h \
             $(MUSL_OUT)/internal/version.h
MUSL_CHECKED := $(MUSL_OUT)/overrides.ok

# musl's own flags (its configure's choices for GCC on x86-64), plus
# -fno-tree-loop-distribute-patterns everywhere (no loop turned into a
# call of the function it implements) and our override directory first
# on the include path (syscall_arch.h).
MUSL_INC    := -I$(POSIX)/musl/arch/x86_64 -I$(MUSL)/arch/x86_64 -I$(MUSL)/arch/generic \
               -I$(MUSL_OUT)/internal -I$(MUSL)/src/include -I$(MUSL)/src/internal \
               -I$(MUSL_OUT)/include -I$(MUSL)/include
MUSL_CFLAGS := -std=c99 -nostdinc -ffreestanding -fexcess-precision=standard -frounding-math \
               -fno-strict-aliasing -Wa,--noexecstack -D_XOPEN_SOURCE=700 $(MUSL_INC) \
               -O2 -pipe -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables \
               -ffunction-sections -fdata-sections -fno-stack-protector \
               -fno-tree-loop-distribute-patterns -fno-PIC -fno-pie -m64 -march=x86-64 \
               -mcmodel=small -Wno-pointer-to-int-cast -Werror=implicit-function-declaration \
               -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith \
               -Werror=int-conversion -Werror=incompatible-pointer-types \
               -Werror=discarded-qualifiers -Werror=discarded-array-qualifiers -Waddress \
               -Warray-bounds -Wchar-subscripts -Wduplicate-decl-specifier -Winit-self \
               -Wreturn-type -Wsequence-point -Wstrict-aliasing -Wunused-function \
               -Wunused-label -Wunused-variable -MMD -MP

# The call layer and the start: our C rules, on the sysroot's headers and
# libjam's (<jamos/libjam.h>). Every handler has the six arguments of a
# Linux call and uses what its call has: -Wno-unused-parameter.
POSIX_CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -Wno-unused-parameter \
                -ffreestanding -fno-stack-protector -fno-stack-check -fno-PIC -fno-pie \
                -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -m64 -march=x86-64 \
                -mcmodel=small -ffunction-sections -fdata-sections -D_GNU_SOURCE \
                -nostdinc -isystem $(SYSROOT)/include -I$(POSIX)/include -Iuser/include \
                -I$(BUILD)/uinc -Idrivers/include -iquote $(POSIX)/sys -iquote $(POSIX)/crt \
                -MMD -MP
POSIX_SRCS := $(wildcard $(POSIX)/sys/*.c) $(filter-out %/crt1.c,$(wildcard $(POSIX)/crt/*.c))
POSIX_OBJS := $(POSIX_SRCS:%=$(POSIX_OUT)/%.o)

SYSROOT_HDRS := $(SYSROOT)/include/.made
SYSROOT_EMPTY := $(addprefix $(SYSROOT)/lib/lib,$(addsuffix .a,m rt pthread crypt util xnet resolv dl))
SYSROOT_LIBS := $(SYSROOT)/lib/libc.a $(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
                $(SYSROOT)/lib/crtn.o $(SYSROOT)/lib/posix.ld $(SYSROOT_EMPTY)
# musl and the overrides alone, without the layer and libjam (checklibjam's).
MUSL_LIB := $(MUSL_OUT)/libmusl.a


.PHONY: sysroot
sysroot: $(SYSROOT_LIBS)
all: $(SYSROOT_LIBS)

$(MUSL_CHECKED): $(MUSL_OVR) tools/checkoverrides.py $(addprefix $(MUSL)/,$(MUSL_OVR_UP)) \
                 $(MUSL_OVR_OURS)
	@mkdir -p $(dir $@)
	python3 tools/checkoverrides.py $(MUSL_OVR) $(MUSL)
	touch $@

$(MUSL_OUT)/include/bits/alltypes.h: $(MUSL)/arch/x86_64/bits/alltypes.h.in \
                                     $(MUSL)/include/alltypes.h.in $(MUSL)/tools/mkalltypes.sed
	@mkdir -p $(dir $@)
	sed -f $(MUSL)/tools/mkalltypes.sed $(MUSL)/arch/x86_64/bits/alltypes.h.in \
	    $(MUSL)/include/alltypes.h.in > $@
$(MUSL_OUT)/include/bits/syscall.h: $(MUSL)/arch/x86_64/bits/syscall.h.in
	@mkdir -p $(dir $@)
	cp $< $@
	sed -n -e s/__NR_/SYS_/p < $< >> $@
$(MUSL_OUT)/internal/version.h: $(MUSL)/VERSION
	@mkdir -p $(dir $@)
	printf '#define VERSION "%s"\n' "$$(cat $<)" > $@

$(MUSL_OUT)/obj/%.c.o: $(MUSL)/%.c | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $@
$(MUSL_OUT)/obj/%.s.o: $(MUSL)/%.s | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $@
$(MUSL_OUT)/obj/%.S.o: $(MUSL)/%.S | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $@

# Our overrides: musl's flags, glue.h (user/posix/sys) and, for the
# assembly, Jam OS's call numbers.
$(MUSL_OUT)/ovr/%.c.o: %.c | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -iquote $(POSIX)/sys -c $< -o $@
$(MUSL_OUT)/ovr/%.S.o: %.S | $(MUSL_GENH) $(MUSL_CHECKED) $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -I$(BUILD)/uinc -c $< -o $@

$(SYSROOT_HDRS): $(MUSL_GENH) $(shell find $(MUSL)/include $(MUSL)/arch/generic/bits \
                                          $(MUSL)/arch/x86_64/bits $(POSIX)/include -name '*.h')
	rm -rf $(SYSROOT)/include
	mkdir -p $(SYSROOT)/include/bits
	cp -R $(MUSL)/include/. $(SYSROOT)/include/
	rm -f $(SYSROOT)/include/alltypes.h.in
	cp $(MUSL)/arch/generic/bits/*.h $(SYSROOT)/include/bits/
	cp $(MUSL)/arch/x86_64/bits/*.h $(SYSROOT)/include/bits/
	cp $(MUSL_OUT)/include/bits/alltypes.h $(MUSL_OUT)/include/bits/syscall.h \
	    $(SYSROOT)/include/bits/
	cp -R $(POSIX)/include/. $(SYSROOT)/include/
	touch $@

$(POSIX_OUT)/%.c.o: %.c | $(SYSROOT_HDRS) $(UINC) $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $(dir $@)
	$(CC) $(POSIX_CFLAGS) -c $< -o $@

$(SYSROOT)/lib/crt1.o: $(POSIX_OUT)/$(POSIX)/crt/crt1.c.o
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSROOT)/lib/crti.o $(SYSROOT)/lib/crtn.o: $(SYSROOT)/lib/%.o: $(MUSL_OUT)/obj/crt/x86_64/%.s.o
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSROOT)/lib/posix.ld: $(POSIX)/posix.ld
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSROOT_EMPTY):
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rc $@

# An archive keeps one member per name: check none was lost to another
# object of the same file name.
define POSIX_ARCHIVE
	rm -f $@
	$(AR) rcs $@ $^
	@test "$$($(AR) t $@ | wc -l)" -eq $(words $^) || \
	    { echo "$@: an object name is used twice"; rm -f $@; exit 1; }
endef
$(MUSL_LIB): $(MUSL_OBJS) $(MUSL_OVR_OBJS)
	$(POSIX_ARCHIVE)
$(SYSROOT)/lib/libc.a: $(MUSL_OBJS) $(MUSL_OVR_OBJS) $(POSIX_OBJS) $(LIBJAM_OBJS)
	@mkdir -p $(dir $@)
	$(POSIX_ARCHIVE)

# posixtest: one program, a section per area (start, memory, threads, ...;
# later tracks add theirs), built with jamos-cc as any port is.
POSIXTEST      := $(BUILD)/user/posixtest
POSIXTEST_SRCS := $(wildcard $(POSIX)/test/*.c)
$(POSIXTEST): $(POSIXTEST_SRCS) $(wildcard $(POSIX)/test/*.h) $(POSIX)/test/wants \
              $(SYSROOT_LIBS) tools/jamos-cc | $(UINC) $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $(dir $@)
	JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc -std=gnu17 -O2 -g -Wall -Wextra \
	    -Werror -Wvla -D_GNU_SOURCE -I$(POSIX)/include -Iuser/include -I$(BUILD)/uinc \
	    -Idrivers/include --jamos-wants=$(POSIX)/test/wants $(POSIXTEST_SRCS) -o $@
$(POSIXTEST).bootfs: $(POSIXTEST) tools/checkwants.py
	$(STRIP) --strip-debug $< -o $@
	python3 tools/checkwants.py $(POSIX)/test=$@
BOOTFS_FILES += bin/posixtest=$(POSIXTEST).bootfs
$(BOOTFS): $(POSIXTEST).bootfs

# The layer's own small programs (user/posix/progs/<name>/: its .c files
# and its list, `wants`), built with jamos-cc as posixtest is: sh, /bin/sh
# until a POSIX shell (`sh -c` of a simple command, for system and popen),
# and ls, a tiny one at test/ls for posixtest's popen check until sbase's,
# and pxio, the shell tests' (tools/shell-tests/posix.txt), also at
# usr/bin/pxio-usr for the shell's PATH lookup past /boot/bin.
POSIX_PROGS := sh=bin/sh ls=test/ls pxio=bin/pxio
define POSIX_PROG_RULES
$(BUILD)/user/posix-$(1): $(wildcard $(POSIX)/progs/$(1)/*.c) $(POSIX)/progs/$(1)/wants \
                          $(SYSROOT_LIBS) tools/jamos-cc | $(UINC) $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $$(dir $$@)
	JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc -std=gnu17 -O2 -g -Wall -Wextra \
	    -Werror -Wvla -D_GNU_SOURCE -I$(POSIX)/include -Iuser/include -I$(BUILD)/uinc \
	    -Idrivers/include --jamos-wants=$(POSIX)/progs/$(1)/wants \
	    $(wildcard $(POSIX)/progs/$(1)/*.c) -o $$@
$(BUILD)/user/posix-$(1).bootfs: $(BUILD)/user/posix-$(1) tools/checkwants.py
	$(STRIP) --strip-debug $$< -o $$@
	python3 tools/checkwants.py $(POSIX)/progs/$(1)=$$@
$(BOOTFS): $(BUILD)/user/posix-$(1).bootfs
BOOTFS_FILES += $(2)=$(BUILD)/user/posix-$(1).bootfs
endef
$(foreach p,$(POSIX_PROGS),$(eval $(call POSIX_PROG_RULES,$(word 1,$(subst =, ,$(p))),$(word 2,$(subst =, ,$(p))))))
BOOTFS_FILES += usr/bin/pxio-usr=$(BUILD)/user/posix-pxio.bootfs

# `make check`: the overrides still match upstream, and the check refuses
# a changed, a missing and a malformed one; no name libjam defines is one
# musl's libc defines.
.PHONY: posix-check
posix-check: $(MUSL_LIB)
	python3 tools/checkoverrides.py $(MUSL_OVR) $(MUSL)
	python3 tools/checkoverrides.py --selftest $(MUSL_OVR) $(MUSL)
	python3 tools/checklibjam.py $(CROSS)nm $(UOBJ)/libjam.a $(MUSL_LIB)
check: posix-check

-include $(MUSL_OBJS:.o=.d) $(MUSL_OVR_OBJS:.o=.d) $(POSIX_OBJS:.o=.d) \
         $(POSIX_OUT)/$(POSIX)/crt/crt1.c.d

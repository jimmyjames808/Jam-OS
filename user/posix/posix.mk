# POSIX on musl (docs/M13-PLAN.md sections 1 and 14), included at the end
# of the Makefile: musl's C library, built by these rules (not musl's
# configure) from third_party/musl, unmodified, less its dynamic linker
# (ldso/, src/ldso) and the files our overrides replace
# (user/posix/musl/OVERRIDES); the Linux call layer (user/posix/sys/) and
# the start (user/posix/crt/); glibc's extensions musl lacks
# (user/posix/glibc/: <execinfo.h>); the sysroot, build/sysroot, that
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
# Of src/ldso, only what a static program has: dl_iterate_phdr (its one
# module, the program's headers from AT_PHDR), by which libunwind finds
# the unwind tables (tools/jamos-clang++'s C++ exceptions), and dladdr's
# stub (no symbols to name: 0), which libunwind's function names call.
# And the rest of what musl's static libc.a has of it: dlopen() that fails
# ("Dynamic loading not supported"), dlsym() that finds nothing, dlclose(),
# dlinfo() and dlerror(), so a program that may load a library (Chromium's
# base) links and gets an error at run time, as a static Linux program does.
MUSL_SRCS += $(MUSL)/src/ldso/dl_iterate_phdr.c $(MUSL)/src/ldso/dladdr.c \
             $(MUSL)/src/ldso/dlopen.c $(MUSL)/src/ldso/__dlsym.c \
             $(MUSL)/src/ldso/x86_64/dlsym.s $(MUSL)/src/ldso/dlclose.c \
             $(MUSL)/src/ldso/dlinfo.c $(MUSL)/src/ldso/dlerror.c
MUSL_OBJS := $(patsubst $(MUSL)/%,$(MUSL_OUT)/obj/%.o,$(MUSL_SRCS))

MUSL_GENH := $(MUSL_OUT)/include/bits/alltypes.h $(MUSL_OUT)/include/bits/syscall.h \
             $(MUSL_OUT)/internal/version.h
MUSL_CHECKED := $(MUSL_OUT)/overrides.ok

# musl's own flags (its configure's choices for GCC on x86-64), plus
# -fno-tree-loop-distribute-patterns everywhere (no loop turned into a
# call of the function it implements) and our override directory first
# on the include path (syscall_arch.h). Unlike musl's configure, musl
# has unwind tables (-fasynchronous-unwind-tables, as the layer's code
# and libjam's in libc.a): backtrace() (user/posix/glibc/execinfo.c)
# walks the stack by them, through qsort's comparator calls, a thread's
# start and the delivery of a signal to its handler (a call boundary in
# libc, user/posix/sys/sig.h), and C++ exceptions unwind through them.
# .eh_frame costs a program about a tenth of its code's size (posixtest:
# 58 KiB more on 576 KiB of code, a file 0.5% bigger).
MUSL_INC    := -I$(POSIX)/musl/arch/x86_64 -I$(MUSL)/arch/x86_64 -I$(MUSL)/arch/generic \
               -I$(MUSL_OUT)/internal -I$(MUSL)/src/include -I$(MUSL)/src/internal \
               -I$(MUSL_OUT)/include -I$(MUSL)/include
MUSL_CFLAGS := -std=c99 -nostdinc -ffreestanding -fexcess-precision=standard -frounding-math \
               -fno-strict-aliasing -Wa,--noexecstack -D_XOPEN_SOURCE=700 $(MUSL_INC) \
               -O2 -pipe -fomit-frame-pointer -fasynchronous-unwind-tables \
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
                -fno-omit-frame-pointer -fasynchronous-unwind-tables -m64 -march=x86-64 \
                -mcmodel=small -ffunction-sections -fdata-sections -D_GNU_SOURCE \
                -nostdinc -isystem $(SYSROOT)/include -I$(POSIX)/include -Iuser/include \
                -I$(BUILD)/uinc -Idrivers/include -iquote $(POSIX)/sys -iquote $(POSIX)/crt \
                -MMD -MP
POSIX_SRCS := $(wildcard $(POSIX)/sys/*.c) $(wildcard $(POSIX)/sys/*.S) \
              $(filter-out %/crt1.c,$(wildcard $(POSIX)/crt/*.c))
POSIX_OBJS := $(POSIX_SRCS:%=$(POSIX_OUT)/%.o)

# glibc's extensions that musl lacks (user/posix/glibc/: <execinfo.h>),
# built with the layer's flags but, unlike the layer, with their names as
# they are: they are libc's public names, so they are not renamed
# (below), and a program that defines one of its own links its own.
POSIX_GLIBC_SRCS := $(wildcard $(POSIX)/glibc/*.c)
POSIX_GLIBC_OBJS := $(POSIX_GLIBC_SRCS:%=$(POSIX_OUT)/%.o)

# The layer's objects as libc.a holds them: every global name that isn't
# the implementation's own (__*) or a public extension (jamos_*) renamed
# __jl_<name> (tools/posixnames.py says why), so a program's own names
# never meet the layer's. JL_NAMES_H gives the renames as #defines, for
# posixtest's one look inside the layer.
POSIX_JL_OBJS := $(POSIX_OBJS:$(POSIX_OUT)/%=$(POSIX_OUT)/jl/%)
JL_NAMES_MAP  := $(POSIX_OUT)/jl-names.map
JL_NAMES_H    := $(POSIX_OUT)/jl-names.h
JL_NAMES_OK   := $(POSIX_OUT)/jl-names.ok
# (posixnames.py leaves the map and the header alone when their text is the
# same, so they stay older than the objects they came from, and a rule on
# them would run it on every make: the rule is on a stamp it touches.)
$(JL_NAMES_OK): $(POSIX_OBJS) tools/posixnames.py
	python3 tools/posixnames.py $(CROSS)nm $(JL_NAMES_MAP) $(JL_NAMES_H) $(POSIX_OBJS)
	touch $@
$(JL_NAMES_MAP) $(JL_NAMES_H): $(JL_NAMES_OK)
$(POSIX_OUT)/jl/%: $(POSIX_OUT)/% $(JL_NAMES_MAP)
	@mkdir -p $(dir $@)
	$(CROSS)objcopy --redefine-syms=$(JL_NAMES_MAP) $< $@

# libjam as libc.a holds it: libos's own files (user/lib/*.c) built again
# with a section per function and per datum, so the link's --gc-sections
# (tools/jamos-cc) drops what a program doesn't call, where libjam.a's
# objects come in whole. The few with flags of their own (dr_mp3's and
# Monocypher's) and the assembly stay as libjam.a has them.
LIBJAM_GC_SKIP := $(UOBJ)/user/lib/mp3.c.o $(UOBJ)/user/lib/updsig.c.o \
                  $(UOBJ)/user/lib/pkgfmt.c.o
LIBJAM_GC_FROM := $(filter-out $(LIBJAM_GC_SKIP),$(filter \
                      $(patsubst %,$(UOBJ)/%.o,$(wildcard user/lib/*.c)),$(LIBJAM_OBJS)))
LIBJAM_GC_OBJS := $(patsubst $(UOBJ)/%,$(POSIX_OUT)/libjam/%,$(LIBJAM_GC_FROM))
LIBJAM_IN_LIBC := $(filter-out $(LIBJAM_GC_FROM),$(LIBJAM_OBJS)) $(LIBJAM_GC_OBJS)
LIBJAM_GC_CFLAGS := $(USER_CFLAGS) -fasynchronous-unwind-tables -ffunction-sections \
                    -fdata-sections
$(POSIX_OUT)/libjam/%.c.o: %.c $(FLAGDIR)/libjam-gc | $(UINC) $(SYSCALLS_OK) $(IDL_OK) $(WL_OK)
	@mkdir -p $(dir $@)
	$(CC) $(LIBJAM_GC_CFLAGS) -c $< -o $@
-include $(LIBJAM_GC_OBJS:.o=.d)

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

# Flag stamps (the Makefile's FLAG_STAMP): each group of objects is built
# again when its own flags change, and only then. musl's objects are the
# most of a build, and a build kept from before a flag changed (the unwind
# tables, say) must not keep them.
FLAGS_musl       = $(TOOLID) $(MUSL_CFLAGS)
FLAGS_musl-ovr   = $(TOOLID) $(MUSL_OVR_CFLAGS)
FLAGS_musl-ovr-s = $(TOOLID) $(MUSL_OVR_SFLAGS)
FLAGS_posix      = $(TOOLID) $(POSIX_CFLAGS)
FLAGS_libjam-gc  = $(TOOLID) $(LIBJAM_GC_CFLAGS)
$(foreach s,musl musl-ovr musl-ovr-s posix libjam-gc,$(eval $(call FLAG_STAMP,$(s),FLAGS_$(s))))

$(MUSL_OUT)/obj/%.c.o: $(MUSL)/%.c $(FLAGDIR)/musl | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $@
$(MUSL_OUT)/obj/%.s.o: $(MUSL)/%.s $(FLAGDIR)/musl | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $@
$(MUSL_OUT)/obj/%.S.o: $(MUSL)/%.S $(FLAGDIR)/musl | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_CFLAGS) -c $< -o $@

# Our overrides: musl's flags, glue.h (user/posix/sys) and, for the
# assembly, Jam OS's call numbers.
MUSL_OVR_CFLAGS := $(MUSL_CFLAGS) -iquote $(POSIX)/sys
MUSL_OVR_SFLAGS := $(MUSL_CFLAGS) -I$(BUILD)/uinc
$(MUSL_OUT)/ovr/%.c.o: %.c $(FLAGDIR)/musl-ovr | $(MUSL_GENH) $(MUSL_CHECKED)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_OVR_CFLAGS) -c $< -o $@
$(MUSL_OUT)/ovr/%.S.o: %.S $(FLAGDIR)/musl-ovr-s | $(MUSL_GENH) $(MUSL_CHECKED) $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(MUSL_OVR_SFLAGS) -c $< -o $@

$(SYSROOT_HDRS): $(MUSL_GENH) $(shell find $(MUSL)/include $(MUSL)/arch/generic/bits \
                                          $(MUSL)/arch/x86_64/bits $(POSIX)/include \
                                          $(POSIX)/musl/arch/x86_64/bits -name '*.h')
	rm -rf $(SYSROOT)/include
	mkdir -p $(SYSROOT)/include/bits
	cp -R $(MUSL)/include/. $(SYSROOT)/include/
	rm -f $(SYSROOT)/include/alltypes.h.in
	cp $(MUSL)/arch/generic/bits/*.h $(SYSROOT)/include/bits/
	cp $(MUSL)/arch/x86_64/bits/*.h $(SYSROOT)/include/bits/
	cp $(POSIX)/musl/arch/x86_64/bits/*.h $(SYSROOT)/include/bits/   # the overrides' (OVERRIDES)
	cp $(MUSL_OUT)/include/bits/alltypes.h $(MUSL_OUT)/include/bits/syscall.h \
	    $(SYSROOT)/include/bits/
	cp -R $(POSIX)/include/. $(SYSROOT)/include/
	touch $@

$(POSIX_OUT)/%.c.o: %.c $(FLAGDIR)/posix | $(SYSROOT_HDRS) $(UINC) $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $(dir $@)
	$(CC) $(POSIX_CFLAGS) -c $< -o $@
$(POSIX_OUT)/%.S.o: %.S | $(SYSROOT_HDRS) $(UINC) $(SYSCALLS_OK) $(IDL_OK)
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
$(SYSROOT)/lib/libc.a: $(MUSL_OBJS) $(MUSL_OVR_OBJS) $(POSIX_JL_OBJS) $(POSIX_GLIBC_OBJS) \
                       $(LIBJAM_IN_LIBC)
	@mkdir -p $(dir $@)
	$(POSIX_ARCHIVE)

# posixtest: one program, a section per area (start, memory, threads, ...;
# later tracks add theirs), built with jamos-cc as any port is.
POSIXTEST      := $(BUILD)/user/posixtest
POSIXTEST_SRCS := $(wildcard $(POSIX)/test/*.c)
# The flags of the programs built here (posixtest and the layer's own, below).
POSIX_PROG_CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -D_GNU_SOURCE \
                     -I$(POSIX)/include -Iuser/include -I$(BUILD)/uinc -Idrivers/include
POSIXTEST_CFLAGS  := $(POSIX_PROG_CFLAGS) -include $(JL_NAMES_H)
# jamos-cc compiles and links in one command, so these programs have no -MMD
# files: the headers they take from outside their own directory are listed
# (the sysroot's, libjam's, the kernel's copies and the IDL's).
POSIX_PROG_HDRS   := $(SYSROOT_HDRS) $(UINC) \
                     $(wildcard user/include/*.h drivers/include/*.h drivers/include/*/*.h)
FLAGS_posixtest   = $(TOOLID) $(POSIXTEST_CFLAGS)
FLAGS_posix-progs = $(TOOLID) $(POSIX_PROG_CFLAGS)
$(foreach s,posixtest posix-progs,$(eval $(call FLAG_STAMP,$(s),FLAGS_$(s))))
$(POSIXTEST): $(POSIXTEST_SRCS) $(wildcard $(POSIX)/test/*.h) $(POSIX)/test/wants \
              $(SYSROOT_LIBS) $(JL_NAMES_MAP) tools/jamos-cc $(FLAGDIR)/posixtest \
              $(POSIX_PROG_HDRS) | $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $(dir $@)
	JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc $(POSIXTEST_CFLAGS) \
	    --jamos-wants=$(POSIX)/test/wants $(POSIXTEST_SRCS) -o $@
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
# usr/bin/pxio-usr for the shell's PATH lookup past /boot/bin. POSIX test
# programs elsewhere the same way, from their own directory (the third
# word): jittest, JIT memory through MAP_JIT (its list says `right jit`,
# which checkwants allows only under user/tests/ and the ports).
POSIX_PROGS := sh=bin/sh ls=test/ls pxio=bin/pxio jittest=bin/jittest=user/tests/jittest
define POSIX_PROG_RULES
$(BUILD)/user/posix-$(1): $(wildcard $(3)/*.c) $(3)/wants \
                          $(SYSROOT_LIBS) tools/jamos-cc $(FLAGDIR)/posix-progs \
                          $(POSIX_PROG_HDRS) | $(SYSCALLS_OK) $(IDL_OK)
	@mkdir -p $$(dir $$@)
	JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc $(POSIX_PROG_CFLAGS) \
	    --jamos-wants=$(3)/wants \
	    $(wildcard $(3)/*.c) -o $$@
$(BUILD)/user/posix-$(1).bootfs: $(BUILD)/user/posix-$(1) tools/checkwants.py
	$(STRIP) --strip-debug $$< -o $$@
	python3 tools/checkwants.py $(3)=$$@
$(BOOTFS): $(BUILD)/user/posix-$(1).bootfs
BOOTFS_FILES += $(2)=$(BUILD)/user/posix-$(1).bootfs
endef
$(foreach p,$(POSIX_PROGS),$(eval $(call POSIX_PROG_RULES,$(word 1,$(subst =, ,$(p))),$(word 2,$(subst =, ,$(p))),$(or $(word 3,$(subst =, ,$(p))),$(POSIX)/progs/$(word 1,$(subst =, ,$(p)))))))
BOOTFS_FILES += usr/bin/pxio-usr=$(BUILD)/user/posix-pxio.bootfs

# `make check`: the overrides still match upstream, and the check refuses
# a changed, a missing and a malformed one; no name libjam defines is one
# musl's libc defines; a program that defines names the layer uses inside
# libc.a (tools/posix-link-test.c) links, and links none of the handlers
# for calls it can't make (calls.def: here, starting programs, socket
# options, timers); and the members of libc.a a backtrace passes through
# (the signal delivery, a thread's start, qsort, execinfo itself, libjam's
# files) hold unwind tables (tools/checkeh.py: a build kept from before the
# flag was set has none, and only posixtest's execinfo section would say).
.PHONY: posix-check
posix-check: $(MUSL_LIB) $(SYSROOT_LIBS)
	python3 tools/checkoverrides.py $(MUSL_OVR) $(MUSL)
	python3 tools/checkoverrides.py --selftest $(MUSL_OVR) $(MUSL)
	python3 tools/checklibjam.py $(CROSS)nm $(UOBJ)/libjam.a $(MUSL_LIB)
	python3 tools/checkeh.py $(CROSS)readelf $(SYSROOT)/lib/libc.a raise.c.o qsort.c.o \
	    pthread_create.c.o signal.c.o calls.c.o sigcalls.c.o execinfo.c.o fs.c.o
	JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc -O2 tools/posix-link-test.c \
	    -o $(BUILD)/posix-link-test
	@! $(CROSS)nm $(BUILD)/posix-link-test | grep -E ' __jl_lx_(execve|setsockopt|timer_create)$$' || \
	    { echo "posix-check: a program that calls write links handlers it never calls"; exit 1; }
check: posix-check

-include $(MUSL_OBJS:.o=.d) $(MUSL_OVR_OBJS:.o=.d) $(POSIX_OBJS:.o=.d) $(POSIX_GLIBC_OBJS:.o=.d) \
         $(POSIX_OUT)/$(POSIX)/crt/crt1.c.d

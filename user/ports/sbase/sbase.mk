# sbase (suckless.org's POSIX utilities, MIT), docs/M13-PLAN.md question 9:
# third_party/sbase unmodified, one program per utility, built with
# jamos-cc into the boot image's usr/bin/. The shell finds them after its
# own commands (PATH is /boot/bin:/boot/usr/bin), so where the shell has a
# command of the name (cat, ls, sort, ...) the shell's is the one a bare
# name runs, and sbase's is `run /boot/usr/bin/<name>`; programs the shell
# has no command for (sed, tr, cut, xargs, od, printf, ...) run by name.
#
# What is built: sbase's own Makefile builds libutf.a and libutil.a and
# each tool from `<tool>.c` and the two libraries, and so do these rules
# (its CPPFLAGS, no -DPREFIX: nothing here installs). Three tools fork
# (find -exec, time, xargs) and run ours (OVERRIDES). What is left out, and
# why, is in docs/M13-PLAN.md's "P1 notes".
#
# Each program's list is one of three, by what it touches (the least that
# lets it do its job; /esp, the boot stick's loader and kernel, is never
# in them):
#   wants-plain   a terminal and pipes only: filters of their input
#                 (tr, seq, printf, ...) and things that report (uname)
#   wants-ro      files read: /data, other sticks, and /boot
#   wants-rw      files read and written (cp, rm, sort -o, sed w, ...), and
#                 the programs that start others (find -exec, xargs, env,
#                 time): a child gets its parent's namespace, so these hold
#                 what the commands they run may need

SBASE     := third_party/sbase
SBASE_DIR := $(PORTS)/sbase
SBASE_OUT := $(PORTS_OUT)/sbase

SBASE_PLAIN := basename dirname echo false true yes seq printf sleep printenv tr uname whoami \
               logname hostname pwd tty date expr cal
SBASE_RO    := cat cmp comm cut du expand fold grep head join ls md5sum nl od paste \
               sha1sum sha256sum sha512sum tail test uniq unexpand wc which cksum cols dc pathchk \
               strings tsort uuencode
SBASE_RW    := cp mv rm rmdir mkdir touch tee split dd tar ed sed sort find xargs env time uudecode unlink
SBASE_PROGS := $(SBASE_PLAIN) $(SBASE_RO) $(SBASE_RW)

# The tools whose file is ours (OVERRIDES).
SBASE_OVR := $(basename $(shell awk '!/^\#/ && NF == 3 { print $$1 }' $(SBASE_DIR)/OVERRIDES))

SBASE_CFLAGS := -std=c99 -O2 -w -D_DEFAULT_SOURCE -D_NETBSD_SOURCE -D_BSD_SOURCE \
                -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -I$(SBASE) -MMD -MP

SBASE_UTF  := $(patsubst %.c,$(SBASE_OUT)/libutf/%.o,$(notdir $(wildcard $(SBASE)/libutf/*.c)))
SBASE_UTIL := $(patsubst %.c,$(SBASE_OUT)/libutil/%.o,$(notdir $(wildcard $(SBASE)/libutil/*.c)))
SBASE_LIBS := $(SBASE_OUT)/libutil.a $(SBASE_OUT)/libutf.a

$(eval $(call PORT_OVERRIDES_CHECK,sbase))
SBASE_CHECKED := $(PORTS_OUT)/sbase/overrides.ok

$(SBASE_OUT)/libutf/%.o: $(SBASE)/libutf/%.c | $(SYSROOT_HDRS) $(SYSROOT)/lib/libc.a $(SBASE_CHECKED)
	@mkdir -p $(dir $@)
	$(PORT_CC) $(SBASE_CFLAGS) -c $< -o $@
$(SBASE_OUT)/libutil/%.o: $(SBASE)/libutil/%.c | $(SYSROOT_HDRS) $(SYSROOT)/lib/libc.a $(SBASE_CHECKED)
	@mkdir -p $(dir $@)
	$(PORT_CC) $(SBASE_CFLAGS) -c $< -o $@
$(SBASE_OUT)/libutf.a: $(SBASE_UTF)
	rm -f $@
	$(AR) rcs $@ $^
$(SBASE_OUT)/libutil.a: $(SBASE_UTIL)
	rm -f $@
	$(AR) rcs $@ $^

# One program per class, not one per tool (upstream's own "sbase-box": each
# tool's main renamed <tool>_main and a table that picks one by the name
# the program was started as). Each static POSIX program is about 250 KB of
# C library and runtime whatever it does, so 80 programs would be 20 MB of
# boot image, which `update` has a limit on (UPDATE_FILE_MAX). The boot
# image names a box once for every tool in it (usr/bin/sort, usr/bin/cat,
# ...: tools/mkbootfs.py stores identical files once), and the shell starts
# a program as its bootfs name, so the tool is the basename of argv[0].
# `usr/bin/sbase-<class> <tool> args` works too.
#
# $(call SBASE_TOOL,<tool>): its object, main renamed <tool>_main.
define SBASE_TOOL
$(SBASE_OUT)/tool/$(1).o: $(if $(filter $(1),$(SBASE_OVR)),$(SBASE_DIR)/override,$(SBASE))/$(1).c \
                         | $(SYSROOT_HDRS) $(SYSROOT)/lib/libc.a $(SBASE_CHECKED)
	@mkdir -p $$(dir $$@)
	$(PORT_CC) $(SBASE_CFLAGS) -Dmain=$(subst -,_,$(1))_main -c $$< -o $$@
endef
$(foreach t,$(SBASE_PROGS),$(eval $(call SBASE_TOOL,$(t))))

# $(call SBASE_BOX,<class>,<tools>)
define SBASE_BOX
$(SBASE_OUT)/box-$(1).c: $(SBASE_DIR)/mkbox.sh $(SBASE_DIR)/sbase.mk
	@mkdir -p $$(dir $$@)
	sh $(SBASE_DIR)/mkbox.sh sbase-$(1) $(2) > $$@
$(SBASE_OUT)/box-$(1).o: $(SBASE_OUT)/box-$(1).c | $(SYSROOT_HDRS) $(SYSROOT)/lib/libc.a
	$(PORT_CC) $(SBASE_CFLAGS) -c $$< -o $$@
$(call PORT_PROGRAM,sbase,sbase-$(1),$(SBASE_OUT)/box-$(1).o $(foreach t,$(2),$(SBASE_OUT)/tool/$(t).o),$(SBASE_DIR)/wants-$(1),$(SBASE_LIBS),usr/bin/sbase-$(1))
BOOTFS_FILES += $(foreach t,$(2),usr/bin/$(t)=$(PORTS_OUT)/sbase/bin/sbase-$(1).bootfs)
endef
$(eval $(call SBASE_BOX,plain,$(SBASE_PLAIN)))
$(eval $(call SBASE_BOX,ro,$(SBASE_RO)))
$(eval $(call SBASE_BOX,rw,$(SBASE_RW)))
# The licence notice, in the image (MIT asks for it to travel with the program).
BOOTFS_FILES += share/licences/sbase=$(SBASE)/LICENSE
$(BOOTFS): $(SBASE)/LICENSE
$(eval $(call PORT_LISTS,sbase))

-include $(SBASE_UTF:.o=.d) $(SBASE_UTIL:.o=.d) $(SBASE_PROGS:%=$(SBASE_OUT)/tool/%.d)

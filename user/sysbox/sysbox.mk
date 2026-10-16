# Jam OS's own commands as programs (docs/M13-PLAN.md, "Jam commands as
# programs"): the shell's commands that talk to Jam OS services and have no
# POSIX counterpart (dmesg, free, net, usb, ...), also built as POSIX
# programs in the boot image's bin/, so a POSIX shell, a script or an ssh
# session can run them. The native shell keeps running its own copy
# in-process, and it is the same code: each program's objects are the
# command's file in user/services/shell/cmd/, compiled with jamos-cc as it
# is (-DSH_BOX), and the few shell library files those commands call
# (sh_kernel.c, sh_sysinfo.c, ...). user/sysbox/box_io.c is the shell's
# output, input and handle calls answered for a program.
#
# One program per privilege, not per command (a static POSIX program is
# 400 KB or so whatever it does, as for sbase: user/ports/sbase/sbase.mk):
# a box, started as a command's name, runs that command (mkbox.sh). The
# boot image names a box once for every command in it (bin/dmesg, ...:
# tools/mkbootfs.py stores identical files once). Each box's list is the
# least its commands need (wants-*; a program started by the shell is given
# exactly that, and tools/checkwants.py checks it):
#   sys-info    read-only kernel information: free uptime version lscpu ps
#               dmesg log           (right sysinfo, right klog)
#   sys-debug   the kernel's debug commands, their output taken from the
#               log: mem memmap pci iommu bench   (right debug, right klog)
#   sys-net     net ping host       (the network and the resolver)
#   sys-usb     usb                 (devmgr's query channel)
#   sys-ro      df hexdump(hd) crashlog   (every mount, read-only)
# What stays in the shell, and why, is in the notes (M13-PLAN.md).

SYSBOX     := user/sysbox
SYSBOX_OUT := $(PORTS_OUT)/sysbox
SH_DIR     := user/services/shell

# The shell's library files the commands call (box_io.c answers the rest of
# sh.h).
SYSBOX_LIB := sh_kernel sh_sysinfo sh_num sh_time sh_lookup sh_vfs sh_text

SYSBOX_INFO  := free uptime version lscpu ps dmesg log
SYSBOX_DEBUG := mem memmap pci iommu bench
SYSBOX_NET   := net ping host
SYSBOX_USB   := usb
SYSBOX_RO    := df hexdump hd=hexdump crashlog

SYSBOX_CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror -Wvla -DSH_BOX -include jamos/libjam.h \
                 -I$(POSIX)/include -Iuser/include -I$(BUILD)/uinc -Idrivers/include \
                 -iquote $(SH_DIR) -iquote $(SYSBOX) -MMD -MP
# Its flag stamp (the Makefile's FLAG_STAMP); jamos-cc itself adds flags.
FLAGS_sysbox  = $(TOOLID) $(SYSBOX_CFLAGS)
$(eval $(call FLAG_STAMP,sysbox,FLAGS_sysbox))
SYSBOX_FLAGS  := $(FLAGDIR)/sysbox tools/jamos-cc
SYSBOX_NEEDS  := | $(SYSROOT_HDRS) $(SYSROOT)/lib/libc.a $(UINC) $(SYSCALLS_OK) $(IDL_OK)

$(SYSBOX_OUT)/obj/cmd/%.o: $(SH_DIR)/cmd/%.c $(SYSBOX_FLAGS) $(SYSBOX_NEEDS)
	@mkdir -p $(dir $@)
	$(PORT_CC) $(SYSBOX_CFLAGS) -c $< -o $@
$(SYSBOX_OUT)/obj/lib/%.o: $(SH_DIR)/%.c $(SYSBOX_FLAGS) $(SYSBOX_NEEDS)
	@mkdir -p $(dir $@)
	$(PORT_CC) $(SYSBOX_CFLAGS) -c $< -o $@
$(SYSBOX_OUT)/obj/box_io.o: $(SYSBOX)/box_io.c $(SYSBOX)/box.h $(SYSBOX_FLAGS) $(SYSBOX_NEEDS)
	@mkdir -p $(dir $@)
	$(PORT_CC) $(SYSBOX_CFLAGS) -c $< -o $@

SYSBOX_LIB_OBJS := $(SYSBOX_LIB:%=$(SYSBOX_OUT)/obj/lib/%.o) $(SYSBOX_OUT)/obj/box_io.o
$(SYSBOX_OUT)/libsysbox.a: $(SYSBOX_LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# $(call SYSBOX_BOX,<group>,<commands>,root|plain): the box sys-<group> and a bootfs
# name bin/<command> for every command (<alias>=<command> for another name).
define SYSBOX_BOX
$(SYSBOX_OUT)/box-$(1).c: $(SYSBOX)/mkbox.sh $(SYSBOX)/sysbox.mk
	@mkdir -p $$(dir $$@)
	sh $(SYSBOX)/mkbox.sh sys-$(1) $(3) $(2) > $$@
$(SYSBOX_OUT)/box-$(1).o: $(SYSBOX_OUT)/box-$(1).c $(SYSBOX)/box.h $(SYSBOX_FLAGS) $(SYSBOX_NEEDS)
	$(PORT_CC) $(SYSBOX_CFLAGS) -c $$< -o $$@
$(call PORT_PROGRAM,sysbox,sys-$(1),$(SYSBOX_OUT)/box-$(1).o $(sort $(foreach c,$(2),$(SYSBOX_OUT)/obj/cmd/$(lastword $(subst =, ,$(c))).o)),$(SYSBOX)/wants-$(1),$(SYSBOX_OUT)/libsysbox.a,bin/sys-$(1))
BOOTFS_FILES += $(foreach c,$(2),bin/$(firstword $(subst =, ,$(c)))=$(SYSBOX_OUT)/bin/sys-$(1).bootfs)
endef
$(eval $(call SYSBOX_BOX,info,$(SYSBOX_INFO),root))
$(eval $(call SYSBOX_BOX,debug,$(SYSBOX_DEBUG),root))
$(eval $(call SYSBOX_BOX,net,$(SYSBOX_NET),plain))
$(eval $(call SYSBOX_BOX,usb,$(SYSBOX_USB),plain))
$(eval $(call SYSBOX_BOX,ro,$(SYSBOX_RO),plain))

# The boxes' lists, checked as any boot-image program's (the programs' own
# directory is the label tools/checkwants.py reads).
$(SYSBOX_OUT)/lists.ok: $(PORT_sysbox_FILES) tools/checkwants.py user/include/os.h
	python3 tools/checkwants.py $(foreach f,$(PORT_sysbox_FILES),$(SYSBOX)=$(f))
	touch $@
$(BOOTFS): $(SYSBOX_OUT)/lists.ok

-include $(wildcard $(SYSBOX_OUT)/obj/*.d $(SYSBOX_OUT)/obj/cmd/*.d $(SYSBOX_OUT)/obj/lib/*.d)

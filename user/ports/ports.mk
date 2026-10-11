# Ported programs (docs/M13-PLAN.md section 14), included at the end of the
# Makefile after user/posix/posix.mk: outside code built unmodified from
# third_party/<name>/ by the port's own fragment, user/ports/<name>/<name>.mk,
# with tools/jamos-cc against the sysroot, each program stripped, its list
# (<wants.h>) checked by tools/checkwants.py as any boot-image program's,
# and packed into bootfs. Where each port's programs go is the port's.
#
# A port's directory holds:
#   <name>.mk     the fragment: which programs, how its objects are built
#   wants-*       the lists (<wants.h>) its programs are linked with: what
#                 each one is given beyond a terminal (mounts, services)
#   OVERRIDES,    the upstream files a program forks in (or otherwise cannot
#   override/     build as released), each replaced by our file: the same
#                 format as musl's (user/posix/musl/OVERRIDES) and the same
#                 check (tools/checkoverrides.py): the build stops if
#                 upstream's file is no longer the one ours was written
#                 against. A port with none has no such files.
#
# Everything lands in $(BUILD)/ports/<name>/: obj/ (objects), bin/<program>
# (linked) and bin/<program>.bootfs (stripped, what bootfs packs).

PORTS     := user/ports
PORTS_OUT := $(BUILD)/ports
PORT_CC    = JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc

# $(call PORT_PROGRAM,<port>,<program>,<objects>,<wants file>,<libraries>,<bootfs name>)
# links <program> from the objects and libraries with the wants file as its
# list, strips it, and packs it as <bootfs name>. $(call PORT_LISTS,<port>),
# once the port's programs are all defined, checks their lists.
define PORT_PROGRAM
$(PORTS_OUT)/$(1)/bin/$(2): $(3) $(4) $(5) $(SYSROOT_LIBS) tools/jamos-cc
	@mkdir -p $$(dir $$@)
	$(PORT_CC) --jamos-wants=$(4) -o $$@ $(3) $(5)
$(PORTS_OUT)/$(1)/bin/$(2).bootfs: $(PORTS_OUT)/$(1)/bin/$(2)
	$$(STRIP) --strip-debug $$< -o $$@
PORT_$(1)_FILES += $(PORTS_OUT)/$(1)/bin/$(2).bootfs
$(BOOTFS): $(PORTS_OUT)/$(1)/bin/$(2).bootfs
BOOTFS_FILES += $(6)=$(PORTS_OUT)/$(1)/bin/$(2).bootfs
endef

# The lists of all of a port's programs, checked in one go (what the build
# approves: tools/checkwants.py, as for any boot-image program).
define PORT_LISTS
$(PORTS_OUT)/$(1)/lists.ok: $(PORT_$(1)_FILES) tools/checkwants.py user/include/os.h
	python3 tools/checkwants.py $(foreach f,$(PORT_$(1)_FILES),$(PORTS)/$(1)=$(f))
	touch $$@
$(BOOTFS): $(PORTS_OUT)/$(1)/lists.ok
endef

# The overrides' check, for a port whose OVERRIDES file is
# $(PORTS)/<port>/OVERRIDES and whose upstream is third_party/<port>:
# $(PORTS_OUT)/<port>/overrides.ok, which every object waits for.
define PORT_OVERRIDES_CHECK
$(PORTS_OUT)/$(1)/overrides.ok: $(PORTS)/$(1)/OVERRIDES tools/checkoverrides.py \
        $(addprefix third_party/$(1)/,$(shell awk '!/^\#/ && NF == 3 { print $$1 }' $(PORTS)/$(1)/OVERRIDES)) \
        $(shell awk '!/^\#/ && NF == 3 { print $$3 }' $(PORTS)/$(1)/OVERRIDES)
	@mkdir -p $$(dir $$@)
	python3 tools/checkoverrides.py $(PORTS)/$(1)/OVERRIDES third_party/$(1)
	touch $$@
.PHONY: $(1)-check
$(1)-check:
	python3 tools/checkoverrides.py $(PORTS)/$(1)/OVERRIDES third_party/$(1)
	python3 tools/checkoverrides.py --selftest $(PORTS)/$(1)/OVERRIDES third_party/$(1)
check: $(1)-check
endef

include $(PORTS)/sbase/sbase.mk
include $(PORTS)/lua/lua.mk

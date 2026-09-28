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
# boot menu's "M4 tests" entry needs the default KTESTS=1.
KTESTS  ?= 1
BUILD   := $(if $(filter 0,$(KTESTS)),build/noktests,build)
KERNEL  := $(BUILD)/jamos.elf
IMAGE   := $(BUILD)/jamos.img
BOOTFS  := $(BUILD)/bootfs.img
IMAGE_MIB := 64

LIMINE  := third_party/limine
OVMF_DIR ?= $(shell brew --prefix qemu 2>/dev/null)/share/qemu

CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
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

.PHONY: all image run debug clean font usb syscalls

all: $(KERNEL) $(BOOTFS)

# System call glue. tools/gensyscalls.py turns abi/syscalls.def into the
# numbers, the kernel dispatch table and the user wrappers. The output is
# committed (so it can be read and grepped like any source); every build
# first checks that it matches the .def, so a table change can't be half
# applied. `make syscalls` regenerates it.
SYSCALL_GEN := kernel/include/jam/syscall_nums.h kernel/include/jam/syscall_impl.h \
               kernel/abi/syscall_table.c kernel/abi/syscall_weak.c \
               user/lib/syscalls.S user/include/jam_syscalls.h
SYSCALLS_OK := $(BUILD)/syscalls.ok

$(SYSCALLS_OK): abi/syscalls.def tools/gensyscalls.py kernel/include/jam/abi.h $(SYSCALL_GEN)
	@mkdir -p $(BUILD)
	python3 tools/gensyscalls.py check
	@touch $@

syscalls:
	python3 tools/gensyscalls.py gen

$(OBJS): | $(SYSCALLS_OK)

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
USER_CFLAGS := -std=gnu17 -O2 -g -Wall -Wextra -Werror \
               -ffreestanding -fno-stack-protector -fno-stack-check \
               -fno-PIC -fno-pie -fno-omit-frame-pointer -fno-lto \
               -fno-asynchronous-unwind-tables -m64 -march=x86-64 -mcmodel=small \
               -Iuser/include -I$(BUILD)/uinc -MMD -MP
USER_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T user/linker.ld
LIBGCC      := $(shell $(CC) -print-libgcc-file-name)
UINC_HDRS   := abi.h bootfs.h startup.h status.h syscall_nums.h
UINC        := $(UINC_HDRS:%=$(BUILD)/uinc/jam/%)
USER_PROGS  := init utest
UOBJ        := $(BUILD)/uobj
LIBOS_SRCS  := $(filter-out user/lib/crt0.S,$(wildcard user/lib/*.c user/lib/*.S))
LIBOS_OBJS  := $(LIBOS_SRCS:%=$(UOBJ)/%.o)
USER_OBJS   := $(LIBOS_OBJS) $(UOBJ)/user/lib/crt0.S.o \
               $(foreach p,$(USER_PROGS),$(patsubst %,$(UOBJ)/%.o,$(wildcard user/$(p)/*.c)))

.SECONDARY: $(UINC)
$(BUILD)/uinc/jam/%.h: kernel/include/jam/%.h
	@mkdir -p $(dir $@)
	cp $< $@

$(UOBJ)/%.c.o: %.c | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UOBJ)/%.S.o: %.S | $(UINC) $(SYSCALLS_OK)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UOBJ)/libos.a: $(LIBOS_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# $(BUILD)/user/<prog> keeps its debug info (for gdb); bootfs gets a copy
# without it ($(BUILD)/user/<prog>.bootfs), symbols kept for backtraces.
define USER_PROG
$(BUILD)/user/$(1): $(UOBJ)/user/lib/crt0.S.o $(patsubst %,$(UOBJ)/%.o,$(wildcard user/$(1)/*.c)) \
                    $(UOBJ)/libos.a user/linker.ld
	@mkdir -p $$(dir $$@)
	$(LD) $(USER_LDFLAGS) $(UOBJ)/user/lib/crt0.S.o \
	    $(patsubst %,$(UOBJ)/%.o,$(wildcard user/$(1)/*.c)) $(UOBJ)/libos.a $(LIBGCC) -o $$@

$(BUILD)/user/$(1).bootfs: $(BUILD)/user/$(1)
	$(STRIP) --strip-debug $$< -o $$@
endef
$(foreach p,$(USER_PROGS),$(eval $(call USER_PROG,$(p))))

# bootfs: the files init and the tests need before USB and FAT32 work,
# loaded by Limine as a module (boot/limine.conf: module_path).
BOOTFS_FILES := $(foreach p,$(USER_PROGS),bin/$(p)=$(BUILD)/user/$(p).bootfs) init.cfg=boot/init.cfg

$(BOOTFS): $(USER_PROGS:%=$(BUILD)/user/%.bootfs) boot/init.cfg tools/mkbootfs.py
	python3 tools/mkbootfs.py $@ $(BOOTFS_FILES)

image: $(IMAGE)

$(IMAGE): $(KERNEL) $(BOOTFS) boot/limine.conf tools/mkimage.py
	python3 tools/mkimage.py $@ $(IMAGE_MIB)
	mformat -i $@@@1M -F -v JAMOS ::
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
    -netdev user,id=net0 -device e1000e,netdev=net0 \
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

font:
	python3 tools/bdf2c.py third_party/spleen/spleen-8x16.bdf kernel/dev/font_8x16.c

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(USER_OBJS:.o=.d)

# Jam OS build.  `make` builds the kernel, `make image` the bootable USB
# image, `make run` boots it in QEMU (q35 + UEFI + xHCI USB boot, like a PC).

CROSS   ?= x86_64-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJDUMP := $(CROSS)objdump

BUILD   := build
KERNEL  := $(BUILD)/jamos.elf
IMAGE   := $(BUILD)/jamos.img
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

C_SRCS := $(shell find kernel -name '*.c')
S_SRCS := $(shell find kernel -name '*.S')
OBJS   := $(C_SRCS:%.c=$(BUILD)/%.o) $(S_SRCS:%.S=$(BUILD)/%.S.o)

.PHONY: all image run run-panic debug clean font usb

all: $(KERNEL)

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

image: $(IMAGE)

$(IMAGE): $(KERNEL) boot/limine.conf tools/mkimage.py
	python3 tools/mkimage.py $@ $(IMAGE_MIB)
	mformat -i $@@@1M -F -v JAMOS ::
	mmd -i $@@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@@@1M $(LIMINE)/BOOTX64.EFI ::/EFI/BOOT/
	mcopy -i $@@@1M boot/limine.conf ::/boot/limine/
	mcopy -i $@@@1M $(KERNEL) ::/boot/

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

# Same, but wait for gdb on :1234 (`make gdb` in another terminal).
debug: $(IMAGE) $(BUILD)/ovmf-vars.fd
	qemu-system-x86_64 $(QEMU_FLAGS) -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

# Write the image to a USB stick. Refuses anything that isn't external.
usb: $(IMAGE)
	tools/write-usb.sh $(IMAGE) $(DEV)

font:
	python3 tools/bdf2c.py third_party/spleen/spleen-8x16.bdf kernel/dev/font_8x16.c

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

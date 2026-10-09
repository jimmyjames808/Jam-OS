# Sourced by the QEMU test scripts: what differs between the machines that
# run them. The firmware: Homebrew's QEMU on the Mac, Ubuntu's ovmf
# package elsewhere (the PC's WSL, docs/TESTING.md "The PC as a test
# host"). The accelerator: KVM where /dev/kvm is ours, else TCG (the Mac
# emulates x86); QEMU_ACCEL overrides. KVM's interrupt remapping (QEMU's
# intel-iommu with intremap=on) needs the split irqchip.
if command -v brew > /dev/null 2>&1; then
    ovmf=$(brew --prefix qemu)/share/qemu
    ovmf_code=$ovmf/edk2-x86_64-code.fd ovmf_vars=$ovmf/edk2-i386-vars.fd
else
    ovmf_code=/usr/share/OVMF/OVMF_CODE_4M.fd ovmf_vars=/usr/share/OVMF/OVMF_VARS_4M.fd
fi
accel=${QEMU_ACCEL:-}
if [ -z "$accel" ]; then
    if [ -w /dev/kvm ]; then accel=kvm; else accel=tcg; fi
fi
machine=q35
if [ "$accel" = kvm ]; then machine=q35,kernel-irqchip=split; fi   # (no && : set -e)

#!/usr/bin/env bash
# Run the nginx sample in QEMU with user-mode networking. This is a
# host check with no cloud calls. It boots the same GCP-style disk
# layout that scripts/gcp_build_image.sh produces (FAT32 ESP, BIOS and
# UEFI GRUB, kernel plus initrd.cpio as a multiboot module).
#
# Usage: ./scripts/qemu_local.sh [run_seconds] [host_port] [disk]
# Then in another shell: ./scripts/smoke_test.sh 127.0.0.1:18080
set -euo pipefail

SAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "${SAMPLE_DIR}/../.." && pwd)"
DURATION="${1:-120}"
HOST_PORT="${2:-18080}"
DISK="${3:-${SAMPLE_DIR}/build/disk.raw}"
LOG="${SAMPLE_DIR}/build/qemu_local.log"

if [ ! -f "${DISK}" ]; then
    echo "[INFO] Disk image not found: ${DISK}"
    echo "[INFO] Building it locally (no cloud calls)..."
    # The local run id keeps the image-name validation happy. The
    # static netdev.ip matches the QEMU user-mode network. The leading
    # 'unikraft' token is the placeholder kernel name ukboot hides from
    # its parameter scan, and the ' --' separator keeps the boot
    # argument away from nginx, exactly as in the cloud run.
    UKG_RUN=ukg-local-qemu UKG_IMAGE_PUSH=0 \
        "${ROOT_DIR}/scripts/gcp_build_image.sh" "${SAMPLE_DIR}" \
        "ukg-local-qemu-img" "unikraft netdev.ip=10.0.2.15/24:10.0.2.2 --" \
        "${SAMPLE_DIR}/initrd.cpio"
fi

KVM_OPT=()
if [ -w /dev/kvm ]; then
    KVM_OPT=(-enable-kvm -cpu host)
else
    echo "[WARN] /dev/kvm not available. Running under TCG: boot is slow."
fi

echo "[INFO] Starting QEMU for ${DURATION}s. Host port ${HOST_PORT} forwards to guest port 80."
echo "[INFO] Console log: ${LOG}"

timeout "${DURATION}" qemu-system-x86_64 \
    -machine q35 \
    "${KVM_OPT[@]}" \
    -smp 1 \
    -m 512M \
    -display none \
    -no-reboot \
    -serial file:"${LOG}" \
    -drive file="${DISK}",format=raw,if=ide,index=0 \
    -netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:80 \
    -device virtio-net-pci,netdev=net0 \
    || true

echo "[done] QEMU run finished. Console log: ${LOG}"

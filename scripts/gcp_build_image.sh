#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build a bootable raw disk image for Google Cloud and register a custom image.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -f "${ROOT_DIR}/.env" ]; then
    # shellcheck source=/dev/null
    source "${ROOT_DIR}/.env"
fi

GCP_PROJECT="${GCP_PROJECT:-$(gcloud config get-value project 2>/dev/null || true)}"
GCP_ZONE="${GCP_ZONE:-$(gcloud config get-value compute/zone 2>/dev/null || true)}"
GCP_BUCKET="${GCP_BUCKET:-}"
UKG_RUN="${UKG_RUN:-ukg-$(date +%m%d-%H%M)}"
APP_DIR="${1:-${ROOT_DIR}/samples/helloworld}"
BUILD_DIR="${APP_DIR}/build"
DISK_RAW="${BUILD_DIR}/disk.raw"
IMAGE_NAME="${2:-${UKG_RUN}-img}"
CMDLINE_ARGS="${3:-}"

if [ -z "${GCP_PROJECT}" ]; then
    echo "[ERROR] GCP_PROJECT is not set." >&2
    exit 1
fi
if [ -z "${GCP_BUCKET}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    exit 1
fi

echo "[INFO] Run ID: ${UKG_RUN}"
echo "[INFO] Project: ${GCP_PROJECT}"
echo "[INFO] Bucket: ${GCP_BUCKET}"
echo "[INFO] Building application in ${APP_DIR}..."

# Build unikernel if binary does not exist
if [ ! -f "${BUILD_DIR}/.config" ]; then
    make -C "${APP_DIR}" defconfig
fi
make -C "${APP_DIR}" -j"$(nproc)"

KERNEL_BIN="$(find "${BUILD_DIR}" -maxdepth 1 -type f -name "*_qemu-x86_64" | head -n 1)"
if [ -z "${KERNEL_BIN}" ] || [ ! -f "${KERNEL_BIN}" ]; then
    echo "[ERROR] Kernel binary not found in ${BUILD_DIR}." >&2
    exit 1
fi

echo "[INFO] Found kernel binary: ${KERNEL_BIN}"
echo "[INFO] Preparing disk layout..."

STAGING_DIR="${BUILD_DIR}/staging"
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}/boot/grub/i386-pc"

cp "${KERNEL_BIN}" "${STAGING_DIR}/boot/unikraft.elf"
cp /usr/lib/grub/i386-pc/*.mod "${STAGING_DIR}/boot/grub/i386-pc/" || true
cp /usr/lib/grub/i386-pc/*.lst "${STAGING_DIR}/boot/grub/i386-pc/" || true

cat << GRUB_CFG > "${STAGING_DIR}/boot/grub/grub.cfg"
set default=0
set timeout=1

serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
terminal_input serial
terminal_output serial

menuentry 'unikraft' {
    multiboot /boot/unikraft.elf ${CMDLINE_ARGS}
    boot
}
GRUB_CFG

echo "[INFO] Creating ext4 filesystem..."
PART_IMG="${BUILD_DIR}/part1.img"
rm -f "${PART_IMG}"
mkfs.ext4 -F -q -L "ukgboot" -d "${STAGING_DIR}" "${PART_IMG}" 63M

echo "[INFO] Creating 10 GiB raw disk image..."
rm -f "${DISK_RAW}"
truncate -s 10G "${DISK_RAW}"

parted -s "${DISK_RAW}" mklabel msdos
parted -s "${DISK_RAW}" mkpart primary ext4 1MiB 64MiB
parted -s "${DISK_RAW}" set 1 boot on

dd if="${PART_IMG}" of="${DISK_RAW}" bs=1M seek=1 conv=notrunc status=none

echo "[INFO] Installing GRUB bootloader..."
CORE_IMG="${BUILD_DIR}/core.img"
grub-mkimage -O i386-pc -o "${CORE_IMG}" -p "(hd0,msdos1)/boot/grub" \
    biosdisk part_msdos ext2 multiboot normal configfile serial terminfo
dd if=/usr/lib/grub/i386-pc/boot.img of="${DISK_RAW}" bs=446 count=1 conv=notrunc status=none
dd if="${CORE_IMG}" of="${DISK_RAW}" bs=512 seek=1 conv=notrunc status=none

echo "[INFO] Packaging disk image as tar.gz for GCP..."
DISK_TAR="${BUILD_DIR}/${IMAGE_NAME}-disk.raw.tar.gz"
rm -f "${DISK_TAR}"
(cd "${BUILD_DIR}" && tar -Szcf "${IMAGE_NAME}-disk.raw.tar.gz" "disk.raw")

GCS_TARGET="gs://${GCP_BUCKET}/${IMAGE_NAME}-disk.tar.gz"
echo "[INFO] Uploading to ${GCS_TARGET}..."
if command -v gcloud &>/dev/null; then
    gcloud storage cp "${DISK_TAR}" "${GCS_TARGET}"
else
    gsutil cp "${DISK_TAR}" "${GCS_TARGET}"
fi

echo "[INFO] Registering custom GCP image ${IMAGE_NAME}..."
# Delete image if exists (rerunnable requirement)
if gcloud compute images describe "${IMAGE_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Image ${IMAGE_NAME} already exists. Deleting..."
    gcloud compute images delete "${IMAGE_NAME}" --project="${GCP_PROJECT}" --quiet
fi

gcloud compute images create "${IMAGE_NAME}" \
    --project="${GCP_PROJECT}" \
    --source-uri="${GCS_TARGET}" \
    --labels="run=${UKG_RUN}"

echo "[SUCCESS] Created GCP image: ${IMAGE_NAME}"
echo "IMAGE_NAME=${IMAGE_NAME}"
echo "UKG_RUN=${UKG_RUN}"

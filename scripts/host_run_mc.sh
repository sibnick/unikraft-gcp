#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Host-side multi-core boot gate for httpreply-mc (ticket dd4a8c64ea).
# No cloud calls. Boots the sample N times locally and checks that all
# vCPU cores come online each time.
#
# Verdict rules, learned from the 2026-11-08 root-cause work:
# - A silent reboot under QEMU re-runs the BIOS inside one serial
#   capture. Two "SeaBIOS (version" lines mean the machine reset.
# - QEMU multi-threaded TCG can replay a stale translation of the
#   boot-loader code at guest 0x1000 when the kernel copies its AP
#   trampoline over that page. The AP then faults and the machine
#   resets. KVM has no emulated translation cache, so this runner
#   prefers KVM. Under forced TCG it uses thread=single, which has
#   no cross-thread cache race.
# - Under TCG the guest TSC is not synced across vCPUs. The bounded
#   AP-online wait in patch 0003 can then print GATE_FAIL AP lines
#   even though every AP comes online shortly after. The runner
#   treats those lines as soft in TCG mode and checks the final
#   worker count instead. Under KVM those lines are hard failures.
#
# Usage: host_run_mc.sh [sample-dir] [boots] [boot-timeout-secs]
# Env:   UKG_SMP=4            vCPU count; must match the sample CPU_MAXCOUNT.
#        UKG_HOST_FORCE_TCG=1 Skip KVM detection and use single-thread TCG.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=./lib/gcp_lib.sh
source "${SCRIPT_DIR}/lib/gcp_lib.sh"

SAMPLE="${1:-${ROOT_DIR}/samples/httpreply-mc}"
BOOTS="${2:-8}"
BOOT_TIMEOUT="${3:-180}"
SMP="${UKG_SMP:-4}"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "[ERROR] qemu-system-x86_64 not found." >&2
    exit 2
fi

NAME="$(basename "${SAMPLE}")"
KERNEL="${SAMPLE}/build/${NAME}_qemu-x86_64"
if [ ! -f "${KERNEL}" ]; then
    echo "[ERROR] Kernel not built: ${KERNEL}" >&2
    echo "[ERROR] Run: make -C ${SAMPLE} defconfig && make -C ${SAMPLE} -j\$(nproc)" >&2
    exit 2
fi

# TCG needs +rdrand. Plain qemu64 has no CPU randomness, and the lwIP
# stack crashes when the CSPRNG has no seed source. The 2026-11 A/B
# logs used the same qemu64+rdrand model.
ACCEL=(-accel "tcg,thread=single" -cpu "qemu64,+rdrand")
ACCEL_NAME="tcg-single"
if [ "${UKG_HOST_FORCE_TCG:-0}" != "1" ] && [ -w /dev/kvm ]; then
    ACCEL=(-enable-kvm -cpu host)
    ACCEL_NAME="kvm"
else
    echo "[WARN] Using single-thread TCG. Boot is slow and the AP-online" \
         "GATE_FAIL lines are soft in this mode."
fi

LOG_DIR="$(ukg_log_dir)/host-smp-$(date -u +%m%d-%H%M%S)" || {
    echo "[ERROR] Cannot create log directory." >&2
    exit 1
}
mkdir -p "${LOG_DIR}"
chmod 700 "${LOG_DIR}"

echo "[INFO] Sample=${NAME} boots=${BOOTS} smp=${SMP} accel=${ACCEL_NAME}"
echo "[INFO] Logs: ${LOG_DIR}"

fails=0
for i in $(seq 1 "${BOOTS}"); do
    serial="${LOG_DIR}/boot${i}.log"
    timeout "${BOOT_TIMEOUT}" qemu-system-x86_64 \
        "${ACCEL[@]}" -smp "${SMP}" -m 512M \
        -device virtio-net-pci -nographic \
        -kernel "${KERNEL}" >"${serial}" 2>"${LOG_DIR}/boot${i}.err" &
    pid=$!
    status=HANG
    while kill -0 "${pid}" 2>/dev/null; do
        sleep 2
        if [ "$(grep -a -c 'SeaBIOS (version' "${serial}" 2>/dev/null)" -ge 2 ]; then
            status=REBOOT
            break
        fi
        if grep -aq "detected ${SMP} worker cores" "${serial}" 2>/dev/null; then
            status=OK
            break
        fi
    done
    kill -TERM "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true

    if [ "${status}" = OK ] && grep -aq "did not come online" "${serial}"; then
        if [ "${ACCEL_NAME}" = kvm ]; then
            status=AP_TIMEOUT
        else
            echo "[WARN] boot ${i}: soft AP-online timeout under TCG;" \
                 "all ${SMP} workers came up, so the boot passed."
        fi
    fi

    if [ "${status}" != OK ]; then
        fails=$((fails + 1))
        echo "[FAIL] boot ${i}: ${status}. See ${serial}"
    else
        echo "[OK] boot ${i}: ${SMP} workers online"
    fi
done

if [ "${fails}" -ne 0 ]; then
    echo "[ERROR] ${fails}/${BOOTS} boots failed. Logs: ${LOG_DIR}" >&2
    exit 1
fi
echo "[DONE] Host MC gate passed: ${BOOTS}/${BOOTS} boots with ${SMP} workers."
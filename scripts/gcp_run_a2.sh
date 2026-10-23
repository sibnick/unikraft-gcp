#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Launch Unikraft VM on Google Cloud and capture serial console evidence for Phase A2.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -f "${ROOT_DIR}/.env" ]; then
    # shellcheck source=/dev/null
    source "${ROOT_DIR}/.env"
fi

GCP_PROJECT="${GCP_PROJECT:-$(gcloud config get-value project 2>/dev/null || true)}"
GCP_ZONE="${GCP_ZONE:-$(gcloud config get-value compute/zone 2>/dev/null || true)}"
UKG_RUN="${1:-${UKG_RUN:-ukg-$(date +%m%d-%H%M)}}"
IMAGE_NAME="${2:-${UKG_RUN}-img}"
INSTANCE_NAME="${UKG_RUN}-srv"
SERIAL_LOG="/tmp/${UKG_RUN}-serial.log"

if [ -z "${GCP_PROJECT}" ]; then
    echo "[ERROR] GCP_PROJECT is not set." >&2
    exit 1
fi
if [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_ZONE is not set." >&2
    exit 1
fi

echo "=================================================="
echo "Phase A2 VirtIO Bind Run"
echo "Project:  ${GCP_PROJECT}"
echo "Zone:     ${GCP_ZONE}"
echo "Run ID:   ${UKG_RUN}"
echo "Image:    ${IMAGE_NAME}"
echo "Instance: ${INSTANCE_NAME}"
echo "=================================================="

# Delete instance if exists (rerunnable requirement)
if gcloud compute instances describe "${INSTANCE_NAME}" --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Instance ${INSTANCE_NAME} already exists. Deleting..."
    gcloud compute instances delete "${INSTANCE_NAME}" --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" --quiet
fi

echo "[INFO] Creating instance ${INSTANCE_NAME} (n1-standard-1)..."
gcloud compute instances create "${INSTANCE_NAME}" \
    --zone="${GCP_ZONE}" \
    --project="${GCP_PROJECT}" \
    --machine-type=n1-standard-1 \
    --image="${IMAGE_NAME}" \
    --tags=ukg-vm \
    --labels="run=${UKG_RUN}"

echo "[INFO] Polling serial console output for GATE_VIRTIO_PASS..."
VIRTIO_PASS=0
for i in $(seq 1 40); do
    sleep 3
    gcloud compute instances get-serial-port-output "${INSTANCE_NAME}" \
        --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" > "${SERIAL_LOG}" 2>&1 || true

    if grep -q "GATE_VIRTIO_PASS" "${SERIAL_LOG}"; then
        echo "[SUCCESS] GATE_VIRTIO_PASS found after attempt ${i}!"
        VIRTIO_PASS=1
        break
    fi
    echo -n "."
done
echo ""

echo "--- Serial Console Output ---"
cat "${SERIAL_LOG}"
echo "-----------------------------"

if [ "${VIRTIO_PASS}" -eq 1 ]; then
    echo "[PASS] Gate A2 completed successfully: GATE_VIRTIO_PASS verified."
    exit 0
else
    echo "[FAIL] GATE_VIRTIO_PASS not found in serial console output!" >&2
    echo "Run teardown manually with: ./scripts/gcp_teardown.sh ${UKG_RUN}" >&2
    exit 1
fi

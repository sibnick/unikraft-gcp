#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build UEFI GVNIC image, launch third-generation VM (c3-standard-4),
# and capture serial console evidence for Phase B1 (GATE_GVE_ADMINQ_PASS).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=./lib/gcp_lib.sh
source "${SCRIPT_DIR}/lib/gcp_lib.sh"

if [ -f "${ROOT_DIR}/.env" ]; then
    # shellcheck source=/dev/null
    source "${ROOT_DIR}/.env"
fi

GCP_PROJECT="${GCP_PROJECT:-$(gcloud config get-value project 2>/dev/null || true)}"
GCP_ZONE="${GCP_ZONE:-$(gcloud config get-value compute/zone 2>/dev/null || true)}"
UKG_RUN="${1:-${UKG_RUN:-$(ukg_default_run_id)}}"
ukg_validate_run_id "${UKG_RUN}"
export UKG_RUN
IMAGE_NAME="${2:-${UKG_RUN}-img}"
ukg_validate_image_name "${IMAGE_NAME}" "${UKG_RUN}"
INSTANCE_NAME="${UKG_RUN}-srv"
MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
SERIAL_LOG="$(ukg_mk_serial_log "${UKG_RUN}")"

# Evidence contract wiring. The teardown trap stores this log as
# reports/<run-id>-serial.log with fossil uv after the leak audit,
# and prints the exact fossil ticket change command for the mapped
# ticket. The default is the phase ticket from the validation skill.
# Override UKG_TICKET for a regression run driven by another ticket.
# Ticket c16353731a.
UKG_GATE="B1"
UKG_EVIDENCE_LOG="${SERIAL_LOG}"
UKG_TICKET="${UKG_TICKET:-e64aa83ff1}"

if [ -z "${GCP_PROJECT}" ]; then
    echo "[ERROR] GCP_PROJECT is not set." >&2
    exit 1
fi
if [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_ZONE is not set." >&2
    exit 1
fi

echo "=================================================="
echo "Phase B1 gVNIC Admin Queue Probe Run"
echo "Project:  ${GCP_PROJECT}"
echo "Zone:     ${GCP_ZONE}"
echo "Machine:  ${MACHINE_TYPE}"
echo "Run ID:   ${UKG_RUN}"
echo "Image:    ${IMAGE_NAME}"
echo "Instance: ${INSTANCE_NAME}"
echo "Log:      ${SERIAL_LOG}"
echo "=================================================="

# Install the teardown trap before the first resource create. The
# image build below already creates billable objects. Any later
# error, Ctrl-C, or killed terminal still tears this run down.
# Export KEEP_RES=1 to keep resources and print the teardown command.
ukg_install_teardown_trap

# Build custom image if it does not exist yet
if ! gcloud compute images describe "${IMAGE_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Building and uploading custom UEFI GVNIC image ${IMAGE_NAME}..."
    "${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/helloworld" "${IMAGE_NAME}"
fi

# Delete instance if exists (rerunnable requirement)
if gcloud compute instances describe "${INSTANCE_NAME}" --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Instance ${INSTANCE_NAME} already exists. Deleting..."
    gcloud compute instances delete "${INSTANCE_NAME}" --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" --quiet
fi

echo "[INFO] Creating instance ${INSTANCE_NAME} (${MACHINE_TYPE})..."
# The gate reads evidence from the serial console. The VM needs no
# billable external IPv4 address, so the GVNIC gets no address.
gcloud compute instances create "${INSTANCE_NAME}" \
    --zone="${GCP_ZONE}" \
    --project="${GCP_PROJECT}" \
    --machine-type="${MACHINE_TYPE}" \
    --boot-disk-type=pd-balanced \
    --network-interface=nic-type=GVNIC,no-address \
    --image="${IMAGE_NAME}" \
    --tags=ukg-vm \
    --labels="run=${UKG_RUN}"
echo "[INFO] Polling serial console output for GATE_GVE_ADMINQ_PASS..."
ADMINQ_PASS=0
for i in $(seq 1 40); do
    sleep 3
    gcloud compute instances get-serial-port-output "${INSTANCE_NAME}" \
        --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" > "${SERIAL_LOG}" 2>&1 || true

    if grep -q "GATE_GVE_ADMINQ_PASS" "${SERIAL_LOG}"; then
        echo "[SUCCESS] GATE_GVE_ADMINQ_PASS found after attempt ${i}!"
        ADMINQ_PASS=1
        break
    fi
    if grep -q "GATE_FAIL" "${SERIAL_LOG}"; then
        echo ""
        echo "[FAIL] GATE_FAIL found after attempt ${i}! Stop polling." >&2
        break
    fi
    echo -n "."
done
echo ""

# Final capture before the verdict. A failure marker can appear after
# the pass marker, so the last serial state decides. Ticket 06ea1706a7.
if [ "${ADMINQ_PASS}" -eq 1 ]; then
    sleep 3
    gcloud compute instances get-serial-port-output "${INSTANCE_NAME}" \
        --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" > "${SERIAL_LOG}" 2>&1 || true
fi

echo "--- Serial Console Output ---"
cat "${SERIAL_LOG}"
echo "-----------------------------"

if [ "${ADMINQ_PASS}" -eq 1 ] && ! grep -q "GATE_FAIL" "${SERIAL_LOG}"; then
    echo "[PASS] Gate B1 completed: GATE_GVE_ADMINQ_PASS verified."
    exit 0
else
    echo "[FAIL] GATE_GVE_ADMINQ_PASS not verified: marker missing or a GATE_FAIL present!" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

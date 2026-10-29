#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build UEFI GVNIC image from samples/networkload, launch third-generation VM
# (c3-standard-4), and capture serial console evidence for Phase B2 (GATE_GVE_PASS).
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

# Every run uploads a disk image to Cloud Storage. Require the
# bucket before any billable call. Ticket afc0a00b06.
if [ -z "${GCP_BUCKET:-}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    echo "[ERROR] Every run uploads a disk image to Cloud Storage." >&2
    echo "[ERROR] Set GCP_BUCKET in .env." >&2
    exit 1
fi
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
UKG_GATE="B2"
UKG_EVIDENCE_LOG="${SERIAL_LOG}"
UKG_TICKET="${UKG_TICKET:-a1c1337265}"

if [ -z "${GCP_PROJECT}" ]; then
    echo "[ERROR] GCP_PROJECT is not set." >&2
    exit 1
fi
if [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_ZONE is not set." >&2
    exit 1
fi

echo "=================================================="
echo "Phase B2 gVNIC Queue Creation & uk_netdev Run"
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
    "${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/networkload" "${IMAGE_NAME}"
fi

# The zone fallback list comes from ukg_zone_list. Override it with
# UKG_ZONES in .env. Ticket 4347c2a8e6 N7.
ZONE_LIST="$(ukg_zone_list)"

# Delete instance if exists in any candidate zone (rerunnable requirement)
for z in ${ZONE_LIST}; do
    if gcloud compute instances describe "${INSTANCE_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
        echo "[INFO] Instance ${INSTANCE_NAME} already exists in ${z}. Deleting..."
        gcloud compute instances delete "${INSTANCE_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet
    fi
done

CREATED_ZONE=""
for z in ${ZONE_LIST}; do
    echo "[INFO] Creating instance ${INSTANCE_NAME} (${MACHINE_TYPE}) in zone ${z}..."
    # The gate reads evidence from the serial console. The VM needs
    # no billable external IPv4 address, so the GVNIC gets no address.
    if gcloud compute instances create "${INSTANCE_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --network-interface=nic-type=GVNIC,no-address \
        --image="${IMAGE_NAME}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"; then
        CREATED_ZONE="${z}"
        GCP_ZONE="${z}"
        break
    else
        # A create can report failure after the instance actually
        # exists. Re-describe before trying the next zone, so the
        # orphan is deleted here and not left billing in this zone.
        echo "[WARN] Create ${INSTANCE_NAME} failed in ${z}. Checking for an orphan instance..."
        if gcloud compute instances describe "${INSTANCE_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
            echo "[WARN] Orphan instance ${INSTANCE_NAME} found in ${z}. Deleting..."
            gcloud compute instances delete "${INSTANCE_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                || echo "[WARN] Orphan delete failed. Teardown later with: ${SCRIPT_DIR}/gcp_teardown.sh ${UKG_RUN}" >&2
        fi
        echo "[WARN] Failed to create ${INSTANCE_NAME} in ${z}, trying next zone..."
    fi
done

if [ -z "${CREATED_ZONE}" ]; then
    echo "[ERROR] Could not create ${INSTANCE_NAME} in any candidate zone." >&2
    exit 1
fi

echo "[INFO] Polling serial console output for GATE_GVE_PASS..."
GVE_PASS=0
for i in $(seq 1 40); do
    sleep 3
    ukg_serial_capture "${INSTANCE_NAME}" "${SERIAL_LOG}" || true

    if grep -q "GATE_GVE_PASS" "${SERIAL_LOG}"; then
        echo "[SUCCESS] GATE_GVE_PASS found after attempt ${i}!"
        GVE_PASS=1
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
if [ "${GVE_PASS}" -eq 1 ]; then
    sleep 3
    ukg_serial_capture "${INSTANCE_NAME}" "${SERIAL_LOG}" || true
fi

echo "--- Serial Console Output ---"
cat "${SERIAL_LOG}"
echo "-----------------------------"

if [ "${GVE_PASS}" -eq 1 ] && ! grep -q "GATE_FAIL" "${SERIAL_LOG}"; then
    echo "[PASS] Gate B2 completed: GATE_GVE_PASS verified."
    exit 0
else
    echo "[FAIL] GATE_GVE_PASS not verified: marker missing or a GATE_FAIL present!" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Launch a two-instance VM pair on Google Cloud for traffic verification (Phase A3).
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
SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "Google Cloud Pair Run"
echo "Project:  ${GCP_PROJECT}"
echo "Zone:     ${GCP_ZONE}"
echo "Run ID:   ${UKG_RUN}"
echo "Image:    ${IMAGE_NAME}"
echo "=================================================="

# Create internal firewall rule if missing
if ! gcloud compute firewall-rules describe "${FIREWALL_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Creating internal firewall rule ${FIREWALL_NAME}..."
    gcloud compute firewall-rules create "${FIREWALL_NAME}" \
        --project="${GCP_PROJECT}" \
        --network=default \
        --direction=INGRESS \
        --allow=icmp,tcp:1-65535,udp:1-65535 \
        --source-ranges=10.128.0.0/9 \
        --target-tags=ukg-vm
fi

# Launch server
echo "[INFO] Launching server instance ${SRV_NAME}..."
gcloud compute instances create "${SRV_NAME}" \
    --zone="${GCP_ZONE}" \
    --project="${GCP_PROJECT}" \
    --machine-type=n1-standard-1 \
    --image="${IMAGE_NAME}" \
    --tags=ukg-vm \
    --labels="run=${UKG_RUN}"

# Launch client
echo "[INFO] Launching client instance ${CLI_NAME}..."
gcloud compute instances create "${CLI_NAME}" \
    --zone="${GCP_ZONE}" \
    --project="${GCP_PROJECT}" \
    --machine-type=n1-standard-1 \
    --image="${IMAGE_NAME}" \
    --tags=ukg-vm \
    --labels="run=${UKG_RUN}"

echo "[SUCCESS] Launched pair ${SRV_NAME} and ${CLI_NAME}."

#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Launch a two-instance c3-standard-4 VM pair on Google Cloud with gVNIC
# and verify traffic and throughput for Phase B3.
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
export UKG_RUN

MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
SRV_IMG="${UKG_RUN}-srv-img"
CLI_IMG="${UKG_RUN}-cli-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

SRV_IP="10.150.0.10"
CLI_IP="10.150.0.11"
NETMASK="20"
GATEWAY="10.150.0.1"

SRV_LOG="/tmp/${UKG_RUN}-srv-serial.log"
CLI_LOG="/tmp/${UKG_RUN}-cli-serial.log"
COMBINED_LOG="/tmp/${UKG_RUN}-serial.log"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "Phase B3 Two-Instance gVNIC Traffic Run"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE}"
echo "Run ID:       ${UKG_RUN}"
echo "Server:       ${SRV_NAME} (${SRV_IP}) image: ${SRV_IMG}"
echo "Client:       ${CLI_NAME} (${CLI_IP}) image: ${CLI_IMG}"
echo "=================================================="

# 1. Build and upload server image
echo "[INFO] Building server image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/networkload" "${SRV_IMG}" \
    "netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} -- role=server peer=${CLI_IP} ip=${SRV_IP}/${NETMASK} gw=${GATEWAY}"

# 2. Build and upload client image
echo "[INFO] Building client image ${CLI_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/networkload" "${CLI_IMG}" \
    "netdev.ip=${CLI_IP}/${NETMASK}:${GATEWAY} -- role=client peer=${SRV_IP} ip=${CLI_IP}/${NETMASK} gw=${GATEWAY}"

# 3. Create internal firewall rule if missing
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

# 4. Delete existing instances if present (rerunnable requirement)
for z in "${GCP_ZONE}" us-east4-b us-east4-c; do
    if gcloud compute instances describe "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
        echo "[INFO] Instance ${SRV_NAME} already exists in ${z}. Deleting..."
        gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet
    fi
    if gcloud compute instances describe "${CLI_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
        echo "[INFO] Instance ${CLI_NAME} already exists in ${z}. Deleting..."
        gcloud compute instances delete "${CLI_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet
    fi
done

# 5. Launch both instances in the candidate zone
SELECTED_ZONE=""
for z in "${GCP_ZONE}" us-east4-b us-east4-c; do
    echo "[INFO] Attempting to launch VM pair in zone ${z}..."
    if gcloud compute instances create "${SRV_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --network-interface="nic-type=GVNIC,private-network-ip=${SRV_IP}" \
        --image="${SRV_IMG}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"; then

        sleep 8

        if gcloud compute instances create "${CLI_NAME}" \
            --zone="${z}" \
            --project="${GCP_PROJECT}" \
            --machine-type="${MACHINE_TYPE}" \
            --boot-disk-type=pd-balanced \
            --network-interface="nic-type=GVNIC,private-network-ip=${CLI_IP}" \
            --image="${CLI_IMG}" \
            --tags=ukg-vm \
            --labels="run=${UKG_RUN}"; then
            SELECTED_ZONE="${z}"
            GCP_ZONE="${z}"
            break
        else
            echo "[WARN] Client creation failed in ${z}. Cleaning up server..."
            gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet
        fi
    fi
    echo "[WARN] Failed to launch VM pair in ${z}, trying next zone..."
done

if [ -z "${SELECTED_ZONE}" ]; then
    echo "[ERROR] Could not launch VM pair in any candidate zone." >&2
    exit 1
fi

echo "[INFO] Both instances launched in ${GCP_ZONE}."
echo "[INFO] Polling serial console output for Phase B3 gate markers..."
PING_PASS=0
NET_PASS=0

for i in $(seq 1 70); do
    sleep 3

    gcloud compute instances get-serial-port-output "${SRV_NAME}" \
        --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" > "${SRV_LOG}" 2>&1 || true

    gcloud compute instances get-serial-port-output "${CLI_NAME}" \
        --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" > "${CLI_LOG}" 2>&1 || true

    if grep -q "GATE_PING_PASS" "${CLI_LOG}" || grep -q "GATE_PING_PASS" "${SRV_LOG}"; then
        PING_PASS=1
    fi

    if grep -q "GATE_NET_PASS" "${CLI_LOG}" && grep -q "GATE_NET_PASS" "${SRV_LOG}" && ! grep -q "GATE_FAIL" "${CLI_LOG}"; then
        NET_PASS=1
    fi

    if [ "${PING_PASS}" -eq 1 ] && [ "${NET_PASS}" -eq 1 ]; then
        echo ""
        echo "[SUCCESS] Both GATE_PING_PASS and GATE_NET_PASS detected after attempt ${i}!"
        break
    fi

    echo -n "."
done
echo ""

# Combine logs for evidence contract
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}) ==="
    cat "${SRV_LOG}" 2>/dev/null || true
    echo ""
    echo "=== CLIENT SERIAL CONSOLE OUTPUT (${CLI_NAME}) ==="
    cat "${CLI_LOG}" 2>/dev/null || true
} > "${COMBINED_LOG}"

echo "--- Server Serial Console Output ---"
cat "${SRV_LOG}" 2>/dev/null || true
echo "------------------------------------"

echo "--- Client Serial Console Output ---"
cat "${CLI_LOG}" 2>/dev/null || true
echo "------------------------------------"

if [ "${PING_PASS}" -eq 1 ] && [ "${NET_PASS}" -eq 1 ]; then
    echo "[PASS] Gate B3 completed successfully: GATE_PING_PASS and GATE_NET_PASS verified."
    exit 0
else
    echo "[FAIL] Missing Phase B3 gate markers in serial console output!" >&2
    echo "PING_PASS=${PING_PASS}, NET_PASS=${NET_PASS}" >&2
    echo "Run teardown manually with: ./scripts/gcp_teardown.sh ${UKG_RUN}" >&2
    exit 1
fi

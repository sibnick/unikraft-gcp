#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Launch a two-instance c3-standard-4 VM pair on Google Cloud with gVNIC
# and verify traffic and throughput for Phase B3.
# The IP plan, the firewall scope, and the zone fallback list are
# overridable through the environment or .env. Ticket e3effabf78 M2.
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

MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
SRV_IMG="${UKG_RUN}-srv-img"
CLI_IMG="${UKG_RUN}-cli-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

# IP plan for the run subnet. Custom subnets are regional, so a
# fallback region needs a different plan. Override all five values
# together in .env when you change region. Defaults keep the
# historical us-east4 plan.
SRV_IP="${UKG_SRV_IP:-10.150.0.10}"
CLI_IP="${UKG_CLI_IP:-10.150.0.11}"
NETMASK="${UKG_NETMASK:-20}"
GATEWAY="${UKG_GATEWAY:-10.150.0.1}"
SUBNET="${UKG_SUBNET:-10.150.0.0/20}"

SRV_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srv")"
CLI_LOG="$(ukg_mk_serial_log "${UKG_RUN}-cli")"
COMBINED_LOG="$(ukg_mk_serial_log "${UKG_RUN}")"

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
echo "Subnet:       ${SUBNET}"
echo "Logs:         ${SRV_LOG} ${CLI_LOG} ${COMBINED_LOG}"
echo "=================================================="

# Install the teardown trap before the first resource create. The
# image builds below already create billable objects. Any later
# error, Ctrl-C, or killed terminal still tears this run down.
# Export KEEP_RES=1 to keep resources and print the teardown command.
ukg_install_teardown_trap

# 1. Build and upload server image
echo "[INFO] Building server image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/networkload" "${SRV_IMG}" \
    "netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} -- role=server peer=${CLI_IP} ip=${SRV_IP}/${NETMASK} gw=${GATEWAY}"

# 2. Build and upload client image
echo "[INFO] Building client image ${CLI_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/networkload" "${CLI_IMG}" \
    "netdev.ip=${CLI_IP}/${NETMASK}:${GATEWAY} -- role=client peer=${SRV_IP} ip=${CLI_IP}/${NETMASK} gw=${GATEWAY}"

# 3. Create internal firewall rule if missing.
# Open only what the gate needs: ICMP for the ping half and UDP 9000
# for the networkload half. The source range is the run subnet only.
if ! gcloud compute firewall-rules describe "${FIREWALL_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Creating internal firewall rule ${FIREWALL_NAME}..."
    gcloud compute firewall-rules create "${FIREWALL_NAME}" \
        --project="${GCP_PROJECT}" \
        --network=default \
        --direction=INGRESS \
        --allow=icmp,udp:9000 \
        --source-ranges="${SUBNET}" \
        --target-tags=ukg-vm
fi

# The zone fallback list comes from ukg_zone_list. Override it with
# UKG_ZONES in .env. Ticket 4347c2a8e6 N7.
ZONE_LIST="$(ukg_zone_list)"

# 4. Delete existing instances if present in any candidate zone
# (rerunnable requirement)
for z in ${ZONE_LIST}; do
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
for z in ${ZONE_LIST}; do
    echo "[INFO] Attempting to launch VM pair in zone ${z}..."
    # The gate reads evidence from the serial console and the pair
    # talks inside the VPC. Both VMs need no billable external IPv4
    # address, so each GVNIC gets no address.
    if gcloud compute instances create "${SRV_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --network-interface="nic-type=GVNIC,no-address,private-network-ip=${SRV_IP}" \
        --image="${SRV_IMG}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"; then

        sleep 8

        if gcloud compute instances create "${CLI_NAME}" \
            --zone="${z}" \
            --project="${GCP_PROJECT}" \
            --machine-type="${MACHINE_TYPE}" \
            --boot-disk-type=pd-balanced \
            --network-interface="nic-type=GVNIC,no-address,private-network-ip=${CLI_IP}" \
            --image="${CLI_IMG}" \
            --tags=ukg-vm \
            --labels="run=${UKG_RUN}"; then
            SELECTED_ZONE="${z}"
            GCP_ZONE="${z}"
            break
        else
            # A create can report failure after the instance actually
            # exists. Re-describe the client for an orphan before the
            # cleanup and the next zone. Ticket 021cb7256c.
            echo "[WARN] Client creation failed in ${z}. Checking for an orphan client..."
            if gcloud compute instances describe "${CLI_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
                echo "[WARN] Orphan client ${CLI_NAME} found in ${z}. Deleting..."
                gcloud compute instances delete "${CLI_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                    || echo "[WARN] Orphan client delete failed. Teardown later with: ${SCRIPT_DIR}/gcp_teardown.sh ${UKG_RUN}" >&2
            fi
            echo "[WARN] Cleaning up server ${SRV_NAME} in ${z}..."
            # A transient delete failure must not abort the script and
            # strand the server VM with no hint. Ticket 021cb7256c.
            gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                || echo "[WARN] Server cleanup delete failed. Teardown later with: ${SCRIPT_DIR}/gcp_teardown.sh ${UKG_RUN}" >&2
        fi
    else
        # The create may have failed after the server actually
        # exists. Check for an orphan before the next zone.
        echo "[WARN] Server creation failed in ${z}. Checking for an orphan server..."
        if gcloud compute instances describe "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
            echo "[WARN] Orphan server ${SRV_NAME} found in ${z}. Deleting..."
            gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                || echo "[WARN] Orphan server delete failed. Teardown later with: ${SCRIPT_DIR}/gcp_teardown.sh ${UKG_RUN}" >&2
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

    if ukg_gate_ping_pass "${SRV_LOG}" "${CLI_LOG}"; then
        PING_PASS=1
    fi

    if ukg_gate_net_pass "${SRV_LOG}" "${CLI_LOG}"; then
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

# Combine logs for evidence contract. Store the file as
# reports/<run-id>-serial.log in fossil uv.
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}) ==="
    cat "${SRV_LOG}" 2>/dev/null || true
    echo ""
    echo "=== CLIENT SERIAL CONSOLE OUTPUT (${CLI_NAME}) ==="
    cat "${CLI_LOG}" 2>/dev/null || true
} > "${COMBINED_LOG}"
echo "[INFO] Combined evidence log: ${COMBINED_LOG}"

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
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

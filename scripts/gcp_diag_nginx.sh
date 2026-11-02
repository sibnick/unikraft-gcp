#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# TEMP DIAG runner (ticket 4ecf9dcedf). Instrumented diagnostic run
# for the nginx-on-GCP investigation. This is NOT a gate: the product
# is root-cause evidence, not a pass.
#
# Layout, all inside one run label and one subnet:
#   -srv     nginx unikernel (threaded lwIP + CONFIG_LWIP_UKG_DIAG),
#            boot arg lwip.diagpeers=<alias ip> installs a static ARP
#            entry for the client alias IP only. The primary client
#            IP has no static entry: the natural experiment.
#   -cli     Ubuntu client. Phase A probes from 10.150.0.11 with no
#            static ARP on the target. Phase B probes from the alias
#            IP 10.150.0.12, which the target has as static ARP.
#            tcpdump records the wire for both phases.
#   -can     canary: the proven no-threads httpreply image with the
#            C1 peer= boot args. Proves fabric and firewall health in
#            the same run and moment.
#
# Discriminators read afterwards:
#   target RX advances under phase A SYNs + client sees the target's
#   ARP who-has with no answer  -> reply-path ARP stall.
#   phase B gets SYN-ACK / 200   -> static ARP is the fix.
#   target RX flat under load    -> frames never reach the ring.
#   canary fails too             -> platform problem, not the stack.
#
# No KEEP_RES support on purpose: the owner requires teardown on
# every exit. The trap chain and the evidence store come from
# scripts/lib/gcp_lib.sh, same as the gate runners.
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

if [ -z "${GCP_BUCKET:-}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    exit 1
fi

INITRD="${ROOT_DIR}/samples/nginx/initrd.cpio"
if [ ! -s "${INITRD}" ]; then
    echo "[ERROR] ${INITRD} is missing or empty." >&2
    exit 1
fi
export UKG_RUN
# The owner instruction for this run forbids keeping resources.
unset KEEP_RES || true

MACHINE_TYPE="${MACHINE_TYPE:-n1-standard-1}"
CLIENT_MACHINE="${UKG_CLIENT_MACHINE:-e2-small}"
UBUNTU_FAMILY="${UKG_UBUNTU_FAMILY:-ubuntu-2404-lts-amd64}"
UBUNTU_PROJECT="${UKG_UBUNTU_PROJECT:-ubuntu-os-cloud}"

SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
CAN_NAME="${UKG_RUN}-can"
SRV_IMG="${UKG_RUN}-srv-img"
CAN_IMG="${UKG_RUN}-can-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

SRV_IP="${UKG_SRV_IP:-10.150.0.10}"
CLI_IP="${UKG_CLI_IP:-10.150.0.11}"
ALIAS_IP="${UKG_ALIAS_IP:-10.150.0.12}"
CAN_IP="${UKG_CAN_IP:-10.150.0.13}"
NETMASK="${UKG_NETMASK:-20}"
GATEWAY="${UKG_GATEWAY:-10.150.0.1}"
SUBNET="${UKG_SUBNET:-10.150.0.0/20}"

SMOKE_MARKER="Unikraft nginx on Google Cloud"

SRV_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srv")"
CLI_LOG="$(ukg_mk_serial_log "${UKG_RUN}-cli")"
CAN_LOG="$(ukg_mk_serial_log "${UKG_RUN}-can")"
COMBINED_LOG="$(ukg_mk_serial_log "${UKG_RUN}")"

LOG_DIR="${UKG_LOG_DIR:-/tmp/opencode}"
mkdir -p "${LOG_DIR}"
CLI_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-diag-startup-XXXXXX.sh")"

UKG_GATE="NGINXDIAG"
UKG_EVIDENCE_LOG="${COMBINED_LOG}"
UKG_TICKET="${UKG_TICKET:-4ecf9dcedf}"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "nginx INSTRUMENTED DIAGNOSTIC Run (TEMP, ticket 4ecf9dcedf)"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE} (client: ${CLIENT_MACHINE})"
echo "Run ID:       ${UKG_RUN}"
echo "Target:       ${SRV_NAME} (${SRV_IP}) image: ${SRV_IMG}"
echo "Client:       ${CLI_NAME} (${CLI_IP}, alias ${ALIAS_IP})"
echo "Canary:       ${CAN_NAME} (${CAN_IP}) image: ${CAN_IMG}"
echo "Subnet:       ${SUBNET}"
echo "Logs:         ${SRV_LOG} ${CLI_LOG} ${CAN_LOG} ${COMBINED_LOG}"
echo "=================================================="

ukg_install_teardown_trap

# 1. Target image: nginx unikernel with the diag build switch. The
# lwip.diagpeers boot argument sits before the ' --' separator, so
# ukboot hands it to the lib-lwip libparam set, not to nginx.
echo "[INFO] Building server image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/nginx" "${SRV_IMG}" \
    "unikraft netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} lwip.diagpeers=${ALIAS_IP} --" \
    "${INITRD}"

# 2. Canary image: the proven no-threads httpreply with the C1 peer
# static-ARP boot args for both client addresses.
echo "[INFO] Building canary image ${CAN_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/httpreply" "${CAN_IMG}" \
    "netdev.ip=${CAN_IP}/${NETMASK}:${GATEWAY} -- peer=${CLI_IP} peer=${ALIAS_IP} ip=${CAN_IP}/${NETMASK} gw=${GATEWAY}"

# 3. Client script. Everything it prints goes to /dev/console and
# lands in the serial log. Phases:
#   A  probe from the primary IP: no static ARP entry on the target.
#   B  probe from the alias IP: the target carries a static entry.
#   C  probe the canary: fabric and firewall proof.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# TEMP DIAG client (run @@RUN@@). Ticket 4ecf9dcedf.
exec > /dev/console 2>&1
TARGET="@@TARGET@@"
ALIAS="@@ALIAS@@"
CANARY="@@CANARY@@"
MARKER="@@MARKER@@"
echo "DIAG start run=@@RUN@@ target=${TARGET} alias=${ALIAS} canary=${CANARY}"

NIC="$(ip -o -4 route show default 2>/dev/null | awk '{print $5; exit}')"
[ -n "${NIC}" ] || NIC=ens4

# Make sure the alias IP is live on the NIC. GCP assigns it at
# instance create; the guest agent configures it. Configure it by
# hand as a fallback and wait until it shows up.
for i in $(seq 1 30); do
    ip -4 addr show dev "${NIC}" 2>/dev/null | grep -q "${ALIAS}" && break
    ip addr add "${ALIAS}/20" dev "${NIC}" 2>/dev/null
    sleep 2
done
echo "DIAG nic=${NIC}"
ip -4 -o addr show dev "${NIC}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1 || echo "DIAGWARN apt-get update failed"
if ! command -v tcpdump >/dev/null 2>&1; then
    apt-get install -y -qq tcpdump >/dev/null 2>&1 || echo "DIAGWARN tcpdump missing"
fi
if ! command -v curl >/dev/null 2>&1; then
    apt-get install -y -qq curl >/dev/null 2>&1 || echo "DIAGWARN curl missing"
fi

# Let the unikernels boot and the diag thread settle.
sleep 30

# Wire capture: 170 seconds of the target's traffic and all ARP.
if command -v tcpdump >/dev/null 2>&1; then
    ( timeout 170 tcpdump -nn -l -i "${NIC}" "host ${TARGET} or arp" \
        > /tmp/diag-tcpdump.log 2>&1 ) &
fi

echo "DIAGPHASE A_BEGIN $(date -u +%H:%M:%S)"
echo "DIAG_A neighbor-state:"
ip neigh show "${TARGET}" || true
ping -c 3 -W 3 "${TARGET}" 2>&1 | tail -n 3
A_OK=0
for i in 1 2 3 4; do
    if timeout 5 bash -c "exec 3<>/dev/tcp/${TARGET}/80" 2>/dev/null; then
        echo "DIAG_A conn${i}=ok"
        A_OK=1
    else
        echo "DIAG_A conn${i}=fail"
    fi
    sleep 6
done
if command -v curl >/dev/null 2>&1; then
    curl -v -m 5 -o /tmp/a-body "http://${TARGET}/" 2>&1 | sed 's/^/DIAG_A curl /'
fi
echo "DIAG_A neighbor-state-after:"
ip neigh show "${TARGET}" || true
echo "DIAGPHASE A_END $(date -u +%H:%M:%S)"

# Give the target one whole diag period (10 s) plus margin, so the
# serial shows the counters for phase A before phase B moves them.
sleep 25

echo "DIAGPHASE B_BEGIN $(date -u +%H:%M:%S)"
ping -c 3 -W 3 -I "${ALIAS}" "${TARGET}" 2>&1 | tail -n 3
if command -v curl >/dev/null 2>&1; then
    curl -v -m 8 --interface "${ALIAS}" -o /tmp/b-body "http://${TARGET}/" 2>&1 | sed 's/^/DIAG_B curl /'
    STATUS_B="$(curl -sS -m 8 --interface "${ALIAS}" -o /tmp/b-body -w '%{http_code}' "http://${TARGET}/" 2>/dev/null || echo 000)"
    sleep 8
    STATUS_B2="$(curl -sS -m 8 --interface "${ALIAS}" -o /tmp/b2-body -w '%{http_code}' "http://${TARGET}/" 2>/dev/null || echo 000)"
    STATUS_B404="$(curl -sS -m 8 --interface "${ALIAS}" -o /dev/null -w '%{http_code}' "http://${TARGET}/missing-page" 2>/dev/null || echo 000)"
else
    STATUS_B=000; STATUS_B2=000; STATUS_B404=000
fi
MARK_B=missing
grep -q "${MARKER}" /tmp/b-body 2>/dev/null && MARK_B=ok
echo "DIAGRES_B_http=${STATUS_B} marker=${MARK_B} conn2=${STATUS_B2} http404=${STATUS_B404}"
echo "DIAGPHASE B_END $(date -u +%H:%M:%S)"

sleep 20

echo "DIAGPHASE C_CANARY_BEGIN $(date -u +%H:%M:%S)"
ping -c 3 -W 3 "${CANARY}" 2>&1 | tail -n 3
if command -v curl >/dev/null 2>&1; then
    STATUS_C="$(curl -sS -m 8 -o /tmp/c-body -w '%{http_code}' "http://${CANARY}/" 2>/dev/null || echo 000)"
else
    STATUS_C=000
fi
echo "DIAGRES_CANARY_http=${STATUS_C}"
echo "DIAG_CANARY_BODY_BEGIN"
head -c 300 /tmp/c-body 2>/dev/null
echo ""
echo "DIAG_CANARY_BODY_END"
echo "DIAGPHASE C_CANARY_END $(date -u +%H:%M:%S)"

# Flush the wire capture once the timeout has expired.
sleep 8
pkill -x tcpdump 2>/dev/null || true
wait 2>/dev/null || true
echo "DIAGTCPDUMP_BEGIN"
head -c 12000 /tmp/diag-tcpdump.log 2>/dev/null
echo "DIAGTCPDUMP_END"

echo "DIAGSUMMARY A_connect=${A_OK} B_http=${STATUS_B} B_marker=${MARK_B} B_conn2=${STATUS_B2} B_404=${STATUS_B404} canary_http=${STATUS_C}"
echo "DIAGDONE"
sleep 3
poweroff
CLI_EOF
sed -i \
    -e "s|@@RUN@@|${UKG_RUN}|g" \
    -e "s|@@TARGET@@|${SRV_IP}|g" \
    -e "s|@@ALIAS@@|${ALIAS_IP}|g" \
    -e "s|@@CANARY@@|${CAN_IP}|g" \
    -e "s|@@MARKER@@|${SMOKE_MARKER}|g" \
    "${CLI_SCRIPT}"

# 4. Internal firewall rule: icmp + tcp:80 from the run subnet. It
# covers target, alias, and canary addresses.
if ! gcloud compute firewall-rules describe "${FIREWALL_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Creating internal firewall rule ${FIREWALL_NAME}..."
    gcloud compute firewall-rules create "${FIREWALL_NAME}" \
        --project="${GCP_PROJECT}" \
        --network=default \
        --direction=INGRESS \
        --allow=icmp,tcp:80 \
        --source-ranges="${SUBNET}" \
        --target-tags=ukg-vm
fi

ZONE_LIST="$(ukg_zone_list)"

# 5. Delete stale instances with these names (rerunnable).
for z in ${ZONE_LIST}; do
    for n in "${SRV_NAME}" "${CLI_NAME}" "${CAN_NAME}"; do
        if gcloud compute instances describe "${n}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
            echo "[INFO] Instance ${n} already exists in ${z}. Deleting..."
            gcloud compute instances delete "${n}" --zone="${z}" --project="${GCP_PROJECT}" --quiet
        fi
    done
done

ukg_diag_create_srv() {
    local z="$1"
    gcloud compute instances create "${SRV_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --image="${SRV_IMG}" \
        --private-network-ip="${SRV_IP}" \
        --no-address \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"
}

ukg_diag_create_can() {
    local z="$1"
    gcloud compute instances create "${CAN_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --image="${CAN_IMG}" \
        --private-network-ip="${CAN_IP}" \
        --no-address \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"
}

ukg_diag_create_cli() {
    local z="$1"
    gcloud compute instances create "${CLI_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${CLIENT_MACHINE}" \
        --boot-disk-type=pd-balanced \
        --network-interface="nic-type=GVNIC,private-network-ip=${CLI_IP},additional-ip-addresses=${ALIAS_IP}" \
        --image-family="${UBUNTU_FAMILY}" \
        --image-project="${UBUNTU_PROJECT}" \
        --metadata-from-file="startup-script=${CLI_SCRIPT}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"
}

# 6. Launch the trio. On a partial create, delete the orphans before
# the next zone, same orphan discipline as the gate runners.
SELECTED_ZONE=""
for z in ${ZONE_LIST}; do
    echo "[INFO] Attempting to launch VM trio in zone ${z}..."
    if ukg_diag_create_srv "${z}"; then
        sleep 5
        if ukg_diag_create_can "${z}"; then
            sleep 5
            if ukg_diag_create_cli "${z}"; then
                SELECTED_ZONE="${z}"
                GCP_ZONE="${z}"
                break
            else
                echo "[WARN] Client creation failed in ${z}. Checking for orphans..."
                if gcloud compute instances describe "${CLI_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
                    gcloud compute instances delete "${CLI_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                        || echo "[WARN] Orphan client delete failed. Teardown later." >&2
                fi
            fi
        else
            echo "[WARN] Canary creation failed in ${z}. Checking for orphans..."
            if gcloud compute instances describe "${CAN_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
                gcloud compute instances delete "${CAN_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                    || echo "[WARN] Orphan canary delete failed. Teardown later." >&2
            fi
        fi
        echo "[WARN] Cleaning up server ${SRV_NAME} in ${z}..."
        gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
            || echo "[WARN] Server cleanup delete failed. Teardown later." >&2
    else
        echo "[WARN] Server creation failed in ${z}. Checking for orphans..."
        if gcloud compute instances describe "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
            gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                || echo "[WARN] Orphan server delete failed. Teardown later." >&2
        fi
    fi
    echo "[WARN] Failed to launch VM trio in ${z}, trying next zone..."
done

if [ -z "${SELECTED_ZONE}" ]; then
    echo "[ERROR] Could not launch VM trio in any candidate zone." >&2
    exit 1
fi

rm -f "${CLI_SCRIPT}"

echo "[INFO] All three instances launched in ${GCP_ZONE}."
echo "[INFO] Polling client serial console for DIAGDONE..."

DIAG_DONE=0
for i in $(seq 1 170); do
    sleep 3
    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true
    ukg_serial_capture "${CAN_NAME}" "${CAN_LOG}" || true
    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true
    if grep -q "DIAGDONE" "${CLI_LOG}" 2>/dev/null; then
        DIAG_DONE=1
        echo ""
        echo "[INFO] Diagnostic finished after attempt ${i}."
        break
    fi
    echo -n "."
done
echo ""

# Final forensic capture before any teardown. Give the serial a last
# propagation window, then re-read all three logs.
sleep 10
ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true
ukg_serial_capture "${CAN_NAME}" "${CAN_LOG}" || true
ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

{
    echo "=== TARGET SERIAL CONSOLE OUTPUT (${SRV_NAME}, unikraft nginx + TEMP DIAG) ==="
    cat "${SRV_LOG}" 2>/dev/null || true
    echo ""
    echo "=== CANARY SERIAL CONSOLE OUTPUT (${CAN_NAME}, unikraft httpreply, proven stack) ==="
    cat "${CAN_LOG}" 2>/dev/null || true
    echo ""
    echo "=== CLIENT SERIAL CONSOLE OUTPUT (${CLI_NAME}) ==="
    cat "${CLI_LOG}" 2>/dev/null || true
} > "${COMBINED_LOG}"
echo "[INFO] Combined evidence log: ${COMBINED_LOG}"

if [ "${DIAG_DONE}" -eq 1 ]; then
    echo "[PASS] Diagnostic run completed. Read the UKGDIAG lines and DIAGRES markers."
    exit 0
else
    echo "[FAIL] DIAGDONE marker missing: the client never finished. Teardown still runs." >&2
    exit 1
fi

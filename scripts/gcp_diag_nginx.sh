#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# TEMP DIAG runner (ticket 4ecf9dcedf). Instrumented diagnostic run
# for the nginx-on-GCP investigation. This is NOT a gate: the product
# is root-cause evidence, not a pass.
#
# Layout, all inside one run label and one subnet, one client, three
# targets that differ in exactly one property:
#   -srv    nginx unikernel, threaded lwIP + CONFIG_LWIP_UKG_DIAG.
#           NO static ARP peer. The natural control: same image and
#           cmdline shape that FAILed gates 1 and 2.
#   -srvb   the same nginx build, but with the boot argument
#           lwip.diagpeers=<client ip>. The diag thread installs the
#           C1 static ARP entry (42:01 + gateway bytes) for the
#           client at boot. If the reply-path ARP stall is the root
#           cause, this target answers and -srv does not.
#   -can    canary: the proven no-threads httpreply image with the C1
#           peer boot args. Proves fabric, firewall, and NIC health
#           in the same run and moment.
#   -cli    Ubuntu client at the primary IP. Phase A probes -srv,
#           phase B probes -srvb, phase C probes -can. tcpdump and
#           ip neigh ride along on the client serial.
#
# Read-out matrix:
#   A fail + B pass + canary pass -> reply-path ARP stall. The static
#       entry is the fix direction.
#   A fail + B fail + canary pass -> the threaded elfloader stack
#   never delivers or answers at all; check the target UKGDIAG RX
#   delta under load: RX flat means frames never reach the ring, RX
#   advancing with no accept means the ring-to-stack path is dead.
#   canary fails too              -> platform (fabric/firewall/NIC).
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
SRVB_NAME="${UKG_RUN}-srvb"
CAN_NAME="${UKG_RUN}-can"
CLI_NAME="${UKG_RUN}-cli"
SRV_IMG="${UKG_RUN}-srv-img"
SRVB_IMG="${UKG_RUN}-srvb-img"
CAN_IMG="${UKG_RUN}-can-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

SRV_IP="${UKG_SRV_IP:-10.150.0.10}"
SRVB_IP="${UKG_SRVB_IP:-10.150.0.14}"
CLI_IP="${UKG_CLI_IP:-10.150.0.11}"
CAN_IP="${UKG_CAN_IP:-10.150.0.13}"
NETMASK="${UKG_NETMASK:-20}"
GATEWAY="${UKG_GATEWAY:-10.150.0.1}"
SUBNET="${UKG_SUBNET:-10.150.0.0/20}"

SMOKE_MARKER="Unikraft nginx on Google Cloud"

SRV_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srv")"
SRVB_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srvb")"
CAN_LOG="$(ukg_mk_serial_log "${UKG_RUN}-can")"
CLI_LOG="$(ukg_mk_serial_log "${UKG_RUN}-cli")"
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
echo "Target A:     ${SRV_NAME} (${SRV_IP}) no static ARP, image: ${SRV_IMG}"
echo "Target B:     ${SRVB_NAME} (${SRVB_IP}) static ARP peer, image: ${SRVB_IMG}"
echo "Canary:       ${CAN_NAME} (${CAN_IP}) image: ${CAN_IMG}"
echo "Client:       ${CLI_NAME} (${CLI_IP})"
echo "Subnet:       ${SUBNET}"
echo "Logs:         ${SRV_LOG} ${SRVB_LOG} ${CAN_LOG} ${CLI_LOG} ${COMBINED_LOG}"
echo "=================================================="

ukg_install_teardown_trap

# 1. Target A image: nginx unikernel with the diag switch, NO static
# ARP peer. Boot args sit before the ' --' separator so ukboot keeps
# them for the libraries and hands nothing to nginx.
echo "[INFO] Building target A image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/nginx" "${SRV_IMG}" \
    "unikraft netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} --" \
    "${INITRD}"

# 2. Target B image: the same nginx build plus the static ARP peer
# for the client IP. Same client MAC path as the C1 gates.
echo "[INFO] Building target B image ${SRVB_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/nginx" "${SRVB_IMG}" \
    "unikraft netdev.ip=${SRVB_IP}/${NETMASK}:${GATEWAY} lwip.diagpeers=${CLI_IP} --" \
    "${INITRD}"

# 3. Canary image: the proven no-threads httpreply with the C1 peer
# static-ARP boot args.
echo "[INFO] Building canary image ${CAN_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/httpreply" "${CAN_IMG}" \
    "netdev.ip=${CAN_IP}/${NETMASK}:${GATEWAY} -- peer=${CLI_IP} ip=${CAN_IP}/${NETMASK} gw=${GATEWAY}"

# 4. Client script. Everything it prints goes to /dev/console and
# lands in the serial log. Phase A: target with no static ARP.
# Phase B: the same target plus the static ARP entry. Phase C: the
# canary.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# TEMP DIAG client (run @@RUN@@). Ticket 4ecf9dcedf.
exec > /dev/console 2>&1
TARGETA="@@TARGETA@@"
TARGETB="@@TARGETB@@"
CANARY="@@CANARY@@"
MARKER="@@MARKER@@"
echo "DIAG start run=@@RUN@@ targetA=${TARGETA} targetB=${TARGETB} canary=${CANARY}"

NIC="$(ip -o -4 route show default 2>/dev/null | awk '{print $5; exit}')"
[ -n "${NIC}" ] || NIC=ens4
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

# Let the unikernels boot and the diag threads settle.
sleep 30

# Wire capture: 220 s covering all three phases plus margin.
if command -v tcpdump >/dev/null 2>&1; then
    ( timeout 220 tcpdump -nn -l -i "${NIC}" \
        "host ${TARGETA} or host ${TARGETB} or host ${CANARY} or arp" \
        > /tmp/diag-tcpdump.log 2>&1 ) &
fi

echo "DIAGPHASE A_BEGIN $(date -u +%H:%M:%S)"
echo "DIAG_A neighbor-state:"
ip neigh show "${TARGETA}" || true
ping -c 3 -W 3 "${TARGETA}" 2>&1 | tail -n 3
A_OK=0
for i in 1 2 3 4; do
    if timeout 5 bash -c "exec 3<>/dev/tcp/${TARGETA}/80" 2>/dev/null; then
        echo "DIAG_A conn${i}=ok"
        A_OK=1
    else
        echo "DIAG_A conn${i}=fail"
    fi
    sleep 6
done
if command -v curl >/dev/null 2>&1; then
    STATUS_A="$(curl -sS -m 5 -o /tmp/a-body -w '%{http_code}' "http://${TARGETA}/" 2>/dev/null || echo 000)"
    curl -v -m 5 -o /dev/null "http://${TARGETA}/" 2>&1 | sed 's/^/DIAG_A curl /'
else
    STATUS_A=000
fi
echo "DIAG_A neighbor-state-after:"
ip neigh show "${TARGETA}" || true
echo "DIAGRES_A_http=${STATUS_A} conn=${A_OK}"
echo "DIAGPHASE A_END $(date -u +%H:%M:%S)"

# One full diag period plus margin, so the target A serial shows the
# counter block for phase A before phase B moves other counters.
sleep 22

echo "DIAGPHASE B_BEGIN $(date -u +%H:%M:%S)"
echo "DIAG_B neighbor-state:"
ip neigh show "${TARGETB}" || true
ping -c 3 -W 3 "${TARGETB}" 2>&1 | tail -n 3
B_OK=0
for i in 1 2 3 4; do
    if timeout 5 bash -c "exec 3<>/dev/tcp/${TARGETB}/80" 2>/dev/null; then
        echo "DIAG_B conn${i}=ok"
        B_OK=1
    else
        echo "DIAG_B conn${i}=fail"
    fi
    sleep 6
done
if command -v curl >/dev/null 2>&1; then
    STATUS_B="$(curl -sS -m 8 -o /tmp/b-body -w '%{http_code}' "http://${TARGETB}/" 2>/dev/null || echo 000)"
    sleep 6
    STATUS_B2="$(curl -sS -m 8 -o /tmp/b2-body -w '%{http_code}' "http://${TARGETB}/" 2>/dev/null || echo 000)"
    STATUS_B404="$(curl -sS -m 8 -o /dev/null -w '%{http_code}' "http://${TARGETB}/missing-page" 2>/dev/null || echo 000)"
    curl -v -m 8 -o /dev/null "http://${TARGETB}/" 2>&1 | sed 's/^/DIAG_B curl /'
else
    STATUS_B=000; STATUS_B2=000; STATUS_B404=000
fi
MARK_B=missing
grep -q "${MARKER}" /tmp/b-body 2>/dev/null && MARK_B=ok
echo "DIAGRES_B_http=${STATUS_B} marker=${MARK_B} conn=${B_OK} conn2=${STATUS_B2} http404=${STATUS_B404}"
echo "DIAGPHASE B_END $(date -u +%H:%M:%S)"

sleep 15

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
sleep 10
pkill -x tcpdump 2>/dev/null || true
wait 2>/dev/null || true
echo "DIAGTCPDUMP_BEGIN"
head -c 14000 /tmp/diag-tcpdump.log 2>/dev/null
echo "DIAGTCPDUMP_END"

echo "DIAGSUMMARY A_http=${STATUS_A} A_conn=${A_OK} B_http=${STATUS_B} B_conn=${B_OK} B_marker=${MARK_B} B_conn2=${STATUS_B2} B_404=${STATUS_B404} canary_http=${STATUS_C}"
echo "DIAGDONE"
sleep 3
poweroff
CLI_EOF
sed -i \
    -e "s|@@RUN@@|${UKG_RUN}|g" \
    -e "s|@@TARGETA@@|${SRV_IP}|g" \
    -e "s|@@TARGETB@@|${SRVB_IP}|g" \
    -e "s|@@CANARY@@|${CAN_IP}|g" \
    -e "s|@@MARKER@@|${SMOKE_MARKER}|g" \
    "${CLI_SCRIPT}"

# 5. Internal firewall rule: icmp + tcp:80 from the run subnet. It
# covers all three targets and the client.
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

# 6. Delete stale instances with these names (rerunnable).
for z in ${ZONE_LIST}; do
    for n in "${SRV_NAME}" "${SRVB_NAME}" "${CAN_NAME}" "${CLI_NAME}"; do
        if gcloud compute instances describe "${n}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
            echo "[INFO] Instance ${n} already exists in ${z}. Deleting..."
            gcloud compute instances delete "${n}" --zone="${z}" --project="${GCP_PROJECT}" --quiet
        fi
    done
done

# Create one unikernel target. Args: name ip image.
ukg_diag_create_vm() {
    local name="$1" ip="$2" img="$3" z="$4"
    gcloud compute instances create "${name}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --image="${img}" \
        --private-network-ip="${ip}" \
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
        --network-interface="nic-type=GVNIC,private-network-ip=${CLI_IP}" \
        --image-family="${UBUNTU_FAMILY}" \
        --image-project="${UBUNTU_PROJECT}" \
        --metadata-from-file="startup-script=${CLI_SCRIPT}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"
}

# Delete every named instance of this zone after a partial launch.
ukg_diag_cleanup_zone() {
    local z="$1" n
    for n in "${CLI_NAME}" "${CAN_NAME}" "${SRVB_NAME}" "${SRV_NAME}"; do
        if gcloud compute instances describe "${n}" --zone="${z}" --project="${GCP_PROJECT}" &>/dev/null; then
            echo "[WARN] Deleting orphan ${n} in ${z}..."
            gcloud compute instances delete "${n}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                || echo "[WARN] Orphan ${n} delete failed. Teardown later." >&2
        fi
    done
}

# 7. Launch the quartet. A create can report failure after the
# instance exists, so re-describe for orphans before the cleanup and
# the next zone, same discipline as the gate runners.
SELECTED_ZONE=""
for z in ${ZONE_LIST}; do
    echo "[INFO] Attempting to launch VM quartet in zone ${z}..."
    if ! ukg_diag_create_vm "${SRV_NAME}" "${SRV_IP}" "${SRV_IMG}" "${z}"; then
        echo "[WARN] Target A creation failed in ${z}."
        ukg_diag_cleanup_zone "${z}"
        echo "[WARN] Failed to launch in ${z}, trying next zone..."
        continue
    fi
    sleep 5
    if ! ukg_diag_create_vm "${SRVB_NAME}" "${SRVB_IP}" "${SRVB_IMG}" "${z}"; then
        echo "[WARN] Target B creation failed in ${z}."
        ukg_diag_cleanup_zone "${z}"
        echo "[WARN] Failed to launch in ${z}, trying next zone..."
        continue
    fi
    sleep 5
    if ! ukg_diag_create_vm "${CAN_NAME}" "${CAN_IP}" "${CAN_IMG}" "${z}"; then
        echo "[WARN] Canary creation failed in ${z}."
        ukg_diag_cleanup_zone "${z}"
        echo "[WARN] Failed to launch in ${z}, trying next zone..."
        continue
    fi
    sleep 5
    if ukg_diag_create_cli "${z}"; then
        SELECTED_ZONE="${z}"
        GCP_ZONE="${z}"
        break
    fi
    echo "[WARN] Client creation failed in ${z}."
    ukg_diag_cleanup_zone "${z}"
    echo "[WARN] Failed to launch in ${z}, trying next zone..."
done

if [ -z "${SELECTED_ZONE}" ]; then
    echo "[ERROR] Could not launch VM quartet in any candidate zone." >&2
    exit 1
fi

rm -f "${CLI_SCRIPT}"

echo "[INFO] All four instances launched in ${GCP_ZONE}."
echo "[INFO] Polling client serial console for DIAGDONE..."

DIAG_DONE=0
for i in $(seq 1 170); do
    sleep 3
    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true
    ukg_serial_capture "${SRVB_NAME}" "${SRVB_LOG}" || true
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
# propagation window, then re-read all four logs.
sleep 12
ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true
ukg_serial_capture "${SRVB_NAME}" "${SRVB_LOG}" || true
ukg_serial_capture "${CAN_NAME}" "${CAN_LOG}" || true
ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

{
    echo "=== TARGET A SERIAL CONSOLE OUTPUT (${SRV_NAME}, nginx, NO static ARP) ==="
    cat "${SRV_LOG}" 2>/dev/null || true
    echo ""
    echo "=== TARGET B SERIAL CONSOLE OUTPUT (${SRVB_NAME}, nginx, static ARP peer) ==="
    cat "${SRVB_LOG}" 2>/dev/null || true
    echo ""
    echo "=== CANARY SERIAL CONSOLE OUTPUT (${CAN_NAME}, httpreply, proven stack) ==="
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

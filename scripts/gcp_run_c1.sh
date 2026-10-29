#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Phase C1 HTTP benchmark run. Target is the httpreply unikernel on
# gVNIC, or a Linux nginx baseline with UKG_TARGET=linux. One Ubuntu
# wrk client VM drives the load through a startup script and powers
# itself off. Both serial logs feed the gate verdict and the evidence
# store. Smoke sizing by default: 2 threads, 50 connections, 10 s.
# Ticket 4c76b4d84d.
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

# Target stack for this run. unikraft boots the ported httpreply
# image. linux boots the Ubuntu nginx baseline on the same machine
# type, so the A/B needs only this flag flip. Ticket 4c76b4d84d.
UKG_TARGET="${UKG_TARGET:-unikraft}"
if [ "${UKG_TARGET}" != "unikraft" ] && [ "${UKG_TARGET}" != "linux" ]; then
    echo "[ERROR] UKG_TARGET must be unikraft or linux." >&2
    exit 1
fi

# The unikraft flavor builds and uploads a disk image, so it needs
# the bucket. The linux flavor installs nginx from apt and needs no
# bucket. Require it before any billable call. Ticket afc0a00b06.
if [ "${UKG_TARGET}" = "unikraft" ] && [ -z "${GCP_BUCKET:-}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    echo "[ERROR] The unikraft target uploads a disk image." >&2
    echo "[ERROR] Set GCP_BUCKET in .env." >&2
    exit 1
fi
export UKG_RUN

# Same machine type as the B gates for the target, so the A/B
# compares like with like. The wrk client is a cheap e2 host.
MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
CLIENT_MACHINE="${UKG_CLIENT_MACHINE:-e2-standard-2}"
UBUNTU_FAMILY="${UKG_UBUNTU_FAMILY:-ubuntu-2404-lts-amd64}"
UBUNTU_PROJECT="${UKG_UBUNTU_PROJECT:-ubuntu-os-cloud}"

# Smoke sweep parameters. The full matrix run overrides these.
WRK_THREADS="${UKG_WRK_THREADS:-2}"
WRK_CONNS="${UKG_WRK_CONNS:-50}"
WRK_DURATION="${UKG_WRK_DURATION:-10}"

SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
SRV_IMG="${UKG_RUN}-srv-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

# IP plan for the run subnet, same contract as the B3 script.
SRV_IP="${UKG_SRV_IP:-10.150.0.10}"
CLI_IP="${UKG_CLI_IP:-10.150.0.11}"
NETMASK="${UKG_NETMASK:-20}"
GATEWAY="${UKG_GATEWAY:-10.150.0.1}"
SUBNET="${UKG_SUBNET:-10.150.0.0/20}"

SRV_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srv")"
CLI_LOG="$(ukg_mk_serial_log "${UKG_RUN}-cli")"
COMBINED_LOG="$(ukg_mk_serial_log "${UKG_RUN}")"

# Guest startup scripts, rendered below and passed through instance
# metadata. Placeholders keep them readable as literal templates.
LOG_DIR="${UKG_LOG_DIR:-/tmp/opencode}"
mkdir -p "${LOG_DIR}"
CLI_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-cli-startup-XXXXXX.sh")"
SRV_SCRIPT=""
if [ "${UKG_TARGET}" = "linux" ]; then
    SRV_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-srv-startup-XXXXXX.sh")"
fi

# Evidence contract wiring, same as the B3 script.
UKG_GATE="C1"
UKG_EVIDENCE_LOG="${COMBINED_LOG}"
UKG_TICKET="${UKG_TICKET:-4c76b4d84d}"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "Phase C1 HTTP Benchmark Run (target: ${UKG_TARGET})"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE} (client: ${CLIENT_MACHINE})"
echo "Run ID:       ${UKG_RUN}"
echo "Server:       ${SRV_NAME} (${SRV_IP})"
echo "Client:       ${CLI_NAME} (${CLI_IP})"
echo "Subnet:       ${SUBNET}"
echo "wrk:          -t${WRK_THREADS} -c${WRK_CONNS} -d${WRK_DURATION}s --latency"
echo "Logs:         ${SRV_LOG} ${CLI_LOG} ${COMBINED_LOG}"
echo "=================================================="

# Install the teardown trap before the first resource create.
# Export KEEP_RES=1 to keep resources and print the teardown command.
ukg_install_teardown_trap

# 1. Target image: the httpreply unikernel for the unikraft flavor.
if [ "${UKG_TARGET}" = "unikraft" ]; then
    echo "[INFO] Building server image ${SRV_IMG}..."
    "${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/httpreply" "${SRV_IMG}" \
        "netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} -- peer=${CLI_IP} ip=${SRV_IP}/${NETMASK} gw=${GATEWAY}"
fi

# 2. Render the wrk client startup script. Everything it prints goes
# to /dev/console, which lands in the GCP serial log. It waits for a
# TCP listener on the target, installs wrk, runs one wrk sweep, and
# decides its own verdict: a Requests/sec number above zero with no
# socket errors and no non-2xx responses is GATE_HTTP_BENCH_PASS.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# C1 wrk benchmark client (run @@RUN@@). Ticket 4c76b4d84d.
exec > /dev/console 2>&1
echo "C1CLIENT start run=@@RUN@@ target=@@TARGET@@ threads=@@THREADS@@ connections=@@CONNS@@ duration=@@DURATION@@s"
TARGET="@@TARGET@@"
PORT=80
READY=0
for i in $(seq 1 90); do
    if timeout 2 bash -c "exec 3<>/dev/tcp/${TARGET}/${PORT}" 2>/dev/null; then
        READY=1
        break
    fi
    sleep 2
done
if [ "${READY}" != "1" ]; then
    echo "GATE_HTTP_BENCH_FAIL: target ${TARGET} port ${PORT} never accepted a connection"
    poweroff
    exit 1
fi
export DEBIAN_FRONTEND=noninteractive
if ! apt-get update -qq; then
    echo "GATE_HTTP_BENCH_FAIL: apt-get update failed"
    poweroff
    exit 1
fi
if ! apt-get install -y -qq wrk; then
    echo "GATE_HTTP_BENCH_FAIL: could not install wrk"
    poweroff
    exit 1
fi
echo "WRK_BEGIN"
wrk -t@@THREADS@@ -c@@CONNS@@ -d@@DURATION@@s --latency "http://${TARGET}:${PORT}/" 2>&1 | tee /tmp/wrk-report.txt
echo "WRK_END"
RPS=$(awk '/Requests\/sec:/ { v=$2 } END { print v }' /tmp/wrk-report.txt)
if ! printf '%s' "${RPS}" | grep -Eq '^[0-9]+(\.[0-9]+)?$'; then
    echo "GATE_HTTP_BENCH_FAIL: no Requests/sec value in the wrk report"
elif grep -E 'Socket errors:' /tmp/wrk-report.txt | grep -q '[1-9]'; then
    echo "GATE_HTTP_BENCH_FAIL: wrk reported socket errors"
elif grep -q 'Non-2xx responses:' /tmp/wrk-report.txt; then
    echo "GATE_HTTP_BENCH_FAIL: wrk reported non-2xx responses"
else
    echo "GATE_HTTP_BENCH_PASS requests_sec=${RPS}"
fi
poweroff
CLI_EOF
sed -i \
    -e "s|@@RUN@@|${UKG_RUN}|g" \
    -e "s|@@TARGET@@|${SRV_IP}|g" \
    -e "s|@@THREADS@@|${WRK_THREADS}|g" \
    -e "s|@@CONNS@@|${WRK_CONNS}|g" \
    -e "s|@@DURATION@@|${WRK_DURATION}|g" \
    "${CLI_SCRIPT}"

# 3. Render the nginx baseline server script for the linux flavor.
# Same machine type as the unikernel target. The verdict stays on
# the client side; this script only brings the server up.
if [ "${UKG_TARGET}" = "linux" ]; then
    cat >"${SRV_SCRIPT}" <<'SRV_EOF'
#!/bin/bash
# C1 nginx baseline server (run @@RUN@@). Ticket 4c76b4d84d.
exec > /dev/console 2>&1
echo "C1SERVER nginx baseline run=@@RUN@@"
export DEBIAN_FRONTEND=noninteractive
if ! apt-get update -qq; then
    echo "GATE_NGINX_FAIL: apt-get update failed"
    exit 1
fi
if ! apt-get install -y -qq nginx; then
    echo "GATE_NGINX_FAIL: nginx install failed"
    exit 1
fi
if ! systemctl enable --now nginx; then
    echo "GATE_NGINX_FAIL: nginx did not start"
    exit 1
fi
sleep 2
if ! systemctl is-active --quiet nginx; then
    echo "GATE_NGINX_FAIL: nginx is not active"
    exit 1
fi
echo "GATE_NGINX_UP"
SRV_EOF
    sed -e "s|@@RUN@@|${UKG_RUN}|g" "${SRV_SCRIPT}" >"${SRV_SCRIPT}.tmp"
    mv -f "${SRV_SCRIPT}.tmp" "${SRV_SCRIPT}"
fi

# 4. Internal firewall rule. Open only what the gate needs: TCP 80
# for the HTTP sweep, ICMP for debug. Source range is the run subnet.
if ! gcloud compute firewall-rules describe "${FIREWALL_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Creating internal firewall rule ${FIREWALL_NAME}..."
    gcloud compute firewall-rules create "${FIREWALL_NAME}" \
        --project="${GCP_PROJECT}" \
        --network=default \
        --direction=INGRESS \
        --allow=icmp,tcp:80 \
        --source-ranges="${SUBNET}" \
        --target-tags=ukg-vm
    # Firewall rules support no labels (verified on SDK 591.0.0). The
    # teardown delete filter covers this rule through the anchored
    # name branch. Ticket 3a59523e76.
fi

ZONE_LIST="$(ukg_zone_list)"

# 5. Delete existing instances if present in any candidate zone
# (rerunnable requirement).
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

# Create helper for the target VM. The unikraft flavor boots the
# custom image. The linux flavor boots the Ubuntu image with the
# nginx startup script. Both VMs stay internal: the gate reads
# evidence from the serial console and the pair talks inside the VPC,
# so each GVNIC gets no address.
ukg_c1_create_srv() {
    local z="$1"
    if [ "${UKG_TARGET}" = "unikraft" ]; then
        gcloud compute instances create "${SRV_NAME}" \
            --zone="${z}" \
            --project="${GCP_PROJECT}" \
            --machine-type="${MACHINE_TYPE}" \
            --boot-disk-type=pd-balanced \
            --network-interface="nic-type=GVNIC,no-address,private-network-ip=${SRV_IP}" \
            --image="${SRV_IMG}" \
            --tags=ukg-vm \
            --labels="run=${UKG_RUN}"
    else
        gcloud compute instances create "${SRV_NAME}" \
            --zone="${z}" \
            --project="${GCP_PROJECT}" \
            --machine-type="${MACHINE_TYPE}" \
            --boot-disk-type=pd-balanced \
            --network-interface="nic-type=GVNIC,no-address,private-network-ip=${SRV_IP}" \
            --image-family="${UBUNTU_FAMILY}" \
            --image-project="${UBUNTU_PROJECT}" \
            --metadata-from-file="startup-script=${SRV_SCRIPT}" \
            --tags=ukg-vm \
            --labels="run=${UKG_RUN}"
    fi
}

# Create helper for the wrk client VM in the candidate zone.
ukg_c1_create_cli() {
    local z="$1"
    gcloud compute instances create "${CLI_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${CLIENT_MACHINE}" \
        --boot-disk-type=pd-balanced \
        --network-interface="nic-type=GVNIC,no-address,private-network-ip=${CLI_IP}" \
        --image-family="${UBUNTU_FAMILY}" \
        --image-project="${UBUNTU_PROJECT}" \
        --metadata-from-file="startup-script=${CLI_SCRIPT}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"
}

# 6. Launch both instances in the candidate zone.
SELECTED_ZONE=""
for z in ${ZONE_LIST}; do
    echo "[INFO] Attempting to launch VM pair in zone ${z}..."
    if ukg_c1_create_srv "${z}"; then

        sleep 8

        if ukg_c1_create_cli "${z}"; then
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
            gcloud compute instances delete "${SRV_NAME}" --zone="${z}" --project="${GCP_PROJECT}" --quiet \
                || echo "[WARN] Server cleanup delete failed. Teardown later with: ${SCRIPT_DIR}/gcp_teardown.sh ${UKG_RUN}" >&2
        fi
    else
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

# The metadata now lives in the instance specs. The local copies are
# no longer needed.
rm -f "${CLI_SCRIPT}" "${SRV_SCRIPT:-}"

echo "[INFO] Both instances launched in ${GCP_ZONE}."
echo "[INFO] Polling serial console output for Phase C1 gate markers..."

# Ukg_c1_verdict decides the run from the two serial logs. The
# unikraft flavor needs both halves: the unikernel served a page
# (GATE_HTTP_FIRST_200) and the client measured a clean sweep
# (GATE_HTTP_BENCH_PASS). The linux baseline has no unikernel marker,
# so the client verdict is the authority there. Both flavors
# re-parse the wrk report on the host with ukg_wrk_ok, so a stray
# pass marker alone cannot pass the gate.
ukg_c1_verdict() {
    if [ "${UKG_TARGET}" = "unikraft" ]; then
        ukg_gate_http_pass "${SRV_LOG}" "${CLI_LOG}" || return 1
    else
        grep -q "GATE_HTTP_BENCH_PASS" "${CLI_LOG}" 2>/dev/null || return 1
        if grep -q "GATE_FAIL" "${CLI_LOG}" 2>/dev/null; then
            return 1
        fi
        if grep -q "GATE_FAIL" "${SRV_LOG}" 2>/dev/null; then
            return 1
        fi
        if grep -q "GATE_HTTP_BENCH_FAIL" "${CLI_LOG}" 2>/dev/null; then
            return 1
        fi
    fi
    ukg_wrk_ok "${CLI_LOG}" || return 1
    return 0
}

GATE_PASS=0

# Poll budget: client boot, apt install of wrk, the 10 s sweep, and
# the serial propagation. The nginx baseline fits the same budget.
for i in $(seq 1 100); do
    sleep 3

    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true

    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

    if ukg_c1_verdict; then
        GATE_PASS=1
        echo ""
        echo "[SUCCESS] Phase C1 gate markers detected after attempt ${i}!"
        break
    fi

    # A client FAIL marker is final: the client prints it and powers
    # off. Stop the poll early so the run reports fast on failure.
    if grep -q "GATE_HTTP_BENCH_FAIL" "${CLI_LOG}" 2>/dev/null; then
        echo ""
        echo "[WARN] Client reported GATE_HTTP_BENCH_FAIL. Stopping the poll."
        break
    fi

    echo -n "."
done
echo ""

# Final capture before the verdict. A failure marker can appear after
# the pass markers, so re-read the logs and re-evaluate. The combined
# evidence log then also carries the final state. Ticket 06ea1706a7.
if [ "${GATE_PASS}" -eq 1 ]; then
    sleep 3
    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true
    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true
    GATE_PASS=0
    if ukg_c1_verdict; then
        GATE_PASS=1
    fi
fi

# Combine logs for the evidence contract. The teardown trap stores
# the file as reports/<run-id>-serial.log in fossil uv after the
# leak audit. Ticket c16353731a.
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}, target: ${UKG_TARGET}) ==="
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

if [ "${GATE_PASS}" -eq 1 ]; then
    echo "[PASS] Gate C1 completed successfully: clean wrk sweep against the ${UKG_TARGET} target."
    exit 0
else
    echo "[FAIL] Missing Phase C1 gate markers in serial console output!" >&2
    echo "GATE_PASS=${GATE_PASS}" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

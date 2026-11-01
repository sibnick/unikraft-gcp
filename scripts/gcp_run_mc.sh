#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Multi-core HTTP benchmark run (ticket 3dc1f3f6b6). The target is the
# httpreply-mc unikernel on gVNIC: per-core lwIP state, one queue pair
# per core, SO_REUSEPORT listeners, and GCP RSS steering. One Ubuntu
# wrk client VM drives the load through a startup script and powers
# itself off, same client pattern as the C1 runner.
#
# The gate has three parts. First: all vCPU workers come online
# (detected N worker cores). Second: every worker core reports a
# heartbeat on the serial console. Third: a clean wrk sweep completes
# and at least one heartbeat carries a served request, so the load
# demonstrably crossed the shared-nothing datapath.
#
# Smoke sizing by default: 2 threads, 50 connections, 10 s at one
# concurrency level. The full sweep is a separate, owner-approved run:
# export the UKG_WRK_* values and loop the concurrency levels.
#
# Usage: gcp_run_mc.sh <run-id>
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

# The unikernel target builds and uploads a disk image, so it needs
# the bucket. Require it before any billable call. Ticket afc0a00b06.
if [ -z "${GCP_BUCKET:-}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    echo "[ERROR] The unikraft target uploads a disk image." >&2
    echo "[ERROR] Set GCP_BUCKET in .env." >&2
    exit 1
fi
export UKG_RUN

# The sample is compiled for four cores (CONFIG_UKPLAT_CPU_MAXCOUNT=4).
# Match the VM vCPU count to that value, and keep the same machine
# type as C1 so the multi-core result compares like with like.
MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
VCPUS="${MACHINE_TYPE##*-}"
if ! [[ "${VCPUS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[ERROR] Cannot read the vCPU count from machine type ${MACHINE_TYPE}." >&2
    exit 1
fi
CLIENT_MACHINE="${UKG_CLIENT_MACHINE:-e2-standard-2}"
UBUNTU_FAMILY="${UKG_UBUNTU_FAMILY:-ubuntu-2404-lts-amd64}"
UBUNTU_PROJECT="${UKG_UBUNTU_PROJECT:-ubuntu-os-cloud}"

# wrk sizing. Smoke defaults hit one concurrency level. The full
# sweep overrides these from the environment.
WRK_THREADS="${UKG_WRK_THREADS:-2}"
WRK_CONNS="${UKG_WRK_CONNS:-50}"
WRK_DURATION="${UKG_WRK_DURATION:-10}"

# The multi-core sample listens on 8000 (SO_REUSEPORT per core).
HTTP_PORT=8000

SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
SRV_IMG="${UKG_RUN}-srv-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

# IP plan for the run subnet, same contract as the C1 script.
SRV_IP="${UKG_SRV_IP:-10.150.0.10}"
CLI_IP="${UKG_CLI_IP:-10.150.0.11}"
NETMASK="${UKG_NETMASK:-20}"
GATEWAY="${UKG_GATEWAY:-10.150.0.1}"
SUBNET="${UKG_SUBNET:-10.150.0.0/20}"

SRV_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srv")"
CLI_LOG="$(ukg_mk_serial_log "${UKG_RUN}-cli")"
COMBINED_LOG="$(ukg_mk_serial_log "${UKG_RUN}")"

LOG_DIR="${UKG_LOG_DIR:-/tmp/opencode}"
mkdir -p "${LOG_DIR}"
CLI_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-cli-startup-XXXXXX.sh")"

# Evidence contract wiring, same as the C1 script.
UKG_GATE="MC"
UKG_EVIDENCE_LOG="${COMBINED_LOG}"
UKG_TICKET="${UKG_TICKET:-3dc1f3f6b6}"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "Multi-core HTTP Benchmark Run (httpreply-mc)"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE} (client: ${CLIENT_MACHINE})"
echo "Workers:      ${VCPUS} vCPU cores expected"
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

# 1. Server image: the httpreply-mc unikernel. The boot arguments
# carry the static-ARP scheme: peer=<client ip> seeds the first
# handshake before accept() can add the entry.
echo "[INFO] Building server image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/httpreply-mc" "${SRV_IMG}" \
    "netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} -- peer=${CLI_IP} ip=${SRV_IP}/${NETMASK} gw=${GATEWAY}"

# 2. Render the wrk client startup script. Same shape as the C1
# client: wait for the listener, install wrk, run one sweep, print
# the verdict, and power off.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# Multi-core wrk benchmark client (run @@RUN@@). Ticket 3dc1f3f6b6.
exec > /dev/console 2>&1
echo "MCCLIENT start run=@@RUN@@ threads=@@THREADS@@ connections=@@CONNS@@ duration=@@DURATION@@s"
TARGET="@@TARGET@@"
PORT=@@PORT@@
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
# Socket-error policy [Ticket 3dc1f3f6b6]. wrk counts a COMPLETED request
# as a "timeout" when its latency exceeds wrk's 2000 ms histogram window
# (SOCKET_TIMEOUT_MS default in wrk.c): an isolated packet loss that TCP
# recovered by retransmission lands there. run ukg-1101-011940-615720: 9
# of 1050064 requests, valid responses, connect/read/write all zero.
# Policy: connect, read, or write errors always fail. A timeout-only
# count up to 0.01 percent of completed requests warns but passes.
VERDICT=$(tr -d '\r' < /tmp/wrk-report.txt | awk '
    /requests in/      { total = $1 }
    /Requests\/sec:/   { rps = $2 }
    /Socket errors:/   { c = $4; r = $6; w = $8; t = $10
                         gsub(/,/, "", c); gsub(/,/, "", r)
                         gsub(/,/, "", w); gsub(/,/, "", t) }
    /Non-2xx/          { n = $1 }
    END {
        if (rps == "" || rps + 0 <= 0)
            { print "FAIL no Requests/sec value in the wrk report"; exit }
        if (c + 0 > 0 || r + 0 > 0 || w + 0 > 0)
            { print "FAIL wrk connect/read/write errors " c "/" r "/" w; exit }
        if (n + 0 > 0)
            { print "FAIL wrk reported non-2xx responses " n; exit }
        if (t + 0 > 0) {
            pct = (total + 0 > 0) ? t * 100.0 / total : 100
            if (pct > 0.01)
                { print "FAIL wrk slow-completed rate " sprintf("%.4f", pct) "% (" t " of " total ")"; exit }
            printf "WARN completed-slow %s of %s over 2s (%s%%, RTO recovery)\n", t, total, sprintf("%.4f", pct)
            print "PASS rps=" rps " slow=" t
            exit
        }
        print "PASS rps=" rps
    }')
case "${VERDICT}" in
    WARN*)
        echo "${VERDICT%%$'\n'*}" | sed 's/^WARN/GATE_HTTP_BENCH_WARN/'
        PASS_LINE="${VERDICT##*$'\n'}"
        ;;
    PASS*)
        PASS_LINE="${VERDICT}"
        ;;
    *)
        PASS_LINE=""
        ;;
esac
if [ -n "${PASS_LINE}" ]; then
    MARK="GATE_HTTP_BENCH_PASS requests_sec=${RPS}"
else
    MARK="GATE_HTTP_BENCH_${VERDICT/FAIL/FAIL:}"
fi
echo "${MARK}"
# Stay alive and reprint the verdict. The runner polls this serial
# console, and get-serial-port-output starts failing once the instance
# is stopped: run ukg-1031-012052-508809 lost every capture after the
# client powered off. Reprinting also covers a serial buffer that only
# propagates late. The run teardown deletes this VM, same session.
# Ticket 3dc1f3f6b6.
for i in $(seq 1 36); do
    sleep 5
    echo "MCCLIENT alive=${i} ${MARK}"
done
poweroff
CLI_EOF
sed -i \
    -e "s|@@RUN@@|${UKG_RUN}|g" \
    -e "s|@@TARGET@@|${SRV_IP}|g" \
    -e "s|@@PORT@@|${HTTP_PORT}|g" \
    -e "s|@@THREADS@@|${WRK_THREADS}|g" \
    -e "s|@@CONNS@@|${WRK_CONNS}|g" \
    -e "s|@@DURATION@@|${WRK_DURATION}|g" \
    "${CLI_SCRIPT}"

# 3. Internal firewall rule: TCP on the benchmark port and ICMP for
# debug. Source range is the run subnet.
if ! gcloud compute firewall-rules describe "${FIREWALL_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Creating internal firewall rule ${FIREWALL_NAME}..."
    gcloud compute firewall-rules create "${FIREWALL_NAME}" \
        --project="${GCP_PROJECT}" \
        --network=default \
        --direction=INGRESS \
        --allow=icmp,tcp:${HTTP_PORT} \
        --source-ranges="${SUBNET}" \
        --target-tags=ukg-vm
    # Firewall rules support no labels (verified on SDK 591.0.0). The
    # teardown delete filter covers this rule through the anchored
    # name branch. Ticket 3a59523e76.
fi

ZONE_LIST="$(ukg_zone_list)"

# 4. Delete existing instances if present in any candidate zone
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

# Create helper for the unikernel target. Internal-only: the gate
# reads evidence from the serial console and the pair talks inside
# the VPC, so the GVNIC gets no external address.
ukg_mc_create_srv() {
    local z="$1"
    gcloud compute instances create "${SRV_NAME}" \
        --zone="${z}" \
        --project="${GCP_PROJECT}" \
        --machine-type="${MACHINE_TYPE}" \
        --boot-disk-type=pd-balanced \
        --network-interface="nic-type=GVNIC,no-address,private-network-ip=${SRV_IP}" \
        --image="${SRV_IMG}" \
        --tags=ukg-vm \
        --labels="run=${UKG_RUN}"
}

# Create helper for the wrk client VM. It installs wrk through apt,
# so it takes an external IPv4 (public-internet Canonical mirrors).
ukg_mc_create_cli() {
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

# 5. Launch both instances in the candidate zone.
SELECTED_ZONE=""
for z in ${ZONE_LIST}; do
    echo "[INFO] Attempting to launch VM pair in zone ${z}..."
    if ukg_mc_create_srv "${z}"; then

        sleep 8

        if ukg_mc_create_cli "${z}"; then
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

# The metadata now lives in the instance specs. The local copy is no
# longer needed.
rm -f "${CLI_SCRIPT}"

echo "[INFO] Both instances launched in ${GCP_ZONE}."
echo "[INFO] Polling serial console output for the multi-core gate markers..."

# Ukg_mc_verdict decides the run from the two serial logs.
# Server side: the boot line reports all ${VCPUS} workers online,
# every worker core 0..VCPUS-1 printed a heartbeat, no core failed
# its allocator self-test, and at least one heartbeat carries a
# served request. Client side: the clean wrk verdict, then the host
# re-parses the wrk report with ukg_wrk_ok, so a stray pass marker
# alone cannot pass the gate.
ukg_mc_verdict() {
    local c seen hb

    # All greps carry -a. Cross-core COM1 writes can leave NUL bytes
    # in the server log. Without -a, grep drops the -o output for a
    # binary file, and the run reports a false FAIL. Run
    # ukg-1031-012052-508809, ticket 3dc1f3f6b6.

    # All workers online: the app counts schedulers, not config slots.
    grep -aq "detected ${VCPUS} worker cores" "${SRV_LOG}" 2>/dev/null || return 1

    # No allocator self-test failure on any core.
    if grep -aq "alloc self-test failed" "${SRV_LOG}" 2>/dev/null; then
        return 1
    fi
    if grep -aq "GATE_FAIL" "${SRV_LOG}" 2>/dev/null; then
        return 1
    fi
    # A crash dump on any core fails the gate: the run must complete
    # the sweep without a Unikraft crash.
    if grep -aq "Unikraft Crash" "${SRV_LOG}" 2>/dev/null; then
        return 1
    fi

    # Every core 0..VCPUS-1 printed at least one heartbeat line.
    for c in $(seq 0 $((VCPUS - 1))); do
        grep -Eaq "core ${c} heartbeat" "${SRV_LOG}" 2>/dev/null || return 1
    done

    # SMP proof: count distinct cores with a heartbeat.
    seen="$(grep -aEo 'core [0-9]+ heartbeat' "${SRV_LOG}" 2>/dev/null \
        | sort -u | wc -l)"
    [ "${seen}" -ge "${VCPUS}" ] || return 1

    # The load must actually have been served by the multi-core
    # stack: a heartbeat with a nonzero request count.
    hb="$(grep -aEo 'req=[0-9]+' "${SRV_LOG}" 2>/dev/null \
        | grep -Ev 'req=0\b' | head -1)"
    [ -n "${hb}" ] || return 1

    # Client verdict.
    grep -aq "GATE_HTTP_BENCH_PASS" "${CLI_LOG}" 2>/dev/null || return 1
    if grep -aq "GATE_FAIL" "${CLI_LOG}" 2>/dev/null; then
        return 1
    fi
    if grep -aq "GATE_HTTP_BENCH_FAIL" "${CLI_LOG}" 2>/dev/null; then
        return 1
    fi
    ukg_wrk_ok "${CLI_LOG}" || return 1
    return 0
}

GATE_PASS=0

# Poll budget: server boot and AP bring-up, client boot, apt install
# of wrk, the sweep, and the serial propagation.
for i in $(seq 1 100); do
    sleep 3

    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true

    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

    if ukg_mc_verdict; then
        GATE_PASS=1
        echo ""
        echo "[SUCCESS] Multi-core gate markers detected after attempt ${i}!"
        break
    fi

    # A client FAIL marker is final: the client prints it and idles.
    # Stop the poll early so the run reports fast on failure.
    if grep -aq "GATE_HTTP_BENCH_FAIL" "${CLI_LOG}" 2>/dev/null; then
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
    if ukg_mc_verdict; then
        GATE_PASS=1
    fi
fi

# Combine logs for the evidence contract. The teardown trap stores
# the file as reports/<run-id>-serial.log in fossil uv after the
# leak audit. Ticket c16353731a.
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}, httpreply-mc) ==="
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
    echo "[PASS] Gate MC completed successfully: ${VCPUS}-core shared-nothing datapath served a clean wrk sweep."
    exit 0
else
    echo "[FAIL] Missing multi-core gate markers in serial console output!" >&2
    echo "GATE_PASS=${GATE_PASS}" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

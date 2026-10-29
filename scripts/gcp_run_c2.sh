#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Phase C2 UDP latency run. Server is the low-latency-hft unikernel
# on gVNIC, or the same C source built natively on Linux with
# UKG_TARGET=linux. One Ubuntu client VM runs an embedded python3
# RTT probe through a startup script and powers itself off. The probe
# mirrors the ENA client report: min, p50, p90, p99, p99.9, max,
# mean. Smoke sizing by default: 20000 synchronous probes.
# Tickets 238f7cbd91 a5874d85fe.
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

# Server stack for this run. unikraft boots the ported hft unikernel.
# linux builds the same server.c natively on the same machine type,
# so the A/B needs only this flag flip. Ticket a5874d85fe.
UKG_TARGET="${UKG_TARGET:-unikraft}"
if [ "${UKG_TARGET}" != "unikraft" ] && [ "${UKG_TARGET}" != "linux" ]; then
    echo "[ERROR] UKG_TARGET must be unikraft or linux." >&2
    exit 1
fi

# The unikraft flavor builds and uploads a disk image, so it needs
# the bucket. Require it before any billable call. Ticket afc0a00b06.
if [ "${UKG_TARGET}" = "unikraft" ] && [ -z "${GCP_BUCKET:-}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    echo "[ERROR] The unikraft target uploads a disk image." >&2
    echo "[ERROR] Set GCP_BUCKET in .env." >&2
    exit 1
fi
export UKG_RUN

# Same machine type as the B gates for the server, so the A/B
# compares like with like. The probe client is a cheap e2 host.
MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
CLIENT_MACHINE="${UKG_CLIENT_MACHINE:-e2-standard-2}"
UBUNTU_FAMILY="${UKG_UBUNTU_FAMILY:-ubuntu-2404-lts-amd64}"
UBUNTU_PROJECT="${UKG_UBUNTU_PROJECT:-ubuntu-os-cloud}"

# Smoke sizing. The full run overrides the probe count.
HFT_PORT="${UKG_HFT_PORT:-9000}"
HFT_PROBES="${UKG_HFT_PROBES:-20000}"

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
# metadata.
LOG_DIR="${UKG_LOG_DIR:-/tmp/opencode}"
mkdir -p "${LOG_DIR}"
CLI_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-cli-startup-XXXXXX.sh")"
SRV_SCRIPT=""
if [ "${UKG_TARGET}" = "linux" ]; then
    SRV_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-srv-startup-XXXXXX.sh")"
fi

# Evidence contract wiring, same as the B3 script.
UKG_GATE="C2"
UKG_EVIDENCE_LOG="${COMBINED_LOG}"
UKG_TICKET="${UKG_TICKET:-a5874d85fe}"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "Phase C2 UDP Latency Run (server: ${UKG_TARGET})"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE} (client: ${CLIENT_MACHINE})"
echo "Run ID:       ${UKG_RUN}"
echo "Server:       ${SRV_NAME} (${SRV_IP})"
echo "Client:       ${CLI_NAME} (${CLI_IP})"
echo "Subnet:       ${SUBNET}"
echo "Probe:        ${HFT_PROBES} synchronous UDP probes to port ${HFT_PORT}"
echo "Logs:         ${SRV_LOG} ${CLI_LOG} ${COMBINED_LOG}"
echo "=================================================="

# Install the teardown trap before the first resource create.
# Export KEEP_RES=1 to keep resources and print the teardown command.
ukg_install_teardown_trap

# 1. Server image: the hft unikernel for the unikraft flavor. The
# app argument list keeps the -p port switch after the -- separator.
if [ "${UKG_TARGET}" = "unikraft" ]; then
    echo "[INFO] Building server image ${SRV_IMG}..."
    "${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/low-latency-hft" "${SRV_IMG}" \
        "netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} -- ip=${SRV_IP}/${NETMASK} gw=${GATEWAY} -p ${HFT_PORT}"
fi

# 2. Render the RTT probe client startup script. It uses the guest
# python3, so it needs no package install. It waits for one echo to
# prove the server, then runs the measured sweep and decides its own
# verdict: zero loss is GATE_HFT_RTT_PASS.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# C2 UDP RTT probe client (run @@RUN@@). Ticket a5874d85fe.
exec > /dev/console 2>&1
echo "C2CLIENT start run=@@RUN@@ target=@@TARGET@@ probes=@@PROBES@@"
TARGET="@@TARGET@@"
PORT=@@PORT@@
PROBES=@@PROBES@@
cat > /root/rtt_probe.py <<'PYEOF'
#!/usr/bin/env python3
# Synchronous UDP RTT probe. Same report shape as the ENA
# low-latency-hft client, so the tables read alike.
import math
import socket
import sys
import time


def percentile(sorted_samples, pct):
    if not sorted_samples:
        return 0.0
    rank = int(math.ceil(pct / 100.0 * len(sorted_samples))) - 1
    rank = max(0, min(rank, len(sorted_samples) - 1))
    return sorted_samples[rank]


def main():
    if len(sys.argv) != 4:
        print("usage: rtt_probe.py <server-ip> <port> <count>")
        return 2
    server = sys.argv[1]
    port = int(sys.argv[2])
    count = int(sys.argv[3])

    payload = b"U" * 64
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 0.5 s is far above any intra-region GCP RTT, so a timeout here
    # means the datagram is lost, not merely slow. One late echo can
    # then pair with the next probe, but any loss fails the gate, so
    # the skew can never reach a passing report.
    sock.settimeout(0.5)
    sock.connect((server, port))

    samples = []
    lost = 0
    streak = 0
    aborted = False
    start = time.monotonic()
    for _ in range(count):
        sent_at = time.monotonic_ns()
        try:
            sock.send(payload)
            sock.recv(65536)
        except socket.timeout:
            lost += 1
            streak += 1
            continue
        except OSError:
            # A connected UDP socket reports ICMP port unreachable
            # here. A down server triggers it on every probe, so stop
            # a dead streak early.
            lost += 1
            streak += 1
            if streak >= 10:
                aborted = True
                break
            continue
        samples.append(time.monotonic_ns() - sent_at)
        streak = 0
        if lost >= 10 and not samples:
            # Nothing ever echoed. Stop the warm-up probe fast so the
            # caller can keep waiting for a still-booting server.
            aborted = True
            break
    total = time.monotonic() - start

    received = len(samples)
    sent = received + lost
    loss_pct = (100.0 * lost / sent) if sent else 100.0
    samples.sort()
    mean = (sum(samples) / received) if received else 0.0
    if received > 1:
        var = sum((x - mean) ** 2 for x in samples) / (received - 1)
        std = math.sqrt(var)
    else:
        std = 0.0

    print("========================================")
    print(" UDP RTT probe report")
    print(" Target: {}:{}  Probes: {}".format(server, port, count))
    if aborted:
        print(" Status: aborted early, no echo traffic")
    print("========================================")
    print("Sent:               {} packets".format(sent))
    print("Received:           {} packets ({:.2f}% loss)".format(received, loss_pct))
    rate = (sent / total) if total > 0 else 0.0
    print("Duration:           {:.3f} seconds ({:.0f} pkts/sec)".format(total, rate))
    print("Round-Trip Latency (RTT):")
    if received:
        print("  Min:              {:9.2f} us".format(samples[0] / 1000.0))
        print("  p50 (Median):     {:9.2f} us".format(percentile(samples, 50) / 1000.0))
        print("  p90:              {:9.2f} us".format(percentile(samples, 90) / 1000.0))
        print("  p99:              {:9.2f} us".format(percentile(samples, 99) / 1000.0))
        print("  p99.9:            {:9.2f} us".format(percentile(samples, 99.9) / 1000.0))
        print("  Max:              {:9.2f} us".format(samples[-1] / 1000.0))
        print("  Mean +/- StdDev:  {:9.2f} +/- {:.2f} us".format(mean / 1000.0, std / 1000.0))
    else:
        print("  no echoes received")
    print("PROBE_RESULT loss_pct={:.2f} p50_us={:.2f} p99_us={:.2f}".format(
        loss_pct,
        percentile(samples, 50) / 1000.0,
        percentile(samples, 99) / 1000.0))
    return 0 if (received > 0 and lost == 0) else 1


sys.exit(main())
PYEOF
READY=0
for i in $(seq 1 60); do
    if python3 /root/rtt_probe.py "${TARGET}" "${PORT}" 1 >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 5
done
if [ "${READY}" != "1" ]; then
    echo "GATE_HFT_RTT_FAIL: target ${TARGET} port ${PORT} never echoed a probe"
    poweroff
    exit 1
fi
echo "PROBE_BEGIN probes=${PROBES}"
python3 /root/rtt_probe.py "${TARGET}" "${PORT}" "${PROBES}"
RC=$?
echo "PROBE_END"
if [ "${RC}" = "0" ]; then
    echo "GATE_HFT_RTT_PASS probes=${PROBES}"
else
    echo "GATE_HFT_RTT_FAIL: measured run finished with packet loss or errors"
fi
poweroff
CLI_EOF
sed -i \
    -e "s|@@RUN@@|${UKG_RUN}|g" \
    -e "s|@@TARGET@@|${SRV_IP}|g" \
    -e "s|@@PORT@@|${HFT_PORT}|g" \
    -e "s|@@PROBES@@|${HFT_PROBES}|g" \
    "${CLI_SCRIPT}"

# 3. Render the baseline server script for the linux flavor. It
# builds the same server.c the unikernel builds, on the same machine
# type as the unikernel target. The client verdict is the authority;
# this script only brings the server up.
if [ "${UKG_TARGET}" = "linux" ]; then
    SERVER_C="${ROOT_DIR}/samples/low-latency-hft/server.c"
    if grep -q '^UKG_SERVER_C$' "${SERVER_C}"; then
        echo "[ERROR] server.c contains the heredoc sentinel UKG_SERVER_C." >&2
        exit 1
    fi
    {
        cat <<'SRV_HEAD'
#!/bin/bash
# C2 Linux hft-server baseline (run @@RUN@@). Ticket a5874d85fe.
exec > /dev/console 2>&1
echo "C2SERVER linux hft baseline run=@@RUN@@"
export DEBIAN_FRONTEND=noninteractive
if ! apt-get update -qq; then
    echo "GATE_LINUX_HFT_FAIL: apt-get update failed"
    exit 1
fi
if ! apt-get install -y -qq build-essential; then
    echo "GATE_LINUX_HFT_FAIL: compiler install failed"
    exit 1
fi
cat > /root/server.c <<'UKG_SERVER_C'
SRV_HEAD
        cat "${SERVER_C}"
        cat <<'SRV_FOOT'
UKG_SERVER_C
if ! gcc -O2 -D_GNU_SOURCE -pthread /root/server.c -o /root/hft-server; then
    echo "GATE_LINUX_HFT_FAIL: server build failed"
    exit 1
fi
/root/hft-server -b 0.0.0.0 -p @@PORT@@ >/root/hft-server.log 2>&1 &
SERVER_PID=$!
sleep 2
if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    echo "GATE_LINUX_HFT_FAIL: server process exited during start"
    cat /root/hft-server.log
    exit 1
fi
echo "GATE_LINUX_HFT_UP"
wait "${SERVER_PID}"
SRV_FOOT
    } >"${SRV_SCRIPT}"
    sed -i \
        -e "s|@@RUN@@|${UKG_RUN}|g" \
        -e "s|@@PORT@@|${HFT_PORT}|g" \
        "${SRV_SCRIPT}"
fi

# 4. Internal firewall rule. Open only what the gate needs: UDP 9000
# for the echo sweep, ICMP for debug. Source range is the run subnet.
if ! gcloud compute firewall-rules describe "${FIREWALL_NAME}" --project="${GCP_PROJECT}" &>/dev/null; then
    echo "[INFO] Creating internal firewall rule ${FIREWALL_NAME}..."
    gcloud compute firewall-rules create "${FIREWALL_NAME}" \
        --project="${GCP_PROJECT}" \
        --network=default \
        --direction=INGRESS \
        --allow=icmp,udp:9000 \
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

# Create helper for the server VM. The unikraft flavor boots the
# custom image. The linux flavor boots the Ubuntu image with the
# native-build startup script. Both VMs stay internal: the gate
# reads evidence from the serial console and the pair talks inside
# the VPC, so each GVNIC gets no address.
ukg_c2_create_srv() {
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

# Create helper for the probe client VM in the candidate zone.
ukg_c2_create_cli() {
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
    if ukg_c2_create_srv "${z}"; then

        sleep 8

        if ukg_c2_create_cli "${z}"; then
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
echo "[INFO] Polling serial console output for Phase C2 gate markers..."

# Ukg_c2_verdict decides the run from the two serial logs. The
# unikraft flavor needs both halves: the unikernel echo server came
# up (GATE_HFT_READY) and the client measured a lossless sweep
# (GATE_HFT_RTT_PASS). The linux baseline has no unikernel marker,
# so the client verdict is the authority there.
ukg_c2_verdict() {
    if [ "${UKG_TARGET}" = "unikraft" ]; then
        ukg_gate_hft_pass "${SRV_LOG}" "${CLI_LOG}" || return 1
    else
        grep -q "GATE_HFT_RTT_PASS" "${CLI_LOG}" 2>/dev/null || return 1
        if grep -q "GATE_FAIL" "${CLI_LOG}" 2>/dev/null; then
            return 1
        fi
        if grep -q "GATE_FAIL" "${SRV_LOG}" 2>/dev/null; then
            return 1
        fi
        if grep -q "GATE_HFT_RTT_FAIL" "${CLI_LOG}" 2>/dev/null; then
            return 1
        fi
    fi
    return 0
}

GATE_PASS=0

# Poll budget: server boot (the linux flavor adds the compiler
# install), the client warm-up wait of up to 300 s, the measured
# sweep, and the serial propagation.
for i in $(seq 1 150); do
    sleep 3

    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true

    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

    if ukg_c2_verdict; then
        GATE_PASS=1
        echo ""
        echo "[SUCCESS] Phase C2 gate markers detected after attempt ${i}!"
        break
    fi

    # A client FAIL marker is final: the client prints it and powers
    # off. Stop the poll early so the run reports fast on failure.
    if grep -q "GATE_HFT_RTT_FAIL" "${CLI_LOG}" 2>/dev/null; then
        echo ""
        echo "[WARN] Client reported GATE_HFT_RTT_FAIL. Stopping the poll."
        break
    fi

    echo -n "."
done
echo ""

# Final capture before the verdict. A failure marker can appear after
# the pass marker, so re-read the logs and re-evaluate. The combined
# evidence log then also carries the final state. Ticket 06ea1706a7.
if [ "${GATE_PASS}" -eq 1 ]; then
    sleep 3
    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true
    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true
    GATE_PASS=0
    if ukg_c2_verdict; then
        GATE_PASS=1
    fi
fi

# Combine logs for the evidence contract. The teardown trap stores
# the file as reports/<run-id>-serial.log in fossil uv after the
# leak audit. Ticket c16353731a.
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}, server: ${UKG_TARGET}) ==="
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
    echo "[PASS] Gate C2 completed successfully: lossless RTT sweep against the ${UKG_TARGET} server."
    exit 0
else
    echo "[FAIL] Missing Phase C2 gate markers in serial console output!" >&2
    echo "GATE_PASS=${GATE_PASS}" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

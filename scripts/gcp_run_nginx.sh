#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# nginx smoke gate run (ticket 4ecf9dcedf). The target is the
# samples/nginx unikernel: app-elfloader boots nginx from a CPIO initrd
# that GRUB passes as a multiboot module. The kernel carries virtio-net
# only, so the target runs on a first or second generation machine
# size (Path A), where virtio-net is the default NIC. One Ubuntu client
# VM fetches GET / with curl. The client verdict on the serial console
# decides the gate: HTTP 200 plus the test page body is
# GATE_NGINX_SMOKE_PASS. Both serial logs feed the evidence store.
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

# The run uploads a disk image to Cloud Storage. Require the bucket
# before any billable call. Same rule as the other run scripts.
if [ -z "${GCP_BUCKET:-}" ]; then
    echo "[ERROR] GCP_BUCKET is not set." >&2
    echo "[ERROR] The run uploads a disk image." >&2
    echo "[ERROR] Set GCP_BUCKET in .env." >&2
    exit 1
fi

# Fail before any cloud call when the tracked initrd is missing.
# samples/nginx/scripts/build_rootfs.sh regenerates it. Ticket 4ecf9dcedf.
INITRD="${ROOT_DIR}/samples/nginx/initrd.cpio"
if [ ! -s "${INITRD}" ]; then
    echo "[ERROR] ${INITRD} is missing or empty." >&2
    echo "[ERROR] Regenerate it with samples/nginx/scripts/build_rootfs.sh." >&2
    exit 1
fi
export UKG_RUN

# The nginx kernel carries virtio-net, not gVNIC, so the target uses a
# first or second generation machine size (Path A). The client is a
# cheap e2 host that installs nothing heavy: curl ships with the
# Ubuntu GCE image.
MACHINE_TYPE="${MACHINE_TYPE:-n1-standard-1}"
CLIENT_MACHINE="${UKG_CLIENT_MACHINE:-e2-small}"
UBUNTU_FAMILY="${UKG_UBUNTU_FAMILY:-ubuntu-2404-lts-amd64}"
UBUNTU_PROJECT="${UKG_UBUNTU_PROJECT:-ubuntu-os-cloud}"

SRV_NAME="${UKG_RUN}-srv"
CLI_NAME="${UKG_RUN}-cli"
SRV_IMG="${UKG_RUN}-srv-img"
FIREWALL_NAME="${UKG_RUN}-allow-internal"

# IP plan for the run subnet, same contract as the A3 and C1 scripts.
SRV_IP="${UKG_SRV_IP:-10.150.0.10}"
CLI_IP="${UKG_CLI_IP:-10.150.0.11}"
NETMASK="${UKG_NETMASK:-20}"
GATEWAY="${UKG_GATEWAY:-10.150.0.1}"
SUBNET="${UKG_SUBNET:-10.150.0.0/20}"

# Marker the client greets in the served page. Keep in sync with
# samples/nginx/html/index.html and samples/nginx/scripts/smoke_test.sh.
SMOKE_MARKER="Unikraft nginx on Google Cloud"

SRV_LOG="$(ukg_mk_serial_log "${UKG_RUN}-srv")"
CLI_LOG="$(ukg_mk_serial_log "${UKG_RUN}-cli")"
COMBINED_LOG="$(ukg_mk_serial_log "${UKG_RUN}")"

# Client startup script, rendered below and passed through instance
# metadata.
LOG_DIR="${UKG_LOG_DIR:-/tmp/opencode}"
mkdir -p "${LOG_DIR}"
CLI_SCRIPT="$(mktemp "${LOG_DIR}/${UKG_RUN}-cli-startup-XXXXXX.sh")"

# Evidence contract wiring, same as the other run scripts.
UKG_GATE="NGINX"
UKG_EVIDENCE_LOG="${COMBINED_LOG}"
UKG_TICKET="${UKG_TICKET:-4ecf9dcedf}"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "nginx Smoke Gate Run"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE} (client: ${CLIENT_MACHINE})"
echo "Run ID:       ${UKG_RUN}"
echo "Server:       ${SRV_NAME} (${SRV_IP}) image: ${SRV_IMG}"
echo "Client:       ${CLI_NAME} (${CLI_IP})"
echo "Subnet:       ${SUBNET}"
echo "Logs:         ${SRV_LOG} ${CLI_LOG} ${COMBINED_LOG}"
echo "=================================================="

# Install the teardown trap before the first resource create. The
# image build below already creates billable objects. Export KEEP_RES=1
# to keep resources and print the teardown command.
ukg_install_teardown_trap

# 1. Target image: the nginx unikernel plus the initrd module.
# Two cmdline rules from the ENA sample apply here (ticket 4ecf9dcedf):
# ukboot hides argv[0] from the library parameter scan and GRUB sends
# no file name in the multiboot cmdline, so 'unikraft' is a placeholder
# kernel name that keeps netdev.ip visible to ukboot. ukboot then reads
# library parameters only up to ' --', and everything after it goes to
# the application. Nothing follows the separator, so nginx starts with
# no arguments. Without the separator nginx exits with
# 'nginx: invalid option'.
# lwip.staticarp=<client ip>: the GCP fabric does not answer ARP for
# instance IPs, so the target needs a static ARP entry for the client
# to send any reply (run ukg-1103-005311-601275: SYN-ACKs queued in
# the ARP pending pool forever). Library file staticarp.c installs it
# from the gateway MAC. Same convention as the C1 gate apps, ticket
# 73b23402f0. Ticket 4ecf9dcedf.
echo "[INFO] Building server image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/nginx" "${SRV_IMG}" \
    "unikraft netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} lwip.staticarp=${CLI_IP} --" "${INITRD}"

# 2. Render the curl client startup script. Everything it prints goes
# to /dev/console, which lands in the GCP serial log. It waits for a
# TCP listener on the target, then fetches GET /. HTTP 200 with the
# test page marker is GATE_NGINX_SMOKE_PASS. The second GET and the
# 404 check ride along as forensic fields; the gate needs only the
# first two fields.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# nginx smoke client (run @@RUN@@). Ticket 4ecf9dcedf.
exec > /dev/console 2>&1
echo "NGINXSMOKE start run=@@RUN@@ target=@@TARGET@@"
TARGET="@@TARGET@@"
MARKER="@@MARKER@@"
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
    echo "GATE_NGINX_SMOKE_FAIL: target ${TARGET} port ${PORT} never accepted a connection"
    poweroff
    exit 1
fi
if ! command -v curl >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    if ! apt-get update -qq || ! apt-get install -y -qq curl; then
        echo "GATE_NGINX_SMOKE_FAIL: could not install curl"
        poweroff
        exit 1
    fi
fi
STATUS1="$(curl -sS -m 10 -o /tmp/ukg-body-1 -w '%{http_code}' "http://${TARGET}:${PORT}/" 2>/dev/null || echo 000)"
echo "NGINX_SMOKE_RESULT status=${STATUS1}"
echo "NGINX_BODY_BEGIN"
head -c 600 /tmp/ukg-body-1 2>/dev/null
echo ""
echo "NGINX_BODY_END"
MARK=missing
if grep -q "${MARKER}" /tmp/ukg-body-1 2>/dev/null; then
    MARK=ok
fi
STATUS2="$(curl -sS -m 10 -o /dev/null -w '%{http_code}' "http://${TARGET}:${PORT}/" 2>/dev/null || echo 000)"
STATUS404="$(curl -sS -m 10 -o /dev/null -w '%{http_code}' "http://${TARGET}:${PORT}/missing-page" 2>/dev/null || echo 000)"
if [ "${STATUS1}" = "200" ] && [ "${MARK}" = "ok" ]; then
    echo "GATE_NGINX_SMOKE_PASS status=${STATUS1} marker=${MARK} conn2=${STATUS2} http404=${STATUS404}"
else
    echo "GATE_NGINX_SMOKE_FAIL status=${STATUS1} marker=${MARK} conn2=${STATUS2} http404=${STATUS404}"
fi
poweroff
CLI_EOF
sed -i \
    -e "s|@@RUN@@|${UKG_RUN}|g" \
    -e "s|@@TARGET@@|${SRV_IP}|g" \
    -e "s|@@MARKER@@|${SMOKE_MARKER}|g" \
    "${CLI_SCRIPT}"

# 3. Internal firewall rule. Open only what the gate needs: TCP 80 for
# the GET, ICMP for debug. Source range is the run subnet.
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

# Create helper for the target VM. The gate reads evidence from the
# serial console and the GET travels inside the VPC, so the target
# gets no billable external IPv4 address. The NIC stays the machine
# default: virtio-net on this generation.
ukg_nginx_create_srv() {
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

# Create helper for the client VM. It may need to install curl through
# apt, so it takes an external IPv4 like the C1 client. Canonical's
# GCE Ubuntu mirrors are public-internet, not Google-internal. Our own
# firewall rules stay internal-only; the ukg-vm tag matches no public
# ingress rule.
ukg_nginx_create_cli() {
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
    if ukg_nginx_create_srv "${z}"; then

        sleep 8

        if ukg_nginx_create_cli "${z}"; then
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
echo "[INFO] Polling serial console output for the nginx gate marker..."

# Ukg_nginx_verdict decides the run from the client log. The client
# prints the status and marker fields it measured, so a stray bare
# marker cannot pass: the host requires the status=200 and marker=ok
# fields in the same line, the same double-check rule as the C1 wrk
# verdict.
ukg_nginx_verdict() {
    grep -q "GATE_NGINX_SMOKE_PASS status=200 marker=ok" "${CLI_LOG}" 2>/dev/null || return 1
    if grep -q "GATE_FAIL" "${CLI_LOG}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_FAIL" "${SRV_LOG}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_NGINX_SMOKE_FAIL" "${CLI_LOG}" 2>/dev/null; then
        return 1
    fi
    return 0
}

GATE_PASS=0

# Poll budget: client boot, target boot plus nginx start from the
# initrd, the curl checks, and the serial propagation.
for i in $(seq 1 120); do
    sleep 3

    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true

    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

    if ukg_nginx_verdict; then
        GATE_PASS=1
        echo ""
        echo "[SUCCESS] nginx gate marker detected after attempt ${i}!"
        break
    fi

    # A client FAIL marker is final: the client prints it and powers
    # off. Stop the poll early so the run reports fast on failure.
    if grep -q "GATE_NGINX_SMOKE_FAIL" "${CLI_LOG}" 2>/dev/null; then
        echo ""
        echo "[WARN] Client reported GATE_NGINX_SMOKE_FAIL. Stopping the poll."
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
    if ukg_nginx_verdict; then
        GATE_PASS=1
    fi
fi

# Combine logs for the evidence contract. The teardown trap stores the
# file as reports/<run-id>-serial.log in fossil uv after the leak
# audit. Ticket c16353731a.
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}, target: unikraft nginx) ==="
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
    echo "[PASS] Gate NGINX completed successfully: curl GET / returned 200 with the test page."
    exit 0
else
    echo "[FAIL] Missing nginx gate markers in serial console output!" >&2
    echo "GATE_PASS=${GATE_PASS}" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

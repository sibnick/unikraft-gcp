#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# gVNIC MSI-X interrupt RX gate run (ticket eae6a73726). The target is
# the samples/nginx-gve unikernel: the same nginx app-elfloader stack
# as ticket 4ecf9dcedf, but with lwIP in threaded mode and RX driven
# by the gve MSI-X interrupt path instead of a poll thread. The kernel
# carries the gve driver, so the target runs on a third generation
# machine size (Path B) with a GVNIC interface: the gVNIC device is
# 1ae0:0042. One Ubuntu client VM fetches GET / with curl, the same
# smoke client as the nginx gate.
#
# The gate needs two markers:
#   server: GATE_GVE_MSIX_PASS vector=<v> q=<n>
#     printed by the gve ISR on the first MSI-X RX event, which proves
#     the interrupt fired and the dispatcher thread saw RX traffic.
#   client: GATE_NGINX_SMOKE_PASS status=200 marker=ok
#     the served page proves the stack answered over the interrupt
#     path end to end.
# The polled RX path is the fallback switch, not the gate: the run
# aborts before any cloud call when the build carries
# CONFIG_LWIP_UKNETDEV_POLLRX=y. The gve.intr=0 boot parameter is the
# manual fallback for debug reruns of the same image.
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

# The sample reuses the nginx rootfs tracked in samples/nginx.
# samples/nginx/scripts/build_rootfs.sh regenerates it. Ticket
# eae6a73726.
INITRD="${ROOT_DIR}/samples/nginx/initrd.cpio"
if [ ! -s "${INITRD}" ]; then
    echo "[ERROR] ${INITRD} is missing or empty." >&2
    echo "[ERROR] Regenerate it with samples/nginx/scripts/build_rootfs.sh." >&2
    exit 1
fi

# The gate is the interrupt path, so the build must not carry the poll
# thread. Check the tracked defconfig before the image build starts.
# The generated .config gets the same check after the build, because
# gcp_build_image.sh reuses an existing .config instead of regenerating
# it. Ticket eae6a73726.
DEFCONFIG="${ROOT_DIR}/samples/nginx-gve/defconfig"
if [ ! -f "${DEFCONFIG}" ]; then
    echo "[ERROR] ${DEFCONFIG} is missing." >&2
    exit 1
fi
if grep -q "^CONFIG_LWIP_UKNETDEV_POLLRX=y" "${DEFCONFIG}"; then
    echo "[ERROR] ${DEFCONFIG} enables CONFIG_LWIP_UKNETDEV_POLLRX." >&2
    echo "[ERROR] The gate measures interrupt RX. A poll thread would make the result meaningless." >&2
    exit 1
fi
if ! grep -q "^CONFIG_LIBUKINTCTLR_APIC=y" "${DEFCONFIG}"; then
    echo "[ERROR] ${DEFCONFIG} lacks CONFIG_LIBUKINTCTLR_APIC." >&2
    echo "[ERROR] Without the local APIC the driver keeps RX polled and the gate cannot pass." >&2
    exit 1
fi
export UKG_RUN

# gVNIC is the default NIC on third generation machine sizes and newer
# (Path B). The gate uses the same c3-standard-4 size that the RSS and
# MC gates proved. The client is a cheap e2 host: curl ships with the
# Ubuntu GCE image.
MACHINE_TYPE="${MACHINE_TYPE:-c3-standard-4}"
CLIENT_MACHINE="${UKG_CLIENT_MACHINE:-e2-small}"
UBUNTU_FAMILY="${UKG_UBUNTU_FAMILY:-ubuntu-2404-lts-amd64}"
UBUNTU_PROJECT="${UKG_UBUNTU_PROJECT:-ubuntu-os-cloud}"

# Pinned guest package version for the smoke client. The install
# branch runs only when the image ships no curl, so pinning never
# fights a preinstalled newer curl. The default is the Ubuntu 24.04
# LTS (noble) release version. Update it together with
# UKG_UBUNTU_FAMILY, or pass a per-run override. The client prints
# the installed version to the serial log, so the stored evidence
# names the exact package. Acquire::Retries keeps a flaky apt mirror
# from failing the gate after billable time. Ticket 1a21e263ad.
CURL_VERSION="${UKG_CURL_VERSION:-8.5.0-2ubuntu10}"

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
# metadata. The helper places it in the private per-user log
# directory.
CLI_SCRIPT="$(ukg_mk_startup_script "${UKG_RUN}" cli)"

# Evidence contract wiring, same as the other run scripts.
UKG_GATE="GVE_MSIX"
UKG_EVIDENCE_LOG="${COMBINED_LOG}"
UKG_TICKET="${UKG_TICKET:-eae6a73726}"

if [ -z "${GCP_PROJECT}" ] || [ -z "${GCP_ZONE}" ]; then
    echo "[ERROR] GCP_PROJECT and GCP_ZONE must be set." >&2
    exit 1
fi

echo "=================================================="
echo "gVNIC MSI-X Interrupt RX Gate Run"
echo "Project:      ${GCP_PROJECT}"
echo "Zone:         ${GCP_ZONE}"
echo "Machine Type: ${MACHINE_TYPE} (client: ${CLIENT_MACHINE})"
echo "Run ID:       ${UKG_RUN}"
echo "Server:       ${SRV_NAME} (${SRV_IP}) image: ${SRV_IMG}"
echo "Client:       ${CLI_NAME} (${CLI_IP})"
echo "Subnet:       ${SUBNET}"
echo "Packages:     curl=${CURL_VERSION}"
echo "Logs:         ${SRV_LOG} ${CLI_LOG} ${COMBINED_LOG}"
echo "=================================================="

# Install the teardown trap before the first resource create. The
# image build below already creates billable objects. Export KEEP_RES=1
# to keep resources and print the teardown command.
ukg_install_teardown_trap

# 1. Target image: the nginx-gve unikernel plus the initrd module.
# The cmdline rules are the proven ones from ticket 4ecf9dcedf: the
# 'unikraft' placeholder keeps netdev.ip visible to ukboot, ' --'
# starts the application arguments, and lwip.staticarp carries the
# client binding because the GCP fabric does not answer ARP for
# instance IPs (ticket 73b23402f0).
# 'gve.intr=1' pins the interrupt mode in the evidence line. The
# parameter is the documented fallback switch: gve.intr=0 keeps every
# notification block masked and the stack falls back to polled RX.
echo "[INFO] Building server image ${SRV_IMG}..."
"${SCRIPT_DIR}/gcp_build_image.sh" "${ROOT_DIR}/samples/nginx-gve" "${SRV_IMG}" \
    "unikraft netdev.ip=${SRV_IP}/${NETMASK}:${GATEWAY} lwip.staticarp=${CLI_IP} gve.intr=1 --" "${INITRD}"

# Re-check the generated .config: gcp_build_image.sh keeps an existing
# .config, so the tracked defconfig alone does not prove what was
# built. Abort before the first instance create. Ticket eae6a73726.
BUILT_CONFIG="${ROOT_DIR}/samples/nginx-gve/build/.config"
if [ ! -f "${BUILT_CONFIG}" ]; then
    BUILT_CONFIG="${ROOT_DIR}/samples/nginx-gve/.config"
fi
if [ -f "${BUILT_CONFIG}" ] && grep -q "^CONFIG_LWIP_UKNETDEV_POLLRX=y" "${BUILT_CONFIG}"; then
    echo "[ERROR] The built kernel carries CONFIG_LWIP_UKNETDEV_POLLRX=y." >&2
    echo "[ERROR] Remove the poll thread option and rebuild before the gate run." >&2
    exit 1
fi

# 2. Render the curl client startup script. It is the nginx smoke
# client from ticket 4ecf9dcedf, unchanged: HTTP 200 plus the test page
# marker is GATE_NGINX_SMOKE_PASS.
cat >"${CLI_SCRIPT}" <<'CLI_EOF'
#!/bin/bash
# nginx smoke client for the gVNIC MSI-X gate (run @@RUN@@). Ticket eae6a73726.
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
    if ! apt-get update -qq -o Acquire::Retries=5 ||
       ! apt-get install -y -qq -o Acquire::Retries=5 "curl=@@CURL_VERSION@@"; then
        echo "GATE_NGINX_SMOKE_FAIL: could not install pinned curl=@@CURL_VERSION@@. Set UKG_CURL_VERSION to a version this image offers."
        poweroff
        exit 1
    fi
fi
dpkg-query -W -f='PKG curl ${Version}\n' curl
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
    -e "s|@@CURL_VERSION@@|${CURL_VERSION}|g" \
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

# Create helper for the target VM. The NIC is an explicit GVNIC with
# no address: third generation sizes default to gVNIC anyway, and the
# flag records the intent in the instance spec. The gVNIC device is
# 1ae0:0042, which the gve driver binds.
ukg_nginx_gve_create_srv() {
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

# Create helper for the client VM. It may need to install curl through
# apt, so it takes an external IPv4 like the nginx and C1 clients.
# Canonical's GCE Ubuntu mirrors are public-internet, not
# Google-internal. Our own firewall rules stay internal-only; the
# ukg-vm tag matches no public ingress rule.
ukg_nginx_gve_create_cli() {
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
    if ukg_nginx_gve_create_srv "${z}"; then

        sleep 8

        if ukg_nginx_gve_create_cli "${z}"; then
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
echo "[INFO] Polling serial console output for the gVNIC MSI-X gate markers..."

# Ukg_nginx_gve_verdict decides the run from both logs. Both markers
# must carry their fields, so a stray bare marker cannot pass: the
# server line must name the vector and queue that fired, and the
# client line must carry status=200 and marker=ok. Same double-check
# rule as the nginx and C1 verdicts. The gve driver prints no FAIL
# marker for the interrupt path: a device that never delivers an MSI-X
# event simply keeps the server marker absent, and the poll budget
# ends the run.
ukg_nginx_gve_verdict() {
    grep -q "GATE_GVE_MSIX_PASS vector=" "${SRV_LOG}" 2>/dev/null || return 1
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
# initrd, the curl checks, and the serial propagation. Same budget as
# the nginx gate.
for i in $(seq 1 120); do
    sleep 3

    ukg_serial_capture "${SRV_NAME}" "${SRV_LOG}" || true

    ukg_serial_capture "${CLI_NAME}" "${CLI_LOG}" || true

    if ukg_nginx_gve_verdict; then
        GATE_PASS=1
        echo ""
        echo "[SUCCESS] gVNIC MSI-X gate markers detected after attempt ${i}!"
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
    if ukg_nginx_gve_verdict; then
        GATE_PASS=1
    fi
fi

# Combine logs for the evidence contract. The teardown trap stores the
# file as reports/<run-id>-serial.log in fossil uv after the leak
# audit. Ticket c16353731a.
{
    echo "=== SERVER SERIAL CONSOLE OUTPUT (${SRV_NAME}, target: unikraft nginx over gVNIC MSI-X) ==="
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
    echo "[PASS] Gate GVE_MSIX completed successfully: RX arrived on the MSI-X event and curl GET / returned 200 with the test page."
    exit 0
else
    echo "[FAIL] Missing gVNIC MSI-X gate markers in serial console output!" >&2
    echo "GATE_PASS=${GATE_PASS}" >&2
    echo "The teardown trap now deletes this run's resources. Export KEEP_RES=1 before the next run to keep them." >&2
    exit 1
fi

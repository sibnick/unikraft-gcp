#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Host test for the gate and naming helpers in scripts/lib/gcp_lib.sh.
# Covers tickets 74936642ce (run-id name boundary), 5e6a2acd8a
# (run-scoped image names), and 06ea1706a7 (failure markers veto
# gate passes). No Fossil and no Google Cloud calls happen.
# The UKG_* variables are inputs to the library functions. ShellCheck
# cannot see that use, so the unused warnings do not apply here.
# shellcheck disable=SC2034
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=../scripts/lib/gcp_lib.sh
source "${ROOT_DIR}/scripts/lib/gcp_lib.sh"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ukg-gate-logic-XXXXXX")"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

PASS_COUNT=0
FAIL_COUNT=0
ok() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - $1"
}
bad() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - $1"
}

# 1. Run-id name boundary. The regex must match the run id and the
# derived names, and never a different run whose id extends this one.
REGEX="$(ukg_run_name_regex "ukg-x")"
match_names=(
    "ukg-x" "ukg-x-srv" "ukg-x-cli" "ukg-x-img"
    "ukg-x-srv-img" "ukg-x-cli-img" "ukg-x-allow-internal"
)
nonmatch_names=(
    "ukg-x-2" "ukg-x-2-srv" "ukg-x-img2" "ukg-x-srv-img2"
    "ukg-x-internal" "ukg-other-srv" "prefix-ukg-x"
)
for name in "${match_names[@]}"; do
    if [[ "${name}" =~ ${REGEX} ]]; then
        ok "regex matches own name ${name}"
    else
        bad "regex matches own name ${name}"
    fi
done
for name in "${nonmatch_names[@]}"; do
    if [[ "${name}" =~ ${REGEX} ]]; then
        bad "regex rejects foreign name ${name}"
    else
        ok "regex rejects foreign name ${name}"
    fi
done
# Every suffix documented in the library must be matched.
for suffix in ${UKG_RUN_SUFFIXES}; do
    if [[ "ukg-x-${suffix}" =~ ${REGEX} ]]; then
        ok "regex covers suffix ${suffix}"
    else
        bad "regex covers suffix ${suffix}"
    fi
done

# 2. Image-name validation. Invalid names must exit non-zero, valid
# run-scoped names must pass. Run each call in a subshell because the
# helper exits on the bad cases.
valid_image() {
    ( ukg_validate_image_name "$1" "$2" ) >/dev/null 2>&1
}
# 64 characters total: one more than the GCP image name limit.
long_name="$(printf 'ukg-x-%s' \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")"
valid_image "ukg-x-img" "ukg-x" && ok "accepts run-scoped ukg-x-img" \
    || bad "accepts run-scoped ukg-x-img"
valid_image "ukg-x-srv-img" "ukg-x" && ok "accepts run-scoped ukg-x-srv-img" \
    || bad "accepts run-scoped ukg-x-srv-img"
for bad_name in "../evil" "a/b" "UPPER" "" "9lives" "other-run-img" \
    "ukg-x--" "ukg-x im" "${long_name}"; do
    if valid_image "${bad_name}" "ukg-x"; then
        bad "rejects image name '${bad_name}'"
    else
        ok "rejects image name '${bad_name}'"
    fi
done

# 3. Gate helpers. A GATE_FAIL marker in either log vetoes the pass.
mk() { printf '%s\n' "$2" > "${WORK}/$1"; }
mk cli_pass.log "GATE_PING_PASS"
mk cli_passfail.log "GATE_PING_PASS
GATE_FAIL: something broke later"
mk cli_fail.log "GATE_FAIL: broke"
mk srv_pass.log "GATE_NET_PASS
GATE_PING_PASS"
mk srv_quiet.log "boot noise only"

if ukg_gate_ping_pass "${WORK}/srv_quiet.log" "${WORK}/cli_pass.log"; then
    ok "ping gate accepts a pass log without a fail marker"
else
    bad "ping gate accepts a pass log without a fail marker"
fi
if ukg_gate_ping_pass "${WORK}/srv_pass.log" "${WORK}/cli_passfail.log"; then
    bad "ping gate vetoes a fail marker in the client log"
else
    ok "ping gate vetoes a fail marker in the client log"
fi
if ukg_gate_ping_pass "${WORK}/cli_fail.log" "${WORK}/cli_pass.log"; then
    bad "ping gate vetoes a fail marker in the server log"
else
    ok "ping gate vetoes a fail marker in the server log"
fi
if ukg_gate_net_pass "${WORK}/srv_pass.log" "${WORK}/cli_passfail.log"; then
    bad "net gate vetoes a fail marker"
else
    ok "net gate vetoes a fail marker"
fi

# 4. Run-scoped delete filter. The filter must carry the run label
# branch and keep the anchored name branch. Ticket 3a59523e76.
FILTER="$(ukg_run_delete_filter "ukg-x")"
if [[ "${FILTER}" == *'(labels.run="ukg-x")'* ]]; then
    ok "delete filter carries the run label branch"
else
    bad "delete filter carries the run label branch"
fi
# Extract the name regex from the filter and reuse it as in teardown.
FREGEX="${FILTER##*name ~ \"}"
FREGEX="${FREGEX%\")*}"
if [[ "ukg-x-srv" =~ ${FREGEX} ]]; then
    ok "delete filter name branch matches own derived name"
else
    bad "delete filter name branch matches own derived name"
fi
if [[ "ukg-x-2-srv" =~ ${FREGEX} ]]; then
    bad "delete filter name branch rejects extending run id"
else
    ok "delete filter name branch rejects extending run id"
fi
if [[ "foreign-disk" =~ ${FREGEX} ]]; then
    bad "delete filter name branch rejects foreign name"
else
    ok "delete filter name branch rejects foreign name"
fi

# 5. Cloud Storage rm classification. Missing object is clean, other
# errors are real. Ticket afc0a00b06.
gcs_ok() { ukg_gcs_rm_ok "$1" "$2"; }
gcs_ok 0 "" && ok "gcs rm accepts exit 0" || bad "gcs rm accepts exit 0"
gcs_ok 1 "No URLs matched: gs://b/ukg-x-img-disk.tar.gz" \
    && ok "gcs rm accepts wildcard miss" || bad "gcs rm accepts wildcard miss"
gcs_ok 1 "googleapi: Error 404: Not Found" \
    && ok "gcs rm accepts 404 miss" || bad "gcs rm accepts 404 miss"
gcs_ok 1 "Error: PERMISSION_DENIED: insufficient rights" \
    && bad "gcs rm records permission error" \
    || ok "gcs rm records permission error"
gcs_ok 2 "" && bad "gcs rm records silent failure" \
    || ok "gcs rm records silent failure"
# Exact absent-object text captured from run ukg-1029-010754: current
# gcloud phrasing must stay classified as nothing-to-delete.
gcs_ok 1 "ERROR: (gcloud.storage.rm) The following URLs matched no objects or files:
gs://bucket/ukg-x-img-disk.tar.gz" \
    && ok "gcs rm accepts current absent-object text" \
    || bad "gcs rm accepts current absent-object text"

# 6. Script-level teardown behavior against a stub gcloud in a
# throwaway checkout. No real cloud call happens.
STUBBIN="${WORK}/stubbin"
mkdir -p "${STUBBIN}"
cat >"${STUBBIN}/gcloud" <<'EOF'
#!/usr/bin/env bash
args="$*"
if [[ "${args}" == *"storage rm"* ]]; then
    case "${STUB_GCS_MODE:-empty}" in
        missing)
            echo "Command failed to remove objects: No URLs matched" \
                "gs://test-bucket/x" >&2
            exit 1 ;;
        current-missing)
            echo "ERROR: (gcloud.storage.rm) The following URLs" \
                "matched no objects or files:" >&2
            echo "gs://test-bucket/x" >&2
            exit 1 ;;
        denied)
            echo "Error: PERMISSION_DENIED: does not have permission" >&2
            exit 1 ;;
        *) exit 0 ;;
    esac
fi
if [[ "${args}" == *"get-serial-port-output"* ]]; then
    case "${STUB_SERIAL_MODE:-ok}" in
        fail)
            echo "ERROR: (gcloud.compute.instances.get-serial-port-output)" \
                "connection refused" >&2
            exit 1 ;;
        *)
            echo "stub serial content"
            exit 0 ;;
    esac
fi
# All list commands report an empty project.
exit 0
EOF
chmod +x "${STUBBIN}/gcloud"
CO2="${WORK}/checkout"
mkdir -p "${CO2}/scripts/lib"
cp "${ROOT_DIR}/scripts/gcp_teardown.sh" "${CO2}/scripts/gcp_teardown.sh"
cp "${ROOT_DIR}/scripts/lib/gcp_lib.sh" "${CO2}/scripts/lib/gcp_lib.sh"
# The guarded assignments keep a value that the test exports, even an
# empty one. GCP_BUCKET stays unset for the unknown-audit case.
cat >"${CO2}/.env" <<'EOF'
GCP_PROJECT="${GCP_PROJECT-test-proj}"
GCP_ZONE="${GCP_ZONE-us-east4-a}"
GCP_BUCKET="${GCP_BUCKET-test-bucket}"
EOF

run_teardown() {
    # $1: extra env assignments, $2: run id or empty.
    local extra="$1" rid="$2"
    shift 2
    env ${extra} PATH="${STUBBIN}:${PATH}" "${CO2}/scripts/gcp_teardown.sh" \
        ${rid:+"${rid}"} </dev/null 2>&1
}

out="$(run_teardown "STUB_GCS_MODE=denied" ukg-x-1)" && rc=0 || rc=$?
if [ "${rc}" -ne 0 ]; then
    ok "permission-denied storage rm fails the teardown"
else
    bad "permission-denied storage rm fails the teardown"
fi
if printf '%s' "${out}" | grep -q "Cloud Storage delete failed"; then
    ok "permission-denied storage rm names the failure"
else
    bad "permission-denied storage rm names the failure"
fi

out="$(run_teardown "STUB_GCS_MODE=missing" ukg-x-2)" && rc=0 || rc=$?
if [ "${rc}" -eq 0 ]; then
    ok "absent storage object stays a clean teardown"
else
    bad "absent storage object stays a clean teardown (rc: ${rc})"
fi

out="$(run_teardown "GCP_BUCKET=" ukg-x-3)" && rc=0 || rc=$?
if [ "${rc}" -ne 0 ]; then
    ok "unset bucket fails the audit as unknown"
else
    bad "unset bucket fails the audit as unknown"
fi
if printf '%s' "${out}" | grep -q "unknown, not clean"; then
    ok "unset bucket audit prints the unknown reason"
else
    bad "unset bucket audit prints the unknown reason"
fi

out="$(UKG_TEARDOWN_ALL=1 run_teardown "UKG_TEARDOWN_ALL=1" "")" \
    && rc=0 || rc=$?
if [ "${rc}" -ne 0 ]; then
    ok "sweep without confirmation refuses on a non-terminal"
else
    bad "sweep without confirmation refuses on a non-terminal"
fi
if printf '%s' "${out}" | grep -q "needs a terminal"; then
    ok "sweep refusal names the terminal requirement"
else
    bad "sweep refusal names the terminal requirement"
fi
if printf '%s' "${out}" | grep -q "Resources the sweep would delete"; then
    ok "sweep lists resources before refusing"
else
    bad "sweep lists resources before refusing"
fi

# 7. Serial capture. A failed fetch must keep the previous log, and a
# good fetch must replace it. Ticket 0bc13135d7.
CAP="${WORK}/cap.log"
(
    export PATH="${STUBBIN}:${PATH}" GCP_ZONE=test-zone \
        GCP_PROJECT=test-proj STUB_SERIAL_MODE=ok
    ukg_serial_capture inst "${CAP}"
) && ok "serial capture succeeds with the stub" \
    || bad "serial capture succeeds with the stub"
if [ -f "${CAP}" ] && grep -q "stub serial content" "${CAP}"; then
    ok "serial capture stores the fetched content"
else
    bad "serial capture stores the fetched content"
fi
(
    export PATH="${STUBBIN}:${PATH}" GCP_ZONE=test-zone \
        GCP_PROJECT=test-proj STUB_SERIAL_MODE=fail
    ukg_serial_capture inst "${CAP}"
) && bad "failed serial capture returns non-zero" \
    || ok "failed serial capture returns non-zero"
if grep -q "stub serial content" "${CAP}"; then
    ok "failed serial capture keeps the previous log"
else
    bad "failed serial capture keeps the previous log"
fi
if [ ! -e "${CAP}.new" ]; then
    ok "failed serial capture removes the temp file"
else
    bad "failed serial capture removes the temp file"
fi

# 8. C1 and C2 gate helpers and the wrk report verdict. The fixtures
# mirror the marker contract of gcp_run_c1.sh and gcp_run_c2.sh. The
# wrk fixtures carry CRLF endings, because the host parses raw GCP
# serial logs. Ticket 4c76b4d84d a5874d85fe.
mk http_srv_pass.log "GATE_HTTP_READY
GATE_HTTP_FIRST_200"
mk http_srv_nofirst.log "GATE_HTTP_READY"
mk http_srv_fail.log "GATE_HTTP_READY
GATE_FAIL: epoll_wait errno 5"
mk http_cli_pass.log "WRK_BEGIN
GATE_HTTP_BENCH_PASS requests_sec=12284.17
WRK_END"
mk http_cli_benchfail.log "GATE_HTTP_BENCH_PASS requests_sec=1
GATE_HTTP_BENCH_FAIL: wrk reported socket errors"
mk http_cli_quiet.log "boot noise only"

if ukg_gate_http_pass "${WORK}/http_srv_pass.log" "${WORK}/http_cli_pass.log"; then
    ok "http gate accepts both markers with no fail marker"
else
    bad "http gate accepts both markers with no fail marker"
fi
if ukg_gate_http_pass "${WORK}/http_srv_nofirst.log" "${WORK}/http_cli_pass.log"; then
    bad "http gate vetoes a server without GATE_HTTP_FIRST_200"
else
    ok "http gate vetoes a server without GATE_HTTP_FIRST_200"
fi
if ukg_gate_http_pass "${WORK}/http_srv_pass.log" "${WORK}/http_cli_quiet.log"; then
    bad "http gate vetoes a client without GATE_HTTP_BENCH_PASS"
else
    ok "http gate vetoes a client without GATE_HTTP_BENCH_PASS"
fi
if ukg_gate_http_pass "${WORK}/http_srv_fail.log" "${WORK}/http_cli_pass.log"; then
    bad "http gate vetoes a GATE_FAIL in the server log"
else
    ok "http gate vetoes a GATE_FAIL in the server log"
fi
if ukg_gate_http_pass "${WORK}/http_srv_pass.log" "${WORK}/http_cli_benchfail.log"; then
    bad "http gate vetoes a late GATE_HTTP_BENCH_FAIL"
else
    ok "http gate vetoes a late GATE_HTTP_BENCH_FAIL"
fi

mk hft_srv_pass.log "Listening on: 0.0.0.0:9000
GATE_HFT_READY"
mk hft_srv_quiet.log "boot noise only"
mk hft_cli_pass.log "PROBE_BEGIN
GATE_HFT_RTT_PASS probes=20000
PROBE_END"
mk hft_cli_fail.log "GATE_HFT_RTT_FAIL: measured run finished with packet loss"

if ukg_gate_hft_pass "${WORK}/hft_srv_pass.log" "${WORK}/hft_cli_pass.log"; then
    ok "hft gate accepts both markers with no fail marker"
else
    bad "hft gate accepts both markers with no fail marker"
fi
if ukg_gate_hft_pass "${WORK}/hft_srv_quiet.log" "${WORK}/hft_cli_pass.log"; then
    bad "hft gate vetoes a server without GATE_HFT_READY"
else
    ok "hft gate vetoes a server without GATE_HFT_READY"
fi
if ukg_gate_hft_pass "${WORK}/hft_srv_pass.log" "${WORK}/hft_cli_fail.log"; then
    bad "hft gate vetoes GATE_HFT_RTT_FAIL on the client"
else
    ok "hft gate vetoes GATE_HFT_RTT_FAIL on the client"
fi
if ukg_gate_hft_pass "${WORK}/http_srv_fail.log" "${WORK}/hft_cli_pass.log"; then
    bad "hft gate vetoes a GATE_FAIL in the server log"
else
    ok "hft gate vetoes a GATE_FAIL in the server log"
fi

printf 'Running 10s test @ http://10.150.0.10/\r\n  2 threads and 50 connections\r\n  123456 requests in 10.05s, 18.63MB read\r\nRequests/sec:  12284.17\r\nTransfer/sec:      1.85MB\r\n' > "${WORK}/wrk_clean.log"
printf 'Running 10s test @ http://10.150.0.10/\r\nRequests/sec:  12284.17\r\nSocket errors: connect 0, read 0, write 0, timeout 3\r\n' > "${WORK}/wrk_sockerr.log"
printf 'Running 10s test @ http://10.150.0.10/\r\nNon-2xx responses: 12\r\nRequests/sec:  12284.17\r\n' > "${WORK}/wrk_non2xx.log"
printf 'Running 10s test @ http://10.150.0.10/\r\nsocket: Connection refused\r\n' > "${WORK}/wrk_norps.log"
printf 'Running 10s test @ http://10.150.0.10/\r\nRequests/sec:  0.00\r\n' > "${WORK}/wrk_zerorps.log"
: > "${WORK}/wrk_empty.log"

if ukg_wrk_ok "${WORK}/wrk_clean.log"; then
    ok "wrk verdict accepts a clean CRLF serial report"
else
    bad "wrk verdict accepts a clean CRLF serial report"
fi
if ukg_wrk_ok "${WORK}/wrk_sockerr.log"; then
    bad "wrk verdict vetoes socket errors"
else
    ok "wrk verdict vetoes socket errors"
fi
if ukg_wrk_ok "${WORK}/wrk_non2xx.log"; then
    bad "wrk verdict vetoes non-2xx responses"
else
    ok "wrk verdict vetoes non-2xx responses"
fi
if ukg_wrk_ok "${WORK}/wrk_norps.log"; then
    bad "wrk verdict vetoes a report without Requests/sec"
else
    ok "wrk verdict vetoes a report without Requests/sec"
fi
if ukg_wrk_ok "${WORK}/wrk_zerorps.log"; then
    bad "wrk verdict vetoes zero requests per second"
else
    ok "wrk verdict vetoes zero requests per second"
fi
if ukg_wrk_ok "${WORK}/wrk_empty.log"; then
    bad "wrk verdict vetoes an empty report"
else
    ok "wrk verdict vetoes an empty report"
fi

echo ""
echo "test_gate_logic: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[ "${FAIL_COUNT}" -eq 0 ]

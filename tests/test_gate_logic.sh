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

echo ""
echo "test_gate_logic: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[ "${FAIL_COUNT}" -eq 0 ]

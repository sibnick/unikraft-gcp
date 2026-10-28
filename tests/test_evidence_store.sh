#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Host test for the evidence-store step in scripts/lib/gcp_lib.sh.
# Tickets c16353731a cf185ab6fd.
#
# The test needs Fossil on the host. It builds a throwaway checkout
# in a temporary directory, so fossil uv commands never touch this
# repository. No Google Cloud calls happen. The teardown trap test
# uses a stub teardown script.
# The UKG_* variables are inputs to the library functions. ShellCheck
# cannot see that use, so the unused warnings do not apply here.
# shellcheck disable=SC2034
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=../scripts/lib/gcp_lib.sh
source "${ROOT_DIR}/scripts/lib/gcp_lib.sh"

if ! command -v fossil >/dev/null 2>&1; then
    echo "test_evidence_store: fossil not installed, skipping"
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ukg-evict-XXXXXX")"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

PASS_COUNT=0
FAIL_COUNT=0
# Assert that haystack $2 contains needle $3 for test name $1.
check_contains() {
    if printf '%s' "$2" | grep -qF "$3"; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "ok   - $1"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "FAIL - $1 (missing: $3)"
        echo "       output: $2"
    fi
}

# Build a throwaway Fossil checkout for the artifact store.
REPO="${WORK}/test.fossil"
CO="${WORK}/co"
mkdir -p "${CO}"
fossil new "${REPO}" >/dev/null
(
    cd "${CO}" \
    && fossil open "${REPO}" >/dev/null \
    && echo seed >seed.txt \
    && fossil add seed.txt >/dev/null \
    && fossil commit -m "test seed" >/dev/null
)
# Redirect the store and the trap test into this checkout.
UKG_REPO_ROOT="${CO}"

# Case 1: a pass stores the log and prints the ticket command.
UKG_RUN="ukg-test-0001"
UKG_GATE="B1"
UKG_TICKET="deadbeef01"
UKG_EVIDENCE_LOG="${WORK}/pass.log"
printf 'GATE_GVE_ADMINQ_PASS\n' >"${UKG_EVIDENCE_LOG}"
out="$(ukg_store_evidence 0 2>&1)"
check_contains "pass stores under reports/<run>-serial.log" "${out}" \
    "reports/ukg-test-0001-serial.log"
check_contains "pass prints the mapped ticket command" "${out}" \
    "fossil ticket change deadbeef01"
check_contains "pass prints a PASS result line" "${out}" "run ukg-test-0001: PASS"
stored="$(cd "${CO}" && fossil uv ls)"
if printf '%s\n' "${stored}" | grep -Fxq "reports/ukg-test-0001-serial.log"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - fossil uv ls shows the stored log"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - fossil uv ls shows the stored log (have: ${stored})"
fi

# Case 2: a reused name never overwrites the stored evidence.
out="$(ukg_store_evidence 0 2>&1)"
check_contains "duplicate warns instead of overwrite" "${out}" "already exists"
check_contains "duplicate prints the replace hint" "${out}" "fossil uv replace"
dup_count="$(cd "${CO}" && fossil uv ls | grep -c "ukg-test-0001-serial.log")"
if [ "${dup_count}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - duplicate run stored no second copy"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - duplicate run stored no second copy (count: ${dup_count})"
fi

# Case 3: a fail stores the log and prints a FAIL result line.
UKG_RUN="ukg-test-0002"
UKG_EVIDENCE_LOG="${WORK}/fail.log"
printf 'no gate marker here\n' >"${UKG_EVIDENCE_LOG}"
out="$(ukg_store_evidence 1 2>&1)"
check_contains "fail stores the log" "${out}" "reports/ukg-test-0002-serial.log"
check_contains "fail prints a FAIL result line" "${out}" "run ukg-test-0002: FAIL"

# Case 4: an empty log stores nothing and stays quiet about tickets.
UKG_EVIDENCE_LOG="${WORK}/empty.log"
: >"${UKG_EVIDENCE_LOG}"
out="$(ukg_store_evidence 1 2>&1)"
check_contains "empty log reports nothing stored" "${out}" "Nothing stored"

# Case 5: no UKG_EVIDENCE_LOG means silent success.
unset UKG_EVIDENCE_LOG
out="$(ukg_store_evidence 0 2>&1)"
if [ -z "${out}" ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - no evidence log means no output"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - no evidence log means no output (have: ${out})"
fi

# Case 6: the trap stores evidence after teardown, never before.
STUB="${WORK}/stub_teardown.sh"
cat >"${STUB}" <<'EOF'
#!/usr/bin/env bash
echo "STUB_TEARDOWN_DONE $1"
EOF
chmod +x "${STUB}"
UKG_TEARDOWN_SCRIPT="${STUB}"
CHILD="${WORK}/child_run.sh"
cat >"${CHILD}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
source "${ROOT_DIR}/scripts/lib/gcp_lib.sh"
UKG_REPO_ROOT="${CO}"
UKG_TEARDOWN_SCRIPT="${STUB}"
export UKG_REPO_ROOT UKG_TEARDOWN_SCRIPT
UKG_RUN="ukg-test-0003"
UKG_GATE="B2"
UKG_TICKET="cafebabeee"
UKG_EVIDENCE_LOG="${WORK}/trap.log"
printf 'GATE_GVE_PASS\n' >"\${UKG_EVIDENCE_LOG}"
ukg_install_teardown_trap
echo "run finished"
exit 0
EOF
out="$(bash "${CHILD}" 2>&1)"
rc=$?
check_contains "trap test ran the run body" "${out}" "run finished"
check_contains "trap test ran the stub teardown" "${out}" \
    "STUB_TEARDOWN_DONE ukg-test-0003"
check_contains "trap test stored the evidence" "${out}" \
    "reports/ukg-test-0003-serial.log"
if [ "${rc}" -eq 0 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - trap preserved the zero exit code"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - trap preserved the zero exit code (rc: ${rc})"
fi
td_line="$(printf '%s\n' "${out}" | grep -n "STUB_TEARDOWN_DONE" | cut -d: -f1)"
ev_line="$(printf '%s\n' "${out}" | grep -n "\[EVIDENCE\] Storing" | cut -d: -f1)"
if [ -n "${td_line}" ] && [ -n "${ev_line}" ] && [ "${td_line}" -lt "${ev_line}" ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - teardown and audit step runs before the store step"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - teardown and audit step runs before the store step" \
        "(teardown line: ${td_line}, store line: ${ev_line})"
fi
trap_stored="$(cd "${CO}" && fossil uv ls)"
if printf '%s\n' "${trap_stored}" | grep -Fxq "reports/ukg-test-0003-serial.log"; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - trap path stored the log in the artifact store"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - trap path stored the log in the artifact store" \
        "(have: ${trap_stored})"
fi

# Case 7: a passed gate over a failed cleanup or kept resources
# stores PARTIAL, never PASS. Ticket cf185ab6fd.
UKG_RUN="ukg-test-0004"
UKG_EVIDENCE_LOG="${WORK}/partial1.log"
printf 'GATE_GVE_ADMINQ_PASS\n' >"${UKG_EVIDENCE_LOG}"
out="$(ukg_store_evidence 0 1 0 2>&1)"
check_contains "cleanup failure downgrades PASS to PARTIAL" "${out}" \
    "run ukg-test-0004: PARTIAL"
check_contains "PARTIAL names the cleanup failure" "${out}" \
    "teardown or the leak audit failed"
UKG_RUN="ukg-test-0005"
out="$(ukg_store_evidence 0 0 1 2>&1)"
check_contains "kept resources downgrade PASS to PARTIAL" "${out}" \
    "run ukg-test-0005: PARTIAL"
check_contains "PARTIAL names the KEEP_RES retention" "${out}" \
    "KEEP_RES kept billable resources"
UKG_RUN="ukg-test-0006"
out="$(ukg_store_evidence 1 1 0 2>&1)"
check_contains "failed gate stays FAIL with failed cleanup" "${out}" \
    "run ukg-test-0006: FAIL"

# Case 8: the trap turns a zero gate exit into 1 when teardown fails,
# and stores PARTIAL.
STUB_FAIL="${WORK}/stub_teardown_fail.sh"
cat >"${STUB_FAIL}" <<'EOF'
#!/usr/bin/env bash
echo "STUB_TEARDOWN_FAIL $1"
exit 3
EOF
chmod +x "${STUB_FAIL}"
CHILD2="${WORK}/child_run_fail_cleanup.sh"
cat >"${CHILD2}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
source "${ROOT_DIR}/scripts/lib/gcp_lib.sh"
UKG_REPO_ROOT="${CO}"
UKG_TEARDOWN_SCRIPT="${STUB_FAIL}"
UKG_RUN="ukg-test-0007"
UKG_GATE="B1"
UKG_TICKET="cafebabeee"
UKG_EVIDENCE_LOG="${WORK}/trap_fail.log"
printf 'GATE_GVE_ADMINQ_PASS\n' >"\${UKG_EVIDENCE_LOG}"
ukg_install_teardown_trap
exit 0
EOF
out="$(bash "${CHILD2}" 2>&1)"
rc=$?
if [ "${rc}" -eq 1 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - trap turns a zero gate exit into 1 when cleanup fails"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - trap turns a zero gate exit into 1 when cleanup fails" \
        "(rc: ${rc})"
fi
check_contains "trap with failed cleanup stores PARTIAL" "${out}" \
    "run ukg-test-0007: PARTIAL"

# Case 9: KEEP_RES skips teardown, keeps the exit code, but the
# stored result is PARTIAL.
CHILD3="${WORK}/child_run_keep.sh"
cat >"${CHILD3}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
source "${ROOT_DIR}/scripts/lib/gcp_lib.sh"
UKG_REPO_ROOT="${CO}"
UKG_TEARDOWN_SCRIPT="${STUB}"
UKG_RUN="ukg-test-0008"
UKG_GATE="B2"
UKG_TICKET="cafebabeee"
UKG_EVIDENCE_LOG="${WORK}/trap_keep.log"
printf 'GATE_GVE_PASS\n' >"\${UKG_EVIDENCE_LOG}"
export KEEP_RES=1
ukg_install_teardown_trap
exit 0
EOF
out="$(bash "${CHILD3}" 2>&1)"
rc=$?
if [ "${rc}" -eq 0 ]; then
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - KEEP_RES run preserves the zero exit code"
else
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - KEEP_RES run preserves the zero exit code (rc: ${rc})"
fi
if printf '%s' "${out}" | grep -qF "STUB_TEARDOWN_DONE"; then
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "FAIL - KEEP_RES skips the teardown script"
else
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "ok   - KEEP_RES skips the teardown script"
fi
check_contains "KEEP_RES run stores PARTIAL" "${out}" \
    "run ukg-test-0008: PARTIAL"

echo ""
echo "test_evidence_store: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[ "${FAIL_COUNT}" -eq 0 ]

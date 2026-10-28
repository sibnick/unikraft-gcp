#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Shared helpers for the Google Cloud run scripts. Source this file.
# It defines functions. Only the three UKG_* path variables at the
# top are set on source. Dependencies: coreutils, gawk, and fossil
# for the evidence store step.
#
# Sourced by: gcp_run_a1.sh gcp_run_a2.sh gcp_run_a3.sh gcp_run_b1.sh
# gcp_run_b2.sh gcp_run_b3.sh gcp_build_image.sh gcp_teardown.sh
# Tickets: 1c76a66b26 4a7a2ca6f5 e3effabf78 4347c2a8e6 c16353731a

# Directory of this file, the path of the teardown script, and the
# root of this Fossil checkout. The evidence store step needs the
# checkout root for the fossil uv and fossil ticket commands.
UKG_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UKG_TEARDOWN_SCRIPT="${UKG_LIB_DIR}/../gcp_teardown.sh"
UKG_REPO_ROOT="$(cd "${UKG_LIB_DIR}/../.." && pwd)"

# Validate a run id. Accept only ^ukg-[a-z0-9-]+$.
# Teardown builds name filters from the run id. A loose id such as
# "." would match every resource in the project.
# Arg $1: run id. The function exits 1 on an invalid id.
ukg_validate_run_id() {
    local id="${1:-}"
    if [[ ! "${id}" =~ ^ukg-[a-z0-9-]+$ ]]; then
        echo "[ERROR] Invalid run id: '${id}'." >&2
        echo "[ERROR] A run id must match ^ukg-[a-z0-9-]+$." >&2
        echo "[ERROR] Example: ukg-1026-213005-4242" >&2
        exit 1
    fi
}

# Print a default run id with seconds and PID resolution.
# Minute resolution let two runs started in the same minute delete
# each other's resources.
ukg_default_run_id() {
    echo "ukg-$(date +%m%d-%H%M%S)-$$"
}

# Print the zone fallback list, one zone per line, duplicates removed.
# Override with the space-separated UKG_ZONES variable.
# The default keeps the zones that earlier B-path runs used.
# Call it after GCP_ZONE is set.
ukg_zone_list() {
    local zones="${UKG_ZONES:-${GCP_ZONE:-} us-east4-b us-east4-c us-central1-a us-central1-b}"
    echo "${zones}" | tr -s '[:space:]' '\n' | awk 'NF && !seen[$0]++'
}

# Print a fresh private path for a serial console log file.
# Predictable /tmp paths can be planted symlinks. mktemp refuses to
# follow them. Override the base directory with UKG_LOG_DIR.
# Arg $1: name prefix, usually the run id.
ukg_mk_serial_log() {
    local dir="${UKG_LOG_DIR:-/tmp/opencode}"
    mkdir -p "${dir}"
    mktemp "${dir}/${1}-serial-XXXXXX.log"
}

# Install the teardown trap. Call it before the first resource create.
# On any exit path the handler tears the run down. This keeps the
# AGENTS.md promise that the session which creates a resource deletes
# it. Export KEEP_RES=1 to keep resources instead. The handler then
# prints the exact teardown command in a loud banner.
# INT and TERM exit with the conventional codes, so a killed run
# never reports success. The EXIT trap then runs the handler once.
ukg_install_teardown_trap() {
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'ukg_teardown_trap_handler' EXIT
}

# Trap handler body. It preserves the original exit code of the
# script. It clears the traps first, so INT does not run teardown
# twice through the EXIT trap. After teardown and the leak audit it
# stores the run evidence. That keeps the store step after the audit
# on every exit path. Ticket c16353731a.
ukg_teardown_trap_handler() {
    local rc=$?
    trap - EXIT INT TERM
    echo ""
    if [ "${KEEP_RES:-0}" = "1" ]; then
        echo "=================================================="
        echo "[KEEP_RES=1] Resources for run ${UKG_RUN} were NOT deleted."
        echo "They keep billing. Delete them when you finish:"
        echo "    ${UKG_TEARDOWN_SCRIPT} ${UKG_RUN}"
        echo "=================================================="
    else
        echo "[INFO] Teardown trap fired for run ${UKG_RUN}..."
        "${UKG_TEARDOWN_SCRIPT}" "${UKG_RUN}" \
            || echo "[WARN] Teardown reported failures. Run it again by hand:" >&2
    fi
    ukg_store_evidence "${rc}"
    exit "${rc}"
}

# Store the combined serial log in the Fossil unversioned artifact
# store and print the exact ticket update command. The teardown trap
# handler calls this after teardown and the leak audit, so a stored
# pass always follows a clean audit. The step never changes the run
# result. A store failure prints the manual commands instead. Ticket
# c16353731a. The run scripts set:
#   UKG_EVIDENCE_LOG  combined serial log path. Required for a store.
#   UKG_GATE          gate name, for example A3 or B3.
#   UKG_TICKET        mapped ticket uuid prefix. Each run script
#                     defaults it to its phase ticket and an operator
#                     can override it for a regression run.
# Arg $1: the original exit code of the run script. Returns 0 always,
# so a store problem cannot mask the gate result.
ukg_store_evidence() {
    local rc="${1:-0}"
    local result="FAIL"
    if [ "${rc}" -eq 0 ]; then
        result="PASS"
    fi
    local log="${UKG_EVIDENCE_LOG:-}"
    if [ -z "${log}" ]; then
        return 0
    fi
    if [ ! -s "${log}" ]; then
        echo "[INFO] Evidence log ${log} is empty. Nothing stored."
        return 0
    fi
    local store_name="reports/${UKG_RUN}-serial.log"
    echo ""
    echo "=================================================="
    echo "[EVIDENCE] Storing ${result} evidence for run ${UKG_RUN}..."
    echo "=================================================="
    if (cd "${UKG_REPO_ROOT}" && fossil uv ls 2>/dev/null) | grep -Fxq "${store_name}"; then
        # Never overwrite stored evidence in silence. A rerun with a
        # reused run id replaces only on purpose.
        echo "[WARN] ${store_name} already exists in the artifact store." >&2
        echo "[WARN] Keep the first copy, or replace it on purpose:" >&2
        echo "[WARN]     fossil uv replace ${log} --as ${store_name}" >&2
    elif (cd "${UKG_REPO_ROOT}" && fossil uv add "${log}" --as "${store_name}") >/dev/null 2>&1; then
        echo "[OK] Stored ${store_name}. Confirmation from fossil uv ls -l:"
        (cd "${UKG_REPO_ROOT}" && fossil uv ls -l) | grep -F "${store_name}" || true
    else
        echo "[WARN] fossil uv add failed. Store the evidence by hand" >&2
        echo "[WARN] from the checkout root:" >&2
        echo "[WARN]     fossil uv add ${log} --as ${store_name}" >&2
    fi
    local result_line
    result_line="Gate ${UKG_GATE:-gate} run ${UKG_RUN}: ${result}. Evidence: ${store_name} (fossil uv)."
    echo "[EVIDENCE] Result line: ${result_line}"
    if [ -n "${UKG_TICKET:-}" ]; then
        echo "[EVIDENCE] Update the mapped ticket from the checkout root:"
        # The \n\n stays literal. fossil ticket change turns it into
        # the blank line before the result. printf keeps it exact.
        printf '    fossil ticket change %s +comment "\\n\\n%s"\n' \
            "${UKG_TICKET}" "${result_line}"
    else
        echo "[EVIDENCE] UKG_TICKET is not set, so no mapped ticket" \
            "command can be printed." >&2
    fi
    return 0
}

# Shared pair gate check: net half. The net gate passes only when
# both serial logs show GATE_NET_PASS and neither log shows GATE_FAIL.
# Used by the A3 and B3 run scripts. Tickets 4347c2a8e6 L2.
# Arg $1: server log path. Arg $2: client log path. Returns 0 on pass.
ukg_gate_net_pass() {
    local srv_log="$1"
    local cli_log="$2"
    grep -q "GATE_NET_PASS" "${cli_log}" 2>/dev/null || return 1
    grep -q "GATE_NET_PASS" "${srv_log}" 2>/dev/null || return 1
    if grep -q "GATE_FAIL" "${cli_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_FAIL" "${srv_log}" 2>/dev/null; then
        return 1
    fi
    return 0
}

# Shared pair gate check: ping half. The ping gate passes when either
# serial log shows GATE_PING_PASS.
# Arg $1: server log path. Arg $2: client log path. Returns 0 on pass.
ukg_gate_ping_pass() {
    local srv_log="$1"
    local cli_log="$2"
    if grep -q "GATE_PING_PASS" "${cli_log}" 2>/dev/null; then
        return 0
    fi
    if grep -q "GATE_PING_PASS" "${srv_log}" 2>/dev/null; then
        return 0
    fi
    return 1
}

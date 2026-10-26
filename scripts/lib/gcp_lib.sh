#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Shared helpers for the Google Cloud run scripts. Source this file.
# It defines functions. Only the two UKG_LIB_* variables are set on
# source. No external dependencies beyond coreutils and gawk.
#
# Sourced by: gcp_run_a1.sh gcp_run_a2.sh gcp_run_a3.sh gcp_run_b1.sh
# gcp_run_b2.sh gcp_run_b3.sh gcp_build_image.sh gcp_teardown.sh
# Tickets: 1c76a66b26 4a7a2ca6f5 e3effabf78 4347c2a8e6

# Directory of this file and the path of the teardown script.
UKG_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UKG_TEARDOWN_SCRIPT="${UKG_LIB_DIR}/../gcp_teardown.sh"

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
    # Word splitting here is intended. Do not quote ${zones}.
    local zones="${UKG_ZONES:-${GCP_ZONE:-} us-east4-b us-east4-c us-central1-a us-central1-b}"
    printf '%s\n' ${zones} | awk 'NF && !seen[$0]++'
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
# twice through the EXIT trap.
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
    exit "${rc}"
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

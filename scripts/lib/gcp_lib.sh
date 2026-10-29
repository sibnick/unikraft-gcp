#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Shared helpers for the Google Cloud run scripts. Source this file.
# It defines functions. Only the three UKG_* path variables at the
# top are set on source. Dependencies: coreutils, gawk, and fossil
# for the evidence store step.
#
# Sourced by: gcp_run_a1.sh gcp_run_a2.sh gcp_run_a3.sh gcp_run_b1.sh
# gcp_run_b2.sh gcp_run_b3.sh gcp_run_c1.sh gcp_run_c2.sh
# gcp_build_image.sh gcp_teardown.sh
# Tickets: 1c76a66b26 4a7a2ca6f5 e3effabf78 4347c2a8e6 c16353731a
# 06ea1706a7 5e6a2acd8a 74936642ce cf185ab6fd afc0a00b06 3a59523e76
# 0bc13135d7 eec304d8cf 4c76b4d84d a5874d85fe

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

# Name suffixes that run scripts derive from a run id: <run>-srv and
# <run>-cli instances, <run>-img, <run>-srv-img, and <run>-cli-img
# images, and the <run>-allow-internal firewall rule. Teardown builds
# its anchored name filter from this list. A run script that creates
# a resource with a new derived name must extend this list in the
# same change. Ticket 74936642ce.
UKG_RUN_SUFFIXES="srv cli img srv-img cli-img allow-internal"

# Print the anchored resource-name regex for one run id. The regex
# matches the run id itself and every name derived from it through
# UKG_RUN_SUFFIXES. It never matches a different run whose id extends
# this one, so run ukg-test cannot match a ukg-test-2 resource.
# Arg $1: run id.
ukg_run_name_regex() {
    local id="$1" alt="" s
    for s in ${UKG_RUN_SUFFIXES}; do
        alt="${alt:+${alt}|}${s}"
    done
    echo "^${id}(-(${alt}))?\$"
}

# Validate a run-scoped custom image name. The name must be a GCP-safe
# name and must start with the run id and a dash. That keeps every
# local path built from the name inside the build directory and limits
# image delete operations to images this run owns. Ticket 5e6a2acd8a.
# Arg $1: image name. Arg $2: run id that owns the image. The function
# exits 1 on an invalid name.
ukg_validate_image_name() {
    local name="${1:-}" owner="${2:-}"
    if [[ ! "${name}" =~ ^[a-z]([-a-z0-9]{0,61}[a-z0-9])?$ ]] ||
       [[ ! "${name}" == "${owner}-"* ]]; then
        echo "[ERROR] Invalid image name: '${name}'." >&2
        echo "[ERROR] The name must match the GCP image name rules" >&2
        echo "[ERROR] (lowercase, at most 63 chars, no trailing dash)" >&2
        echo "[ERROR] and start with the run id '${owner}-'." >&2
        exit 1
    fi
}

# Print the delete filter for one run id. The filter matches the run
# label that AGENTS.md section 5 requires on every billable resource,
# or a name from the anchored run-name regex. The name branch stays
# because Compute Engine auto-creates instance boot disks without
# labels, and only the name ties those disks to the run. Ticket
# 3a59523e76.
# Arg $1: run id.
ukg_run_delete_filter() {
    local id="$1"
    echo "(labels.run=\"${id}\") OR (name ~ \"$(ukg_run_name_regex "${id}")\")"
}

# Classify one gcloud storage rm result. An absent object is a
# nothing-to-delete case, not a failure. Auth, permission, and other
# service errors must be recorded by the caller instead of hidden
# behind "|| true". Ticket afc0a00b06.
# Arg $1: exit code of the rm call. Arg $2: combined output.
# Returns 0 when the result is acceptable, 1 on a real error.
ukg_gcs_rm_ok() {
    local rc="$1" out="${2:-}"
    if [ "${rc}" -eq 0 ]; then
        return 0
    fi
    # gcloud reports an absent object as "The following URLs matched
    # no objects or files" (current text, seen in run
    # ukg-1029-010754), as "No URLs matched" (older text), or as an
    # HTTP 404 "not found" for an exact name. All mean the object is
    # already gone.
    if printf '%s' "${out}" | grep -qiE "matched no objects|No URLs matched|404|not found"; then
        return 0
    fi
    return 1
}

# Capture one instance's full serial console output into one log file
# without ever destroying a good log. gcloud get-serial-port-output
# returns the whole log on every call, so the fetch replaces the file.
# A failed fetch must not truncate the previous content: the redirect
# goes to a temporary file that replaces the log only on success.
# Ticket 0bc13135d7. Needs GCP_ZONE and GCP_PROJECT from the caller.
# Arg $1: instance name. Arg $2: log path. Returns the gcloud status.
ukg_serial_capture() {
    local inst="$1" log="$2"
    local tmp="${log}.new"
    if gcloud compute instances get-serial-port-output "${inst}" \
        --zone="${GCP_ZONE}" --project="${GCP_PROJECT}" \
        >"${tmp}" 2>&1; then
        mv -f "${tmp}" "${log}"
        return 0
    fi
    rm -f "${tmp}"
    echo "[WARN] Serial capture failed for ${inst}. Keeping the previous log." >&2
    return 1
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
# on every exit path. Ticket c16353731a. The teardown exit code is
# captured, so a failed cleanup or leak audit downgrades the stored
# result and turns a zero gate exit code into a failure. Ticket
# cf185ab6fd.
ukg_teardown_trap_handler() {
    local rc=$?
    local cleanup_rc=0
    local kept=0
    trap - EXIT INT TERM
    echo ""
    if [ "${KEEP_RES:-0}" = "1" ]; then
        kept=1
        echo "=================================================="
        echo "[KEEP_RES=1] Resources for run ${UKG_RUN} were NOT deleted."
        echo "They keep billing. Delete them when you finish:"
        echo "    ${UKG_TEARDOWN_SCRIPT} ${UKG_RUN}"
        echo "=================================================="
    else
        echo "[INFO] Teardown trap fired for run ${UKG_RUN}..."
        # Capture with ||, not if-bang. After "! cmd" the shell sets
        # $? to the negated status, so an if-bang capture would always
        # read 0.
        "${UKG_TEARDOWN_SCRIPT}" "${UKG_RUN}" || cleanup_rc=$?
        if [ "${cleanup_rc}" -ne 0 ]; then
            echo "[WARN] Teardown or leak audit failed (rc=${cleanup_rc})." >&2
            echo "[WARN] Run it again by hand:" >&2
            echo "[WARN]     ${UKG_TEARDOWN_SCRIPT} ${UKG_RUN}" >&2
        fi
    fi
    ukg_store_evidence "${rc}" "${cleanup_rc}" "${kept}"
    if [ "${rc}" -eq 0 ] && [ "${cleanup_rc}" -ne 0 ]; then
        echo "[WARN] The gate passed, but the run is reported as" \
            "failed because cleanup or the leak audit failed." >&2
        rc=1
    fi
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
# Arg $1: the original exit code of the run script. Arg $2: the
# teardown and leak audit exit code, 0 for a clean cleanup. Arg $3:
# 1 when KEEP_RES kept the resources. A stored PASS requires all
# three to be clean: a gate that passed over a failed cleanup or a
# retained run stores PARTIAL instead. Ticket cf185ab6fd. Returns 0
# always, so a store problem cannot mask the gate result.
ukg_store_evidence() {
    local rc="${1:-0}" cleanup_rc="${2:-0}" kept="${3:-0}"
    local result="FAIL"
    if [ "${rc}" -eq 0 ]; then
        result="PASS"
    fi
    if [ "${result}" = "PASS" ] &&
       { [ "${cleanup_rc}" -ne 0 ] || [ "${kept}" -eq 1 ]; }; then
        result="PARTIAL"
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
    local stored=0
    echo ""
    echo "=================================================="
    echo "[EVIDENCE] Storing ${result} evidence for run ${UKG_RUN}..."
    echo "=================================================="
    if (cd "${UKG_REPO_ROOT}" && fossil uv ls 2>/dev/null) | grep -Fxq "${store_name}"; then
        # Never overwrite stored evidence in silence. A rerun with a
        # reused run id replaces only on purpose. The stored artifact
        # belongs to the earlier run, so it must not be cited for this
        # one.
        echo "[WARN] ${store_name} already exists in the artifact store." >&2
        echo "[WARN] Keep the first copy, or replace it on purpose:" >&2
        echo "[WARN]     fossil uv replace ${log} --as ${store_name}" >&2
    elif (cd "${UKG_REPO_ROOT}" && fossil uv add "${log}" --as "${store_name}") >/dev/null 2>&1; then
        echo "[OK] Stored ${store_name}. Confirmation from fossil uv ls -l:"
        (cd "${UKG_REPO_ROOT}" && fossil uv ls -l) | grep -F "${store_name}" || true
        stored=1
    else
        echo "[WARN] fossil uv add failed. Store the evidence by hand" >&2
        echo "[WARN] from the checkout root:" >&2
        echo "[WARN]     fossil uv add ${log} --as ${store_name}" >&2
    fi
    local result_line
    result_line="Gate ${UKG_GATE:-gate} run ${UKG_RUN}: ${result}."
    if [ "${result}" = "PARTIAL" ]; then
        if [ "${kept}" -eq 1 ]; then
            result_line="${result_line} The gate passed, but KEEP_RES kept billable resources."
        else
            result_line="${result_line} The gate passed, but teardown or the leak audit failed."
        fi
    fi
    if [ "${stored}" -eq 1 ]; then
        result_line="${result_line} Evidence: ${store_name} (fossil uv)."
        echo "[EVIDENCE] Result line: ${result_line}"
        if [ -n "${UKG_TICKET:-}" ]; then
            echo "[EVIDENCE] Update the mapped ticket from the checkout root:"
            # The \n\n stays literal. fossil ticket change turns it
            # into the blank line before the result. printf keeps it
            # exact.
            printf '    fossil ticket change %s +comment "\\n\\n%s"\n' \
                "${UKG_TICKET}" "${result_line}"
        else
            echo "[EVIDENCE] UKG_TICKET is not set, so no mapped ticket" \
                "command can be printed." >&2
        fi
    else
        # Ticket 4 of the 2026-10-29 follow-up audit (ticket
        # eec304d8cf): never cite an artifact this run did not store.
        # No ready-to-paste ticket command either, because pasting it
        # would put unsupported evidence on the ticket.
        result_line="${result_line} Evidence: NOT STORED, the local log is ${log}."
        echo "[EVIDENCE] Result line: ${result_line}"
        echo "[EVIDENCE] No ticket command is printed: store the log" \
            "with fossil uv first, then add the result to the ticket" \
            "by hand." >&2
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
# serial log shows GATE_PING_PASS and neither log shows GATE_FAIL. The
# failure guard matches the net gate. A run whose ping passed and that
# failed later must not report a pass. Ticket 06ea1706a7.
# Arg $1: server log path. Arg $2: client log path. Returns 0 on pass.
ukg_gate_ping_pass() {
    local srv_log="$1"
    local cli_log="$2"
    if grep -q "GATE_FAIL" "${cli_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_FAIL" "${srv_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_PING_PASS" "${cli_log}" 2>/dev/null; then
        return 0
    fi
    if grep -q "GATE_PING_PASS" "${srv_log}" 2>/dev/null; then
        return 0
    fi
    return 1
}

# Authoritative pair gate check: HTTP benchmark (gate C1). Passes only
# when the server log shows the unikernel served a page
# (GATE_HTTP_FIRST_200), the client log shows a clean wrk run
# (GATE_HTTP_BENCH_PASS), and neither log shows a GATE_FAIL or a
# client-side GATE_HTTP_BENCH_FAIL. The server half proves the unikernel
# datapath; the client half proves the measured load. Ticket 4c76b4d84d.
# Arg $1: server log path. Arg $2: client log path. Returns 0 on pass.
ukg_gate_http_pass() {
    local srv_log="$1"
    local cli_log="$2"
    grep -q "GATE_HTTP_FIRST_200" "${srv_log}" 2>/dev/null || return 1
    grep -q "GATE_HTTP_BENCH_PASS" "${cli_log}" 2>/dev/null || return 1
    if grep -q "GATE_FAIL" "${cli_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_FAIL" "${srv_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_HTTP_BENCH_FAIL" "${cli_log}" 2>/dev/null; then
        return 1
    fi
    return 0
}

# Authoritative pair gate check: UDP latency (gate C2). Passes only
# when the server log shows the unikernel echo server came up
# (GATE_HFT_READY), the client log shows a lossless RTT run
# (GATE_HFT_RTT_PASS), and neither log shows a failure marker.
# Ticket 238f7cbd91 a5874d85fe.
# Arg $1: server log path. Arg $2: client log path. Returns 0 on pass.
ukg_gate_hft_pass() {
    local srv_log="$1"
    local cli_log="$2"
    grep -q "GATE_HFT_READY" "${srv_log}" 2>/dev/null || return 1
    grep -q "GATE_HFT_RTT_PASS" "${cli_log}" 2>/dev/null || return 1
    if grep -q "GATE_FAIL" "${cli_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_FAIL" "${srv_log}" 2>/dev/null; then
        return 1
    fi
    if grep -q "GATE_HFT_RTT_FAIL" "${cli_log}" 2>/dev/null; then
        return 1
    fi
    return 0
}

# Decide the wrk verdict from one wrk report file. Returns 0 (pass) only
# when Requests/sec is a number above zero and the report shows no
# socket errors and no non-2xx responses. A wrk run that finished but
# hit socket errors, timeouts, or non-2xx responses is a fail, the same
# rule the ENA httpreply tables apply (they report a Socket Errors
# column and expect 0). Arg $1: report file. The file may be a raw
# GCP serial log, so strip carriage returns before parsing.
# Ticket 4c76b4d84d.
ukg_wrk_ok() {
    local rep="$1"
    local rps
    [ -s "${rep}" ] || return 1
    rps="$(tr -d '\r' <"${rep}" | awk '/Requests\/sec:/ { v=$2 } END { print v }')"
    case "${rps}" in
        ''|*[!0-9.]*) return 1 ;;
    esac
    awk -v r="${rps}" 'BEGIN { exit !(r > 0) }' || return 1
    if tr -d '\r' <"${rep}" | grep -qE "Socket errors:.*[1-9]"; then
        return 1
    fi
    if tr -d '\r' <"${rep}" | grep -qE "Non-2xx responses:"; then
        return 1
    fi
    return 0
}

# Parse one combined C1/C2 serial log into a single CSV data row
# without header. Columns:
#   run,gate,stack,machine,concurrency,throughput,p50_ms,p99_ms,p999_ms,errors,loss_pct
# Gate detection: a wrk Requests/sec report is C1, a PROBE_RESULT line
# is C2. Throughput is requests per second for C1 and packets per
# second for C2. The parser reads the stack from the combined log
# header (target:/server: unikraft|linux) and the run id from the
# client banner. The machine type never reaches the serial console,
# so the caller passes it as the second argument. Percentiles are
# normalized to milliseconds; wrk prints us, ms, or s by magnitude.
# Returns 1 when the log carries neither report. Ticket a1134318f5.
ukg_ab_parse() {
    local log="$1"
    local machine="${2:-unknown}"
    local gate=""
    [ -s "${log}" ] || return 1
    if grep -q "Requests/sec:" "${log}" 2>/dev/null; then
        gate="C1"
    elif grep -q "PROBE_RESULT" "${log}" 2>/dev/null; then
        gate="C2"
    else
        return 1
    fi
    awk -v gate="${gate}" -v machine="${machine}" \
        -v defrun="$(basename "${log}" .log)" '
    function toms(v,   n) {
        n = v + 0
        if (v ~ /ms$/) return n
        if (v ~ /us$/) return n / 1000
        if (v ~ /s$/)  return n * 1000
        return n
    }
    { gsub(/\r/, "") }
    run == "" && match($0, /run=ukg-[A-Za-z0-9-]+/) {
        run = substr($0, RSTART + 4, RLENGTH - 4)
    }
    stack == "" && ($0 ~ /target: (unikraft|linux)/ || $0 ~ /server: (unikraft|linux)/) {
        stack = ($0 ~ /unikraft/) ? "unikraft" : "linux"
    }
    gate == "C1" && $0 ~ /threads and [0-9]+ connections/ {
        line = $0
        gsub(/[^0-9]+/, " ", line)
        split(line, na, " ")
        conc = na[2]
    }
    gate == "C1" && $0 ~ /^Requests\/sec:/ { rps = $2 }
    gate == "C1" && $0 ~ /^[ \t]*50%[ \t]/    { p50 = toms($2) }
    gate == "C1" && $0 ~ /^[ \t]*90%[ \t]/    { p90 = toms($2) }
    gate == "C1" && $0 ~ /^[ \t]*99%[ \t]/    { p99 = toms($2) }
    gate == "C1" && $0 ~ /^[ \t]*99\.9%[ \t]/ { p999 = toms($2); has999 = 1 }
    gate == "C1" && $0 ~ /Socket errors:/ {
        line = $0
        sub(/.*Socket errors:/, "", line)
        gsub(/[^0-9]+/, " ", line)
        n = split(line, ea, " ")
        for (i = 1; i <= n; i++) if (ea[i] != "") sockerr += ea[i]
    }
    gate == "C1" && $0 ~ /^Non-2xx responses:/ { non2xx = $NF }
    gate == "C2" && $0 ~ /^PROBE_RESULT / {
        n = split($0, kv, /[ =]+/)
        for (i = 2; i + 1 <= n; i += 2) val[kv[i]] = kv[i + 1]
    }
    END {
        if (run == "") run = defrun
        if (stack == "") stack = "unknown"
        if (gate == "C1") {
            if (rps == "") exit 1
            v999 = has999 ? sprintf("%.3f", p999) : ""
            printf "%s,%s,%s,%s,%s,%s,%.3f,%.3f,%s,%d,\n", \
                run, gate, stack, machine, conc, rps, \
                p50, p99, v999, sockerr + non2xx
        } else {
            if (!("p50_us" in val)) exit 1
            printf "%s,%s,%s,%s,1,%s,%.3f,%.3f,%.3f,%d,%s\n", \
                run, gate, stack, machine, val["rate"], \
                val["p50_us"] / 1000, val["p99_us"] / 1000, \
                val["p999_us"] / 1000, val["sent"] - val["recv"], \
                val["loss_pct"]
        }
    }
    ' "${log}"
}

#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build the combined A/B benchmark table from stored C1 and C2 serial
# logs. Reads one or more combined serial logs (the file the run
# scripts store as reports/RUNID-serial.log), parses each into one CSV
# row with ukg_ab_parse, and prints a CSV to stdout. With --md it also
# writes the markdown summary table for the wiki Home benchmark
# section. With --store it writes both files and registers them in the
# Fossil unversioned store as reports/RUNID-ab.csv and
# reports/RUNID-ab.md. No Google Cloud calls. Ticket a1134318f5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=./lib/gcp_lib.sh
source "${SCRIPT_DIR}/lib/gcp_lib.sh"

usage() {
    echo "usage: $(basename "$0") [--machine NAME] [--run ID] [--md FILE] [--store] LOG [LOG...]" >&2
    echo "  --machine NAME  target machine label for the rows (default unknown)" >&2
    echo "  --run ID        run id used for stored artifact names" >&2
    echo "  --md FILE       also write the markdown summary table to FILE" >&2
    echo "  --store         store CSV and markdown as reports/RUNID-ab.* in fossil uv" >&2
}

MACHINE="unknown"
RUN_OVERRIDE=""
MD_FILE=""
STORE=0
LOGS=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --machine) MACHINE="${2:?--machine needs a value}"; shift 2 ;;
        --run)     RUN_OVERRIDE="${2:?--run needs a value}"; shift 2 ;;
        --md)      MD_FILE="${2:?--md needs a value}"; shift 2 ;;
        --store)   STORE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*)        echo "[ERROR] Unknown option: $1" >&2; usage; exit 1 ;;
        *)         LOGS+=("$1"); shift ;;
    esac
done

if [ "${#LOGS[@]}" -eq 0 ]; then
    usage
    exit 1
fi

CSV_HEADER="run,gate,stack,machine,concurrency,throughput,p50_ms,p99_ms,p999_ms,errors,loss_pct"

rows=()
for log in "${LOGS[@]}"; do
    if ! row="$(ukg_ab_parse "${log}" "${MACHINE}")"; then
        echo "[WARN] No C1 wrk report and no C2 probe report in ${log}. Skipped." >&2
        continue
    fi
    rows+=("${row}")
done

if [ "${#rows[@]}" -eq 0 ]; then
    echo "[ERROR] None of the input logs carried a parseable report." >&2
    exit 1
fi

print_csv() {
    printf '%s\n' "${CSV_HEADER}"
    printf '%s\n' "${rows[@]}"
}

# Render the same rows as a markdown table. The wiki Home benchmark
# section takes this block verbatim.
print_md() {
    echo "| run | gate | stack | machine | conc | throughput | p50 ms | p99 ms | p99.9 ms | errors | loss pct |"
    echo "| :-- | :-- | :-- | :-- | --: | --: | --: | --: | --: | --: | --: |"
    local row
    for row in "${rows[@]}"; do
        echo "${row}" | awk -F, '{ printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n", \
            $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11 }'
    done
}

# Resolve the run id used for stored names: --run wins, else all rows
# must agree on one run id.
resolve_run() {
    if [ -n "${RUN_OVERRIDE}" ]; then
        printf '%s' "${RUN_OVERRIDE}"
        return 0
    fi
    local run="" row
    for row in "${rows[@]}"; do
        candidate="${row%%,*}"
        if [ -z "${run}" ]; then
            run="${candidate}"
        elif [ "${run}" != "${candidate}" ]; then
            echo "[ERROR] Rows span runs ${run} and ${candidate}. Pass --run for a mixed report." >&2
            return 1
        fi
    done
    printf '%s' "${run}"
}

if [ -n "${MD_FILE}" ]; then
    print_md > "${MD_FILE}"
    echo "[INFO] Markdown summary written to ${MD_FILE}" >&2
fi

if [ "${STORE}" -eq 1 ]; then
    run_id="$(resolve_run)"
    ukg_validate_run_id "${run_id}"
    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ukg-ab-XXXXXX")"
    # shellcheck disable=SC2064  # expand TMP at mktemp time, not trap time.
    trap "rm -rf '${tmp_dir}'" EXIT
    csv_file="${tmp_dir}/ab.csv"
    md_file="${tmp_dir}/ab.md"
    print_csv > "${csv_file}"
    print_md > "${md_file}"
    store_one() {
        local file="$1" name="$2"
        if (cd "${UKG_REPO_ROOT}" && fossil uv ls 2>/dev/null) | grep -Fxq "${name}"; then
            (cd "${UKG_REPO_ROOT}" && fossil uv replace "${file}" --as "${name}")
        else
            (cd "${UKG_REPO_ROOT}" && fossil uv add "${file}" --as "${name}")
        fi
    }
    if store_one "${csv_file}" "reports/${run_id}-ab.csv" \
        && store_one "${md_file}" "reports/${run_id}-ab.md"; then
        echo "[OK] Stored reports/${run_id}-ab.csv and reports/${run_id}-ab.md."
    else
        echo "[ERROR] fossil uv store failed. Store by hand:" >&2
        echo "[ERROR]     fossil uv add ${csv_file} --as reports/${run_id}-ab.csv" >&2
        exit 1
    fi
fi

print_csv

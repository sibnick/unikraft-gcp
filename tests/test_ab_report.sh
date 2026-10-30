#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Host test for the A/B report parser (ukg_ab_parse in
# scripts/lib/gcp_lib.sh) and the report driver
# scripts/gcp_ab_report.sh. Fixtures mirror real wrk and probe
# output, including CRLF serial endings and us/ms/s latency unit
# variants. No Fossil and no Google Cloud calls happen.
# Ticket a1134318f5.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=../scripts/lib/gcp_lib.sh
source "${ROOT_DIR}/scripts/lib/gcp_lib.sh"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ukg-ab-report-XXXXXX")"
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

# 1. C1 fixture: a combined log with a wrk --latency report. The p99
# uses us and the p99.9 uses s, so the ms conversion is exercised.
printf '=== SERVER SERIAL CONSOLE OUTPUT (ukg-fix1-srv, target: unikraft) ===\r\nGATE_HTTP_READY\r\nGATE_HTTP_FIRST_200\r\n\r\n=== CLIENT SERIAL CONSOLE OUTPUT (ukg-fix1-cli) ===\r\nC1CLIENT start run=ukg-fix1 target=10.150.0.10 threads=2 connections=50 duration=10s\r\nWRK_BEGIN\r\nRunning 10s test @ http://10.150.0.10/\r\n  2 threads and 50 connections\r\n  Thread Stats   Avg      Stdev     Max   +/- Stdev\r\n    Latency     4.05ms    1.85ms  20.11ms   84.33%%\r\n    Req/Sec     6.21k     1.22k    8.50k    69.50%%\r\n  Latency Distribution\r\n     50%%    3.86ms\r\n     90%%    5.70ms\r\n     99%%   911.50us\r\n    99.9%%    1.05s\r\n  61842 requests in 10.10s, 48.34MB read\r\nRequests/sec:   6123.45\r\nTransfer/sec:      4.79MB\r\nWRK_END\r\nGATE_HTTP_BENCH_PASS requests_sec=6123.45\r\n' \
    > "${WORK}/c1.log"
# 2. C1 fixture with error counters: 49 timeouts plus 12 non-2xx.
printf '=== SERVER SERIAL CONSOLE OUTPUT (ukg-fixe-srv, target: linux) ===\r\n\r\n=== CLIENT SERIAL CONSOLE OUTPUT (ukg-fixe-cli) ===\r\nC1CLIENT start run=ukg-fixe target=10.150.0.10 threads=2 connections=25 duration=10s\r\n  2 threads and 25 connections\r\n  Latency Distribution\r\n     50%%    1.00ms\r\n     90%%    2.00ms\r\n     99%%    4.00ms\r\n    99.9%%    8.00ms\r\nSocket errors: connect 0, read 0, write 0, timeout 49\r\nNon-2xx responses: 12\r\nRequests/sec:   900.00\r\n' \
    > "${WORK}/c1_err.log"
# 3. C2 fixture: combined log with the probe PROBE_RESULT summary.
printf '=== SERVER SERIAL CONSOLE OUTPUT (ukg-fix2-srv, server: linux) ===\r\nGATE_LINUX_HFT_UP\r\n\r\n=== CLIENT SERIAL CONSOLE OUTPUT (ukg-fix2-cli) ===\r\nC2CLIENT start run=ukg-fix2 target=10.150.0.10 probes=20000\r\nPROBE_BEGIN probes=20000\r\nPROBE_RESULT loss_pct=0.01 p50_us=155.20 p90_us=210.00 p99_us=480.50 p999_us=900.00 mean_us=170.11 sent=20000 recv=19999 rate=4851\r\nPROBE_END\r\nGATE_HFT_RTT_FAIL: measured run finished with packet loss or errors\r\n' \
    > "${WORK}/c2.log"
printf 'boot noise only\r\n' > "${WORK}/noise.log"
: > "${WORK}/empty.log"

row="$(ukg_ab_parse "${WORK}/c1.log" "c3-standard-4")"
expected="ukg-fix1,C1,unikraft,c3-standard-4,50,6123.45,3.860,0.911,1050.000,0,"
if [ "${row}" = "${expected}" ]; then
    ok "C1 row parses with unit-normalized percentiles"
else
    bad "C1 row parses with unit-normalized percentiles (got ${row})"
fi

row="$(ukg_ab_parse "${WORK}/c1_err.log" "c3-standard-4")"
expected="ukg-fixe,C1,linux,c3-standard-4,25,900.00,1.000,4.000,8.000,61,"
if [ "${row}" = "${expected}" ]; then
    ok "C1 row sums socket errors and non-2xx responses"
else
    bad "C1 row sums socket errors and non-2xx responses (got ${row})"
fi

# 2b. Plain wrk --latency on Ubuntu 24.04 prints 50/75/90/99 only,
# no 99.9% line, exactly as captured in run ukg-1030-004040-290322.
# The p99.9 CSV field must stay blank, never a misleading zero.
printf '=== SERVER SERIAL CONSOLE OUTPUT (ukg-fixg-srv, target: unikraft) ===\r\n\r\n=== CLIENT SERIAL CONSOLE OUTPUT (ukg-fixg-cli) ===\r\nC1CLIENT start run=ukg-fixg target=10.150.0.10 threads=2 connections=50 duration=10s\r\n  2 threads and 50 connections\r\n  Latency Distribution\r\n     50%%  283.00us\r\n     75%%  358.00us\r\n     90%%  472.00us\r\n     99%%  837.00us\r\n  1051700 requests in 10.10s, 150.45MB read\r\nRequests/sec: 104130.35\r\n' \
    > "${WORK}/c1_no999.log"
row="$(ukg_ab_parse "${WORK}/c1_no999.log" "c3-standard-4")"
expected="ukg-fixg,C1,unikraft,c3-standard-4,50,104130.35,0.283,0.837,,0,"
if [ "${row}" = "${expected}" ]; then
    ok "C1 row keeps p99.9 blank when wrk printed no 99.9%% line"
else
    bad "C1 row keeps p99.9 blank when wrk printed no 99.9%% line (got ${row})"
fi

row="$(ukg_ab_parse "${WORK}/c2.log" "c3-standard-4")"
expected="ukg-fix2,C2,linux,c3-standard-4,1,4851,0.155,0.480,0.900,1,0.01"
if [ "${row}" = "${expected}" ]; then
    ok "C2 row parses from PROBE_RESULT with loss counted as errors"
else
    bad "C2 row parses from PROBE_RESULT with loss counted as errors (got ${row})"
fi

ukg_ab_parse "${WORK}/noise.log" x >/dev/null 2>&1 && bad "noise log returns non-zero" \
    || ok "noise log returns non-zero"
ukg_ab_parse "${WORK}/empty.log" x >/dev/null 2>&1 && bad "empty log returns non-zero" \
    || ok "empty log returns non-zero"

# 4. Driver: header plus one row per parseable log, noise skipped.
DRIVER="${ROOT_DIR}/scripts/gcp_ab_report.sh"
out="$("${DRIVER}" --machine c3-standard-4 "${WORK}/c1.log" "${WORK}/c2.log" "${WORK}/noise.log" 2>/dev/null)"
hdr="$(printf '%s\n' "${out}" | head -1)"
nrows="$(printf '%s\n' "${out}" | tail -n +2 | grep -c '^ukg-')"
if [ "${hdr}" = "run,gate,stack,machine,concurrency,throughput,p50_ms,p99_ms,p999_ms,errors,loss_pct" ]; then
    ok "driver prints the documented CSV header"
else
    bad "driver prints the documented CSV header (got ${hdr})"
fi
if [ "${nrows}" -eq 2 ]; then
    ok "driver emits one row per parseable log"
else
    bad "driver emits one row per parseable log (got ${nrows})"
fi

"${DRIVER}" --machine x "${WORK}/noise.log" >/dev/null 2>&1 && bad "driver fails when no log parses" \
    || ok "driver fails when no log parses"

# 5. Driver markdown table: one header rule plus one line per row.
md="${WORK}/summary.md"
"${DRIVER}" --machine c3-standard-4 --md "${md}" "${WORK}/c1.log" "${WORK}/c2.log" >/dev/null 2>&1
mdatapairs="$(grep -c '^| ukg-' "${md}")"
if [ "${mdatapairs}" -eq 2 ]; then
    ok "driver markdown carries one table line per row"
else
    bad "driver markdown carries one table line per row (got ${mdatapairs})"
fi

echo ""
echo "test_ab_report: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[ "${FAIL_COUNT}" -eq 0 ]

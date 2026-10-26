#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Deprecated wrapper for the Phase A3 pair run.
# This script once kept a stale, half-implemented copy of the pair
# flow. The copy drifted from the real flow and could strand a
# firewall rule on rerun. It now only delegates to gcp_run_a3.sh.
# Ticket e3effabf78 M3.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "[INFO] gcp_run_pair.sh is a thin wrapper. Delegating to gcp_run_a3.sh."
exec "${SCRIPT_DIR}/gcp_run_a3.sh" "$@"

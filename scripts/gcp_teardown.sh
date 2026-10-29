#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Tear down billable Google Cloud resources and run the leak audit.
# The script is rerunnable. Every delete stays guarded with a
# "delete if exists" flow. Every list capture is guarded too, so one
# failed gcloud call cannot abort the script and strand the resource
# classes that come later. Failures are counted and reported with a
# non-zero exit code at the end. Tickets: 8c0f80d874 4a7a2ca6f5
# e3effabf78 (M5) 4347c2a8e6 (N4) f3df948f2c 74936642ce
# afc0a00b06 3a59523e76.
# Usage: gcp_teardown.sh <run-id>. The run id is mandatory. The
# project-wide sweep needs the explicit opt-in UKG_TEARDOWN_ALL=1.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck source=./lib/gcp_lib.sh
source "${SCRIPT_DIR}/lib/gcp_lib.sh"

if [ -f "${ROOT_DIR}/.env" ]; then
    # shellcheck source=/dev/null
    source "${ROOT_DIR}/.env"
fi

RUN_ID="${1:-}"

# A run id is mandatory. This check runs before any gcloud call, so
# a wrong invocation cannot touch the project. A non-empty run id
# must pass validation. Otherwise a loose pattern such as "."
# matches resources that this project rule must never delete.
# Ticket f3df948f2c.
if [ -n "${RUN_ID}" ]; then
    ukg_validate_run_id "${RUN_ID}"
    # Match the run label or an exact-boundary name. AGENTS.md
    # section 5 requires the run label on every billable resource,
    # so the label branch is the primary one. The anchored name
    # branch stays: Compute Engine auto-creates instance boot disks
    # without labels, and only the name ties them to the run. The
    # anchored regex keeps run ukg-test from matching ukg-test-2
    # resources. Tickets 74936642ce 3a59523e76.
    NAME_FILTER="$(ukg_run_delete_filter "${RUN_ID}")"
elif [ "${UKG_TEARDOWN_ALL:-0}" = "1" ]; then
    # Explicit opt-in for the project-wide sweep. It deletes every
    # ukg- resource and storage object in the project.
    NAME_FILTER='name ~ "^ukg-"'
else
    echo "[ERROR] Missing run id." >&2
    echo "[ERROR] A teardown without a run id deletes every" >&2
    echo "[ERROR] ukg- resource in the project. That can kill a" >&2
    echo "[ERROR] run owned by another session." >&2
    echo "[ERROR] Usage: gcp_teardown.sh <run-id>" >&2
    echo "[ERROR] Example: gcp_teardown.sh ukg-1026-213005-4242" >&2
    echo "[ERROR] To sweep every ukg- resource on purpose, run:" >&2
    echo "[ERROR] UKG_TEARDOWN_ALL=1 gcp_teardown.sh" >&2
    exit 2
fi

GCP_PROJECT="${GCP_PROJECT:-$(gcloud config get-value project 2>/dev/null || true)}"
GCP_ZONE="${GCP_ZONE:-$(gcloud config get-value compute/zone 2>/dev/null || true)}"
GCP_BUCKET="${GCP_BUCKET:-}"

# One filter style for this whole script: lowercase field name with
# a double-quoted regex. The audit always covers every ukg- resource,
# even when the delete pass was scoped to one run id.
AUDIT_FILTER='name ~ "^ukg-"'

FAILURES=0
LEAK=0

record_failure() {
    echo "[WARN] $*" >&2
    FAILURES=$((FAILURES + 1))
}

echo "=================================================="
echo "Google Cloud Teardown & Leak Audit"
echo "Project: ${GCP_PROJECT}"
echo "Zone:    ${GCP_ZONE}"
if [ -n "${RUN_ID}" ]; then
    echo "Run ID:  ${RUN_ID}"
elif [ "${UKG_TEARDOWN_ALL:-0}" = "1" ]; then
    echo "[WARNING] Project-wide sweep active (UKG_TEARDOWN_ALL=1)."
    echo "[WARNING] This run deletes every ukg- resource in the"
    echo "[WARNING] project, also from other sessions."
fi
echo "=================================================="

# Project-wide sweep gate. The sweep deletes other sessions' work, so
# it lists the current ukg- resources first and asks for a typed
# confirmation that names the project. Without a terminal it refuses.
# UKG_TEARDOWN_FORCE=1 skips the prompt when the review happened
# elsewhere, for example in an approved automation.
# Ticket 3a59523e76.
print_sweep_class() {
    local label="$1"
    shift
    local out
    if ! out="$("$@" --project="${GCP_PROJECT}" \
        --filter="${AUDIT_FILTER}" --format="value(name)" 2>/dev/null)"; then
        echo "  ${label}: list failed, content unknown."
        return 0
    fi
    if [ -n "${out}" ]; then
        echo "  ${label}:"
        printf '%s\n' "${out}" | sed 's/^/    /'
    else
        echo "  ${label}: none."
    fi
}

confirm_sweep() {
    local reply
    if [ "${UKG_TEARDOWN_FORCE:-0}" = "1" ]; then
        echo "[WARNING] UKG_TEARDOWN_FORCE=1: sweep proceeds without review."
        return 0
    fi
    echo "[INFO] Resources the sweep would delete:"
    print_sweep_class "instances" gcloud compute instances list
    print_sweep_class "images" gcloud compute images list --no-standard-images
    print_sweep_class "disks" gcloud compute disks list
    print_sweep_class "firewall rules" gcloud compute firewall-rules list
    echo ""
    if [ ! -t 0 ]; then
        echo "[ERROR] The sweep confirmation needs a terminal." >&2
        echo "[ERROR] Run this command interactively, or set" >&2
        echo "[ERROR] UKG_TEARDOWN_FORCE=1 when the review happened" >&2
        echo "[ERROR] elsewhere." >&2
        exit 1
    fi
    printf "Type SWEEP %s to delete every ukg- resource listed above.\n" \
        "${GCP_PROJECT}"
    read -r reply
    if [ "${reply}" != "SWEEP ${GCP_PROJECT}" ]; then
        echo "[ABORT] Confirmation text did not match. Nothing was deleted."
        exit 1
    fi
}

if [ -z "${RUN_ID}" ] && [ "${UKG_TEARDOWN_ALL:-0}" = "1" ]; then
    confirm_sweep
fi

# 1. Instances
delete_instances() {
    echo "[INFO] Deleting instances..."
    local list
    list="$(gcloud compute instances list --project="${GCP_PROJECT}" \
        --filter="${NAME_FILTER}" --format="value(name,zone)")" \
        || { record_failure "Could not list instances. Skipping this class."; return 0; }
    if [ -z "${list}" ]; then
        echo "No instances found to delete."
        return 0
    fi
    local name zone
    while read -r name zone; do
        [ -z "${name}" ] && continue
        echo "Deleting instance ${name} in ${zone}..."
        gcloud compute instances delete "${name}" --zone="${zone}" \
            --project="${GCP_PROJECT}" --quiet \
            || record_failure "Instance delete failed (already gone?): ${name} in ${zone}"
    done <<< "${list}"
}

# 2. Images
delete_images() {
    echo "[INFO] Deleting images..."
    local list
    list="$(gcloud compute images list --project="${GCP_PROJECT}" --no-standard-images \
        --filter="${NAME_FILTER}" --format="value(name)")" \
        || { record_failure "Could not list images. Skipping this class."; return 0; }
    if [ -z "${list}" ]; then
        echo "No images found to delete."
        return 0
    fi
    local img
    while read -r img; do
        [ -z "${img}" ] && continue
        echo "Deleting image ${img}..."
        gcloud compute images delete "${img}" --project="${GCP_PROJECT}" --quiet \
            || record_failure "Image delete failed (already gone?): ${img}"
    done <<< "${list}"
}

# 3. Disks
delete_disks() {
    echo "[INFO] Deleting disks..."
    local list
    list="$(gcloud compute disks list --project="${GCP_PROJECT}" \
        --filter="${NAME_FILTER}" --format="value(name,zone)")" \
        || { record_failure "Could not list disks. Skipping this class."; return 0; }
    if [ -z "${list}" ]; then
        echo "No orphaned disks found to delete."
        return 0
    fi
    local name zone
    while read -r name zone; do
        [ -z "${name}" ] && continue
        echo "Deleting disk ${name} in ${zone}..."
        gcloud compute disks delete "${name}" --zone="${zone}" \
            --project="${GCP_PROJECT}" --quiet \
            || record_failure "Disk delete failed (already gone?): ${name} in ${zone}"
    done <<< "${list}"
}

# 4. Firewall rules
delete_firewall_rules() {
    echo "[INFO] Deleting firewall rules..."
    local list
    list="$(gcloud compute firewall-rules list --project="${GCP_PROJECT}" \
        --filter="${NAME_FILTER}" --format="value(name)")" \
        || { record_failure "Could not list firewall rules. Skipping this class."; return 0; }
    if [ -z "${list}" ]; then
        echo "No firewall rules found to delete."
        return 0
    fi
    local rule
    while read -r rule; do
        [ -z "${rule}" ] && continue
        echo "Deleting firewall rule ${rule}..."
        gcloud compute firewall-rules delete "${rule}" --project="${GCP_PROJECT}" --quiet \
            || record_failure "Firewall delete failed (already gone?): ${rule}"
    done <<< "${list}"
}

# 5. Cloud Storage objects

# Remove one Cloud Storage object or wildcard and classify errors.
# An absent object is a normal nothing-to-delete case. Auth,
# permission, or service errors are recorded as teardown failures
# instead of disappearing behind "|| true". Ticket afc0a00b06.
remove_gcs_object() {
    local url="$1"
    shift
    local out="" rc=0
    out="$(gcloud storage rm "$@" "${url}" 2>&1)" || rc=$?
    if ukg_gcs_rm_ok "${rc}" "${out}"; then
        return 0
    fi
    record_failure "Cloud Storage delete failed for ${url} (rc=${rc})."
    printf '%s\n' "${out}" >&2
    # Return 0: a recorded failure already fails the run at the end,
    # and the teardown must continue with the remaining classes and
    # the leak audit under set -e.
    return 0
}

clean_gcs_objects() {
    if [ -z "${GCP_BUCKET}" ]; then
        echo "[INFO] GCP_BUCKET is not set. Skipping Cloud Storage cleanup."
        echo "[INFO] The leak audit reports storage as unknown in this case."
        return 0
    fi
    echo "[INFO] Cleaning Cloud Storage objects in gs://${GCP_BUCKET}..."
    # Run-scoped cleanup removes the exact objects that the run
    # scripts upload: one per run-derived image name. An earlier
    # ${RUN_ID}* wildcard could also delete another run's objects,
    # for example ukg-test-2 objects during a ukg-test teardown.
    # Ticket 74936642ce. A custom image name outside the derived set
    # is out of reach here. The leak audit below still reports it.
    if [ -n "${RUN_ID}" ]; then
        local obj
        for obj in "${RUN_ID}-img-disk.tar.gz" \
                   "${RUN_ID}-srv-img-disk.tar.gz" \
                   "${RUN_ID}-cli-img-disk.tar.gz"; do
            remove_gcs_object "gs://${GCP_BUCKET}/${obj}"
        done
        return 0
    fi
    # A wildcard with no match is the "No URLs matched" case handled
    # inside remove_gcs_object. The sweep scope is the whole ukg-
    # prefix, so it recurses into prefixes.
    remove_gcs_object "gs://${GCP_BUCKET}/ukg-*" --recursive
}

# 6. Leak Audit per skills/gcloud-compute/SKILL.md Section 5
audit_class() {
    local label="$1"
    shift
    local out=""
    if ! out="$("$@" 2>/dev/null)"; then
        echo "[FAIL] Audit query failed for ${label}. The result is unknown, not clean." >&2
        LEAK=1
        record_failure "Audit query failed: ${label}"
        return 0
    fi
    if [ -n "${out}" ]; then
        echo "[FAIL] Leaked ${label} found:"
        echo "${out}"
        LEAK=1
    else
        echo "[OK] No leaked ${label}."
    fi
}

audit_gcs_objects() {
    if [ -z "${GCP_BUCKET}" ]; then
        # Every run script uploads a disk image to Cloud Storage, so
        # an unset bucket makes the storage result unknown. Unknown
        # is not clean. Ticket afc0a00b06.
        echo "[FAIL] GCP_BUCKET is not set. Cloud Storage objects are" \
            "unknown, not clean." >&2
        LEAK=1
        record_failure "Audit skipped: Cloud Storage objects (GCP_BUCKET unset)"
        return 0
    fi
    local out=""
    if ! out="$(gcloud storage ls "gs://${GCP_BUCKET}/" 2>/dev/null)"; then
        echo "[FAIL] Audit query failed for Cloud Storage objects. The result is unknown, not clean." >&2
        LEAK=1
        record_failure "Audit query failed: Cloud Storage objects"
        return 0
    fi
    local matches
    matches="$(printf '%s\n' "${out}" | grep "/ukg-" || true)"
    if [ -n "${matches}" ]; then
        echo "[FAIL] Leaked Cloud Storage objects found:"
        echo "${matches}"
        LEAK=1
    else
        echo "[OK] No leaked Cloud Storage objects."
    fi
}

run_leak_audit() {
    echo ""
    echo "=================================================="
    echo "[AUDIT] Running Leak Audit..."
    echo "=================================================="
    audit_class "instances" gcloud compute instances list \
        --project="${GCP_PROJECT}" --filter="${AUDIT_FILTER}" --format="value(name)"
    audit_class "images" gcloud compute images list \
        --project="${GCP_PROJECT}" --no-standard-images --filter="${AUDIT_FILTER}" --format="value(name)"
    audit_class "disks" gcloud compute disks list \
        --project="${GCP_PROJECT}" --filter="${AUDIT_FILTER}" --format="value(name)"
    audit_class "global addresses" gcloud compute addresses list \
        --project="${GCP_PROJECT}" --filter="${AUDIT_FILTER} AND address_type=GLOBAL" --format="value(name)"
    audit_class "firewall rules" gcloud compute firewall-rules list \
        --project="${GCP_PROJECT}" --filter="${AUDIT_FILTER}" --format="value(name)"
    audit_gcs_objects
}

delete_instances
delete_images
delete_disks
delete_firewall_rules
clean_gcs_objects
run_leak_audit

echo ""
if [ "${LEAK}" -eq 0 ] && [ "${FAILURES}" -eq 0 ]; then
    echo "[AUDIT PASS] All resource lists empty. Zero leaked resources."
    exit 0
fi
if [ "${LEAK}" -ne 0 ]; then
    echo "[AUDIT FAIL] Leak audit detected remaining or unverified resources!" >&2
fi
if [ "${FAILURES}" -ne 0 ]; then
    echo "[TEARDOWN FAIL] ${FAILURES} teardown or audit steps failed." >&2
    echo "Fix the reported errors and run this script again." >&2
fi
exit 1

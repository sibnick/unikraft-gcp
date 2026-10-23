#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Tear down billable Google Cloud resources and run leak audit.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -f "${ROOT_DIR}/.env" ]; then
    # shellcheck source=/dev/null
    source "${ROOT_DIR}/.env"
fi

GCP_PROJECT="${GCP_PROJECT:-$(gcloud config get-value project 2>/dev/null || true)}"
GCP_ZONE="${GCP_ZONE:-$(gcloud config get-value compute/zone 2>/dev/null || true)}"
GCP_BUCKET="${GCP_BUCKET:-}"

RUN_ID="${1:-}"

echo "=================================================="
echo "Google Cloud Teardown & Leak Audit"
echo "Project: ${GCP_PROJECT}"
echo "Zone:    ${GCP_ZONE}"
if [ -n "${RUN_ID}" ]; then
    echo "Run ID:  ${RUN_ID}"
fi
echo "=================================================="

# 1. Instances
echo "[INFO] Deleting instances..."
if [ -n "${RUN_ID}" ]; then
    INSTANCES=$(gcloud compute instances list --project="${GCP_PROJECT}" --filter="name ~ '^${RUN_ID}'" --format="value(name,zone)")
else
    INSTANCES=$(gcloud compute instances list --project="${GCP_PROJECT}" --filter="name ~ '^ukg-'" --format="value(name,zone)")
fi

if [ -n "${INSTANCES}" ]; then
    while read -r name zone; do
        [ -z "${name}" ] && continue
        echo "Deleting instance ${name} in ${zone}..."
        gcloud compute instances delete "${name}" --zone="${zone}" --project="${GCP_PROJECT}" --quiet
    done <<< "${INSTANCES}"
else
    echo "No instances found to delete."
fi

# 2. Images
echo "[INFO] Deleting images..."
if [ -n "${RUN_ID}" ]; then
    IMAGES=$(gcloud compute images list --project="${GCP_PROJECT}" --no-standard-images --filter="name ~ '^${RUN_ID}'" --format="value(name)")
else
    IMAGES=$(gcloud compute images list --project="${GCP_PROJECT}" --no-standard-images --filter="name ~ '^ukg-'" --format="value(name)")
fi

if [ -n "${IMAGES}" ]; then
    while read -r img; do
        [ -z "${img}" ] && continue
        echo "Deleting image ${img}..."
        gcloud compute images delete "${img}" --project="${GCP_PROJECT}" --quiet
    done <<< "${IMAGES}"
else
    echo "No images found to delete."
fi

# 3. Disks
echo "[INFO] Deleting disks..."
if [ -n "${RUN_ID}" ]; then
    DISKS=$(gcloud compute disks list --project="${GCP_PROJECT}" --filter="name ~ '^${RUN_ID}'" --format="value(name,zone)")
else
    DISKS=$(gcloud compute disks list --project="${GCP_PROJECT}" --filter="name ~ '^ukg-'" --format="value(name,zone)")
fi

if [ -n "${DISKS}" ]; then
    while read -r name zone; do
        [ -z "${name}" ] && continue
        echo "Deleting disk ${name} in ${zone}..."
        gcloud compute disks delete "${name}" --zone="${zone}" --project="${GCP_PROJECT}" --quiet
    done <<< "${DISKS}"
else
    echo "No orphaned disks found to delete."
fi

# 4. Firewall rules
echo "[INFO] Deleting firewall rules..."
if [ -n "${RUN_ID}" ]; then
    RULES=$(gcloud compute firewall-rules list --project="${GCP_PROJECT}" --filter="name ~ '^${RUN_ID}'" --format="value(name)")
else
    RULES=$(gcloud compute firewall-rules list --project="${GCP_PROJECT}" --filter="name ~ '^ukg-'" --format="value(name)")
fi

if [ -n "${RULES}" ]; then
    while read -r rule; do
        [ -z "${rule}" ] && continue
        echo "Deleting firewall rule ${rule}..."
        gcloud compute firewall-rules delete "${rule}" --project="${GCP_PROJECT}" --quiet
    done <<< "${RULES}"
else
    echo "No firewall rules found to delete."
fi

# 5. Cloud Storage Objects
if [ -n "${GCP_BUCKET}" ]; then
    echo "[INFO] Cleaning Cloud Storage objects in gs://${GCP_BUCKET}..."
    if [ -n "${RUN_ID}" ]; then
        gcloud storage rm --recursive "gs://${GCP_BUCKET}/${RUN_ID}*" 2>/dev/null || true
    else
        gcloud storage rm --recursive "gs://${GCP_BUCKET}/ukg-*" 2>/dev/null || true
    fi
fi

# 6. Leak Audit per skills/gcloud-compute/SKILL.md Section 5
echo ""
echo "=================================================="
echo "[AUDIT] Running Leak Audit..."
echo "=================================================="

LEAK=0

INSTANCES_LEAK=$(gcloud compute instances list --project="${GCP_PROJECT}" --filter='NAME~"^ukg-"' --format="value(name)")
if [ -n "${INSTANCES_LEAK}" ]; then
    echo "[FAIL] Leaked instances found:"
    echo "${INSTANCES_LEAK}"
    LEAK=1
fi

IMAGES_LEAK=$(gcloud compute images list --project="${GCP_PROJECT}" --no-standard-images --filter="name ~ '^ukg-'" --format="value(name)")
if [ -n "${IMAGES_LEAK}" ]; then
    echo "[FAIL] Leaked images found:"
    echo "${IMAGES_LEAK}"
    LEAK=1
fi

DISKS_LEAK=$(gcloud compute disks list --project="${GCP_PROJECT}" --filter='NAME~"^ukg-"' --format="value(name)")
if [ -n "${DISKS_LEAK}" ]; then
    echo "[FAIL] Leaked disks found:"
    echo "${DISKS_LEAK}"
    LEAK=1
fi

RULES_LEAK=$(gcloud compute firewall-rules list --project="${GCP_PROJECT}" --filter='NAME~"^ukg-"' --format="value(name)")
if [ -n "${RULES_LEAK}" ]; then
    echo "[FAIL] Leaked firewall rules found:"
    echo "${RULES_LEAK}"
    LEAK=1
fi

if [ "${LEAK}" -eq 0 ]; then
    echo "[AUDIT PASS] All resource lists empty. Zero leaked resources."
    exit 0
else
    echo "[AUDIT FAIL] Leak audit detected remaining resources!" >&2
    exit 1
fi

#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Prepare pinned Unikraft core at RELEASE-0.21.0 (7351f8b563c9e6773da9fdd5bacde6ac91e32112)
# and lib-lwip at RELEASE-0.21.0 (ec55ae17618feeb57c8c10109bcf5c42723e8e95).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UK_DEST="${ROOT_DIR}/.unikraft/unikraft"
UK_TAG="RELEASE-0.21.0"
UK_COMMIT="7351f8b563c9e6773da9fdd5bacde6ac91e32112"
UK_URL="https://github.com/unikraft/unikraft.git"

LWIP_DEST="${ROOT_DIR}/.unikraft/libs/lwip"
LWIP_TAG="RELEASE-0.21.0"
LWIP_COMMIT="ec55ae17618feeb57c8c10109bcf5c42723e8e95"
LWIP_URL="https://github.com/unikraft/lib-lwip.git"

mkdir -p "${ROOT_DIR}/.unikraft/libs"

# 1. Unikraft core
if [ ! -d "${UK_DEST}/.git" ]; then
    echo "[INFO] Cloning Unikraft core at ${UK_TAG}..."
    git clone --depth 1 --branch "${UK_TAG}" "${UK_URL}" "${UK_DEST}"
else
    echo "[INFO] Verifying Unikraft core commit..."
    CURRENT_REV="$(git -C "${UK_DEST}" rev-parse HEAD 2>/dev/null || true)"
    if [ "${CURRENT_REV}" != "${UK_COMMIT}" ]; then
        echo "[INFO] Checking out pinned commit ${UK_COMMIT}..."
        git -C "${UK_DEST}" fetch origin "${UK_TAG}"
        git -C "${UK_DEST}" checkout "${UK_COMMIT}"
    fi
fi

# 2. lib-lwip
if [ ! -d "${LWIP_DEST}/.git" ]; then
    echo "[INFO] Cloning lib-lwip at ${LWIP_TAG}..."
    git clone --depth 1 --branch "${LWIP_TAG}" "${LWIP_URL}" "${LWIP_DEST}"
else
    echo "[INFO] Verifying lib-lwip commit..."
    CURRENT_REV="$(git -C "${LWIP_DEST}" rev-parse HEAD 2>/dev/null || true)"
    if [ "${CURRENT_REV}" != "${LWIP_COMMIT}" ]; then
        echo "[INFO] Checking out pinned commit ${LWIP_COMMIT}..."
        git -C "${LWIP_DEST}" fetch origin "${LWIP_TAG}"
        git -C "${LWIP_DEST}" checkout "${LWIP_COMMIT}"
    fi
fi

# Apply patches from patches/ directory if present
if [ -d "${ROOT_DIR}/patches" ]; then
    for patch_file in "${ROOT_DIR}/patches"/*.patch; do
        [ -f "${patch_file}" ] || continue
        patch_name="$(basename "${patch_file}")"
        if [[ "${patch_name}" == lib-lwip* ]]; then
            TARGET_REPO="${LWIP_DEST}"
        else
            TARGET_REPO="${UK_DEST}"
        fi
        echo "[INFO] Checking patch ${patch_name} against $(basename "${TARGET_REPO}")..."
        if git -C "${TARGET_REPO}" apply --check "${patch_file}" &>/dev/null; then
            echo "[INFO] Applying patch ${patch_name}..."
            git -C "${TARGET_REPO}" apply "${patch_file}"
        else
            echo "[INFO] Patch ${patch_name} already applied or not applicable."
        fi
    done
fi

echo "[SUCCESS] Unikraft core is ready at ${UK_DEST} (commit: ${UK_COMMIT})."
echo "[SUCCESS] lib-lwip is ready at ${LWIP_DEST} (commit: ${LWIP_COMMIT})."

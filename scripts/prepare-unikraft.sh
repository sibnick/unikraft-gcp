#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Prepare pinned Unikraft core at RELEASE-0.21.0 (7351f8b563c9e6773da9fdd5bacde6ac91e32112).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UK_DEST="${ROOT_DIR}/.unikraft/unikraft"
UK_TAG="RELEASE-0.21.0"
UK_COMMIT="7351f8b563c9e6773da9fdd5bacde6ac91e32112"
UK_URL="https://github.com/unikraft/unikraft.git"

mkdir -p "${ROOT_DIR}/.unikraft"

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

# Apply patches from patches/ directory if present
if [ -d "${ROOT_DIR}/patches" ]; then
    for patch_file in "${ROOT_DIR}/patches"/*.patch; do
        [ -f "${patch_file}" ] || continue
        echo "[INFO] Checking patch $(basename "${patch_file}")..."
        if git -C "${UK_DEST}" apply --check "${patch_file}" &>/dev/null; then
            echo "[INFO] Applying patch $(basename "${patch_file}")..."
            git -C "${UK_DEST}" apply "${patch_file}"
        else
            echo "[INFO] Patch $(basename "${patch_file}") already applied or not applicable."
        fi
    done
fi

echo "[SUCCESS] Unikraft core is ready at ${UK_DEST} (commit: ${UK_COMMIT})."

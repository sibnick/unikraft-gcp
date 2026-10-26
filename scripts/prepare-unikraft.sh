#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Prepare pinned Unikraft core at RELEASE-0.21.0 (7351f8b563c9e6773da9fdd5bacde6ac91e32112)
# and lib-lwip at RELEASE-0.21.0 (ec55ae17618feeb57c8c10109bcf5c42723e8e95).
# Fetch by commit hash, not by tag. A tag can move and then no longer
# contains the pinned commit. Ticket 4347c2a8e6 L3.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UK_DEST="${ROOT_DIR}/.unikraft/unikraft"
UK_COMMIT="7351f8b563c9e6773da9fdd5bacde6ac91e32112"
UK_URL="https://github.com/unikraft/unikraft.git"

LWIP_DEST="${ROOT_DIR}/.unikraft/libs/lwip"
LWIP_COMMIT="ec55ae17618feeb57c8c10109bcf5c42723e8e95"
LWIP_URL="https://github.com/unikraft/lib-lwip.git"

# Clone or update one repository to the pinned commit. Shallow fetch
# of the commit keeps the download small.
# Arg $1: remote URL. Arg $2: destination dir. Arg $3: pinned commit.
ukg_fetch_pinned() {
    local url="$1"
    local dest="$2"
    local commit="$3"
    if [ ! -d "${dest}/.git" ]; then
        echo "[INFO] Fetching pinned commit ${commit} into ${dest}..."
        git init "${dest}"
        git -C "${dest}" remote add origin "${url}"
    fi
    if [ "$(git -C "${dest}" rev-parse HEAD 2>/dev/null || true)" = "${commit}" ]; then
        echo "[INFO] ${dest} is already at ${commit}."
        return 0
    fi
    echo "[INFO] Fetching commit ${commit} from ${url}..."
    git -C "${dest}" fetch --depth 1 origin "${commit}"
    git -C "${dest}" checkout --detach FETCH_HEAD
}

mkdir -p "${ROOT_DIR}/.unikraft/libs"

# 1. Unikraft core
ukg_fetch_pinned "${UK_URL}" "${UK_DEST}" "${UK_COMMIT}"

# 2. lib-lwip
ukg_fetch_pinned "${LWIP_URL}" "${LWIP_DEST}" "${LWIP_COMMIT}"

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
        elif git -C "${TARGET_REPO}" apply --reverse --check "${patch_file}" &>/dev/null; then
            # The reverse patch applies cleanly, so the patch is
            # already in the tree. Ticket 4347c2a8e6 L4.
            echo "[INFO] Patch ${patch_name} is already applied. Skipping."
        else
            # Neither forward nor reverse applies. Silent continuation
            # would build a tree without a required patch.
            echo "[ERROR] Patch ${patch_name} does not apply to $(basename "${TARGET_REPO}")." >&2
            echo "[ERROR] The checkout may not match the pinned commit." >&2
            exit 1
        fi
    done
fi

echo "[SUCCESS] Unikraft core is ready at ${UK_DEST} (commit: ${UK_COMMIT})."
echo "[SUCCESS] lib-lwip is ready at ${LWIP_DEST} (commit: ${LWIP_COMMIT})."

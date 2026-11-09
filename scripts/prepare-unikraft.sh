#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Prepare the Unikraft core from the owner fork (github.com/sibnick/unikraft,
# branch feature/gcp-patches), pinned at commit
# 399ebe383380786b69b328a480fe7a728771ed88. The fork carries the former
# patches/0001-0007 core patch set as committed history on top of upstream
# staging, so no patch step runs for the core any more. Fetch by commit
# hash, same discipline as the other pins. The former 0.21 core patch set is
# archived under patches/archive/core-release-0.21/.
# lib-lwip at RELEASE-0.21.0 (ec55ae17618feeb57c8c10109bcf5c42723e8e95), and the
# two libraries that samples/nginx needs: app-elfloader at RELEASE-0.21.0
# (6ecbcd08d85b4688112a3b275a9c8446cf59e25d) and lib-libelf (libelf) at the commit
# the proven ENA nginx build carried (c03d4ceb22a2b9e9799238f9c277a14284d313ad).
# Fetch by commit hash, not by tag. A tag can move and then no longer
# contains the pinned commit. Ticket 4347c2a8e6 L3. The upstream
# app-elfloader tag RELEASE-0.21.0 already moved once, so the hash pin is
# mandatory. Ticket 4ecf9dcedf.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
UK_DEST="${ROOT_DIR}/.unikraft/unikraft"
# Owner fork of unikraft. Branch feature/gcp-patches; the hash below is its
# tip at the time of the fork migration. Push new work to the branch, then
# update this hash.
UK_COMMIT="399ebe383380786b69b328a480fe7a728771ed88"
UK_URL="git@github.com:sibnick/unikraft.git"

LWIP_DEST="${ROOT_DIR}/.unikraft/libs/lwip"
LWIP_COMMIT="ec55ae17618feeb57c8c10109bcf5c42723e8e95"
LWIP_URL="https://github.com/unikraft/lib-lwip.git"

# app-elfloader is not in the KraftKit package index. The samples/nginx
# build uses this clone. Ticket 4ecf9dcedf.
APPELFLOADER_DEST="${ROOT_DIR}/.unikraft/libs/app-elfloader"
APPELFLOADER_COMMIT="6ecbcd08d85b4688112a3b275a9c8446cf59e25d"
APPELFLOADER_URL="https://github.com/unikraft/app-elfloader.git"

# app-elfloader reads ELF program headers through libelf (unikraft/lib-libelf).
# This is the commit the proven ENA nginx build carried. Ticket 4ecf9dcedf.
LIBELF_DEST="${ROOT_DIR}/.unikraft/libs/libelf"
LIBELF_COMMIT="c03d4ceb22a2b9e9799238f9c277a14284d313ad"
LIBELF_URL="https://github.com/unikraft/lib-libelf.git"

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
    # The core pin moved from upstream to the owner fork. Re-point the
    # remote on every run so an existing checkout switches correctly.
    if [ "$(git -C "${dest}" remote get-url origin)" != "${url}" ]; then
        echo "[INFO] Switching origin of ${dest} to ${url}..."
        git -C "${dest}" remote set-url origin "${url}"
    fi
    if [ "$(git -C "${dest}" rev-parse HEAD 2>/dev/null || true)" = "${commit}" ]; then
        echo "[INFO] ${dest} is already at ${commit}."
        return 0
    fi
    echo "[INFO] Fetching commit ${commit} from ${url}..."
    git -C "${dest}" fetch --depth 1 origin "${commit}"
    # --force: an older run may have left patches applied in the working
    # tree (uncommitted). The pin carries its patches as commits, so local
    # changes to tracked files are stale build state, not user work.
    git -C "${dest}" checkout --force --detach FETCH_HEAD
}

mkdir -p "${ROOT_DIR}/.unikraft/libs"

# 1. Unikraft core
ukg_fetch_pinned "${UK_URL}" "${UK_DEST}" "${UK_COMMIT}"

# 2. lib-lwip
ukg_fetch_pinned "${LWIP_URL}" "${LWIP_DEST}" "${LWIP_COMMIT}"

# 3. app-elfloader (no patches)
ukg_fetch_pinned "${APPELFLOADER_URL}" "${APPELFLOADER_DEST}" "${APPELFLOADER_COMMIT}"

# 4. libelf (no patches)
ukg_fetch_pinned "${LIBELF_URL}" "${LIBELF_DEST}" "${LIBELF_COMMIT}"

# Apply patches from patches/ directory if present.
# Try the forward pass for one repository's patch set. Arg $1: target
# repo dir. Remaining args: patch files in apply order.
ukg_apply_forward() {
    local repo="$1"
    shift
    local pf
    for pf in "$@"; do
        git -C "${repo}" apply "${pf}" || return 1
    done
    return 0
}

# Unapply one repository's patch set. A later patch can depend on an
# earlier one, so undo in reverse order. Used to detect and roll back
# an already-patched tree. Arg $1: target repo dir. Remaining args:
# patch files in apply order.
ukg_unapply_reverse() {
    local repo="$1"
    shift
    local n i rc pf
    n=$#
    for i in $(seq "${n}" -1 1); do
        eval "pf=\${$i}"
        git -C "${repo}" apply --reverse "${pf}" 2>/dev/null
        rc=$?
        if [ "${rc}" -ne 0 ]; then
            echo "[ERROR] Cannot unapply $(basename "${pf}") during rollback." >&2
            echo "[ERROR] The tree may be half-patched. Fix it by hand." >&2
            return 1
        fi
    done
    return 0
}

# Apply one repository's patch set. If the whole set does not apply
# forward, test whether the tree already carries it: unapply the set
# in reverse order, then apply it again. When even that test fails,
# stop. Silent continuation would build a tree without a required
# patch. Ticket 4347c2a8e6 L4, e74d4f8b70.
# Arg $1: target repo dir. Remaining args: patch files in apply order.
ukg_apply_set() {
    local repo="$1"
    shift
    local pf
    if ukg_apply_forward "${repo}" "$@" 2>/dev/null; then
        for pf in "$@"; do
            echo "[INFO] Applied patch $(basename "${pf}")."
        done
        return 0
    fi
    if ukg_unapply_reverse "${repo}" "$@" 2>/dev/null; then
        echo "[INFO] Patch set is already applied to $(basename "${repo}"). Restoring."
        ukg_apply_forward "${repo}" "$@" || return 1
        return 0
    fi
    echo "[ERROR] The patch set does not apply to $(basename "${repo}")." >&2
    echo "[ERROR] The checkout may not match the pinned commit." >&2
    return 1
}

if [ -d "${ROOT_DIR}/patches" ]; then
    LWIP_PATCHES=()
    UK_PATCHES=()
    for patch_file in "${ROOT_DIR}"/patches/*.patch; do
        [ -f "${patch_file}" ] || continue
        patch_name="$(basename "${patch_file}")"
        if [[ "${patch_name}" == lib-lwip* ]]; then
            LWIP_PATCHES+=("${patch_file}")
        else
            UK_PATCHES+=("${patch_file}")
        fi
    done
    if [ "${#LWIP_PATCHES[@]}" -gt 0 ]; then
        echo "[INFO] Checking ${#LWIP_PATCHES[@]} patch(es) against lwip..."
        ukg_apply_set "${LWIP_DEST}" "${LWIP_PATCHES[@]}"
    fi
    if [ "${#UK_PATCHES[@]}" -gt 0 ]; then
        echo "[INFO] Checking ${#UK_PATCHES[@]} patch(es) against unikraft..."
        ukg_apply_set "${UK_DEST}" "${UK_PATCHES[@]}"
    fi
fi

echo "[SUCCESS] Unikraft core is ready at ${UK_DEST} (commit: ${UK_COMMIT})."
echo "[SUCCESS] lib-lwip is ready at ${LWIP_DEST} (commit: ${LWIP_COMMIT})."
echo "[SUCCESS] app-elfloader is ready at ${APPELFLOADER_DEST} (commit: ${APPELFLOADER_COMMIT})."
echo "[SUCCESS] libelf is ready at ${LIBELF_DEST} (commit: ${LIBELF_COMMIT})."

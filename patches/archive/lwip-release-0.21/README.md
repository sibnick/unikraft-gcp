# Archived lwIP patch set (lib-lwip RELEASE-0.21.0)

These seven patches carried the lib-lwip changes this project needed
at `RELEASE-0.21.0` (commit `ec55ae17`). They no longer apply.
`scripts/prepare-unikraft.sh` now fetches the owner fork
`github.com/sibnick/lib-lwip`, branch `feature/gcp-patches`. That
branch commits the same changes on top of `ec55ae17`.

Fork tip at migration time: `2cf0eb7cac8c75ca607acea96c79fbf8eaa51440`.

Proof of equivalence: `diff -r` of the fork branch tree against the
patched `ec55ae17` tree showed byte-identical content on 2026-11-09.
Gate evidence recorded on ticket `2f77ddd973`. The apply order of the
old set was the alphabetical order of these file names. Keep the files
for evidence and bisect use.

Note: `lib-lwip-mcscratch` and `lib-lwip-percore` ship numbered nested
patch files (`patches/0015`, `0016`, `0018`). Those files are content
inside the lib-lwip tree. They remain in the fork branch, applied by
lib-lwip at build time as before.
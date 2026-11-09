# Archived core patch set (RELEASE-0.21.0)

These seven patches carried the Unikraft core changes this project
needed at `RELEASE-0.21.0` (commit `7351f8b5`). They no longer apply.
`scripts/prepare-unikraft.sh` now fetches the owner fork
`github.com/sibnick/unikraft`, branch `feature/gcp-patches`. That
branch commits the same changes on top of upstream staging, which also
brings the upstream `lib/uklcpu` per-CPU work.

Fork tip at migration time: `399ebe383380786b69b328a480fe7a728771ed88`.

Proof of the port: branch `feature/gcp-patches` in the fork. Commits
`d50ed1e0`, `56176a55`, `3755790f`, `4bfedb84`, `f5f5b385`, `a1e39125`
(patch 0003, conflict in `lib/ukconsole/console.c` resolved), and
`399ebe38`. Validation on the fork must pass before this archive is
trusted as history. Keep the files for evidence and bisect use.
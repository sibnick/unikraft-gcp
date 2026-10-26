# OpenCode Agent Instructions & Repository Guidelines

This document is the single source of agent guidance for this
repository. OpenCode, Antigravity, GitHub Copilot, and other agents
that read the `AGENTS.md` standard all use this file. Do not fork it
into tool-specific copies. `CLAUDE.md` only points here.

---

## 1. Core Operating Principles

1. **Fossil SCM is Primary**: This repository uses **Fossil SCM** (not
   Git) for configuration management, issue tracking, and the wiki.
2. **The Workspace Is The Checkout**: `/home/nikolay/projects/unikraft-gcp`
   is an open Fossil checkout of `Unikraft-GCP.fossil`. Work directly
   in it. Never edit the `.fossil` file or `.fslckout` by hand.
3. **Simplified Technical English (STE)**: All prose (documentation,
   commit messages, ticket updates, release notes, code comments)
   follows ASD-STE100 principles.
4. **Safety & Non-Destruction**:
   - Never delete or modify `.fslckout` or `_FOSSIL_` database files.
   - Do not rewrite history. No rebase or reset. Use `fossil amend`
     or `fossil undo`.
   - Run SQL with the readonly flag: `fossil sql --readonly "..."`.
5. **Cloud Cost Discipline**: Google Cloud resources are billable.
   Follow section 5 without exception.

---

## 2. Project Context

Run Unikraft unikernels as native Google Cloud VMs with
cloud-accelerated networking. Path A uses the core virtio-net driver
on first or second generation machine sizes. Path B adds a gve driver
for third generation and newer sizes. The wiki `Home` page holds the
facts, phases, and scope. Gate markers and the evidence contract live
in `skills/unikraft-gcp-validation/SKILL.md`.

Key device facts: virtio-net is `1af4:1000` subsystem 1 (first and
second generation only). gVNIC is `1ae0:0042` (third generation and
newer). The portal serial console reads COM1. GCP is a standard
QEMU/KVM PCI model, so the existing `kvm` platform is the platform.

---

## 3. Writing Style: ASD-STE100

Apply these rules to all written prose, not to code or command syntax.

- **Active Voice**: Write "the parser processes the input".
- **Short Common Words**: `start`, `use`, `help`, `make sure`,
  `before`, `after`, `about`, `get`, `show`, `also`.
- **No Marketing Fluff**: Remove words like *seamless, robust,
  powerful, cutting-edge, effortless, next-generation*.
- **Sentence Limits**: Maximum 20 words per instruction sentence and
  25 words per descriptive sentence.
- **Punctuation**: Do not use semicolons. Split into separate
  sentences.
- **One Term Per Concept**: Keep naming consistent. Do not use
  multiple terms for the same entity.

See `skills/ste-writing/SKILL.md` for the slop list and checks.

---

## 4. Fossil SCM Workflows

### A. Environment Verification

```bash
fossil version
fossil status
fossil branch current
fossil timeline -n 10
```

### B. Daily Development Cycle

```bash
fossil changes      # modified tracked files
fossil extras       # untracked files not in the ignore list
fossil diff         # uncommitted workspace changes
fossil add <file>   # register new files only
fossil addremove    # track new files and flag deleted ones
fossil undo         # revert the last commit, update, or merge
```

- There is **no staging index**. `fossil commit` commits all modified
  tracked files.
- Default branch is **`trunk`**.

### C. Branching & Merging

```bash
fossil branch new feature/<name> trunk
fossil update feature/<name>
# develop, then:
fossil update trunk && fossil merge feature/<name>
fossil commit -m "Merge feature/<name> into trunk [Ticket <UUID>]"
```

### D. Commits

Use conventional prefixes: `feat(scope):`, `fix(scope):`, `docs:`,
`test(scope):`, `chore:`. Reference tickets in brackets:
`[Ticket <10-char-UUID>]`.

```bash
fossil commit -m "feat(image): add GRUB serial boot recipe [Ticket 4395182d17]"
```

> [!NOTE]
> Both remotes now exist. The Fossil sync remote is named `sibnick`:
> `ssh://nikolay@sibnick.men:2222//home/nikolay/Unikraft-GCP.fossil`.
> Keep the double slash after the port. A single slash makes Fossil
> send a relative path. Autosync runs on every command while this
> default is set. Use `fossil remote off` to work offline and
> `fossil remote sibnick` to resume. A warning about multiple open
> leaf check-ins on trunk is cosmetic. It names the empty initial
> check-in of the remote database. Checkouts resolve to the real tip.
> The Git mirror is `git@github.com:sibnick/unikraft-gcp.git`, wired
> through the `gitpush` setting. After merging work to trunk, run
> `scripts/git_export.sh`. It exports the pending check-ins, pushes
> the mirror, and fixes the mirror index. Follow
> `skills/fossil-git-sync/SKILL.md` for the full procedure.

---

## 5. Google Cloud Rules

1. Every billable resource name starts with `ukg-` and carries the
   label `run=<run-id>`.
2. The session that creates a resource deletes it. Run the leak audit
   from `skills/gcloud-compute/SKILL.md` before closing work.
3. Debug from the serial console first. It is the only reliable guest
   view on this platform.
4. Open firewall rules only for internal ranges during a run.
5. Never store project ids, buckets, or image names in tracked files.
   Use environment variables or an untracked `.env`.

---

## 6. Fossil Tickets

Vocabulary in this repository: type `Feature_Request` or
`Code_Defect`, priority `High|Medium|Low`, severity
`Important|Minor`, status `Open|Closed`.

```bash
fossil sql --readonly "SELECT substr(tkt_uuid,1,10) AS id, priority, status, title FROM ticket WHERE status='Open';"
fossil ticket add title "..." type "Feature_Request" priority "Medium" severity "Important" status "Open" subsystem "net" comment "..."
fossil ticket change <UUID> +comment "\n\n<result line>"
fossil ticket change <UUID> status "Closed" +comment "\n\nResolved in check-in [<HASH>]."
```

> [!CAUTION]
> Do not run `fossil ticket show <UUID>` without a report number. Use
> `fossil ticket show 0 "tkt_uuid LIKE '<UUID_PREFIX>%'"` or
> `fossil ticket history <UUID_PREFIX>`.

---

## 7. Wiki, Tech Notes, and Artifact Store

```bash
fossil wiki list
fossil wiki export "Home" /tmp/home.md
fossil wiki commit "Home" /tmp/home.md -M markdown
fossil uv add /tmp/<run-id>-serial.log --as reports/<run-id>-serial.log
fossil uv ls -l
```

Store every validation log, benchmark CSV, and build report in
`fossil uv` under `reports/`. Do not commit large binaries.

---

## 8. End-to-End Task Lifecycle for Subagents

One subagent drives one gate at a time. Complete the full cycle:

1. **Triage**: `fossil status && fossil branch current && fossil
   timeline -n 5`. Read the ticket and the wiki `Home` page.
2. **Branch**: `fossil branch new feature/<name> trunk && fossil
   update feature/<name>`.
3. **Develop & Test**: Implement, then run the host checks. Build with
   the pinned core.
4. **Cloud Gate Run**: Execute per
   `skills/unikraft-gcp-validation/SKILL.md`. Capture the serial log,
   grep the gate marker, tear down, and pass the leak audit.
5. **Track & Review**: `fossil addremove`, then inspect with
   `fossil status` and `fossil diff`.
6. **Commit**: conventional message with `[Ticket <UUID>]`.
7. **Close Ticket**: only when the gate passed with stored evidence.
8. **Merge to Trunk**: follow section 4C, then update the wiki when a
   fact changed.

If a step fails, stop. Record the failure on the ticket. Ask the
owner before retries that cost cloud time.

---

## 9. Skills Directory Reference

| Skill | Path | Description |
| :--- | :--- | :--- |
| **Fossil Overview** | `skills/fossil-overview/SKILL.md` | Master entry point, troubleshooting table, triage commands. |
| **Fossil SCM** | `skills/fossil-scm/SKILL.md` | Core SCM, checkout, addremove, commits, branch and merge rules, gotchas. |
| **Fossil Tickets** | `skills/fossil-tickets/SKILL.md` | Ticket creation, schema reference, SQL templates, lifecycle. |
| **Fossil Wiki** | `skills/fossil-wiki/SKILL.md` | Wiki pages, markdown formatting, timeline Tech Notes. |
| **Fossil Unversioned** | `skills/fossil-unversioned/SKILL.md` | Store logs, reports, and binaries in `fossil uv`. |
| **Fossil Git Sync** | `skills/fossil-git-sync/SKILL.md` | Mirror this repository to Git and GitHub when created. |
| **STE Writing** | `skills/ste-writing/SKILL.md` | ASD-STE100 guidelines and slop checks. |
| **GCloud Compute** | `skills/gcloud-compute/SKILL.md` | GCP lifecycle, naming, serial console, teardown audit, errors. |
| **GCP Custom Images** | `skills/gcp-custom-images/SKILL.md` | Unikernel to bootable GCP image: disk, GRUB, import. |
| **GCP Validation** | `skills/unikraft-gcp-validation/SKILL.md` | Gated phases, gate markers, run scripts, evidence contract. |

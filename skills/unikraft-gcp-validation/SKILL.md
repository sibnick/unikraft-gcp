---
name: unikraft-gcp-validation
description: Instructions for AI agents on running the gated validation phases of this project on Google Cloud. Covers the gate markers, run scripts, evidence contract, and ticket reporting.
---

# Validation Phases And Evidence Skill for AI Agents

This skill defines how validation runs happen and what counts as
evidence. It maps to the paths and phases on the wiki `Home` page.

---

## 1. Gate Markers

Print one machine-readable marker per gate on the serial console. Grep
the captured serial log for these exact tokens.

| Marker | Meaning | Ticket |
| :--- | :--- | :--- |
| `GATE_BOOT_PASS` | Unikernel banner visible in GCP serial log. Phase A1 done | `4395182d17` |
| `GATE_VIRTIO_PASS` | virtio-net PCI device probed and configured. Phase A2 done | `a5504bc848` |
| `GATE_PING_PASS` | Two instances exchange ICMP. Phase A3 or B3 first half | `84e256b934`, `9374183aab` |
| `GATE_NET_PASS` | networkload bidirectional check passes. Phase A3 or B3 done | `84e256b934`, `9374183aab` |
| `GATE_GVE_ADMINQ_PASS` | gve PCI device probed and Admin Queue describes device. Phase B1 done | `e64aa83ff1` |
| `GATE_GVE_PASS` | gve queues created and uk_netdev interface configured. Phase B2 done | `a1c1337265` |
| `GATE_FAIL` | Any gate failed. Follow with the failing step name | current ticket |

A run is valid only when the log shows the expected marker and the
teardown audit is empty.

## 2. Run Script Rules

1. Keep one script per action in `scripts/`:
   `gcp_build_image.sh`, `gcp_run_pair.sh`, `gcp_teardown.sh`.
2. Start every script with `set -euo pipefail`. Make every script
   rerunnable: create-if-missing, delete-if-exists.
3. Use the run-id and label rules from `skills/gcloud-compute/SKILL.md`.
4. On failure, scripts keep resources alive for debug and print the run
   id plus the cleanup command. The owner then runs
   `gcp_teardown.sh <run-id>`.
5. Never embed project ids, buckets, or images in git history. Read
   them from environment or a local untracked `.env`.

## 3. Evidence Contract

For every run store, in this order:

1. The full serial log of each instance: `reports/<run-id>-serial.log`
   in `fossil uv`.
2. The gate result line in the ticket through
   `fossil ticket change <uuid> +comment`.
3. On code change, the check-in hash inside the same ticket comment.

Do not report a pass without the stored serial log.

## 4. Build Discipline

1. Pin the Unikraft core with a `Kraftfile` at a release tag. Start at
   `RELEASE-0.21.0`. Record the hash, not a floating branch.
2. Build the `kvm` platform, `x86_64`, boot console on COM1.
3. Reuse the lib-mana app pattern for the workload: the
   `samples/networkload` app plus the pinned `lib-lwip`.
4. Keep driver or platform patches as separate commits. Each patch
   links one gate.

## 5. Phase Order

1. A1 (ticket `4395182d17`): stock helloworld boots from a custom
   image. The serial log shows the banner and `GATE_BOOT_PASS`.
2. A2 (ticket `a5504bc848`): record virtio probe lines and confirm the
   transitional device binds.
3. A3 (ticket `84e256b934`): two-instance ping and networkload pass,
   then the teardown audit passes.
4. B1 (ticket `e64aa83ff1`): boot custom UEFI GVNIC image on a third
   generation VM and verify `GATE_GVE_ADMINQ_PASS`.
5. B2 (ticket `a1c1337265`): configure gve queues and `uk_netdev`
   interface, and verify `GATE_GVE_PASS`.
6. B3 (ticket `9374183aab`): two-instance ping and networkload over
   gVNIC pass on third generation VMs, then the teardown audit passes.

One agent drives one gate at a time. The full lifecycle is in
`AGENTS.md`.

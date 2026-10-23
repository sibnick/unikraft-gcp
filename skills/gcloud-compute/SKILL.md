---
name: gcloud-compute
description: Instructions for AI agents on running, observing, and tearing down Google Cloud Compute Engine resources for this project. Covers naming, serial console, firewall, teardown audit, and common errors.
---

# Google Cloud Compute Engine Skill for AI Agents

This skill governs all billable cloud work in this repository. Follow it
for every instance, image, disk, or network change.

---

## 1. Hard Rules

1. Every billable resource uses the `ukg-` name prefix and a `run=<id>`
   label. The run id ties all resources of one test together.
2. Delete everything the run created before the session ends. Tests
   leave nothing behind.
3. Never delete a resource whose name does not start with `ukg-`.
4. Read the serial console first. Debug from the serial log, not from
   SSH guesses.
5. Ask the project owner before the first resource of a new region or
   a machine family with a quota risk.

---

## 2. Session Setup

```bash
export GCP_PROJECT="<project-id>"        # owner supplies once
export GCP_REGION="europe-west4"         # pick one, stay there
export GCP_ZONE="${GCP_REGION}-a"
export UKG_RUN="ukg-$(date +%m%d-%H%M)"  # run id for one test session
gcloud config set project "${GCP_PROJECT}"
gcloud auth list                          # confirm the active account
```

## 3. Instance Lifecycle

```bash
# Create from a project custom image. First and second generation
# series default to the virtio-net NIC, which is the Path A target.
gcloud compute instances create "${UKG_RUN}-srv" \
    --zone "${GCP_ZONE}" \
    --machine-type n1-standard-1 \
    --image "${UKG_IMAGE}" \
    --tags ukg-vm \
    --labels "run=${UKG_RUN}"

# Boot evidence. Poll until the banner appears.
gcloud compute instances get-serial-port-output "${UKG_RUN}-srv" \
    --zone "${GCP_ZONE}"

# Interactive console when the unikernel needs input.
gcloud compute connect-to-serial-port "${UKG_RUN}-srv" --zone "${GCP_ZONE}"

# Stop the run.
gcloud compute instances delete "${UKG_RUN}-srv" --zone "${GCP_ZONE}" --quiet
```

## 4. Firewall For Ping Tests

```bash
gcloud compute firewall-rules create "${UKG_RUN}-allow-icmp" \
    --network default --direction INGOING --allow icmp \
    --source-ranges 10.128.0.0/9 --target-tags ukg-vm
# Internal ranges only. Do not open 0.0.0.0/0 for test runs.
```

## 5. Teardown And Leak Audit

Delete in order: instances, then images, then disks, then firewall
rules, then Cloud Storage objects. Then audit:

```bash
gcloud compute instances       list --filter   'NAME~"^ukg-"'
gcloud compute images          list --filter   'NAME~"^ukg-"'
gcloud compute disks           list --filter   'NAME~"^ukg-"'
gcloud compute addresses       list --filter   'NAME~"^ukg-" AND ADDRESS_TYPE=GLOBAL'
gcloud compute firewall-rules  list --filter   'NAME~"^ukg-"'
gcloud compute images list --project "${GCP_PROJECT}" --no-standard-images
```

Every list must be empty. A non-empty list means the run is not
finished.

## 6. Troubleshooting Table

| Symptom | Likely cause | Action |
| :--- | :--- | :--- |
| Instance boots but no network | Wrong NIC for the series, or missing firewall rule | Read serial log; confirm virtio probe lines; check the ICMP rule |
| `Invalid value ... nicType` | Image lacks the `GVNIC` guest OS feature tag | Only request GVNIC on images tagged for it |
| `zoneResourceExhausted` or `SkuNotAvailable` | Quota or capacity in the zone | Try another zone in the region; then ask the owner |
| Instance unreachable by ping | Firewall, or the unikernel has no IP yet | Serial log first, then the firewall list |
| Serial log empty | Wrong port or the kernel console is not on COM1 | Use `--port=1`; check the image boot config |
| Image import stuck | Import job failure hidden in the bucket logs | Read the `defaultimages-import-logs` bucket |

## 7. Evidence

Save the full serial log of every validation run before teardown:

```bash
gcloud compute instances get-serial-port-output "${UKG_RUN}-srv" \
    --zone "${GCP_ZONE}" > "/tmp/${UKG_RUN}-serial.log" 2>&1
```

Store the log in `fossil uv` as `reports/<run-id>-serial.log`. See
`skills/fossil-unversioned/SKILL.md`.

# Nginx on Unikraft with app-elfloader (`nginx`)

## 1. Overview

This sample runs the official nginx web server inside a Unikraft
unikernel on Google Cloud. It is the port of the `samples/nginx`
sample from the Unikraft-ENA workspace (ticket `4ecf9dcedf`). The
network path uses the virtio-net driver and the lwIP stack, the Path A
target of this project. The target runs on a first or second
generation machine size, where virtio-net is the default NIC.

The sample uses the binary-compatibility path: `app-elfloader` loads
the nginx ELF binary and its shared libraries from a CPIO initrd. You
do not build nginx from source. The initrd comes from the official
nginx Alpine image, so the binary and the libraries match the ones
upstream ships.

Nginx runs in single-process mode: `master_process off`, `daemon off`,
and `worker_processes 1`. The unikernel has no process supervisor, so
nginx must not fork. The lwIP socket layer has no `sendfile` path, so
the config sets `sendfile off` and nginx copies file data through user
space.

## 2. Adaptations versus the ENA original

| Area | ENA sample | This port |
| :--- | :--- | :--- |
| Driver | `lib-ena` | `lib-virtio-net` (in core), Path A |
| Address | DHCP or `NETDEV_IP` env | `netdev.ip=<ip>/<bits>:<gw>` boot argument, like the other GCP runners |
| Pin | `unikraft: stable` + local `.libs` clone | core from the owner fork `github.com/sibnick/unikraft`, branch `feature/gcp-patches`, pinned by commit hash (`prepare-unikraft.sh`); the fork commits the former core patch set on upstream staging. `app-elfloader` `RELEASE-0.21.0` = commit `6ecbcd08d85b`, `libelf` = commit `c03d4ceb22a2`, both fetched by hash |
| Disk builder | `scripts/build_disk.sh` (ext4, `grub-bios-setup`) | `scripts/gcp_build_image.sh` (FAT32 ESP, BIOS + UEFI GRUB), initrd passed as a fourth argument |
| Empty `PT_LOAD` fix | `scripts/elf_drop_empty_segments.py` rewrites the kernel before GRUB | not needed: the fork carries the former core patches `0001` (drop empty TLS program headers) and `0004` (keep real `.tdata` bytes in the data segment) as commits. The image build needs no ELF rewrite |
| LAPIC idle crash | ENA core carried its own LAPIC wake fix | core patch `0005-plat-kvm-x86-map-lapic-page.patch` maps the local APIC page in the kernel page table. GRUB may omit that page from the memory map, and the LAPIC one-shot wake path faults on its first MMIO write when the system idles |
| eventfd `FIONBIO` | ENA core carried its own eventfd fix | core patch `0006-lib-posix-eventfd-accept-fionbio.patch` accepts the non-blocking switch that nginx does on its notify eventfd at startup |
| Idle wake | ENA gate ran on AWS with the LAPIC one-shot wake | `CONFIG_KVM_X86_LAPIC_ONESHOT=n` in the defconfig. Core patch `0007-plat-kvm-x86-lapic-oneshot-option.patch` adds the switch. The first GCP run showed the one-shot probe passing, yet the guest never woke on a halted core. The nginx defconfig now uses the i8254 wake that every GCP gate in this repo has passed with |
| Gate markers | none (external smoke test only) | client prints `GATE_NGINX_SMOKE_PASS` / `GATE_NGINX_SMOKE_FAIL` (see `skills/unikraft-gcp-validation/SKILL.md`) |
| Deploy | `scripts/deploy_aws.py` | `scripts/gcp_run_nginx.sh` |

The initrd content is identical to the proven ENA archive except for
the test page wording and the config comment, which now name Google
Cloud and virtio-net.

## 3. Directory layout

| Path | Purpose |
| :--- | :--- |
| `Kraftfile` | KraftKit spec. The make flow is the build path of this repo; the Kraftfile mirrors its pins |
| `Makefile` | make entry point against the pinned core and libraries |
| `Config.uk` | Thin app marker. `app-elfloader` is the entry point |
| `defconfig` | Kernel configuration for x86_64 KVM, initrd mount, and virtio-net |
| `Dockerfile` | Builds the rootfs from `nginx:1.27-alpine` (musl-linked) |
| `conf/nginx.conf` | Nginx configuration for single-process operation |
| `conf/mime.types` | MIME type table |
| `html/index.html` | Test page served at `/` |
| `initrd.cpio` | Tracked build input: the rootfs as a CPIO archive (about 8 MB) |
| `scripts/build_rootfs.sh` | Regenerates `initrd.cpio` from `Dockerfile` (needs Docker) |
| `scripts/qemu_local.sh` | Host-only QEMU boot check of the GCP-style disk |
| `scripts/smoke_test.sh` | Checks HTTP status and response body against a running instance |

## 4. Prerequisites

Run `scripts/prepare-unikraft.sh` from the repository root once per
checkout. It fetches the core fork commit, the lib-lwip fork commit,
`app-elfloader`, and `libelf`. Both forks carry their former patch sets
as commits. A fresh checkout cannot run
`make defconfig` without it (lesson from ticket `9e7086837e`).

## 5. Build

The tracked `initrd.cpio` is the build input. Regenerate it only when
you change `Dockerfile`, `conf/`, or `html/`:

```bash
./scripts/build_rootfs.sh   # needs Docker and internet, not needed for a normal build
```

Build the unikernel:

```bash
make defconfig
make -j"$(nproc)"
```

Output: `build/nginx_qemu-x86_64`, a Multiboot ELF kernel.

## 6. Disk image and boot arguments

`scripts/gcp_build_image.sh` builds the disk. Its fourth argument adds
the initrd as a GRUB multiboot module:

```bash
UKG_IMAGE_PUSH=0 ../../../scripts/gcp_build_image.sh .        # local disk only, no cloud calls
../../../scripts/gcp_build_image.sh . <img-name> "<cmdline>" ./initrd.cpio   # full cloud flow
```

The unikernel unpacks the initrd into ramfs at `/`
(`CONFIG_LIBVFSCORE_AUTOMOUNT_CI_INITRD`).

The runner passes the boot line
`unikraft netdev.ip=<ip>/<bits>:<gw> --`. Two rules apply:

1. ukboot hides the first command-line token from its library
   parameter scan, and the GRUB multiboot command does not put the
   kernel file name into the command line. The literal `unikraft`
   placeholder therefore keeps `netdev.ip` visible to ukboot. The ENA
   sample used the same placeholder.
2. ukboot reads library parameters only before ` --`. Everything after
   it goes to the application. Without the separator, nginx receives
   `netdev.ip=...` as an option and exits with `nginx: invalid
   option`. Nothing follows the separator, so nginx starts with no
   arguments. The runner always passes a `netdev.ip` argument, so the
   separator is always present.

## 7. Host check with QEMU

```bash
./scripts/qemu_local.sh 180 18080    # boots build/disk.raw, builds it locally if missing
./scripts/smoke_test.sh 127.0.0.1:18080
```

The guest console log is `build/qemu_local.log`. The check covers the
initrd mount, the ELF loader, nginx, and virtio-net. It needs no cloud
calls.

## 8. Cloud run

`scripts/gcp_run_nginx.sh` (ticket `4ecf9dcedf`) builds the image,
boots the target on `n1-standard-1` (virtio-net, `--no-address`) plus
an Ubuntu client VM, and decides the gate from the client serial log:

```bash
UKG_TICKET=4ecf9dcedf scripts/gcp_run_nginx.sh
```

The client waits for TCP 80, fetches `GET /` with curl, and prints the
verdict: HTTP 200 plus the test page marker is `GATE_NGINX_SMOKE_PASS
status=200 marker=ok ...`. It also probes a second connection and a
404 page as forensic fields. See
`skills/unikraft-gcp-validation/SKILL.md` for the marker contract and
the evidence rule.

## 9. Known limits

- One vCPU and one virtio-net queue pair. Multi-core nginx needs a
  different process and queue layout.
- `sendfile` is off. The lwIP socket layer has no sendfile path.
- HTTP only. TLS is present in the binary, but this sample serves
  plain HTTP on port 80.
- The initrd is about 8 MB and lives in guest RAM.
- The kernel carries virtio-net only, so the target needs a first or
  second generation machine size. Third generation sizes need the gve
  driver, which this sample does not include.

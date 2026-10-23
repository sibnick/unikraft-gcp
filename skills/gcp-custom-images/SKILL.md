---
name: gcp-custom-images
description: Instructions for AI agents on packaging a Unikraft unikernel into a bootable Google Cloud custom image. Covers disk layout, GRUB serial boot, import, launch, and the evidence loop.
---

# Unikraft Custom Image For Google Cloud Skill for AI Agents

This skill turns a Unikraft build into a GCP image an instance can
boot. It serves Path A phase A1 (ticket `4395182d17`). Verify each
recipe step once, then record the confirmed steps on the wiki. Treat
unconfirmed steps as spike work, not fact.

---

## 1. Boot Model

1. GCP legacy instances boot BIOS, then GRUB from the VHD/raw disk.
2. GRUB loads the Unikraft kernel. Use the GRUB `multiboot` command.
   The Unikraft x86_64 `kvm` platform image is a multiboot ELF.
3. The kernel talks to COM1. GCP shows COM1 as serial port 1. No guest
   OS services are required.
4. The unikernel does not touch the disk after boot. It needs no block
   driver. The disk only carries GRUB and the kernel.

## 2. Image Recipe

Build the disk on a Linux workstation. Loopback mounts need root.

```bash
# 1. Zero-filled 10 GiB raw disk, one MBR partition, bootable, ext4.
truncate -s 10G ukg-disk.img
parted -s ukg-disk.img mklabel msdos mkpart primary ext4 1MiB 100%
parted -s ukg-disk.img set 1 boot on
# Attach a loop device with partition scan, format the partition, mount it.
LOOP=$(losetup --find --show --partscan ukg-disk.img)
mkfs.ext4 -q -L ukgboot "${LOOP}p1"
mkdir -p /mnt/ukg && mount "${LOOP}p1" /mnt/ukg

# 2. Kernel and GRUB layout.
mkdir -p /mnt/ukg/boot/grub
cp <app>-image-x86_64-kvm-*.elf /mnt/ukg/boot/unikraft.elf
cat > /mnt/ukg/boot/grub/grub.cfg <<'CFG'
set timeout=1
set serial_console="serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1"
$serial_console
terminal_input serial
terminal_output serial
menuentry 'unikraft' {
    multiboot /boot/unikraft.elf
}
CFG
# 3. Install GRUB to the MBR, then unmount and detach.
grub-install --target=i386-pc --boot-directory=/mnt/ukg/boot "${LOOP}"
umount /mnt/ukg && losetup -d "${LOOP}"
```

Notes:

1. GRUB must reach the BIOS serial console, or the boot is invisible.
   Keep the serial lines in `grub.cfg`.
2. Keep `timeout=1`. A hanging GRUB menu shows nothing until timeout.

## 3. Import And Launch

```bash
gsutil cp ukg-disk.img "gs://${GCP_BUCKET}/ukg-${UKG_RUN}-disk.img"
gcloud compute images create "ukg-${UKG_RUN}-img" \
    --source-uri "gs://${GCP_BUCKET}/ukg-${UKG_RUN}-disk.img" \
    --labels "run=${UKG_RUN}"
# Launch and read the serial log per skills/gcloud-compute/SKILL.md,
# then delete instance, image, and the storage object.
```

## 4. Evidence Loop Per Iteration

1. Build, package, import, launch. One run id covers all names.
2. Capture the serial log. Grep for the expected banner.
3. Store the log with `fossil uv add` as `reports/<run-id>-serial.log`.
4. Tear down. Run the leak audit.
5. Append one result line to the ticket:
   `fossil ticket change <uuid> +comment "\n\n<run-id>: <pass|fail + reason>"`.

## 5. Known Risks

| Risk | Mitigation |
| :--- | :--- |
| Multiboot path for GCP BIOS boot unconfirmed | First task of ticket `4395182d17`; keep a fallback GRUB linux-protocol entry ready |
| Disk too small for GCP minimums | Use 10 GiB; adjust once and record the real minimum |
| Import latency dominates the loop | Import is minutes; batch several test binaries per image while iterating |
| GRUB modules missing from image | Install GRUB with `grub-install`, never by copying files by hand |

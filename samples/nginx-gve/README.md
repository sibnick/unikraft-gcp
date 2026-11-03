# Nginx on Unikraft over gVNIC (ticket eae6a73726)

Threaded lwIP nginx, booted through `app-elfloader` from the CPIO
initrd, running on a third generation Google Cloud machine size with
the gVNIC device (`1ae0:0042`).

This sample is the gVNIC sibling of `samples/nginx`. It reuses that
sample's initrd (`samples/nginx/initrd.cpio`) and html test page. Only
the kernel changes: the NIC is gVNIC and RX arrives through MSI-X
interrupts instead of a poll thread.

## What this build proves

1. RX flows through the gve MSI-X interrupt path: the device raises an
   MSI-X event, the driver wakes the uknetdev dispatcher thread, and
   lwIP drains the ring. No RX poll thread exists in this image.
2. The legacy-virtio lesson (ticket `4ecf9dcedf`) does not apply to
   gVNIC: the GCP fabric does deliver gVNIC RX interrupts to the
   guest.

## Gate

Run `scripts/gcp_run_nginx_gve.sh <run-id>` (one cloud run at a time,
owner approval first). The run boots this unikernel on
`c3-standard-4`, then an Ubuntu client fetches `GET /` with curl.

Evidence lines in the server serial log:

| Line | Meaning |
| :--- | :--- |
| `gve: MSI-X ready: ...` | The probe reserved the interrupt path |
| `gve: RXq0 interrupt mode on: vector=...` | lwIP chose interrupt delivery |
| `GATE_GVE_MSIX_PASS vector=... q=0` | First RX event arrived over MSI-X |
| `gve: RX MSI-X event q=0 events=N` | Event counter advances |

Evidence line in the client serial log:

| Line | Meaning |
| :--- | :--- |
| `GATE_NGINX_SMOKE_PASS status=200 marker=ok` | HTTP 200 with the test page |

## Fallback switches

The polled RX path stays reachable in two ways:

1. Boot parameter `gve.intr=0`: the driver keeps every notification
   block masked, reports no RX-interrupt support, and lib-lwIP starts
   its per-netif poll thread.
2. Set `CONFIG_LWIP_UKNETDEV_POLLRX=y` in `defconfig` and rebuild:
   the stack ignores RX-interrupt support unconditionally.

## Build (host only)

```
make defconfig
make -j$(nproc)
```

The QEMU host cannot emulate `1ae0:0042`, so no host smoke test
exists for this sample; the cloud run is the validator, as for every
gVNIC gate in this repository.

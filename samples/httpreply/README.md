# HTTP Reply Benchmark Server (`httpreply`)

Port of the Unikraft-ENA `httpreply` sample to Google Cloud (ticket
[73b23402f0]). The server answers `GET /` with a fixed 14-byte body. Use it
with `wrk` to measure HTTP throughput and latency for one gVNIC queue pair.

## Design

One thread does everything: it polls the netdev (`uknetdev_poll_all()`),
drives lwIP timers (`sys_check_timeouts()`), and multiplexes sockets with
level-triggered `epoll`. lwIP runs in `NO_SYS` mode. No worker threads, no
mailboxes, no context switches between device, stack, and application.

Partial sends are tracked per fd in `resp_pending[]`. A full socket buffer
arms `EPOLLOUT` and the rest of the response goes out on the next event, so
no response byte is dropped.

## GCP adaptations versus the ENA original

| Area | ENA sample | This port |
| :--- | :--- | :--- |
| Driver | `lib-ena` | `drivers/gve` (`CONFIG_LIBGVE`) |
| Address | DHCP | `ip=<ip>/<bits>:<gw>` app parameter, like `samples/networkload` |
| ARP | device resolves | static entry per accepted client, GCP does not answer ARP (ticket [2569ff0157]) |
| Gate markers | none | `GATE_HTTP_READY`, `GATE_HTTP_FIRST_200`, `GATE_FAIL: <reason>` |

## Build

```bash
make -C samples/httpreply defconfig
make -C samples/httpreply
```

Output: `samples/httpreply/build/httpreply_qemu-x86_64` (Multiboot ELF,
GRUB-bootable through `scripts/gcp_build_image.sh`).

The lwIP build prints two `-Woverflow` warnings from `memp.h` at
`CONFIG_LWIP_NUM_TCPCON=1024`. A 16-bit pool count field saturates large
pool counts, so those pools cap lower than requested. There is no
overwrite: the array allocation uses the full width. The same warnings
appear in the proven ENA builds that served 32 million requests with
zero socket errors.

## Local smoke with QEMU

```bash
qemu-system-x86_64 -cpu qemu64,+rdrand \
  -kernel samples/httpreply/build/httpreply_qemu-x86_64 \
  -append "-- ip=10.0.2.15/24:10.0.2.2" \
  -device virtio-net-pci,netdev=n0 \
  -netdev user,id=n0,hostfwd=tcp::18081-:80 -nographic
```

Then, from another shell: `curl -i http://127.0.0.1:18081/`.

Pass check: the log shows `Interface configured`, `GATE_HTTP_READY`, then
`GATE_HTTP_FIRST_200`, and curl prints `Hello, World!` with status 200.

Note the `+rdrand`: the CSPRNG seeds from the CPU (`CONFIG_LIBUKRANDOM_LCPU`).
A CPU model without RDRAND has no entropy source and lwIP crashes at init.
GCP machine types expose RDRAND, so cloud runs need nothing extra. A `qemu64`
model without `+rdrand` is the one place this bites.

## Cloud run

`scripts/gcp_run_c1.sh` (ticket [4c76b4d84d]) deploys this server plus a
`wrk` client VM and captures both serial logs as evidence.

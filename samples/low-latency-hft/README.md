# Low-Latency UDP Echo (`low-latency-hft`)

Port of the Unikraft-ENA `low-latency-hft` sample to Google Cloud (ticket
[238f7cbd91]). One C source, `server.c`, builds two ways: a native Linux
binary and a Unikraft unikernel on the gve driver. The same server code on
both sides makes this the cleanest Unikraft-versus-Linux comparison in the
set: identical algorithm, different runtime.

`client.c` builds only for Linux. It is the RTT probe, run from a client VM.

## What it measures

The client sends `n` synchronous UDP probes (send, wait for the echo, time
the round trip) and reports min, p50, p90, p99, p99.9, max, and mean RTT.
RTT-synchronous pacing means each packet pays full round-trip cost, so the
distribution reflects the datapath, not batching.

## Server optimizations

- Zero-copy echo: receive and send in place in stack buffers.
- 4 MiB socket send and receive buffers to avoid drops.
- Low-delay IP TOS (`IPTOS_LOWDELAY`) on the socket.
- `SO_BUSY_POLL` on Linux, and a Unikraft poll of `uknetdev_poll_all()` in
  the loop on the unikernel.

## Build

Unikernel (for the GCP target):

```bash
make -C samples/low-latency-hft defconfig
make -C samples/low-latency-hft
```

Output: `samples/low-latency-hft/build/hft-server_qemu-x86_64`.

Native server and client (for the client and baseline VMs):

```bash
make -f samples/low-latency-hft/Makefile.native
```

Outputs: `samples/low-latency-hft/bin/server` and `bin/client`.

## Unikraft adaptations versus the ENA original

| Area | ENA sample | This port |
| :--- | :--- | :--- |
| Driver | `lib-ena` | `drivers/gve` |
| Address | DHCP | `ip=<ip>/<bits>:<gw>` app parameter |
| ARP | device resolves | static gateway entry per client source (ticket [2569ff0157]) |
| nolibc gaps | (had libc) | `perror`, `inet_pton`, `inet_ntop`, `EXIT_*`, no `signal.h` filled in under `__Unikraft__` |
| Gate marker | none | `GATE_HFT_READY`, `GATE_FAIL: <reason>` |

The 64 KiB echo buffer is `static`, not on the stack. The unikernel boot
stack is small and a stack buffer that size crashes before `main`.

## Local smoke with QEMU

```bash
qemu-system-x86_64 -cpu qemu64,+rdrand \
  -kernel samples/low-latency-hft/build/hft-server_qemu-x86_64 \
  -append "-- ip=10.0.2.15/24:10.0.2.2 -p 9000" \
  -device virtio-net-pci,netdev=n0 \
  -netdev user,id=n0,hostfwd=udp::19000-:9000 -nographic
```

Then, from another shell:

```bash
samples/low-latency-hft/bin/client -s 127.0.0.1 -p 19000 -n 20000
```

Pass check: the guest prints `Interface configured` and `GATE_HFT_READY`;
the client reports `Received: 20000 packets (0.00% loss)`.

Same RDRAND note as the httpreply sample: the CSPRNG seeds from the CPU, so
local QEMU needs a model with RDRAND. GCP machine types have it.

## Cloud run

`scripts/gcp_run_c2.sh` (ticket [a5874d85fe]) runs the unikernel server, a
native client VM, and a Linux-server baseline, and stores the RTT logs.

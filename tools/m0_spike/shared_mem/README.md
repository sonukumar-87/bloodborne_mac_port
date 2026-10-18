# M0 spike (a)+(b): shared guest memory and IPC, x86-64 (Rosetta) ↔ arm64

Feasibility spike for `docs/DESIGN_NATIVE_GPU_PROCESS.md` §3.1, §3.2, §3.4 and milestone M0 (a), (b).
It is standalone and doesn't touch the runtime.

- `parent` (`parent.c`): `-arch x86_64 -mmacosx-version-min=15.0`, runs under Rosetta 2 (it checks
  `sysctl.proc_translated=1`). Plays `bb-probe`: owns the memory entry, spawns the child, drives every test.
- `child` (`child.c`): native `-arch arm64`. Plays `bbgpu-host`: maps the entry and serves commands
  sent over a shared control block.
- `common.h`: shared layout (control block, two SPSC rings), helpers and the ring code.

```sh
./build.sh          # -> out/parent, out/child (/usr/bin/clang)
./run.sh [N]        # 3 configurations; N = ring messages per repetition (default 10M, 3 reps)
SPIKE_ENTRY=named|share SPIKE_XFER=register|bootstrap ./out/parent [N]   # one configuration
```

Measured on an M4 Max running macOS 26.6. A full `run.sh` takes about 5 minutes.

## (a) Shared memory at identical VAs: works

| Step | Mechanism | Result |
|---|---|---|
| Create the entry | `mach_make_memory_entry_64(size, MAP_MEM_NAMED_CREATE \| RW)`, then `mach_vm_map(entry, VM_FLAGS_FIXED\|VM_FLAGS_OVERWRITE)` at `0x7000000000` (1 GiB) | works |
| Alternative | `mach_vm_allocate` at `0x7000000000`, then `mach_make_memory_entry_64(..., MAP_MEM_VM_SHARE \| RW)` on that range, then re-map the entry over it | works the same (`SPIKE_ENTRY=share`) |
| Hand over ports | Parent: `mach_ports_register(self, &port, 1)` right before `posix_spawn`, unregister right after. The child inherits it and gets it with `mach_ports_lookup`. Child sends a hello with a `MAKE_SEND` reply right to its own receive port. Parent replies with a complex message that carries the memory entry, 2 semaphores and its bootstrap port. | works. Needs no bootstrap, launchd, entitlements or root, and gives a two-way channel (a port in each direction). |
| Rejected: `posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT)` | The child gets the port, but **libxpc caches the bootstrap port during libSystem init**. Restoring it later with `task_set_special_port` + `bootstrap_port =` doesn't help: `bootstrap_look_up` still sends to the parent's port and hangs (confirmed with `sample`). The GPU host needs launchd/XPC (WindowServer, Metal), so this route is **unusable** (`SPIKE_XFER=bootstrap` shows the expected FAIL). | |
| Child maps at the same VA | `mach_vm_map(entry, offset 0, VM_FLAGS_FIXED)` at `0x7000000000`. The arm64 kernel hole is the same, so the address is free. | works |
| Host-style view (VA ≠ entry offset) | child `mach_vm_map(entry, offset X)` at another VA | works and stays coherent. This is how the host mirrors guest views of the backing pool. |
| Data both ways | 4 MiB patterns parent→child and child→parent | coherent |
| Parent aliases | `mach_vm_remap(copy=FALSE)` of the parent's mapping, and a second `mach_vm_map` of the entry | both coherent with the child in both directions |
| Per-view protection | Parent `mprotect(PROT_READ)` on its view. Child writes into the page and the parent reads them. A parent write raises **SIGBUS** (signal 10). The child's data is intact. A parent alias stays writable. | works as the design needs |

Page sizes: the Rosetta parent runs with **4 KiB** pages (`getpagesize()`, `vm_page_size`). The arm64 child
uses **16 KiB**. Sharing one object between the two works, and the parent can `mprotect` at 4 KiB granularity.

### Hole punching (freeing guest pages to zero): constrained

| Attempt | Result |
|---|---|
| P1 parent `madvise(MADV_ZERO)` | **ENOTSUP**. This happens in any translated process, on private memory too (checked separately). |
| P2 parent `mach_vm_behavior_set(VM_BEHAVIOR_ZERO)` | **KERN_PROTECTION_FAILURE**. Also Rosetta-wide; native arm64 gets success on the same calls. |
| P3 **child** (arm64) `madvise(MADV_ZERO)` on its view | **works**: both processes see zeros. About 10 µs per MiB on the child side, about 20–35 µs including the command round trip. |
| P4 **child** `mach_vm_behavior_set(VM_BEHAVIOR_ZERO)` | **works**, same cost |
| P3b child `MADV_ZERO` on a 4 KiB sub-range | **zeroes the whole 16 KiB host page**, so the neighbouring 12 KiB is lost. Sub-16K punches must use `memset`. |
| P5 parent `mmap(MAP_FIXED\|MAP_ANON)` over the range (what `runtime_memory.c punch()` does today) | The parent sees zeros, but **the child keeps the old pages** and the view is no longer shared. Re-mapping the entry over the range restores sharing, and the old data comes back. |
| P6 parent `memset` | always correct, about 25–45 µs per MiB |

**Constraint for M1:** in the split, `punch()` can no longer map anonymous pages over the backing. It
must go through the entry: either (1) ask the native host to run `madvise(MADV_ZERO)` on 16 KiB-aligned
ranges and `memset` the unaligned head and tail, or (2) `memset` in `bb-probe`. Not measured: whether
`MADV_ZERO` also lowers the memory footprint. It is documented as "zero without faulting in", so actually
returning memory may need `MADV_FREE_REUSABLE` from the native side as well.

## (b) Ordering: release/acquire is required and sufficient

SPSC rings of 256 × 64 B slots: `seq`, 6 data words and a checksum, written with plain stores. 3 × 10M
messages per variant. "RELAXED" uses relaxed index loads and stores, with `atomic_signal_fence` so the
compiler can't reorder; any violation therefore comes from the hardware.

| Direction | acq/rel violations | RELAXED violations | Throughput (acq/rel) |
|---|---|---|---|
| x86 (Rosetta, TSO) producer → arm64 consumer | **0** / 30M | **806 – 1916** / 30M (~1 in 20k–40k) | ~105–118 M msg/s |
| arm64 producer → x86 (TSO) consumer | **0** / 30M | 0 – 1 / 30M | ~27 M msg/s |

Both runs of each variant are counted, including the 3M runs. With relaxed indices the arm64 consumer reads
stale payloads regularly: it reorders loads, and TSO on the producer doesn't protect it. A relaxed arm64
producer violation was seen once. This confirms design §3.2: **every native-side read of guest-produced
data, including the renderer's polling of guest labels (WAIT_REG_MEM), needs acquire semantics.** It must
not rely on Rosetta's TSO.

## (b) Doorbell round trip x86 → arm64 → x86

Each sample is one round trip; 100k samples, the first 100 dropped.

| Mechanism | median | p99 | p99.9 |
|---|---|---|---|
| `os_sync_wait_on_address` / `wake_by_address_any` with `..._SHARED` | 9.0–9.7 µs | 11.6–12.0 µs | 14–16 µs |
| Mach semaphores (`semaphore_signal` / `semaphore_wait`, ports passed in the handshake) | **4.9–5.0 µs** | 8.4–9.7 µs | 11 µs |
| Mach message ping-pong (header only, combined send+receive) | **4.9–5.1 µs** | 8.1–9.9 µs | 10–13 µs |
| Pure spin on a shared word (lower bound) | 0.08 µs | 0.12 µs | 0.12 µs |

`os_sync_wait_on_address` works across processes, including between a translated and a native process,
but it is about 2× slower than the Mach primitives here. Mach semaphores and Mach messages are equivalent.
Recommendation: rings plus Mach semaphores (or messages) as the doorbell, with a short spin before
blocking where the peer is likely to answer quickly.

## (b) Fault relay (design §3.4)

The parent `mprotect`s a 4 KiB page read-only and writes to it. The SIGBUS handler stores the fault
address in shared memory, rings the doorbell, waits for the native child to "handle" it (read the page
through its view) and reply, then runs `mprotect(RW)` and returns. Times are end to end per fault, 10k
faults, first 100 dropped.

| Variant | median | p99 |
|---|---|---|
| No relay (signal + mprotect only, today's in-process cost) | 16.5–19.5 µs | 25–35 µs |
| Relay via Mach message | **23.2–23.5 µs** | 30–33 µs |
| Relay via Mach semaphore | 23.1–25.0 µs | 30–41 µs |
| Relay via os_sync | 26.5–30.7 µs | 35–49 µs |
| Relay via spin | 17.0–18.1 µs | 24–47 µs |

One `mprotect` costs 0.41 µs. **Signal delivery under Rosetta dominates, at about 16 µs.** The relay adds
about 6–7 µs with Mach IPC. At the 150–800 faults/s seen in gameplay, that is 4–19 ms of CPU per second
(≤ 2% of a core), of which only about 1–5 ms is new. This is acceptable.

## Blockers

None for the design. Points that change details:

1. **Port hand-off:** use `mach_ports_register` and inheritance, not the bootstrap special port, because
   libxpc caches the bootstrap port.
2. **Punch:** the runtime's "map fresh anonymous pages over the backing" breaks sharing. Zeroing must
   happen on the entry. `MADV_ZERO` and `VM_BEHAVIOR_ZERO` are unavailable under Rosetta, so delegate the
   16 KiB-aligned part to the native host (or `memset`). Native zeroing works at 16 KiB granularity only.
3. **Doorbells:** prefer Mach semaphores or messages (about 5 µs RTT) over `os_sync_wait_on_address`
   (about 9 µs).
4. **Ordering:** relaxed cross-process reads on the arm64 side do fail in practice (~1 in 30k), so the
   acquire/release discipline is mandatory.

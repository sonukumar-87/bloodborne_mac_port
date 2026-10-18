# macOS Port Plan (Apple Silicon)

Status: **work in progress — boots on MoltenVK; character creation and the first level are
playable (2026-10-11).**

## Approach

The game's code is the original PS4 `eboot.bin` (x86-64 machine code); there is no game
source, so it cannot be recompiled for ARM64. The loader, HLE runtime and GPU core are
therefore built as an **x86-64 macOS binary** and the whole process runs under **Rosetta 2**
(AVX2 is supported by Rosetta on macOS 15+).

Graphics use a staged hybrid plan:

1. **MoltenVK** (Vulkan on Metal) with the existing shadPS4-derived Vulkan renderer.
2. **Metal from inside the Vulkan renderer** via `VK_EXT_metal_objects`: MetalFX
   upscaling, zero-copy guest memory (`newBufferWithBytesNoCopy` on unified memory).
3. **Native Metal backend** only where profiling (Instruments / Metal System Trace) shows
   MoltenVK translation overhead is the bottleneck.

## Porting work

| Area | Linux/Windows today | macOS plan |
|---|---|---|
| Guest TLS (`fs:[0]`) | rewritten to `gs:[0]` + `arch_prctl` (Linux), TEB slot (Windows) | **done:** `gs:[0xc00]`, pthread key 384 reserved at startup holds the guest TCB |
| Low (<1 TiB) host memory | `MAP_FIXED_NOREPLACE`, brk heap via `mallopt` | **done:** hint + verify; guest range from 0x7000000000 (above a kernel hole at 63–448 GiB), reserved up front; aliases via `mach_vm_remap` |
| Fault handling | `ucontext gregs[REG_RIP]` | **done:** `uc_mcontext->__ss.__rip`; write faults arrive as SIGBUS |
| Thread dumps | `getdents64` + `tgkill` | **done:** `task_threads` / `pthread_kill` |
| Restart | `close_range` | **done:** close loop |
| Timed locks / clocks | `pthread_mutex_timedlock`, `clock_nanosleep`, `RUSAGE_THREAD` | **done:** trylock backoff, monotonic sleep loop, Mach `thread_info` |
| Upscaling | FSR 3.1 / FSR 4 | FSR 3.1, then MetalFX (FSR 4 not expected to work) |

## Toolchain

- Xcode command line tools (Apple clang, targeting `x86_64-apple-macos`, deployment target 15.0
  because Rosetta runs AVX2 from macOS 15)
- `bash scripts/macos/bootstrap_deps.sh`: builds pinned x86-64 SDL3, Vulkan headers/loader,
  fmt, xxHash, miniz, Zydis, FFmpeg 7.1 and header-only libs into `deps/x86_64` (no sudo, no x86
  Homebrew), and installs the universal MoltenVK dylib with an ICD manifest. Host tools (cmake,
  ninja, nasm, glslang, pkgconf, boost headers) come from the normal arm64 Homebrew.
- `bash build_macos.sh [--test|--gpu-only]`: builds `out/gpu/libbbgpu.dylib`, `out/bb-probe` and
  `out/bb-gpu-capabilities` (x86-64), optionally running the unit tests.
- Run with `VK_DRIVER_FILES=deps/x86_64/share/vulkan/icd.d/MoltenVK_icd.json` (`run.sh` sets it on macOS).

Verified 2026-10-11 on an M4 Max (macOS 26.6): an x86-64 binary under Rosetta enumerates
"Apple M4 Max", Vulkan 1.3, via MoltenVK 1.4.2. Relevant for the plan: `VK_EXT_metal_objects`
and `VK_EXT_external_memory_host` are available; `geometryShader` and `logicOp` are not.

## Unpacking the game (`tools/pkg_extract`)

A fake-signed `.pkg` dump can be unpacked natively with the bundled extractor (shadPS4's
PKG/PFS code from before its removal, with 64-bit file sizes):

```sh
cmake -S tools/pkg_extract -B out/pkg_extract -G Ninja && ninja -C out/pkg_extract   # needs: brew install cryptopp pkgconf
out/pkg_extract/pkg_extract <game.pkg>   <dir>                   # -> <dir>/CUSA03173
out/pkg_extract/pkg_extract <update.pkg> <dir2>/CUSA03173-patch  # then copy its contents over the game folder
python3 -c "import sys; sys.path.insert(0,'scripts'); import game_check; print(game_check.problem('<dir>/CUSA03173') or 'OK')"
```

## Progress

- [x] Repo, plan, README header (2026-10-11)
- [x] `tools/pkg_extract` — game unpacked and verified as 1.09 (2026-10-11)
- [x] x86-64 dependency bootstrap; MoltenVK verified under Rosetta (2026-10-11)
- [x] macOS build script `build_macos.sh` for `gpu/` (bbgpu dylib) and the loader (2026-10-11)
- [x] Runtime port: TLS via pthread TSD slot, low memory, signals, timed locks, thread dumps —
  compiles, unit tests pass under Rosetta (2026-10-11)
- [x] Boot with `--cpu-only`: guest runs to graphics init (stops on unbound Gnm, as expected) (2026-10-11)
- [x] GPU boot on MoltenVK: character creation, save, first level loads and plays (2026-10-11)
- [ ] Rendering gaps (ghosting halo), job-pool crash, performance
- [ ] GPU on MoltenVK; fix rendering gaps
- [x] MetalFX temporal upscaling (upscaler `metalfx`; 1080p presets render the whole frame
  smaller on macOS) (2026-10-11)
- [ ] Zero-copy guest memory
- [ ] Launcher and `.app` bundle


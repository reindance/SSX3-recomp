# Nintendo Switch port (branch `switch`)

Goal: SSX 3 as a homebrew NRO on a CFW Switch (Atmosphère, hbmenu in
full-memory title mode), built from your own PAL rip like every other build.
No game code or data in git; the NRO contains SSX 3's recompiled code and is
for private use only (as the web build).

## What already carries over

- **arm64.** The Asahi build already compiles the lifted C and toolkit for
  aarch64 and boots to the menus.
- **No-trap, no-alias memory model.** The web build (docs/WEB.md) runs with
  explicit MMIO checks (`tools/explicit_stores.py`, `xbox_mmio_read/write`)
  and a page map instead of mirrored mappings. It is the fallback if the
  native layout below fails.
- **Renderer interface.** `src/vk/vkr.h` (~27 calls) is already implemented
  twice: Vulkan (`src/vk/vkr.c`) and WebGPU (`src/web/wgr.c`), both driven by
  `vkr_backend.c`. A Switch backend is a third implementation.
- **SDL2 for window, audio and pads.** The Linux path (`fb_present_sdl.c`,
  SDL audio behind `xa2_*`) exists; devkitPro ships switch-sdl2.
- **SSX Tricky's Switch work** (`ssx-tricky` branch `switch-homebrew`, same
  xboxrecomp toolkit). Proven on hardware: the guest arena with all mirrors
  through kernel-owned shared memory (self-heap aliasing returns 0xD401),
  loader-aware heap budgeting (3.34 GB allowance), libnx file and directory
  handles, SD volume queries, the hardware monotonic clock, SDL pad-slot
  filtering, the native deko3d framebuffer and draw path, and the pinned
  devkitA64 container build. These are toolkit patches 0137-0155 there,
  against xboxrecomp 6f55eaa. Ours is 193e299 (317 commits newer) plus
  `patches/xboxrecomp-local.patch`, so they need rebasing, not copying.

## Plan

1. **Toolchain.** Reuse Tricky's pinned `devkitpro/devkita64` image and
   container build script; a CMake Switch toolchain file; an NRO target next
   to the Linux one (`if(NINTENDO_SWITCH)`). First milestone: the full
   generated game plus toolkit links as an NRO.
2. **Platform layer (toolkit patch).** Rebase Tricky's Switch patches:
   guest arena (1:1 layout plus mirrors through shared memory, keeping the
   native fast path instead of the web page map), heap budget, files and SD,
   clocks, threads, exit and cleanup. Partition images on the SD card
   (exFAT; no sparse files, so check their sizes).
   MMIO: use the web build's explicit-access path unless resuming from a
   libnx exception handler is proven on hardware.
3. **Renderer: deko3d backend** (`src/nx/`, implements `vkr.h`). deko3d is
   what Tricky settled on after Switch Mesa EGL problems. Shaders are
   compiled offline with `uam`, so there is no runtime specialisation:
   start with the combiner interpreter (`xbox.frag` without SPEC), then
   capture SSX 3's distinct combiner setups (`RECOMP_VKR_STAGES=1` over a
   playthrough) and precompile them as fixed variants. Vertex programs stay on
   the GPU (`xbox_vsh.vert`). This avoids the CPU vertex work that holds
   Tricky at ~10 FPS. Targets: 720p handheld, 1080p docked, MSAA off, HD
   textures (BC7, which Maxwell supports) later.
4. **Audio, input, packaging.** SDL audio at 48 kHz stereo; Joy-Con/Pro
   Controller mapping onto the emulated XID pad (Tricky's Npad filtering);
   settings from an ini on the SD card instead of the launcher; game data in
   `sdmc:/switch/ssx3/game/`.
5. **Performance on hardware.** Profile game thread, GPU executor, APU and
   timer threads across the 3 application cores; sys-clk as an option, not
   a requirement.

## Risks

- **CPU speed.** A57 at 1 GHz against a 733 MHz P3, with lifted x86 (register
  globals in TLS, x87 emulation) plus the APU's DSP emulation. Native
  Switch performance for this kind of recomp is still unmeasured (Tricky's
  10 FPS is on an emulator and CPU-vertex-bound).
- **Memory ordering.** Asahi relies on Apple's hardware TSO; the A57 has none.
  The guest CPU lock serialises guest threads and the web build runs without
  TSO, but host device threads (GPU executor, APU) read guest memory
  concurrently.
- **Fixed shader set.** Combiner setups not captured fall back to the
  interpreter (slower, still correct).

## Test setup

- Hardware: the same Switch Tricky's builds run on (full-memory title mode).
- Emulators: Tricky's patched Ryujinx and suyu setups for quick iteration.
  Both are known to diverge from hardware on GPU details, so hardware has the
  final say.

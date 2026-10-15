<h1 align=center>Need for Speed: Carbon — Nintendo Switch port (nfscarbon-nx)</h1>

A native Nintendo Switch port of the Xbox 360 version of **Need for Speed: Carbon** (2006).

It combines two projects:

- **[nfs-carbon-recomp](https://github.com/Sampreeth004/nfs-carbon-recomp)** (Sampreeth004) — the Carbon side: the
  ReXGlue manifest for Carbon's `default.xex`, the game hooks and a native Vulkan renderer that draws the game's GPU
  command ring directly instead of emulating the Xenos GPU.
- **[nfsmw-nx](https://github.com/StevensND/nfsmw-nx)** (StevensND) — the Switch side: the ReXGlue SDK with the
  Horizon layer (memory, threads, crash handling, clocks, audio, input, presentation), the NVK (Mesa) Vulkan driver
  patch and the devkitA64 toolchain.

It is not an emulator: the PowerPC code of `default.xex` is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and compiled for the Switch.

> [!WARNING]
> **Status: work in progress, not yet tested on hardware.** Everything compiles against the Switch SDK on the host
> side (syntax checks with GCC and Clang, see [Verification](#verification)), but the NRO has not been built with
> devkitA64 nor run on a console yet. Expect the first boot to need fixes. See [Roadmap](#roadmap).

> [!NOTE]
> You need your own copy of the Xbox 360 game. Nothing from the game is in this repository, and nothing made from it
> may be added to it.

## Layout

| Folder | Contents | From |
|---|---|---|
| `app/` | The game: manifest, hooks, Switch settings, CMake project | nfs-carbon-recomp + new |
| `app/renderer/` | Carbon's native Vulkan renderer, linked into the NRO | nfs-carbon-recomp |
| `sdk/` | ReXGlue SDK v0.10.0 with the Horizon layer | nfsmw-nx (+ small changes below) |
| `mesa/` | Patch for mesa-switch (NVK) and its build script | nfsmw-nx |
| `tools/` | Extraction, code generation, direct calls, build scripts, Switch toolchain | both |
| `docs/carbon/` | Carbon's renderer notes, frame census and render-mode addresses | nfs-carbon-recomp |

## How the port fits together

1. **The game's code** — `rexglue codegen` translates Carbon's `default.xex` to C++ with
   `app/nfscarbon_manifest.toml` (Carbon's function list from nfs-carbon-recomp). `tools/direct_calls.py` then turns
   calls between unhooked game functions into direct calls so LTO can inline them.
2. **The Switch layer** — the SDK in `sdk/` is the one nfsmw-nx runs on the console. It is not tied to Most Wanted
   (see its [porting guide](https://github.com/StevensND/nfsmw-nx/blob/main/docs/porting-another-game.md)): guest
   memory with on-demand mirror views, eight exception stacks, thread priorities, `audout` audio, libnx input,
   NVK presentation, APM clocks and SaltyNX overlays.
3. **The renderer** — nfs-carbon-recomp's renderer (`app/renderer`) reads the PM4 ring, translates Xenos shaders to
   GLSL and SPIR-V (glslang) at run time, keeps render targets as real Vulkan images and a `VkPipelineCache` on the SD
   card. On PC and Android it is a GPU plugin loaded at run time; an NRO cannot load code, so here it is an OBJECT
   library linked into the executable and created directly in `OnPreSetup` (`config.graphics`). The SDK's Xenos
   emulation is still linked as a fallback (`nfscarbon_renderer = "xenos"`).
4. **Game hooks** — from nfs-carbon-recomp:
   - `ring_wait_hook.cpp`: the game's D3D sleeps instead of spinning while it waits for ring space. The Android
     version slept on a futex; the Switch has none, so the renderer now offers `nfscarbon_cp_wait_rptr` (mutex +
     condition variable, futex on Linux) and the hook calls it directly.
   - `render_mode_hook.cpp`: retail render mode 2 (one tile, no MSAA) instead of predicated tiling
     (`carbon_single_pass`, off until checked in game).
   - `dispatch_tolerance.cpp`: indirect calls to functions the recompiler missed are logged to
     `unregistered_targets.txt` next to the NRO and skipped instead of stopping the game.
5. **Switch settings** — `app/src/nfscarbon_switch.cpp`, adapted from nfsmw-nx: Mesa/NVK environment, nvdrv transfer
   memory (32 MB, MW died at libnx's 8 MB), the official 460.8 MHz handheld GPU profile and the overlays.

## Build

Same tools as nfsmw-nx ([its building.md](https://github.com/StevensND/nfsmw-nx/blob/main/docs/building.md)):
devkitPro with devkitA64 and libnx, CMake 3.25+, Ninja, Python 3.10+, a PC C++ compiler (Clang 20 was used for MW)
and about 16 GB of RAM for LTO.

1. **Third-party sources** (only the files the port changed are in the repository):

   ```sh
   python tools/fetch_thirdparty.py
   ```

2. **Extract the game** to `assets/game_root` (ignored by git):

   ```sh
   python tools/fase1_extraer.py path/to/NFSC.iso -o assets/game_root
   ```

3. **NVK driver** — build [mesa-switch](https://github.com/danfromtico/mesa-switch) at commit `1a8c1a66d6f` with
   `mesa/mesa-switch-nfsmw.patch` ([mesa/README.md](mesa/README.md)). You need its `opt/devkitpro/portlibs/switch`
   folder, which contains `lib/libvulkan.a`.

4. **Code generator** for your PC:

   ```sh
   cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build out/host --target rexglue
   ```

5. **Translate the game**:

   ```sh
   REXGLUE=out/host/rexglue tools/codegen.sh
   ```

6. **Build the NRO**:

   ```sh
   tools/build.sh /path/to/mesa-sdk/opt/devkitpro/portlibs/switch
   ```

   or on Windows `powershell -ExecutionPolicy Bypass -File tools\build.ps1 -MesaSdk C:\path\to\...\portlibs\switch`.
   The result is `app/out/switch/nfscarbon.nro`. Options in `app/CMakeLists.txt`: `NFSCARBON_LTO` (ON),
   `NFSCARBON_RECOMP_O2` (OFF). An optional Homebrew Menu icon goes in `tools/switch/icon/nfscarbon_icon.jpg`
   (256x256 JPEG).

## Install

```text
sdmc:/switch/nfscarbon-nx/
  nfscarbon-nx.nro          (app/out/switch/nfscarbon.nro, renamed)
  nfscarbon.toml            (app/nfscarbon.toml, settings)
  game_root/                (the files of your disc: default.xex, NFS/, ...)
```

A `.iso` next to the NRO also works instead of `game_root/`. Launch with **title takeover** (hold **R** while
starting a game) or a forwarder with a **39-bit address space**: album applet mode does not give enough memory
(guest memory alone is 512 MB). Saves are copied to `saves/` next to the NRO.

The first races compile the shaders and pipelines they have not seen yet and will stutter; they are cached in
`cache/carbon_gpu` on the SD card.

## Settings

`nfscarbon.toml` describes each one. The important ones:

| Setting | Default | What it does |
|---|---|---|
| `nfscarbon_renderer` | `carbon` | `carbon` native renderer, `xenos` emulation (slow, to compare) |
| `carbon_gpu_fps_cap` | `30` | frame rate cap |
| `carbon_gpu_render_scale` | `100` | resolution in % of 1280x720 |
| `carbon_gpu_reflection_faces` | `2` | car reflection cube faces redrawn per frame |
| `carbon_single_pass` | `false` | retail mode 2: one tile, no MSAA |
| `nfscarbon_switch_gpu_mhz` | `460` | handheld GPU profile (official, not an overclock) |
| `nfscarbon_switch_nvmap_mb` | `32` | nvdrv bookkeeping area |
| `nfscarbon_ring_wait` | `true` | D3D sleeps instead of spinning |

## Changes to the SDK

The SDK is nfsmw-nx's, with only these changes:

- `src/system/function_dispatcher.cpp`: `ResolveIndirectFunction` is weak, so `app/src/dispatch_tolerance.cpp` can
  replace it (on the Switch the runtime is linked with `--whole-archive`; on Windows nfs-carbon-recomp used
  `/force:multiple`).
- `include/rex/ui/vulkan/functions/device_1_0.inc`: `vkCreatePipelineCache`, `vkDestroyPipelineCache` and
  `vkGetPipelineCacheData`, which the renderer uses (from nfs-carbon-recomp's `rexglue-android.patch`).
- `thirdparty/CMakeLists.txt`: a submodule folder fetched by `tools/fetch_thirdparty.py` (plain files, no `.git`)
  counts as initialized.

And in `tools/fetch_thirdparty.py`: dangling symlinks (MoltenVK's, macOS only) are skipped instead of stopping the
fetch.

Changes to nfs-carbon-recomp's renderer: Switch-sized defaults for the texture budget (640 MB) and anisotropy (4),
the game frame reported to the Switch profiler and overlays, `nfscarbon_cp_wait_rptr` for the ring wait hook, a
`CreateGpuSystem()` factory, and the CMake for linking it statically. Its other Android patches (SDL Android
surface, AAudio, touch, turnip loading) are not needed on the Switch.

## Edition

Only the **USA (NTSC-U) disc version, without title update**, is supported: `Need for Speed - Carbon (USA) (En,Es)`.

| | |
|---|---|
| Title ID | `454107EC` |
| Media ID | `51D2B53F` |
| XEX version | `0.0.0.1` |
| Region flags | `0x000000FF` (NTSC-U) |
| Entry point | `0x829446E0` |
| `default.xex` SHA-1 | `75ed29cfde2af689fb4f0615d743c07cab31b9da` |

Checked by decrypting both retail XEXs: in the USA one all four hook fingerprints match
(`0x824FFBE0`, `0x8250B200`, `0x82500298`, `0x826E0250`), both ring-wait return addresses (`0x826DBE14`,
`0x826DBBF8`) follow a `bl 0x826E0250`, and the manifest's function hints sit right after `b`/`blr` boundaries.
In the Europe one (`(En,Nl,Sv,Da,Fi)`, Media ID `573FDB37`, version `0.0.0.4`) none of them match: it would need
its own manifest and addresses (`tools/find_fingerprints.py`), as nfsmw-nx does per edition.

## Verification

What was checked without a console or devkitA64 (neither is reachable from the environment this was put together in):

- the renderer and the app sources compile against this SDK's headers with **GCC 13 and Clang 18** (`-fsyntax-only`,
  with the generated headers rendered from the SDK's own codegen templates), including the `__SWITCH__` branches;
- the mutex/condition variable read-pointer wait times out after the cap, wakes up on publish and returns at once
  for a stale sequence (standalone test).

Not checked yet: the devkitA64 build and link of the whole NRO, codegen of Carbon's XEX with this SDK version, and
anything at run time.

## Roadmap

1. **First boot** — build, fix link and runtime errors, get to the menu with the native renderer. Compare with
   `nfscarbon_renderer = "xenos"` when an image looks wrong.
2. **Playable** — verify `carbon_single_pass` in game and turn it on by default; collect `unregistered_targets.txt`
   into the manifest.
3. **Performance**, in the order that paid off for nfsmw-nx (its
   [performance-history.md](https://github.com/StevensND/nfsmw-nx/blob/main/docs/performance-history.md)): the code
   generator options in `app/overrides.toml` (`cr_as_local`, `non_volatile_as_local`, ...), host CRT replacements
   (`[rexcrt]`), PGO and function ordering, the game's other busy waits, shaders translated ahead of time
   (XenosRecomp library) instead of at run time.

## Credits

- **Electronic Arts and EA Black Box** — creators of Need for Speed: Carbon. Unofficial fan project, no affiliation.
- **[Sampreeth004](https://github.com/Sampreeth004/nfs-carbon-recomp)** — nfs-carbon-recomp: Carbon's recompilation,
  hooks and native renderer.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)** — nfsmw-nx: the Switch SDK layer, NVK patch, toolchain and
  the knowledge in its docs.
- **[madelrandel-blip](https://github.com/madelrandel-blip/NFSMW-Recompiled)** — NFSMW Recompiled.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)** — ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[danfromtico](https://github.com/danfromtico/mesa-switch)** and **[NaGaa95](https://github.com/NaGaa95/mesa-switch)**
  — mesa-switch.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)** — devkitA64 and libnx.

## Legal

No affiliation with Electronic Arts. "Need for Speed" is a trademark of Electronic Arts Inc. This repository contains
no assets or program code from the original game; users must provide their own legally obtained copy. Running
homebrew requires custom firmware, which violates Nintendo's ToS.

GPL-3.0 (see [LICENSE](LICENSE)), inherited from nfsmw-nx and NFSMW Recompiled; nfs-carbon-recomp's renderer is
derived from that GPL code too. The SDK keeps its BSD-3-Clause license and the Mesa patch MIT
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). nfs-carbon-recomp has no license file of its own: ask its author
before publishing a build.

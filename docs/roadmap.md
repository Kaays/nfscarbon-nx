# Roadmap: the pieces left

The goal is a playable frame rate at the Switch's **stock clocks** (CPU 1020 MHz, GPU 460.8 MHz, RAM 1331 MHz):
25 FPS in normal races as a first target, 30 later. With an overclock the game already holds 30
([README](../README.md#where-it-stands)), so most of the work below is about doing the same with less CPU and GPU.

Each piece is independent. Pick one, say so in an issue so two people do not do the same thing, and send the
measurements with the change: [measurements.md](measurements.md) shows how every number so far was taken.

Measured at stock clocks in Race Wars (the heaviest scene): 3 cores at 270-290 % of 300 %. Per thread:

| Thread | CPU | What it is |
|---|---|---|
| `GPU Commands` | 70-78 % | our renderer: reads the game's GPU commands and records Vulkan |
| `XThread...` (preparer) | 70-85 % | the game: prepares each frame's command list (simulation, scene) |
| `RWAudioCore Dac` | 50-67 % | the game's audio mixer |
| `Main XThread` (executor) | 35-45 % | the game: runs the command list through its D3D |
| `XMA Decoder` | 15-27 % | the SDK: decodes the game's compressed audio (FFmpeg) |

---

## 1. Renderer: CPU cost per draw

**Size: large. Impact: high (Race Wars).** `app/renderer/src/renderer.cpp`.

About 13 µs of CPU per draw, spread over every phase (`carbon_gpu_draw_profile = true` prints them): state,
pipeline lookup, textures, index and vertex uploads, constants and recording. Race Wars has 3,000-4,500 draws per
frame, so 45-50 ms per frame.

- **Constants (largest single item)**: every draw uploads ~4 KiB per shader stage (Carbon's shaders declare ~250
  constants): 25-60 MiB per frame. The NVK build here already offers a descriptor set updated by differences
  (`NVK_SWITCH_DYN_UBO_DELTA`, from nfsmw-nx's Mesa patch). Upload only what the game changed since the previous draw.
- **Textures** (3-9 ms): the per-draw texture lookup and descriptor set.
- **Index/vertex data** (8-13 ms in Race Wars): vertex arena hashing and index conversion.
- **Recording** (5-11 ms): fewer state changes per draw, as nfsmw-nx's renderer does.

## 2. GPU: the scene is drawn three times

**Size: medium. Impact: high.** 35-44 ms of GPU per frame in races. The biggest item is the 1280x720 scene pass with
depth, **3 passes per frame** (16-19 ms). `carbon_single_pass = true` (retail render mode 2, one tile, no MSAA)
removes the extra passes in the menu (1160 → 455 draws) but not in races. Find out whether the three passes are
predicated-tiling strips (removable) or three different views. nfsmw-nx got most of its frame rate from this same
change in Most Wanted.

## 3. Audio mixer under load

**Size: medium. Impact: the sound in crowded races.** At stock clocks the host audio output finds no mixed block
13-450 times per 10 s in Race Wars (`rex_perfil.log`, "solicitudes sin datos").

What is known: the game's mixer (`sub_826C6AB0`, thread entry `sub_826C6CD0`) double-buffers 256-sample blocks, so it
is at most ~10 ms ahead. Any delay loses a block. Done so far (v0.2.0, details in
[measurements.md](measurements.md)):

- the mixer at priority 0x2A, above the renderer and the SDK's audio threads (Horizon only moves a woken thread to
  an idle core, so at a lower priority it waited behind the renderer on its own core);
- a watch on the audio server: each frame waits briefly for its packet and rescues a stalled one
  (`app/src/audio_server_hook.cpp`);
- XMA decoding in a hybrid mode (first frames of each stream on the calling thread, the rest on a worker);
- native, bit-exact versions of the hottest mixing loops (`app/src/audio_native.cpp`: gain sum, one-pole filter,
  two resamplers, int16 to float).

At 1683 MHz the missing blocks are now only at the start of Race Wars (59-179 per 10 s for 20-40 s), when the CPU is
saturated. Next: more native loops (`sub_826D1678` volume ramp, `sub_826B56A8`, `sub_826CCA20`), and fewer draws in
that scene (item 4).

## 4. The game's own threads

**Size: large. Impact: high.** The preparer and executor threads are a flat profile of game code (simulation,
scene, D3D). What helped Most Wanted, in nfsmw-nx's order
([performance-history.md](https://github.com/StevensND/nfsmw-nx/blob/main/docs/performance-history.md)):

- code generator options (`app/overrides.toml`): `cr_as_local`, `xer_as_local`, `ctr_as_local` are on.
  `non_volatile_as_local` crashes at boot: about 160 functions (mostly exception-handler chunks) take r23-r31 from
  the function that jumps to them and need `share_registers`. The list comes from nfsmw-nx's `tools/lee_antes.py`;
- host replacements for the game's CRT functions (`memcpy`, `memset`, string functions; `[rexcrt]` in the manifest);
- native replacements for hot game functions (nfsmw-nx has native D3D state, matrices, materials...);
- PGO and function ordering.

## 5. Intro videos at stock clocks

**Size: small to medium.** The video frames' texture upload is fixed (27 → 1.4 ms per frame). The videos are still
slow, yet the CPU is almost idle (31 % of 300 %) and the GPU at 2-3 ms. Something waits instead of working: the
frame pacing between the game, the command processor and the vblank (`cp idle` 20-35 ms per frame in the log).

## 6. The second car shadow

**Size: unknown.** A dark patch beside and behind the player's car. Not the vertex arena
(`carbon_gpu_vertex_arena_mb = 0`), not `skip_redundant_resolves`, not the reflection settings. Likely a render
target or resolve the shadow pass reads from the wrong frame or position. `carbon_gpu_trace_frame` and
`carbon_gpu_dump_frame_interval` help.

## 7. Freeze when the player takes control

**Size: small.** A short freeze at the start of a race. Probably pipelines or textures created at that moment:
look for `slow frame` lines with `pipelines built inline` or `texture uploads` in the log.

## 8. More editions

**Size: medium.** Only the USA edition is supported. Europe (`(En,Nl,Sv,Da,Fi)`) needs its own manifest and hook
addresses (`tools/find_fingerprints.py`). nfsmw-nx builds one NRO per edition.

## 9. Shaders ahead of time

**Size: large.** Shaders are translated and compiled at run time (glslang on the Switch CPU), which causes the
first-race stutter. nfsmw-nx translates them ahead of time into a shader pack made from the user's disc.

## 10. Random white flash

**Size: unknown.** A short, mostly transparent white flash at random places in races (seen in Race Wars). It is
not the game's own flash at speedtrap checkpoints and lap crossings: that one is longer and happens only there.
Likely a render target or resolve read before it is written (stale or cleared contents for one frame), or a
post-processing pass (bloom, exposure) getting a wrong value for a frame. `carbon_gpu_dump_frame_interval` around the
moment, or a short video at 60 fps, would help find which pass it is.

# Measurements on the console

Every number here comes from the renderer's own stats (`carbon_gpu_stats`, one line per 5 s in
`logs/nfscarbon_NNN.log`) and the SDK profiler (`logs/rex/rex_perfil.log`, per thread CPU). Stock clocks:
CPU 1020 MHz, GPU 460.8 MHz, RAM 1331/1600 MHz. Target: 30 fps at stock.

## Session 1 (2026-10-16), first build

Edition: USA, Media ID `51D2B53F`. Settings: `nfscarbon.toml` as shipped (fps cap 30, render scale 100,
reflection faces 2, mirror half rate).

### Front end (main menu, ~1160 draws/frame)

| setting | fps | GPU ms p50 | CP draw ms (incl. fence wait) |
|---|---|---|---|
| shipped | 25 | 23.8 | 23 (2.5) |
| `carbon_gpu_render_scale = 50` | 30 | 12.0 | 20.5 (0) |
| `carbon_single_pass = true` | 29 | 20.6 | 16 (7); draws/frame 1160 -> 455 |

The menu is GPU bound: at half resolution it holds 30.

### Race (Canyon, ~2000-2700 draws/frame)

| setting | fps | GPU ms p50 | CP draw ms (fence wait) | CP swap ms |
|---|---|---|---|---|
| shipped | 16-20 | 30-38 | 28-35 (5-20) | 8-13 |
| render scale 50 | 16-24 | 11-13 | 22-29 (0) | 5-13 |
| single pass | 17 | 33 | 29 (2.5) | 12 |

Both sides are over the 33.3 ms budget in a race:

- **GPU**: ~35 ms at 1280x720. The biggest item is the 1280x720 scene pass with depth, **3 passes per frame**
  (16-19 ms). `carbon_single_pass` did not change it in the race (still 3 passes, 1080-1360 draws), only in the
  menu. To find out: whether those 3 passes are predicated-tiling strips (removable) or three different views.
- **CPU, renderer thread ("GPU Commands")**: 22-29 ms recording draws even when the GPU is idle (render scale 50),
  about 11 us per draw. 13-18 MiB are uploaded per frame (vertex data copied again every frame, ~1000-1600 arena
  fetches per frame).
- **CPU, game**: in the race the three cores are 174-253 % busy (of 300 %): GPU Commands 40-54 %, RWAudioCore Dac
  38-49 % (10 % in the menu), Main XThread 33-47 %, XMA Decoder 11-16 %. The renderer also waits 2-15 ms per frame
  on the game (`cp WAIT_REG_MEM`).

### Problems seen

- **Pipeline cache never grew past the first boot** (fixed): every session loaded the same 57 KiB cache and a
  2-pipeline warm list, though each session saved 700-900 KiB. Horizon's rename fails when the target exists, so the
  replace silently failed. Effect: all pipelines recompiled every session on a 1 GHz CPU, and draws skipped while
  they compiled (`skipped per frame: pipeline=11` to `260`): black screen at the countdown, road appearing late,
  missing loading animation.
- **Intro videos slow, audio fine**: the intros are WMV (`MOVIES/*.wmv`), decoded by the game's own recompiled XDK
  decoder. nfsmw-nx had the same problem and replaced that decoder with FFmpeg (its `nfsmw_video_wmv3.cpp`).
- **Crackling audio in races, worse on crashes, also with overclock**: the host audio output shows no underruns
  and no clipping (`rex_perfil.log`), so the noise is produced inside the game's mixer. Not explained yet.

## Session 2 (2026-10-16), pipeline cache fix

Second boot after the fix: `loaded pipeline cache (1509 KiB)`, `pipeline warm-up: 119 pipelines queued`,
no draws skipped for pipelines. Canyon race and road textures fixed; a short freeze remains when the player takes
control at the start.

| scene | fps | draws/frame | upload MiB/frame | GPU ms p50 | CP draw ms | CP swap ms |
|---|---|---|---|---|---|---|
| Canyon race | 15-18 | 1300-1950 | 9-13 | 34-40 | 31-39 | 10-14 |
| Race Wars (crowded) | 6-12 | 3500-7000 | 28-65 | 38-46 | 38-84 | 19-35 |

Race Wars doubles to quadruples the draws (crowd, cars) and the renderer thread scales with them: CPU is the main
limit there. Next measurement: where the draw time goes (`carbon_gpu_draw_profile`).

## Session 3 (2026-10-16), draw CPU profile

Intro videos (loading animation, EA logo) now play through, slowly. Shadows still trail the car for a moment;
`carbon_gpu_skip_redundant_resolves = false` did not change that.

Renderer thread, ms per frame (`carbon_gpu_draw_profile`):

| scene | draws | state | pipeline | textures | indices | vertices | constants | record | swap |
|---|---|---|---|---|---|---|---|---|---|
| menu | 1160 | 1.2 | 1.0 | 2.8 | 6.5 | 2.5 | 2.9 | 4.2 | 4 |
| Canyon | 1500-2100 | 1.1-1.6 | 1.0-1.3 | 3-4 | 1.6-2.3 | 2.7-3.7 | 2.2-3.0 | 4-5 | 10-14 |
| Race Wars | 3400-3700 | 3.1 | 2.6 | 8.5 | 6.6 | 6.0 | 7.3 | 10.6 | 21-26 |

About 13 us per draw spread over every phase (no single hot spot), plus a swap that grows with the draws
(4 ms in the menu, 21-35 ms in Race Wars). Constant uploads were 4 KiB per draw and stage whatever the shader
reads: 10.5 MiB per frame in the menu, 26-58 MiB in Race Wars. Now sized to the shader's constant count; the swap
is split into submit and presenter to see what grows.

## Session 4 (2026-10-16), swap breakdown: the fps cap overslept

Race Wars swap 25-35 ms per frame, of which submit 0.1 ms and presenter 0.8-1 ms. The rest was the fps cap
(`carbon_gpu_fps_cap = 30`): when a frame was more than two periods late it restarted the schedule from now and then
added a full period, so it slept 33 ms. Frames of 70 ms became ~103 ms (9.7 fps instead of 14.3), and at 50 ms
work one frame in four slept (15.7 instead of 20 fps). Simulated old vs fixed: 40 ms work 21.7 -> 25.0 fps,
50 ms 15.7 -> 20.0, 70 ms 9.7 -> 14.3. Fixed: a late frame restarts the schedule without sleeping.

Constants stay 24-59 MiB per frame after sizing them to the shader: Carbon's shaders declare ~250 constants, so
nearly every draw really uploads ~4 KiB per stage. Next: upload only the constants the game changed.

Shadows trailing the car: not `skip_redundant_resolves`, `mirror_half_rate` or `reflection_faces`.

## Session 5 (2026-10-16), fps cap fixed

Race Wars 12.9-13.1 fps (was 9-11), swap 1.3 ms (was 21-35), fps cap sleep 0. Canyon/menu 23-25.
With `carbon_gpu_vertex_arena_mb = 0`: Race Wars 11.4-11.7 fps, shadows unchanged, so the arena is not the cause.
Shadows trail less with `carbon_gpu_mirror_half_rate = false` and `carbon_gpu_reflection_faces = 6`, still there.

Race Wars now: renderer thread draw 45-48 ms, GPU 38-44 ms per frame.

Next build: code generator options on (`cr_as_local`, `non_volatile_as_local`, `xer_as_local`, `ctr_as_local`)
and the function 0x822AF8A0 added. Checked on the PC: codegen runs (347 files), calls made direct 142,521 ->
110,207 (the register save/restore helpers are gone), sample files and the hooks compile.

## Session 6 (2026-10-16), boot crash with the register options

With all four options on, the game crashed at boot right after loading the pipeline cache:
`Unhandled guest access violation: read of guest 0x00000064` (null pointer + 100). The second test meant to have
`non_volatile_as_local` off ran the same binary (same module id in both crash reports): the option is read by the
code generator, so it needs codegen and a full build.

Reads of registers before they are written in the generated code (nfsmw-nx's `tools/lee_antes.py`): with
`non_volatile_as_local`, 160 functions take r23-r31 from the function that jumps to them (almost all exception
handler chunks, `r12 = r31 + N`), and they got 0. With it off: none for cr/ctr/xer except setjmp (sub_8279D060,
now `share_registers`). Next build: `non_volatile_as_local = false`, the other three on.

## Session 7 (2026-10-16), three register options on

Boots and plays with `cr_as_local`, `xer_as_local`, `ctr_as_local` (`non_volatile_as_local` off). Little change:

| scene | fps | draws | renderer draw ms | fence wait ms | GPU ms p50 | frame ms p50/p95 |
|---|---|---|---|---|---|---|
| menu | 25 | 1409 | | | | |
| Race Wars | 11-18 | 1900-3900 | 50 | 0.1 | 44 | 67/99 |
| Canyon | 15-17 | 1300-2100 | 44 (of which 23 fence) | 23 | 35 | 50/67 |

`rex_perfil.log`, Race Wars: CPU 272-295 % of 300: GPU Commands 77 %, XThread2F448080 73-79 %, RWAudioCore 41-64 %,
Main XThread 37-44 %, XMA Decoder 15-27 %. The CPU is saturated. Canyon: 171-183 %, renderer waits 370-410 ms/s on
GPU fences. Frame times land on multiples of 16.7 ms (vblank).

Carbon has Most Wanted's frame handoff between its two render threads, with the same Sleep(0) loops
(flag 0x82C651C4, preparer sub_824E9948, executor sub_824E9720, list 0x82B8C764; the only Sleep(0) calls in the
game). nfsmw-nx measured 41 % of the preparer in those yields. Next build: nfsmw-nx's fix ported
(`app/src/frame_handoff_hook.cpp`): sleep until the flag changes, executor sleeps instead of spinning on an empty
list, both threads at priority 0x3A. `tools/codegen.sh` now starts from an empty folder, so newly hooked functions
are not left called directly.

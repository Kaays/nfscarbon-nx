# Maintaining nfscarbon-nx

A handover for whoever works on this port next (a person or an AI assistant): how it is built, how a new
disc is added, how problems were found, and what is still open. Read it together with the README.

## The build, in one paragraph

ReXGlue turns the game's PowerPC `default.xex` into C++ (`tools/codegen.sh`, output in `app/generated/`), which is
compiled with the SDK (`sdk/`), the native Vulkan renderer (`app/renderer/`) and the port's own code (`app/src/`)
into one NRO (`tools/build.sh`). Each disc ("edition", `REGION`) has its own function list
(`app/nfscarbon_manifest_<region>.toml`) and code generator options (`app/overrides_<region>.toml`).

```
REGION=<region> bash tools/codegen.sh                     # after a pull that touched manifests or hooks,
                                                          # and always when switching edition
REGION=<region> [PGO=generate|use] bash tools/build.sh <mesa-sdk dir> <jobs>
```

- `app/generated` holds one edition at a time; `build.sh` refuses to build another one ("app/generated is for
  'pal2', not 'usa'"). Run codegen for the edition first.
- Editions: `usa`, `pal2` (Europe), `pal` (Europe (Nordic)), `fr`, `it`, `de`, `es`, `collector_usa`,
  `collector_pal`. The disc goes in `assets/game_root_<region>/`. Table with SHA-1s: README, Edition.
- PGO: `PGO=generate` writes profiles to `sdmc:/switch/nfscarbon-nx/pgo_<region>/` while playing; copy the
  `.gcda` files straight into `pgo_<region>/` at the repo root (not into a subfolder) and build with `PGO=use`.
  25-30 varied minutes are enough (menus, a cutscene, 2-3 Race Wars, a normal race, a pursuit, a canyon duel):
  the profile is about ratios, not length. PGO gives about 15-20 % in crowded scenes and nothing at the 30 cap.
- The version shown in the HOME menu is `VERSION` in `app/CMakeLists.txt` (author "Kaays"); it is baked in at
  build time, so every edition must be rebuilt for a new number.
- Test machine: Switch Lite, CPU 1683 / GPU 844 / RAM 2133 MHz, four cores (Sphaira forwarder with 4 cores and a
  39-bit address space).

## Hooks and editions

- A hook defines `sub_8XXXXXXX` (overriding the weak generated one); `__imp__sub_8XXXXXXX` is the original. Guest
  functions it calls are declared with `REX_EXTERN(sub_X)`; memory goes through `REX_LOAD/STORE_*`.
- Hook sources are written with **USA** addresses and listed in `app/hooks_usa.txt`. For another edition,
  `codegen.sh` maps every address (`tools/map_edition.py`) and writes translated copies to `app/src_<region>/`
  (`tools/region_sources.py`). A source with one unmapped address is left out for that edition, so check the
  codegen output: "10 hook sources translated" means all of them.
- `app/CMakeLists.txt` has an explicit source list: a new `.cpp` must be added there (and to `hooks_usa.txt` if it
  names game addresses).

### Adding a new disc

1. Identify it: SHA-1 of `default.xex`, version, Media ID, entry point (see `docs/collectors_editions.md` for how
   they were read). If the SHA-1 matches a known edition, that NRO already works.
2. `tools/compare_images.py <known> <new>` (images in `assets/guest_image_<region>.bin`, written by codegen) tells
   whether the code is the same layout or moved.
3. Moved: `tools/make_edition_manifest.py --region <new> --disc "<name>" --sources <closest first>,...`. It carries
   the hand-declared functions whose code matches uniquely, and functions with hand-set bounds with their switch
   tables when the code matches. If it says "bounded function differs here, add it by hand", find the function in
   the new image (same instructions, shifted) and move its bounds and tables by hand after checking the
   instructions they point at (done for `collector_pal`).
4. `tools/add_thunks.py --region <new>`: declares every this-adjust thunk (two instructions reached from a vtable).
   Their order differs per disc; a missing one made the French disc unable to start a race.
5. Groups of identical small functions are skipped by the unique match. Compare every source manifest entry with
   the new image (same 48 bytes at the address shifted like its nearest matched neighbour; the previous
   instruction a `blr`, `b` or `bctr`) and add them by hand. The German disc called three of them (the group around
   0x82189548) before this was done.
6. Codegen, test build, play: first video, menus, a race. A call to a missed function is skipped and written to
   `unregistered_targets.txt` next to the NRO; its address tells what to add.

## Finding problems

Logs are in `logs/` next to the NRO: `nfscarbon_*.log` (renderer and port), `rex/rex_perfil.log` (10-second
profile: CPU per thread, audio counters, clocks; the last five sessions are kept), `rex/rex_crash.log`.

Renderer debug cvars (toml or command line):

| cvar | use |
|---|---|
| `carbon_gpu_trace_interval = N` | logs every pass, draw (shaders and every texture it reads) and resolve of one frame in N |
| `carbon_gpu_dump_frame_interval = N` | saves that frame as `carbon_frames/frame_<n>.bmp`; with the same N, the picture matches the trace |
| `carbon_gpu_trace_frame = n` | the same for one frame number |
| `carbon_gpu_skip_ps = <hash,...>` | skips draws with these pixel shaders (find what a shader draws) |
| `carbon_gpu_post_census` | lists the full-screen post-processing draws |

Tracing writes thousands of lines per traced frame to the SD card and stalls the game for seconds at each one
(sound runs ahead of cutscenes meanwhile): use it only for the scene being looked at. For a scene deep in the game,
a temporary Options > Video row that sets these cvars (`app/src/options_menu_hook.cpp`, rows are
`{text, "cvar=v;cvar=v"}`) lets the tester turn it on right before it; remove the row before a release.

## How the past problems were solved (and what they teach)

| Problem | Cause | Fix |
|---|---|---|
| Europe crashed at the first video | a function with two switch tables crossing 0x827D0000; the scanner cut it there | bounds and tables by hand in `overrides_pal2.toml` |
| French disc never started a race | a this-adjust thunk not found (`unregistered_targets.txt`) | `tools/add_thunks.py` for every edition |
| Car shadow slid behind the car at low fps | vertex cache checked unchanged pages every 4 frames; the game rewrites the shadow every frame | check every frame; buffers that changed before checked on every draw (no fps cost) |
| Neville hidden in the garage cutscene (tuner/muscle) | depth clamp on in every pipeline: geometry behind the camera inside the car stretched across the screen | clamp only when the game sets `PA_CL_CLIP_CNTL.clip_disable` |
| Sound crackled after HOME/sleep until restart | the SDK audio worker slept 500 ms more after a timed-out wait; when the suspend fell inside that wait the game's audio stopped, the audio server rescue fired and freed packets still playing | no extra sleep; the rescue ignores the time away; audio output restarted on return |
| Speed blur could not be turned off | the blend shader `0xD92CA043533999AF` reads c0.x; with packed constants c0 is `float_constant_map[0]`, not register 0 | `carbon_gpu_motion_blur` zeroes it in that draw's copy |
| Menu freeze on the Challenge Series screen | `RWAudioCore` spinning at 100 % | watchdog lowers it to 0x3B (`sdk/src/ui/switch_perf.cpp`) |

Lessons: compare the per-draw trace with the frame picture before guessing; when something happens "only
sometimes", look for a phase or a race with a timer; read the counters in `rex_perfil.log` (audio "solicitudes sin
datos" = frames requested and not delivered) before changing code. Two wrong guesses were made on the way and are
worth knowing: user clip planes (now supported, but the game does not use them there) and the guest clock jump
(reverted: not the cause, and it also skipped a real 1.6 s stall at boot).

## Still open

- **Race Wars start at 18-22 FPS** (whole pack on screen, ~6000 draws). The game is CPU-bound: lower resolution
  does not help. Ideas not done: the retail single-pass render mode (`carbon_single_pass`, gave nfsmw-nx most of its
  speed; off until checked), fewer guest-side draws.
- **Rare crashes in police chases** (seen twice in Europe (Nordic)): the game finds an object it expects missing.
  `protect_zero = false` lets its null-store traps carry on; no root cause.
- **Rare one-frame white flash** (a stray giant polygon) and a few black frames at some race starts. The same flash
  exists in nfsmw-nx.
- **Lips behind the voice** in in-engine cutscenes when the frame rate drops below 30 (seen once, not confirmed).
- EA Trax in pursuits (a user request): the game picks its own pursuit score; would need a patch to its music logic.

## Rules this project keeps

- No game files, keys or guest images in the repository; game data is only read for identification and analysis.
- Code taken from another project keeps its source and licence named in the file and in the README credits (GPL).
- Release notes in `docs/releases/v<version>.md`, pasted into the GitHub release.

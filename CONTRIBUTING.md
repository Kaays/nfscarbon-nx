# Contributing

Thanks for looking. Any help is welcome: code, measurements on a console, or a report of what does not work.

## Ground rules

- **Never commit anything from the game or generated from it**: no game files, no `default.xex`, no
  `app/generated/`, no NRO. `.gitignore` covers the usual paths. Check `git status` before committing.
- **Measure on a console**: say which clocks (stock or overclock), which scene, and attach the log lines
  before and after. A change that cannot be measured is hard to judge. [docs/measurements.md](docs/measurements.md)
  is the log of everything measured so far.
- **One change per pull request**, with a short explanation of the cause and what it does.
- Say in an issue which piece of the [roadmap](docs/roadmap.md) you are working on.

## Useful tools

| What | How |
|---|---|
| Per-frame renderer stats | `nfscarbon_*.log`: one block every 5 s (fps, draws, uploads, GPU time per pass) |
| Renderer CPU per phase | `carbon_gpu_draw_profile = true` in `nfscarbon.toml` |
| CPU per thread, audio, fences | `logs/rex/rex_perfil.log`, every 10 s |
| Where each thread spends its time | empty file `logs/rex/perfil_pilas.flag`, then `python3 tools/symbolize_profile.py rex_perfil.log app/out/switch/nfscarbon` |
| Indirect calls the analysis missed | `unregistered_targets.txt` next to the NRO |
| Crashes | `logs/rex/rex_crash.log` and `sdmc:/atmosphere/crash_reports/` |

Stack sampling pauses each thread at 1 kHz, so it costs some frame rate. Remove the flag file afterwards.

## Workflow

- Changes to `app/nfscarbon_manifest.toml`, `app/overrides.toml` or a new hook in `app/src` need
  `bash tools/codegen.sh` and a full build. Changes to the renderer or other source files only need
  `bash tools/build.sh`.
- Function names in hooks are guest addresses (`sub_826C6CD0`). They are for the USA `default.xex`
  (see the README's Edition section).

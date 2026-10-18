# Native macOS launcher (0.4) — design

Status: approved in conversation 2026-10-16, awaiting review of this spec. Branch `launcher` (from
master).

## Goal

Someone who downloads 0.4 opens `Bloodborne.app`, sees a native Mac window with sensible defaults,
changes what they want, presses **Play**, and their choices are remembered next time.

What the user asked for:

- output resolution and upscaler quality (the render size follows from them);
- full screen or windowed;
- upscaler choice;
- the optional effects;
- frame rate cap: 30, 40, 60, 90, 120;
- game folder and save folder, each with a default;
- the launcher shows every time (like Steam), with a "Show launcher every time" toggle.

Decisions made in conversation:

- SwiftUI app as the bundle's main executable (not ImGui, not AppleScript dialogs).
- Shown every time by default; when the toggle is off the app goes straight into the game, and
  holding ⌥ (Option) while opening the app shows the launcher again.
- "Save location" moves the saves only. Settings and the shader cache stay in
  `~/Library/Application Support/Bloodborne`.
- Resolution = output resolution + upscaler quality level (Native, Quality, Balanced, Performance,
  Ultra Performance); the launcher shows the resulting render size. No separate input list.
- Built so more games can be added later, but 0.4 handles Bloodborne only (no game list UI).

## Architecture

### Units

`launcher/` (Swift, built with `swiftc` by a script: no Xcode project):

| Unit | Does | Depends on |
|---|---|---|
| `IniFile` | Parses and writes `key=value` files, keeping comments, unknown keys and their order | nothing |
| `GameSettings` | Typed view of `bbport.ini`: output, upscaler, preset, effects, sharpening, show FPS, skip intro, fps cap, full screen, renderer, shader compile. Defaults and validation | `IniFile` |
| `LaunchConfig` | The launcher's own file `launch.conf` in the data folder: game folder, save folder, show launcher every time | `IniFile` |
| `Resolution` | Output list and preset scales → render size (same rounding as `scripts/patches.py`: even sizes) | nothing |
| `GameChecker` | Runs the bundled `game_check.py` on a folder and returns OK or the reason | the bundle's Python |
| `GameRunner` | Starts the game the way `app_launcher.sh` does today (environment, logs, Rosetta check), reports when it exits and with what status | `LaunchConfig`, `GameSettings` |
| `LauncherView` | The window | all of the above |
| `main` | Option key / toggle check: show the window or run the game directly | `GameRunner`, `LauncherView` |

The pure units (`IniFile`, `GameSettings`, `LaunchConfig`, `Resolution`) have no UI or process code,
so they are unit-tested on their own.

### The window

One window, sections top to bottom:

1. **Display**: output resolution (1280×720, 1920×1080, 2560×1440, 3840×2160), Full screen /
   Windowed, frame rate cap (30, 40, 60, 90, 120). Under the cap: a note that above 60 is
   experimental, and that 120 needs a 120 Hz display.
2. **Upscaler**: Off, FSR 3.1, MetalFX, TAA (FSR 4 options appear only when the build reports them, as in the menu); quality level; the line "Renders W × H → outputs
   W × H". The quality level is disabled when the upscaler is Off. Sharpening (on/off and
   strength) shows for FSR.
3. **Effects**: depth of field, motion blur, chromatic aberration, ambient occlusion (SSAO),
   dynamic shadows, screen-space reflections, the game's own anti-aliasing; show FPS; skip intro
   videos.
4. **Locations**: game folder and save folder, each with the path, **Choose…** and **Reset to
   default**; the game check result under the game folder.
5. **Advanced** (collapsed): renderer (Metal / MoltenVK; shown only when the build has the Metal
   renderer, detected from a marker file the packaging writes), shader compilation
   (balanced / wait / skip).

Bottom bar: **Show launcher every time** checkbox, **Play** button (default button, Return key).

Settings are saved when Play is pressed (and when the window closes), not on every click.

### Defaults

- Game folder: the remembered one in `launch.conf`; else empty and Play disabled until one is
  chosen (no reliable default location exists).
- Save folder: `~/Library/Application Support/Bloodborne/user/savedata` (today's location).
- Everything in `bbport.ini`: the engine's current defaults (written by `GameSettings` when the
  file does not exist). Windowed. Frame rate cap 60.
- Show launcher every time: on.

### Data flow

1. The app starts. If ⌥ is held or `show_launcher` is on (or no valid game folder is known): show
   the window. Otherwise: go to step 4.
2. The window loads `launch.conf` and `bbport.ini` and checks the game folder.
3. Play: write `bbport.ini` (only the keys the launcher owns change) and `launch.conf`.
4. `GameRunner` sets the environment and runs the bundled `run.sh`, as `app_launcher.sh` does
   today: `BB_DATA_DIR`, `BB_GAME_DIR`, `BB_PREBUILT`, `BB_PROBE`, `PYTHON`, `VK_DRIVER_FILES`,
   plus `BB_SAVE_DIR` and `BB_FULLSCREEN`. The game's output goes to
   `~/Library/Logs/Bloodborne/game.log` (previous one kept as `game.previous.log`).
5. While the game runs, the launcher window is hidden (the launcher keeps running to watch the
   game); if it is brought back, it shows "Running…" with Play disabled.
6. The game exits: status 0 → the launcher is ready again (or quits if it was not shown);
   otherwise "Bloodborne stopped (status N)" with **Show Log**.

### Engine changes

- **`BB_SAVE_DIR`** (`src/runtime_savedata.c`): when set, saves live in `$BB_SAVE_DIR` instead of
  `<user>/savedata` (the two places that build that path). Unset: unchanged.
- **`fps_cap` in `bbport.ini`** (`run.sh`): mapped to the existing controls. Every cap keeps run.sh's default frame rate patch (uncapped,
  delta time) and sets the presenter's limit `BB_FPS_LIMIT=<cap>` (decided while planning: the
  shipped and tested path). No key: today's behaviour. An explicit `BB_FPS` / `BB_FPS_LIMIT` environment variable
  still wins. The exact mapping is checked against the log during implementation (the patch name
  and the present limit are both logged).
- **`fullscreen` in `bbport.ini`** (`run.sh`): exports `BB_FULLSCREEN=1`; the window code already
  reads it.
- **The in-game menu keeps keys it does not own** (`gpu/shim/bbport_settings.cpp`): today it
  rewrites the whole file from its own values and drops `fps_cap`, `fullscreen` and anything
  else. It will copy over the lines whose keys it does not write.
- **Output resolutions**: the launcher offers the same list as the menu (`OutputWidths`). The
  3456×1944 entry from branch `metal-backend` is not part of this (it was unstable; parked).

### Packaging

- `scripts/macos/build_launcher.sh` builds `out/launcher/Bloodborne` (arm64, macOS 15) with
  `swiftc`.
- `scripts/macos/package_app.sh`: the Swift launcher becomes `Contents/MacOS/Bloodborne` and
  `app_launcher.sh` is retired (the launcher does everything it did). Ad-hoc signing as today. CI
  (`.github/workflows/macos-release.yml`) builds it on the same runner.
- `scripts/macos/make_app.sh` (the local app for this checkout) uses the same launcher, running
  this checkout's `run.sh` (which rebuilds).

## Error handling

- **Game folder**: Play is enabled only when `game_check.py` passes; otherwise its message shows
  under the folder (not a dialog).
- **Save folder**: must be writable (checked when chosen and before Play). When the folder
  changes and the old one has saves while the new one has none, offer to **copy** them. Never
  move or delete. If the folder is missing at Play (e.g. an unplugged drive): say so and offer
  "Use default".
- **Rosetta 2 missing**: offer to install it (as `app_launcher.sh` does).
- **Game already running** (`bb-probe` / `bbgpu-host` processes): Play shows "Running…" and is
  disabled.
- **Game exits with an error**: as in step 6 above.
- **Bad values in `bbport.ini`**: the default for that setting; unknown keys are kept as they are.

## Testing

- **Unit tests** (run by `bash build_macos.sh --test`, a new PASS line):
  - `IniFile`: read, change, write; comments, order and unknown keys survive.
  - `Resolution`: render size for every output × preset, matching `patches.py`.
  - `GameSettings`: defaults when keys are missing or invalid; `fps_cap` values.
- **Engine**: `BB_SAVE_DIR` redirects a save (extend the save data test), and the menu's writer
  keeps unknown keys (test in the GPU suite).
- **By hand, on the test data (`BB_DATA_DIR=out/macdata`, never the live save)**: open the built
  app, set each option, press Play, and confirm in the log that the game received it (output and
  render size, upscaler, full screen, frame rate patch and present limit, effects). Window
  screenshots of the launcher for the user. Change a setting in the in-game menu and confirm the
  launcher shows it next time, and the reverse.
- **Release**: `package_app.sh` builds the app with the launcher; the CI workflow produces it.

## Out of scope (0.4)

- A list of several games.
- Per-game controller mapping, audio settings, mods.
- Signing/notarization (still ad hoc; users clear quarantine).
- Settings for the parked 3456×1944 output.

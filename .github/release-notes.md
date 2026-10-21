Bloodborne for Mac: a launcher app for Apple Silicon Macs that runs your own copy of Bloodborne. **No game files are included.**

## Download

**Bloodborne-macOS-1.0.0.zip** (below, about 50 MB). Unzip it and move **Bloodborne.app** to Applications.

## Requirements

- An Apple Silicon Mac (M1 or newer) with **macOS 15 or newer** and **16 GB of memory or more**
- **macOS 26** for frame generation
- Your own dump of Bloodborne: **CUSA03173, update 1.09** (an unpacked folder with `eboot.bin`)

## Install

1. Unzip the download and move **Bloodborne.app** to Applications.
2. The app is not signed with an Apple developer certificate, so release it from quarantine once. In Terminal: `xattr -dr com.apple.quarantine /Applications/Bloodborne.app` (or open it, then click **Open Anyway** in System Settings → Privacy & Security).
3. Open it. In the launcher, choose your game folder under **Locations**, pick your settings and press **Play**. If Rosetta 2 is missing, the launcher offers to install it.

Saves and settings are kept in `~/Library/Application Support/Bloodborne`.

## Features

### Launcher

- Choose your game folder and save folder, then press **Play**.
- **Display**: output resolution, full screen or windowed, frame rate cap, FPS counter.
- **Upscaler**: FSR 3.1, MetalFX or TAA with quality presets and sharpening.
- **Effects**: turn depth of field, motion blur, chromatic aberration, ambient occlusion, dynamic shadows, screen-space reflections and the game's anti-aliasing on or off.
- Native **Metal renderer**, skip intro videos, shader compilation options.

### Frame generation

- **MetalFX frame interpolation** (macOS 26): a generated frame between every two real frames, so a 60 FPS cap shows 120 FPS on a 120 Hz display.
- **Frame generation: Off / 2×** in the launcher, under *Upscaler*. It works with the Metal renderer and an upscaler (FSR 3.1, MetalFX or TAA).
- The HUD stays steady over the generated frames.

### Keyboard and mouse

- **Mouse look**: click the game window to capture the mouse, ⌘ releases it.
- **Mouse buttons**: left click light attack, hold for strong attack; right click gun; middle click lock on; scroll switches weapons.
- A full PC key layout (W A S D, Shift sprint, Space jump, Esc options and more, listed in the README).
- Settings in `~/Library/Application Support/Bloodborne/mouse.ini`. A connected controller takes over automatically.

### The Old Hunters (DLC)

- Put the extracted add-on folder (the one that contains `sce_sys/param.sfo`) in `~/Library/Application Support/Bloodborne/addcont/` and the game sees The Old Hunters as installed.

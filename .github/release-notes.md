Bloodborne for Mac: the ready-to-run app for Apple Silicon Macs (macOS 15 or newer, 16 GB of
memory or more). Frame generation needs macOS 26.

**No game files are included.** You need your own dump of Bloodborne (CUSA03173, version 1.09).

1. Download `Bloodborne-macOS-<version>.zip` below, unzip it and move **Bloodborne.app** to
   Applications.
2. The app is not signed with an Apple developer certificate, so release it from quarantine
   once, in Terminal: `xattr -dr com.apple.quarantine /Applications/Bloodborne.app`
   (or open it, then click **Open Anyway** in System Settings → Privacy & Security).
3. Open it, choose your game folder under **Locations**, pick your settings and press **Play**.

What this version adds: frame generation (MetalFX), keyboard and mouse controls, The Old
Hunters DLC support. See the README for details.

# bbmouse: keyboard and mouse controls

A small library (`libbbmouse.dylib`, x86-64 + arm64) that `run.sh` loads into both game processes
with `DYLD_INSERT_LIBRARIES`:

- **GPU host (arm64, owns the window):** wraps `SDL_PollEvent`. A click in the game window
  captures the mouse (SDL relative mode); motion, buttons and the wheel go to a small shared file
  (`BB_MOUSE_SHM`). ⌘, focus loss or the settings overlay release it.
- **Game process (x86-64 under Rosetta):** when no controller is connected, it presents a virtual
  SDL gamepad to the port's pad code (`src/runtime_pad.c`), built from the keyboard and that mouse
  state. The camera follows the Windows port's mouse look (impulse with 60 ms decay, starting past
  the game's ~16/127 stick deadzone).

Layout and settings: see the repository README. `mouse.ini` keys: `enabled`, `sensitivity_x`,
`sensitivity_y`, `invert_x`, `invert_y`, `deadzone`, `smoothing`, `hold_ms` (left click held this
long = strong attack), `jump_sprint_ms` (Space while moving: sprint this long, then jump).

    bash tools/bbmouse/build.sh dist/Bloodborne.app     # out/bbmouse/libbbmouse.dylib
    bash tools/bbmouse/test.sh dist/Bloodborne.app      # the virtual gamepad's tests
    bash tools/bbmouse/install.sh /Applications/Bloodborne.app   # add it to an installed app

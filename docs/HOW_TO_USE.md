# How to use the Windows earbud-sync tool

*No programming needed.*

## What this does

It sends whatever plays on your laptop to **both** Bluetooth earbuds at the
same time, and keeps them in sync with each other. Without it, one earbud
usually plays noticeably later than the other.

## What you need

- Windows 10 or 11 (64-bit)
- A pair of Bluetooth earbuds, connected to the laptop
- **`SyncWifi-Setup-1.0.0.exe`** from the GitHub Releases page — installs
  everything and adds shortcuts (no admin needed). Portable alternative:
  the two raw files, `sync_gui.exe` (GUI) + `pipewire_sync_win.exe`
  (engine). Both are self-contained (no DLLs), but you must keep them in the
  **same folder** — the GUI spawns the engine as a `--ui` child.

## Steps

1. Put both earbuds in your ears.

2. Connect them to the laptop: **Settings → Bluetooth & devices**.
   Both earbuds must say "Connected".

3. Start SyncWifi **before** starting the music:
   - **Installed** (recommended): double-click the **SyncWifi** shortcut on
     your Desktop / in the Start Menu (created by `SyncWifi-Setup-1.0.0.exe`).
   - **Portable**: double-click `sync_gui.exe` in the folder holding both
     `.exe` files. (`START_HERE.bat` from a source checkout does the same,
     with a pause at the end so error messages stay readable.)

   Either way the GUI opens — same device list / trim / mute / volume / log
   panel as Linux, driving `pipewire_sync_win.exe --ui` underneath. No console
   window appears anywhere.

4. Wait a couple of seconds for the devices to appear. The status line shows
   something like:

   ```
   dev 0 ... latency=  14.0 ms  offset=  +0.00 ms ...
   ```

   "offset" close to `+0.00` means the two earbuds are in sync. That's it —
   now play music or a video and both earbuds play together.

   (Console fallback: running `pipewire_sync_win.exe` directly puts the same
   status block in a black window.)

5. To stop: press `Q` in the GUI (or just close the window). Your speakers
   and volume are restored automatically.

## Important to know

- The laptop speakers are **muted** while it runs (that is normal — the sound
  comes from the earbuds instead). Stop the program and the speakers come back.
  If the engine log instead says `staying audible`, neither muting nor the
  fallback per-process tap silenced this machine — see *If something goes
  wrong*.
- The build is **unsigned**: SmartScreen may show a blue "Windows protected
  your PC" screen for the installer or the app. Click **More info**, then
  **Run anyway**.
- Start the program **before** the music/video. It resyncs constantly, so even
  if a bud briefly disconnects, it recovers by itself.

## Building it yourself

From a Developer Command Prompt / `desktop/windows/run_win.bat` (MSVC):

```bat
cd desktop\windows
run_win.bat
```

That builds `pipewire_sync_win.exe` and launches it. With CMake + vcpkg
SDL2 on PATH it ALSO builds `sync_gui.exe` (same ImGui panel as Linux)
and launches the GUI instead; pass `--no-gui` to keep the console:

```bat
run_win.bat --no-gui
```

For a GUI-only build step (SDL2 comes from the `desktop/vcpkg.json` manifest —
the toolchain installs it automatically, no manual `vcpkg install`. The
static triplet makes the exe self-contained, exactly like the shipped one):

```bat
cmake -S desktop -B desktop/build -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build desktop/build --config Release --target sync_gui
```

(`<vcpkg>` is your vcpkg checkout, or set `VCPKG_ROOT` and let
`run_win.bat` find the toolchain.)

MSYS2 alternative (engine only, no GUI):

```bash
cd desktop/windows
g++ -O2 -std=c++17 -Wall pipewire_sync_win.cpp -o pipewire_sync_win.exe \
    -lole32 -loleaut32 -luuid -lwinmm
```

Or cross-compile from Linux with no Windows toolchain at all:

```bash
python3 -m ziglang c++ -target x86_64-windows-gnu -O2 -std=c++17 -Wall \
    desktop/windows/pipewire_sync_win.cpp -o pipewire_sync_win.exe \
    -lole32 -loleaut32 -luuid -lwinmm
```

Both are also wired up as VS Code tasks (Terminal → Run Build Task).

### GUI (optional, same panel as Linux)

Needs CMake + vcpkg (SDL2 comes from the `desktop/vcpkg.json` manifest;
OpenGL ships with the Windows SDK) — set that up once:

```bat
git clone https://github.com/microsoft/vcpkg.git %ProgramFiles%\vcpkg
%ProgramFiles%\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT "%ProgramFiles%\vcpkg"
```

then re-run `run_win.bat`: it builds `pipewire_sync_win.exe` with MSVC,
configures `desktop/CMakeLists.txt` against the vcpkg toolchain and
builds `sync_gui` next to it. Without SDL2/vcpkg it skips the GUI and
still builds + launches the console engine (or pass `--no-gui` to skip
the GUI attempt outright). `sync_gui.exe` owns
`pipewire_sync_win.exe --ui` as a child and shows the per-device table
+ engine log; Q quits and restores the speakers. CI does the same
(`x64-windows-static`, so both exes are self-contained) and attaches the two
exes plus **`SyncWifi-Setup-1.0.0.exe`** to draft releases. The installer is
compiled from `desktop/windows/installer.iss` (Inno Setup): stage both exes
next to it and run `ISCC.exe desktop\windows\installer.iss` from the repo root.

`diag_props.cpp` in the same folder is a companion diagnostic: it dumps every
audio-endpoint property Windows exposes. Build it the same way to produce
`diag_props.exe` when debugging why a device isn't classified as Bluetooth.

## If something goes wrong

- **"No Bluetooth sinks found" / it exits immediately**
  The earbuds were not connected. Connect both in *Settings → Bluetooth &
  devices*, then start SyncWifi again (the shortcut, `sync_gui.exe`, or
  `START_HERE.bat` in a source checkout).

- **You hear the sound on the laptop speakers AND earbuds**
  That means the mute self-test found this machine cannot do a silent source:
  muting the captured endpoint also kills the loopback tap there.
  The program then tries volume-0 as a fallback (self-test line ends
  `vol0 ...` with a healthy level); if that works the source sits at volume
  0 while streaming and the room stays quiet.
  If muting *and* volume-0 both fail, it runs a second probe with Windows'
  per-process tap (`process-tap self-test: ...` line) — a differently-placed
  capture point (Windows 10 build 20438+ / Windows 11) that filters by
  process instead of by endpoint. When its level survives the mute, the
  engine switches to that tap, mutes the speakers, and the room stays quiet
  while the buds keep playing — the log says `switching capture to it`.
  Start with `--no-process-tap` to skip that second probe. Only when every
  probe fails does it stay audible on purpose — silencing would also silence
  the buds; the sync still works either way.
  Every probe prints its three levels on start (e.g.
  `0.0899 unmuted / 0.0035 muted / 0.0014 vol0`); a quiet mode is only
  chosen when its level stays above 25 % of the unmuted one — above 0.0225
  in that example, while both probes here came in far below it. Before
  accepting it, try turning off the speakers'
  enhancement DSP: *Sound control panel → Speaker Properties → Enhancements →
  Disable all enhancements* (Windows 11: *Advanced → Audio enhancements →
  Off*), then restart SyncWifi — the test re-runs at every start, and a line
  reading `mute does not affect the loopback tap` (or `switching capture to
  it`) means the room now stays quiet.

- **One earbud still lags the others**
  Stop the program (`q`), then start it again with a small fix. For example,
  if earbud #2 is late by about 15 ms, run:

  ```
  pipewire_sync_win.exe 2:15
  ```

  Open the folder in File Explorer, type `cmd` in the address bar, press
  Enter, then type that command. The setting is remembered for next time.

- **The window flashes and closes too fast to read**
  In the GUI the engine's output stays in the **log panel** — scroll it instead
  of chasing an exit. In console mode, use `START_HERE.bat` instead of
  double-clicking the `.exe` — it waits for a keypress so you can read the
  message.

# How to use the Windows earbud-sync tool

*No programming needed.*

## What this does

It sends whatever plays on your laptop to **both** Bluetooth earbuds at the
same time, and keeps them in sync with each other. Without it, one earbud
usually plays noticeably later than the other.

## What you need

- Windows 10 or 11 (64-bit)
- A pair of Bluetooth earbuds, connected to the laptop
- `pipewire_sync_win.exe` (built from [`desktop/windows/`](../desktop/windows))
- Optional GUI: `sync_gui.exe` — same ImGui panel as Linux (needs SDL2 via
  vcpkg; see "Building it yourself" below). It spawns the engine as a
  `--ui` child, so keep both `.exe` files in the same folder.

## Steps

1. Put both earbuds in your ears.

2. Connect them to the laptop: **Settings → Bluetooth & devices**.
   Both earbuds must say "Connected".

3. Double-click `START_HERE.bat` in `desktop/windows/`
   (double-clicking `pipewire_sync_win.exe` directly works too — whichever
   you click, do it *before* starting the music).
   When `sync_gui.exe` sits next to it (built by `run_win.bat` when SDL2/vcpkg
   is present, or downloaded from a GitHub Release), `START_HERE.bat` opens
   the GUI instead: same device list / trim / mute / volume / log panel as
   Linux, driving `pipewire_sync_win.exe --ui` underneath.

4. A black window opens. Wait a couple of seconds — it prints some lines and
   then a status block like:

   ```
   dev 0 ... latency=  14.0 ms  offset=  +0.00 ms ...
   ```

   "offset" close to `+0.00` means the two earbuds are in sync. That's it —
   now play music or a video and both earbuds play together.

5. To stop: click the window and press `q` (or just close the window).
   Your speakers and volume are restored automatically.

## Important to know

- The laptop speakers are **muted** while it runs (that is normal — the sound
  comes from the earbuds instead). Stop the program and the speakers come back.
- If Windows shows a blue "Windows protected your PC" screen: click
  **More info**, then **Run anyway**. (The program is not signed.)
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

For a GUI-only build step:

```bat
vcpkg install sdl2 --triplet x64-windows
cmake -S desktop -B desktop/build -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake
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

Needs CMake + vcpkg with SDL2 (OpenGL ships with the Windows SDK) — set
that up once:

```bat
git clone https://github.com/microsoft/vcpkg.git %ProgramFiles%\vcpkg
%ProgramFiles%\vcpkg\bootstrap-vcpkg.bat
%ProgramFiles%\vcpkg\vcpkg install sdl2 --triplet x64-windows
setx VCPKG_ROOT "%ProgramFiles%\vcpkg"
```

then re-run `run_win.bat`: it builds `pipewire_sync_win.exe` with MSVC,
configures `desktop/CMakeLists.txt` against the vcpkg toolchain and
builds `sync_gui` next to it. Without SDL2/vcpkg it skips the GUI and
still builds + launches the console engine (or pass `--no-gui` to skip
the GUI attempt outright). `sync_gui.exe` owns
`pipewire_sync_win.exe --ui` as a child and shows the per-device table
+ engine log; Q quits and restores the speakers. CI does the same via
`desktop/vcpkg.json` and attaches both exes to draft releases.

`diag_props.cpp` in the same folder is a companion diagnostic: it dumps every
audio-endpoint property Windows exposes. Build it the same way to produce
`diag_props.exe` when debugging why a device isn't classified as Bluetooth.

## If something goes wrong

- **"No Bluetooth sinks found" / it exits immediately**
  The earbuds were not connected. Connect both in *Settings → Bluetooth &
  devices*, then double-click `START_HERE.bat` again.

- **You hear the sound on the laptop speakers AND earbuds**
  That means the mute self-test found this machine cannot do a silent source:
  muting the captured endpoint also kills the loopback tap there.
  The program then tries volume-0 as a fallback (self-test line ends
  `vol0 ...` with a healthy level); if that works the source sits at volume
  0 while streaming and the room stays quiet. If both fail it stays audible
  on purpose — silencing would also silence the buds. The sync still works.

- **One earbud still lags the others**
  Stop the program (`q`), then start it again with a small fix. For example,
  if earbud #2 is late by about 15 ms, run:

  ```
  pipewire_sync_win.exe 2:15
  ```

  Open the folder in File Explorer, type `cmd` in the address bar, press
  Enter, then type that command. The setting is remembered for next time.

- **The window flashes and closes too fast to read**
  Use `START_HERE.bat` instead of double-clicking the `.exe` — it waits for a
  keypress so you can read the message.

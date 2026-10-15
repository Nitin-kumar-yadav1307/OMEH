# Dear ImGui (vendored)

**Version:** 1.92.2
**Source:** https://github.com/ocornut/imgui (release tag `v1.92.2`)
**License:** MIT — see [LICENSE.txt](LICENSE.txt). Copyright (c) 2014-2025 Omar Cornut.

This directory contains only the files SyncWifi's desktop GUI compiles:

- Core: `imgui.{h,cpp}`, `imgui_draw.cpp`, `imgui_tables.cpp`,
  `imgui_widgets.cpp`, `imgui_demo.cpp`, `imgui_internal.h`, `imconfig.h`,
  `imstb_*.h`
- Backends: `backends/imgui_impl_sdl2.{h,cpp}` and
  `backends/imgui_impl_opengl3.{h,cpp}` (plus its embedded GL loader,
  `imgui_impl_opengl3_loader.h` — the backend does not need system OpenGL
  headers).

## Why vendored

ImGui is a source distribution, not a packaged library — vendoring the exact
files keeps the build reproducible and hermetic (no network at build time).
Update by downloading the next release tarball and replacing these files.

## Notes

- The SDL2 backend includes `<SDL.h>`, so compilation requires the SDL2 include
  path (on Linux: `pkg-config --cflags sdl2`; on MSYS2: the SDL2 mingw package).
- The OpenGL3 backend uses its own GL loader, so no `-lGL`/system GL headers are
  required for it beyond what the host GL context provides at runtime.
- Upstream MIT terms: keep `LICENSE.txt` and this provenance when redistributing.

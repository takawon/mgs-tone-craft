# JUCE 8.0.12 Windows painting overlay

Production workaround: keep FetchContent JUCE **8.0.15**, and replace only the
Windows GUI painting implementation with the matching files from JUCE **8.0.12**.

This is the Track A overlay. It is **not** `direct2d.patch` (reuk). Do not apply
that patch on top of these files.

## Versions

| Role | JUCE | Git |
|---|---|---|
| Overlay source | 8.0.12 | tag `8.0.12` / `29396c22c93392d6738e021b83196283d6e4d850` |
| Host tree | 8.0.15 | `91ad83ae34a81e0833b1a2b0866f54846370ae53` |

## Files (from `modules/juce_gui_basics/native/`)

- `juce_Direct2DHwndContext_windows.cpp`
- `juce_Direct2DHwndContext_windows.h`
- `juce_Windowing_windows.cpp`

The three files are a set. 8.0.12 `Direct2DHwndContext` takes a
`SwapchainDelegate`; 8.0.15 does not. Overlaying only the Direct2D pair onto
8.0.15 `Windowing_windows.cpp` will not compile. Overlaying all three restores
the 8.0.12 HWND painting / Present path.

## Why

Cubase editor mouse jank on Windows is tied to JUCE 8.0.13+ Direct2D
`Present1()` blocking the message thread. The 8.0.12 painting implementation is
the known-good Track A baseline.

## Compatibility adaptation (Displays fields only)

JUCE 8.0.15 `Displays` uses `logicalBounds` / `userBounds` / `physicalBounds`.
8.0.12 `findDisplays` wrote only the deprecated `totalArea` / `userArea`.
`Displays::init` then copies *from* `logicalBounds` into `totalArea`, so the
8.0.12 values are discarded and work-area clamping sees an empty display.
Standalone windows then open off-screen.

After copying the official 8.0.12 blobs, CMake rewrites those `findDisplays`
assignments to the 8.0.15 field names. Painting / Present / swapchain code is
not changed. The vendored blobs stay unmodified.

## How it is applied

CMake copies these files over the FetchContent tree after JUCE is populated
(`tools/apply-juce-8012-windows-paint-overlay.cmake`). A clean checkout plus
`.\tools\build.cmd` is enough; do not edit `build/_deps/juce-src` by hand.

## Provenance

Official blobs from `https://github.com/juce-framework/JUCE` tag `8.0.12`.
License remains JUCE AGPLv3 (same as the FetchContent tree). SHA-256 of the
vendored blobs:

| File | SHA-256 |
|---|---|
| `juce_Direct2DHwndContext_windows.cpp` | `6B2417C4CA04CBC80D4B19C2C64B132EF5617A9D7690629DD97C8676D3919E98` |
| `juce_Direct2DHwndContext_windows.h` | `DF333EAAE59899ABD9CB028F1560217FE8B58947456D61320AD396FD0FBD460F` |
| `juce_Windowing_windows.cpp` | `3B8932C84EEDC7C0FBCB6C954C41056F1B7D4BD6BC64B6152098DD55F371DE76` |

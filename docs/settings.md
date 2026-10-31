# Settings and display decisions

This file records the decisions that govern the settings window, config
persistence, and HDR work.  Update it when a decision changes rather than
re-deriving it from chat history.

## Settings UI

- The settings UI is an **in-window Dear ImGui panel** titled **Settings**.
  Do not introduce a native OS settings window, Qt, SDL, or another UI toolkit.
- It is rendered **outside** `BuildChrome()` and forces
  `ImGuiStyleVar_Alpha = 1.0`, so chrome fade and the Tab chrome toggle cannot
  hide it.
- It has four tabs: **Interface**, **Viewing**, **Filter**, **Advanced**.
- It can be opened before an image exists with `--settings` (the window is
  shown only for that explicit request).  At runtime it is opened by the
  bottom-strip **Settings** button, the **Filter** button (Filter tab), or
  `Ctrl+,`.  The shortcut is checked in the main loop, outside any window.
- Closing the window leaves the app running; use the Settings button or
  `Ctrl+,` to reopen it.
- There are **no HDR controls** in the UI until a working HDR backend exists.
- `pwm_bits` is displayed as a fixed `1` readout; N-phase output is not
  implemented and the UI must not offer > 1.

## Config format and persistence

- Keep the existing flat `key = value` format and search order:
  1. `--config=<path>`
  2. `%APPDATA%/ImageViewer/iview.conf` (XDG equivalent on Linux)
  3. `<exe_dir>/iview.conf`
  4. builtin defaults
- No JSON, TOML, or new dependency.
- Save behavior: explicit **Save** plus save-on-exit only when the window's
  `settings_dirty_` flag is set.  Every Settings edit, Tab chrome toggle, and
  image drop that re-shows the chrome marks it dirty.  Explicit Save clears it
  only on success and shows `Saved to <path>` / `Write failed: <path>`.
  `Config::Save()` remains atomic (sibling `.tmp` + rename) and idempotent.
- All values pass through `Config::Sanitize()` before being persisted.
- When `remember_window_size = true`, the window's **placement** is captured
  once on close: client size, client position and maximised state.  Nothing is
  written on resize ticks.  When false, the stored placement is left untouched.
  Placement lives in `window_width`, `window_height`, `window_x`, `window_y`
  and `window_maximized`.
- Restore order: normal rect first, then maximise, so un-maximising returns to
  the rect the user left.  GLFW exposes no restore rectangle, so the viewer
  samples the geometry while the window is normal; a window closed while
  maximised therefore still restores both its maximised state and the rect
  underneath it.
- A saved position that no longer lands on a monitor work area (monitor
  removed, layout changed) is ignored and the window is centred on the primary
  monitor.  A saved size larger than every work area is clamped.  With no saved
  position at all the window is centred rather than left to the OS default,
  which can place it partly under the taskbar.
- Wayland has no client-side window placement: `glfwGetWindowPos()` reports
  `(0,0)` and `glfwSetWindowPos()` does nothing.  Position is therefore neither
  captured nor applied on Wayland (size and maximised state still are), so a
  Wayland session never writes a bogus `0,0` that a later X11 session would
  honour.
- `ui_scale` is applied from an unscaled base `ImGuiStyle` (`ScaleAllSizes()`
  is cumulative).  ImGui 1.93 applies `FontScaleMain` at draw time, so changing
  it does not require rebuilding the font atlas.

## HDR

- HDR is **Windows-first**.
- Do not expose HDR controls in the UI until a working backend exists.
- `hdr_mode`, `hdr_transfer`, `hdr_sdr_white_nits`, `hdr_peak_nits`, and
  `hdr_tone_map` exist in the config now and round-trip, but have **zero
  renderer effect** in the current stage.
- 10-bit SDR banding reduction is **not** a separate product goal.  Only true
  HDR matters.
- Do not fork GLFW for HDR.  The preferred later path is a manual WGL float
  context on a `GLFW_NO_API` window, with DXGI interop as a fallback if driver
  behavior requires it.

## Current new keys

- `ui_scale = 2.0` (clamped 1.0..3.0)
- `show_readout = true`
- `show_control_strip = true`
- `remember_window_size = true` (covers size, position and maximised state)
- `window_x`, `window_y` (client origin in screen coordinates; omitted until a
  position has been observed, which means "centre the window")
- `window_maximized = false`
- `hdr_mode = off` (`off`, `auto`, `force`)
- `hdr_transfer = scrgb` (`scrgb`, `pq`)
- `hdr_sdr_white_nits = 0` (0 = auto; otherwise 80..1000)
- `hdr_peak_nits = 0` (0 = auto; otherwise 100..10000)
- `hdr_tone_map = true`

`pwm_bits` is capped at 1 until N-phase output exists.  Raise that cap only
together with the renderer support and the startup check in
`src/imageviewer/main_generic.hpp`.

## Testing

E2E/UI testing is planned for a future stage, likely on Linux using a
permissive framework (doctest or Catch2). There is currently no UI test target.

## Historical symptom

`0x142` / “GLFW DLL not found” is a historical symptom, unexplained, and
should not block work.  The real rapid-switch crash was a filter-worker
use-after-free (fixed in Stage 3.5); the plugin load failure is the Win32
`126` debug-dependency issue diagnosed in Stage 3.5 Part B.

## Follow-up

`BuildSettingsWindow()` remains in `Window.ixx` (under the ~400-line
extraction threshold).  Revisit extraction only if it grows beyond that.

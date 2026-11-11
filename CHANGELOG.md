# Changelog

## 1.0.1 (2026-11-10)

### Fixed

- **Crash on minimize:** the app could close without warning when its window
  was minimized. A repaint with a zero-size window made the graph layout
  negative, and the resulting error was never caught. The app now skips
  drawing at that size, and the graph code is safe at any window size.
- **Silent updates kept your sign-in choice:** `WindowsEQ-Setup.exe --silent`
  no longer turns **Start with Windows** back on when updating.

### Added

- **Resilience:**
  - An unexpected error while drawing is logged to
    `%APPDATA%\WindowsEQ\error.log` and the app keeps running.
  - An unexpected error in the audio threads restarts audio routing instead
    of ending the app.
- **Crash reports:** if the app does crash, it writes
  `%APPDATA%\WindowsEQ\crashes\crash.log` and a small minidump.
  - Each release now includes `WindowsEQ.pdb`, the symbol file needed to read
    those dumps.
  - `WindowsEQ.exe --crash-test` checks that crash reporting works.

## 1.0.0 (2026-11-10)

This is the first release.

### App

- **Interactive EQ graph:** up to 32 bands of peak, shelf, pass, band-pass,
  notch and all-pass filters. Bands can apply to both channels, left only or
  right only.
- **Live spectrum analyzer:** shows the audio before and after the EQ.
- **One-click A/B bypass:** button or `B` key.
- **Preamp:** with an **Auto** setting that prevents clipping, plus a safety
  limiter and L/R output meters.
- **Presets:**
  - built-in presets;
  - your own presets;
  - AutoEQ / Equalizer APO import and export;
  - undo and redo.
- **Tray icon:** closing the window keeps the EQ running in the tray.
  **Start with Windows** is optional.

### Audio routing

- **Device clocks:** VB-Cable → EQ → any output device. An adaptive resampler
  keeps the two device clocks in step without audible pitch change.
- **Problem drivers:** it copes with drivers that stall, change request sizes
  or deliver packets late. That covers start-up stalls, a bias that came from
  stall/catch-up cycles, and a 10 ms margin for late VB-Cable packets.

### Installer

- **Per-user install:** `WindowsEQ-Setup.exe` needs no administrator rights
  and has the app embedded.
- **What it adds:** a Start menu shortcut, an Apps & features entry with an
  uninstaller, and a VB-Cable check.
- **Updates:** running a newer setup closes the running copy and updates it in
  place.

### Experimental

- **System-effect APO and `weq-setup` tool:** a passthrough only. It works on
  devices whose drivers load system effects; it is not part of the installer.

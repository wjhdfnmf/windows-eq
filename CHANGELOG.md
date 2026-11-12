# Changelog

## 1.1.0 (2026-11-11)

### Added

- **Dynamic EQ:** any peak or shelf band can be made **dynamic**, so its gain
  moves with the music's level in that band's range.
  - **Mode:** *Above threshold* acts when the band gets loud (tame harshness,
    de-ess). *Below threshold* acts when it's quiet (lift bass at low volume).
  - **Threshold** (dBFS), **Ratio**, **Range** (the most the gain can move;
    negative cuts, positive boosts), **Attack** and **Release**, with a 6 dB
    soft knee.
  - **On the graph:** a shaded area shows how far each dynamic band can move.
    The band's own curve and fill move live with the dynamics, over the
    spectrum. A live marker and a bright overall curve show what the EQ is
    doing right now. The band editor has a live **NOW** meter.
  - **New built-in presets:** *De-esser (dynamic)*, *Tame harshness
    (dynamic)*, *Dynamic loudness*.
  - **Preset files:** dynamic settings are saved as `# WindowsEQ Dynamic …`
    comment lines, so the files still work in Equalizer APO and AutoEQ tools,
    which ignore them.
- **Auto preamp:** now accounts for dynamic bands that can boost.
- **Snapshot and A/B compare (this session):**
  - **Save snapshot** (camera icon) keeps a copy of the current EQ.
  - **Load snapshot** makes it the current EQ; Ctrl+Z undoes this.
  - **A / B** switches instantly between your EQ (A) and the snapshot (B).
    Each side keeps its own edits and undo history.
  - A chip on the graph shows which side you're hearing.
  - Snapshots last until you close the app.
- **Toolbar:** Import and Export are now icon buttons. On narrow windows,
  Reset and the analyzer menu shrink to fit.
- **11 new built-in presets:**
  - *Speech / Podcast clarity*, *Movie dialogue*, *Night mode*, *Gaming:
    footsteps*;
  - *Laptop / small speakers*, *Warm (less fatigue)*, *Bright / airy*;
  - *Boomy bass tamer (dynamic)*, *Hiss reducer (dynamic)*;
  - *Hum removal (50 Hz)* and *(60 Hz)*.

  `WindowsEQ.exe --self-test` checks every built-in: it round-trips through
  the preset format, has sane parameters, can't clip (the preamp covers its
  largest boost, dynamic boosts included), and processes audio cleanly.

### Changed

- **Asks before overwriting your presets:** saving over one of your own
  presets now asks first, with No as the default. **Save** is greyed out
  when there's nothing new to save.

### Quality

- **Level detection:** each dynamic band listens through a band-pass (peaks)
  or a low/high-pass (shelves) at the band. A Hilbert-transform envelope
  detector means the gain doesn't wobble with the waveform: distortion
  measured at −142 dB on steady tones, where a plain RMS detector measured
  −47 dB. Channels are linked, so the stereo image doesn't shift.
- **Responsiveness:** filters update every 32 samples (0.7 ms) and are blended
  per sample, so they can react fast enough to de-ess without clicks.
- **Exact when idle:** a dynamic band that isn't triggered leaves the audio
  bit-exact.
- **CPU:** about 0.07% of one core per dynamic band (stereo, 48 kHz).

## 1.0.2 (2026-11-11)

### Fixed

- **GPU and CPU use while the window isn't visible:** the app kept redrawing
  about 30 times a second when its window was covered by other windows, on
  another virtual desktop, behind the lock screen, or with the display off.
  It now draws nothing in all those cases, as it already did when minimized
  or in the tray. Measured: 8.8% GPU and 7.5% CPU covered before, 0% after.
- **Silent updates restart the app:** `WindowsEQ-Setup.exe --silent` now
  restarts Windows EQ if it was running, in the window or in the tray.
  Before, it left the PC without audio until the app was started again.

- **Covering detection for browser-style apps:** apps built on
  Chromium/Electron (browsers, the Claude app, VS Code…) mark their windows
  as "layered" even though they're fully opaque. Windows EQ treated those as
  see-through and kept drawing underneath them. They now count as covering
  when Windows reports them fully opaque.

### Improved

- **Lower CPU:** about 3.6× less CPU per frame.
  - Text layouts are cached instead of rebuilt every frame.
  - The clipping check behind the Auto button is computed only when the EQ
    changes; it was about 20,000 filter evaluations per frame.
  - EQ curve paths are cached.
- **Adaptive frame rates:**
  - analyzer while audio plays: 24 fps, or 12 fps when the window isn't the
    active one;
  - meters only: 15 / 8 fps;
  - idle: 2 fps.
- **Measured on the dev machine** (music playing, 5-band EQ, standard
  renderer):

  | Window state | FPS | GPU | CPU |
  |---|---|---|---|
  | Visible and active | 22 | 4.0% | ~4% |
  | Visible, not active | 11 | 2.0% | 1.6% |
  | Covered or minimized | 0 | 0% | ~0.2% (audio routing only) |

  `tools/eval-resources.ps1` reproduces these checks.

### Added

- **Low-GPU renderer, experimental, off by default:** Direct3D 11 with a
  flip-model swap chain. It needs well under half the GPU per frame.
  - **Why it's off by default:** a system crash (DPC_WATCHDOG_VIOLATION, a
    kernel driver timeout; the dump was inconclusive) happened while it was
    being stress-tested. It may be unrelated, because that system's graphics
    stack was already behaving abnormally before the crash.
  - **Turning it on:** gear menu → *Low-GPU renderer (experimental)*.
- **Benchmark mode:** `WindowsEQ.exe --bench out.txt` measures the rendering
  cost, including GPU utilisation at real frame rates.
- **Diagnostics:** a `WindowsEQ.Diagnostics` window message reports frames
  drawn and audio/routing/visibility state, for automated tests.

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

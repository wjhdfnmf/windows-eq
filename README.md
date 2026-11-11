# Windows EQ

System-wide parametric EQ for Windows, implemented as an Audio Processing
Object (APO) loaded by the Windows audio engine, plus a control app.

## Status

| Phase | What | State |
|---|---|---|
| 1 | DSP core (`dsp/`) + tests + benchmark | ✅ done |
| 2 | Passthrough APO DLL registered on one device | ✅ verified on NVIDIA HDMI (BenQ); ❌ Focusrite driver never loads APOs |
| 3 | DSP inside the APO, shared-memory parameters | ⏳ |
| 4 | Control app (graph, bands, devices, presets) | ✅ `WindowsEQ.exe` (hosts the router) |
| 5 | Installer, tray, start with Windows, AutoEQ import | ✅ `dist/WindowsEQ-Setup.exe` |

## DSP core (`dsp/`)

- RBJ cookbook biquads: peaking, low/high shelf, low/high pass, band pass,
  notch, all pass. Up to 32 bands, each with a per-channel mask.
- Transposed Direct Form II with 64-bit double math, float I/O, and SSE2
  (channel pairs share one `__m128d`).
- Per-sample coefficient ramps (20 ms) on every change, so there are no clicks
  or zipper noise.
- A flat EQ or bypass is a **bit-exact passthrough**, and 0 dB bands are skipped.
- An optional zero-latency safety peak limiter (instant attack, 50 ms release)
  that only acts above the ceiling.
- Denormal flushing (FTZ/DAZ), NaN/Inf recovery, and parameter clamping.
- No allocation and no locks in `setSettings()` / `process()`, so both are
  real-time safe.

`EqSettings` is a fixed-layout, trivially copyable struct so it can be placed
in shared memory between the app and the APO.

## APO (`apo/`) and setup tool (`setup/`)

- `WindowsEqApo.dll` is a COM APO built on the SDK's `CBaseAudioProcessingObject`.
  It is statically linked, so it imports only system DLLs. Right now it is a
  bit-exact passthrough that publishes live status in shared memory
  (`Global\WindowsEQ.Status.v1`) and writes a log to
  `%ProgramData%\WindowsEQ\apo.log`.
- `weq-setup.exe` does the following:
  - copies the DLL to `%ProgramFiles%\WindowsEQ`
  - registers it
  - sets `DisableProtectedAudioDG=1`
  - writes the device's `FxProperties` values, using the backup/restore
    privilege and leaving registry ACLs untouched
  - restarts the Windows Audio service

  Every original value is backed up under `HKLM\SOFTWARE\WindowsEQ` and
  restored on uninstall.

```bash
weq-setup list                       # devices + current effects (no admin)
weq-setup status                     # install state + live frames/s (no admin)
weq-setup install --device 0         # admin; default slot = MFX (post-mix)
weq-setup uninstall --device 0       # admin; restores the original effects
weq-setup uninstall --all            # admin; removes everything
```

### Lessons from bring-up

- **The audio engine creates APOs aggregated.** `CreateInstance` gets an outer
  `IUnknown`. An APO that refuses aggregation gets loaded, but it silences the
  endpoint without logging anything.
- **A device with no driver effects needs `PKEY_FX_Association`**
  (`KSNODETYPE_ANY`) written next to the effect CLSIDs.
- **Effect settings are cached by AudioEndpointBuilder**, so the setup tool
  restarts that service and the services that depend on it, not just Audiosrv.
- **The endpoint ID comes from `PKEY_AudioEndpoint_GUID`** in
  `pAPOEndpointProperties`. The device collection's software-IO item is the
  KS adapter, not the endpoint.
- **Focusrite's own WDM driver (v4.150) never gets system effects loaded**, in
  any slot. Windows doesn't even load the DLL for that endpoint.

## Router (`router/`): EQ for devices that refuse APOs

Some output drivers never load system effects (see the Focusrite note above).
For those, `weq-router` sits between a virtual audio cable and the real device:

```
apps -> CABLE Input (default output) ==VB-Cable==> CABLE Output -> weq-router -> real device
                                                     capture -> FIFO -> adaptive resampler -> EQ -> render
```

- **Clock bridging:** a 64-tap Kaiser-sinc adaptive resampler (110–150 dB SNR
  in tests) trims the ratio within ±5000 ppm. A critically damped PI
  controller (15 s time constant) drives it from a sawtooth-free fill
  estimate.
- **Self-tuning buffer:** the target is a *mean* fill of one capture packet +
  half the largest render request seen + 10 ms. The margin covers VB-Cable
  delivering a packet a full period late. The buffer grows 2 ms per underrun
  (up to 80 ms), but not when the output itself ran dry because the whole
  system stalled.
- **Output stalls:** the Focusrite driver skips periods (then sometimes
  catches up in a burst) and switches between 10 ms and 20 ms requests.
  While everything queued is digital silence, the router corrects a fill
  error directly in both directions: it drops excess input, or inserts silent
  output when short. Both are inaudible. It decides on the 1 s smoothed fill,
  so a stall followed by a catch-up does nothing. Correcting only excess, as
  it used to, left a deficit after every stall/catch-up cycle, and over about
  30 minutes the drift reached −4000 ppm. The PI integral only sees ±1 packet
  of error.
- **Live presets:** it reads Equalizer APO / AutoEQ preset text
  (`%APPDATA%\WindowsEQ\eq.txt` by default) and reloads it whenever the file
  is saved.
- **Measured with VB-Cable → Focusrite** (48 kHz, 2 ch, shared mode): about
  0.7–1% CPU, 49–52 ms added latency, buffer on target, drift within ±2 ppm,
  and no underruns over 60 s. Before these fixes, the same setup had 1–4
  underruns in the first 30 s.

```bash
weq-router list
weq-router run --to Focusrite                 # reads the VB-Audio "CABLE Output"
weq-router run --from loopback:BenQ --to 0    # test without a virtual cable
```

## Control app (`gui/`): `WindowsEQ.exe`

It's a native Win32 app drawn with Direct2D and DirectWrite, with no
third-party code. It's DPI-aware and uses a dark theme. The app hosts the
router in-process, so opening the window routes audio. A named mutex stops it
and `weq-router` from running at the same time.

- **Graph:**
  - shows the EQ curve, each band's own curve, and a live spectrum before and
    after the EQ (8192-point FFT, +3 dB/oct tilt);
  - double-click to add a band, drag a node to move it, scroll on it to change
    Q, right-click to remove it; hold Shift for fine control.
- **Band editor:** type, frequency, gain, Q and channels. Number fields
  accept dragging, scrolling or typing (`1.5k` works).
- **EQ ON / BYPASSED:** a one-click A/B comparison (shortcut `B`).
- **Preamp and limiter:** a preamp slider with an **Auto** button that
  cancels the largest boost, L/R output meters with peak hold, and the safety
  limiter switch.
- **Presets:** built-ins (Bass Boost, V-Shape, Vocal Clarity, Telephone
  test, and others), user presets in `%APPDATA%\WindowsEQ\presets`, and
  AutoEQ / Equalizer APO import and export.
- **Undo/redo:** Ctrl+Z / Ctrl+Y.
- **Live state:** the current EQ is saved to `%APPDATA%\WindowsEQ\eq.txt`,
  which `weq-router` also reads. Edits made in another editor are picked up
  live.
- **Warnings:** a banner explains when Windows isn't sending audio to
  "CABLE Input", or when VB-Cable is missing.
- **CPU:** the app redraws at 60 fps while you interact, 30 fps while audio
  plays and 10 fps when idle. That's about 2% of one core, including the
  router. F12 shows frame timings.
- **`WindowsEQ.exe --render-png out.png [w h secs mx my]`** renders the UI
  off-screen to a PNG, for checking the layout without a desktop.

## Install, tray and start with Windows

`dist/WindowsEQ-Setup.exe` (about 1.4 MB, with the app embedded) installs Windows
EQ for the current user. It needs no admin rights:

- **Installed files:** `%LOCALAPPDATA%\Programs\WindowsEQ\WindowsEQ.exe` and a
  Start menu shortcut.
- **Apps & features:** an uninstall entry (HKCU).
- **Start with Windows (optional):** an HKCU `Run` value that starts the app
  hidden in the tray (`--tray`).
- **VB-Cable check:** setup reports whether VB-Cable is installed and links to
  it if not.
- **Updates:** running a newer setup closes the running copy, replaces the
  exe and starts it again.
- **Silent install:** `WindowsEQ-Setup.exe --silent [--no-autostart]`.

App behaviour:

- **Closing the window** hides it to the **tray**, and the EQ keeps running.
  Click the tray icon to open the window. Right-click it for EQ on/off,
  presets, start with Windows, Sound settings, and Quit. The icon turns amber
  when the EQ is bypassed.
- **Quit** asks first if Windows is playing to CABLE Input, since audio stops
  while the app isn't running.
- **Launching a second time** brings the running window to the front.
- **Uninstall** from Apps & features or the gear menu
  (`WindowsEQ.exe --uninstall [--quiet]`). It removes the app, its shortcut,
  sign-in launch and registry entries. Presets and settings are deleted only
  if you tick the box.

## Build

Requires Visual Studio 2022/2026 with the C++ workload, and CMake 3.20+.

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build/tests/Release/eqdsp_tests.exe
build/tests/Release/eqdsp_bench.exe
```

Benchmark on the dev machine: 10 bands of stereo at 48 kHz use about 0.16% of
one CPU core, and 32 bands about 0.43%.

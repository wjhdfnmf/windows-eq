# How Windows EQ works

```
apps ──► "CABLE Input" (Windows default output)
              │  VB-Cable (virtual audio driver)
              ▼
         "CABLE Output" ──► WindowsEQ.exe ──► your real output device
                             capture → FIFO → adaptive resampler → EQ → render
```

Windows has no supported way for an ordinary app to process *all* system
audio on any device:

- **System-effect APOs** need the device's driver to load them, and many
  drivers never do. See the [APO notes](#experimental-system-effect-apo-apo-setup)
  below.
- **Kernel filter drivers** need driver signing.

So Windows EQ uses a **virtual cable**. Windows plays into VB-Cable, and
Windows EQ captures that audio, equalizes it and plays it on the real device.

## Source layout

| Folder | What |
|---|---|
| `dsp/` | `eqdsp` library: filter design, the equalizer, adaptive resampler, drift controller, preset parser |
| `router/` | `weqrouter` library (capture → EQ → render over WASAPI) and the `weq-router` command-line tool |
| `gui/` | `WindowsEQ.exe`: the app (Win32 + Direct2D/DirectWrite) |
| `install/` | `weqinstall` library: per-user install paths, shortcut, registry, sign-in launch |
| `installer/` | `WindowsEQ-Setup.exe` with `WindowsEQ.exe` embedded as a resource |
| `apo/`, `setup/` | Experimental system-effect APO and its admin setup tool |
| `common/` | Shared headers (IDs, version) |
| `tests/` | Unit tests, benchmark, APO smoke test |
| `tools/` | `check-install.ps1` |

## DSP core (`dsp/`)

- **Filters:** RBJ cookbook biquads (peaking, low/high shelf, low/high pass,
  band pass, notch, all pass). Up to 32 bands, each with a per-channel mask.
  The maths matches what AutoEQ and Equalizer APO presets assume.
- **Arithmetic:** Transposed Direct Form II with 64-bit doubles, float I/O and
  SSE2 (channel pairs share one `__m128d`).
- **No clicks:** per-sample coefficient ramps (20 ms) on every change avoid
  clicks and zipper noise.
- **Exact bypass:** a flat EQ or bypass is a **bit-exact passthrough**, and
  0 dB bands are skipped.
- **Safety limiter:** optional, zero latency (instant attack, 50 ms release),
  acting only above the ceiling.
- **Robustness:** denormal flushing (FTZ/DAZ), NaN/Inf recovery, and parameter
  clamping. There is no allocation or locking in `setSettings()` /
  `process()`.
- **Cost:** 10 bands of stereo at 48 kHz use about 0.16% of one CPU core;
  32 bands about 0.43%.
- **Settings layout:** `EqSettings` is a fixed-layout, trivially copyable
  struct, so it can be placed in shared memory.

## Router (`router/`)

- **Streams:** WASAPI shared mode, event-driven capture and render threads at
  MMCSS "Pro Audio" priority, with a lock-free SPSC ring between them. EQ
  settings arrive through a seqlock mailbox, so the audio threads never block.
- **Clock bridging:** the cable and the output device run on different clocks.
  - A 64-tap Kaiser-windowed sinc **adaptive resampler** (110–150 dB SNR in
    tests) trims the ratio within ±5000 ppm.
  - A critically damped **PI controller** (15 s time constant) drives it from
    a sawtooth-free fill estimate: FIFO plus resampler plus the frames accrued
    since the last packet.
  - The PI integral only sees ±1 capture packet of error, so a run of output
    stalls can't wind it up.
- **Buffer target:** the target is a *mean* fill: one capture packet + half
  the largest render request seen + 10 ms.
  - The 10 ms margin covers VB-Cable delivering a packet a whole period late.
  - The target grows 2 ms per underrun (up to 80 ms), but not when the output
    itself ran dry because the whole system stalled.
- **Output stalls:** some drivers (Focusrite USB, for example) skip periods,
  catch up in bursts, and switch between 10 ms and 20 ms requests.
  - While everything queued is digital silence, the router corrects the fill
    directly in both directions: it drops excess input, or inserts silent
    output. Both are inaudible.
  - It decides on the 1 s smoothed fill, so a stall followed by a catch-up
    does nothing.
  - An earlier version corrected only excess, which left a deficit after every
    stall/catch-up cycle. After about 30 minutes that version drifted to
    −4000 ppm.
- **Analyzer tap:** mono {before, after} pairs go into a second ring for the
  app's spectrum analyzer. Frames are dropped if the app falls behind; the
  audio thread never waits.
- **Single instance:** a named mutex (`Local\WindowsEQ.Router`) stops two
  routers from playing everything twice.
- **Measured on VB-Cable → Focusrite** (48 kHz stereo, shared mode):
  - about 0.7–1% CPU;
  - 49–52 ms added latency;
  - drift within ±2 ppm;
  - no dropouts over a 60 s run, and none over 6.5 minutes of real use.

`weq-router` is the same engine as a command-line tool:

```bash
weq-router list
weq-router run --to Focusrite                  # reads VB-Cable's "CABLE Output"
weq-router run --from loopback:BenQ --to 0     # test without a virtual cable
```

`WEQ_ROUTER_TRACE=file.csv` dumps per-period buffer and controller state for
the first ~40 s.

## App (`gui/`)

- **UI toolkit:** a small immediate-mode toolkit on Direct2D/DirectWrite
  (`Ui.h`). It has per-monitor DPI awareness, a dark theme, Segoe UI and
  Segoe MDL2 icons.
- **Spectrum analyzer:**
  - 8192-point FFT with a Hann window, at most 35 transforms per second;
  - smoothing in the power domain;
  - +3 dB/octave display tilt around 1 kHz.
- **Rendering backends:**
  - **Default:** `ID2D1HwndRenderTarget`, the Direct2D window target.
  - **Opt-in "low-GPU" renderer** (`renderer=flip` in `config.ini`, or the
    gear menu; `WEQ_RENDERER=flip|legacy` overrides it): Direct3D 11 device
    → DXGI flip-model swap chain (`FLIP_DISCARD`, 2 buffers) → Direct2D 1.1
    device context. It falls back to WARP when there's no GPU, and recovers
    from device loss. The compositor uses its back buffer directly, so it
    needs well under half the GPU per frame.
  - **Why it's opt-in:** a DPC_WATCHDOG_VIOLATION bugcheck happened while it
    was being stress-tested (rapid cover/minimize cycles). The kernel dump
    was unreadable (`CORRUPT_MODULELIST`). The same machine's graphics stack
    had been abnormally slow before the crash, which a clean reboot fixed, so
    the renderer may be unrelated. It stays opt-in until that's known.
- **CPU per frame:**
  - text layouts (`IDWriteTextLayout`) are cached by string, font, box and
    alignment, and evicted after about 48 unused frames;
  - EQ curve path geometries are cached until the settings or graph geometry
    change;
  - the max-boost scan behind the Auto button is cached per settings.

  That's about 3.6× less CPU per frame than building everything every frame.
- **Frame rate:**
  - **60 fps** while you interact;
  - **24 fps** for the analyzer while audio plays (12 fps when the window
    isn't the active one);
  - **15 / 8 fps** for meters only;
  - **2 fps** when idle.
- **Not visible means not drawn:** no frames at all while the window is
  hidden in the tray, minimized, cloaked (another virtual desktop), fully
  covered, behind the lock screen (`WTSRegisterSessionNotification`) or with
  the display off (`GUID_CONSOLE_DISPLAY_STATE`).
  - Coverage is checked every 0.25 s by subtracting the frame bounds of the
    visible, opaque top-level windows above it in z-order from the client
    rectangle.
  - **See-through windows don't count as covering:** click-through windows,
    and layered windows with per-pixel alpha, a colour key or constant alpha
    below 255.
  - **Chromium/Electron windows do count:** they're layered with constant
    alpha 255, so they're opaque.
  - Routing keeps being ticked 4 times a second.
- **`--bench out.txt [frames]`:** renders a test window with demo audio. It
  reports the CPU per frame with vsync off (plus settings-change and
  curve-rebuild counts), and this process's GPU 3D-engine utilisation (PDH
  counter) at the app's real frame rates with vsync on.
- **Diagnostics:** the registered window message `WindowsEQ.Diagnostics`
  answers `wParam 0` with the number of frames drawn so far. `wParam 1`
  returns flags: 1 = audio active, 2 = routing running, 4 = seen,
  8 = foreground.
- **Resource evaluation:** `tools/eval-resources.ps1` uses that message and the
  GPU counters to check five window states against targets: visible, covered
  by a normal window, covered by an opaque layered window, covered by a
  semi-transparent window, and minimized.
  - Its cover windows run in a separate process (`tools/cover-window.ps1`)
    with a live message loop. A window that stops responding gets replaced by
    a see-through "ghost", which would make the test wrong.
  - `tools/zorder-check.ps1` lists what's above Windows EQ and whether it
    counts as covering.
- **Settings:** stored in `%APPDATA%\WindowsEQ` (override with `WEQ_DATA_DIR`).
  - `eq.txt` holds the live EQ, in Equalizer APO format. It's saved 0.4 s
    after a change and reloaded if another program edits it.
  - `presets\*.txt` holds user presets.
  - `config.ini` holds devices, view options and the window position.
- **Undo:** a mouse gesture is one undo step; wheel and typing changes
  coalesce for 0.8 s.
- **Command line:**
  - `--tray`: start hidden in the tray (used by sign-in launch);
  - `--uninstall [--quiet]`;
  - `--render-png out.png [w h secs mx my] [--demo]`: render the UI off-screen
    (`--demo` opens no audio devices);
  - `--write-icon file.ico`;
  - `--crash-test`: throw from a window procedure to check crash reporting.
- **Error handling:**
  - **UI errors:** exceptions in drawing, background ticks, menus and dialogs
    are caught at the message-handler boundary. They're logged to
    `error.log`, and the render target is recreated.
  - **Why the boundary matters:** an exception escaping a window procedure
    makes Windows end the process. That's how 1.0.0 crashed: `updateCurves`
    sized a vector from a negative graph width while the window was
    minimized.
  - **Audio-thread errors:** exceptions in the audio threads become an
    ordinary routing failure, and the app restarts routing after 2 s.
  - **Fatal errors:** an unhandled-exception filter and a `std::terminate`
    handler write `crashes\crash.log` (including the C++ exception message)
    and a minidump, then chain to Windows Error Reporting. Release builds
    produce `WindowsEQ.pdb` (`/Zi`, `/DEBUG /OPT:REF /OPT:ICF`) for reading
    the dumps.

## Installer (`installer/`, `install/`)

- **Per-user, no admin.** It installs to
  `%LOCALAPPDATA%\Programs\WindowsEQ`, adds a Start menu shortcut and an HKCU
  uninstall entry, and can add a sign-in launch (HKCU `Run` →
  `WindowsEQ.exe --tray`).
- **Payload:** the build copies `WindowsEQ.exe` to `build/installer/payload/`
  and embeds it as `RCDATA`. Release builds also copy the installer to `dist/`.
- **Updates:** the setup posts `WM_APP+20` to the running app to make it quit,
  waits for it to exit, replaces the exe atomically, and starts it again.
- **Command line:**
  - `--silent [--autostart | --no-autostart]` installs without a dialog. An
    update keeps the current sign-in choice unless a flag is given, and
    restarts the app if it was running, in the window or in the tray.
  - `--extract-only DIR` just writes the payload (for testing).

## Experimental: system-effect APO (`apo/`, `setup/`)

`WindowsEqApo.dll` is a COM APO built on the SDK's
`CBaseAudioProcessingObject`. Currently it is a bit-exact **passthrough** that
publishes live status in `Global\WindowsEQ.Status.v1` and logs to
`%ProgramData%\WindowsEQ\apo.log`. It is **not** part of the installer.

`weq-setup.exe` must be run as administrator for every command except `list`
and `status`. It copies and registers the DLL, sets `DisableProtectedAudioDG`,
writes the device's `FxProperties` and restarts Windows Audio. It backs up
every original value under `HKLM\SOFTWARE\WindowsEQ` and restores it on
uninstall.

```bash
weq-setup list | status
weq-setup install --device 0 [--slot sfx,mfx,efx]
weq-setup uninstall --device 0 | --all
```

Lessons from bring-up:

- **The audio engine creates APOs aggregated.** An APO that refuses COM
  aggregation still gets loaded, but it silences the endpoint without logging
  anything.
- **A device with no driver effects needs `PKEY_FX_Association`**
  (`KSNODETYPE_ANY`) written next to the effect CLSIDs.
- **AudioEndpointBuilder caches effect settings.** Restart it and the
  services that depend on it, not just Audiosrv.
- **The endpoint ID is `PKEY_AudioEndpoint_GUID`** in
  `pAPOEndpointProperties`, not the KS adapter path.
- **Some drivers never load system effects at all.** Focusrite's own WDM
  driver v4.150 is one; Windows doesn't even load the DLL for that endpoint.
  That's why the router exists.

Next step for this route, not done yet: run the DSP inside the APO, with
parameters in shared memory, for devices that support APOs. That would give
zero added latency.

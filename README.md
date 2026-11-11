# Windows EQ

A system-wide parametric equalizer for Windows. It EQs **everything** you hear
(music, video, games, calls) with studio-quality filters, a live spectrum
analyzer and one-click A/B comparison. It is light on CPU and needs no admin
rights to install.

![Windows EQ](docs/screenshot.png)

## Features

- **Up to 32 bands:** peak, low/high shelf, low/high pass, band pass, notch
  and all pass. Each band can apply to both channels, the left only or the
  right only.
- **Interactive graph:** drag bands with the mouse, scroll to change their
  width, and see each band's curve and the combined curve.
- **Live spectrum analyzer:** shows your audio **before and after** the EQ.
- **EQ ON / BYPASSED switch** (or press `B`) for instant A/B comparison.
- **Presets:**
  - built-in presets, plus your own;
  - **import AutoEQ** headphone corrections (`ParametricEQ.txt`) or
    Equalizer APO configs, and export them back;
  - undo and redo.
- **No clipping:** an **Auto preamp**, a safety limiter and output meters.
- **Runs in the tray:** closing the window keeps the EQ running. It can
  start with Windows.
- **High quality, low CPU:**
  - 64-bit filter maths;
  - click-free parameter changes;
  - a bit-exact passthrough when the EQ is flat;
  - about 2% of one CPU core in total.

## Requirements

- **Windows:** Windows 10 version 1703 or later, or Windows 11, 64-bit.
- **[VB-Cable](https://vb-audio.com/Cable/)**, the free virtual audio cable
  from VB-Audio. Windows EQ receives your audio through it (see
  [How it works](#how-it-works)).

## Install

1. **Install VB-Cable:** download it from
   [vb-audio.com/Cable](https://vb-audio.com/Cable/), extract it, then
   right-click `VBCABLE_Setup_x64.exe` and choose **Run as administrator**.
   Reboot if it asks.
2. **Install Windows EQ:** download `WindowsEQ-Setup.exe` from the
   [latest release](../../releases/latest) and run it.
   - It installs for your account only (no admin prompt).
   - Leave **Start Windows EQ when I sign in** ticked if you want the EQ on
     all the time.
3. **Send Windows audio through the EQ:** click the speaker icon in the
   taskbar and choose **CABLE Input** as the output. If you forget, Windows
   EQ shows a banner with a button that opens Sound settings.
4. **Choose your real speakers or headphones** in Windows EQ's **OUTPUT**
   box at the top.

**SmartScreen warning:** the installer isn't code-signed, so Windows may say
"Windows protected your PC". Click **More info → Run anyway**.

## Using it

| Do this | To |
|---|---|
| Double-click the graph | Add a band there |
| Drag a band | Change its frequency and gain |
| Scroll on a band | Change its width (Q) |
| Right-click a band | Remove it |
| Hold **Shift** while dragging or scrolling | Fine adjustment |
| Double-click a band | Reset its gain to 0 dB |
| Drag, scroll or click a number field | Edit it (typing `1.5k` works) |
| `B` | EQ on / bypassed |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Delete` | Remove the selected band |
| `F12` | Show frame timings |

Tips:

- **Use a measured correction:** for headphones, find your model on
  [AutoEQ](https://autoeq.app), download its `ParametricEQ.txt`, click
  **Import**, then **Save as** to keep it as a preset.
- **To hear the EQ working,** pick **Built-in · Telephone (test)** and
  press `B` a few times.
- **Avoid clipping:** if the **Auto** button under the preamp turns blue,
  your boosts could clip. Click it.
- **The tray:** closing the window hides Windows EQ there. Click the icon to
  open it; right-click it for EQ on/off, presets, Start with Windows and
  **Quit**. The icon turns amber while the EQ is bypassed.

Your settings live in `%APPDATA%\WindowsEQ`:

- `eq.txt` is the current EQ, in Equalizer APO format; editing it in a text
  editor works too;
- `presets\` holds your saved presets.

## Limitations

- **Latency:** Windows EQ adds about **50 ms**. That's unnoticeable for music
  and video, but you may notice it when monitoring yourself live or in
  fast-paced games.
- **Keep it running:** while Windows plays to CABLE Input, you hear nothing
  unless Windows EQ is running. Quit asks you first; use the tray, or turn on
  Start with Windows.
- **Some apps aren't EQ'd:** apps that bypass the Windows default output
  (DAWs using ASIO, apps in exclusive mode, programs set to a specific device)
  don't go through VB-Cable.
- **Tested hardware:** so far it has been tested on a Focusrite USB interface
  and HDMI output. The routing adapts to each device's timing, but please
  report problems; the status bar's Latency / Drift / Dropouts numbers help.

## Uninstall

Use **Settings → Apps → Windows EQ → Uninstall**, or the gear menu in the
app. It removes the app, its shortcut and its sign-in launch. It deletes your
presets only if you tick the box.

Afterwards, set your speakers or headphones as the Windows output again.
VB-Cable stays installed; remove it with its own setup if you no longer
need it.

## How it works

```
apps ──► CABLE Input (Windows default) ══VB-Cable══► CABLE Output ──► Windows EQ ──► your speakers
```

Windows offers apps no supported way to process all audio on every device.
Windows EQ therefore receives the audio from a virtual cable, equalizes it,
and plays it on your real device.

An adaptive resampler keeps the two device clocks in step without audible
pitch changes. The routing also handles drivers that stall or deliver audio
irregularly.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the DSP, the router, the
app, the installer and the experimental APO in detail.

## Building from source

You need **Visual Studio** with the *Desktop development with C++* workload
(built and tested with Visual Studio 2026), and **CMake 3.20+**. There are no
other dependencies.

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Visual Studio 2022 (`-G "Visual Studio 17 2022"`) should work but hasn't been
tested.

Outputs:

| File | What |
|---|---|
| `build/gui/Release/WindowsEQ.exe` | The app |
| `build/installer/Release/WindowsEQ-Setup.exe` | Installer, also copied to `dist/` |
| `build/router/Release/weq-router.exe` | Command-line router (same engine, no UI) |
| `build/setup/Release/weq-setup.exe`, `build/apo/Release/WindowsEqApo.dll` | Experimental APO route |

Tests:

```bash
build/tests/Release/eqdsp_tests.exe      # DSP, resampler, drift controller, preset parser
build/tests/Release/analyzer_test.exe    # spectrum analyzer
build/tests/Release/apo_smoke.exe        # APO COM/aggregation smoke test
build/tests/Release/eqdsp_bench.exe      # CPU benchmark
```

`tools/check-install.ps1` reports the state of an installed copy.

## License

[MIT](LICENSE). VB-Cable is a separate product by VB-Audio with its own
license; it is not included in or distributed with Windows EQ.

See [CHANGELOG.md](CHANGELOG.md) for release notes.

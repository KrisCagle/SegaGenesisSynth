# Genisys
<img width="749" height="518" alt="image" src="https://github.com/user-attachments/assets/c2bda7b4-472a-4ded-a733-a1a0ee88c6fb" />

A synthesizer that recreates the sound of 16-bit console FM music, built on a cycle-accurate emulation of the **Yamaha YM2612** FM chip and the **SN76489** PSG.

> **Status: early beta.** Genisys builds as a **VST3** (Windows, macOS, Linux), an **AU** (macOS) and a **Standalone** app. For now the plugin window is a plain list of controls; the themed interface comes later. See the [roadmap](docs/ROADMAP.md).

## Installing

Download the zip for your system from the latest CI run or release, then copy the plugin into your plugin folder:

| System | VST3 goes in | AU (Mac only) goes in |
|---|---|---|
| Windows | `C:\Program Files\Common Files\VST3\` | — |
| macOS | `~/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/Components/` |
| Linux | `~/.vst3/` | — |

Then rescan plugins in your DAW. `Genisys.vst3` is a folder; copy the whole folder. The Standalone app runs on its own, with no DAW needed.

> The builds aren't code-signed yet, so Windows SmartScreen or macOS Gatekeeper may warn the first time. On macOS, right-click the app or plugin and choose **Open**, or run `xattr -dr com.apple.quarantine <path>`.

## What's inside

- **YM2612 FM core** (`synth-core/`)
  - all 8 algorithms and operator-1 feedback
  - the real envelope generator and rate tables
  - SSG-EG, LFO (vibrato and tremolo), detune, and channel-3 special mode
  - written in plain C with no OS dependencies
- **SN76489 PSG core:** 3 square-wave channels plus periodic/white noise.
- **Engine** (`engine/`): the layer every front-end shares.
  - 6-voice allocation, with oldest-voice stealing that doesn't cut off release tails
  - MIDI note and velocity handling
  - patch application
  - a band-limited resampler that converts the chip's native ~53 kHz output to any host sample rate
- **The console's sound path:**
  - the YM2612's 9-bit DAC and its gritty "ladder effect" (switchable to the cleaner YM3438 or a fully clean output)
  - a Model 1-style output filter
- **Drum kit on MIDI channel 10:**
  - 8-bit kick, snare, clap and tom samples played through FM channel 6's DAC, as Genesis games did. They're synthesized, not recorded.
  - hi-hats and cymbals on the PSG noise channel
- **PSG layer:** the square-wave channels can double your FM notes or play a chiptune arpeggio, with envelopes that step at 60 Hz like the original sound drivers.
- **Playing:** velocity, pitch bend, mod-wheel vibrato and sustain pedal; Poly, Mono and Legato modes with glide; stereo Unison; an Octave shift.
- **Effects:** chorus, echo (tempo-synced, ping-pong) and reverb.
- **Patch files:** load community Genesis patches (`.tfi`, `.vgi`, `.dmp`) and save your sounds as `.tfi`.
- **Demo renderer** (`tools/genisys_render`): writes demo WAVs, so you can hear the engine without a DAW.
- **Desktop app** (`desktop/`, Windows): a raylib prototype with an on-screen keyboard, MIDI input, operator editing and WAV recording. The plugin's Standalone app will replace it.
- **Nintendo DS port** (`nds/`): proof that the core runs on real, constrained hardware.

## Building

Requires CMake 3.22+, a C11 compiler, and for the plugin a C++17 compiler: MSVC 2022 on Windows, Xcode/Clang on macOS, or GCC/Clang on Linux. CMake downloads JUCE 9.0.2 automatically, pinned to a verified checksum.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Plugin files end up in `build/plugin/Genisys_artefacts/Release/`.

Options:

| Option | What it builds |
|---|---|
| `-DGENISYS_BUILD_PLUGIN=OFF` | skip the JUCE plugin (on by default, except with MinGW) |
| `-DGENISYS_BUILD_DESKTOP=ON` | raylib desktop app (Windows with MinGW/GCC, needs raylib from MSYS2) |
| `-DGENISYS_BUILD_PC_DEMO=ON` | miniaudio test-tone demo (Windows) |

## Project layout

```
synth-core/   chip emulation (YM2612, SN76489): platform-independent integer C
engine/       voices, patches, MIDI, resampling: shared by every front-end
plugin/       the JUCE plugin (VST3 / AU / Standalone)
tests/        unit tests for the cores and the engine, run through CTest
desktop/      raylib prototype app
pc/           headless test-tone demo
nds/          Nintendo DS port (devkitPro, built separately)
docs/         roadmap and design notes
```

## Credits

The YM2612 envelope-rate, detune and key-code tables are reverse-engineered hardware timing data. They come from the lineage of MAME's YM2612 core (Jarek Burczynski, Tatsuyuki Satoh), refined by Eke-Eke for Genesis Plus GX using Nemesis's and Sauraen's hardware research. See the header of `synth-core/src/ym2612.c`.

## Third-party

- [JUCE](https://juce.com) 9 (AGPLv3), which includes the Steinberg VST3 SDK.

## License

Genisys is free software under the [GNU General Public License v3.0](LICENSE). You can use, share and modify it. If you distribute a modified version, you must also share its source code under the same license.

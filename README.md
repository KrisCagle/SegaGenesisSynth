# Genisys

A synthesizer that recreates the sound of 16-bit console FM music, built on a cycle-accurate emulation of the **Yamaha YM2612** FM chip and the **SN76489** PSG.

> **Status: in development.** The emulation core works and is tested. A VST3 / AU / Standalone plugin is being built. See the [roadmap](docs/ROADMAP.md).

## What's inside

- **YM2612 FM core** (`synth-core/`)
  - all 8 algorithms and operator-1 feedback
  - the real envelope generator and rate tables
  - SSG-EG, LFO (vibrato and tremolo), detune, and channel-3 special mode
  - written in plain C with no OS dependencies
- **SN76489 PSG core:** 3 square-wave channels plus periodic/white noise.
- **Desktop app** (`desktop/`, Windows): a raylib prototype with an on-screen keyboard, MIDI input, operator editing and WAV recording. The plugin's Standalone app will replace it.
- **Nintendo DS port** (`nds/`): proof that the core runs on real, constrained hardware.

## Building

Requires CMake 3.22+ and a C11 compiler (MSVC, GCC or Clang).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Optional targets:

| Option | What it builds |
|---|---|
| `-DGENISYS_BUILD_DESKTOP=ON` | raylib desktop app (Windows, needs raylib) |
| `-DGENISYS_BUILD_PC_DEMO=ON` | miniaudio test-tone demo (Windows) |

## Project layout

```
synth-core/   chip emulation (YM2612, SN76489): platform-independent C
tests/        unit tests for the cores, run through CTest
desktop/      raylib prototype app
pc/           headless test-tone demo
nds/          Nintendo DS port (devkitPro, built separately)
docs/         roadmap and design notes
```

## Credits

The YM2612 envelope-rate, detune and key-code tables are reverse-engineered hardware timing data. They come from the lineage of MAME's YM2612 core (Jarek Burczynski, Tatsuyuki Satoh), refined by Eke-Eke for Genesis Plus GX using Nemesis's and Sauraen's hardware research. See the header of `synth-core/src/ym2612.c`.

## License

Genisys is free software under the [GNU General Public License v3.0](LICENSE). You can use, share and modify it. If you distribute a modified version, you must also share its source code under the same license.

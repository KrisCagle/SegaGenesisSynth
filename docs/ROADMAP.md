# Genisys: Roadmap to a downloadable VST

**Goal:** make this sound as close to a real Genesis as possible, give it a UI that a musician understands without knowing the YM2612 register map, ship a big preset library, and release it on GitHub as a plugin anyone can load in their DAW.

---

## 1. Where things stand today

**What's already strong**

- `synth-core/` is a careful, cycle-level YM2612 + SN76489 emulation in plain C. It covers all 8 algorithms, operator-1 feedback, the real envelope tables, SSG-EG, the LFO, detune and channel-3 special mode. It has no OS dependencies, which is exactly what a plugin needs at its centre.
- There are 39 unit tests across 4 suites, and they all pass.
- The desktop app builds with zero warnings.
- The DS port proves the core runs on very constrained hardware.

**What's missing for "other people download it"**

- There's no plugin build, no CMake, no README, no LICENSE and no CI.
- Patches can't be saved or loaded.
- The app logic for playing notes (voice allocation, note-to-frequency conversion, applying a patch) is copy-pasted across `desktop/`, `pc/` and `nds/`.

---

## 2. Bugs found so far

These need fixing whatever else we decide. The **[Plugin blocker]** ones would break or misbehave inside a DAW.

| # | Bug | Where | What the user hears |
|---|-----|-------|---------------------|
| 1 | Every note uses `block` (octave register) 4, so fnum gets clamped above ~832 Hz | [desktop/main.c:324](../desktop/main.c:324) | **Every note from A5 up plays the same wrong pitch.** I verified this with a test program. |
| 2 | A voice is marked free the instant you release a key, and allocation always picks the lowest free slot | [desktop/main.c:343](../desktop/main.c:343) | Release tails get chopped off when you play the next note |
| 3 | No voice stealing | [desktop/main.c:335](../desktop/main.c:335) | The 7th note of a chord is silently dropped |
| 4 | All 6 channels are summed and then hard-clamped to 16 bits | [synth-core/src/ym2612.c:892](../synth-core/src/ym2612.c:892) | Harsh digital clipping on loud chords. Real hardware mixes in analog, so this never happens there. |
| 5 | Resampling averages either 1 or 2 chip samples per output sample, alternating | [desktop/main.c:64](../desktop/main.c:64) | Uneven filtering adds grit the real console doesn't have. **[Plugin blocker]** At 88.2/96 kHz host rates the chip runs too fast, so everything plays sharp and envelopes speed up. |
| 6 | Lookup-table init uses a plain `static int` flag | [synth-core/src/ym2612.c:227](../synth-core/src/ym2612.c:227) | **[Plugin blocker]** A race condition when two plugin instances load at once |
| 7 | The UI thread writes chip registers while the audio thread reads them, with no lock | desktop/main.c (by design, documented) | **[Plugin blocker]** Glitches under DAW automation |
| 8 | MIDI velocity is ignored | [desktop/midi_input.c:32](../desktop/midi_input.c:32) | Notes play at the same volume no matter how hard you hit the key |
| 9 | Key Scaling is always written as 0 | [desktop/main.c:289](../desktop/main.c:289) | The core supports KS but you can't reach it from the UI, so high notes can't get naturally shorter envelopes |
| 10 | Widgets are drawn before `BeginDrawing()` | [desktop/main.c:750](../desktop/main.c:750) | Works by accident in raylib. This goes away if the plugin UI replaces the raylib app. |

---

## 3. Sound: what makes it *sound like a Genesis*

The core reproduces the YM2612 **chip** accurately, but the Genesis **sound** also comes from what happens after the chip and from how games drove it. These are the gaps, ordered by how much they matter to the ear.

1. **The 9-bit DAC and the "ladder effect" (the most important one).** The original YM2612 sends each channel's output through a 9-bit DAC that drops the low bits and adds crossover distortion near zero. That's where the famous grit on quiet notes and bass comes from. I'd add a selectable **"Chip: YM2612 (Model 1, gritty) / YM3438 (Model 2, clean)"** switch.
2. **The console's analog output filter.** The Model 1 Genesis has a fairly aggressive low-pass filter on its output, which is a big part of the warm, rounded tone people remember. I'd add **"Console: Model 1 / Model 2 / Unfiltered"** and calibrate the curves against published measurements and hardware recordings.
3. **Proper resampling.** Run the chip at its true ~53,267 Hz and use a band-limited resampler to convert to whatever rate the DAW uses. This fixes bug #5 and removes the grit that isn't authentic.
4. **Channel-6 DAC drums (sampled percussion).** Many classic soundtracks play kick/snare samples through channel 6's DAC mode (registers `$2A`/`$2B`), and the core doesn't model DAC mode yet. I'd add it plus a **drum kit built from original, procedurally generated 8-bit samples**. Ripping samples from games has copyright problems.
5. **PSG as a real instrument layer.** Right now the PSG only doubles FM notes with on/off gating. Game sound drivers gave the PSG software volume envelopes and used the noise channel for hi-hats and snares. I'd give it its own envelope, arpeggio and noise-drum modes.
6. **Channel-3 special mode in the UI.** Each operator gets its own pitch, which is good for detuned "fat" chorus sounds and FM percussion. The core already supports it.
7. **Mix calibration.** Balance FM against PSG, and the overall level, against hardware recordings. Also give the mix proper headroom so 6 loud voices don't clip (bug #4).

## 4. Playability: features a musician expects

- **Velocity** controls carrier volume. It's algorithm-aware, so only operators that reach the output get louder. A sensitivity knob set to 0 gives fully authentic behaviour.
- **Pitch bend** (adjustable range), **mod wheel → vibrato depth**, and **sustain pedal**.
- **Voice modes:**
  - Poly (6 voices, the hardware limit)
  - Mono/Legato with **portamento/glide**, which game drivers often used for bass and leads
  - Unison: 2–3 detuned channels for one big note
- **Smarter voice allocation:** reuse the voice that has been released the longest, and steal the oldest note when all 6 are busy.
- **Per-patch stereo:** real hardware panning is L / C / R, with an optional "spread voices" mode.
- **Patch import/export** in the community formats (**TFI** from TFM Music Maker, **DMP** from DefleMask, **VGI**, later **FUI** from Furnace). This opens up thousands of existing Genesis instruments.
- **Undo/redo** and **A/B compare**.

## 5. Presets

The target is about **80–100 factory presets** in categories, all designed from scratch. I won't copy them from game rips, and preset names won't use game titles or trademarks.

| Category | Examples |
|---|---|
| Bass | Slap Bass, Finger Bass, Punch Synth Bass, Rubber Bass, Fretless, Sub, Distorted Bass |
| Lead | Square Lead, Saw Lead, Whistle Lead, Fifth Lead, Sync-style Lead |
| Keys | Tine E.Piano, Clav, Harpsichord, Drawbar Organ, Toy Piano |
| Mallets & Bells | Marimba, Vibraphone, Glockenspiel, Tubular Bell, Music Box, Steel Drum |
| Brass & Winds | Brass Section, Synth Brass, Trumpet, Flute, Clarinet, Oboe |
| Strings & Pads | Strings, Slow Pad, Choir-ish, SSG-EG Sweep |
| Guitar | Distortion Guitar (the classic high-feedback Genesis rock sound), Clean, Muted Pluck |
| Plucks | Harp, Koto, Pizzicato, Banjo |
| FM Percussion | Kick, Snare, Toms, Cowbell, Timpani, Orchestra Hit |
| PSG | Chip Square, Noise Hat, Noise Snare, Arp |
| SFX | Coin Chime, Laser, Jump, Explosion, Siren |
| Drum Kits | DAC samples + FM + noise, mapped across the keyboard |

**Process:** I'll draft the patches and build a tool that renders every preset to WAV so you can audition them quickly. Your ears make the final call, and we tune together.

**Why presets come late:** if parameters change after presets are written (for example adding velocity or KS), every preset has to be revisited. So presets wait until the parameter set is stable.

## 6. UI/UX: Genesis-themed and musician-first

**Problems today**

- There are 32 sliders labelled with register names (TL, D1R, SL) showing raw integers.
- You can't fine-adjust, reset to default, or type in a value.
- The window is a fixed 1150×880.
- Nothing shows which operators are **carriers** (the ones you hear) and which are **modulators** (the ones that shape tone). That's the most important thing to understand in FM synthesis.
- There's no output meter, no preset browser and no way to save.

**Direction**

- **Theme:** Model 1 console styling with a matte black chassis, ridged vent-grille texture, the bold red/white "16-bit" stripe look, a preset display styled like a **cartridge label** that slots in, and buttons that echo the **A / B / C controller buttons**. All artwork would be original with **no Sega logo or wordmark**, because those are trademarks (see §8).
- **Two views:**
  - **Simple:** macro knobs for Brightness, Attack, Release, Vibrato and Crunch (DAC grit) that adjust the right operators behind the scenes. Someone who has never touched FM can shape a sound.
  - **Advanced:** full operator editing.
- **Musical labels with real names underneath.** For example: "Attack" with "AR" in small text, "Ratio ×2" instead of `MUL 2`, and a level control that reads **"Volume"** on carriers and **"Brightness / Mod amount"** on modulators, switching with the algorithm.
- **Algorithm picker:** 8 clickable thumbnails in the standard FM diagram style, with carriers highlighted in every operator panel.
- **Editable envelope graphs:** drag the points; the graph is rendered from the real envelope generator, which you already do. A playhead follows the note you're holding.
- **Standard plugin interaction conventions:**
  - drag to turn a knob, Shift+drag for fine control
  - double-click resets to default
  - mouse-wheel adjusts the value
  - right-click lets you type a value
  - hover shows a plain-English tooltip
- **Scalable window:** 100–200%, for high-DPI screens.
- **Feedback:**
  - output level meter
  - oscilloscope
  - 6 voice-activity LEDs
  - on-screen keyboard that lights up incoming MIDI
  - pitch/mod wheels

I'd make a **clickable mockup first** so we can agree on the look before writing UI code.

## 7. The VST: architecture

```
┌──────────────────────── Plugin (VST3 / AU / Standalone) ───────────────────────┐
│  UI (themed editor)  ──params──▶  Parameter store (atomic, automatable, saved) │
│                                         │ read once per audio block            │
│  DAW MIDI ─────────────────────────▶  Engine (C, one instance per plugin)      │
│                                         ├─ voice allocator / velocity / glide  │
│                                         ├─ YM2612 + SN76489 cores (unchanged)  │
│                                         ├─ DAC ladder + console filter         │
│                                         └─ resampler → host sample rate        │
└────────────────────────────────────────────────────────────────────────────────┘
```

- **A new `synth-core/engine` layer in C** holds the voice allocation, patch model, MIDI handling and resampling. It's instance-based (no globals), so multiple plugin instances in one DAW project don't interfere. The plugin, `pc/` and optionally `nds/` all share it, which ends the copy-paste. *Industry convention:* keep DSP separate from the UI and from the plugin wrapper so you can test and reuse it.
- **Plugin shell: JUCE (recommended).** JUCE is the industry-standard C++ framework for audio plugins.
  - From one codebase it builds **VST3** (Win/Mac/Linux), **AU** (Logic, on Mac) and a **Standalone app**. The standalone app would replace the raylib app.
  - It also handles automation, saving state in your DAW project, and MIDI from the host.
  - Most audio-dev job postings mention it.
  - **Costs:**
    - It needs the **free Visual Studio Build Tools** on Windows, a few GB. JUCE officially supports Microsoft's compiler there, and building it with MinGW/GCC isn't supported.
    - It means learning some C++.
    - Its license is AGPLv3, or a free "Starter" commercial license, so the project needs a compatible open-source license.
- **Alternative: CLAP + clap-wrapper with a custom C UI**, built with your existing GCC. It's closer to the current code, but we'd build all the UI plumbing ourselves, and the result is less useful on a résumé.

## 8. Shipping it

- **CMake build** replacing `.vscode/tasks.json`. *Why:* anyone on any OS, plus CI, can build it the same way. Tests run through **CTest**.
- **GitHub Actions:**
  - Every push builds and tests on Windows, macOS and Linux.
  - Pushing a version tag (`v0.1.0`) builds release zips (VST3 + Standalone) and attaches them to a **GitHub Release**.
  - CI can build the Mac version even without a Mac.
- **README** with screenshots, audio demos, per-OS install steps (where `.vst3` files go) and troubleshooting.
  - Unsigned plugins trigger macOS Gatekeeper and Windows SmartScreen warnings. Code signing costs money, Apple's is $99/yr, so v1 documents the workaround instead.
- **LICENSE + third-party notices.**
  - The envelope/detune tables come from the MAME / Genesis Plus GX lineage. Your source comments already credit this. Before a public release we should confirm the license terms and add a NOTICE file.
  - Also license notices for JUCE and the VST3 SDK.
- **Name:** Genisys (see §10).
- **Versioning:** Semantic Versioning plus a `CHANGELOG.md`. Public **0.x betas** start as soon as the plugin loads in a DAW, and 1.0 comes after the UI and presets.

## 9. Proposed order of work

Each phase ends with a working build, passing tests and a commit or PR.

| Phase | What | Why this order |
|---|---|---|
| **0. Housekeeping** | Commit the pending DS work, add CMake + CTest, add CI, add a README skeleton, decide the license | Every later step builds and tests on top of this |
| **1. Engine + bug fixes** | Extract `synth-core/engine`, fix bugs #1–#9 with new tests, switch the desktop app over to the engine | The plugin needs this layer, and fixing the bugs here fixes them everywhere at once |
| **2. Plugin shell** | JUCE project, parameters, state save/load, a plain auto-generated editor, VST3 + Standalone | **De-risks the biggest unknown early.** Every later improvement can then be heard in a real DAW. This becomes **v0.1 beta.** |
| **3. Authentic sound** | DAC ladder, console filter, resampler, DAC drums, PSG layer, ch3 mode, mix calibration | Changes the sound itself, so it comes before presets get tuned |
| **4. Playability** | Velocity, bend/mod/sustain, mono/glide/unison, patch import/export, undo, A/B | Finishes the **parameter set** |
| **5. UI/UX** | Mockup → themed editor with Simple/Advanced views | Builds on a stable parameter set |
| **6. Presets** | 80–100 presets + WAV audition tool | Only once parameters stop changing |
| **7. Release 1.0** | Release pipeline, docs, demo audio, notices | — |

## 10. Decisions (made 2026-10-09)

1. **Plugin framework: JUCE.** Requires Visual Studio Build Tools (MSVC) on Windows.
2. **Product name: Genisys.** The audience is Kris's friends, not a commercial release, so the trademark question is settled. The UI still uses original artwork rather than Sega logos.
3. **Who builds:** Claude implements and explains; Kris approves installs, commits/pushes, releases and scope calls.
4. **Platforms:** Windows + macOS + Linux, all built by GitHub Actions. Windows is the one tested by hand.
5. **DS port:** out of scope for now. The focus is the VST only.

# PitchGrid for Ableton Move

A microtonal scale engine for [Ableton Move](https://www.ableton.com/move/), built as a [Move Everything](https://github.com/charlesvestal/move-anything) MIDI FX module. Retunes the pad grid to any MOS (Moment of Symmetry) scale using the [scalatrix](https://github.com/pitchgrid-io/scalatrix) library, with full MPE output for precise per-note pitch control.

**Important: Requires an MPE-enabled synth.** Currently only tested with Surge XT (via [move-anything-surge](https://github.com/charlesvestal/move-anything-surge)).

## Quick Start

1. Set **ME Slot 1-4** as your instrument
2. Route MIDI to **channels 1-4**
3. In the **Move Everything shadow UI**, insert **PitchGrid** as a MIDI FX module
4. In the **Move Everything shadow UI**, insert **Surge XT** (or any MPE-capable synth) as the synth engine
5. **Enable MPE mode** on the synth
6. Set the **MPE pitch bend range** in PitchGrid and the synth to the **same value** (lower values = better resolution = more precise pitch)

## Parameters

| Parameter | Range | Description |
|-----------|-------|-------------|
| Preset | 0-190 | Factory preset (hand-crafted + TAMNAMS EDO-MOS scales) |
| Depth | 1-7 | MOS scale depth (number of generator iterations) |
| Stretch | 100-2400 | Equave size in cents (1200 = standard octave) |
| Skew | 0.0-1.0 | Generator ratio (determines scale structure) |
| Mode | 0-20 | Modal rotation of the scale |
| Root | -1200 to 1200 | Root frequency offset in cents |
| Layout | chromatic / in-key | Pad layout mode |
| Row Ofs | 3-9 | Chromatic steps between pad rows |
| PB Range | 1-96 | MPE pitch bend range in semitones |

## How It Works

PitchGrid generates scales using a two-MOS approach:
- **Scale MOS** (at `depth`) defines the scale degrees and pad coloring (root / in-scale / off-scale)
- **Chromatic MOS** (at `depth + extra_depth`) defines the full chromatic grid

Incoming pad MIDI is intercepted via firmware hooks, mapped to the chromatic grid, and output as MPE with per-note pitch bend to achieve microtonal tuning.

## Things to Note

- A single active PitchGrid instance will modify the **pad layout and coloring for all instrument tracks**. Drum tracks remain unaffected.
- For MIDI note handling (microtonal retuning), a PitchGrid instance is required **on every track** that should be affected by tuning.

## Building

Requires [Docker](https://www.docker.com/).

```sh
# Clone with submodules
git clone --recursive https://github.com/pitchgrid-io/move-anything-pitchgrid.git
cd move-anything-pitchgrid

# Build the Docker cross-compilation image (one-time setup)
cd move-anything && docker build -t move-anything-builder . && cd ..

# Build DSP shared library (ARM64 cross-compilation)
./scripts/build-dsp.sh

# Build and deploy to Move
./scripts/build-dsp.sh --deploy
```

Dependencies ([scalatrix](https://github.com/pitchgrid-io/scalatrix) and [move-anything](https://github.com/charlesvestal/move-anything)) are included as git submodules and pinned to specific commits for reproducible builds.

## Project Structure

```
dsp/                  C DSP source (scale engine, layout, MPE)
pad_hooks/            Pad hooks API — firmware pad interception layer
src/module.json       Module metadata
src/ui.js             QuickJS UI
scripts/              Build, deploy, and preset generation scripts
scalatrix/            Scalatrix library (submodule)
move-anything/        Move Everything framework (submodule)
```

## License

MIT

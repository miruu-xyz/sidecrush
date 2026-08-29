# HardCap

**A sidechain-driven dynamic hard clipper.** VST3, by [miruu](https://github.com/miruu-xyz).

It recreates the crunch that happens when a sub-bass and a distorted signal are summed into a clipper: the sub's peaks push the other signal into the clipping region, where its detail is *flattened* rather than turned down. HardCap does this deliberately, driving a clip ceiling — the lid — from a sidechain signal at waveform rate.

Despite the family resemblance, it is not a ring modulator. There is no bipolar multiply and no phase inversion.

---

## The idea

The sidechain does not drive a VCA. It drives a **clip ceiling that descends into the carrier.**

```
mag  = |lowpass(sidechain)|
t    = clamp((mag - floor) / (ceiling - floor), 0, 1)
lid  = 1 - t^p                p = 2^(-4 * shape)

CLIP on:   out = clamp(carrier * pre, ±lid)
CLIP off:  out = carrier * pre * lid
```

A multiply **attenuates**: at the sidechain peak the output is silent, and the carrier's detail is perfectly intact — merely scaled to nothing. A clamp **flattens**: the output is at full level and the carrier's detail is gone for good.

The two cannot be morphed into each other by any curve, because the clamp reads the carrier and the multiply does not. They agree at exactly two points — fully open and fully shut — and disagree across the entire descent, which is where all the character lives. The `CLIP` toggle picks between them; `PRE` drives the carrier up into the lid and is the difference between "barely touched" and "annihilated".

There is **no smoothing after the rectifier** — no attack, no release, no envelope follower. The lid tracks the sidechain waveform sample by sample. Smoothing there would turn this into a sidechain compressor, and those already exist.

Full details, including everything deliberately left out, are in [SPEC.md](SPEC.md).

## Controls

| | |
|---|---|
| **PRE** | Carrier drive into the lid. The aggression control. |
| **CEILING** | Sidechain level at which the lid slams fully shut. |
| **FLOOR** | Sidechain level below which nothing happens at all. |
| **SHAPE** | Bipolar. Where the lid breaks: late at CEILING (−1), linear (0), instantly at FLOOR (+1). |
| **FILTER** / **SLOPE** | Lowpass on the detector, 20 Hz–20 kHz then OFF, 6–48 dB/oct. |
| **OUTPUT** | Output gain. |
| **CLIP** | Clip ceiling, or plain VCA ducking. |
| **FILTER PRE/POST** | Rectifier order. POST turns the detector into an envelope follower. |
| **MONO/STEREO** | Sidechain detection linking. The output is always stereo. |
| **EXT/INT** | External sidechain bus, or the main input driving its own lid. |

Everything runs at 8× oversampling: a hard clip whose threshold moves every sample is about the most alias-prone thing you can build.

## Building

Requires CMake 3.22+ and a C++20 compiler. JUCE 9.0.1 is fetched automatically.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The VST3 lands in `build/HardCap_artefacts/Release/VST3/`, and a standalone app beside it.

On macOS, building without full Xcode works — Command Line Tools are enough for VST3 and Standalone.

### Tests

Two, both plain executables with no framework:

- `hardcap_engine_test` — the DSP core in isolation. Asserts the window endpoints, the SHAPE curve, filter stability at 20 Hz with 8 poles, and the claim the whole plugin rests on: that a clamp flattens where a multiply scales.
- `hardcap_host_test` — instantiates the real `AudioProcessor` and pushes audio through it across five bus layouts and three sample rates, checking for NaN and verifying state round-trips.

They use a `CHECK` macro rather than `assert`, because `assert` compiles out under `NDEBUG` and a self-check that vanishes in Release is worse than none.

## Status

The DSP is complete and matches the spec. The interface is **functional but not yet the real one** — controls, layout and the oscilloscope all work, but the Figma design's custom knob artwork, glows and typography are not yet drawn. That is the next chunk of work.

## Licence

MIT — see [LICENSE](LICENSE).

Built with [JUCE](https://juce.com) under the free Starter tier; JUCE's own modules remain under the JUCE licence. If you fork this and ship it, you need your own JUCE licence — which is normal, and free below $20k/year.

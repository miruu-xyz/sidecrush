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
| **HQ** | Oversampling quality. On: 8×, linear phase. Off: 4×, minimum phase, for about a third of the CPU. |

By default everything runs at 8× oversampling: a hard clip whose threshold moves every sample is about the most alias-prone thing you can build. Both paths need it — the clipper loses roughly 9 dB of alias floor per halving, and the detector cannot be run slower and interpolated up, because the interpolation's images land exactly on the decimator's fold points.

**HQ** off drops both paths to 4× minimum phase when you would rather have the CPU back: measured at about a third the cost, for an alias floor of −60 dB instead of −69 dB. Toggling it never changes the reported latency — the cheaper path is padded back out to match, so no host is asked to renegotiate delay compensation mid-session — and the swap is ducked over a few milliseconds, because linear phase and minimum phase do not line up and the seam would otherwise click.

## Building

Requires CMake 3.22+ and a C++20 compiler. JUCE 9.0.1 is fetched automatically.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The VST3 lands in `build/HardCap_artefacts/Release/VST3/`, with a standalone app beside it, and is **copied into your user plug-in folder after every build** so it is immediately loadable in a DAW:

| | |
|---|---|
| macOS | `~/Library/Audio/Plug-Ins/VST3/` |
| Windows | `%COMMONPROGRAMFILES%\VST3\` |
| Linux | `~/.vst3/` |

Pass `-DHARDCAP_COPY_AFTER_BUILD=OFF` to skip it. CI always does.

On macOS, building without full Xcode works — Command Line Tools are enough for VST3 and Standalone.

### Tests

Two, both plain executables with no framework:

- `hardcap_engine_test` — the DSP core in isolation. Asserts the window endpoints, the SHAPE curve, filter stability at 20 Hz with 8 poles, and the claim the whole plugin rests on: that a clamp flattens where a multiply scales.
- `hardcap_host_test` — instantiates the real `AudioProcessor` and pushes audio through it across five bus layouts and three sample rates, checking for NaN and verifying state round-trips. It also guards the three things most likely to break silently: that the detector's routing shortcuts are bit-identical to the long way round, that toggling HQ does not move the reported latency, and that the HQ swap does not click.

They use a `CHECK` macro rather than `assert`, because `assert` compiles out under `NDEBUG` and a self-check that vanishes in Release is worse than none.

### Measurement tools

Two more executables that print numbers rather than pass or fail. They are **not** built by default and are not registered with `ctest`, so CI never links them — build them by hand when you touch the DSP:

```bash
cmake --build build --target hardcap_bench hardcap_alias
```

They land next to the tests, in `build/hardcap_bench_artefacts/<config>/` and `build/hardcap_alias_artefacts/<config>/`.

- `hardcap_bench` — where the CPU goes. Cost per block size and sample rate, HQ on versus off, each sidechain routing, the cost of each oversampler and of the engine loop on its own, and how far the output steps when HQ is toggled mid-signal.
- `hardcap_alias` — what the oversampling buys. Alias floor per factor for the clipper and for the detector, FIR against IIR, and how far the finished output moves if the detector's upsampler is swapped for a cheaper one.

Between them they are the evidence for why both paths sit at 8×, why the detector cannot simply run slower, and what HQ costs. The constants in `PluginProcessor.h` quote their figures, so if you change the DSP, re-run them and update the comments.

### CI

Every push builds and validates on Linux and Windows: `ctest`, then `pluginval` at strictness 10, plus a separate ASan/UBSan job. **macOS runs on tags and manual dispatch only** — it bills at 10x on a private repo and it is the one platform that can be tested locally for free. Run it on demand from the Actions tab when you want it.

A newer push cancels an in-flight run for the same ref. Tags are exempt.

## Status

The DSP is complete and matches the spec. The interface is **functional but not yet the real one** — controls, layout and the oscilloscope all work, but the Figma design's custom knob artwork, glows and typography are not yet drawn. That is the next chunk of work.

**HQ** has no on-screen control yet. The parameter exists and is automatable and saved, so hosts will show it in their generic view, but it needs a place in the Figma layout before it gets a button.

## Licence

MIT — see [LICENSE](LICENSE).

Built with [JUCE](https://juce.com) under the free Starter tier; JUCE's own modules remain under the JUCE licence. If you fork this and ship it, you need your own JUCE licence — which is normal, and free below $20k/year.

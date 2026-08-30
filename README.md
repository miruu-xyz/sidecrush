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
| **STEREO/MONO/WTF** | Sidechain detection linking. The output is always stereo. |
| **EXT/INT** | External sidechain bus, or the main input driving its own lid. |
| **HQ/LQ/YUCK** | Oversampling quality: 8× linear phase, 4× minimum phase, or none at all. |

By default everything runs at 8× oversampling: a hard clip whose threshold moves every sample is about the most alias-prone thing you can build. Both paths need it — the clipper loses roughly 9 dB of alias floor per halving, and the detector cannot be run slower and interpolated up, because the interpolation's images land exactly on the decimator's fold points.

**LQ** drops both paths to 4× minimum phase when you would rather have the CPU back: measured at about a third the cost, for an alias floor of −60 dB instead of −69 dB. **YUCK** goes further and turns oversampling off entirely — 1×, no anti-imaging filter of any kind, a tenth of HQ's CPU and a −32 dB alias floor. That is not a saving, it is the point: the moving clip folds its own harmonics back down the spectrum and you hear it. Changing quality never moves the reported latency — the cheaper paths are padded back out to match, so no host is asked to renegotiate delay compensation mid-session — and the swap is ducked over a few milliseconds, because none of the three cascades line up and the seam would otherwise click.

**WTF** is the third position on the sidechain link. It sums the sidechain like MONO, then splits the sum by sign and hands one half to each channel: the modulator's positive peaks clip only the left, its negative peaks only the right. On a low sub the two clippers take turns and the carrier appears to pan. In POST the split happens before the filter, so each side gets its own envelope and the pan survives the smoothing. The scope shows it: the aperture's top edge is the left lid and its bottom edge the right, and the output is drawn once per channel — full brightness where the two channels agree, both fading back to 30% where the clippers are taking turns.

The **WTF** dial next to it says how far to take that, and it spans further in both directions than the switch alone can. At 0% the split closes up and both channels get the same lid, which is MONO. 50% is the behaviour above, and the default. At 100% the plugin stops throwing away the part of the carrier the lid cuts and hands it to *both* channels with opposite signs instead — so in stereo it is still there, wide and on the wrong side of the lid, and on the way to mono it cancels. The point of that is what a mono listener hears: below 100% WTF half-cancels when summed, because at any instant one channel is clipped and the other is not, so a phone speaker gets a weaker plugin than the room does. At 100% the sum is *exactly* the MONO link while the stereo image comes apart. Right-click the dial for the three settings by name.

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

- `hardcap_engine_test` — the DSP core in isolation. Asserts the window endpoints, the SHAPE curve, filter stability at 20 Hz with 8 poles, WTF's intensity landing on MONO, the original WTF and an exact mono sum at its three marked settings, and the claim the whole plugin rests on: that a clamp flattens where a multiply scales.
- `hardcap_host_test` — instantiates the real `AudioProcessor` and pushes audio through it across five bus layouts and three sample rates, checking for NaN and verifying state round-trips. It also guards the five things most likely to break silently: that the detector's routing shortcuts are bit-identical to the long way round, that WTF actually drives the two channels apart, that WTF at 100% sums back to exactly the MONO link, that changing quality does not move the reported latency, and that the swap does not click.

They use a `CHECK` macro rather than `assert`, because `assert` compiles out under `NDEBUG` and a self-check that vanishes in Release is worse than none.

### Measurement tools

Two more executables that print numbers rather than pass or fail. They are **not** built by default and are not registered with `ctest`, so CI never links them — build them by hand when you touch the DSP:

```bash
cmake --build build --target hardcap_bench hardcap_alias
```

They land next to the tests, in `build/hardcap_bench_artefacts/<config>/` and `build/hardcap_alias_artefacts/<config>/`.

- `hardcap_bench` — where the CPU goes. Cost per block size and sample rate, each quality mode, each sidechain routing, the cost of each oversampler and of the engine loop on its own, and how far the output steps when quality is changed mid-signal.
- `hardcap_alias` — what the oversampling buys. Alias floor per factor for the clipper and for the detector, FIR against IIR, and how far the finished output moves if the detector's upsampler is swapped for a cheaper one.

Between them they are the evidence for why both paths sit at 8×, why the detector cannot simply run slower, and what each quality mode costs. The constants in `PluginProcessor.h` quote their figures, so if you change the DSP, re-run them and update the comments.

### Looking at the GUI

`hardcap_shot` renders the editor offscreen to a PNG. It exists because the only other ways to see the interface are a DAW or the Standalone, and the Standalone wrapper opens the default audio input *and* output — which on a laptop can feed back through the monitors. Also not built by default:

```bash
cmake --build build --target hardcap_shot
build/hardcap_shot_artefacts/Release/hardcap_shot out.png [args...]
```

| argument | effect |
|---|---|
| `<id>=<value>` | any parameter from `ids::`, in its own units — `ceiling=-24`, `slope=3`, `clip=0` |
| `audio=<hz>` | push a sub at that frequency through the sidechain against a carrier, so the scope has something to draw |
| `hover=<id>` | put a control into its hover state — `hover=clip`, `hover=ceiling`, `hover=scope` |
| `drag=<id>` | put a control into its dragging state, which is what the scope's overlays key off — `drag=ceiling`, `drag=shape` |
| `drag=<id>:<dy>` | actually press and pull it that many pixels, negative being up — `drag=scope:-40`, `drag=slope:60`. Checks what a drag *does*, not just how it looks |
| `settings` | render the settings panel instead of the scope |
| `crop=x,y,w,h` | crop, in design coordinates |
| `scale=N` | supersample, for looking at detail |

An unknown key is an error rather than a silent no-op: a typo that quietly renders the default state is worse than useless when the point is comparing against a design.

`tools/pngpick.py` reads pixels back out (`python3 tools/pngpick.py shot.png 640,140`), which is how the palette was matched to `Resources/reference/figma-1-11.png` — the design leans on `color-dodge`, so the layer colours are not the rendered colours.

### CI

Every push builds and validates on Linux and Windows: `ctest`, then `pluginval` at strictness 10, plus a separate ASan/UBSan job. **macOS runs on tags and manual dispatch only** — it bills at 10x on a private repo and it is the one platform that can be tested locally for free. Run it on demand from the Actions tab when you want it.

A newer push cancels an in-flight run for the same ref. Tags are exempt.

## Status

The DSP is complete and matches the spec, and the interface now implements the Figma design — layout, typeface, dial and fader artwork, the recessed wells, the glows, the oscilloscope overlays, and the hover and engaged states from the file's component variants. See [SPEC §5](SPEC.md) for what was taken from where.

**QUALITY** has a control: the gear in the scope's upper-right swaps in the settings panel, where it sits alongside the sidechain link, FILTER PRE/POST and SIGNAL EXT — matching the Oscilloscope "Variant3" component rather than a popup, because the file has no popup designed anywhere. Both it and the link pill are three-way cycles.

The scope's overlays are state-driven rather than always on — see [SPEC §5.3](SPEC.md) for which control summons which.

Known gaps, both cosmetic: the readouts print the parameters' own strings (`-6.0 dB`) where the design shows a compact `0dB`, and the design's fader caps are parked at a position that does not correspond to their labelled value, so cap travel is mapped linearly across the track instead.

## Licence

MIT — see [LICENSE](LICENSE).

Built with [JUCE](https://juce.com) under the free Starter tier; JUCE's own modules remain under the JUCE licence. If you fork this and ship it, you need your own JUCE licence — which is normal, and free below $20k/year.

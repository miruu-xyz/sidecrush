# HardCap — Specification

**HardCap by miruu** — a sidechain-driven dynamic hard clipper.

Recreates the crunch that happens when a sub-bass and a distorted signal are summed into a clipper: the sub's peaks push the other signal into the clipping region, where its detail is flattened rather than turned down. HardCap does this deliberately and controllably, driving a clip ceiling ("the lid") from a sidechain signal at waveform rate.

Not a ring modulator, despite the original framing. There is no bipolar multiply and no phase inversion.

---

## 1. Core algorithm

The sidechain does not drive a VCA. It drives a **clip ceiling that descends into the carrier**.

```
sc      = source select (EXT sidechain bus | INT main input)
sc      = MONO ? (L+R)/2 : per-channel

FILTER PRE:   mag = |lowpass(sc)|
FILTER POST:  mag = max(0, lowpass(|sc|))     -- clamped, see 4.3

t       = clamp((mag - floor) / (ceiling - floor), 0, 1)
d       = t^p              p = clamp(2^(-4 * shape), 1/32, 32)
lid     = 1 - d

CLIP on:   out = clamp(carrier * pre, -lid, +lid)
CLIP off:  out = carrier * pre * lid

out     = out * output
```

`floor` and `ceiling` are linear amplitudes converted from their dBFS parameters. Mapping is done in the **linear** domain with dB-labelled endpoints — that is what makes SHAPE 0 a true 1:1 curve.

### Why a clip ceiling and not a gain

A multiply attenuates: at the sidechain peak the output is silent and the carrier's detail is intact, merely scaled to nothing. A clamp flattens: the output is at full level and the carrier's detail is destroyed.

The two cannot be morphed into each other by any curve, because **the clamp reads the carrier and the multiply does not**. They agree at exactly two points — fully open (gain 1) and fully shut (gain 0) — and disagree across the entire descent, which is where all the character lives.

Consequences of the clip path that the VCA path does not have:
- generates harmonic distortion of the carrier, not just translated sidebands
- depends on the carrier's own level, so quiet passages survive a sidechain peak that destroys loud ones
- peaks are walls, not holes

The CLIP toggle selects between them. `PRE` drives the carrier up into the lid and is the difference between "barely touched" and "annihilated".

### Detector notes

- **Rectified**, so a sine sidechain produces two lid closures per cycle — matching a real clipper, which clips both peaks.
- **No smoothing anywhere after the rectifier.** No attack, no release, no envelope follower, no parameter ramp on the lid. The pre-rectifier lowpass is the only smoothing in the detector, and that is deliberate: smoothing here turns HardCap into a sidechain compressor.
- Even with the lid fully open the plugin still bites, because once `mag >= ceiling` the lid is 0 and the carrier is clamped to silence regardless of its own level.

### SHAPE

Bipolar. Controls **where along the sidechain's travel the lid breaks**, not what the break does.

| shape | p | behaviour |
|---|---|---|
| −1 | 16 | lid stays open, breaks late, at CEILING |
| 0 | 1 | linear descent across the FLOOR→CEILING window |
| +1 | 1/16 | lid slams shut immediately above FLOOR |

`p` is clamped to [1/32, 32] rather than reaching a true step. A literal instantaneous step at audio rate is a click generator — a broadband transient twice per sidechain cycle — and a very fast finite break is audibly identical.

---

## 2. Parameters

| Parameter | Type | Range | Default | Notes |
|---|---|---|---|---|
| PRE | float, dB | −36 … +36 | 0 | carrier drive into the lid |
| CEILING | float, dBFS | −60 … 0 | −6 | sidechain level at which the lid fully closes |
| FLOOR | float, dBFS | OFF … 0 | OFF | absolute; displays "OFF" at minimum |
| SHAPE | float | −1.00 … +1.00 | 0.00 | bipolar, centre detent |
| FILTER | float, Hz | 20 … 20000, then OFF | OFF | logarithmic; OFF is the top detent |
| SLOPE | choice | 6/12/18/24/30/36/42/48 dB/oct | 12 | 1–8 poles, snapped |
| OUTPUT | float, dB | −36 … +36 | 0 | same range as PRE, deliberately |
| CLIP | bool | off / on | **on** | on = clip ceiling, off = VCA multiply |
| FILTER POS | choice | PRE / POST | PRE | rectifier order |
| SC LINK | choice | MONO / STEREO | STEREO | sidechain detection only; output is always stereo |
| SC SOURCE | choice | EXT / INT | EXT | INT = main input drives its own lid |
| HQ | bool | off / on | **on** | on = 8x linear phase, off = 4x minimum phase (see 4.1) |

Notes:

- **FLOOR is absolute, not relative to CEILING** — sweeping CEILING does not drag FLOOR with it. FLOOR is internally clamped to `min(floor, ceiling - 1 dB)` so the window can never invert. The preview shows the effective window, so the clamp is visible rather than silent.
- **PRE and OUTPUT share a range** so the pair is not confusing to read.
- **CEILING turns conventionally**: clockwise = higher dBFS = gentler. Counter-clockwise lowers the ceiling.
- 20 kHz is the top of the FILTER sweep because Nyquist at 48 kHz is 24 kHz. Above ~20 kHz the filter does nothing, so the top of the sweep *is* the off position.
- Smoothing (~10–20 ms) on PRE, OUTPUT, CEILING, FLOOR and SHAPE to avoid zipper noise. **Never** on the lid path.

---

## 3. Signal flow

```
                        ┌─────── 8x oversampled, 4x without HQ ────────┐
                        │                                              │
 main in ──> PRE ───────┼──────────────────────────┐                   │
                        │                          │                   │
 sc source ──> link ────┼──> filter ──> rectify ──> t ──> d = t^p ──> lid
   EXT | INT   MONO|ST  │   (PRE/POST order)                 │         │
                        │                                    v         │
                        │                       CLIP ? clamp(x, ±lid)  │
                        │                            : x * lid         │
                        └──────────────────┬───────────────────────────┘
                                           v
                                        OUTPUT ──> main out
```

Sidechain source and link happen at base rate; everything from the filter onward runs oversampled — 8x with HQ on, 4x with it off.

---

## 4. Implementation notes

### 4.1 Oversampling

**8x with HQ on, 4x with it off.** Four independent alias sources: the rectifier's corner at every zero crossing, the `t^p` exponent, the moving hard clamp, and the multiply itself (`carrier * lid` is bandwidth-expanding even in VCA mode, since `lid` carries harmonics up to Nyquist and the products fold). Moving hard clip is the worst of them; dedicated clippers routinely run 16–32x.

Use `juce::dsp::Oversampling` and report latency to the host.

Both the carrier and the detector run at the full rate, and measurement says both have to. Alias floor against the fundamental, from `tests/alias.cpp`:

| | 1x | 2x | 4x | 8x |
|---|---|---|---|---|
| clipper | −32 dB | −48 dB | −60 dB | −69 dB |

Roughly 9 dB per halving. The obvious saving — run the detector slower and interpolate the lid up to the carrier's rate — **does not work**: the interpolation's images land on the decimator's fold points, so a 2x detector with an interpolated lid measures −53 dB against 8x's −85 dB. The lid has to be computed at the rate it is used at.

Filtering at base rate and upsampling afterward stays valid in principle — the filter is linear and generates no aliasing — but it is not where the money is. Profiling put the engine loop at roughly an eighth of the plugin's cost and the two oversamplers at the rest, so a second code path would buy almost nothing. The rectifier is the part that cannot move.

What the routing *can* skip is arithmetic it has already done: with INT + STEREO the detector is the main input, so the carrier's upsampled block is reused directly, and any routing that puts the same signal on every detector channel upsamples one channel instead of two. Both are exact and are checked bit-for-bit in `tests/host_test.cpp`.

#### HQ

HQ off switches both paths to 4x polyphase IIR — minimum phase instead of linear — for about a third of the CPU, at an alias floor of −60 dB instead of −69 dB. Two constraints make it shippable rather than merely cheap:

- **Latency must not move.** The shorter path is padded back out to the 8x figure, so `setLatencySamples` reports the same number in both modes and no host is asked to renegotiate delay compensation mid-session.
- **The swap must not click.** Linear phase and minimum phase do not line up, so the seam steps regardless of how carefully the incoming cascade is primed — priming does not fix it. The output is ducked over ~4 ms and held silent until the padding delay has flushed. Unducked, the seam steps 22x the steady-state sample delta.

Both oversampler pairs are built in `prepareToPlay`, so toggling HQ never allocates on the audio thread.

### 4.2 The shaping exponent

`p` changes only when SHAPE moves, so `t^p` must not be a per-sample `pow()` — at 8x stereo that is ~770k calls per second and would dominate the whole plugin. Build a **1024-point lookup table with linear interpolation**, rebuilt on parameter change. What remains per sample is a few multiplies, a table lookup and a clamp.

### 4.3 The filter

Cascaded **TPT state-variable** sections, Butterworth-aligned, one 1-pole section for odd orders. Not biquads: at 20 Hz with 8 poles, cascaded biquads are numerically fragile at 44.1 kHz, ring badly and dislike being modulated — and this knob is meant to be swept.

**FILTER POST changes what the knob means.** In PRE it is a frequency: which band of the sidechain drives the lid. In POST it is an envelope follower and the cutoff is effectively a release time (20 Hz ≈ 8 ms of smoothing; 20 kHz ≈ no smoothing, i.e. PRE mode with the filter off). The readout must relabel itself in POST mode — showing "20 Hz" while the user dials a release time is a lie the UI can cheaply avoid.

**Butterworth overshoots on a rectified signal.** The envelope will ring below zero, which would send the lid above 1 and hand a negative base to the exponent. Clamp `mag` to `>= 0`. This makes the values valid; it does not remove the ringing, which at high slopes remains audible as post-transient wobble. That is accepted as character.

### 4.4 Channel configurations

- mono → mono and stereo → stereo main buses; sidechain accepted as mono or stereo in both
- no mono → stereo (doubles the bus-layout matrix for something nobody asks for)
- on a mono instance, **grey out** MONO/STEREO rather than hiding it, so the panel does not reflow

### 4.5 Real-time safety

`ScopedNoDenormals` in `processBlock`. No allocation, no locks, no file or GUI access on the audio thread. Scope data crosses to the editor via a lock-free FIFO only. HQ is the obvious way this could be broken — both oversampler pairs are therefore allocated up front and the toggle only switches which pair is addressed.

---

## 5. Interface

Panel layout is defined in Figma:
https://www.figma.com/design/Hc5nzirm4UIGqWrgbs5Uy2/Miruu-Plugin-Collection?node-id=1-11

Controls: PRE slider, CEILING knob (large), FILTER knob, SHAPE knob, SLOPE field, FLOOR field, OUTPUT slider, and a toggle row under the scope — CLIP / FILTER POS / SC LINK / SC SOURCE.

### 5.1 Oscilloscope

| element | colour | meaning |
|---|---|---|
| sidechain | cyan line | filtered sidechain, post-filter, pre-rectifier — the signal the thresholds measure |
| ±CEILING | blue lines | symmetric, because the detector is rectified |
| ±FLOOR | red lines | symmetric; collapse onto the centre line when FLOOR is OFF |
| lid | grey aperture mask | fills everything *outside* ±lid, so the cap visibly closes in from top and bottom |
| output | white line | squashes flat against the mask as it descends |

The lid is drawn as a **closing aperture rather than a line** — it stops competing with the sidechain for line-reading attention, and makes "hard cap" literal. No input ghost trace: four elements is already a lot, and the undamaged parts of the output imply it.

Both traces share one normalised amplitude axis (±1.0 full scale), which is legitimate because carrier and sidechain are both full-scale audio.

**Triggering:** latch on the rising zero crossing of the filtered sidechain with a holdoff; free-run when it is silent.
**Timebase:** derive the period from the measured zero-crossing interval and clamp it, so the window auto-scales to show ~2 cycles regardless of the sub's pitch. Without this a 40 Hz sub and a 100 Hz sub look wildly different.
**Channel:** left only.
**Repaint:** 30 fps from a lock-free FIFO. The FIFO is still filled at base rate; 30 is plenty to read and costs half of 60, and this timer runs for as long as the editor is open whether or not the host is playing.

### 5.2 Interaction states

- dragging **SHAPE** → highlight the aperture mask, showing the ramp as it would be applied
- dragging **CEILING** or **FLOOR** → highlight the corresponding threshold lines
- **FILTER** knob greys out at the OFF detent
- **SLOPE** field is draggable and snaps to the eight values
- the dot beside CEILING is a **lid-activity LED**: brightness tracks instantaneous gain reduction
- mono instance → MONO/STEREO greyed to its inoperative state

---

## 6. Build and distribution

- **JUCE 9 + CMake**, VST3 (plus standalone for development)
- **MIT** for this repository. JUCE is used under the free **Starter** tier — its own modules remain under the JUCE licence, and MIT demands nothing of code it links against, so there is no conflict. Forkers need their own JUCE licence, which is normal.
  - GPLv3 was rejected: it conflicts with the proprietary Starter terms unless a linking exception is granted.
  - The VST3 SDK is MIT as of late 2025, so no Steinberg agreement is required.
- **CI** (GitHub Actions): Linux x64 and Windows x64 on every push, `pluginval` at strictness 10, plus an ASan/UBSan job. macOS universal runs on tags and manual dispatch only — it bills at 10x on a private repo and is the one platform testable locally for free.
- **No preset browser.** Host-saved state only, plus a handful of `.vstpreset` files in the repo as starting points. Ship one that turns CLIP on with PRE around +12 dB — that is where the signature sound lives.
- Figma SVGs exported to `Resources/` and loaded via `juce::Drawable`. JUCE 9's lunasvg-based parser handles the radial gradients, blend modes and clip paths this design uses; JUCE 8's would not have.

---

## 7. Deliberately not built

| | why | when to revisit |
|---|---|---|
| True bipolar ring-mod mode | different effect, needs a DEPTH control, no panel space | if the lid turns out to be tuneable enough that depth is the missing axis |
| CROSS sidechain routing (L↔R) | MONO+CROSS collapses to INT — a dead option in a four-way matrix | never, unless SC LINK is redesigned |
| Auto-makeup gain | fights the effect; the whole point is that the output ducks | never |
| Preset browser | real chunk of undesigned UI | if `.vstpreset` files prove insufficient |
| mono → stereo bus layout | doubles the matrix for nothing | if a user actually asks |

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
| FLOOR | float, dBFS | −60 … 0 | −60 | absolute; displays "INSTANT" at minimum |
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

- **CEILING and FLOOR are tapered to amplitude, not to dB.** Both are quoted in dB, but a dial that is linear in dB puts almost everything in the top of its sweep: on a −60 … 0 range, half amplitude (−6 dB) sits 90 % of the way round, and the bottom half of the travel is inaudible fractions. Because the scope plots linear amplitude, the dial and the band it controls would move at visibly different rates. The normalisable range therefore maps dial position to linear gain, so the pointer and the threshold track each other. −100 dB is the conversions' minus-infinity so the bottom detent round-trips instead of collapsing to zero.
- **FLOOR at minimum reads INSTANT, not OFF.** Nothing is switched off there — the lid starts moving the moment the sidechain does. Reading "OFF" next to a SHAPE dial invites the reading that the shaping is disabled.
- **FLOOR is absolute, not relative to CEILING** — sweeping CEILING does not drag FLOOR with it. FLOOR is internally clamped to `min(floor, ceiling - 1 dB)` so the window can never invert. The preview shows the effective window, so the clamp is visible rather than silent.
- **PRE and OUTPUT share a range** so the pair is not confusing to read.
- **CEILING turns conventionally**: clockwise = higher dBFS = gentler. Counter-clockwise lowers the ceiling.
- 20 kHz is the top of the FILTER sweep because Nyquist at 48 kHz is 24 kHz. Above ~20 kHz the filter does nothing, so the top of the sweep *is* the off position.
- **No parameter smoothing anywhere.** Not on the lid path, and — deliberately, unlike most plugins — not on PRE, OUTPUT, CEILING, FLOOR or SHAPE either. Every parameter is read once per block and applied as a step. The usual 10–20 ms ramp exists to hide zipper noise; here that noise is the point. A hard automation lane on PRE or CEILING should sound *yanked*, not eased, and the block-rate stepping is part of what makes a fast sweep sound like something breaking rather than something fading. Smoothing would sand off exactly the roughness this plugin exists to produce. The host's buffer size is the only knob that changes how coarse the steps are, and that is the user's call.

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

Panel layout is defined in Figma and the file is the source of truth:
https://www.figma.com/design/Hc5nzirm4UIGqWrgbs5Uy2/Miruu-Plugin-Collection?node-id=1-11

The canvas is **968 x 326** and the editor is laid out against those coordinates
exactly. It does not reflow: the whole editor carries an affine scale of 75 /
100 / 125 / 150 %, stored in the plug-in state. The switch lives in the settings
panel rather than on a background right-click, because a menu that only exists
where nothing is drawn is a menu nobody finds.

Typeface is Zalando Sans Expanded (OFL), embedded from `Resources/fonts/`. Figma
sizes are em sizes, so they are applied with `withPointHeight`, not `withHeight`.

Most of the design's text and hairlines use `mix-blend-mode: color-dodge`, which
has no JUCE equivalent. The palette in `PluginEditor.h` is therefore **sampled
from a 1:1 render of the frame** (`Resources/reference/figma-1-11.png`) rather
than copied from the layer list — #b1b1b1 dodged over the #101419 background is
#344151, and only the second number can be used directly.

Controls: PRE fader, CEILING dial (large), FILTER dial, SHAPE dial, SLOPE pill,
FLOOR pill, OUT fader, a CLIP toggle in the scope's lower-right corner, and a
gear in the upper-right that swaps the scope for a settings panel carrying
STEREO / HQ / FILTER PRE / SIGNAL EXT, and SCALE below them.

SCALE sits apart from the other four: it is a UI preference, not a plug-in
parameter, so it is deliberately absent from the host's automation list.

The settings panel is a **swap, not an overlay** — it takes the scope's exact
bounds, which is how Figma draws it (Oscilloscope variant "Variant3"). There is
no designed popup anywhere in the file.

### 5.1 Oscilloscope

| element | colour | meaning |
|---|---|---|
| sidechain | cyan line | filtered sidechain, post-filter, pre-rectifier — the signal the thresholds measure |
| ±CEILING | cyan gradient bands | fill from each edge inward to the threshold; the clamped region. Shown on demand — see 5.3 |
| ±FLOOR | red band | symmetric around the centre; collapses to nothing at INSTANT. Shown on demand — see 5.3 |
| lid | grey line pair | ±lid, the aperture the carrier is squashed against |
| output | white line | slams flat against the aperture as it closes |

Both traces share one normalised amplitude axis (±1.0 full scale), which is
legitimate because carrier and sidechain are both full-scale audio.

**Layer order** follows Figma and is not the obvious one: zero line, then the
floor band, then the traces, then the lid bands over the top. Drawing the lid
bands first instead loses the tint where a trace crosses into the clamped
region, which is the one place the overlay says something.

**Thresholds** are derived in the editor from the parameters, not mirrored out
of the engine. The engine only refreshes its copy inside `processBlock`, so in a
stopped host the bands would sit at their defaults until playback started.

**Triggering:** latch on the most recent rising crossing of the trace's own
mean. In PRE the detector is bipolar and the mean is ~0, i.e. a zero crossing; in
POST it is a rectified envelope that never goes negative, where a zero crossing
could only fire at the bottom of the clamp and the display would free-run.

**Timebase:** derive the period from the measured crossing interval and clamp it,
so the window auto-scales to ~2 cycles regardless of the sub's pitch. Without
this a 40 Hz sub and a 100 Hz sub look wildly different.

**Sampling:** two cycles of a 40 Hz sub is ~2400 samples across 380 pixels, so
each pixel column is drawn as the min/max of the samples inside it. A polyline
through every sixth sample misses the peaks, and which samples it lands on shifts
frame to frame, so the carrier appears to crawl.

Each extreme is placed at the x of the sample it came from, not at the column's
centre, and the whole trace is one continuous path. Snapping to columns turns
every diagonal into a staircase — which is most of what a slow sidechain is made
of — and separate subpaths get their own end caps, which doubles the line weight
wherever columns meet and leaves the trace visibly heavier in dense passages.

**Channel:** left only.
**Repaint:** 30 fps from a lock-free FIFO. The FIFO is still filled at base rate;
30 is plenty to read and costs half of 60, and this timer runs for as long as the
editor is open whether or not the host is playing.

### 5.2 Interaction states

Taken from the Figma component variants where they exist, and from the principle
that a control should explain itself where they do not.

- **Generic Interactable** — a pill gains a cyan border and a soft cyan glow while
  hovered or dragged. Applies to SLOPE, FLOOR, CLIP, the settings switches and SCALE.
- **CLIP** — engaged, it tints its own well red, borders in #e73131 and hangs a
  wide red glow. Off, it keeps a hairline border and drops its text to the
  section-caption tone rather than full white. **LQ** is dimmed the same way
  against **HQ**, so the pair reads as one switch rather than two labels.
- **FILTER** — the word under the dial reads "FILTER" at rest and swaps to a cyan
  PRE / POST while hovered. Clicking it flips the two.
- **FILTER and SHAPE captions** — while their dial is being dragged, the caption
  is replaced by the dial's current value in the readout colour. Those two dials
  are the only ones with no readout of their own, so without this they are the
  only controls you cannot see the value of while setting it.
- **FILTER dial** — its pointer goes flat grey at the OFF detent, and the SLOPE
  pill reads OFF with it. It is the only dial that does this.
- **SLOPE** — draggable, and clicking it opens the eight choices as a menu rather
  than stepping blindly through them. With the filter switched off there are no
  slopes to choose between, so dragging sweeps the **filter's cutoff** instead of
  doing nothing visible, and picking a slope from the menu brings the filter in
  at **160 Hz**. A control that is inert in a reachable state reads as broken.
- the dot beside CEILING is a **lid-activity LED**: brightness tracks
  instantaneous gain reduction.

### 5.3 What the scope shows, and when

The overlays are not decoration and are never all on at once. Each answers the
question the control being touched is asking, and hides whatever would compete
with the answer.

| state | shown |
|---|---|
| at rest | sidechain, lid and output |
| pointing at CEILING, FLOOR or the scope | ... plus the ceiling and floor bands |
| dragging CEILING or FLOOR | bands, sidechain and lid — **no output** |
| dragging SHAPE | the SHAPE curve alone; no audio, no bands |

Dropping the output while a threshold is moving is the point of the reference
state: setting a threshold is a question about where the *sidechain* sits against
the bands, and the output crosses the same ground answering a different one.

The SHAPE curve replaces the scope rather than overlaying it, because a transfer
curve and a waveform share an axis and mean different things by it. It is drawn
**mirrored about the centre line** for the same reason — the display it stands in
for is bipolar, and a one-sided curve in that frame reads as a signal sitting
off-centre rather than as an aperture.

The drawn floor is put through the engine's own `clampFloor`, so the band cannot
be shown above the ceiling when the audio would not allow it. The clamp constant
lives in `HardCapEngine.h` and is used by both; two copies would drift apart and
the symptom would be a band that does not sit where the floor is.

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
| Parameter smoothing | ramps sand off the roughness; step-per-block on PRE and CEILING is the character, not a defect | never |
| mono → stereo bus layout | doubles the matrix for nothing | if a user actually asks |

# SideCrush — Specification

**SideCrush by miruu** — a sidechain-driven dynamic hard clipper. VST3 and standalone, built on JUCE 9.

The idea came from the crunch that happens when a sub-bass and a distorted signal are summed into a clipper: the sub's peaks push the other signal into the clipping region, where its detail is flattened rather than turned down. SideCrush produces that on purpose, driving a clip ceiling — "the lid" — from a sidechain signal at waveform rate.

Nothing below assumes that use. The detector is a full-range signal path with a lowpass on it, and every requirement here is written against "the sidechain" and "the carrier" rather than against a source.

It is not a ring modulator: there is no bipolar multiply and no phase inversion.

---

## 1. Core algorithm

The sidechain does not drive a VCA. It drives a **clip ceiling that descends into the carrier**.

```
sc      = source select (EXT sidechain bus | INT main input)
sc      = STEREO ? per-channel : (L+R)/2      -- MONO and WTF both sum

               STEREO / MONO                 WTF, per channel (see 4.5)
FILTER PRE:    mag = |lowpass(sc)|            mag = max(0, ±lowpass(sc))
FILTER POST:   mag = max(0, lowpass(|sc|))    mag = max(0, lowpass(max(0, ±sc)))
                                              -- clamped, see 4.3

t       = clamp((mag - floor) / (ceiling - floor), 0, 1)
d       = t^p              p = clamp(2^(-4 * shape), 1/32, 32)
lid     = 1 - d

CLIP on:   out = clamp(carrier * pre, -lid, +lid)
CLIP off:  out = carrier * pre * lid

out     = mix * out + (1 - mix) * dry   -- dry delayed by the reported latency
out     = out * output
```

`floor` and `ceiling` are linear amplitudes converted from their dBFS parameters. The mapping is done in the **linear** domain with dB-labelled endpoints, which is what makes SHAPE 0 a true 1:1 curve.

### The two lid paths

CLIP selects which operation the lid drives.

- **Multiply (CLIP off) attenuates.** At the sidechain peak the output is silent and the carrier's detail is intact, scaled to nothing.
- **Clamp (CLIP on) flattens.** The output is at full level and the carrier's detail is gone.

The two agree at exactly two points — fully open (gain 1) and fully shut (gain 0) — and differ across the entire descent, because **the clamp reads the carrier and the multiply does not**. No curve maps one onto the other.

The clamp path additionally:

- generates harmonic distortion of the carrier, not only translated sidebands
- depends on the carrier's own level, so quiet passages survive a sidechain peak that flattens loud ones
- produces peaks that are walls rather than holes

PRE drives the carrier into the lid and determines how much of that descent is reached.

### Detector

- **Rectified**, so a sine sidechain produces two lid closures per cycle, matching a clipper, which clips both peaks.
- **No smoothing after the rectifier.** No attack, no release, no envelope follower, no parameter ramp on the lid. The pre-rectifier lowpass is the only smoothing in the detector; smoothing after it would make this an envelope-driven compressor rather than a waveform-rate clipper.
- Once `mag >= ceiling` the lid is 0 and the carrier is clamped to silence regardless of its own level.

### SHAPE

Bipolar. Sets **where along the sidechain's travel the lid breaks**, not what the break does.

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
| OUTPUT | float, dB | −36 … +36 | 0 | same range as PRE |
| MIX | float, % | 0 … 100 | 100 | parallel blend; the dry side is latency-compensated |
| CLIP | bool | off / on | **on** | on = clip ceiling, off = VCA multiply |
| FILTER POS | choice | PRE / POST | PRE | rectifier order |
| SC LINK | choice | STEREO / MONO / WTF | STEREO | sidechain detection only; output is always stereo (see 4.5) |
| WTF | float, % | 0 … 100 | **50** | how far WTF is taken; inert in the other two link modes (see 4.5) |
| SC SOURCE | choice | EXT / INT | EXT | INT = main input drives its own lid |
| QUALITY | choice | HQ / LQ / YUCK | **HQ** | 8x linear phase / 4x minimum phase / 1x unfiltered (see 4.1) |

Notes:

- **CEILING and FLOOR are tapered to amplitude, not to dB.** Both are quoted in dB, but a range that is linear in dB puts almost everything in the top of the sweep: on −60 … 0, half amplitude (−6 dB) sits 90 % of the way round. The scope plots linear amplitude, so the normalisable range maps dial position to linear gain and the pointer and the threshold it controls move at the same rate. −100 dB is the conversions' minus-infinity, so the bottom detent round-trips instead of collapsing to zero.
- **FLOOR at minimum reads INSTANT, not OFF.** Nothing is switched off there: the lid starts moving the moment the sidechain does.
- **FLOOR is absolute, not relative to CEILING.** Sweeping CEILING does not drag FLOOR with it. FLOOR is internally clamped to `min(floor, ceiling - 0.1 dB)` so the window cannot invert — one step of FLOOR's own 0.1 dB grid, which is as narrow as the two controls can express. The clamp is visible rather than silent: the scope shows the effective window, and the readout brackets the value it is being held at (`- -6.1 dB -`) instead of reporting a number the audio is not using. That text comes from the parameter's own string function, so the host's automation lane shows it too.
- **WTF rests at 50 %.** Both of the things the dial moves are inactive there, which is what makes 50 % the reference behaviour. It remains a real, automatable parameter in the other two link modes despite doing nothing in them; a parameter that disappears from the host's list when a switch moves is harder to automate than one that is temporarily inert.
- **PRE and OUTPUT share a range**, so the pair reads symmetrically.
- **MIX is fully wet by default.** Two consequences follow: below 100 % the ceiling is no longer hard, because the dry half is unlimited by definition; and the dry half is not ducked during a QUALITY switch, because the duck exists to hide a splice the dry path does not have.
- **OUTPUT is last in the chain, after MIX**, so lowering MIX cannot make the plugin louder by however much OUTPUT is cutting.
- **CEILING turns conventionally**: clockwise = higher dBFS = gentler.
- **20 kHz is the top of the FILTER sweep** because Nyquist at 48 kHz is 24 kHz; above ~20 kHz the filter does nothing, so the top of the sweep is the off position.
- **No parameter smoothing anywhere.** Not on the lid path, and not on PRE, OUTPUT, MIX, CEILING, FLOOR or SHAPE. Every parameter is read once per block and applied as a step, so a fast automation lane steps rather than glides. The host's buffer size is the only thing that changes how coarse those steps are.

---

## 3. Signal flow

```
                        ┌── 8x oversampled; 4x on LQ, 1x on YUCK ──────┐
                        │                                              │
 main in ──> PRE ───────┼──────────────────────────┐                   │
                        │                          │                   │
 sc source ──> link ────┼──> filter ──> rectify ──> t ──> d = t^p ──> lid
   EXT | INT  ST|MONO|  │   (PRE/POST order)                 │         │
              WTF       │                                    │         │
                        │                                    v         │
                        │                       CLIP ? clamp(x, ±lid)  │
                        │                            : x * lid         │
                        └──────────────────┬───────────────────────────┘
                                           v
 main in ──> delay(latency) ─────────────> MIX ──> OUTPUT ──> main out
```

Sidechain source and link happen at base rate. Everything from the filter onward runs oversampled — 8x on HQ, 4x on LQ, and not at all on YUCK.

---

## 4. Implementation

### 4.1 Oversampling

**8x on HQ, 4x on LQ, 1x on YUCK**, via `juce::dsp::Oversampling`, with latency reported to the host.

There are four independent alias sources: the rectifier's corner at every zero crossing, the `t^p` exponent, the moving hard clamp, and the multiply itself — `carrier * lid` is bandwidth-expanding even in VCA mode, since `lid` carries harmonics up to Nyquist and the products fold. The moving hard clip is the worst of them; dedicated clippers routinely run 16–32x.

**The carrier and the detector both run at the full rate.** Alias floor against the fundamental, from `tests/alias.cpp`:

| path | 1x | 2x | 4x | 8x |
|---|---|---|---|---|
| clipper | −32 dB | −48 dB | −60 dB | −69 dB |

Roughly 9 dB per halving.

- **The lid is computed at the rate it is used at.** Running the detector slower and interpolating the lid up to the carrier's rate puts the interpolation's images on the decimator's fold points: a 2x detector with an interpolated lid measures −53 dB against 8x's −85 dB.
- **The rectifier does not move to base rate.** Filtering at base rate and upsampling afterward is valid in principle, since the filter is linear and generates no aliasing, but the engine loop is roughly an eighth of the plugin's cost and the two oversamplers are the rest, so a second code path buys almost nothing.

The routing does skip arithmetic it has already done. With INT + STEREO the detector is the main input, so the carrier's upsampled block is reused directly; any routing that puts the same signal on every detector channel upsamples one channel instead of two. Both shortcuts are exact and are checked bit-for-bit in `tests/host_test.cpp`.

#### QUALITY

Three modes, measured at 48 kHz in 512-sample blocks:

| mode | factor | filter | alias floor | CPU |
|---|---|---|---|---|
| HQ | 8x | linear phase FIR | −69 dB | 3.5 % |
| LQ | 4x | minimum phase IIR | −60 dB | 1.3 % |
| YUCK | 1x | none | −32 dB | 0.3 % |

LQ trades 9 dB of alias floor and linear phase for a third of the CPU. **YUCK is not an eco mode.** It is 1x — `juce::dsp::Oversampling` built with factor 0, a pass-through stage — so there is no anti-imaging filter of any kind and neither phase response applies. The aliasing is the feature: the moving hard clip folds its own harmonics back down the spectrum, and that is what the mode sounds like.

Two constraints apply to all three:

- **Latency does not move.** The shorter paths are padded back out to the 8x figure, so `setLatencySamples` reports the same number in every mode and no host is asked to renegotiate delay compensation mid-session. YUCK, having no latency of its own, is padded by the whole of it.
- **The swap does not click.** No two of the cascades line up, so the seam steps regardless of how the incoming one is primed. The output is ducked over ~4 ms and held silent until the padding delay has flushed; unducked, the seam steps 22x the steady-state sample delta.

All three oversampler pairs are built in `prepareToPlay`, so changing QUALITY never allocates on the audio thread.

### 4.2 The shaping exponent

`p` changes only when SHAPE moves, so `t^p` is a **1024-point lookup table with linear interpolation**, rebuilt on parameter change. A per-sample `pow()` would be ~770k calls per second at 8x stereo and would dominate the plugin. What remains per sample is a few multiplies, a table lookup and a clamp.

### 4.3 The filter

Cascaded **TPT state-variable** sections, Butterworth-aligned, with one 1-pole section for odd orders. Not biquads: at 20 Hz with 8 poles, cascaded biquads are numerically fragile at 44.1 kHz, ring badly and dislike being modulated, and this control is meant to be swept.

**FILTER POST changes what the control means.** In PRE it is a frequency — which band of the sidechain drives the lid. In POST it is an envelope follower and the cutoff is effectively a release time: 20 Hz ≈ 8 ms of smoothing, 20 kHz ≈ no smoothing. The readout relabels itself in POST to show milliseconds.

**`mag` is clamped to `>= 0`.** Butterworth overshoots on a rectified signal, so the envelope rings below zero, which would otherwise send the lid above 1 and hand a negative base to the exponent. The clamp makes the values valid; it does not remove the ringing, which at high slopes remains audible as post-transient wobble.

### 4.4 Channel configurations

- mono → mono and stereo → stereo main buses; sidechain accepted as mono or stereo in both
- no mono → stereo
- on a mono instance the SC LINK selector is **greyed out rather than hidden**, so the panel does not reflow

### 4.5 The WTF link

The third position on SC LINK, not a control of its own: it is one more way for the detector's two channels to relate to each other.

It **sums** the sidechain the way MONO does, then splits the sum by sign and gives one half to each channel — the positive excursions close the left lid, the negative ones close the right. Fed a low sine the two lids alternate at the sidechain's own rate and the carrier appears to pan, a panning produced by two clippers taking turns rather than by any gain law.

Two requirements follow from where the split sits:

- **The rectifier is half-wave, not full-wave.** A full-wave rectifier discards the sign the split is made of. Flipping the right channel's copy of the sum turns "the negative half" into "the positive half", so one half-wave rectifier serves both sides.
- **In POST the split happens before the filter.** POST rectifies first and filters the result, so splitting afterwards would hand both channels the same envelope and leave no pan. Splitting first gives the filter two half-wave signals and it returns two envelopes, one per side. In PRE the filter is linear and runs on the still-bipolar sum, so the sign survives it and the split happens after, which is what keeps PRE at waveform rate.

#### The WTF dial

How far the mode is taken. It moves **three** things across two halves of travel, all three at rest at 50 %:

| intensity | detector | mid | invented side |
|---|---|---|---|
| **0 %** | no split — both channels see the whole rectified sum | untouched | ×1 |
| **50 %** | the full split described above | untouched | ×1 |
| **100 %** | the full split | replaced by what MONO would have produced | **×2** |

- **0 % is MONO.** With both lids driven by the same detector the two channels agree, which is what the MONO link does. The dial therefore spans MONO → WTF → the inverted mode.
- **100 % makes the effect survive a mono speaker.** Below it, WTF *half-cancels* when summed: at any instant one channel is clipped and the other is not, so the sum keeps half of what the lid removed. At 100 % the pair is slid until its mid is the mono result, so a mono listener hears **exactly** the MONO link while the difference between the channels — the whole of the effect — stays in the sides.

The mechanism is phase cancellation, not clipping. The part of the carrier the lid removes is handed to *both* channels with opposite signs, so it is still present in stereo, wide and on the wrong side of the lid, and annihilates on the way to mono. The clipping a mono listener hears is that annihilation, not either channel's own output.

#### The width on the top half

The same stretch of travel that replaces the mid also **widens the side the effect invented**, up to ×2. This is a width control in the mid/side sense and is mono-blind for the same reason the mode is: what it scales is added to one channel and subtracted from the other, so the sum never sees it.

**Only the invented side is scaled.** The pair's own stereo image is already present in the mono-processed pair the mid is built from; that side is subtracted out before the scaling and added back after, so material the lid is not touching passes through untouched. `tests/engine_test.cpp` covers this.

The travel stops at ×2, which is where the invented side starts overtaking the carrier and the output peak begins to climb with the gain. Below that point the peak does not move at all. Measured figures are in [Appendix A](#appendix-a--wtf-derivations).

**The width is decorrelation, not panning, and cannot be anything else.** Exact mono cancellation means `out_L = M + d` and `out_R = M − d`, so `|out_L| = |out_R|` wherever `M = 0` — which is exactly when the lid is shut, the moment the effect peaks. The interchannel *level* difference is structurally zero there whatever `d` is, and the correlation depends only on `⟨d²⟩/⟨M²⟩`. Scaling `d` is the only lever available.

Measured across the dial, mono carrier:

| intensity | correlation | short-term ILD | side/mid |
|---|---|---|---|
| 0 % | +1.00 | 0.0 dB | 0.00 |
| 50 % | +0.28 | **53.2 dB** | 0.75 |
| 75 % | −0.41 | 4.4 dB | 1.55 |
| 100 % | **−0.74** | 0.3 dB | **2.58** |

50 % is therefore a wider *position* than 100 %, and 100 % a wider *image*: at 50 % the lid closing to zero silences one channel outright while the other runs full, a 53 dB alternating level difference over 5 ms windows; at 100 % the same instant is `±d`, equal levels and opposite polarity, 0.3 dB. The dial trades one kind of width for the other rather than morphing between them.

#### Consequences

- **Above 50 % neither channel is clamped to the lid.** One of them is pushed past it by the half of the excess it was handed. Two signals both clamped at the lid sum to something also clamped at the lid, so a per-channel ceiling and an exact mono sum cannot both hold: the ceiling becomes a mono-sum ceiling.
- **The scope's aperture stops bounding the output trace** above 50 %, which everywhere else it does. The trace escaping the mask is the display reporting that this is no longer clipping — see 5.1.
- **Peak headroom is guaranteed in PRE and not in POST.** In PRE the growth factor is identically 1 at every sample, for any material at any setting. In POST it reaches 2 (+6 dB). The mono sum is bounded by MONO's own ceiling either way. Derivation and figures in [Appendix A](#appendix-a--wtf-derivations).
- **A mono instance stays at the 50 % behaviour** whatever the dial says: only the positive half is left, and with no second channel there is no mid to cancel against.

**The mono detector costs no third filter.** The sections are linear, so the two half-wave envelopes sum to the envelope of the whole and the mid's detector falls out of the two the split already computes. In POST that sum is taken **before** the `>= 0` clamp of 4.3, not after — clamping the halves and adding them is not clamping the sum, and the two disagree wherever the Butterworth ringing puts one half below zero while the other is above.

#### On the scope

The scope draws the bipolar detector in PRE, so in WTF its top lobe is the left channel's lid and its bottom lobe the right's, and the display follows that through:

- **The aperture splits.** The mask closing from the top is the left lid, the one closing from the bottom is the right. In every other mode the two lids are the same signal and the aperture is symmetric; here it alternates, top then bottom, at the sidechain's own rate.
- **The output is drawn once per channel.** The right sits at 30 % throughout. The left carries the reading: full strength wherever the two outputs are the same signal — as bright as the single line every other mode draws, with the right hidden underneath — fading to the same 30 % as the two come apart. The ramp is linear over about two pixels of separation at the display's own scale.

  The alpha is a horizontal `ColourGradient` on the stroke with **a stop per pixel column**, taking the widest gap between the two channels inside that column. The gap turns over at the carrier's rate rather than the sidechain's, so coarser stops smear every bright stretch away and leave the whole trace at 30 %.

### 4.6 Real-time safety

`ScopedNoDenormals` in `processBlock`. No allocation, no locks, no file or GUI access on the audio thread. Scope data crosses to the editor via a lock-free FIFO only. All three oversampler pairs are allocated up front, so the QUALITY switch only changes which pair is addressed.

---

## 5. Interface

Panel layout is defined in Figma, and the file is the source of truth:
https://www.figma.com/design/Hc5nzirm4UIGqWrgbs5Uy2/Miruu-Plugin-Collection?node-id=1-11

The canvas is **968 x 326** and the editor is laid out against those coordinates
exactly. It does not reflow: the whole editor carries an affine scale of 75 /
100 / 125 / 150 %, stored in the plug-in state. The switch lives in the settings
panel rather than on a background right-click, so that it is reachable without
knowing it is there.

Typeface is Zalando Sans Expanded (OFL), embedded from `Resources/fonts/`. Figma
sizes are em sizes, so they are applied with `withPointHeight`, not `withHeight`.

Most of the design's text and hairlines use `mix-blend-mode: color-dodge`, which
has no JUCE equivalent. The palette in `PluginEditor.h` is therefore **sampled
from a 1:1 render of the frame** (`Resources/reference/figma-1-11.png`) rather
than read from the layer list: #b1b1b1 dodged over the #101419 background is
#344151, and only the second number can be used directly.

Controls: PRE fader, CEILING dial (large) with the activity LED above it, FILTER
dial, SHAPE dial, SLOPE pill, FLOOR pill, MIX fader, OUT fader, a CLIP toggle in
the scope's lower-right corner, and a gear in the upper-right that swaps the
scope for a settings panel carrying STEREO / HQ / FILTER PRE / SIGNAL EXT, and
SCALE below them — with WTF beside SCALE on that last row, where it is present
**only** while the link is on WTF. It is hidden rather than dimmed; SCALE
re-centres when it goes and the two routing rows above never reflow, which is
what 4.4's greying protects. It sits with SCALE rather than with the link it
belongs to because it is an amount and everything on the rows above it is a
switch.

The first two settings pills are three-way: STEREO cycles STEREO / MONO / WTF,
HQ cycles HQ / LQ / YUCK. The link cycles from its default into the ordinary
alternative first — one click from STEREO is MONO, and WTF is past it.

**Right-clicking any pill backed by a list of choices opens that list as a
menu**, so a three-way switch does not have to be cycled to find out what it can
do; left-click cycling remains, as the faster gesture once the options are known.
A pill backed by a *number* has no such list, so WTF's right-click names the
three settings worth naming — 0, 50 and 100 % — and dragging it covers
everything between.

SCALE is a UI preference rather than a plug-in parameter, so it is absent from
the host's automation list.

The settings panel is a **swap, not an overlay**: it takes the scope's exact
bounds (Oscilloscope variant "Variant3").

### 5.1 Oscilloscope

| element | colour | meaning |
|---|---|---|
| sidechain | cyan line, or a cyan body | the detector — the signal the thresholds measure. Bipolar in PRE; in POST the envelope and its mirror image |
| lid | white mask at 8% | fills everything *outside* ±lid, so the cap visibly closes in from top and bottom |
| output | white line | slams flat against the aperture as it closes — except above WTF 50 %, where it is meant to escape it (4.5) |
| **in WTF** | one output per channel, split aperture | see 4.5 |
| ±CEILING | cyan gradient bands | from each edge inward to the threshold; the clamped region |
| ±FLOOR | red band | symmetric around the centre; collapses to nothing at INSTANT |

Which of those are drawn depends on what is being touched — see 5.3. There is no
zero line: the sidechain is symmetric about it and says where it is by itself.

Both traces share one normalised amplitude axis (±1.0 full scale), which is
legitimate because carrier and sidechain are both full-scale audio.

**The sidechain is drawn only where it is doing something.** Its cyan runs
through a vertical gradient whose stops sit on the thresholds: solid outside
±CEILING, fading through the window, and fully transparent inside ±FLOOR. The
trace reports its own relevance without the bands being drawn, which is what lets
the resting state carry no overlay at all. At INSTANT the floor stops collapse
together and the fade runs to the zero crossing.

This and the greyed outline in 5.3 are both a `ColourGradient` plus a
`strokePath` — JUCE fills a stroke from a gradient as readily as it fills a
shape.

**POST is drawn mirrored.** The detector in POST is a rectified envelope, so
plotted literally it is a half-wave sitting on the centre line and reads as a
broken trace rather than as the signal a symmetric pair of thresholds is
measuring. It is drawn together with its reflection about the centre, which
brackets the centre the way the lid does. The data is untouched — the reflection
is the same sample at the same x, so the envelope still crosses CEILING exactly
where the lid closes. Redrawing the raw pre-rectifier sidechain there instead
would drift out of step, since the filter is after the rectifier in POST and the
wave is no longer the signal driving the lid.

**Layer order:** traces first, then the ceiling bands, then the floor band over
the top. Bands underneath lose the tint where a trace crosses into the clamped
region, which is the one place the overlay is informative.

**Thresholds** are derived in the editor from the parameters, not mirrored out of
the engine, which only refreshes its copy inside `processBlock` — in a stopped
host the bands would otherwise sit at their defaults until playback started.

**Triggering:** latch on the most recent rising crossing of the trace's own mean.
In PRE the detector is bipolar and the mean is ~0, i.e. a zero crossing; in POST
it is a rectified envelope that never goes negative, where a zero crossing could
only fire at the bottom of the clamp and the display would free-run.

**Timebase:** the period is derived from the measured crossing interval and
clamped, so the window auto-scales to ~2 cycles regardless of the sidechain's
pitch and a 40 Hz and a 100 Hz sidechain read the same.

**Sampling:** two cycles of a 40 Hz sidechain is ~2400 samples across 380 pixels,
so each pixel column is drawn as the min/max of the samples inside it. A polyline
through every nth sample misses the peaks and lands on different samples each
frame, which makes the carrier appear to crawl.

Each extreme is placed at the x of the sample it came from, not at the column's
centre, and the whole trace is one continuous path. Snapping to columns turns
every diagonal into a staircase, and separate subpaths get their own end caps,
which doubles the line weight wherever columns meet.

**Channel:** left only.
**Repaint:** 30 fps from a lock-free FIFO. The FIFO is filled at base rate. The
timer runs for as long as the editor is open, whether or not the host is playing.

### 5.2 Interaction states

- **Generic Interactable** — a pill gains a cyan border and a soft cyan glow while
  hovered or dragged. Applies to SLOPE, FLOOR, CLIP, the settings switches and SCALE.
- **CLIP** — engaged, it tints its own well red, borders in #e73131 and hangs a
  wide red glow. Off, it keeps a hairline border and drops its text to the
  section-caption tone rather than full white. **LQ** is dimmed the same way
  against **HQ**, so the set reads as one switch rather than three labels.
  **YUCK** is #8d5c3d, the only warm colour on the panel, light enough to read as
  a warning rather than as another dimmed label.
- **FILTER** — the word under the dial reads "FILTER" at rest and swaps to a cyan
  PRE / POST while hovered. Clicking it flips the two.
- **FILTER and SHAPE captions** — while their dial is being dragged, the caption
  is replaced by the dial's current value in the readout colour. Those two dials
  have no readout of their own.
- **FILTER dial** — its pointer goes flat grey at the OFF detent, and the SLOPE
  pill reads OFF with it. It is the only dial that does this.
- **SLOPE** — draggable, and clicking it opens the eight choices as a menu rather
  than stepping through them. With the filter switched off there are no slopes to
  choose between, so dragging sweeps the **filter's cutoff** instead, and picking
  a slope from the menu brings the filter in at **160 Hz**.
- **The scope is a CEILING control.** Drag anywhere in it and the threshold, the
  bands and the dial all follow. CEILING is tapered to amplitude and the display's
  vertical axis *is* amplitude, so one pixel of drag is one pixel of threshold and
  there is no sensitivity constant to choose.
- **The dot beside CEILING is a lid-activity LED**: brightness tracks
  instantaneous gain reduction.

### 5.3 What the scope shows, and when

The overlays are never all on at once. Each answers the question the control
being touched is asking, and hides whatever would compete with the answer.

| state | shown |
|---|---|
| at rest | the lid aperture, the sidechain as a line, the output |
| **dragging** CEILING, FLOOR, or the scope itself | the sidechain as a filled body, the ceiling and floor bands, the output at 10% — **no lid** |
| dragging SHAPE | the SHAPE curve alone; no audio, no bands, no CLIP |

**Only a gesture that changes a threshold changes the picture.** Hover does not
raise the bands: the resting state already carries the thresholds in the
sidechain's own fade, and raising them on hover makes the display flash whenever
the pointer crosses a dial on its way somewhere else.

A drag on CEILING and a drag on FLOOR show the same thing, because that state
answers both questions at once. The sidechain fills out into a solid body against
the bands, the output drops to a 10% ghost, and the lid aperture goes entirely —
the question being asked is about the sidechain's level, and the aperture is
about what happened to the carrier.

The sidechain's outline also **greys out where it passes inside the floor band**,
where the lid is wide open and the level is doing nothing. That is one more
`ColourGradient` on the stroke, with its stops on ±FLOOR.

The SHAPE curve replaces the scope rather than overlaying it, because a transfer
curve and a waveform share an axis and mean different things by it. Only the
wordmark and the gear carry over; CLIP goes with the rest, belonging to the
scope's frame rather than to the curve standing in for it.

It is drawn on a **centred square**. SHAPE 0 is then a true 45 degrees, and the
two halves of the control read as the reflections they are: the exponent is
`2^(-4*shape)`, so +s and −s give `t^p` against `t^(1/p)`, exact mirror images
about that diagonal. On the scope's own 380×230 frame neither would hold.

The values come from an instance of the engine's own `ShapeTable`, so the
exponent, its [1/32, 32] clamp and the table's quantisation are the ones the
audio uses.

**The curve is a guide, not a transfer plot.** The engine's shaping is `1 - t^p`,
which has a single knee. What is drawn is that half-curve plus its own 180°
rotation about the centre of the square, so it has two: SHAPE −1 reads as an S
that eases in and out, SHAPE +1 as a squared-off step that slams and holds, which
is what those settings sound like.

The doubling cannot misreport anything that matters. Both halves meet exactly at
(0.5, 0.5); the curve still leaves (0,0) and arrives at (1,1); it stays monotonic
throughout; and at SHAPE 0 the exponent is 1, both halves collapse onto the same
straight line, and the diagonal is exact. Only the curvature between the
endpoints is stylised.

The drawn floor is put through the engine's own `clampFloor`, so the band cannot
be shown above the ceiling when the audio would not allow it. That constant lives
in `SideCrushEngine.h` and is used by both.

---

## 6. Build and distribution

- **JUCE 9 + CMake.** VST3 plus a standalone build.
- **macOS builds universal** (arm64 + x86-64), deployment target 10.15. Ableton Live's plug-in scanner runs as x86-64 even on Apple Silicon and rejects an arm64-only bundle outright, so the slice has to be there.
- **MIT** for this repository. JUCE is used under the free **Starter** tier: its own modules remain under the JUCE licence, and MIT demands nothing of code it links against, so there is no conflict. Forkers need their own JUCE licence. GPLv3 is not an option here — it conflicts with the proprietary Starter terms unless a linking exception is granted. The VST3 SDK is MIT as of late 2025, so no Steinberg agreement is required.
- **CI** (GitHub Actions): Linux x64 and Windows x64 on every push, `pluginval` at strictness 10, plus an ASan/UBSan job. macOS universal runs on tags and manual dispatch only, since macOS runners bill at 10x on a private repo and it is the one platform testable locally.
- **State is host-saved only.** No presets ship and there is no preset browser.
- **The plugin is unsigned and unnotarised**, so a macOS user clears the quarantine attribute by hand once.
- Figma SVGs are exported to `Resources/` and loaded via `juce::Drawable`. JUCE 9's lunasvg-based parser handles the radial gradients, blend modes and clip paths this design uses.

---

## 7. Scope of this specification

Everything specified above is built. The following are outside it.

| axis | why it is out | when it would be revisited |
|---|---|---|
| True bipolar ring-mod mode | a different effect, and it needs a DEPTH control there is no panel space for | if the lid turns out to be tuneable enough that depth is the genuinely missing axis |
| Preset browser | a real chunk of undesigned UI, and the control set is small enough to dial in from scratch | if `.vstpreset` files turn out to be worth shipping at all |
| mono → stereo bus layout | doubles the layout matrix | if a user asks |
| AU, AAX, CLAP | VST3 covers the target hosts, and each extra format is its own validation surface | on demand, AU first |

Three exclusions are structural rather than deferred:

- **No parameter smoothing.** The step-per-block behaviour on PRE and CEILING is specified in 2.
- **No auto-makeup gain.** It works against the effect, whose output is meant to duck.
- **No CROSS sidechain routing (L↔R).** MONO+CROSS collapses to INT, making it a dead option in a four-way matrix.

---

## Appendix A — WTF derivations

Supporting material for 4.5. Nothing here changes the specified behaviour.

### A.1 Why the width stops at ×2

×2 is where an Utility's own width control stops, and by measurement it is also
where the invented side starts overtaking the carrier. Measured **in PRE**, on a
0.9 carrier with the ceiling at −9 dB:

| side gain | interchannel correlation | side/mid | peak |
|---|---|---|---|
| ×1 | −0.25 | 1.29 | 0.90 |
| ×1.5 | −0.58 | 1.93 | 0.90 |
| **×2** | **−0.74** | **2.58** | **0.90** |
| ×3 | −0.88 | 3.87 | 1.35 |

Below ×2 the invented side is smaller than the carrier and the peak does not move
at all; above it the side takes over and the peak climbs with the gain.

### A.2 Peak headroom

Write `lL`, `lR` for the two split lids and `lM` for the mono one. At 100 % the
pair collapses to

```
out_L = d_L·lL + d_R·(lM − lR)
out_R = d_R·lR + d_L·(lM − lL)
```

so the most the peak can grow is `max(lL + |lR − lM|, lR + |lL − lM|)`.

**In PRE that factor is identically 1**, structurally and at every sample. The
split there is exact: one of the two halves is always zero, so one lid is wide
open at 1 and the other *is* `lM`. Substituting gives `lM + (1 − lM)` and `1 + 0`.
The coefficients are a convex combination of the two carriers, so the output peak
cannot exceed the input peak for any material at any setting. A.1's table is that
result, measured.

**In POST nothing ties the three lids together**, and the factor drifts off 1 in
both directions:

- the two half-wave envelopes overlap once smoothed, so `lM`'s detector — their
  sum — exceeds both halves and `lM` closes further than either lid;
- the Butterworth ringing of 4.3 can put one half-envelope below zero, so the sum
  falls *under* the other half and `lM` sits **above** a channel lid instead.

The factor tops out at 2, i.e. +6 dB, and reaches it in practice. Same carrier,
decorrelated:

| detector | max factor | output peak (input 0.900) |
|---|---|---|
| PRE, any setting | **1.000** | 0.900 |
| POST 400 Hz 2-pole, INSTANT floor | 1.032 | 0.924 |
| POST 200 Hz 4-pole, floor −12 dB | 1.404 | 1.226 |
| POST 20 Hz 8-pole, narrow window | 2.000 | 1.793 |

The POST drift is accepted rather than corrected. The guarantee the mode makes
still holds there — the mono sum is exactly MONO's output and is still bounded by
MONO's own ceiling — and it is the per-channel peak that moves, which is what the
top half of the dial has already given up (4.5, Consequences). Capping the width
per sample would put a divide in the 8x oversampled inner loop and make the width
modulate with the sidechain. `tests/engine_test.cpp` pins the PRE invariant at
exactly 1.

### A.3 Why the width is decorrelation and not a level difference

Exact mono cancellation means `out_L = M + d` and `out_R = M − d`. Two things
follow from that form alone:

- **`|out_L| = |out_R|` wherever `M = 0`.** `M` is the mono result, zero exactly
  when the lid is shut — the moments the effect peaks. The interchannel level
  difference is structurally zero there, whatever `d` is.
- **The correlation depends only on `⟨d²⟩/⟨M²⟩`.** The shape, spectrum and phase
  of `d` are invisible to it; decorrelating `d` with allpasses moves the measured
  correlation by nothing. Only its level does anything.

Scaling `d` is therefore the only lever the arithmetic leaves, which is why the
width rides the same half of the travel as the mid replacement: it is the
compensation for the stereo position that replacement costs, not a separate idea.

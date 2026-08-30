# HardCap — Specification

**HardCap by miruu** — a sidechain-driven dynamic hard clipper.

Recreates the crunch that happens when a sub-bass and a distorted signal are summed into a clipper: the sub's peaks push the other signal into the clipping region, where its detail is flattened rather than turned down. HardCap does this deliberately and controllably, driving a clip ceiling ("the lid") from a sidechain signal at waveform rate.

Not a ring modulator, despite the original framing. There is no bipolar multiply and no phase inversion.

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
| MIX | float, % | 0 … 100 | 100 | parallel blend; the dry side is latency-compensated |
| CLIP | bool | off / on | **on** | on = clip ceiling, off = VCA multiply |
| FILTER POS | choice | PRE / POST | PRE | rectifier order |
| SC LINK | choice | STEREO / MONO / WTF | STEREO | sidechain detection only; output is always stereo (see 4.5) |
| WTF | float, % | 0 … 100 | **50** | how far WTF is taken; inert in the other two link modes (see 4.5) |
| SC SOURCE | choice | EXT / INT | EXT | INT = main input drives its own lid |
| QUALITY | choice | HQ / LQ / YUCK | **HQ** | 8x linear phase / 4x minimum phase / 1x unfiltered (see 4.1) |

Notes:

- **CEILING and FLOOR are tapered to amplitude, not to dB.** Both are quoted in dB, but a dial that is linear in dB puts almost everything in the top of its sweep: on a −60 … 0 range, half amplitude (−6 dB) sits 90 % of the way round, and the bottom half of the travel is inaudible fractions. Because the scope plots linear amplitude, the dial and the band it controls would move at visibly different rates. The normalisable range therefore maps dial position to linear gain, so the pointer and the threshold track each other. −100 dB is the conversions' minus-infinity so the bottom detent round-trips instead of collapsing to zero.
- **FLOOR at minimum reads INSTANT, not OFF.** Nothing is switched off there — the lid starts moving the moment the sidechain does. Reading "OFF" next to a SHAPE dial invites the reading that the shaping is disabled.
- **FLOOR is absolute, not relative to CEILING** — sweeping CEILING does not drag FLOOR with it. FLOOR is internally clamped to `min(floor, ceiling - 0.1 dB)` so the window can never invert: one step of FLOOR's own 0.1 dB grid, which is as narrow as the two controls can express and narrow enough that the pair reads as a single threshold. The preview shows the effective window, so the clamp is visible rather than silent — and so does the readout, which brackets the value it is being held at (`- -6.1 dB -`) rather than reporting a number the audio is not using. That text comes from the parameter's own string function, so the host's automation lane says it too.
- **WTF defaults to 50 %, which is the behaviour that existed before the dial did.** Both of the things it moves are at rest there, so the parameter appearing does not shift any session that predates it. It stays a real, automatable parameter in the other two link modes even though it does nothing in them — a parameter that disappears from the host's list when a switch moves is worse to automate than one that is temporarily inert.
- **PRE and OUTPUT share a range** so the pair is not confusing to read.
- **MIX is fully wet by default.** This is a limiter first and a parallel one only if asked. Two consequences are deliberate: below 100 % the ceiling is no longer hard, because the dry half is by definition unlimited; and the dry half is *not* ducked during a QUALITY switch, because the duck exists to hide a splice the dry path does not have.
- **OUTPUT is the last thing in the chain, after MIX.** Trimming the wet half alone would mean pulling MIX down made the plugin louder by however much OUTPUT was cutting.
- **CEILING turns conventionally**: clockwise = higher dBFS = gentler. Counter-clockwise lowers the ceiling.
- 20 kHz is the top of the FILTER sweep because Nyquist at 48 kHz is 24 kHz. Above ~20 kHz the filter does nothing, so the top of the sweep *is* the off position.
- **No parameter smoothing anywhere.** Not on the lid path, and — deliberately, unlike most plugins — not on PRE, OUTPUT, MIX, CEILING, FLOOR or SHAPE either. Every parameter is read once per block and applied as a step. The usual 10–20 ms ramp exists to hide zipper noise; here that noise is the point. A hard automation lane on PRE or CEILING should sound *yanked*, not eased, and the block-rate stepping is part of what makes a fast sweep sound like something breaking rather than something fading. Smoothing would sand off exactly the roughness this plugin exists to produce. The host's buffer size is the only knob that changes how coarse the steps are, and that is the user's call.

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

Sidechain source and link happen at base rate; everything from the filter onward runs oversampled — 8x on HQ, 4x on LQ, and not at all on YUCK.

---

## 4. Implementation notes

### 4.1 Oversampling

**8x on HQ, 4x on LQ, 1x on YUCK.** Four independent alias sources: the rectifier's corner at every zero crossing, the `t^p` exponent, the moving hard clamp, and the multiply itself (`carrier * lid` is bandwidth-expanding even in VCA mode, since `lid` carries harmonics up to Nyquist and the products fold). Moving hard clip is the worst of them; dedicated clippers routinely run 16–32x.

Use `juce::dsp::Oversampling` and report latency to the host.

Both the carrier and the detector run at the full rate, and measurement says both have to. Alias floor against the fundamental, from `tests/alias.cpp`:

| | 1x | 2x | 4x | 8x |
|---|---|---|---|---|
| clipper | −32 dB | −48 dB | −60 dB | −69 dB |

Roughly 9 dB per halving. The obvious saving — run the detector slower and interpolate the lid up to the carrier's rate — **does not work**: the interpolation's images land on the decimator's fold points, so a 2x detector with an interpolated lid measures −53 dB against 8x's −85 dB. The lid has to be computed at the rate it is used at.

Filtering at base rate and upsampling afterward stays valid in principle — the filter is linear and generates no aliasing — but it is not where the money is. Profiling put the engine loop at roughly an eighth of the plugin's cost and the two oversamplers at the rest, so a second code path would buy almost nothing. The rectifier is the part that cannot move.

What the routing *can* skip is arithmetic it has already done: with INT + STEREO the detector is the main input, so the carrier's upsampled block is reused directly, and any routing that puts the same signal on every detector channel upsamples one channel instead of two. Both are exact and are checked bit-for-bit in `tests/host_test.cpp`.

#### QUALITY

Three modes, measured at 48 kHz in 512-sample blocks:

| | factor | filter | alias floor | CPU |
|---|---|---|---|---|
| HQ | 8x | linear phase FIR | −69 dB | 3.5 % |
| LQ | 4x | minimum phase IIR | −60 dB | 1.3 % |
| YUCK | 1x | none | −32 dB | 0.3 % |

LQ is the eco mode: a third of the CPU for 9 dB of alias floor, spending linear phase to get it. **YUCK is not an eco mode.** It is 1x — `juce::dsp::Oversampling` built with factor 0, which is a pass-through stage — so there is no anti-imaging filter of any kind and neither phase response applies. The aliasing is not a cost here, it is the feature; the moving hard clip folds its own harmonics back down the spectrum and that is what it sounds like.

Two constraints apply to all three:

- **Latency must not move.** The shorter paths are padded back out to the 8x figure, so `setLatencySamples` reports the same number in every mode and no host is asked to renegotiate delay compensation mid-session. YUCK, having no latency of its own, is padded by the whole of it.
- **The swap must not click.** No two of the cascades line up, so the seam steps regardless of how carefully the incoming one is primed — priming does not fix it. The output is ducked over ~4 ms and held silent until the padding delay has flushed. Unducked, the seam steps 22x the steady-state sample delta.

All three oversampler pairs are built in `prepareToPlay`, so changing QUALITY never allocates on the audio thread.

### 4.2 The shaping exponent

`p` changes only when SHAPE moves, so `t^p` must not be a per-sample `pow()` — at 8x stereo that is ~770k calls per second and would dominate the whole plugin. Build a **1024-point lookup table with linear interpolation**, rebuilt on parameter change. What remains per sample is a few multiplies, a table lookup and a clamp.

### 4.3 The filter

Cascaded **TPT state-variable** sections, Butterworth-aligned, one 1-pole section for odd orders. Not biquads: at 20 Hz with 8 poles, cascaded biquads are numerically fragile at 44.1 kHz, ring badly and dislike being modulated — and this knob is meant to be swept.

**FILTER POST changes what the knob means.** In PRE it is a frequency: which band of the sidechain drives the lid. In POST it is an envelope follower and the cutoff is effectively a release time (20 Hz ≈ 8 ms of smoothing; 20 kHz ≈ no smoothing, i.e. PRE mode with the filter off). The readout must relabel itself in POST mode — showing "20 Hz" while the user dials a release time is a lie the UI can cheaply avoid.

**Butterworth overshoots on a rectified signal.** The envelope will ring below zero, which would send the lid above 1 and hand a negative base to the exponent. Clamp `mag` to `>= 0`. This makes the values valid; it does not remove the ringing, which at high slopes remains audible as post-transient wobble. That is accepted as character.

### 4.4 Channel configurations

- mono → mono and stereo → stereo main buses; sidechain accepted as mono or stereo in both
- no mono → stereo (doubles the bus-layout matrix for something nobody asks for)
- on a mono instance, **grey out** the SC LINK selector rather than hiding it, so the panel does not reflow

### 4.5 The WTF link

The last position on SC LINK, not a control of its own — it is one more way for the detector's two channels to relate to each other, which is exactly what that selector already chooses.

It **sums** the sidechain the way MONO does, then splits the sum by sign and gives one half to each channel: the positive excursions close the left lid, the negative ones close the right. Fed a low sine the two lids alternate at the sidechain's own rate, so the carrier appears to pan — a panning that comes from two clippers taking turns, not from any gain law.

Two things follow from where the split sits:

- **The rectifier has to be half-wave, not full-wave.** A full-wave rectifier is precisely the thing that discards the sign the split is made of. Flipping the right channel's copy of the sum turns "the negative half" into "the positive half", so one half-wave rectifier serves both sides.
- **In POST the split must happen before the filter.** POST rectifies first and filters the result, so splitting afterwards would hand both channels the same envelope and there would be no pan left. Splitting first gives the filter two half-wave signals and it returns two envelopes, one per side. In PRE the filter is linear and runs on the still-bipolar sum, so the sign survives it and the split happens after — which is what keeps PRE at waveform rate.

The scope keeps drawing the bipolar detector in PRE, so in WTF its top lobe is the left channel's lid and its bottom lobe the right's — and in this mode the display follows that all the way through, so that it looks the way it sounds:

- **the aperture splits.** The mask closing from the top is the left lid, the one closing from the bottom is the right. Everywhere else the two lids are the same signal and the aperture is symmetric; here it visibly alternates, top then bottom, at the sidechain's own rate.
- **the output is drawn once per channel.** The right sits at 30% throughout. The left carries the reading: full strength wherever the two outputs are the same signal — so that stretch of the trace is exactly as bright as the single line every other mode draws, with the right hidden underneath it — fading to the same 30% as the two come apart. The ramp is linear over about two pixels of separation at the display's own scale, which is where two traces stop being one line. So agreement reads as brightness: inside one half of the sub the lid holds one channel down at its peaks and lets both run free through the zero crossings, and the trace brightens and dims within a single carrier cycle to say so.

  The alpha is a horizontal `ColourGradient` on the stroke with **a stop per pixel column**, taking the widest gap between the two channels inside that column. Coarser stops are what the first attempt did and they are wrong here: the gap turns over at the carrier's rate, not the sidechain's, so a stop every few columns smears every bright stretch away and the whole trace sits at 30%.

#### The WTF dial

How far the mode is taken. It moves **three** things across two halves of travel, and all three are at rest at 50 % — which is why 50 % is the original behaviour to the sample.

| | detector | mid | invented side |
|---|---|---|---|
| **0 %** | no split — both channels see the whole rectified sum | untouched | ×1 |
| **50 %** | the full split described above | untouched | ×1 |
| **100 %** | the full split | replaced by what MONO would have produced | **×2** |

- **0 % is MONO.** With both lids driven by the same detector the two channels agree, which is precisely what the MONO link does. The bottom of the dial and the middle position of the switch meet there deliberately: the dial spans MONO → WTF → the inverted mode, so what it selects between is legible without a manual.
- **100 % makes the effect survive a mono speaker.** Below it, WTF *half-cancels* when summed: at any instant one channel is clipped and the other is not, so the sum keeps half of what the lid removed and a phone speaker hears a weaker version of the plugin than the room does. At 100 % the pair is slid until its mid is the mono result, so what a mono listener hears is **exactly** the MONO link — not an approximation of it — while the difference between the channels, which is the whole of the effect, is untouched and stays in the sides.

The mechanism is phase cancellation, not clipping. The part of the carrier the lid removes is handed to *both* channels with opposite signs, so it is still there in stereo — wide, and on the wrong side of the lid — and annihilates on the way to mono. The clipping a mono listener hears is that annihilation, not either channel's own output.

#### The width that rides the top half

Replacing the mid costs the mode its stereo *position* (see below), so the same stretch of travel that replaces it also widens what is left. This is a width control in the mid/side sense — the same thing an Utility patched in after the plugin does — and it is mono-blind for exactly the reason the mode is: what it scales is added to one channel and subtracted from the other, so the sum never sees it. It rides the mid replacement rather than sitting on a dial of its own because it exists to pay for what that replacement costs; the two belong at the same end of the travel.

**What it scales is only the side the effect invented.** The pair's own stereo image is already in the mono-processed pair the mid is built from, and that side is subtracted out before the scaling and added back after. Without that step this would be an ordinary widener bolted to the output, which would pull an already-stereo input apart whenever the plugin was idling. With it, material the lid is not touching comes through untouched — checked in `tests/engine_test.cpp`.

It stops at ×2, which is where an Utility's own control stops and also, by measurement, exactly where the invented side starts overtaking the carrier. Measured **in PRE**, on a 0.9 carrier with the ceiling at −9 dB:

| side gain | interchannel correlation | side/mid | peak |
|---|---|---|---|
| ×1 | −0.25 | 1.29 | 0.90 |
| ×1.5 | −0.58 | 1.93 | 0.90 |
| **×2** | **−0.74** | **2.58** | **0.90** |
| ×3 | −0.88 | 3.87 | 1.35 |

Below ×2 the invented side is smaller than the carrier and the peak does not move at all; above it the side takes over and the peak climbs with the gain. So the top of the dial is the last setting that costs no headroom — **in PRE**. The next section is why that qualifier is there.

#### The headroom claim is a property of PRE, not of the width

Write `lL`, `lR` for the two split lids and `lM` for the mono one. At 100 % the whole pair collapses to

```
out_L = d_L·lL + d_R·(lM − lR)
out_R = d_R·lR + d_L·(lM − lL)
```

so the most the peak can grow is `max(lL + |lR − lM|, lR + |lL − lM|)`.

**In PRE that factor is identically 1**, structurally and at every sample. The split there is exact: one of the two halves is always zero, so one lid is wide open at 1 and the other *is* `lM`. Substituting gives `lM + (1 − lM)` and `1 + 0`. The coefficients are a convex combination of the two carriers, so the output peak cannot exceed the input peak — for any material, at any setting. The table above is that theorem, measured.

**In POST nothing ties the three lids together**, and the factor drifts off 1 in both directions:

- the two half-wave envelopes overlap once they are smoothed, so `lM`'s detector — their sum — exceeds both halves and `lM` closes further than either lid;
- the Butterworth ringing of 4.3 can put one half-envelope below zero, so the sum falls *under* the other half and `lM` sits **above** a channel lid instead.

The factor tops out at 2, i.e. +6 dB, and gets there in practice. Same carrier, decorrelated:

| detector | max factor | output peak (input 0.900) |
|---|---|---|
| PRE, any setting | **1.000** | 0.900 |
| POST 400 Hz 2-pole, INSTANT floor | 1.032 | 0.924 |
| POST 200 Hz 4-pole, floor −12 dB | 1.404 | 1.226 |
| POST 20 Hz 8-pole, narrow window | 2.000 | 1.793 |

This is accepted rather than corrected. The guarantee this mode actually makes still holds in POST — the mono sum is exactly MONO's output and is still bounded by MONO's own ceiling; it is the *per-channel* peak that moves, and the per-channel ceiling is something this half of the dial has already given up (see the consequences listed below). Capping the width per sample would mean a divide in the 8× oversampled inner loop and a width that modulates with the sidechain, which is a worse trade than trimming OUTPUT. `tests/engine_test.cpp` pins the PRE invariant at exactly 1 so a change to the detector maths cannot quietly break the half that is guaranteed.

#### Why width and not a level difference

The width this adds is **decorrelation**, not panning, and it cannot be anything else. Exact mono cancellation means `out_L = M + d` and `out_R = M − d`, and two things follow from that form alone:

- **`|out_L| = |out_R|` wherever `M = 0`.** `M` is the mono result, which is zero exactly when the lid is shut — the moments the effect peaks. So the interchannel *level* difference is structurally zero there, whatever `d` is.
- **The correlation depends only on `⟨d²⟩/⟨M²⟩`.** The shape, spectrum and phase of `d` are invisible to it. Decorrelating `d` with allpasses moves the measured correlation by nothing; only its level does anything.

This is why 50 % sounds like a wider *position* than 100 % does. At 50 % the lid closing to zero silences one channel outright while the other runs full — a 53 dB alternating level difference over 5 ms windows, which is the strongest lateralisation cue there is. At 100 % the same instant is `±d`: equal levels, opposite polarity, 0.3 dB. The two are different kinds of width and the dial does not morph between them, it trades one for the other. Scaling `d` is the only lever the arithmetic leaves, which is why the width rides that half of the travel — it is the compensation, not a separate idea.

Measured across the dial, mono carrier:

| intensity | correlation | short-term ILD | side/mid |
|---|---|---|---|
| 0 % | +1.00 | 0.0 dB | 0.00 |
| 50 % | +0.28 | **53.2 dB** | 0.75 |
| 75 % | −0.41 | 4.4 dB | 1.55 |
| 100 % | **−0.74** | 0.3 dB | **2.58** |

Two consequences follow and are accepted:

- **Above 50 % neither channel is clamped to the lid.** One of them is deliberately pushed past it by the half of the excess it was handed. This is not avoidable: two signals both clamped at the lid sum to something also clamped at the lid, so a per-channel ceiling and an exact mono sum cannot both hold. The ceiling becomes a mono-sum ceiling.
- **The scope's aperture therefore stops bounding the output trace**, which everywhere else it does. The trace visibly escaping the mask is the display saying this is no longer clipping — see 5.1.

**The mono detector costs no third filter.** The sections are linear, so the two half-wave envelopes sum to the envelope of the whole and the mid's detector falls out of the two the split already computes. In POST that sum has to be taken **before** the `>= 0` clamp of 4.3 and not after: clamping the halves and adding them is not clamping the sum, and wherever the Butterworth ringing puts one half below zero while the other is above, the two disagree.

On a mono instance only the positive half is left, since there is no second channel to hand the other one to — and with no second channel there is no mid to cancel anything against either, so a mono instance stays at the 50 % behaviour whatever the dial says.

### 4.6 Real-time safety

`ScopedNoDenormals` in `processBlock`. No allocation, no locks, no file or GUI access on the audio thread. Scope data crosses to the editor via a lock-free FIFO only. QUALITY is the obvious way this could be broken — all three oversampler pairs are therefore allocated up front and the switch only changes which pair is addressed.

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
STEREO / HQ / FILTER PRE / SIGNAL EXT, and SCALE below them — with WTF beside
SCALE on that last row, where it is present **only** while the link is on WTF.
It is hidden rather than dimmed because SCALE simply re-centres when it goes;
the two routing rows above never reflow, which is what 4.4's greying protects.
It sits with SCALE rather than with the link it belongs to because it is an
amount and everything on the rows above it is a switch. The first two are
three-way: STEREO cycles STEREO / MONO / WTF and HQ cycles HQ / LQ / YUCK.
The link cycles away from its default into the ordinary alternative first —
one click from STEREO is MONO, and WTF is past it. That also leaves the far
end of the switch open: a second WTF engine would be another entry after this
one rather than a renumbering of everything before it.

**Right-clicking any pill backed by a list of choices opens that list as a
menu**, so a three-way switch does not have to be cycled to find out what it
can do. Cycling on left-click stays: it is the faster gesture once the options
are known. A pill backed by a *number* has no such list, so WTF's right-click
names the three settings worth naming — 0, 50 and 100 % — and dragging it
covers everything between.

SCALE sits apart from the other four: it is a UI preference, not a plug-in
parameter, so it is deliberately absent from the host's automation list.

The settings panel is a **swap, not an overlay** — it takes the scope's exact
bounds, which is how Figma draws it (Oscilloscope variant "Variant3"). There is
no designed popup anywhere in the file.

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
±CEILING, fading through the window, and fully transparent inside ±FLOOR. So the
trace reports its own relevance without needing the bands drawn, which is what
lets the resting state carry no overlay at all. At INSTANT the floor stops
collapse together and the fade simply runs to the zero crossing.

JUCE fills a stroke from a gradient as readily as it fills a shape, so this and
the greyed outline below are both a `ColourGradient` and a `strokePath` rather
than anything cleverer — which is what the Figma annotation asks for when it says
it built the effect by hand and to "do this in the easiest programmatic way".

**POST is drawn mirrored.** The detector in POST is a rectified envelope, so
plotted literally it is a half-wave sitting on the centre line -- it reads as a
broken trace, not as the signal a symmetric pair of thresholds is measuring. The
trace is drawn together with its reflection about the centre, which brackets the
centre the way the lid does. The data is untouched: the reflection is the same
sample at the same x, so the envelope still crosses CEILING exactly where the lid
closes and cannot drift out of step with it. Redrawing the raw pre-rectifier
sidechain there instead would drift -- the filter is after the rectifier in POST,
so the wave and the thing driving the lid are no longer the same signal.

**Layer order** follows Figma: traces first, then the ceiling bands, then the
floor band over the top. Drawing the bands underneath instead loses the tint
where a trace crosses into the clamped region, which is the one place the
overlay is telling you something.

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
  against **HQ**, so the set reads as one switch rather than three labels.
  **YUCK** breaks that: #8d5c3d, the only warm colour on the panel, and light
  enough to read as a warning rather than as another dimmed label — it is the one
  setting that is not a trade-off but a deliberate mess.
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
- **the scope is a CEILING control.** Drag anywhere in it and the threshold, the
  bands and the dial all follow. Because CEILING is tapered to amplitude and the
  display's vertical axis *is* amplitude, one pixel of drag is one pixel of
  threshold — the band edge tracks the pointer exactly, and there is no
  sensitivity constant to pick because the display already fixes the scale.
- the dot beside CEILING is a **lid-activity LED**: brightness tracks
  instantaneous gain reduction.

### 5.3 What the scope shows, and when

The overlays are not decoration and are never all on at once. Each answers the
question the control being touched is asking, and hides whatever would compete
with the answer.

| state | shown |
|---|---|
| at rest | the lid aperture, the sidechain as a line, the output |
| **dragging** CEILING, FLOOR, or the scope itself | the sidechain as a filled body, the ceiling and floor bands, the output at 10% — **no lid** |
| dragging SHAPE | the SHAPE curve alone; no audio, no bands, no CLIP |

**Hover does not raise the bands.** The resting state already carries the
thresholds in the sidechain's own fade, so there is nothing to reveal, and
raising them on hover made the display flash every time the pointer crossed a
dial on its way somewhere else. Only a gesture that is actually changing a
threshold changes the picture.

There are two audio states, not three: a drag on CEILING and a drag on FLOOR show
the same thing, because that state answers both questions at once. The sidechain
fills out into a solid body against the bands and everything else steps back
rather than being removed. The output drops to a 10% ghost — enough to keep the
context, not enough to compete — and the lid aperture goes entirely, since the
question being asked is about the sidechain's level and the aperture is about
what happened to the carrier.

The sidechain's outline also **greys out where it passes inside the floor band**,
where the lid is wide open and the level is not doing anything. That is one more
`ColourGradient` on the stroke, with its stops on ±FLOOR.

The SHAPE curve replaces the scope rather than overlaying it, because a transfer
curve and a waveform share an axis and mean different things by it. Only the
wordmark and the gear carry over — CLIP goes with the rest, since it belongs to
the scope's frame rather than to the curve standing in for it.

It is drawn on a **centred square**, x being how far the detector has crossed the
FLOOR→CEILING window and y how far the lid has closed by then. The square is the
point: SHAPE 0 is then a true 45 degrees, and the two halves of the control read
as the reflections they actually are. The exponent is `2^(-4*shape)`, so +s and
−s give `t^p` against `t^(1/p)` — exact mirror images about that diagonal. On the
scope's own 380×230 frame neither would be true.

The values come from an instance of the engine's own `ShapeTable`, not from a
second copy of the formula, so the exponent, its [1/32, 32] clamp and the table's
quantisation are all the ones the audio uses.

**The curve is deliberately a guide, not a transfer plot.** The engine's shaping
is `1 - t^p`, which has a single knee. What is drawn is that half-curve plus its
own 180° rotation about the centre of the square, so it has two: SHAPE −1 reads
as an S that eases in and out, SHAPE +1 as a squared-off step that slams and
holds. That is what those settings *sound* like, and it is the shape the design
asks for; the literal one-knee curve reads as a lopsided swoop that says nothing
useful about the difference between the two ends.

The doubling is chosen so it cannot lie about anything that matters. Both halves
meet exactly at (0.5, 0.5); the curve still leaves (0,0) and arrives at (1,1); it
stays monotonic throughout; and at SHAPE 0 the exponent is 1, both halves
collapse onto the same straight line, and the diagonal is exact. So "left of
centre breaks later and gentler, right of centre breaks sooner and harder" stays
true — only the curvature between the endpoints is stylised.

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

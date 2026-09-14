# Unified expression profiles

Implemented after the 2026-09-11 isolated control tests. The final recording
`SkyrimSE_jtWvyvCd8e.mp4` showed visible native brow movement. During that run,
each raised brow moved the eyebrow geometry by about 0.328 mesh-local units;
the squints moved the head by about 0.134, matching the source TRI. Native brow
application is therefore no longer the leading explanation for the weak normal
performance. This does not establish that the separate regional overlay renders.

## Changes

- Removed the standalone brow controller, text interpretation and release tail
  from LipSync. Its separate Faces toggles/sliders are gone. Old `bBrowSync` and
  `iBrowStrength` keys are ignored, not migrated or deleted from user files.
- Each player expression beat selects one complete profile. An explicit emotional
  tone selects its emotion profile; otherwise conversational intent selects
  Inquiry, Confirmation, Skepticism, Explanation, Reassurance, Greeting or Neutral.
- Brow lift, inward movement, squint and regional detail are authored together.
  Their intensity and clause clock are shared; there is no extra lip-sync brow pose.
- Lift is signed, both in the profile and during smoothing: negative lowers,
  positive raises. A raised-to-lowered transition passes through neutral instead
  of accumulating opposing modifiers. The previous inquiry's cancellation is removed.
- Neutral statements have zero added pose by default. A neutral line releases
  the prior expression rather than getting a generic opening eyebrow gesture.
- While a silent-listener native full expression is active, the upper-face profile
  eases to zero. The brief handoff can overlap, but no separate sustained pose is
  added over the native expression or built up invisibly behind it.
- The low-level eight-control diagnostic remains opt-in and off; it is not a
  normal expression layer.

The existing mouth synthesizer, DBReV timing, phoneme sampling, gaze/blink owners,
camera and dialogue controls are unchanged. This is not a new cheek/eyelid mesh
asset or a verified richer regional-expression implementation.

## Configure profiles

Use sections in `SKSE/Plugins/SD_user.ini` such as the example below. Profile
values use the existing MCM -> user -> shipped -> built-in configuration precedence.
Missing keys retain defaults. Profiles load with expression settings and on the
next conversation; editing the file alone does not immediately change an active line.
No new required configuration file is introduced, and existing user settings do
not need to be replaced. Global `bExpressions` still applies. `iExpressionStrength`
is retired and ignored in every configuration source; the strength slider is removed.

```ini
[Expression.Inquiry]
iBrowLiftLeft=45
iBrowLiftRight=60
iBrowInLeft=30
iBrowInRight=18
iSquintLeft=22
iSquintRight=17
iRegionEmotion=7
iRegionStrength=52
```

`iBrowLiftLeft/Right` accept -100..100. The inward and squint controls accept
0..100. These are profile weights before fixed intensity, timing and the soft
0.65 actuator ceiling, not guaranteed percentages of visible movement.
Use a negative lift to lower a brow; do not add separate down/up keys.

Regions: 0 none, 1 anger, 2 disgust, 3 fear, 4 sad, 5 happy, 6 surprise, 7 puzzled.
`iRegionStrength` accepts 0..100 and requires the existing matching local upper-face
asset plus `bRegionalExpressions=1`. It never grants permission to use mouth morphs.

| Profile section suffix | Lift L/R | In L/R | Squint L/R | Region / strength |
|---|---|---|---|---|
| Neutral | 0 / 0 | 0 / 0 | 0 / 0 | none / 0 |
| Inquiry | 45 / 60 | 30 / 18 | 22 / 17 | puzzled / 52 |
| Confirmation | 36 / 29 | 6 / 6 | 9 / 9 | surprise / 40 |
| Skepticism | -30 / 50 | 32 / 18 | 30 / 22 | puzzled / 52 |
| Explanation | 15 / 12 | 13 / 13 | 17 / 17 | none / 0 |
| Reassurance | 16 / 16 | 10 / 10 | 15 / 15 | happy / 24 |
| Greeting | 24 / 20 | 2 / 2 | 23 / 22 | happy / 32 |
| Anger | -40 / -40 | 32 / 32 | 32 / 32 | anger / 70 |
| Disgust | -29 / -19 | 27 / 22 | 36 / 26 | disgust / 70 |
| Fear | 43 / 43 | 20 / 20 | 0 / 0 | fear / 70 |
| Sad | 24 / 24 | 37 / 37 | 10 / 10 | sad / 70 |
| Happy | 14 / 12 | 0 / 0 | 43 / 40 | happy / 70 |
| Surprise | 49 / 49 | 0 / 0 | 0 / 0 | surprise / 70 |
| Puzzled | -17 / 30 | 30 / 18 | 24 / 18 | puzzled / 70 |

Emotion profiles are scaled by the selected tone's strength; they are not added
on top of the conversational action's brow profile. The native full-face silent
listener response is retained and is not retargeted by these upper-face keys.

## Fixed strength and smoothing (v4, 2026-09-11)

The user's 200% run showed hard-clipped inquiry brows (both around 0.649), which
loses the configured left/right distinction. Inspection also found two discontinuity
paths: a moving target crossing the current pose zeroed its velocity, and native
listener ownership replaced the published upper-face pose with an instantaneous zero.
The latter could reveal an already-moving hidden profile at release. These are
code-level findings, not proof that every observed twitch came from SD.

- Fixed intensity at **1.50**, selected within a **1.40–1.60** tuning band.
  Existing saved 200% values are left in place but no longer read.
- Soft ceiling: linear through 0.35, then `0.35 + 0.30*(1-exp(-(x-0.35)/0.30))`.
  Weak details keep their authored weight; strong controls approach 0.65 continuously.
- Brow and regional motion use a critically damped response with omega 6 (previously
  8). Target crossings preserve velocity; only physical bounds and tiny rest
  residuals stop it. This changes motion, not dialogue/voice clocks.
- Listener suppression now gates the target before smoothing, never the published
  pose. Native listener expressions accelerate smoothly from rest (omega 8) rather
  than using the previous fast first-order onset. Native full expressions still
  clear immediately on player speech to protect lip sync.

Reproducible sweep from `ExpressionProfilesTests`: production sampler, default
Inquiry, three-second voice window, 60fps, maxima over the line. These are actuator
weights, not measured mesh displacement or a visual preference study.

| Fixed-scale candidate | Peak lift L/R | Peak inward L | Peak squint L |
|---|---|---|---|
| 100% | .514 / .577 | .391 | .290 |
| 125% | .565 / .610 | .462 | .362 |
| 140% | .585 / .622 | .495 | .400 |
| **150%** | **.596 / .628** | **.514** | **.422** |
| 160% | .605 / .632 | .530 | .443 |
| 175% | .616 / .637 | .550 | .470 |
| 200% | .628 / .643 | .577 | .508 |

140–160% keeps the squint near .40–.44 and brow asymmetry above .025 without
returning to the weak 100% squint. Beyond that, the lift gains diminish and the
left/right difference shrinks. Those are engineering tuning criteria informed by
the user's preference for expressive acting, not a universally optimal range for
every head or profile override. The new ceiling means 150% is not numerically
identical to the old slider at 150%.

## Verification

Pure tests cover profile selection, editable-field routing, default neutral rest,
invalid values, frame-rate consistency, signed transitions, interruption and full
release. The existing acting/timing/camera safety tests remain part of the suite.
V3 received positive user feedback, with a remaining eyebrow-jump report. V4 still
needs a normal-dialogue visual test; successful unit tests are not proof that every
visible twitch is fixed. Engine/native-expression ownership restoration can still
change the underlying face, especially at voice start; that mouth-safety gate is
deliberately not weakened here.

V4 Release build passed with warnings treated as errors and all 14 ctest tests passed,
including new target-crossing, soft-knee, native-onset, and handoff regressions at
30/60/144fps. Built DLL SHA256:
`60375265ABD6BF72DC81E009A774D2D052BF5572104630B8BC0784C6B05BF6BA`.
At verification Skyrim was still running, so installation is pending. The installed
DLL remains v3 (`DD46D41F6CE8109AAA970A1115694FB3CB6032826BBBF71BD242473AAB6B2F9F`).
Live settings/head assets are untouched and the diagnostic remains off (`iRun=0`).

Pre-v4 runtime/UI sources, log and installed v3 DLL were backed up under
`build/expression-smoothing-backup-20260911-164108`. Earlier v3 backups remain under
`build/unified-expression-backup-20260911`.

At the user's subsequent request, the 1.4.5 ZIP was repacked with this DLL and
the current shipped defaults. Both archive entries match their source hashes;
all 14 tests passed again. The previous ZIP/checksum/notes are preserved under
`package/backups/1.4.5-before-expression-v4-20260911-164707`. ZIP SHA256:
`E6BBE571F3BAE86A73FDE2A3188043A32B023EBECD40F31C91C1469C0A021EB2`.
Packaging does not imply visual validation or installation into the running game.

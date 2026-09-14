# Preset rebuild — what shipped

Rebuilt 2026-08-16, from the brief written at `d55e368`.

The five presets were rebuilt, per-shot **lenses** and **how-often** became
things a preset can set, and every name and description in the menu was gone
over. Built and deployed. **Not yet tested in game** — that is the next thing
to do.

---

## The one rule for all text

Unchanged, and it drove most of this session.

**Short, plain, and for somebody who has never seen a film set.**

- Say what they will **see**, not what the technique is called.
- One idea per sentence. Two or three sentences per description, not five.
- No jargon without the plain word next to it — and prefer the plain word alone.
- Never explain the code.

---

## The headline change: presets set the glass, and how often

`Preset` gained two fields alongside the existing `motion` list of
`{shot, move, amount, time}` — `lenses` as `{shot, degrees}` and `weights` as
`{shot, weight}`. `PresetLens()` and `PresetWeight()` resolve one setup exactly
as `PresetMotion()` does, and **apply, drift and the menu all call them**, for
the same reason those three had to agree about motion.

A preset now says four things about each angle it uses: whether it is in play,
what it does, what it is shot on, and how often it comes up. It used to say
one and a half.

Before this, every preset inherited the shot table's lens for every angle it
used, so two looks drawing the same setup were shooting it identically whatever
their blurbs claimed. That was the largest single axis of difference available
and no preset could reach it.

Kept as its own list rather than as two more fields on `Motion`, because most
setups change glass without changing what they do — folding them together would
mean restating a look's baseline move on every entry that only wanted a lens.

Applying a preset writes a lens and a weight for **every** setup it uses,
including ones it does not name (those get the table's own value back). A
preset that only wrote the ones it mentioned would inherit the previous look's
numbers on everything else and could never be reproduced.

### Why how-often mattered more than it looks

Weight is not just an ordering knob inside a group. `Coverage()` rolls
`PoolWeight(kEnvironmental)` against the live coverage pool's total, so **the
share of a conversation given to the room is the sum of what its setups are
worth**. A preset that could not set weights could not say how much of itself
was room — the loudest single thing four of these five looks have to express.

Two looks were being actively undercut by the table's defaults, which are
authored for the whole 39-setup vocabulary (eight staples at 100, the rest at
50) and mean nothing once a preset has cut that to twelve:

- **Show the Room** shipped its master and its wide as accents and its close-up
  as a staple. The preset named for the room spent 60–69% of every speech on
  faces, with the tightest angle in its list drawn more often than any wide.
- **From a Distance** drew its long-lens close-up exactly as often as everything
  else, when a close single from across a room is punctuation in that look.

Measured shares after the rebuild, from the real pool memberships:

| | NPC line (early) | NPC line (late) | your turn |
|---|---|---|---|
| Over the Shoulder | 15% room | 14% | 14% |
| Up Close | 0% | 0% | 0% |
| From a Distance | 49% | 41% | 48% |
| Always Moving | 0% | 0% | 0% |
| Show the Room | 67% | 47% | 47% |

Up Close and Always Moving reach zero by arithmetic rather than by hope: neither
list contains an environmental setup, so that pool totals zero and the camera
never once cuts away from the two faces.

Show the Room is held at 85 rather than 100 on its two widest angles
deliberately. Its early NPC pool is thin, and pushing the room higher would take
about three quarters of the opening lines of every speech — the faces would stop
arriving at all.

Weights use one tier vocabulary across all five so the tables read as intent:
**100** spine, **85** regular, **70/55** the working middle, **45/35**
seasoning, **25 and below** punctuation. Matched pairs carry matched weights —
a reverse drawn half as often as the shot it answers is the same failure as a
reverse on the wrong lens.

### The constraint that governs all lens authoring

Distance is solved for the fill **at the chosen lens** and then floored at the
subject's minimum:

```
distance = 21 / tan(vFOV × fill / 2)      vFOV = 2·atan(tan(hFOV/2) / 1.78)
floor    = 68 units (a human; scales with the subject)
```

So a tight fill on a wide lens asks to stand closer than allowed, gets clamped,
and **renders looser than it was authored for** — the exact trap `kExtremeClose`
fell into on the widest glass. Roughly, `vFOV × fill` must stay under 34°:

| fill | lens ceiling |
|---|---|
| 0.85 | ~66° |
| 0.68 | ~80° |
| 0.60 | ~88° |
| ≤0.46 | no practical limit |

Over-the-shoulders are exempt — their standoff is forced past the other
participant regardless.

**A push-in multiplies distance after the solve with no re-clamp**, so a push on
a tight framing can put the lens inside a face. Every preset's push was sized
against this. `tools/` has no checker for it; the arithmetic was run once by
hand — see "worth building" below.

---

## The five, as they now stand

| Key | Shown as | Shots | Lenses | Avg | Moving | Leans on | Timing |
|---|---|---|---|---|---|---|---|
| classical | Over the Shoulder | 16 | 40–60 | 55 | 3 | the shoulder pair | 260–900, cut 1–2 |
| intimate | Up Close | 12 | 55–88 | 77 | 12 | the close-ups | 220–700, cut 1 |
| observed | From a Distance | 12 | 40–50 | 44 | 0 | From Far Off | 520–1500, cut 3–5 |
| kinetic | Always Moving | 17 | 55–95 | 81 | 17 | Three Quarters | 180–620, cut 1 |
| epic | Show the Room | 16 | 75–100 | 92 | 8 | The Whole Room, Wide | 300–1100, cut 2–3 |

Four of the five hold a tight lens range and stay in it. Up Close is the one
that spreads, and it spreads because of the floor above: its two extreme
close-ups are pinned at 55 while everything else runs 72–88. That outlier is
why the menu's plain-language line is computed from the **average** and not from
the range — reading the range alone called the widest look in the mod "mixed".

### What changed in each

**classical** — gained the two loose over-the-shoulders, pulled off the table's
88 onto the same 60 as everything else; at 88 they were room shots with a
shoulder in them. Mediums and three-quarters off 72 (close enough to the
player's own view to read as no camera at all) onto 60. The two-shot and the
profile onto 50, so the pair stack up against each other. The close-up's push
now has a **matching reverse** — a push on their close-up and not on yours
breaks the one claim this look makes. The extreme close-ups were pushing and are
now locked, per the shot table's own note. `holdOnShortLines` switched **on**:
nobody cuts for "Yes."

**intimate** — the lenses run backwards on purpose, tightest framing on the
longest glass, for the floor reason above. The extreme close-ups dropped their
named push of 30 to the baseline 20; 30 would have driven them through the floor.

**observed** — shot list unchanged, it was already right. Nothing over 50 now.
A close-up here stands two metres further back than the same close-up in Up
Close and still fills the frame, which is the entire look.

**kinetic** — all 17 angles now named in the motion list, so nothing falls
through to a generic orbit. The four over-the-shoulders were taking the baseline
orbit, which arcs around the subject and swings the shoulder the shot is named
for out of frame; they crane instead. Wide throughout, because a moving camera
on a long lens moves a background that was already flat.

**epic** — had `kMediumPlayer` and `kThreeQuarterPlayer` without their NPC
counterparts, so their half of the exchange had one tight angle to come back to
against the player's three. Both added. The shot of the pair now arcs with the
other wides. Even the close shots stay wide (75/80) so a cut to a face does not
land in a different scene.

---

## Names

Every trade term that had a plain equivalent lost. The full mapping is in the
comment above `Name()` in `Shot.cpp`; the ones worth knowing:

| Was | Now |
|---|---|
| Dirty Single | Over Your / Their Shoulder (Tight) |
| Medium | Head And Shoulders |
| Low Angle / High Angle | From Below / From Above |
| Long | Full Figure |
| Profile | Side On |
| Two-Shot | Both Of You |
| Master / Ground | The Whole Room / From The Floor |
| Distant | From Far Off |

**The shoulder shots are named by whose shoulder it is.** "Over Your Shoulder"
is a shot *of them*, camera behind you. That was the most confusable pair in the
old set — the label read "Over The Shoulder" on both sides of the eyeline.

### The duplicate-name decision, made

**Seven names are still deliberately duplicated** across the two sides: Close
Up, Extreme Close Up, Head And Shoulders, Three Quarters, From Below, Full
Figure, From High Above. **Leave them.** Those pairs are the same framing on
opposite sides of the eyeline — that is what a reverse shot is, and matching
names are how the Shots page shows the pair go together. The panel heading says
which side you are reading; the log prints `SubjectName` beside the name. The
reasoning is written into the comment above `Name()` so nobody "fixes" it.

The one duplicate that was never defensible is gone: "Profile" appeared on both
the player list and the room list for two different shots, one of a person and
one of both of them.

### Names carry the explanation, because nothing else does now

A `Description(ShotType)` was written this session — one plain line per setup,
shown in the opened shot row — and then removed with the row that displayed it.
Nothing shows it, so it is gone rather than kept as dead data. That puts the
whole weight of the Shots page back on the names, which is exactly why they were
made plain in the same pass.

---

## The menu went flat

Two passes. The first replaced jargon; the second took out the explanation
entirely.

**The Presets page is five checkboxes.** Ticking one applies it. Gone from each
row: the summary line, four paragraphs of detail, the angle and lens and
leans-on lines, the drift count, and the Apply button. The names carry it —
somebody scanning "Over the Shoulder / Up Close / From a Distance / Always
Moving / Show the Room" knows which one they want without reading four
paragraphs, and the honest test of a look is applying it and walking up to
somebody.

Radio behaviour falls out of the existing design rather than being built:
nothing stores which preset is on, it is derived by comparing live settings, so
applying one makes every other comparison fail and they untick themselves. Green
when in use, grey otherwise.

**MY PRESETS stay as dropdowns**, because a slot needs a name box and
save/use/delete buttons. The two lists still cannot both claim to be running: an
active slot nulls the active preset, so ticking a built-in clears the slot's
"(in use)" and pressing Use on a slot unticks all five.

**The Shots page keeps its rows**, minus the explanation:

- the per-shot description line — gone
- every `(?)` marker on the row (Move, How often, Amount, Lens, Duration,
  Default) — gone, along with `Help()` itself, which had no callers left and
  would have been C4505 under `/W4 /WX`
- the three "ships as" hints — gone. They answered "what was this before I
  touched it", which the Default button answers better by putting it back, and
  three lines appearing and vanishing as values crossed their defaults made the
  row change height mid-drag.
- **the Default button stays**, and still leaves the Enabled tick alone.

Everything the removed text explained is still there and still works: Enabled,
the Move combo, and the How often / Amount / Lens / Duration sliders.

Earlier in the same session: `FOV` → **Lens**, `Weight` → **How often**, and the
About page lost "two-shot" and "Coverage".

---

## Saved slots carry both new fields

`kSlotVersion` is **3** — 2 added the lens, 3 added how-often. A slot must hold
everything a preset can write or it cannot reproduce its own look: the fields it
skipped come back holding whatever the last preset applied left behind. A slot
saved off Show the Room without weights would return as a shot list full of
wides the camera almost never cuts to.

**Anything added to `ApplyPreset` belongs in `Snapshot` in the same commit.**
That rule is now written into the header.

Older payloads still load. `kStrideForVersion` maps a version to how many
numbers it wrote per setup (v1 four, v2 five, v3 six), and anything a payload
never carried is **left exactly as it is** rather than guessed at — a slot saved
before a field existed has no opinion about it. An older slot **re-saves itself
once, the moment it is used**, because `ActiveCustomSlot` compares against a
fresh snapshot and an old payload could never equal one; without that the slot
could be running and still never say "in use".

---

## Five static asserts now guard the authoring

Every one of these failures is silent at runtime, which is why they earned a
build break. A look is authored by reading four lists side by side and none of
them shows up in the list you happen to be looking at.

1. Every setup a preset uses has a **lens** named for it. Forget one and it
   plays at the table's 72 in the middle of a look that runs at 45.
2. Every setup a preset uses has a **how-often** named for it. Forget one and it
   inherits a staple-or-accent tier authored against all 39 — which is exactly
   how Show the Room came to draw its close-up more often than its master.
3. No lens entry names a setup the preset does not use.
4. No weight entry names a setup the preset does not use.
5. No motion entry names a setup the preset does not use.

---

## Do not break these

- `key` strings on presets, and `b<Shot>` ini keys. Both are shipped contracts.
- The `Move` enum ordering. Append only.
- `bCoverPlayerTurn` — no preset writes it, and neither does a slot.
- The per-shot Default button leaves `Enabled` alone, on purpose.
- Anything that reads live state instead of the ini. Every slider that re-reads
  the file mid-drag snaps back under the cursor; fixed twice now.
- `PresetMotion`, `PresetLens` and `PresetWeight` are called by apply, drift
  **and** the menu. Keep it that way.
- `Snapshot` must cover everything `ApplyPreset` writes. See the slot section.

---

## Still open

1. **Test all five in game.** Nothing here has been seen on screen. Watch for:
   the classical loose over-the-shoulders at 60 (they stand much further back
   than at 88 and may not place in a tavern); epic's close-ups at 75/80; the
   kinetic over-the-shoulder cranes. And check the room shares above against a
   real conversation — 67% on the opening lines of a speech in Show the Room is
   the number most likely to feel wrong in play, and it is one weight away.
2. **A floor checker.** The fill/lens/push arithmetic was run once this session
   against a throwaway script and lives nowhere. Something that flags a preset
   shot solving inside the floor would make the next lens edit safe. It was
   deliberately **not** dropped into `tools/` as-is: that script carried its own
   hand-copied table of fills, which goes stale the first time a shot is
   retuned and then quietly checks the wrong numbers. Drive it off the C++
   table instead. It cannot join the three static asserts — `DistanceForFill`
   needs `tan` and `atan`, which are not constexpr before C++26 — so the
   feasible version is a startup pass that walks the five presets and logs any
   setup solving inside the floor.
3. **The dialogue menu position branch** — `dialogue-menu-position`, three
   commits, still parked. Range and clamp were wrong for a list that starts hard
   against the left edge.
4. **Legacy `iNameZoom` keys** still linger in the ini (`iMasterZoom=2` and
   friends). They are read only as a migration seed when `iNameMove` is absent,
   so they are inert but they read as live settings. Cannot go in `$retired`
   until the migration is dropped.

---

## Build and deploy

```
cmake --build build/skyrim --config Release --parallel 8
.\tools\Deploy.ps1
```

`VCPKG_ROOT=X:\vcpkg`. On this machine `cmake` is not on PATH — it lives at
`X:\VisualStudio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

Close Skyrim first or the copy is blocked.

Log: `C:\Users\Tobih\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\SceneDirector.log`
(Documents is redirected to OneDrive on this machine.)

When removing a setting, add its key to `$retired` in `Deploy.ps1` or it lingers
in the player's ini for ever, reading as if it still does something.

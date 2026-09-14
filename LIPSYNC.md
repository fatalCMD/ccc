# Player lipsync vs. the absolute-pose camera

> **CLOSED, 2026-08-18. This file is history, not the current state.**
>
> The premise it is built on — that the camera was suppressing a mouth the
> engine would otherwise have animated — is wrong, and every camera-side theory
> below is dead. Nothing ever wrote the player's viseme track: `SpeakSound`
> plays the audio and the `.lip` beside it is never decoded, so there was no
> animation to suppress. The morph pass runs on the player's head as often as on
> an NPC's while Scene Director is directing.
>
> Scene Director now drives the mouth itself, shipped in 1.3.1. **The authority
> on how that works is the header comment in `include/SD/Scene/LipSync.h`**,
> which supersedes this document and `LIPSYNC_NEXT_INVESTIGATION.md` entirely.
>
> Kept because the eliminations are real and expensive, and because §7's traps
> are still traps. Read it for what it rules out, not for what it concludes.

**Status when written: OPEN, but the search space collapsed.** Investigated
across one long session (2026-08-02 → 03) and re-audited on 2026-08-03. This
document exists so none of that has to be repeated.

**Symptom.** With a player-voice mod installed (DBVO), the player character's
mouth does not animate during conversations while Scene Director is directing
the camera. NPC mouths are unaffected. Reported by a user, reproduced by the
maintainer.

**Read §0 first.** It is a reporter observation from 2026-08-03 that identifies
the gate, and it makes most of what follows either confirmation or history.

---

## CORRECTION, 2026-08-10: the phoneme channel IS written

**The single most-cited measurement in this document and in HANDOFF §5 is wrong,
and everything reasoned from it needs re-reading.**

The claim was: *nothing writes the player's phoneme channel — `isUpdated` reads 0
on every sample, with a frozen stale 0.050 sitting in slot 4.* From that came the
conclusion that there is no morph to render and the fix must be to write the
channel ourselves, which is what the reverted `lipsync-1.2.2` branch did.

Measured twice tonight, on 1.2.3, with the face probe, mouth still not moving:

```
iPoseMode=0 (control)              iPoseMode=5 (camera record synced)
x68  ph=0.000 [i=0 up=1]           x50  ph=0.000 [i=0 up=1]
x12  ph=0.050 [i=5 up=0]           x4   ph=0.250 [i=5 up=0]
x11  ph=0.050 [i=4 up=0]           x4   ph=0.150 [i=5 up=0]
x9   ph=0.250 [i=5 up=0]           x3   ph=0.050 [i=5 up=0]
x6   ph=0.450 [i=7 up=0]           x2   ph=0.400 [i=5 up=0]
x6   ph=0.200 [i=4 up=0]           ... 0.100, 0.300, 0.450
```

`up=1` appears on the PLAYER, dozens of times per conversation — the state §5
says never occurs. The value is not frozen and not confined to one slot: it moves
across a range in slots 4, 5 and 7, which is a viseme track playing. `up=0` on
those samples means written AND consumed, which by this document's own
calibration is the reading for a mouth that is moving.

**So the failure is downstream of the channel, not at it.** Phonemes are being
written and drained while the mouth visibly does not move. That is a different
bug from the one this document has been chasing, and it means:

- §5's "there is no morph to render" is false.
- The `lipsync-1.2.2` approach — synthesising all 16 visemes from the authored
  `.lip` track — was solving a problem that does not exist. Something already
  writes them.
- The question to ask now is why written, consumed visemes do not reach the
  head's geometry.

**Not the camera record.** `iPoseMode=5` syncs `ThirdPersonState::translation`
and `::rotation` to where SD actually put the camera, on the theory that the
engine gates the player's mouth on its own stale record. The two columns above
are that test. They are the same. The stale record is not the gate, and mode 5
can be struck off.

Both runs are the same build, same NPC, same approach, minutes apart. The
mode-5 log is preserved at `scratchpad/posemode5-run.log`.

**What is NOT re-opened by this.** `culled` still reads 0 on every sample ever
taken, and `bHoldPlayerFace` still does nothing. The visibility and budget
theories in §0 stay dead. This correction moves the failure downstream; it does
not move it back upstream.

---

## THE TEST TO RUN FIRST, 2026-08-12: `[Diagnostics] iForceViseme`

**Do this before reading anything below.** It splits the entire remaining search
space with an eyeball answer, and it takes one launch.

`iForceViseme=0` pins viseme slot 0 to 1.0 on the player's head **inside the
`BSFaceGenNiNode::UpdateDownwardPass` hook, immediately before the original
runs** — so the pass that drains the channel drains our value, whatever else
wrote phonemes earlier in the frame. Stand in third person and look at your own
face. No conversation, no NPC, no voice mod, no line.

- **Jaw visibly stuck open** → the phoneme channel reaches the geometry. The
  fault is **upstream**, in what `SpeakSound` feeds the viseme track. Every
  camera-side theory in this document is finished, and so is SD's culpability.
- **Mouth does not move** → the channel never reaches the head. The fault is on
  the head itself: morph data, a head replacer, or the **HDT-SMP hair / beard /
  headgear** that §2 named as the DBVO troubleshooting guide's #1 documented
  cause of "audio plays but the mouth does not move" and that has been
  uncontrolled for this entire investigation.

Neither outcome depends on a judgement about whether a mouth was moving *enough*,
which is what every eyeball test here has actually been, and neither can be
confounded by whether DBVO voiced a particular line — the §7 trap.

**The morph-pass counter (`Scene/FaceGen.cpp`) had never been run.** Built
2026-08-10, deployed, and the log for 2026-08-12 is load, 69 seconds, zero
conversations. Its report is the second half of this run: `calls`, `didWork`,
`phonemePending`, `consumed`, per head, player against NPC.

Two defects in it were fixed the same day. `phonemePeakIn` was overwritten every
call, so the report printed the **last frame's** peak — 0.000 for any mouth at
rest, however loudly it moved a second earlier; there is now a `peakMax` over the
window, and every "the channel is flat" reading this probe has produced should be
re-read as that defect. And `bLogFaceAnim` was **hand-added to the live ini and
missing from the shipped one**, so `Deploy.ps1` — which drops any key the shipped
file does not carry — would have silently deleted it on the next deploy and taken
the morph hook with it. Both keys are in `config/SD.ini` now.

---

## 0. The gate is whether the player's head is on camera

> "When I face the camera towards the character so the face is visible and talk
> to someone, the lips begin moving. When the camera is on the character's back,
> the lips don't move."

Positional, reproducible, and it does what none of the instrumented theories
managed: it names the variable. §3 had narrowed the cause to *the camera's final
transform*. This says **which property of that transform matters — whether the
player's head is visible to it.**

**Four unexplained results collapse into this one, and three of them turn out to
carry no information at all:**

- **"AnimCam makes it work."** AnimCam is a free-flying camera the user points at
  their own face. Identical condition. Recorded as folklore; it was a sighting.
- **"Modes 1 and 2 fix lipsync."** They restore the *vanilla third-person camera,
  which looks at the back of the player's head.* The player's face is not on
  screen, so a still mouth was never observable. That is a **third** independent
  false-positive mechanism for those modes, on top of the camera being dead.
- **"IACC does not break lipsync."** Same reason. IACC keeps roughly vanilla
  over-shoulder framing, so the player's face is rarely on screen and there was
  nothing to see. **The IACC control was never a control.** This matters more
  than the other two: it was the comparative observation that "broke the case
  open" and pointed the whole investigation at SD, and it is empty.
- **"It works while seated."** Never reproduced under instrumentation. Now
  plausibly just a framing difference — worth one deliberate re-check.

**This also puts SD's culpability in doubt.** If the engine declines to morph a
head that is not on camera, SD is not causing that. SD is the first thing that
ever points a camera at the player's face during dialogue, and therefore the
first thing to make an existing engine behaviour observable. That is a very
different bug from the one this document has been chasing.

**One caveat, and it is the reason for the test below.** Pure frustum culling
does not cleanly explain an over-the-shoulder shot: there the player's head is
*in frame*, in the corner, and should pass a frustum test. So "on the character's
back" needs pinning down — directly behind and close, or over-the-shoulder with
the head visible in frame? The two point at different mechanisms.

### The test that settles it, in about a minute

**Is this player-specific, or engine-wide?**

```
tfc 1
```

Free camera, frozen, during an NPC's spoken line. Fly behind the NPC's head,
then back around to their face.

- **NPC's mouth also stops when you are behind them** → engine-wide,
  visibility-gated morphing. SD is innocent, the "bug" is vanilla behaviour newly
  made visible, and §10 becomes the answer rather than a fallback.
- **NPC's mouth keeps moving from behind** → the player is special-cased, and
  there is something left to find. §8 step 5 is then the way in.

**And the question that decides whether anything needs fixing at all:** when SD
cuts to the player's face mid-line, does the mouth pick up *immediately*, or is
there a visible beat of stale face first? Immediate means there is no
user-visible defect — you only ever see the mouth when it is framed, and when it
is framed it works. A lag means the morph state is stale on the cut, and that is
worth fixing.

If it does need fixing, the shape is now cheap and obvious: keep the player's
head in the morph set while it is off camera. `BSFaceGenManager::GetSingleton()`
is a plain singleton (`RELOCATION_ID(514182, 400331)`) with `numActorsToMorph` at
offset `0x04` and `emotions` at `0x08`, both directly writable while staging;
the ini dial behind it is `uiNumActorsAllowedToMorph` (default 10, max 64).
`NiAVObject::Flag::kAlwaysDraw` (`1 << 11`) on the player's face node is the
other candidate. Read those values during a conversation before writing any of
them — if `numActorsToMorph` moves when the shot changes, that is the mechanism.

---

## 1. The contradiction, and why it is no longer blocking

The reporter observed a configuration where the camera cut normally AND the
player's lipsync worked: `iPoseMode=3` in the build deployed at 23:58
(**build A**). In the build deployed at 00:18 (**build B**) the modes were
renumbered and A's mode 3 became B's mode 0. On B, mode 0 failed.

**A-mode-3 and B-mode-0 are the same code path.** This is now confirmed from an
independent artifact rather than from memory: `iPoseMode` does not exist at all
in the packaged `Cinematic Conversation Camera 1.2.0` (23:31) — its
`[Diagnostics]` section ends at `bLogFaceAnim`. Build A introduced the dial, and
build A's mode matrix survived verbatim as a stale comment in `Director.cpp`,
where it read `0 = time 0.0 (the bug as shipped)` and `3 = delta`. B's shipped
ini documents `0 = real frame time`, `3 = time = 0`. The renumbering is exactly
as described, and both configurations write `local` + `world`, run `Update()`,
`time = delta`, `flags = 0x2000`.

So the contradiction is real. But it is no longer the thing to spend a session
on, for three reasons:

1. **The A-mode-3 observation came from the session that also reported modes 1
   and 2 as working.** Those two are confirmed false positives — the camera is
   dead in both. The eyeball method used to score that session has a known
   failure rate of 2 in 3 on precisely this question. Item 3 of the original
   three-way split ("the observation was mistaken") is therefore much better
   supported than the other two, and it costs nothing to assume it.
2. **§2 gives a mechanism for genuine intermittency** that does not require the
   result to be unreproducible: whether DBVO voiced that particular line at all.
3. **§4 makes the whole question cheap to settle** — the player's lipsync path
   can be fired from the console on demand, so a "does this configuration work"
   answer no longer costs a conversation.

Settle it with §4, not with the five-conversation protocol that used to live
here. That protocol also had a fatal flaw: see §5.

---

## 2. The player and the NPC are not the same subsystem

**This is the finding that reframes everything, and it is verified from the
installed mod's own compiled Papyrus, not inferred.**

DBVO 1.1.1 is what is installed here — no SKSE plugin, no DLL. The chain is:

```
Edge UI's dialoguemenu.swf  (outranks DBVO's own copy in MO2 priority,
                             and has DBVO integration built in)
  -> skse.SendExternalEvent
  -> Papyrus mod event
  -> DBVO_Script_MCM
  -> ConsoleUtil.ExecuteCommand("Player.SpeakSound \"DBVO/<pack>/<file>.fuz\"")
```

The string table of `Dragonborn Voice Over/Scripts/DBVO_Script_MCM.pex` contains
`Player.SpeakSound "`, `DBVO/`, `.fuz`, `consoleutil` and `ExecuteCommand`.
The voice packs ship real `.fuz` files with embedded `.lip` tracks, laid out as
`Sound\DBVO\<pack>\<line>.fuz`.

**`SpeakSound` is a debug console command.** It is not the path an NPC's voiced
dialogue takes. So:

- **The NPC was never a control.** "The NPC's mouth works under the same camera
  write" does not imply a shared gate, because there is no shared path to gate.
  Every conclusion below that leaned on the NPC as a control is weaker than it
  looked, and the biggest one is retired outright — see §6.
- **The player's phoneme channel moving while the NPC's sits at zero stops being
  a paradox.** Two subsystems, writing to different places, is the ordinary
  reading of that observation.
- **`AnimCam` and "it works while seated"** stop looking like folklore. Both are
  reports that `SpeakSound`-driven lipsync is sensitive to player camera and
  animation-graph state, which is exactly the class of thing being chased.

Also worth knowing before the next run: the DBVO troubleshooting guide's #1
documented cause of "audio plays but the mouth does not move" is **HDT-SMP hair,
beards and headgear**. That has never been controlled for here.

---

## 3. Mode 2 is a control, and it exonerates the camera write

Mode 2 was dismissed as "functionally identical to `bEnabled=0`". It is not, and
the mistake is load-bearing.

In mode 2, `staging` is still true. The director runs, the letterbox is up,
`Performance` drives gaze and expressions, `Presence` writes the player's
headtracking graph variable, `KeyLight` aims, `Interface` fades the topic list —
and `ApplyPose` still runs **the same update traversal over the same subtree with
the same `NiUpdateData`, the same number of times per frame**. The only thing
mode 2 skips is the write to `cameraRoot->local`.

So mode 0 and mode 2 differ at the machine level in exactly one value, and
therefore in exactly one outcome: where the camera ends up. Mode 2 works.

**That kills every hypothesis about the write mechanism**, including all of these
which were live until now:

| dead | why |
|---|---|
| the update traversal is toxic | mode 2 runs it identically |
| `NiUpdateData::time` is wrong | mode 2 passes the same value |
| the flags are wrong | mode 2 passes the same flags |
| a facegen node caught in the traversal gets a frame delta instead of absolute time | mode 2 traverses the same subtree |
| `lastUpdatedFrameCounter` stamped by SD makes the engine skip nodes | mode 2 stamps the same counters |
| reentrancy — calling `Update()` from inside `ThirdPersonState::Update` | mode 2 is equally reentrant |

**What survives: something downstream reads the camera's final transform and
acts on it, and what it does is specific to the player.** That is the whole
remaining search space, and it is a much smaller one than §6 used to describe.

Two corollaries fell out of the same reasoning:

- **The `world` write in `ApplyPose` is a dead store.** Mode 2 proves it: writing
  `world` and then running the traversal leaves the camera where the engine put
  it, so `Update()` recomputes `world` from `parent->world * local` and discards
  what was written. Only `local` plus the traversal moves anything.
- **The flags have never been tested correctly.** `RE::NiUpdateData::Flag` is
  `{ kDirty = 1<<0, kDisableCollision = 8193 }` — and `8193` is `0x2001`, which
  *includes* `kDirty`. The `0x2000` SD passes is `kDisableCollision` with
  `kDirty` stripped, and is not a value the engine names. Mode 4 tested `0x0000`.
  Nobody has passed `0x2001`. Mode 3 vs mode 4 shows the flags changing nothing,
  so this is tidiness rather than a lead — but it is still wrong as written.

---

## 4. The test that costs a console command

Because the player's path is `SpeakSound` (§2), it can be driven directly:

```
player.SpeakSound "DBVO/voicebella/1750_gold..fuz"
```

That exact file is verified good: `FUZE` magic, version 1, **1753 bytes of
embedded lip data** in 12,989 bytes total — long enough to watch, and it cannot
fail for want of a `.lip` track. It comes from the Bella pack, which is the one
`selected_voice_pack.json` names. Any other file under `Sound\DBVO\<pack>\`
works; prefer names without apostrophes, commas or parentheses, which the
console will fight you over.

This needs no dialogue, no NPC, no topic list, and no DBVO involvement at all —
`SpeakSound` is the same engine entry point DBVO itself calls.

That dissolves the trap that cost most of a night. **You chose the file, so you
know the line was voiced.** A still mouth is now unambiguous. And it can be
fired repeatedly, on the same file, in one session — while SD is staging and
while it is not, at each `iPoseMode`, standing and seated.

**Do this before anything else:**

1. In an ordinary third-person stance with no conversation open, fire the
   command. Does the mouth move? That is the baseline, and nothing else means
   anything without it.
2. Open a conversation so SD is staging, and fire the same command again.
3. Set `iPoseMode=2` (camera dead, everything else live) and repeat.
4. Set `iPoseMode=0` and repeat.
5. Seated, repeat.

Five answers, one session, no eyeball ambiguity, no dependence on DBVO having a
pack for the line the topic list happened to offer.

**Check first, free:** `Dragonborn Voice Over/DragonbornVoiceOver/settings/selected_voice_pack.json`
currently reads `{"enabled": 0, "id": "voicebella", ...}` and has not been
written since 2026-08-01 14:02 — before the entire investigation. There is only
one copy of that file on disk and MO2's `overwrite` has no DBVO folder. If
`enabled: 0` means what it appears to mean, DBVO was not voicing the player on
this machine at all, and the maintainer-side reproduction is the §7 trap rather
than the bug. Open the DBVO MCM and confirm before trusting any local result.

---

## 5. Why the old five-run protocol could not have worked

Three defects, all now fixed in the tree:

- **`iPoseMode` was only logged when non-zero.** A mode-0 run produced no line at
  all, so "no mode line" meant either mode 0 or an ini that never loaded. The
  protocol scored three of its five runs at mode 0. Now logged unconditionally,
  with the mode named and a warning on the camera-dead modes.
- **`gazeReported` and `expressionReported` were process-lifetime `OnceFlag`s.**
  They fired once and never again, while the settings behind them are re-read
  every conversation and are changeable from the menu mid-session. Any A/B that
  flipped a Performance dial between conversations produced a log that could not
  say which conversation ran under which setting. Now reset per conversation,
  plus an unconditional settings snapshot in `Performance::Engage`.
- **The DBVO guard was circular.** The protocol said to discard any run where the
  player's phoneme peak never rose above ~0.1, on the grounds that DBVO had not
  voiced the player. But §6 concludes the phoneme channel is not the lipsync
  channel. If that conclusion holds the guard is invalid; if the guard is valid
  the conclusion falls. They cannot both stand. §4 replaces the guard with
  knowing which file you played.

---

## 6. What the instrument could and could not prove

The probe (`Scene/Performance.cpp`, `bLogFaceAnim=1`) was the basis for most of
§3-as-was. Auditing it moved several rows from "ruled out by measurement" to
"never actually measured".

**`Sample()` collapsed four different conditions into `peak = 0.0f`:** the
channel is genuinely flat, `values` is null, `count` is zero, or `count` is
absurd. `ReportFace` logged only `.peak`. So every `ph=0.000` in the logs was
ambiguous, and `readable`, `count` and `isUpdated` were computed and thrown away.

**That undermines the single most consequential conclusion in the old document** —
"`BSFaceGenAnimationData::phenomeKeyFrame` is not the channel that drives visible
lip movement", which rested entirely on "NPCs lipsync perfectly with it flat at
zero across hundreds of samples". The NPC reading it rests on is equally
consistent with the channel simply not being readable from the probe's vantage.
Combined with §2 — the NPC is on a different subsystem — that conclusion should
be treated as **withdrawn, not merely doubtful.** It is what sent §7-as-was
toward `BGShkPhonemeController`.

Other instrument defects, all now fixed:

- **`lastTime` cannot distinguish the two actors.** Measured over 145 paired
  samples it was identical between PLAYER and npc to two decimal places on every
  sample and advanced at wall-clock rate: it is a global timestamp. It froze once
  for three samples on *both* actors at once — a global pause, not a per-actor
  stall. And at 2 Hz, "advanced ~0.5" is satisfied by a node that updated thirty
  times and by one that updated once. It was read as proof the player's face node
  updates normally. It is not capable of showing that.
- **`SAME=` does not exist in this build.** The row citing "`SAME=yes` on every
  sample" refers to a token from a rewritten build with no version control behind
  it. The current probe never logged either pointer. The claim is separately
  *true* — `GetFaceNode()` is not overridden by `Actor`, `Character` or
  `PlayerCharacter`, and `TESObjectREFR::GetFaceNode` delegates to
  `GetFaceNodeSkinned` — but that is a header fact, not a measurement, and the
  comment in `Performance.cpp` asserting the two "return different pointers on
  every sample" was simply wrong.
- **"172 of 280 samples" is unauditable.** No process-side value was logged in
  the current build, and there is no history to recover the build that produced
  it from.
- **"the jaw modifier" is a guessed label on an unlabelled argmax.** `peak` is a
  maximum across the channel's slots at one instant, not over time, and the probe
  never recorded which slot won. Skyrim's FaceGen modifier set is eyes and brows;
  the jaw sits on the phoneme side. A bare max cannot tell a blink from a mouth.
- **`skinCulled` read 0 on all 290 samples** and was never once reported in this
  document. It is the one thing that row established.
- **The face node's `NiAVObject` flags were never logged** — only bit 0, off the
  skinned node. The bits that decide whether a downward pass reaches a node at
  all are the selective-update ones, and nobody looked. Note also that the
  `0x003C` / `0x001C` values this document calls "the face node's flags" are
  `BSFaceGenNiNode::RUNTIME_DATA::flags`, a different field from
  `NiAVObject::flags`, with no enum anywhere in CommonLibSSE. The document
  conflates them; the log now prints both as `fg=` and `av=`.

**§5-as-was, the gaze finding, does not survive.** "With `bGaze=0` the player's
phoneme peaks roughly doubled (0.250 → 0.550) and the jaw modifier rose
fivefold" compares single snapshots of a slowly-moving envelope. Within one
conversation, with nothing changed, the modifier ran 0.050 → 0.550 → 0.000 over
about twelve seconds. The two logs on disk pointed the *opposite* way: the
session with gaze definitely off maxed at 0.050 across 104 samples, the one with
gaze on reached 0.450. And the logs did not record which gaze setting was live
for any given run — which is defect two in §5. Every `ph` and `md` value across
both logs was an exact multiple of 0.05 while `ex` was not, which is its own
unexplained thing and probably the more interesting one.

---

## 7. Traps

**A still mouth and a line DBVO never voiced are indistinguishable by eye.**
This is what produced the retracted `NiUpdateData::time = 0.0f` "SOLVED"
conclusion, from one uncontrolled observation on a different NPC. §4 removes the
trap entirely — use it.

**A "fix" that stops the camera moving is not a fix.** Modes 1 and 2 read as
successes for hours. Always confirm the cuts still happen. (Mode 2 is still
useful — see §3 — but as a control, never as a fix.)

**The menu writes `iPoseMode` to the ini as the slider moves, and `Deploy.ps1`
preserves live ini values.** A deploy will carry a diagnostic mode forward into
what is meant to be a clean run. Check after every deploy.

**The log keeps exactly one generation back, and this cost real data.** The
face-probe samples that §6 is built on were destroyed by two ordinary game
launches on the morning of 2026-08-03; `SceneDirector.log` and
`SceneDirector.previous.log` were both overwritten before anyone copied them.
The numbers quoted in §6 are now the only record. **Archive the log immediately
after any diagnostic session.** It lives at
`C:\Users\Tobih\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\` —
OneDrive-redirected, which is why it is not where you would look for it.

**Do not try to disassemble `SkyrimSE.exe` from disk.** It is SteamStub-encrypted
(a `.bind` section is the marker); the `.text` section is ciphertext until Steam
decrypts it at runtime. An attempt to resolve `BSFaceGenNiNode::UpdateDownwardPass`
this way burned a lot of effort and produced nothing. The Address Library parses
fine and the vtable IDs resolve — it is only the code bytes that are unreadable.

---

## 8. What to do next

In order, cheapest first.

1. **Confirm DBVO is actually enabled** (§4, the `selected_voice_pack.json`
   check). Free, and it may invalidate the local reproduction outright.
2. **Run the console `SpeakSound` sweep** (§4). One session, five answers, no
   ambiguity. This is the whole of §1's old protocol for a fraction of the cost,
   and it will also say whether the bug exists outside dialogue at all — which
   nobody knows.
3. **If the bug reproduces on a bare console `SpeakSound` with SD staging and
   `iPoseMode=0`, but not at `iPoseMode=2`:** the target is a consumer that reads
   the camera transform and gates the player's face. Add a diagnostic mode that
   blends between the engine's transform and SD's by a live 0–100 dial, and sweep
   it in one session. If lipsync fails at any non-zero displacement, the gate is
   binary and positional. If it degrades with distance, it is an LOD or audibility
   threshold. That single sweep discriminates between every remaining hypothesis
   and costs one slider.
4. **Free, no hook:** read `BSFaceGenManager::GetSingleton()->numActorsToMorph`
   and `->emotions` during a conversation. The engine morphs a bounded number of
   actors per frame (`uiNumActorsAllowedToMorph`, default 10, max 64) and the
   candidate list is built by distance. If that count moves when the camera write
   lands, the question is answered without hooking anything. In a 3,632-mod
   Nolvus profile in a settlement, that budget is plausibly exhausted anyway.
5. **The hook, if it comes to it — and it is far cheaper than this document used
   to claim.** `BSFaceGenNiNode::UpdateDownwardPass` is vfunc `0x2C` and
   `RE::VTABLE_BSFaceGenNiNode` is `REL::VariantID(252410, 200333, 0x1678620)`,
   already in `Offsets_VTABLE.h`. It is a `write_vfunc` in the same shape as
   `Core/Tick.cpp`, not a signature scan. Two caveats: this build defines
   `SKYRIM_CROSS_VR`, so the declaration is compiled out of the header and the
   thunk must be a free function taking `BSFaceGenNiNode*` explicitly; and `0x2C`
   is a flat-Skyrim slot, so guard the install exactly as `SKSEPlugin_Load`
   already guards against VR. Filter to the two cached participant face nodes —
   this fires every frame for every head in the cell. Log, around the call to the
   original: `lastTime` before and after, `NiUpdateData::time` as received, and
   `phenomeKeyFrame.isUpdated` before and after. **The `isUpdated` flip is the
   decisive one** — it is set by `SetValue` and cleared by whoever consumes the
   keyframe, so watching it go true → false identifies the consumer directly,
   which is the thing nobody has established.

`BGShkPhonemeController` and `LipSynchAnimDB__LipAudioInterface` are no longer
the next stop. They were reached for because the phoneme channel had been ruled
out, and §6 withdraws that.

---

## 9. Diagnostics available

`[Diagnostics] bLogFaceAnim=1` — twice a second, for both participants. Now
reports, per channel: peak, whether it was readable at all, slot count, which
slot peaked, and the engine's `isUpdated` flag; plus process-vs-node object
identity, the facegen node flags and `lastTime`, the node's full `NiAVObject`
flags and cull bit, and posture. Driven from `Director::Tick`, deliberately, so
it still reports with `bEnabled=0` — the only way to run the IACC control.

`[Diagnostics] iPoseMode` — 0 normal, 1 no traversal (**camera dead**), 2 world
only (**camera dead — the control, see §3**), 3 `time = 0`, 4 `time = 0` and no
flags, 5 normal plus syncing `ThirdPersonState`. Logged on every conversation
open, including mode 0.

---

## 10. Shipping position

Unchanged. 1.2.1 is built and verified; every other fix from the session is in
it. The bug affects only players running a player-voice mod, and is only visible
because SD points a camera at a face vanilla never shows. Shipping with it
documented remains a legitimate answer.

What has changed is the cost of the alternative. §4 makes the next experiment a
console command rather than a session, and §3 cuts the remaining search space
down to one question: what reads the camera's final transform and treats the
player's head differently because of it.

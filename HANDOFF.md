# Scene Director — session handoff, 2026-08-09/10

Supersedes the 2026-08-08/09 handoff. That session's §4 was a long account of
trying to make hiding the topic list safe by controlling dialogue input. All of
that shipped, was tested, and has since been **deleted**. Read §2 before
reinstating any of it.

Everything here is measured or flagged as unverified.

---

## 1. Where things stand

**Version 1.2.3**, built, packaged, deployed, committed and tagged. Branch
`dialogue-fade-1.2.3` (renamed from `topic-commit-lock`, which described a
feature that no longer exists). `master` still holds 1.3.0 untouched — this is
**not** merged.

```
13ecd74  Ship 1.2.3: fade the topic list, and stay out of dialogue input
0f96af0  Add session handoff for 2026-08-08/09
d4645a3  Ship 1.3.0: stop gating input the mod has no reason to gate
```

Tag `v1.2.3`. Archive at `package/Cinematic Conversation Camera 1.2.3.zip`
(272 KB, `SKSE\Plugins\` + `SD.ini`).

**The version went DOWN from 1.3.0 on purpose.** 1.3.0's headline was dialogue
input control; every piece of it is gone. This is 1.2.2 plus a fade. Calling it
1.3.1 would advertise features the build does not have.

### Build and deploy

```
$env:VCPKG_ROOT = 'X:\vcpkg'
X:\VisualStudio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build "X:\!--- Main\Documents\Nolvus\SD\build\skyrim" --config Release --parallel 8
& "X:\!--- Main\Documents\Nolvus\SD\tools\Deploy.ps1" -SkipBuild
& "X:\!--- Main\Documents\Nolvus\SD\tools\Package.ps1" -SkipBuild
```

`pwsh` does not exist on this machine — use `&` with the path. Deploy refuses
while `SkyrimSE.exe` runs; **it printed that refusal twice tonight and the user
went and tested a stale DLL both times.** Always compare the deployed and built
timestamps before telling anyone to launch.

Deploy MERGES `SD.ini` — live values win for keys that still exist in the
shipped file, and keys removed from the shipped file are **dropped**. Verify the
key set before rewriting that file.

Logs: `C:\Users\Tobih\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\SceneDirector.log`
(Documents is redirected into OneDrive).

---

## 2. Dialogue input: built, proven, and deliberately removed

**Do not rebuild this without the user asking for it by name.** It worked. It
was removed because of how it felt, not because it failed.

Deleted outright: `src/Dialogue/Input.cpp`, `include/SD/Dialogue/Input.h`,
`ClickGuard`, `InputWatch`, the commit lock, the press-to-skip redirect, the
facegen mouth reset. Ini keys `bBlockBlindClicks`, `bSkipLineOnPress` and
`iSelectGraceMs` are gone with them. Nothing registers with `MenuControls`; no
press is intercepted.

The user's words: *"this is too much control in my opinion, it feels game
breaking"*, then *"relinquish all control on the dialogue controls."*

### What was learned on the way, which is worth keeping

**The §4 mystery is solved, and the old explanation was wrong.** The previous
handoff concluded the click reached the menu by a route outside Scaleform. It
did not. `disableInput`, `disableSelection` and `selectedIndex` are `addProperty`
ACCESSORS on the list prototype; the list reads the backing members
`bDisableInput`, `bDisableSelection`, `iSelectedIndex`. The probe reported
"member exists" because the accessor does. The write went where nothing looks.

```
addProperty("disableInput", __get__/__set__disableInput)
__set__disableInput(abFlag) { this.bDisableInput = abFlag }
onItemPress()              { if (bDisableInput) return; ... }
```

`onItemPress` dispatches with `index = iSelectedIndex` — the HIGHLIGHTED row,
never the row under the cursor. "Lands on the first option regardless of where
you clicked" was the list doing exactly what it says.

**The real gate is `DialogueMenu.bAllowProgress`**, a plain instance member.
`onItemSelect` — the single funnel every commit arrives through — opens with
`if (!bAllowProgress) return`. Writing it false was proven in play: 27 clicks,
none committed.

**Instantly Skip Dialogue NG drives the same field**, confirmed by the literal
string `_root.DialogueMenu_mc.bAllowProgress` in its DLL. It forces it *true* to
kill vanilla's debounce. That is what the memory note "the click culprit — a
layer SD cannot guard" was pointing at. Moot now, but if the lock is ever
reinstated, this is a direct conflict and ISD is in the load order.

**The live `dialoguemenu.swf` is Edge UI's.** Only two mods ship one; DBVO 1's
is disabled. `onItemSelect` is byte-identical (222-byte body) between Edge UI's
movie and a vanilla-derived one, so all of the above is vanilla UI code rather
than one replacer's invention. Disassembler: `scratchpad/swfdis.ps1`, generic
AVM1, works on any SWF.

### A line cannot be ended early from outside

Measured with a purpose-built autopsy, now removed. Stopping a line's audio does
**not** shorten the line: a 5.84s line cut at 0.89s still ended at 5.84s. Across
the whole dead window every field on the speaker's `HighProcessData` sat flat —
`voiceTimer`, `voiceTimeElapsed`, `voiceRecoveryTime`, `soundDelay` all 0.00,
`closeDialogueTimer` and `clearTalkToListTimer` constant. The only thing that
moved was `MenuTopicManager::currentTopicInfo` going null at line end, which is
the engine reporting the result rather than the clock driving it.

`voiceTimer` was the confident first guess and the log killed it. Do not try
again without a new mechanism.

Stopping the sound also does not stop the mouth — lipsync runs off the phoneme
channel on `BSFaceGenAnimationData`, not off the sound. `Reset(0, false, true,
false, false)` empties it while leaving the expression channel (SD's emotion
work) alone. That code is deleted but the fact is worth keeping.

---

## 3. The fade — rewritten on `eMenuState`, 2026-08-15

Ships **on**. Everything below this heading used to describe a machine that
reconstructed "whose turn is it" from nine proxies and animated the list against
the answer. It is gone: ~450 lines out of `Director.cpp`, which went 3793 → 3244.

**The movie publishes the state machine.** `_root.DialogueMenu_mc.eMenuState`:
`0` greeting, `1` topicList, `2` topicClicked, `3` transitioning. Phase `1` means
exactly "the options are live and are the NEW ones", because `1` is only
re-entered *after* the rebuild — the rebuild is the transitioning step. SD read
this all along via `Interface::ReadDialoguePhase` and used it for one decision.

The whole feature is now:

```
target = (phase == kTopicList) ? 100 : 0     // unreadable movie → 100, fail open
alpha  = ease(alpha → target)
```

What that closed, by construction rather than by another special case:

| Symptom | Was |
|---|---|
| list shows the answer you just picked | the early return *predicted* the rebuild from `responseCount` + a row fingerprint |
| menu arrives seconds late | its return was gated on `playerSideSince` — a held master shot pinned it |
| resets / fights itself | `choicesOnScreen` sticky across turn edges, `_visible` re-asserted against the engine's rebuild |
| flashes between responses of one reply | a gap in `npcSpeaking` read as a turn |
| dead under DBVO | `npcSpeaking` comes from `currentTopicInfo`, null for the player's whole turn. The phase sits at `topicClicked` throughout |

**Kept on purpose:** the list is never hidden before the engine has declared it
live once (`listWasLive`). Accept commits the *highlighted* topic without
consulting the cursor and SD does not gate input, so a list hidden on the
approach to an NPC is one the greeting click selects blind. Oldest bug here.

**Removed on purpose:** the 4s dead man. It forced the list back on a stopwatch,
which is how it came to fire in the middle of a four-second voiced line. A
stopwatch cannot tell a stuck movie from a long monologue, so nothing replaced
it — the `Menu phase:` log line is the instrument. Also gone:
`Director::ChoicesVisible` / `ChoicesVisibleFor`, dead since the input guard was
removed but still maintaining a 95-alpha threshold for a caller that no longer
existed.

`Interface::ReadTopicListFingerprint` is now unused. Left in place.

**`_visible` is pulled at alpha 1.5, not 95.** That removes an object from
RENDERING as well as hit testing, so at 95 the ease was only ever seen across its
top five percent and read as a cut in both directions. 95 was correct for the
deleted input guard and for nothing else.

### Dials

All four are output-side only and cannot change *when* the options are live:
`iChoiceFadeDelay` 2.40s (hold before the spent list starts going),
`iChoiceFadeTime` 0.25s (how long it takes to go), `iListReturnDelay` 0.20s
(hold before it eases back after the engine rebuilds it), `kChoiceFadeSeconds`
0.55s (the ease in, still a constant). `bHideChoicesWhilePlayerSpeaks` off holds
the spent list up until the player's own line ends — the only place a sound
handle is still read, and it delays an output rather than deciding one.

---

## 4. Lipsync — §5's foundation is falsified

**See the CORRECTION block at the top of `LIPSYNC.md`.** In short: the claim that
nothing writes the player's phoneme channel is wrong. Measured twice tonight at
`iPoseMode=0` and `iPoseMode=5`, mouth not moving either time, the channel is
alive in both — `up=1` appears on the player dozens of times per conversation,
and the value moves across slots 4, 5 and 7 rather than sitting frozen at 0.050
in slot 4.

Consequences:

- The failure is **downstream** of the channel. Visemes are written and drained
  while the mouth does not move.
- The `lipsync-1.2.2` branch was solving a problem that does not exist.
- **`iPoseMode=5` is struck off.** Syncing the engine's own camera record changes
  nothing, so the stale-record theory — attractive, and the one the code comments
  still point at — is dead. Mode-5 log preserved at
  `scratchpad/posemode5-run.log`.

`culled` still reads 0 on every sample and `bHoldPlayerFace` still does nothing;
the visibility theories stay dead.

---

## 5. Config state

Shipped `config/SD.ini` was rewritten: 33.8k → 6.9k, every comment cut to a line
or two. Key set and order verified identical before deploying, because Deploy
drops keys the shipped file no longer has. Keys read once at startup are now
marked `(restart)` — the header promised everything was live at the next
conversation and that was false for ten of them.

Shipped defaults: `bFadeTopicList=1`, `bLogFaceAnim=0`, `iPoseMode=0`,
`iOpenShotHold=0`.

**The user's live ini currently has `bLogFaceAnim=1`** from the lipsync testing.
It writes two lines every half second. Turn it off.

The user's live ini is heavily hand-tuned and not representative: `iLensBias=10`,
`iLetterboxHeight=68`, `iDollyAmount=26`, most shots disabled. Applying a preset
resets the shot table.

DBVO 2's `default_options.json` was restored to its shipped state earlier in this
session; `hide_dialogue_tree` is off and the file is inert again.

---

## 6. Open threads

- **The flash on NPC finish.** §3. Known, shipped, has a candidate fix.
- **Lipsync is downstream of the phoneme channel.** §4. This is the live
  question and the search space has moved.
- **`README.md` was rewritten** — its "One thing this mod deliberately does not
  do" section said the list never fades and told people to get it from their
  voice mod. Re-read before it becomes the Nexus description.
- **`DISPLAY_NAME` is "Scene Director"** in `cmake/Plugin.h.in` while the
  package, README and Nexus name are all *Cinematic Conversation Camera*. The
  log banner and MO2 comment say the wrong one.
- **`master` is not merged.** 1.2.3 lives on `dialogue-fade-1.2.3`.
- **`iOpenShotHold=600`** — never run. Tests whether SD takes the camera early
  enough to be what the on-camera decision reads. Cheap, and now one of the few
  camera-side lipsync experiments left.
- **`kChoiceFadeDelay` / `kChoiceReturnDelay` are hardcoded.**

---

## 7. Process note

The pattern from the last handoff repeated exactly: confident explanations
derived from reading the surrounding code, each disproven by the next log. The
fade alone took four wrong diagnoses, and the one that finally landed came from
a temporary transition log that named which branch moved the list — not from
reading the branches.

What worked, every time: add an instrument, run one conversation, read the
timestamps. What did not: reasoning about what the code must be doing.

Two of tonight's test cycles were wasted entirely because a build was made while
Skyrim was running, the deploy refused, and the user tested an unchanged DLL.
Check the deployed timestamp against the built one before saying "go test".

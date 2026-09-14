# Controls and features

## Camera

Choose Standard, Close or Room, then adjust individual shots. There are 39 angles,
each with its own field of view, movement and frequency. Three slots store custom
presets. Disabled shots and shots with zero frequency stay excluded during opening,
obstruction recovery and menu resume.

Change angles after a number of dialogue lines, on a timer, or with both.
Timers have separate settings for speaking and choosing a reply.
Camera effects include push, pull, crane, tilt, drift, orbit, slide and zoom.

## Settings

Open **Cinematic Conversation Camera** in SKSE Menu Framework. Pages cover
Presets, Camera, Shots, Screen, Faces, Keys and About. There is no MCM.

Defaults are in `Data/SKSE/Plugins/SD.ini`. Menu changes save to `SD_user.ini`
and override the corresponding defaults. Updates replace SD.ini; keep SD_user.ini.
Settings marked `(restart)` need a game restart.

| Setting | Default |
|---|---|
| Preset | Close |
| Direct the Camera | On |
| Return to First Person Afterwards | On |
| Keep Subject Visible | Off |
| First-Person Fallback | On, with subject protection |
| Per Line Angle Change | Every 3–6 eligible lines |
| Ignore Short Lines | On |
| Timers | Off |
| Black Bars | On, 12% |
| Fade Out Dialogue | On |
| Hide NPC Name | On |
| Fade After PC Line | On, 1.5 s delay, 2 s fade |
| Lip Sync Fallback | On |
| Responsive Expressions | On |
| Lighting | Off |

## Obstructions

**Keep Subject Visible** checks the face from the camera position and looks for
a clear enabled shot. Set `[Direction] bKeepSubjectVisible=1` and
`bHoldPlacement=0`, or enable it under Camera > Framing.

**First-Person Fallback** keeps dialogue running in first person when no suitable
shot is available. Cinematic coverage returns after the view stays clear.
With fallback off, the current enabled shot may remain obstructed. If no valid
pose remains, the normal game camera takes over.

**Ignore Obstructions Mid-Shot** checks placement when a shot starts, then holds
it. Moving characters and objects can cross the frame; a moving conversation
can clip. It is mutually exclusive with Keep Subject Visible. If both INI keys
are enabled, `bHoldPlacement` takes priority.

Obstruction adjustments stay within 12 degrees of the intended angle, in
3-degree steps. Selected orbit effects still move normally. Subject protection
checks actors regardless of Avoid Framing Bystanders.

Detection uses collision geometry and approximate actor shapes. Collisionless
objects and transparent foliage may behave differently from their appearance.
Subject protection still needs in-game testing with SmoothCam and Improved Camera.

## Dialogue and menus

Dialogue options stay visible through the greeting and first choice. After that,
**Fade Out Dialogue** hides them during speech and brings them back for a reply.
Faded options remain clickable. Turn the setting off to keep them visible.

**Fade After PC Line** waits for the player's voiced line to end before fading.
ReVoiced reports line endings and skips. DBVO timing uses player sound handles
with a 15-second timeout.

Inventory, barter, crafting and other menus suspend the camera presentation.
Closing the menu resumes the conversation and selects a new shot if needed.

## Player voice and expressions

DBVO 1, DBVO 2 and Dragonborn ReVoiced are detected automatically. A voice mod
is optional. ReVoiced 1.4.4 or newer reports voice timing directly and supplies
player lip sync. DBVO uses recording lookup and the optional Lip Sync Fallback.

**Responsive Expressions** adds brow and squint movement while speaking and
facial reactions while listening. Player dialogue uses English text cues; NPC
dialogue uses the game's emotion data. Voice tone and sarcasm are not analyzed.
Timing is estimated across phrases, without word-level audio alignment.

Strength is fixed. Profiles can be changed in `Expression.*` INI sections.
Speaking leaves mouth lip sync, blinking and gaze to their existing systems.
Listening expressions yield when the player speaks.

Eye & Cheek Detail needs a matching regional asset. The tested asset uses the
High Poly Head female mesh and is not bundled. Other heads retain native
brow and squint movement. Developer details are in EXPRESSION-PROFILES.md
in the source download.

## Compatibility

Keep SmoothCam enabled. The mod takes control during dialogue and returns it
afterward.

For Improved Camera in first person, set `bScripted=0` under `[EVENTS]` in
`SKSE/Plugins/ImprovedCameraSE/Profiles/Default.ini`, or your active profile.
This affects all of Improved Camera's scripted forced-third-person events.
Third-person players do not need the change.

Disable Improved Alternate Conversation Camera, Alternate Conversation Camera,
Switch Camera During Dialogue, and Camera Noise or other camera-shake mods.
Detected conflicts are listed in SceneDirector.log.

Compatible mods include True Directional Movement, No Camera Collision, ENB,
TrueHUD, Compass Navigation Overhaul, moreHUD, SkyHUD, Infinity UI and SkyUI.

## Lighting

Experimental and off by default. Set `[Lighting] bLights=1` to enable it.
There is no lighting page in the panel, and presets leave it off.

## Troubleshooting

Read `Documents/My Games/Skyrim Special Edition/SKSE/SceneDirector.log`.
It records camera cuts, available space, conflicts and conversation setup time.

Version 1.4.6 reuses cached settings while INI files are unchanged. File edits
apply at the next conversation; menu changes invalidate cached values.
In-game timing still needs testing. The reported exit/re-entry dialogue control
lock remains unresolved.

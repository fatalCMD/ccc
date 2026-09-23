# Cinematic Conversation Camera 1.4.8

## Changes

- True 180 Rule (Camera > Framing): you are filmed over one shoulder and they
  are filmed over the opposite one, so the camera stays on one side of the
  conversation. Turns on Never Cross The Eyeline.
- Reaction Shots (Camera > Reaction Shots): after every N of their lines in a
  reply, a chance that the next line is shown on you. Skips short and
  full-intensity lines.
- Stay On You After You Speak (Camera > Holds): how long the camera stays on
  you after a voiced player line before cutting to them.
- The Per Line Angle Change count now resets with every reply, including a
  reply to a topic picked while they were still talking.
- Restore Defaults also resets Never Cross The Eyeline and the new settings.
- TrueHUD: the Recent Loot list stays visible during conversations, so items
  received from an NPC show up. Its bars and other widgets are still hidden.

All new options ship off or at 0, so existing setups behave as before.

## Install

Close Skyrim and replace the old version through MO2 or Vortex.
Keep SD_user.ini and any separately installed regional face asset.

The mod ZIP contains the DLL, default INI and documentation.
The Source.zip is for building and modifying the mod.

## Checks

The Release build and all 17 tests pass. The DLL version is 1.4.8.0.
The reported dialogue exit/re-entry control lock remains unresolved.

## Publish

Upload the mod ZIP and matching complete Source.zip together.
Apply the prepared Nexus text and GPL permissions in [LICENSING.md](../LICENSING.md).

Archive hashes are in the adjacent .sha256 files. SOURCE-MANIFEST.json lists
source file hashes.

# Cinematic Conversation Camera 1.4.6

## Changes

- Cache settings between conversations while INI files are unchanged.
- Load expression profiles once when a conversation starts.
- Log conversation setup time.
- Include GPL notices, build instructions and the matching complete source ZIP.

File edits apply at the next conversation. Menu changes clear the cache.
Settings precedence stays the same.

## Install

Close Skyrim and replace the old version through MO2 or Vortex.
Keep SD_user.ini and any separately installed regional face asset.

The mod ZIP contains the DLL, default INI and documentation.
The Source.zip is for building and modifying the mod.

## Checks

The Release build and all 16 tests pass. The DLL version is 1.4.6.0.
In-game performance testing and an independent clean source build are still needed.
The reported dialogue exit/re-entry control lock remains unresolved.

Check first and repeated conversations, INI edits, menu changes and barter resume.
Compare setup times in SceneDirector.log.

## Publish

Upload the mod ZIP and matching complete Source.zip together.
Apply the prepared Nexus text and GPL permissions in [LICENSING.md](../LICENSING.md).
The live page and game installation are unchanged.

Archive hashes are in the adjacent .sha256 files. SOURCE-MANIFEST.json lists
source file hashes. Previous same-version archives are kept in backups.

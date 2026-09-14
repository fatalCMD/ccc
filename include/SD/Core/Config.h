#pragma once

#include "SD/Core/SettingsCache.h"

namespace SD::Config
{
	// Check the three INI revisions once, then reuse native read results for
	// this batch. Nested batches share the snapshot. Menu writes invalidate it.
	class ReadScope
	{
	public:
		ReadScope();
	private:
		SettingsCache::Scope scope;
	};
	// An absolute path to something under the game's Data, for the profile API.
	//
	// Exported because SD's own three ini files are no longer the only ones read
	// under Data: Compat::ImprovedCamera reads ANOTHER mod's config at load, so it
	// can name that mod's one incompatible setting rather than leave the player to
	// discover it in a conversation.
	//
	// Goes through the same exe-derived root the settings do, and returns wide, for
	// the reasons Config.cpp gives at length — a relative path sends the profile
	// API to the WINDOWS directory, and a narrowed one mangles any install path
	// the user's ANSI codepage cannot represent. Both failures are silent. Empty
	// in, or an unknowable root, gives empty out; callers check.
	[[nodiscard]] std::wstring DataPath(std::wstring_view a_relative);

	// Settings, read from whichever source is actually present.
	//
	// MCM Helper writes the player's choices to Data/MCM/Settings/SceneDirector.ini
	// and is consulted first. When it is absent — or has no value for a key yet —
	// the mod's own SKSE/Plugins/SD.ini answers instead.
	//
	// The point of the order is that neither is a dependency: Scene Director ships
	// an MCM for people who have MCM Helper and a plain ini for people who do not,
	// and behaves identically either way.
	[[nodiscard]] int  Int(const char* a_section, const char* a_key, int a_default);
	[[nodiscard]] bool Bool(const char* a_section, const char* a_key, bool a_default);

	// Text values. Only one setting needs these — the preset an ini-only player
	// asks for by name — but a number could not carry it: preset keys are a
	// shipped contract and an ordinal would silently point at a different preset
	// the moment the list was reordered.
	[[nodiscard]] std::string String(const char* a_section, const char* a_key, const char* a_default);

	// Persist a value to SD_user.ini. Used by the in-game menu so a change survives the
	// session; the live effect is applied separately and immediately, because
	// waiting for the next conversation to re-read the file would make a slider
	// feel broken.
	void SetInt(const char* a_section, const char* a_key, int a_value);
	void SetBool(const char* a_section, const char* a_key, bool a_value);
	void SetString(const char* a_section, const char* a_key, const char* a_value);

	// Logs which source answered, once, so a setting that appears to do nothing
	// can be traced to the file it was actually read from.
	void ReportSource();
}

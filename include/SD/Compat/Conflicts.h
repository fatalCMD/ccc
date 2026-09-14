#pragma once

namespace SD::Compat
{
	// Names the mods that will stop Scene Director working, at startup, before the
	// player has talked to anyone.
	//
	// Written after a test run produced "nothing is happening" and a log line
	// reading "camera already held by another plugin (owner 6)". That was correct
	// behaviour and useless information: it cost a full game launch to discover
	// that handle 6 was AlternateConversationCamera.dll. A conflict that can be
	// detected at load should be reported at load, by name.
	void ReportKnownConflicts();
}

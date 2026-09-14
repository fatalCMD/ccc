#pragma once

namespace SD::Menu
{
	// The in-game settings panel, drawn through SKSE Menu Framework.
	//
	// A soft dependency, and deliberately so. The framework header resolves every
	// entry point through GetProcAddress against a module that may not be loaded,
	// and IsInstalled() checks for the DLL on disk before anything is registered —
	// so a load order without the framework gets a plugin that behaves exactly as
	// it did before, with SD.ini as its only interface. Nothing here is linked, so
	// there is no import to fail at load.
	//
	// That matters more than usual for this mod: Scene Director now runs on every
	// runtime from SE 1.5.97 to AE 1.6.1170, and a hard dependency on a framework
	// with narrower coverage would give that away for a settings menu.
	//
	// SD.ini remains the source of truth. The menu writes to it and applies the
	// change live; it does not hold state of its own.
	class Settings
	{
	public:
		// Safe to call unconditionally. Does nothing when the framework is absent.
		static void Register();
	};
}

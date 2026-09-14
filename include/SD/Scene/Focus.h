#pragma once

namespace SD::Scene
{
	// Depth of field, without shipping a plugin.
	//
	// Engine DoF needs a TESImageSpaceModifier form, which normally means bundling
	// an ESL. It does not have to: Skyrim.esm already contains one built for the
	// purpose — VATSImodDOF, 0x00035301, a leftover of a feature Skyrim never
	// shipped. It is present in every install by definition, so it can simply be
	// borrowed.
	//
	// The key light alone could not do this. Falloff darkens what the light fails
	// to reach, but a tavern is already lit by its own lamps, so the background
	// stayed sharp and fully bright. Separation needs the blur.
	class Focus
	{
	public:
		static void Engage();
		static void Release();
		static void Configure(bool a_enabled, float a_strength);

		[[nodiscard]] static bool Available();
	};
}

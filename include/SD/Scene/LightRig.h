#pragma once

namespace SD::Scene
{
	// FOUR LOOKS AND TWO DIALS, and the reason this is the second design.
	//
	// The first one was a full gaffer's kit: ten rigs, three lamps each, seven
	// values per lamp, all of it editable from the ini and from a settings page,
	// plus a table of what every authored emotion did to whichever rig was
	// running. Two hundred and fifty keys. It worked, and it was unusable —
	// somebody who wants their conversations lit does not want to be asked what
	// azimuth the fill sits at.
	//
	// The mistake was assuming the depth was the feature. It was not: the feature
	// is that faces stop looking flat, and every knob past the point where that
	// happens is a knob that makes the mod harder to turn on.
	//
	// So the lamps are still here and the geometry is still authored — that part
	// was never the problem, and it is what stops this looking like a torch taped
	// to the camera. It is simply no longer anybody's business. What is left on
	// the surface is: on, how bright, what colour, and which of four looks.
	enum class Lamp : std::uint8_t
	{
		// The light that models the face.
		kKey = 0,

		// Lifts the shadow the key leaves.
		kFill,

		// Behind the subject, edging the hair and the shoulder. The lamp that
		// produces separation from the background — a key alone only darkens what
		// it fails to reach, and in a lit tavern it fails to reach nothing.
		kRim,

		kCount
	};

	inline constexpr std::size_t kLampCount = static_cast<std::size_t>(Lamp::kCount);

	// ONE LAMP'S PLACE IN A LOOK. Geometry and relative strength only.
	//
	// NO COLOUR HERE ANY MORE. Every lamp used to carry its own temperature and
	// tint, which is how a real rig works and is also two more numbers per lamp
	// that nobody was ever going to set. Colour is now one global dial, applied
	// across the look — see KeyLight::Configure.
	struct LampSpec
	{
		// 0-300, per cent, RELATIVE to the player's brightness dial.
		//
		// ZERO IS NOT DIM, IT IS ABSENT. A lamp at zero is never created, which is
		// what lets a look be defined by what it leaves out — Hard has no fill,
		// and that omission is the whole of what makes it hard.
		int intensity;

		// World units the light reaches, 60-1200. The softness control whatever it
		// is called: a small radius falls off fast and the shadow edge is hard.
		int radius;

		// Degrees around the subject, measured from the camera. 0 sits at the lens
		// and flattens the face; 180 is directly behind, where a rim belongs. The
		// sign picks a side, and KeyLight flips it with the camera so a key stays
		// on the same side of the frame through a cut.
		int azimuth;

		// Degrees above the subject's eye line, -60 to 80.
		int elevation;

		// Per cent of the camera-to-subject distance, 10-200. Relative rather than
		// absolute so a look lights a close-up and a master the same way instead
		// of blowing out one of them.
		int distance;
	};

	// A NAMED LOOK. Four of them, plus Off.
	//
	// They differ by SHAPE and nothing else — how hard the key is, whether there
	// is a fill, how much edge. They deliberately do not differ by colour, because
	// colour is a dial the player already has and two ways to set one thing is
	// how the last version got away from itself.
	struct LookSpec
	{
		// The ini token, and a contract once shipped. A shot stores this string
		// rather than an ordinal so the table can be reordered or extended without
		// silently relighting everything.
		const char* key;

		const char* name;     // menu label
		const char* summary;  // one line, under the label

		LampSpec lamps[kLampCount];
	};

	[[nodiscard]] std::span<const LookSpec> AllLooks();

	// Index into AllLooks, or -1. Case-insensitive: this is read from a file
	// people hand-edit.
	[[nodiscard]] int FindLook(std::string_view a_key);

	// What an angle falls back to when its key is missing or names a look that no
	// longer exists. Never -1.
	[[nodiscard]] int DefaultLook();

	// Three 0-255 channels to the colour the engine wants.
	//
	// WHAT THIS REPLACED: a single "warmth" dial running 0 cold to 100 warm
	// through a hand-written tungsten curve. It was one control and it was the
	// right number of controls — it was simply the wrong control. A curve can only
	// offer the colours somebody thought to put on it, so amber and pale blue were
	// reachable and the green of a wisp, the sickly cast of a Dwemer hall and
	// anything else with a hue were not.
	//
	// A colour picker is not more complicated than a slider. It is the same one
	// widget, and everybody has already used one.
	//
	// NOTE THE CEILING. These are the light's DIFFUSE, which is a colour and not a
	// quantity — how much light there is comes from the brightness dial, which
	// drives fade separately. So a channel saturates at 255 and pushing all three
	// there gives white, not a brighter light.
	[[nodiscard]] RE::NiColor ColourFrom(int a_red, int a_green, int a_blue);
}

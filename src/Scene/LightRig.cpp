#include "SD/Scene/LightRig.h"

#include <cctype>

namespace SD::Scene
{
	namespace
	{
		// THE FOUR LOOKS, PLUS OFF.
		//
		// They differ by SHAPE and by nothing else. There is no warm look and no
		// cold one, because colour is a dial the player already has, and a list
		// that mixes "how hard is the light" with "what colour is it" is a list
		// where half the entries are the same entry twice.
		//
		// A look is defined as much by the lamp it OMITS as by the ones it runs.
		// Hard has no fill and Edge has no key, and softening either would produce
		// something that merely resembles it — which is how a short list stops
		// being worth choosing from.
		//
		// ORDER IS NOT STORAGE. A shot stores the `key` string, so this table can
		// be reordered or extended freely; only a rename breaks an existing config.
		constexpr LookSpec kLooks[] = {
			//                    intensity, radius, azimuth, elevation, distance
			{ "off", "Off", "No added light. The room lights the scene, as the game always did.",
				{ { 0, 400, 34, 20, 68 },
					{ 0, 560, -55, 4, 92 },
					{ 0, 300, 155, 34, 60 } } },

			{ "natural", "Natural", "A gentle lift on the face. Safe anywhere, and hard to notice.",
				{ { 85, 440, 34, 20, 68 },
					{ 34, 560, -55, 4, 92 },
					{ 18, 300, 155, 34, 60 } } },

			{ "soft", "Soft", "Broad and forgiving. Low contrast, flattering.",
				{ { 110, 490, 32, 18, 65 },
					{ 62, 600, -58, 0, 95 },
					{ 34, 310, 155, 34, 60 } } },

			{ "hard", "Hard", "One strong side light and nothing filling the shadow.",
				{ { 155, 300, 48, 28, 55 },
					{ 0, 520, -55, 0, 95 },
					{ 32, 260, 160, 40, 55 } } },

			{ "edge", "Edge only", "No key at all. Shape from behind, face left in shadow.",
				{ { 0, 300, 45, 25, 60 },
					{ 14, 620, -60, 0, 100 },
					{ 130, 320, 172, 34, 55 } } },
		};

		constexpr std::size_t kLookCount = std::size(kLooks);
	}

	std::span<const LookSpec> AllLooks()
	{
		return std::span<const LookSpec>{ kLooks, kLookCount };
	}

	int FindLook(std::string_view a_key)
	{
		if (a_key.empty()) {
			return -1;
		}

		for (std::size_t i = 0; i < kLookCount; ++i) {
			const std::string_view candidate{ kLooks[i].key };
			if (candidate.size() != a_key.size()) {
				continue;
			}

			// ASCII-only on purpose. Look keys are ASCII by construction, and
			// std::tolower on a signed char above 0x7F is undefined — a
			// hand-edited file is exactly where such a byte turns up.
			bool match = true;
			for (std::size_t c = 0; c < candidate.size(); ++c) {
				const auto lhs = static_cast<unsigned char>(candidate[c]);
				const auto rhs = static_cast<unsigned char>(a_key[c]);
				if (std::tolower(lhs) != std::tolower(rhs)) {
					match = false;
					break;
				}
			}
			if (match) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	int DefaultLook()
	{
		// Natural rather than Off, and that is a decision rather than a fallback.
		// An angle whose key is missing has never been touched, and the honest
		// answer to "what should an untouched angle look like" is the look that
		// was authored to be safe everywhere — not one that quietly switches the
		// feature off for anybody who has not been through the list.
		const int natural = FindLook("natural");
		return natural >= 0 ? natural : 0;
	}

	RE::NiColor ColourFrom(int a_red, int a_green, int a_blue)
	{
		// Stored 0-255 and used 0-1, because 0-255 is what a colour picker hands
		// back and what anybody reading the ini expects to see. There is no curve
		// here and there should not be one: whatever the picker showed is what the
		// lamp gets.
		return RE::NiColor{
			std::clamp(a_red, 0, 255) / 255.0f,
			std::clamp(a_green, 0, 255) / 255.0f,
			std::clamp(a_blue, 0, 255) / 255.0f
		};
	}
}

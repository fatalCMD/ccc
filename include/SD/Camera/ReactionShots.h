#pragma once

#include <algorithm>
#include <cstdint>

namespace SD::Camera
{
	// Occasionally shows one of the NPC's lines on the player instead.
	//
	// Counts eligible NPC lines within a reply. After `every` of them it rolls
	// `chance`; on a hit the next eligible line becomes the reaction. The
	// Director resets this at the start of each reply and when the NPC's turn
	// ends. Only the camera is affected: npcSpeaking is left alone, so lip sync
	// and expressions still follow the real speaker.
	class ReactionShots
	{
	public:
		struct Settings
		{
			bool enabled{ false };
			int  every{ 3 };    // eligible NPC lines before a roll
			int  chance{ 50 };  // percent
		};

		void Reset() noexcept
		{
			lines = 0;
			owed = false;
			active = false;
		}

		// Call on every NPC line.
		//   a_eligible:  not a short line. Short lines neither count nor become reactions.
		//   a_takeable:  may be the reaction. False for full-intensity lines, which
		//                get the NPC close-up; a pending reaction waits for the next line.
		//   a_automatic: framing is automatic. Manual framing drops a pending reaction.
		//   a_roll:      random value in [0, 100).
		void OnNpcLine(const Settings& a_settings, bool a_eligible, bool a_takeable,
			bool a_automatic, std::uint32_t a_roll) noexcept
		{
			// A reaction only lasts one line.
			active = false;

			if (!a_settings.enabled || !a_automatic) {
				owed = false;
				return;
			}
			if (!a_eligible) {
				return;
			}
			if (owed) {
				if (a_takeable) {
					owed = false;
					active = true;
					lines = 0;
				}
				return;
			}
			if (++lines >= std::max(a_settings.every, 1)) {
				lines = 0;
				owed = static_cast<int>(a_roll % 100u) < a_settings.chance;
			}
		}

		// The current NPC line is being shown on the player.
		[[nodiscard]] bool Active() const noexcept { return active; }

		// The next eligible NPC line will be.
		[[nodiscard]] bool Owed() const noexcept { return owed; }

	private:
		int  lines{ 0 };
		bool owed{ false };
		bool active{ false };
	};
}

#pragma once

#include <cstdint>

namespace SD::Camera
{
	// Camera-only anticipation. Never declares the NPC to be speaking or changes
	// dialogue readiness. A voice must start in this staging session to arm it.
	class PlayerVoiceHandoff
	{
	public:
		void Reset(std::uint64_t a_serial) noexcept
		{
			serial = a_serial;
			phase = Phase::kIdle;
		}

		void Update(std::uint64_t a_serial, bool a_playerSpeaking,
			bool a_npcSpeaking, bool a_choosing) noexcept
		{
			if (a_serial != serial) {
				serial = a_serial;
				phase = a_playerSpeaking ? Phase::kPlayer : Phase::kIdle;
			} else if (phase == Phase::kPlayer && !a_playerSpeaking) {
				phase = Phase::kAwaitingReply;
			}

			if (phase == Phase::kAwaitingReply && a_npcSpeaking) {
				phase = Phase::kReply;
			} else if (phase == Phase::kReply && !a_npcSpeaking) {
				phase = Phase::kIdle;
			}
			// A topic with no answer (or a cancelled line) must not strand the
			// camera on the NPC once the game returns control to the topic list.
			if (!a_playerSpeaking && !a_npcSpeaking && a_choosing) {
				phase = Phase::kIdle;
			}
		}

		[[nodiscard]] bool Active() const noexcept
		{
			return phase == Phase::kAwaitingReply || phase == Phase::kReply;
		}

	private:
		enum class Phase { kIdle, kPlayer, kAwaitingReply, kReply };
		std::uint64_t serial{ 0 };
		Phase phase{ Phase::kIdle };
	};
}

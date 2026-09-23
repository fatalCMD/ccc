#pragma once

#include <cstdint>

namespace SD::Camera
{
	// Detects the start of a new NPC reply, where the per-reply counters reset.
	//
	// npcSpeaking alone isn't enough. It's debounced by 0.4s, so if the player
	// picks a topic while the NPC is still talking (or just after), it never
	// drops and the answer looks like a continuation. Topic picks are counted
	// separately and checked on the next NPC line.
	class ReplyBoundary
	{
	public:
		// The tick and the line hook run in no fixed order, so the click that
		// started a reply can be seen just after the reply's first line. Picks
		// this soon after a reply starts are ignored.
		static constexpr float kLateEdgeSeconds = 0.5f;

		void Reset() noexcept
		{
			picks = 0;
			seen = 0;
			sinceStart = kLateEdgeSeconds;
		}

		void Advance(float a_delta) noexcept
		{
			if (a_delta > 0.0f && sinceStart < kLateEdgeSeconds) {
				sinceStart += a_delta;
			}
		}

		// Menu click or voiced player line starting. Both may fire for the same
		// pick; that's harmless.
		void OnPick() noexcept
		{
			if (sinceStart >= kLateEdgeSeconds) {
				++picks;
			}
		}

		// Call on every NPC line. Returns true if it starts a new reply.
		[[nodiscard]] bool OnLine(bool a_alreadySpeaking) noexcept
		{
			const bool fresh = !a_alreadySpeaking || picks != seen;
			if (fresh) {
				seen = picks;
				sinceStart = 0.0f;
			}
			return fresh;
		}

	private:
		std::uint32_t picks{ 0 };
		std::uint32_t seen{ 0 };
		float         sinceStart{ kLateEdgeSeconds };
	};
}

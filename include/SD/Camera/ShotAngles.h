#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace SD::Camera::ShotAngles
{
	// Collision avoidance may adjust a composition, but cannot turn a portrait
	// into a profile. Deliberately selected orbit effects supply their own base.
	inline constexpr float kMaxAdjustment = 12.0f;
	inline constexpr float kLineCeiling = 165.0f;
	inline constexpr std::array kOffsets{ 0.0f, 3.0f, -3.0f, 6.0f, -6.0f, 9.0f, -9.0f, 12.0f, -12.0f };
	using Candidates = std::array<float, kOffsets.size()>;

	[[nodiscard]] inline bool AllowedAdjustment(float offset)
	{
		// Subtracting the moving nominal bearing can add a few float ULPs.
		return std::isfinite(offset) && std::abs(offset) <= kMaxAdjustment + 0.0001f;
	}

	// Intersect the shot's small adjustment window with the continuity limits.
	// Clamping to the line must never move a candidate outside that window.
	[[nodiscard]] inline std::size_t MakeCandidates(
		float angle, float lineFloor, bool enforceLine, Candidates& out)
	{
		if (!std::isfinite(angle) || (enforceLine && !std::isfinite(lineFloor))) {
			return 0;
		}
		const float low = enforceLine ? std::max(angle - kMaxAdjustment, lineFloor) : angle - kMaxAdjustment;
		const float high = enforceLine ? std::min(angle + kMaxAdjustment, kLineCeiling) : angle + kMaxAdjustment;
		if (low > high) {
			return 0;
		}
		std::size_t count = 0;
		for (const auto offset : kOffsets) {
			const float candidate = std::clamp(angle + offset, low, high);
			bool duplicate = false;
			for (std::size_t i = 0; i < count; ++i) {
				if (std::abs(out[i] - candidate) < 1.0f) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) {
				out[count++] = candidate;
			}
		}
		return count;
	}
}

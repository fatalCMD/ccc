#pragma once

#include <cmath>
#include <optional>
#include <span>

namespace SD::Camera
{
	template <typename Type, typename Eligible>
	[[nodiscard]] bool ReusableShot(bool valid, Type recorded, Type current, Eligible eligible)
	{
		return valid && recorded == current && eligible(recorded);
	}

	// Eligibility is a hard gate, independent of placement quality. A failed
	// search has no implicit default shot, even when every option is off.
	template <typename Type, std::size_t Extent, typename Eligible, typename Score>
	[[nodiscard]] std::optional<Type> BestAvailable(
		std::span<const Type, Extent> pool, Eligible eligible, Score score)
	{
		std::optional<Type> best;
		float bestScore = -1.0f;
		for (const auto type : pool) {
			if (!eligible(type)) {
				continue;
			}
			const float value = score(type);
			if (std::isfinite(value) && value >= 0.0f && value > bestScore) {
				best = type;
				bestScore = value;
			}
		}
		// Settings can change while an expensive placement search is in flight.
		return best && eligible(*best) ? best : std::nullopt;
	}
}

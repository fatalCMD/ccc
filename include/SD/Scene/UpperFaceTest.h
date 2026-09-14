#pragma once

#include "SD/Scene/UpperFace.h"

namespace SD::Scene::UpperFaceTest
{
	// Three seconds at neutral, then eight isolated two-second holds separated
	// by one-second neutral gaps. No recipes, gain, smoothing or regional layer.
	inline constexpr float kDuration = 27.0f;
	inline constexpr std::array<std::size_t, 8> kOrder{ 4, 5, 0, 1, 2, 3, 6, 7 };
	inline constexpr std::array<const char*, 8> kNames{
		"BrowDownLeft", "BrowDownRight", "BrowInLeft", "BrowInRight",
		"BrowUpLeft", "BrowUpRight", "SquintLeft", "SquintRight"
	};

	struct Frame
	{
		UpperFace::Shape values{};
		int phase{ -1 };
		int index{ -1 };
		bool active{};
	};

	[[nodiscard]] inline Frame Sample(float elapsed)
	{
		Frame result;
		if (!std::isfinite(elapsed) || elapsed < 0 || elapsed >= kDuration) return result;
		result.active = true;
		result.phase = 0;
		if (elapsed < 3.0f) return result;
		const float time = elapsed - 3.0f;
		const auto step = static_cast<std::size_t>(time / 3.0f);
		const bool hold = time - static_cast<float>(step) * 3.0f < 2.0f;
		result.phase = 1 + static_cast<int>(step) * 2 + (hold ? 0 : 1);
		if (hold) {
			result.index = static_cast<int>(kOrder[step]);
			result.values[kOrder[step]] = 1.0f;
		}
		return result;
	}

	// Restore only values we still own, and remember intervening external writes.
	// The engine adapter additionally binds this to the exact node/data/buffer.
	struct OwnedPose
	{
		UpperFace::Shape baseline{}, last{};
		bool owned{};

		void Apply(const UpperFace::Shape& live, const UpperFace::Shape& target)
		{
			for (std::size_t i = 0; i < live.size(); ++i)
				if (!owned || std::abs(live[i] - last[i]) > .0001f) baseline[i] = live[i];
			last = target;
			owned = true;
		}
		[[nodiscard]] UpperFace::Shape Release(UpperFace::Shape live)
		{
			if (owned) for (std::size_t i = 0; i < live.size(); ++i)
				if (std::abs(live[i] - last[i]) <= .0001f) live[i] = baseline[i];
			*this = {};
			return live;
		}
	};
}

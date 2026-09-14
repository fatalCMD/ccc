#pragma once

#include "SD/Scene/ExpressionModel.h"

namespace SD::Scene::Brows
{
	// Fixed-size morph storage/smoothing. Runtime pose authorship belongs to
	// ExpressionProfiles. No speech gestures or emotion recipes live here.
	inline constexpr std::size_t kSlots = 8;
	using Shape = std::array<float, kSlots>;

	// Exact critically damped step. A moving target may cross the current pose:
	// snapping to that target would discard velocity and introduce a visible kink.
	// Only the physical actuator bounds may stop motion abruptly.
	inline void Damp(float& position, float& velocity, float goal, float delta,
		float low = 0, float high = .65f, float omega = 6)
	{
		const float decay = std::exp(-omega * delta);
		const float offset = position - goal;
		const float change = (velocity + omega * offset) * delta;
		velocity = (velocity - omega * change) * decay;
		const float next = goal + (offset + change) * decay;
		position = std::clamp(next, low, high);
		if ((next <= low && velocity < 0) || (next >= high && velocity > 0)) velocity = 0;
		if (goal == 0 && std::abs(position) < .001f && std::abs(velocity) < .01f) {
			position = 0; velocity = 0;
		}
	}

	struct Motion
	{
		Shape current{};
		Shape velocity{};

		// Critically damped motion retains velocity when a line changes. A new
		// target therefore cannot snap the brow or restart a fast accent.
		bool Step(const Shape& target, float delta)
		{
			if (!std::isfinite(delta) || delta <= 0.0f) {
				return std::any_of(current.begin(), current.end(), [](float v) { return v > 0.0f; });
			}
			delta = std::min(delta, 0.1f);
			bool any = false;
			for (std::size_t i = 0; i < kSlots; ++i) {
				const float goal = std::isfinite(target[i]) ? std::clamp(target[i], 0.0f, 0.65f) : 0.0f;
				Damp(current[i], velocity[i], goal, delta);
				any |= current[i] > 0.0f;
			}
			return any;
		}
	};
}

#pragma once

#include "SD/Scene/BrowMotion.h"

namespace SD::Scene::Listener
{
	// Stronger cinematic test: bounded native poses, not extra overlapping layers.
	inline constexpr float kPoseScale = .72f;
	inline constexpr float kPoseBudget = .85f;
	// Acting choices, not a claim about the player's internal state. Do not
	// automatically play fear in response to an angry NPC or mirror full intensity.
	[[nodiscard]] inline Expressions::Reading ResponseTo(std::uint32_t emotion, std::uint16_t percent)
	{
		using namespace Expressions;
		switch (emotion) {
		case kHappy: return { kHappy, std::min<std::uint16_t>(percent, 70) };
		case kSad: case kFear: return { kSad, std::min<std::uint16_t>(percent, 65) };
		case kDisgust: return { kDisgust, std::min<std::uint16_t>(percent, 35) };
		case kSurprise: return { kSurprise, std::min<std::uint16_t>(percent, 55) };
		case kPuzzled: return { kPuzzled, std::min<std::uint16_t>(percent, 45) };
		default: return { kNeutral, 0 };
		}
	}

	[[nodiscard]] inline float Gain(std::uint32_t emotion, float age)
	{
		if (!std::isfinite(age)) return 0.0f;
		const auto pulse = [age](float center, float width) {
			const float t = (std::max(age, 0.0f) - center) / width;
			return std::exp(-2.0f * t * t);
		};
		using namespace Expressions;
		switch (emotion) {
		case kHappy: return .55f + .45f * pulse(1.30f, 1.50f);
		case kSad: case kFear: return .65f + .35f * pulse(1.40f, 1.40f);
		case kSurprise: return .22f + .78f * pulse(.70f, .70f);
		case kDisgust: case kPuzzled: return .48f + .52f * pulse(1.15f, 1.10f);
		case kAnger: return .60f + .40f * pulse(1.10f, 1.10f);
		default: return .75f;
		}
	}

	struct Performance
	{
		Expressions::Shape current{}, velocity{};

		bool Step(float delta, Expressions::Reading reading, float age, float intensity,
			bool listening, bool playerSpeaking, bool enabled)
		{
			// Mouth authority is a hard gate, not a leisurely fade through speech.
			// Feature disable/close similarly cannot leave an owned expression behind.
			if (playerSpeaking || !enabled || !std::isfinite(intensity) || intensity <= 0.0f) {
				current = {};
				velocity = {};
				return false;
			}
			Expressions::Shape target{};
			if (listening && reading.emotion > Expressions::kNeutral && reading.emotion <= Expressions::kPuzzled) {
				const float strength = kPoseScale * std::sqrt(std::clamp(reading.percent / 100.0f, 0.0f, 1.0f)) *
					std::clamp(intensity, 0.0f, 2.0f) * Gain(reading.emotion, age);
				target = Expressions::MakeShape(Expressions::ExpressionFor(reading.emotion), std::min(strength, kPoseBudget));
			}
			if (!std::isfinite(delta) || delta <= 0)
				return std::any_of(current.begin(), current.end(), [](float v) { return v > 0; });
			delta = std::min(delta, .1f);
			for (std::size_t i = 0; i < current.size(); ++i)
				Brows::Damp(current[i], velocity[i], target[i], delta, 0, kPoseBudget, 8);
			// Bound the sum as well as each slot through opposing-pose crossfades.
			float sum = 0;
			for (float value : current) sum += value;
			if (sum > kPoseBudget) for (std::size_t i = 0; i < current.size(); ++i) {
				current[i] *= kPoseBudget / sum;
				velocity[i] *= kPoseBudget / sum;
			}
			return sum > 0;
		}

		[[nodiscard]] float UpperContribution() const
		{
			float sum = 0;
			for (float value : current) sum += value;
			return std::clamp(sum / .35f, 0.0f, 1.0f);
		}
	};

	// Per-keyframe ownership. The engine adapter binds this to one exact head,
	// animation data object and values buffer, never to a lookup on a new head.
	struct OwnedPose
	{
		Expressions::Shape baseline{}, last{};
		bool owned{ false };

		Expressions::Shape Apply(const Expressions::Shape& live, const Expressions::Shape& target)
		{
			for (std::size_t i = 0; i < live.size(); ++i) {
				if (!owned || std::abs(live[i] - last[i]) > .0001f) baseline[i] = live[i];
			}
			last = target;
			owned = true;
			return target;
		}

		Expressions::Shape Release(Expressions::Shape live)
		{
			if (owned) for (std::size_t i = 0; i < live.size(); ++i) {
				if (std::abs(live[i] - last[i]) <= .0001f) live[i] = baseline[i];
			}
			*this = {};
			return live;
		}
	};
}

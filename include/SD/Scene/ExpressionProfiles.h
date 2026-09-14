#pragma once

#include "SD/Scene/BrowMotion.h"

namespace SD::Scene::ExpressionProfiles
{
	// Fixed cinematic tuning. Retired iExpressionStrength keys are not read.
	inline constexpr float kIntensity = 1.50f;

	[[nodiscard]] inline float SoftLimit(float value)
	{
		if (!std::isfinite(value) || value <= 0) return 0;
		// Preserve weaker details; approach the .65 safety ceiling continuously
		// instead of flattening different brow poses into the same hard plateau.
		constexpr float knee = .35f, headroom = .30f;
		return value <= knee ? value : knee + headroom * (1 - std::exp(-(value - knee) / headroom));
	}
	enum class ID : std::size_t {
		Neutral, Inquiry, Confirmation, Skepticism, Explanation, Reassurance, Greeting,
		Anger, Disgust, Fear, Sad, Happy, Surprise, Puzzled, Count
	};
	inline constexpr std::array<const char*, static_cast<std::size_t>(ID::Count)> kSections{
		"Expression.Neutral", "Expression.Inquiry", "Expression.Confirmation", "Expression.Skepticism",
		"Expression.Explanation", "Expression.Reassurance", "Expression.Greeting", "Expression.Anger",
		"Expression.Disgust", "Expression.Fear", "Expression.Sad", "Expression.Happy",
		"Expression.Surprise", "Expression.Puzzled"
	};
	inline constexpr std::array<const char*, 6> kKeys{
		"iBrowLiftLeft", "iBrowLiftRight", "iBrowInLeft", "iBrowInRight", "iSquintLeft", "iSquintRight"
	};
	struct Profile
	{
		// Signed vertical lift: negative lowers, positive raises. No opposing pair.
		std::array<float, 6> controls{};
		std::uint32_t region{};
		float regionWeight{};
	};
	using Library = std::array<Profile, static_cast<std::size_t>(ID::Count)>;
	struct Pose { Brows::Shape modifiers{}, regional{}; };

	[[nodiscard]] inline Library Defaults()
	{
		using namespace Expressions;
		return {{
			{ {}, kNeutral, 0 },
			{ { .45f, .60f, .30f, .18f, .22f, .17f }, kPuzzled, .52f },
			{ { .36f, .29f, .06f, .06f, .09f, .09f }, kSurprise, .40f },
			{ { -.30f, .50f, .32f, .18f, .30f, .22f }, kPuzzled, .52f },
			{ { .15f, .12f, .13f, .13f, .17f, .17f }, kNeutral, 0 },
			{ { .16f, .16f, .10f, .10f, .15f, .15f }, kHappy, .24f },
			{ { .24f, .20f, .02f, .02f, .23f, .22f }, kHappy, .32f },
			{ { -.40f, -.40f, .32f, .32f, .32f, .32f }, kAnger, .70f },
			{ { -.29f, -.19f, .27f, .22f, .36f, .26f }, kDisgust, .70f },
			{ { .43f, .43f, .20f, .20f, 0, 0 }, kFear, .70f },
			{ { .24f, .24f, .37f, .37f, .10f, .10f }, kSad, .70f },
			{ { .14f, .12f, 0, 0, .43f, .40f }, kHappy, .70f },
			{ { .49f, .49f, 0, 0, 0, 0 }, kSurprise, .70f },
			{ { -.17f, .30f, .30f, .18f, .24f, .18f }, kPuzzled, .70f }
		}};
	}
	inline const Library kDefaults = Defaults();

	[[nodiscard]] inline Pose Resolve(const Profile& profile)
	{
		Pose result;
		for (std::size_t side = 0; side < 2; ++side) {
			const float lift = std::isfinite(profile.controls[side]) ? std::clamp(profile.controls[side], -1.0f, 1.0f) : 0;
			result.modifiers[side] = std::max(0.0f, -lift);
			result.modifiers[side + 4] = std::max(0.0f, lift);
			for (std::size_t part = 1; part < 3; ++part) {
				const float v = profile.controls[part * 2 + side];
				result.modifiers[(part == 1 ? 2 : 6) + side] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0;
			}
		}
		if (profile.region > Expressions::kNeutral && profile.region <= Expressions::kPuzzled && std::isfinite(profile.regionWeight))
			result.regional[profile.region] = std::clamp(profile.regionWeight, 0.0f, 1.0f);
		return result;
	}

	// Interpolate lift on a signed axis; an anger -> surprise crossfade must pass
	// through neutral instead of writing simultaneous brow-up and brow-down.
	struct Motion
	{
		Brows::Shape current{}, velocity{}, axes{}, axisVelocity{};
		bool Step(const Brows::Shape& target, float delta)
		{
			if (!std::isfinite(delta) || delta <= 0)
				return std::any_of(current.begin(), current.end(), [](float v) { return v > 0; });
			delta = std::min(delta, .1f);
			for (std::size_t i = 2; i < axes.size(); ++i) {
				const bool vertical = i == 4 || i == 5;
				const float value = vertical ? target[i] - target[i - 4] : target[i];
				const float low = vertical ? -.65f : 0.0f;
				const float goal = std::isfinite(value) ? std::clamp(value, low, .65f) : 0;
				Brows::Damp(axes[i], axisVelocity[i], goal, delta, low);
			}
			current = axes;
			velocity = axisVelocity;
			for (std::size_t side = 0; side < 2; ++side) {
				const float lift = axes[side + 4];
				current[side] = std::max(0.0f, -lift);
				current[side + 4] = std::max(0.0f, lift);
				velocity[side] = lift < 0 ? -axisVelocity[side + 4] : 0;
				velocity[side + 4] = lift > 0 ? axisVelocity[side + 4] : 0;
			}
			return std::any_of(current.begin(), current.end(), [](float v) { return v > 0; });
		}
	};
}

#pragma once

#include "SD/Scene/Acting.h"
#include "SD/Scene/ListenerPerformance.h"

namespace SD::Scene::UpperFace
{
	// Deliberately excludes phonemes, whole expressions, blink and gaze.
	inline constexpr std::array<std::uint32_t, 8> kSlots{ 2, 3, 4, 5, 6, 7, 12, 13 };
	using Shape = Brows::Shape;

	[[nodiscard]] inline float EstimateDuration(std::string_view text)
	{
		return std::clamp(static_cast<float>(Acting::Words(Acting::Normalize(text))) / 2.6f, 1.2f, 20.0f);
	}

	struct Performance
	{
		ExpressionProfiles::Motion motion{};
		Brows::Motion regionalMotion{};
		ExpressionProfiles::Library profiles{ ExpressionProfiles::kDefaults };
		Acting::Plan plan{};
		Expressions::Reading reading{};
		float age{}, quietFor{}, duration{ 1.2f };
		bool listening{ true }, voiceDuration{};
		std::size_t beatIndex{};

		void Begin(Expressions::Reading value, bool listener, std::string_view text)
		{
			listening = listener;
			age = quietFor = 0;
			beatIndex = 0;
			voiceDuration = false;
			duration = EstimateDuration(text);
			if (listener) {
				// Respond to the NPC's current tone, not their question gestures
				// or future clauses. Neutral lines release prior affect.
				plan = {};
				plan.beats[0].tone = value;
				plan.beats[0].evidence = value.emotion ? Acting::Evidence::Authored : Acting::Evidence::Neutral;
			} else plan = Acting::Build(text, value);
			reading = plan.beats[0].tone;
			// Keep position and velocity: a new line never teleports the brows.
		}

		void SetDuration(float value)
		{
			if (std::isfinite(value) && value > 0) {
				duration = std::clamp(value, .15f, 120.0f);
				voiceDuration = true;
			}
		}

		void SynchronizeSpeech(float elapsed, float nextDelta)
		{
			if (!listening && std::isfinite(elapsed) && elapsed >= 0 && std::isfinite(nextDelta) && nextDelta > 0)
				age = std::max(0.0f, elapsed - std::min(nextDelta, .1f));
		}

		[[nodiscard]] Acting::Pose TargetPose(float intensity) const
		{
			auto pose = Acting::Sample(plan, age, duration, listening, intensity, profiles);
			const float rest = std::exp(-std::max(0.0f, quietFor - .15f) / .65f);
			for (auto& v : pose.modifiers) v *= rest;
			for (auto& v : pose.regional) v *= rest;
			return pose;
		}
		[[nodiscard]] Shape Target(float intensity) const { return TargetPose(intensity).modifiers; }

		bool Step(float delta, bool enabled, bool speechActive, float intensity, bool nativeListener = false)
		{
			if (std::isfinite(delta) && delta > 0) {
				delta = std::min(delta, .1f);
				age += delta;
				quietFor = speechActive ? 0 : quietFor + delta;
				beatIndex = Acting::Index(plan, age, duration);
				reading = plan.beats[beatIndex].tone;
			}
			// Gate the target, never the published pose. Hidden motion used to build
			// up during native listening and appear in one frame at the handoff.
			const auto pose = enabled && !nativeListener ? TargetPose(intensity) : Acting::Pose{};
			const bool regional = regionalMotion.Step(pose.regional, delta);
			return motion.Step(pose.modifiers, delta) || regional;
		}
	};
}

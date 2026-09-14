#include "SD/Scene/UpperFace.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
	void Expect(bool value, const char* why) {
		if (!value) { std::cerr << why << '\n'; std::exit(EXIT_FAILURE); }
	}
	void Exclusive(const SD::Scene::Brows::Shape& pose) {
		for (std::size_t side = 0; side < 2; ++side)
			Expect(pose[side] == 0 || pose[side + 4] == 0, "never raise and lower the same brow simultaneously");
		for (float v : pose) Expect(std::isfinite(v) && v >= 0 && v <= 1, "finite bounded profile");
	}
}

int main()
{
	using namespace SD::Scene;
	using namespace ExpressionProfiles;
	for (const auto& profile : kDefaults) Exclusive(Resolve(profile).modifiers);
	const auto neutral = Acting::Build("I live here.");
	Expect(Acting::Sample(neutral, 1, 4, false, 2).modifiers == Brows::Shape{}, "neutral has no generic brow gesture");
	const auto question = Acting::Build("What have you learned in your travels?");
	const auto angryQuestion = Acting::Build("What have you learned?", { Expressions::kAnger, 100 });
	Expect(Acting::ProfileFor(question.beats[0]) == ID::Inquiry, "question selects complete inquiry profile");
	Expect(Acting::ProfileFor(angryQuestion.beats[0]) == ID::Anger, "emotion selects one complete profile, not extra question brows");
	Expect(Acting::Recipe(angryQuestion.beats[0], false).modifiers == Resolve(kDefaults[static_cast<std::size_t>(ID::Anger)]).modifiers,
		"no hidden action mix on emotional profile");
	Library custom = kDefaults;
	custom[static_cast<std::size_t>(ID::Inquiry)] = { { -.4f, .5f, .2f, .1f, .3f, .4f }, Expressions::kHappy, .25f };
	const auto overridden = Acting::Recipe(question.beats[0], false, custom);
	Expect(overridden.modifiers[0] == .4f && overridden.modifiers[4] == 0 && overridden.modifiers[5] == .5f,
		"profile owns signed brow directions");
	Expect(overridden.regional[Expressions::kHappy] == .25f && overridden.regional[Expressions::kPuzzled] == 0,
		"same profile owns regional detail");
	UpperFace::Performance live;
	live.profiles = custom;
	live.Begin({}, false, "What have you learned?");
	for (int i = 0; i < 60; ++i) live.Step(1/60.0f, true, true, 1);
	Expect(live.motion.current[0] > .2f && live.motion.current[4] == 0, "custom library reaches frame sampler");
	live.Begin({}, false, "I live here.");
	for (int i = 0; i < 360; ++i) live.Step(1/60.0f, true, true, 1);
	Expect(live.motion.current == Brows::Shape{}, "neutral releases previous expression");

	for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() }) {
		Profile p{ { bad, -bad, bad, -bad, bad, -bad }, 999, bad };
		const auto pose = Resolve(p);
		Expect(pose.modifiers == Brows::Shape{} && pose.regional == Brows::Shape{}, "invalid fields cannot escape profile");
	}
	Profile large{ { -100, 100, -5, 200, 200, -100 }, 7, 100 };
	Exclusive(Resolve(large).modifiers);
	const auto low = Resolve(kDefaults[static_cast<std::size_t>(ID::Anger)]).modifiers;
	const auto high = Resolve(kDefaults[static_cast<std::size_t>(ID::Surprise)]).modifiers;
	for (float fps : { 30.0f, 60.0f, 144.0f }) {
		Motion motion;
		for (int f = 0; f < static_cast<int>(fps * 4); ++f) {
			const auto before = motion.current;
			motion.Step(f < fps ? low : high, 1/fps);
			Exclusive(motion.current);
			for (std::size_t i = 0; i < before.size(); ++i)
				Expect(std::abs(before[i] - motion.current[i]) < .1f, "signed crossfade stays smooth");
		}
		Expect(motion.current[4] > .48f && motion.current[0] == 0, "crossfade arrives at upward pose");
		for (int f = 0; f < static_cast<int>(fps * 5); ++f) motion.Step({}, 1/fps);
		Expect(motion.current == Brows::Shape{}, "disable releases every profile control");
		const auto before = motion.current;
		motion.Step(high, std::numeric_limits<float>::quiet_NaN());
		Expect(motion.current == before, "bad delta leaves pose untouched");
	}

	// A moving goal passing through the brow is not a reason to kill velocity.
	Motion crossing;
	crossing.axes[4] = .20f;
	crossing.axisVelocity[4] = .30f;
	Brows::Shape nearby{};
	nearby[4] = .201f;
	crossing.Step(nearby, 1/60.0f);
	Expect(crossing.current[4] > nearby[4] && crossing.axisVelocity[4] > .20f,
		"moving-target crossing retains momentum instead of snapping to the target");
	Expect(SoftLimit(.2f) == .2f, "weak details retain their weight");
	Expect(SoftLimit(.65f) < SoftLimit(.80f) && SoftLimit(.80f) < SoftLimit(1.0f),
		"strong poses remain distinguishable above the old hard ceiling");
	const float leftSlope = (SoftLimit(.35f) - SoftLimit(.3499f)) / .0001f;
	const float rightSlope = (SoftLimit(.3501f) - SoftLimit(.35f)) / .0001f;
	Expect(std::abs(leftSlope - rightSlope) < .003f, "soft knee has no velocity kink");

	// Reproducible tuning sweep, not a claim of perceptual optimality for all heads.
	// Same 3-second inquiry, all candidates through the production frame sampler.
	for (float strength : { 1.0f, 1.25f, 1.40f, 1.50f, 1.60f, 1.75f, 2.0f }) {
		UpperFace::Performance sample;
		sample.Begin({}, false, "What have you learned in your travels?");
		sample.SetDuration(3);
		Brows::Shape peak{};
		for (int f = 0; f < 180; ++f) {
			sample.Step(1/60.0f, true, true, strength);
			for (std::size_t i = 0; i < peak.size(); ++i) peak[i] = std::max(peak[i], sample.motion.current[i]);
		}
		std::cout << "inquiry strength=" << strength << " peak lift=" << peak[4] << '/' << peak[5]
			<< " knit=" << peak[2] << " squint=" << peak[6] << "\n";
		if (strength == kIntensity) {
			Expect(peak[4] > .55f && peak[5] - peak[4] > .025f, "fixed tuning preserves readable asymmetric brows");
			Expect(peak[6] > .39f && peak[2] > .49f, "fixed tuning retains squint and knit detail");
		}
	}

	for (float fps : { 30.0f, 60.0f, 144.0f }) {
		UpperFace::Performance handoff;
		handoff.Begin({}, false, "What have you learned?");
		handoff.SetDuration(3);
		for (int f = 0; f < static_cast<int>(fps); ++f) handoff.Step(1/fps, true, true, kIntensity);
		const auto spoken = handoff.motion.current;
		handoff.Begin({ Expressions::kSurprise, 50 }, true, "There were towers that touched the stars.");
		handoff.Step(1/fps, true, true, kIntensity, true);
		Expect(handoff.motion.current[5] > .5f, "listener handoff fades visible speaking pose, not a hard zero");
		for (std::size_t i = 0; i < spoken.size(); ++i)
			Expect(std::abs(handoff.motion.current[i] - spoken[i]) < .04f, "first listener frame is continuous");
		for (int f = 0; f < static_cast<int>(fps * 3); ++f) handoff.Step(1/fps, true, true, kIntensity, true);
		Expect(handoff.motion.current == Brows::Shape{} && handoff.regionalMotion.current == Brows::Shape{},
			"no hidden profile builds up while native expression owns the face");
		handoff.Step(1/fps, true, true, kIntensity, false);
		for (float v : handoff.motion.current) Expect(v < .015f, "native release eases in rather than revealing stale pose");
		handoff.Begin({}, false, "Why would you say that?");
		for (int f = 0; f < static_cast<int>(fps); ++f) handoff.Step(1/fps, true, true, kIntensity);
		Expect(handoff.motion.current[5] > .5f, "return to speech reaches its profile");
	}
	std::cout << "Unified expression profiles, overrides and signed motion passed\n";
}

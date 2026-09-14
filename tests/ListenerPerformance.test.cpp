#include "SD/Scene/ListenerPerformance.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) { std::cerr << "Listener: " << message << '\n'; std::exit(EXIT_FAILURE); }
	}
	float Sum(const SD::Scene::Expressions::Shape& pose)
	{
		float result = 0;
		for (float value : pose) result += value;
		return result;
	}
}

int main()
{
	using namespace SD::Scene;
	using namespace Expressions;
	Expect(Listener::ResponseTo(kAnger, 100).emotion != kFear, "NPC anger does not assign player fear");
	Expect(Listener::ResponseTo(kDisgust, 100).emotion == kDisgust, "distaste no longer collapses into puzzlement");
	Expect(Listener::ResponseTo(kDisgust, 100).percent == 35, "listener does not copy full NPC disgust");
	Expect(Listener::ResponseTo(kPuzzled, 100).percent == 45, "uncertainty is restrained");
	Expect(Listener::ResponseTo(kNeutral, 100).percent == 0, "neutral speech does not invent a full-face emotion");
	Expect(Listener::ResponseTo(999, 100).percent == 0, "unknown metadata stays neutral");
	Expect(Listener::Gain(kHappy, 1.30f) > 1.7f * Listener::Gain(kHappy, 6), "smile has an accent and a readable settled pose");
	Expect(Listener::Gain(kSurprise, .70f) > 4 * Listener::Gain(kSurprise, 3), "surprise must not remain wide-eyed");
	Expect(Listener::Gain(kSad, 4) > Listener::Gain(kSurprise, 4), "concern persists more than surprise");
	Expect(Listener::Gain(kHappy, std::numeric_limits<float>::quiet_NaN()) == 0, "invalid age is safe");

	for (float fps : { 30.0f, 60.0f, 144.0f }) {
		Listener::Performance face;
		face.Step(1/fps, { kHappy, 100 }, 1.3f, 2, true, false, true);
		Expect(Sum(face.current) < .03f, "native brow/cheek onset accelerates from rest instead of jumping");
		face = {};
		float peak = 0;
		for (int frame = 0; frame < static_cast<int>(fps * 6); ++frame) {
			face.Step(1/fps, Listener::ResponseTo(kHappy, 80), frame/fps, 1.64f, true, false, true);
			peak = std::max(peak, Sum(face.current));
			Expect(Sum(face.current) <= Listener::kPoseBudget + .00001f, "full-face aggregate stays bounded");
		}
		Expect(peak > .80f && Sum(face.current) > .50f && Sum(face.current) < peak * .75f,
			"strong onset settles to a still-readable smile during a long line");
		Expect(face.current[kDialogueHappy] > 0, "native happy expression contributes to the whole face");
		Expect(!face.Step(1/fps, { kHappy, 100 }, 1, 2, true, true, true), "voice start is an immediate safety gate");
		Expect(Sum(face.current) == 0 && face.UpperContribution() == 0 && Sum(face.velocity) == 0,
			"no whole-face tail or momentum overlaps player speech");
		face.Step(1/fps, { kSad, 100 }, 1, 2, true, false, true);
		Expect(!face.Step(1/fps, {}, 0, 1, false, false, false), "disable and conversation close clear the envelope");
		Expect(Sum(face.current) == 0, "disable cannot leave a frozen pose");
		face.Step(1/fps, {}, 0, 1, true, false, true);
		Expect(Sum(face.current) == 0, "new neutral conversation inherits no old face");
		for (int frame = 0; frame < static_cast<int>(fps * 5); ++frame) {
			face.Step(1/fps, { frame % 2 ? kHappy : kSad, 100 }, 1, 2, true, false, true);
			Expect(Sum(face.current) <= Listener::kPoseBudget + .00001f, "opposing-pose crossfade has a shared amplitude budget");
		}
		for (int frame = 0; frame < static_cast<int>(fps * 5); ++frame)
			face.Step(1/fps, {}, 0, 1, false, false, true);
		Expect(Sum(face.current) == 0, "NPC silence eventually releases the face");
	}

	// The user's tests used 50% authored emotion at 1.64x and 2.0x strength. Measure the
	// actual smoothed pose, not just a higher unreachable target or clamp.
	for (float fps : { 30.0f, 60.0f, 144.0f }) for (float intensity : { 1.64f, 2.0f }) {
		for (auto emotion : { kHappy, kSurprise, kSad, kDisgust, kPuzzled }) {
			Listener::Performance face;
			float peak = 0, heldFor = 0;
			for (int frame = 0; frame < static_cast<int>(fps * 6); ++frame) {
				face.Step(1/fps, Listener::ResponseTo(emotion, 50), frame/fps, intensity, true, false, true);
				peak = std::max(peak, Sum(face.current));
				if (Sum(face.current) > .60f) heldFor += 1/fps;
			}
			Expect(peak > .65f, "common half-intensity lines now produce a substantial peak");
			Expect(heldFor > .25f, "strong peak survives smoothing for more than a few frames");
			if (emotion == kHappy) Expect(Sum(face.current) > .44f, "settled smile no longer falls to roughly 0.12");
			if (emotion == kSurprise) Expect(Sum(face.current) > .17f && Sum(face.current) < .24f,
				"surprise remains detectable but does not hold the initial gasp");
			std::cout << "50% emotion " << emotion << " strength=" << intensity << " @ " << fps << "fps: peak=" << peak
				<< " settled=" << Sum(face.current) << " above0.6=" << heldFor << "s\n";
		}
	}

	for (float intensity : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity() }) {
		Listener::Performance face;
		face.Step(.1f, { kHappy, 100 }, 1, 2, true, false, true);
		Expect(Sum(face.current) > 0, "zero-strength test starts with an owned pose");
		Expect(!face.Step(.1f, { kHappy, 100 }, 1, intensity, true, false, true), "zero or invalid strength releases immediately");
		Expect(Sum(face.current) == 0, "zero strength cannot retain a previous smile");
	}

	Listener::OwnedPose owner;
	Shape baseline{};
	baseline[kMoodNeutral] = .13f;
	auto live = owner.Apply(baseline, MakeShape(kDialogueHappy, .4f));
	Expect(owner.Release(live) == baseline, "release restores pre-existing expression values");
	live = owner.Apply(baseline, MakeShape(kDialogueHappy, .4f));
	live[kDialogueHappy] = .7f;
	auto restored = owner.Release(live);
	Expect(restored[kDialogueHappy] == .7f && restored[kMoodNeutral] == .13f, "handback preserves another writer's new value");
	live = owner.Apply(baseline, MakeShape(kDialogueHappy, .4f));
	live[kDialogueSad] = .2f;
	live = owner.Apply(live, MakeShape(kDialogueHappy, .3f));
	restored = owner.Release(live);
	Expect(restored[kDialogueSad] == .2f, "another writer's latest contribution becomes its return value");
	owner = {};
	Expect(owner.Release(baseline) == baseline, "unowned or replaced buffer receives no stale debt");
	std::cout << "Listener acting, speech priority, interruption and ownership passed.\n";
}

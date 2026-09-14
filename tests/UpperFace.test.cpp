#include "SD/Scene/UpperFace.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
	void Expect(bool value, const char* why) {
		if (!value) { std::cerr << why << '\n'; std::exit(EXIT_FAILURE); }
	}
	float Peak(const SD::Scene::UpperFace::Shape& pose) {
		return *std::max_element(pose.begin(), pose.end());
	}
}

int main()
{
	using namespace SD::Scene;
	using namespace Expressions;
	constexpr std::array<std::uint32_t, 8> safeSlots{ 2, 3, 4, 5, 6, 7, 12, 13 };
	Expect(UpperFace::kSlots == safeSlots, "Only mouth/blink/gaze-safe modifier slots");
	for (float fps : { 30.0f, 60.0f, 144.0f }) for (float strength : { .98f, 1.64f, 2.0f }) {
		UpperFace::Performance face;
		face.Begin({}, false, "I wouldn't have expected to see one of the Legion here. Why Ivarstead?");
		face.SetDuration(4.04f);
		float inquiryPeak = 0, earlierIn = 0, maxJump = 0;
		UpperFace::Shape previous{};
		for (int frame = 0; frame < static_cast<int>(fps * 4.04f); ++frame) {
			face.Step(1/fps, true, true, strength);
			if (face.beatIndex == 1) inquiryPeak = std::max(inquiryPeak, face.motion.current[2]);
			else earlierIn = std::max(earlierIn, face.motion.current[2]);
			for (std::size_t i = 0; i < previous.size(); ++i) {
				maxJump = std::max(maxJump, std::abs(face.motion.current[i] - previous[i]));
				Expect(std::isfinite(face.motion.current[i]) && face.motion.current[i] <= .65001f, "bounded modifiers");
			}
			previous = face.motion.current;
		}
		Expect(inquiryPeak > .20f && inquiryPeak > earlierIn * 3, "late question gets distinct brow knit at saved strength");
		Expect(maxJump < .075f, "clause transition stays smooth even at 30fps");
		Expect(face.reading.emotion == kNeutral, "inquiry is not mislabeled sadness or surprise");
		const auto position = face.motion.current, velocity = face.motion.velocity;
		face.Begin({}, false, "Thank you. I'm sorry.");
		Expect(position == face.motion.current && velocity == face.motion.velocity, "replacement retains position and velocity");
		face.Step(1/fps, true, true, strength);
		for (int frame = 0; frame < static_cast<int>(fps * 5); ++frame) face.Step(1/fps, false, false, strength);
		Expect(Peak(face.motion.current) == 0 && Peak(face.regionalMotion.current) == 0, "disable fully releases both layers");

		face.Begin({ kHappy, 60 }, true, "Good to see you.");
		for (int frame = 0; frame < static_cast<int>(fps); ++frame) face.Step(1/fps, true, true, strength);
		face.Begin({}, true, "Here are the details.");
		Expect(face.reading.emotion == kNeutral, "neutral NPC continuation does not inherit a forced smile");
		face.Begin({}, false, "What have you learned in your travels?");
		face.SetDuration(3.0f);
		for (int frame = 0; frame < static_cast<int>(fps * 1.5f); ++frame) face.Step(1/fps, true, true, strength);
		Expect(face.motion.current[2] > .20f && face.regionalMotion.current[kPuzzled] > .30f, "inquiry has brow and regional support");
		const auto clean = face.motion.current;
		face.Step(std::numeric_limits<float>::quiet_NaN(), true, true, strength);
		Expect(face.motion.current == clean, "invalid time cannot corrupt pose");
		for (int frame = 0; frame < static_cast<int>(fps * 7); ++frame) face.Step(1/fps, true, false, 0);
		Expect(Peak(face.motion.current) == 0 && Peak(face.regionalMotion.current) == 0, "zero strength releases pose");

		face.Begin({}, false, "Why would you want to learn about them? Killing is simple enough.");
		face.SetDuration(3.8f);
		for (int frame = 0; frame < static_cast<int>(fps * 3.2f); ++frame) face.Step(1/fps, true, true, strength);
		Expect(face.beatIndex == 1 && face.reading.emotion == kNeutral, "question followed by statement changes beat without inventing emotion");
		face.Begin({}, false, "I wouldn't have expected to see one of the Legion here. Why Ivarstead?");
		face.SetDuration(4.04f);
		face.SynchronizeSpeech(3.8f, 1/fps);
		face.Step(1/fps, true, true, strength);
		Expect(face.beatIndex == 1 && std::abs(face.age - 3.8f) < .001f, "reenabling mid-voice resumes the current clause, not the opening");
	}
	std::cout << "Clause integration, inquiry visibility, speech-safe channels and lifecycle passed.\n";
}

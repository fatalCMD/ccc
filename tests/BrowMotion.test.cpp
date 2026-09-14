#include "SD/Scene/BrowMotion.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "BrowMotion: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	using namespace SD::Scene;
	const Brows::Shape happy{ 0, 0, 0, 0, .16f, .16f, .24f, .24f };
	const Brows::Shape angry{ .32f, .32f, .28f, .28f, 0, 0, .16f, .16f };

	// A long voiced question must read as a few deliberate gestures, not a
	// series of phoneme-rate spikes. Include its full release in the trace.
	for (const float fps : { 30.0f, 60.0f, 144.0f }) {
		Brows::Motion motion;
		float last = 0.0f, largestStep = 0.0f;
		int previousDirection = 0, reversals = 0;
		for (int frame = 0; frame < static_cast<int>(fps * 12.0f); ++frame) {
			const float elapsed = static_cast<float>(frame) / fps;
			const auto target = elapsed < 8.0f ? happy : Brows::Shape{};
			motion.Step(target, 1.0f / fps);
			const float change = motion.current[4] - last;
			largestStep = std::max(largestStep, std::abs(change));
			const int direction = change > 0.00001f ? 1 : change < -0.00001f ? -1 : 0;
			if (direction && previousDirection && direction != previousDirection) ++reversals;
			if (direction) previousDirection = direction;
			last = motion.current[4];
		}
		Expect(reversals <= 3, "one onset, settle, question lift and release must not become repeated jitter");
		Expect(largestStep < 0.035f, "brow movement cannot jump sharply between frames");
		Expect(!motion.Step({}, 1.0f / fps), "release ends at exact zero");
	}

	Brows::Motion interrupted;
	for (int frame = 0; frame < 8; ++frame) interrupted.Step(happy, 1.0f / 60.0f);
	const auto before = interrupted.current;
	interrupted.Step(angry, 1.0f / 60.0f);
	for (std::size_t i = 0; i < before.size(); ++i) {
		Expect(std::abs(interrupted.current[i] - before[i]) < 0.02f,
			"interrupting or replacing a line preserves continuity");
	}
	for (int frame = 0; frame < 240; ++frame) interrupted.Step({}, 1.0f / 60.0f);
	Expect(!interrupted.Step({}, 1.0f / 60.0f), "toggling off or handing over releases the entire old pose");

	Brows::Motion at30, at144;
	for (int frame = 0; frame < 30; ++frame) at30.Step(happy, 1.0f / 30.0f);
	for (int frame = 0; frame < 144; ++frame) at144.Step(happy, 1.0f / 144.0f);
	for (std::size_t i = 0; i < happy.size(); ++i) {
		Expect(std::abs(at30.current[i] - at144.current[i]) < 0.0001f, "motion is consistent across frame rates");
	}
	const auto saved = at30.current;
	at30.Step(happy, std::numeric_limits<float>::quiet_NaN());
	Expect(at30.current == saved, "invalid frame time cannot corrupt the brows");
	std::cout << "Brow gestures, interruption, release and frame-rate checks passed.\n";
}

#include "SD/Scene/UpperFaceTest.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
	void Expect(bool value, const char* why) {
		if (!value) { std::cerr << why << '\n'; std::exit(EXIT_FAILURE); }
	}
}

int main()
{
	using namespace SD::Scene;
	for (float bad : { -1.0f, 27.0f, 100.0f, std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN() })
		Expect(!UpperFaceTest::Sample(bad).active, "bounded one-shot runtime");
	for (float fps : { 30.0f, 60.0f, 144.0f }) {
		std::array<unsigned, 8> seen{};
		for (int frame = 0; frame < static_cast<int>(27 * fps); ++frame) {
			const auto state = UpperFaceTest::Sample(static_cast<float>(frame) / fps);
			Expect(state.active, "active within runtime");
			unsigned nonzero = 0;
			for (std::size_t i = 0; i < state.values.size(); ++i) {
				Expect(state.values[i] == 0 || state.values[i] == 1, "no acting gain/smoothing in isolated test");
				if (state.values[i]) { ++nonzero; ++seen[i]; }
			}
			Expect(nonzero <= 1, "never combine controls");
			Expect(state.index >= 0 ? nonzero == 1 : nonzero == 0, "neutral gaps truly neutral");
		}
		for (auto count : seen) Expect(count >= static_cast<unsigned>(fps * 2) - 1, "every safe control held for two seconds");
	}
	Expect(UpperFaceTest::Sample(3).index == 4, "first hold is BrowUpLeft");
	Expect(UpperFaceTest::Sample(5).index == -1, "neutral follows first hold");
	Expect(UpperFaceTest::Sample(6).index == 5, "second hold is BrowUpRight");
	Expect(UpperFaceTest::Sample(26).index == -1, "last second is neutral");
	UpperFaceTest::OwnedPose owner;
	UpperFace::Shape baseline{ .1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f };
	UpperFace::Shape target{};
	target[4] = 1;
	owner.Apply(baseline, target);
	Expect(owner.Release(target) == baseline, "restore exact nonzero starting pose");
	owner.Apply(baseline, target);
	auto external = target;
	external[2] = .9f;
	auto restored = owner.Release(external);
	Expect(restored[2] == .9f && restored[4] == baseline[4], "do not overwrite intervening external writer");
	owner.Apply(baseline, target);
	owner.Apply(external, target);
	Expect(owner.Release(target)[2] == .9f, "remember external writes during ownership");
	Expect(!owner.owned, "release clears ownership");
	std::cout << "Upper-face isolated test policy passed\n";
}

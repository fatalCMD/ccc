#include "SD/Scene/RegionalGeometry.h"
#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
	}
}

int main()
{
	using namespace SD::Scene::Regional;
	std::array<Position, 3> original{ Position{ 1, 2, 3, 99 }, Position{ 4, 5, 6, 88 }, Position{ 7, 8, 9, 77 } };
	auto live = original;
	std::array<Vertex, 3> asset{};
	asset[0].delta[4] = { .2f, .1f, -.1f }; // cheek
	asset[1].delta[4] = { 0, 0, .3f }; asset[1].blinkLeft = 1; // eyelid
	// vertex 2 is the protected mouth: no deformation for any emotion.
	std::array<Undo, 3> undo{};
	std::array<float, 8> weights{};
	weights[5] = .65f;
	for (int frame = 0; frame < 10000; ++frame) {
		Restore(live, undo);
		Expect(live == original, "overlay must not accumulate or drift across frames");
		Apply(live, asset, undo, weights, 0, 0);
		Expect(live[2] == original[2], "mouth stays bit-for-bit unchanged");
		for (std::size_t i = 0; i < live.size(); ++i) Expect(live[i][3] == original[i][3], "opaque fourth component is preserved");
	}
	Restore(live, undo);
	Apply(live, asset, undo, weights, 1, 0);
	Expect(live[1] == original[1] && live[0] != original[0], "native blinking suppresses eyelid overlay without suppressing cheeks");
	// Another writer replaced the cheek position: release must leave that alone.
	live[0] = { 20, 30, 40, 99 };
	Restore(live, undo);
	Expect(live[0][0] == 20 && live[0][1] == 30, "handback cannot restore stale positions over another writer");
	live = original;
	Expect(Apply(live, std::span<const Vertex>(asset.data(), 2), undo, weights, 0, 0) == 0 && live == original,
		"mismatched vertex counts skip all writes");
	Apply(live, asset, undo, weights, 0, 0);
	Restore(live, undo);
	Expect(live == original, "reset restores baseline exactly");
	std::cout << "Regional geometry bounds, mouth exclusion, blink and handback passed.\n";
}

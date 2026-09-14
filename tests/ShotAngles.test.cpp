#include "SD/Camera/ShotAngles.h"
#include "SD/Camera/ShotSelection.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "ShotAngles: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	using namespace SD::Camera::ShotAngles;
	Candidates candidates{};
	const auto closeCount = MakeCandidates(26.0f, 8.0f, true, candidates);
	Expect(closeCount == 9 && candidates[0] == 26.0f, "close-up tries its authored bearing first");
	for (std::size_t i = 0; i < closeCount; ++i) {
		Expect(candidates[i] >= 14.0f && candidates[i] <= 38.0f, "close-up cannot become a profile during recovery");
	}
	// A wall blocks every portrait bearing; only a side-on view would be clear.
	// Recovery must refuse this shot rather than silently make it a profile.
	const auto blocked = SD::Camera::BestAvailable(std::span<const float>{ candidates.data(), closeCount },
		[](float) { return true; }, [](float bearing) { return bearing >= 70.0f ? 1.0f : -1.0f; });
	Expect(!blocked, "a clear profile outside the selected shot's range is not a fallback");
	const auto clear = SD::Camera::BestAvailable(std::span<const float>{ candidates.data(), closeCount },
		[](float) { return true; }, [](float bearing) { return bearing == 35.0f ? 1.0f : -1.0f; });
	Expect(clear && *clear == 35.0f, "a small clear adjustment remains available");

	const auto otsCount = MakeCandidates(20.0f, 8.0f, true, candidates);
	for (std::size_t i = 0; i < otsCount; ++i) {
		Expect(candidates[i] >= 8.0f && candidates[i] <= 32.0f, "OTS preserves its bearing range");
	}
	const auto profileCount = MakeCandidates(72.0f, 8.0f, true, candidates);
	Expect(profileCount > 0 && candidates[0] == 72.0f, "explicit profile shots retain their authored angle");
	Expect(MakeCandidates(-30.0f, 8.0f, true, candidates) == 0,
		"continuity clamping cannot bypass the maximum adjustment");
	Expect(MakeCandidates(-30.0f, 8.0f, false, candidates) == 9,
		"disabling the line rule retains the small sweep on the chosen side");
	Expect(MakeCandidates(-20.0f, 8.0f, false, candidates) == 9,
		"scene shots keep their signed room-relative angles");
	Expect(MakeCandidates(56.0f, 8.0f, true, candidates) == 9 && candidates[0] == 56.0f,
		"an explicitly selected orbit can supply its moved nominal angle");
	Expect(AllowedAdjustment(12.0f) && AllowedAdjustment(-12.0f), "held boundary adjustments remain valid");
	Expect(!AllowedAdjustment(12.01f) && !AllowedAdjustment(80.0f), "wide cached sweeps cannot bypass the new limit");
	Expect(!AllowedAdjustment(std::numeric_limits<float>::quiet_NaN()), "invalid held offsets are refused");
	Expect(MakeCandidates(std::numeric_limits<float>::quiet_NaN(), 8.0f, true, candidates) == 0,
		"invalid nominal angles produce no candidates");
	for (float nominal : { 26.321f, 47.234f, 81.317f, 152.197f }) {
		const auto count = MakeCandidates(nominal, 8.0f, true, candidates);
		for (std::size_t i = 0; i < count; ++i) {
			Expect(AllowedAdjustment(candidates[i] - nominal),
				"rounding a moving bearing must not reject a valid boundary adjustment");
		}
	}

	unsigned windows = 0;
	for (int halfDegrees = -360; halfDegrees <= 360; ++halfDegrees) {
		const float nominal = static_cast<float>(halfDegrees) * 0.5f;
		for (float floor : { 8.0f, 34.0f }) {
			for (bool enforce : { false, true }) {
				const auto count = MakeCandidates(nominal, floor, enforce, candidates);
				Expect(count <= candidates.size(), "candidate count stays within its raycast budget");
				for (std::size_t i = 0; i < count; ++i) {
					Expect(std::abs(candidates[i] - nominal) <= 12.0f, "every candidate respects the hard adjustment limit");
					Expect(!enforce || (candidates[i] >= floor && candidates[i] <= 165.0f), "continuity and framing limits both apply");
					for (std::size_t j = 0; j < i; ++j) {
						Expect(std::abs(candidates[i] - candidates[j]) >= 1.0f, "clamped candidates are deduplicated");
					}
				}
				++windows;
			}
		}
	}
	std::cout << "Shot angles: " << windows << " windows plus portrait, obstruction and held-pose cases passed.\n";
}

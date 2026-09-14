#include "SD/Camera/ShotSelection.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "ShotSelection: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	using SD::Camera::BestAvailable;
	using SD::Camera::ReusableShot;
	constexpr std::array<unsigned, 3> shots{ 0, 1, 2 };
	constexpr std::array<float, 3> quality{ 0.5f, 1.0f, 0.75f };
	const auto pool = std::span{ shots };

	// Every combination of enabled, positive-frequency and placeable shots.
	// Shot 1 represents the old emergency medium: it always scores highest,
	// but is still forbidden unless the user actually enabled it.
	for (unsigned enabled = 0; enabled < 8; ++enabled) {
		for (unsigned weighted = 0; weighted < 8; ++weighted) {
			for (unsigned clear = 0; clear < 8; ++clear) {
				const auto eligible = [&](unsigned shot) {
					return (enabled & weighted & (1u << shot)) != 0;
				};
				const auto selected = BestAvailable(pool, eligible, [&](unsigned shot) {
					Expect(eligible(shot), "disabled or zero-frequency shot must never reach placement");
					return (clear & (1u << shot)) ? quality[shot] : -1.0f;
				});
				const auto possible = enabled & weighted & clear;
				Expect(selected.has_value() == (possible != 0), "empty or blocked pool must not invent a fallback");
				if (selected) {
					Expect((possible & (1u << *selected)) != 0, "selected shot must satisfy all three constraints");
					for (unsigned other = 0; other < shots.size(); ++other) {
						Expect(!(possible & (1u << other)) || quality[*selected] >= quality[other],
							"best enabled placement must win");
					}
				}
			}
		}
	}

	unsigned enabled = 3;
	const auto eligible = [&](unsigned shot) { return (enabled & (1u << shot)) != 0; };
	const auto score = [&](unsigned shot) { return quality[shot]; };
	const auto held = BestAvailable(pool, eligible, score);
	Expect(held && *held == 1, "initially enabled medium may be selected");
	Expect(ReusableShot(true, *held, *held, eligible), "eligible held pose may continue");
	enabled = 1;
	Expect(!ReusableShot(true, *held, *held, eligible), "disabling a held shot invalidates its cached pose");
	const auto replacement = BestAvailable(pool, eligible, score);
	Expect(replacement && *replacement == 0, "recovery must use the remaining enabled shot");
	Expect(!ReusableShot(true, *held, *replacement, eligible), "old shot cannot be relabeled as its replacement");
	Expect(!ReusableShot(false, *replacement, *replacement, eligible), "an unsolved pose cannot be reused");
	enabled = 0;
	Expect(!BestAvailable(pool, eligible, score), "disabling the final shot leaves no cinematic candidate");

	enabled = 3;
	const auto changedDuringSearch = BestAvailable(pool, eligible, [&](unsigned shot) {
		if (shot == 1) {
			enabled = 1;
		}
		return quality[shot];
	});
	Expect(!changedDuringSearch, "a candidate disabled during evaluation cannot be committed");
	Expect(!BestAvailable(pool, [](unsigned) { return true; }, [](unsigned) {
		return std::numeric_limits<float>::quiet_NaN();
	}), "unknown placement quality cannot become a candidate");

	std::cout << "Shot selection: 512 eligibility/placement combinations and live cache changes passed.\n";
}

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

	// 2D helpers for the line-side checks, mirroring Solve's rotation.
	struct Flat
	{
		float x;
		float y;
	};

	Flat Rotate(Flat v, float degrees)
	{
		const float r = degrees * 3.14159265f / 180.0f;
		return { v.x * std::cos(r) - v.y * std::sin(r), v.x * std::sin(r) + v.y * std::cos(r) };
	}

	Flat Unit(Flat from, Flat to)
	{
		const float dx = to.x - from.x;
		const float dy = to.y - from.y;
		const float length = std::sqrt(dx * dx + dy * dy);
		return { dx / length, dy / length };
	}

	Flat Place(Flat at, Flat direction, float distance)
	{
		return { at.x + direction.x * distance, at.y + direction.y * distance };
	}

	// +1 or -1 for the side of the a->b line that p is on.
	int SideOfLine(Flat a, Flat b, Flat p)
	{
		return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) > 0.0f ? 1 : -1;
	}

	// +1 if a camera behind `person` is over their right shoulder, -1 for left.
	// Right is Cross(facing, up), as in the solver.
	int Shoulder(Flat person, Flat facing, Flat camera)
	{
		const Flat right{ facing.y, -facing.x };
		return (camera.x - person.x) * right.x + (camera.y - person.y) * right.y > 0.0f ? 1 : -1;
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

	// True 180 rule.
	Expect(LineSide(1.0f, true, true) == 1.0f && LineSide(-1.0f, true, true) == -1.0f,
		"NPC shots keep the scene's sign");
	Expect(LineSide(1.0f, false, true) == -1.0f && LineSide(-1.0f, false, true) == 1.0f,
		"player shots flip the sign when true180 is on");
	Expect(LineSide(1.0f, false, false) == 1.0f && LineSide(-0.5f, false, false) == -1.0f,
		"with true180 off every shot shares the scene's sign");

	// Place both cameras the way Solve does and check which side of the line
	// they land on, and which shoulder the over-the-shoulders use.
	unsigned stagings = 0;
	for (int heading = 0; heading < 360; heading += 15) {
		const Flat npc{ 30.0f, -40.0f };
		const Flat player = Place(npc, Rotate({ 1.0f, 0.0f }, static_cast<float>(heading)), 110.0f);
		const Flat toPlayer = Unit(npc, player);
		const Flat toNpc = Unit(player, npc);
		const float standoff = 110.0f + 60.0f;  // an OTS stands past the other person

		for (float side : { 1.0f, -1.0f }) {
			for (bool true180 : { false, true }) {
				for (float angle : { 8.0f, 15.0f, 20.0f, 26.0f, 40.0f, 72.0f, 90.0f, 165.0f }) {
					const Flat onNpc = Place(npc, Rotate(toPlayer, angle * LineSide(side, true, true180)), standoff);
					const Flat onPlayer = Place(player, Rotate(toNpc, angle * LineSide(side, false, true180)), standoff);
					const bool sameSide = SideOfLine(player, npc, onNpc) == SideOfLine(player, npc, onPlayer);
					Expect(sameSide == true180, true180 ?
						"true180: both cameras on the same side of the line" :
						"true180 off: the reverse lands on the far side");

					if (angle >= 15.0f && angle <= 28.0f) {
						const bool opposite = Shoulder(player, toNpc, onNpc) != Shoulder(npc, toPlayer, onPlayer);
						Expect(opposite == true180, true180 ?
							"true180: over-the-shoulders use opposite shoulders" :
							"true180 off: over-the-shoulders use the same shoulder");
					}
				}
				++stagings;
			}
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
	std::cout << "Shot angles: " << windows << " windows, " << stagings
			  << " line-side stagings, plus portrait, obstruction and held-pose cases passed.\n";
}

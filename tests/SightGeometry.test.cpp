#include "SD/Camera/SightGeometry.h"

#include <cstdlib>
#include <iostream>

using SD::Camera::SightState;
namespace G = SD::Camera::SightGeometry;

int main()
{
	int checks = 0;
	const auto require = [&](bool a_condition, const char* a_name) {
		++checks;
		if (!a_condition) {
			std::cerr << "FAIL: " << a_name << '\n';
			std::exit(1);
		}
	};
	const G::Point subject{ 0.0f, 0.0f, 100.0f };
	const G::Point camera{ 0.0f, 100.0f, 100.0f };
	const auto hits = [&](G::Point a_from, G::Point a_to, float a_radius) {
		return G::SegmentHitsCapsule(subject, camera, a_from, a_to, a_radius);
	};

	require(hits({ 0.0f, 50.0f, 60.0f }, { 0.0f, 50.0f, 120.0f }, 8.0f), "body over the face blocks");
	require(!hits({ 20.0f, 50.0f, 60.0f }, { 20.0f, 50.0f, 120.0f }, 8.0f), "foreground beside face is allowed");
	require(!hits({ 0.0f, 50.0f, 0.0f }, { 0.0f, 50.0f, 70.0f }, 8.0f), "counter or actor below face is allowed");
	require(!hits({ 0.0f, 50.0f, 140.0f }, { 0.0f, 50.0f, 190.0f }, 8.0f), "vertically separated actor is allowed");
	require(!hits({ 0.0f, 120.0f, 60.0f }, { 0.0f, 120.0f, 120.0f }, 8.0f), "object behind lens is allowed");
	require(!hits({ 0.0f, -20.0f, 60.0f }, { 0.0f, -20.0f, 120.0f }, 8.0f), "object behind subject is allowed");
	require(hits({ 0.0f, 97.0f, 100.0f }, { 0.0f, 97.0f, 100.0f }, 2.0f), "no close-up blind zone near lens");
	require(hits({ 0.0f, 3.0f, 100.0f }, { 0.0f, 3.0f, 100.0f }, 2.0f), "no blind zone near subject");
	require(!hits({ 10.0f, 0.0f, 100.0f }, { 10.0f, 100.0f, 100.0f }, 8.0f), "parallel capsule beside sightline is allowed");
	require(hits({ -20.0f, 50.0f, 100.0f }, { 20.0f, 50.0f, 100.0f }, 1.0f), "horizontal seated or prone body blocks");
	require(G::SegmentHitsCapsule(camera, camera, { 0.0f, 100.0f, 100.0f }, { 0.0f, 100.0f, 100.0f }, 6.0f), "degenerate lens sphere inside actor blocks");
	require(!G::SegmentHitsCapsule(camera, camera, { 8.0f, 100.0f, 100.0f }, { 8.0f, 100.0f, 100.0f }, 6.0f), "lens sphere ignores remote body");
	const std::array<G::Point, 5> faceSamples{
		subject, G::Point{ 6.5f, 0.0f, 100.0f }, { -6.5f, 0.0f, 100.0f },
		{ 0.0f, 0.0f, 108.0f }, { 0.0f, 0.0f, 92.0f }
	};
	bool harmlessForeground = true;
	bool centeredForeground = true;
	for (const auto sample : faceSamples) {
		harmlessForeground &= !G::SegmentHitsCapsule(sample, camera,
			{ 5.0f, 90.0f, 95.0f }, { 5.0f, 90.0f, 105.0f }, 1.0f);
		centeredForeground &= G::SegmentHitsCapsule(sample, camera,
			{ 0.0f, 90.0f, 95.0f }, { 0.0f, 90.0f, 105.0f }, 1.0f);
	}
	require(harmlessForeground, "rays converge at lens and ignore side foreground near camera");
	require(centeredForeground, "same narrow object moved directly over NPC covers face samples");
	require(std::isnan(G::SegmentDistanceSquared({ std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f }, camera, subject, camera)), "nonfinite geometry remains invalid");

	const auto frame = G::MakeFrame({ 0.0f, 0.0f, 0.0f }, { 0.0f, 100.0f, 0.0f }, 90.0f, 2.0f, 0.0f);
	const auto cropped = G::MakeFrame({ 0.0f, 0.0f, 0.0f }, { 0.0f, 100.0f, 0.0f }, 90.0f, 2.0f, 0.10f);
	require(frame.valid && G::InFrame(frame, { 0.0f, 100.0f, 0.0f }), "face at aim is in frame");
	require(G::InFrame(frame, { 0.0f, 100.0f, 45.0f }), "face before letterbox crop is visible");
	require(!G::InFrame(cropped, { 0.0f, 100.0f, 45.0f }), "letterbox crop excludes hidden face");
	require(!G::InFrame(frame, { 110.0f, 100.0f, 0.0f }), "clear line toward offscreen subject does not qualify");
	require(!G::InFrame(frame, { 0.0f, -100.0f, 0.0f }), "subject behind camera does not qualify");
	const auto overhead = G::MakeFrame({ 0.0f, 0.0f, 100.0f }, { 0.0f, 0.0f, 0.0f }, 60.0f, 1.77778f, 0.0f);
	require(overhead.valid && G::InFrame(overhead, { 6.0f, 8.0f, 0.0f }), "direct overhead retains both projection axes");
	require(!G::MakeFrame(subject, subject, 90.0f, 2.0f, 0.0f).valid, "zero camera direction is unknown");
	require(!G::MakeFrame(subject, camera, 90.0f, 0.0f, 0.0f).valid, "invalid aspect is unknown");
	require(!G::MakeFrame(subject, camera, 90.0f, 2.0f, 0.5f).valid, "invalid crop is unknown");

	constexpr auto C = SightState::kClear;
	constexpr auto B = SightState::kBlocked;
	constexpr auto U = SightState::kUnknown;
	require(G::FaceState({ C, C, C, C, C }) == C, "five clear face samples qualify");
	require(G::FaceState({ C, C, C, C, B }) == C, "four including center qualify");
	require(G::FaceState({ B, C, C, C, C }) == B, "covered face center fails");
	require(G::FaceState({ C, C, C, B, B }) == B, "only three clear samples fail");
	require(G::FaceState({ C, C, C, C, U }) == U, "query failure never becomes verified-clear");
	require(G::FaceState({ U, C, C, C, C }) == U, "unknown center cannot qualify");
	require(G::FaceState({ U, U, U, B, B }) == B, "known blocked majority fails despite unknown readings");
	require(G::FaceState({ U, U, U, U, U }) == U, "missing physics remains unknown");

	std::cout << "Passed " << checks << " subject visibility geometry checks.\n";
}

#include "SD/Camera/VisibilityRecovery.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "VisibilityRecovery: " << a_message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	using SD::Camera::VisibilityRecovery;

	VisibilityRecovery partial;
	Expect(!partial.Observe(0.0, false, false, false, true),
		"partial obstruction starts its grace period at time zero");
	Expect(!partial.Observe(0.125, false, false, false, true),
		"brief partial obstruction does not interrupt a verified shot");
	Expect(partial.Observe(0.2, false, false, false, true),
		"persistent partial obstruction requests recovery at 200 ms");
	Expect(!partial.Observe(0.25, true, false, false, true),
		"a clear observation cancels the failure interval");
	Expect(!partial.Observe(1.0, false, false, false, true),
		"new obstruction receives a fresh interval after clearance");
	Expect(!partial.Observe(1.125, false, false, false, true),
		"previous failure time does not accumulate across clearance");
	Expect(partial.Observe(1.25, false, false, false, true),
		"a fresh persistent obstruction eventually requests recovery");

	VisibilityRecovery transient;
	Expect(!transient.Observe(0.0, false, false, false, true),
		"transient obstruction starts its interval");
	Expect(!transient.Observe(0.125, true, false, false, true),
		"a transient blocker clears without recovery");
	Expect(!transient.Observe(1.0, true, false, false, true),
		"continued clearance never requests recovery");

	VisibilityRecovery severe;
	Expect(severe.Observe(0.0, false, true, false, true),
		"severe obstruction bypasses the partial grace period");
	Expect(!severe.Observe(0.125, true, true, true, false),
		"clear is authoritative and resets even contradictory failure flags");

	VisibilityRecovery unknown;
	Expect(!unknown.Observe(0.0, false, false, true, true),
		"unknown query may briefly retain a verified pose");
	Expect(!unknown.Observe(0.25, false, false, true, true),
		"unknown grace is longer than partial obstruction grace");
	Expect(unknown.Observe(0.5, false, false, true, true),
		"unavailable queries cannot retain a pose past 500 ms");
	Expect(!unknown.Observe(0.75, true, false, false, true),
		"healthy clearance resets unavailable-query timing");
	Expect(!unknown.Observe(1.0, false, false, true, true),
		"later unknown query starts a fresh bounded interval");

	VisibilityRecovery firstPose;
	Expect(firstPose.Observe(0.0, false, false, false, false),
		"an unverified first pose cannot use partial obstruction grace");
	firstPose.Reset();
	Expect(firstPose.Observe(0.0, false, false, true, false),
		"an unverified first pose cannot use unavailable-query grace");

	VisibilityRecovery fallback;
	Expect(!fallback.Ready(10.0, true, true),
		"recovery cannot complete before fallback entry");
	fallback.EnterFallback(0.0);
	Expect(!fallback.Ready(0.0, true, true),
		"fallback starts clear timing correctly at time zero");
	Expect(!fallback.Ready(0.75, true, true),
		"clear dwell alone cannot bypass the minimum residence");
	Expect(!fallback.Ready(2.0, true, false),
		"eligible clearance waits for a dialogue boundary");
	Expect(fallback.Ready(2.0, true, true),
		"sustained clearance and residence permit recovery at a boundary");

	fallback.EnterFallback(10.0);
	Expect(!fallback.Ready(12.0, true, true),
		"residence alone cannot bypass the clear dwell");
	Expect(!fallback.Ready(12.5, true, true),
		"a half-second clear interval is insufficient");
	Expect(fallback.Ready(12.75, true, true),
		"750 ms of uninterrupted clearance satisfies clear dwell");
	Expect(!fallback.Ready(13.0, false, true),
		"new obstruction revokes recovery eligibility");
	Expect(!fallback.Ready(13.25, true, true),
		"clearance starts over after a new obstruction");
	Expect(!fallback.Ready(13.75, true, true),
		"old clear time does not survive a new obstruction");
	Expect(fallback.Ready(14.0, true, true),
		"recovery becomes eligible after a fresh complete clear interval");

	VisibilityRecovery reset;
	Expect(!reset.Observe(0.0, false, false, false, true),
		"reset case begins with a pending failure");
	reset.Reset();
	Expect(!reset.Observe(1.0, false, false, false, true),
		"Reset removes the pending failure interval");
	reset.EnterFallback(2.0);
	Expect(!reset.Ready(2.0, true, true), "reset case begins clear dwell");
	reset.Reset();
	Expect(!reset.Ready(5.0, true, true),
		"Reset removes fallback membership and clearance timing");
	reset.EnterFallback(6.0);
	Expect(!reset.Ready(8.0, true, true),
		"fresh fallback has no inherited clear dwell");
	Expect(reset.Ready(8.75, true, true),
		"fresh fallback recovers after its own dwell");
	Expect(!reset.Observe(9.0, false, false, true, true),
		"fallback entry also clears any previous observation failure");

	std::cout << "VisibilityRecovery timing tests passed\n";
	return EXIT_SUCCESS;
}

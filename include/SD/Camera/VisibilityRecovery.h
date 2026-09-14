#pragma once

namespace SD::Camera
{
	// Camera-independent timing policy. Callers supply monotonic seconds and
	// decide how to obtain a new shot or enter/leave the engine's camera states.
	struct VisibilityRecovery
	{
		void Reset()
		{
			failing = false;
			failureSince = 0.0;
			inFallback = false;
			fallbackSince = 0.0;
			clearing = false;
			clearSince = 0.0;
		}

		// Return true when obstruction recovery is due. A verified shot may
		// tolerate a brief partial obstruction or unavailable query, but a severe
		// obstruction and an unverified starting pose have no grace period.
		[[nodiscard]] bool Observe(double a_now, bool a_clear, bool a_severe,
			bool a_unknown, bool a_hasVerifiedPose)
		{
			if (a_clear) {
				failing = false;
				failureSince = 0.0;
				return false;
			}
			if (!failing) {
				failing = true;
				failureSince = a_now;
			}
			if (!a_hasVerifiedPose || a_severe) {
				return true;
			}
			return a_now - failureSince >= (a_unknown ? 0.5 : 0.2);
		}

		void EnterFallback(double a_now)
		{
			Reset();
			inFallback = true;
			fallbackSince = a_now;
		}

		// Return only at a dialogue boundary after both the fallback residence
		// and a continuous clear interval. Booleans make time zero a valid start.
		[[nodiscard]] bool Ready(double a_now, bool a_fullyClear, bool a_boundary)
		{
			if (!inFallback) {
				return false;
			}
			if (!a_fullyClear) {
				clearing = false;
				clearSince = 0.0;
				return false;
			}
			if (!clearing) {
				clearing = true;
				clearSince = a_now;
			}
			return a_boundary && a_now - fallbackSince >= 2.0 &&
			       a_now - clearSince >= 0.75;
		}

	private:
		bool failing{ false };
		double failureSince{ 0.0 };
		bool inFallback{ false };
		double fallbackSince{ 0.0 };
		bool clearing{ false };
		double clearSince{ 0.0 };
	};
}

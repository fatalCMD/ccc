#pragma once

namespace SD::Compat
{
	// SmoothCam writes the camera every frame, and the pose override writes the
	// same transform. Racing it would mean whichever mod ran last that frame won,
	// which looks like a camera that judders between two answers.
	//
	// SmoothCam publishes an interface for exactly this handoff, so Scene Director
	// asks for the camera when a conversation opens and gives it back when the
	// conversation ends. SmoothCam keeps working everywhere else, and the user's
	// presets are untouched.
	class SmoothCam
	{
	public:
		// Callback registration must happen at plugin load; the interface itself
		// only becomes available once SmoothCam has answered.
		static void Register();
		static void Request();

		[[nodiscard]] static bool Present() noexcept;

		// True when Scene Director may write the camera.
		//
		// Also true when SmoothCam is simply not installed — there is then nothing
		// to negotiate with. False means another consumer holds the camera, in
		// which case the correct behaviour is to decline to stage the conversation
		// rather than fight for the transform.
		[[nodiscard]] static bool Acquire();
		static void                Release();
		[[nodiscard]] static bool  Holding() noexcept;
	};
}

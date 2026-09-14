#pragma once

namespace SD::Compat
{
	// Improved Camera 1.1.x publishes no handshake, so this is the whole of the
	// negotiation: know it is there, and stay off the fields it drives.
	//
	// SCOPED TO 1.1.x ON PURPOSE. Measured on 1.1.2.4228, the build this was
	// written against: its exports are SKSEPlugin_Load, SKSEPlugin_Query and
	// SKSEPlugin_Version and nothing else, and the api headers in its tree are
	// ones it CONSUMES (SmoothCam, True Directional Movement) rather than one it
	// offers. Later generations are reported to expose camera ownership to other
	// plugins; if 2.x becomes a support target, the right move is to negotiate
	// with it rather than to go on inferring from its config file. Do not restate
	// this as a timeless fact about the mod.
	//
	// SmoothCam is the easy case and the reason this file reads as thin by
	// comparison. It exposes RequestCameraControl and ReleaseCameraControl, so one
	// mod owns the transform at a time and neither has to guess — see
	// Compat::SmoothCam, which is a real conversation between two plugins.
	//
	// Improved Camera has no such interface. It is a first-person body mod, and
	// the way it moves between first and third person is by driving the
	// THIRD-PERSON CAMERA STATE'S ZOOM: targetZoomOffset, currentZoomOffset,
	// savedZoomOffset and pitchZoomOffset are its transition, frame by frame.
	//
	// Those are four of the nine fields Director::RestoreCameraRest writes when a
	// conversation ends. Written while Improved Camera is mid-transition — or
	// simply written at all, since savedZoomOffset is the value the camera keeps —
	// they are a second author on its animation, and what a player sees is the
	// view pumping in and out. That is the reported symptom, and there is no API
	// to ask permission with, so the only correct move is not to write them.
	//
	// Detection only. Nothing here hooks, patches or calls into Improved Camera.
	class ImprovedCamera
	{
	public:
		// Called once at startup, alongside the conflict report, and idempotent so
		// a second call cannot re-log.
		//
		// Also READS Improved Camera's own profile, and this is the more useful
		// half. Skyrim takes movement controls away during dialogue, which Improved
		// Camera reads as a scripted third-person event, so its [EVENTS] bScripted
		// setting decides whether it fakes first person over every conversation
		// this mod stages. That is one ini key between "these two work together"
		// and "the camera flips between the shot and the player's eyes", and it is
		// knowable at load — so it is said at load, by name, with the file path.
		// Compat::ReportKnownConflicts makes the same argument at more length.
		static void Detect();

		// Read on the conversation-end path, so it must be cheap: the module
		// lookup happens once in Detect and this reads the answer it recorded.
		[[nodiscard]] static bool Present() noexcept;
	};
}

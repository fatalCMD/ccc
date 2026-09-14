#pragma once

namespace RE
{
	class PlayerCamera;
}

namespace SD::Runtime
{
	void Initialize();

	// Called once per frame from the PlayerCamera::Update hook, after the game's
	// own camera update has run.
	void OnFrame(RE::PlayerCamera* a_camera, float a_delta);

	// Ticks observed since load. Read by the engine-driven menu watch to prove
	// whether the frame source is actually per-frame.
	[[nodiscard]] std::uint64_t FrameCount() noexcept;

	// The conversation Runtime believes it has staged is no longer staged, so let
	// it be staged again.
	//
	// Runtime opens once per partner and will not reopen while that key still
	// matches, because the branch that used to reopen on any release fought the
	// exit path: leaving mid-line releases the director while the session runs on
	// through the NPC's trailing line, and it was reopened, released and reopened
	// once a second until they stopped talking.
	//
	// That rule is right and stays. This is how the one release that genuinely
	// expects to come back says so — the director handing the screen to a menu a
	// dialogue topic opened, with the same conversation still waiting underneath
	// it.
	void RearmConversation();

	void OnGameLoaded();

	// A load is starting. Drop everything that refers to the outgoing world,
	// synchronously, on this thread.
	void AbandonForLoad();
}

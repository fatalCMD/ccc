#pragma once

namespace SD::Scene
{
	// A hook on the thing that actually applies facegen morphs to a head.
	//
	// Every reading before this one came from a side channel — cull flags, the
	// morph budget, BSFaceGenNiNode::lastTime, and the phoneme keyframe values —
	// and every one of them failed. The phoneme channel in particular was read
	// three different ways across one session (it drives the mouth, it does not
	// drive the mouth, it reads inverted) and none of the three survived the next
	// run. A threshold calibrated on two conversations called a frozen mouth
	// "APPLIED" twice.
	//
	// So this stops inferring. BSFaceGenNiNode overrides UpdateDownwardPass at
	// vfunc 0x2C, and that override is where animationData is walked onto the
	// head's morphs. Sitting on it answers, per frame and without interpretation:
	//
	//   does the morph pass run on the player's head at all
	//   how often, relative to the NPC whose mouth demonstrably works
	//   what NiUpdateData::time it receives
	//   whether it advances lastTime — i.e. whether it did anything
	//   whether it CONSUMES the phoneme keyframe (isUpdated true -> false)
	//
	// That last one is the question nobody has answered. isUpdated is set by
	// BSFaceGenKeyframeMultiple::SetValue and cleared by whoever reads the
	// keyframe. Watching it flip across the call identifies the consumer directly.
	//
	// Cheap, despite this note: RE::VTABLE_BSFaceGenNiNode is already in
	// Offsets_VTABLE.h, so it is a write_vfunc in the same shape as Core/Tick.
	// The declaration is compiled out of the header under SKYRIM_CROSS_VR, hence
	// the free-function thunk; and 0x2C is a flat-Skyrim slot, so the install is
	// guarded the same way SKSEPlugin_Load already refuses VR.
	class FaceGen
	{
	public:
		static void Install();

		[[nodiscard]] static bool Installed() noexcept;

		// Pin one viseme slot open on the player's head. -1 off, 0-15 a slot.
		//
		// The one measurement this investigation has never taken: put a value in
		// the phoneme channel that cannot be argued with, immediately before the
		// pass that consumes it, and look at the face. A mouth held open says the
		// channel reaches the geometry and the fault is upstream in what feeds the
		// viseme track; a mouth that does not move says it never reaches the
		// geometry and the fault is on the head. Nothing else splits those two.
		//
		// Needs no conversation and no voice mod, so none of the confounders in
		// LIPSYNC.md §7 apply to it.
		static void SetForcedViseme(int a_slot);

		// Begin a counting window. Called when a conversation opens.
		static void Begin(RE::Actor* a_npc);

		// End the window and print the comparison.
		static void End();

		// Per-second summary while a window is open, driven from the frame tick so
		// the cadence does not depend on how often heads happen to be updated.
		static void Tick(float a_delta);
		// Synchronous handback before load: only modifier values still owned by SD.
		static void ReleaseModifiers();
	};
}

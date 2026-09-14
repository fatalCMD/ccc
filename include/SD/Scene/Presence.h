#pragma once

namespace SD::Scene
{
	// Makes the player behave like someone in a conversation.
	//
	// Vanilla never turns player headtracking on in third person during dialogue —
	// the graph variable is simply false — so the player character stares straight
	// ahead while an NPC talks to the side of their face. Nothing is broken; the
	// behaviour was never implemented, because vanilla's dialogue camera pointed
	// away from the player and nobody could see it.
	//
	// The moment the camera turns around, it is the most obvious thing on screen.
	class Presence
	{
	public:
		static void Engage(RE::Actor* a_npc);

		// Re-assert, every frame, for the whole conversation.
		//
		// Engage alone was not enough and the symptom was "the player doesn't
		// really look at the NPC". The headtrack target is not a setting, it is a
		// slot the engine and every AI package write to constantly — a package
		// change, a combat check, or the dialogue system's own headtracking will
		// take it back, and once taken it is never given again. Performance's gaze
		// model then nudges an OFFSET around a target that no longer points at
		// anybody, which looks exactly like the model not working.
		//
		// Same lesson as the topic list's _visible and the letterbox: a value
		// written once is one the engine quietly undoes and leaves undone.
		static void Update(float a_delta);

		static void Release();
	};
}

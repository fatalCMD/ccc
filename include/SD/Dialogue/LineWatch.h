#pragma once

namespace SD::Dialogue
{
	// The real line signal.
	//
	// `TESObjectREFR::UpdateInDialogue(DialogueResponse*, bool)` is virtual 0x4C,
	// overridden by Actor, and the engine calls it on the actor that is speaking
	// with the response it is speaking. That is strictly better than polling
	// MenuTopicManager from a frame hook:
	//
	//   - it needs no frame source at all, so it cannot be defeated by a tick that
	//     turns out not to tick;
	//   - it identifies the speaker directly rather than by inference from a
	//     handle that goes null mid-conversation;
	//   - it hands over the DialogueResponse, which carries the emotion, the
	//     intensity, the text, the voice file and both authored idles — the data
	//     the first build had to go looking for and often could not match.
	//
	// Hooked on Character and PlayerCharacter separately. They are distinct
	// vtables and may hold distinct implementations, so one shared trampoline
	// would be a coin flip on which original gets called.
	class LineWatch
	{
	public:
		static void Install();
		[[nodiscard]] static bool Installed() noexcept;
	};
}

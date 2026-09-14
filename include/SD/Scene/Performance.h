#pragma once

namespace SD::Scene
{
	// Player reactions use coordinated upper-face modifiers; mouth lip sync,
	// blinking and gaze retain their existing owners. NPCs use dialogue morphs.
	// NPC records supply authored emotion; explicit English text cues fill in
	// neutral records and player topics. Listener reactions are softer, separate
	// readings. Mouth phonemes, blink and eye-direction channels remain separate.
	class Performance
	{
	public:
		static void Engage(RE::Actor* a_npc);
		static void Release();
		// Synchronous handback before the outgoing world is unloaded; no fade carries across saves.
		static void ResetForLoad();

		// A new line has begun. Emotion and intensity come straight from the
		// response the engine is about to speak.
		static void OnLine(RE::Actor* a_speaker, std::uint32_t a_emotion, std::uint16_t a_percent, std::string_view a_text);

		static void Update(float a_delta, bool a_npcSpeaking);

		// The player's face, driven every frame and ahead of the staging gate.
		//
		// SPLIT FROM Update FOR TWO REASONS, both of which were real bugs.
		//
		// Update is called from deep inside Director::Tick's staging path, after
		// the camera has resolved both anchor points — and it returns early when
		// either fails to resolve. An expression envelope driven from there stalls
		// on exactly the frames the camera is having trouble, which is the worst
		// possible moment for a face to freeze. This runs beside LipSync::Update,
		// ahead of every one of those gates.
		//
		// It also has to keep running after Release, or the ease-out never
		// completes and the last expression of a conversation is left stamped on
		// the player's face with exprOverride still raised. LipSync::Release has
		// the same contract and the same comment.
		static void UpdateFace(float a_delta);

		// A synchronized snapshot consumed immediately before the face morph.
		// a_player=false selects the current conversation partner's envelope.
		[[nodiscard]] static bool SampleExpression(float* a_out, std::uint32_t a_count,
			bool a_player = true) noexcept;
		[[nodiscard]] static bool SampleUpperFace(float* a_out, std::uint32_t a_count) noexcept;
		[[nodiscard]] static bool SampleListenerExpression(float* a_out, std::uint32_t a_count) noexcept;
		[[nodiscard]] static bool SampleRegionalFace(std::array<float, 8>& a_out) noexcept;

		// The emotion currently read off the player's own line, as a
		// RE::DialogueResponse::EmotionType value.
		//
		// A semantic reading only; expression profiles own all normal brow motion.
		[[nodiscard]] static std::uint32_t PlayerEmotion() noexcept;

		static void Configure(bool a_expressions, bool a_gaze);
		[[nodiscard]] static bool ExpressionsEnabled() noexcept;
		// Includes the coordinated expression's release tail.
		[[nodiscard]] static bool PlayerExpressionActive() noexcept;

		// Pin one expression slot wide open on the player. -1 off, 0-16 a slot.
		//
		// The measurement that splits the remaining search space, and the exact
		// counterpart of FaceGen::SetForcedViseme. As of the 14:06 run the picks
		// are right, the strengths are right, the channel demonstrably carries
		// SD's values on SD's envelope — and the face does not visibly move. A
		// face held in a full grimace tells you which half of that is wrong
		// without anyone having to judge a subtle expression by eye.
		//
		// Needs no conversation, no staging and no voice mod, so none of the
		// dialogue-side confounders apply to it.
		static void SetForcedExpression(int a_slot);

		// Keep the player's head animating while it is off camera.
		//
		// The engine will not morph a face it is not showing. That is fine in
		// vanilla, where the player has no voice and their head is behind the
		// camera anyway — and it is why this mod exposed the problem: SD is the
		// first thing that ever cuts to the player's face mid-line, so it is the
		// first thing for which "the head was not on screen a moment ago" has a
		// visible consequence.
		//
		// Reported behaviour, 2026-08-03: with the camera on the player's face the
		// lips move; with the camera at their back they do not. This forces the
		// head to stay in the drawn set for the length of a conversation so the
		// morph keeps running through the shots that face away, and the mouth is
		// already correct on the frame a reverse lands rather than catching up
		// after it.
		//
		// Third person only, and restored exactly on Release — the flags are put
		// back to what they were, not cleared, because a heavy load order has other
		// mods with opinions about the player's head.
		static void HoldPlayerFace(bool a_hold);

		// Fraction of the time each party holds eye contact. The GAP between them
		// is the model; setting them equal turns the exchange back into two
		// characters staring at each other. Separate from Configure so the in-game
		// menu can move it live rather than at the next conversation.
		static void SetGaze(float a_listenerHold, float a_speakerHold);

		// Diagnostic: report both participants' facegen state twice a second.
		//
		// Exists to settle the player-lipsync question, which is currently answered
		// wrongly in HANDOFF.md. That note reasons from `UpdateInDialogue` never
		// firing on PlayerCharacter to "no .lip data is ever attached, so the
		// phoneme channel is never driven, so fixing it means inventing lipsync".
		// The second step does not follow, and a field report that the player DOES
		// lipsync while seated contradicts it outright — a channel that moves under
		// any condition is a channel that is being fed.
		//
		// The NPC is sampled alongside the player deliberately. Their mouth
		// demonstrably works, so their numbers are the control: without them, all
		// zeroes on the player cannot be told apart from a probe reading the wrong
		// field.
		static void ConfigureProbe(bool a_probeFace);

		// Run the probe against an explicit NPC, without requiring Engage.
		//
		// Separate from Update for one reason, and it is the whole point: the
		// question on the table is whether SD's own camera is what stops the
		// player's face animating, and IACC — which does not — is the control.
		// A probe that only runs while SD is staging cannot measure the case
		// where SD is staging nothing, so it could never see the control at all.
		// Driven from Director::Tick, which runs whether or not staging is on.
		static void Probe(RE::Actor* a_npc, float a_delta);
	};
}

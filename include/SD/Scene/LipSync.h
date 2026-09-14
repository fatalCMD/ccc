#pragma once

namespace SD::Scene
{
	// Drive the player's mouth directly, instead of asking the engine to.
	//
	// WHY THIS EXISTS, and it is now measured rather than inferred.
	//
	// Under a player-voice mod the player's line is played with
	// `Player.SpeakSound "DBVO/<pack>/<line>.fuz"`. The audio plays; the `.lip`
	// track embedded in that same file is never decoded or applied. Measured
	// 2026-08-13 over a whole conversation, on a line the player audibly spoke:
	//
	//   PLAYER  peakMax=0.400  slotsLit=1/16 (0x0020)
	//   npc     peakMax=0.000  slotsLit=0/16 (0x0000)
	//
	// One slot out of sixteen ever held a value, it sat at a constant 0.050 for
	// the whole of the player's own line, and it belongs to Conditional
	// Expressions / Expressive Facial Animation rather than to speech. Real speech
	// lights eight to fourteen. There is no viseme track. Nothing is being blocked
	// downstream — nothing was ever generated.
	//
	// And the channel demonstrably WORKS: forcing slot 0 to 1.0 through the morph
	// hook held the player's mouth visibly wide open, with no conversation and no
	// voice mod involved. So there is nothing to repair and everything to supply.
	//
	// This is what every shipping mod in this space already does. Open Your Mouth
	// calls its version "fake lipsyncing" and recommends itself specifically for
	// Alternate Conversation Camera plus Dragonborn Voice Over; crajjjj/AudioUtil
	// drives the same two phoneme slots from an amplitude envelope with the same
	// 0.35 spill between them. Both work.
	//
	// The camera is not involved, and that is settled: the morph pass runs on the
	// player's head 342 times out of 342, identical to the NPC's, while SD is
	// directing. See LIPSYNC.md.
	class LipSync
	{
	public:
		// a_strength is 0..100 from the ini and scales the jaw opening, not the
		// cadence. Calibrated: slot 0 at 1.0 is a mouth hanging completely open, so
		// this is a real ceiling rather than an arbitrary gain.
		//
		// THE JAW TAKES IT LINEARLY AND THE LIPS DO NOT, which matters because for
		// a while they did and the mouth read as stiff for it. The shape table puts
		// the identity of each sound in the LIPS and keeps the jaw low — Ooh and W
		// and F sit at 0.95-1.00 against jaw values of 0.10-0.24 — so scaling all
		// sixteen slots by a jaw control cut the most recognisable shapes by a
		// third at the default setting. Lips now take sqrt(strength): same at 0,
		// same at 100, more generous everywhere between. See Sample.
		static void Configure(bool a_enabled, int a_strength);

		[[nodiscard]] static bool Enabled() noexcept;

		// The live values, for the settings panel to draw from.
		//
		// The panel must NOT re-read these from the ini each frame. Slider() only
		// persists on release, by design — a full-range drag would otherwise write
		// to disk hundreds of times — so an ini read gives back the pre-drag value
		// on every frame of the gesture and the widget snaps home under the cursor.
		// Every slider that works in this panel reads live memory; these are that.
		[[nodiscard]] static int StrengthPercent() noexcept;

		static void Engage();
		static void Release();

		// Per-frame. Driven from Director::Tick, which runs whether or not staging
		// is on: a conversation where SD is directing nothing is still one where the
		// player speaks, and the feature has to be testable with bEnabled=0.
		//
		// This only COMPUTES the envelope. The write happens in Scene::FaceGen, for
		// the reason on Sample.
		static void Update(float a_delta);

		// Main-thread voice context shared with full-face expressions. This follows
		// DBReV/DBVO line events and measured audio duration even with synthesis off.
		[[nodiscard]] static bool PlayerSpeaking() noexcept;
		[[nodiscard]] static std::uint64_t PlayerLineSerial() noexcept;
		[[nodiscard]] static std::string_view PlayerLineText() noexcept;
		[[nodiscard]] static float PlayerLineDuration() noexcept;
		// Read-only clock for facial acting; no change to mouth scheduling.
		[[nodiscard]] static float PlayerLineElapsed() noexcept;
		static void OnResponse();


		// This frame's mouth shape, written into a_out. False when SD is not driving.
		//
		// A SHAPE, not a jaw amount. Driving slots 0 and 1 alone — both of which are
		// the jaw hinge at different sizes — makes every syllable the same movement
		// at a different size, which is a chewing motion and reads as one. Speech is
		// mostly the LIPS: rounding, spreading, and closing. So the whole channel is
		// filled, including the slots being set to zero, because a value another mod
		// parked in an unused slot is part of the face whether SD put it there or not.
		//
		// Split from Update deliberately. Conditional Expressions and Expressive
		// Facial Animation both drive MFG and were measured parking values in this
		// very channel, so a value written from the frame tick can be overwritten
		// before the morph pass consumes it — and would be, silently, on exactly the
		// heavily-modded load orders this mod is built for. FaceGen's hook sits on
		// BSFaceGenNiNode::UpdateDownwardPass and writes immediately before the
		// original runs, which is the one point in the frame where nothing else can
		// get in afterwards. That placement is not a guess: it is how the forced
		// viseme test drove the mouth wide open through the same channel those two
		// mods were occupying at the time.
		[[nodiscard]] static bool Sample(float* a_out, std::uint32_t a_count) noexcept;

	};
}

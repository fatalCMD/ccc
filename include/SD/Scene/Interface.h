#pragma once

namespace SD::Scene
{
	// Clears the HUD for the duration of a conversation, keeping the parts the
	// game is using to TELL the player something.
	//
	// A KEEP-LIST, NOT A BLOCK-LIST, and that swap is the whole of this class's
	// history. Two approaches were shipped before it and both were wrong in the
	// same direction:
	//
	//   - By name. A list of elements to hide, guessed from vanilla. It survives a
	//     HUD replacer only by luck: measured against the three hudmenu.swf files
	//     on this profile (SkyHUD, Edge UI, Edge UI Explorer Addon), the shipped
	//     list matched three of twenty-nine children. The crosshair, all three
	//     meters, the activate prompt, the clock and the charge meters were left
	//     on screen every conversation, and the log said the hide had worked.
	//
	//   - Wholesale. Hide HUDMenu's root and every child goes with it. It is
	//     perfectly reliable and it takes the notifications too — an item picked
	//     up mid-conversation, a quest objective completing — and those are LOST
	//     rather than deferred, because the movie is what queues them.
	//
	// So neither the compass nor the quest update can be reached by naming the
	// wrong half of the screen. What is hidden is EVERY child of
	// HUDMovieBaseInstance except a short list that is deliberately released, and
	// the children are discovered at runtime by walking the movie's own depths
	// rather than being written down from a swf nobody here is running.
	//
	// The log is the point: every child found is named in it, so an element that
	// is hidden and should not be — or survives and should not — can be moved
	// across the line by editing one array.
	class Interface
	{
	public:
		static void Suppress();
		static void Restore();

		// THE TWO HALVES OF Restore(), SEPARATELY.
		//
		// They used to be one, gated on a single `suppressed` flag, and that was
		// only correct while the HUD hide and the topic-list fade were the same
		// feature — which they were, because the fade was nested inside the HUD
		// hide's setting and could not run without it.
		//
		// They are independent now, and the pairing has to follow. A fade running
		// with the HUD left alone must still hand the list back at the end of the
		// conversation: alpha 0 with _visible false is exactly the invisible,
		// clickable list that is the oldest bug in this mod, and `suppressed` would
		// have been false the whole time, so the old Restore() returned before
		// reaching it.
		static void RestoreHud();
		// Restore only retained display objects owned by the current movie.
		// Pending restoration is never transferred to a replacement menu.
		static void ReleaseChoices();

		// Re-push what Suppress() took away, once per staged frame.
		//
		// Suppression used to be a single write at the top of the conversation, and
		// a single write is one the game quietly undoes. HUDMenu's _visible is
		// rewritten by the engine and by HUD mods on their own schedule, and menus
		// that OPEN mid-conversation — a widget re-registering, a compass coming
		// back after a cell load — were never in the map when the one-shot sweep
		// ran, so they simply appeared over the scene and stayed.
		//
		// This is the same lesson the topic list's _visible lock already carries,
		// applied to the HUD: enforce every frame, sweep for newcomers on a timer.
		static void Enforce();

		// Fades the topic list out once a choice has been made and back in when
		// the next one appears. The list has no business sitting on screen through
		// an NPC's reply — the choice is spent, and it is the largest non-diegetic
		// object in the frame.
		//
		// Only the topic holder is touched; DialogueMenu's subtitles stay.
		//
		// Returns whether the write actually reached the list. False means the
		// movie or the path could not be resolved on this frame — which the fade
		// can ignore, because it runs again next frame, and which ReleaseChoices
		// must not, because it is trying to hand something back. A value of 100
		// releases our fade; it never forces an untouched clip to full opacity.
		static bool SetChoiceAlpha(float a_alpha);


		// What the dialogue movie says it is doing, read from its own state.
		//
		// The engine drives this: it calls NotifyVoiceReady on the movie by name,
		// the movie answers by clearing bAllowProgress and arming a timer to set it
		// again. So the movie holds an authoritative account of dialogue timing
		// that this mod has, until now, been reconstructing from sound handles and
		// MenuTopicManager — both of which cache state the engine stops
		// maintaining, and both of which have latched the fade.
		//
		// Read only. Writing bAllowProgress is how the deleted commit lock worked
		// and that is not what this is.
		enum class MenuPhase : std::uint8_t
		{
			kUnknown = 0,  // no movie, or the member did not resolve
			kGreeting,     // SHOW_GREETING    = 0
			kTopicList,    // TOPIC_LIST_SHOWN = 1
			kTopicClicked, // TOPIC_CLICKED    = 2
			kTransitioning // TRANSITIONING    = 3
		};

		struct DialoguePhase
		{
			MenuPhase phase{ MenuPhase::kUnknown };

			// !bAllowProgress. The movie's own answer to "is a line still running",
			// which no stale BSSoundHandle can corrupt.
			bool lineInFlight{ false };

			// False when nothing could be read. Callers must fall back rather than
			// treat an unreadable movie as a quiet one.
			bool valid{ false };
		};

		[[nodiscard]] static DialoguePhase ReadDialoguePhase();

		// The topic the player has highlighted, as displayed. Empty if it cannot
		// be read.
		//
		// This is the line the player is about to speak, and it is the ONLY place
		// it can be got from. Under a player-voice mod the engine's dialogue state
		// is blank for the whole of the player's turn — currentTopicInfo null,
		// selectedResponseNode empty — and lastSelectedDialogue is worse than
		// useless: it holds the PREVIOUS line, whose .fuz exists, so it passes an
		// on-disk check while dressing the wrong words as the right ones. The
		// movie's own list is correct throughout.
		[[nodiscard]] static std::string ReadSelectedTopic();

		// An identity for whatever rows the topic list is showing right now.
		// 0 means it could not be read — never treat that as a real value.
		//
		// This is the cue the fade always needed and never had. SD decides to
		// bring the list back on a TIMER — during the NPC's last response, so the
		// options are already there as the line lands — but nothing checked that
		// the engine had rebuilt the rows for the new choices yet. So the list
		// returned still showing the answer the player had already picked, and
		// the click that followed made the engine repopulate it, which SD's
		// alpha read-back saw as a clobber and corrected, running the whole fade
		// a second time on the new rows.
		//
		// Comparing this against what was on screen when the choice was spent
		// turns "wait a bit and hope" into "wait until the rows actually change".
		[[nodiscard]] static std::uint64_t ReadTopicListFingerprint();

		[[nodiscard]] static std::string_view Name(MenuPhase a_phase);

		// Hides the vanilla speaker name — the "Arngeir" that Skyrim prints beside
		// the highlighted topic. It is a sibling of the topic list rather than a
		// child, so the choice fade never reached it and it was left floating in an
		// otherwise empty letterboxed frame once the choices went.
		//
		// Set false to let it fade with the topic list instead of vanishing.
		//
		// Applies LIVE, and has to: switching it off is a request to see the name
		// again, and nothing else would put it back until the conversation ended.
		static void SetHideSpeakerName(bool a_hide);
	};
}

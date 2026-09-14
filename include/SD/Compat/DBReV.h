#pragma once

namespace SD::Compat
{
	// Dragonborn ReVoiced tells us when the player speaks, and for how long.
	//
	// WHAT THIS REPLACES, and every one of these was a measured defect rather
	// than a theoretical improvement. From `SceneDirector.log`, 2026-08-18, a
	// single conversation on a DBReV 1.4.4 profile:
	//
	//   1. THE MOUTH RAN ~5x TOO LONG ON EVERY LINE. Every line in the log —
	//      matched and estimated alike — ended on LipSync's eight-second
	//      stuck-handle timeout, ~8.0s after it started, including one SD had
	//      already measured off the .fuz at 1.49s. The soundHandle never reports
	//      the end, so the measured length only ever scaled the phoneme sequence
	//      and never stopped the mouth.
	//
	//   2. SD ANIMATED LINES THE PLAYER NEVER SPOKE. 14 of 30 logged lines found
	//      no .fuz, and those topics genuinely have no recording — they are
	//      modded-follower dialogue and the active pack is 27,731 vanilla lines.
	//      Handle polling cannot tell "the player's voice" from any other sound
	//      on the player, so the mouth moved through all of them.
	//
	//   3. THE FILENAME WAS GUESSED. LipSync's two sanitize heuristics probed
	//      against every pack directory; DBReV hands over the exact path it gave
	//      to SpeakSound.
	//
	// WHAT IT DELIBERATELY DOES NOT REPLACE. `PlayerLineStart` also carries the
	// FUZ's LIP chunk, and that is not the win it looks like. LipSync already
	// reaches those bytes — `FuzSeconds` reads the chunk's size at offset 8 and
	// seeks past it — and skips them because decoding FaceFX is a project in
	// itself, not because they were out of reach. Nothing here parses lipData.
	//
	// Communication is one-way: DBReV pushes, we receive. See DBReV_API.h and the
	// author's INTEGRATION.md.
	class DBReV
	{
	public:
		// Must be called from kPostLoad, not from plugin load.
		//
		// SKSE loads plugins alphabetically, so SceneDirector.dll registers before
		// DBReV.dll and asking that early returns false — a listener that silently
		// never fires. Same reason SmoothCam::Register is deferred.
		static void Register();

		// True when DBReV is in this load order, decided by whether the listener
		// registration was accepted. Not by testing for DBReV.esp: DBVO 2 dropped
		// its own ESP, and the author asks integrators not to sniff plugin names.
		//
		// False means every caller keeps its existing behaviour untouched — the
		// soundHandle polling, the .fuz filename guess, the timeout backstops.
		// This is the DBVO 1 / DBVO 2 path and it is not going away.
		[[nodiscard]] static bool Present() noexcept;

		// One player line, copied out of the broadcast.
		//
		// Copied because every pointer in the message dies when the callback
		// returns, and the callback is on the Papyrus thread while the consumer is
		// on the main thread.
		struct Line
		{
			// When the MOUTH stops. Measured from the FUZ header before playback.
			// 0.0 when DBReV could not measure the file, in which case only
			// totalSeconds means anything and the caller should fall back to its
			// own estimate.
			float audioSeconds{ 0.0f };

			// When the CONVERSATION advances: audioSeconds plus the user's
			// configured post-line delay.
			//
			// NOT interchangeable with audioSeconds, and the gap is a DBReV MCM
			// setting SD cannot read any other way. The author's own example is a
			// user running 1510ms: audio 2.229s, total 3.739s. Drive lips off
			// totalSeconds and the character mouths silently for a second and a
			// half after the voice stops.
			float totalSeconds{ 0.0f };

			// Position in MenuTopicManager::dialogueList, as the engine's
			// TopicClicked delegate reported it.
			std::uint32_t topicIndex{ 0 };

			// Sanitized topic text — the form used to build the filename, so
			// punctuation is already mangled. A fallback only: LipSync reads the
			// movie's own list for the words, which keeps the real punctuation the
			// phoneme builder wants.
			std::string topicKey;
		};

		// The start of a line, if one has arrived since the last call. Consuming.
		//
		// Main thread only. Returns false when nothing new has arrived, which for
		// a DBReV profile is the positive statement "the player is not speaking" —
		// the signal handle polling could never give.
		[[nodiscard]] static bool TakeLineStart(Line& a_out);

		// The end of a line, if one has arrived since the last call. Consuming.
		// a_reason is one of DBReV::kEndReason_* — completed, skipped, superseded.
		[[nodiscard]] static bool TakeLineEnd(std::uint32_t& a_reason);

		// "completed" / "skipped" / "superseded", for the log. Wrapped so callers
		// do not have to include DBReV_API.h just to name a constant.
		[[nodiscard]] static std::string_view EndReasonName(std::uint32_t a_reason);

		// Is a player line open right now — that is, between a start and its end?
		//
		// This is the CONVERSATION window (totalSeconds), not the audio window, so
		// it is the right question for holding a shot or a topic list and the
		// wrong one for driving a mouth.
		[[nodiscard]] static bool Speaking() noexcept;

		// Has DBReV broadcast a single line this session?
		//
		// PRESENT IS NOT THE SAME AS SPEAKING FOR YOU, and the difference is a real
		// configuration rather than a hypothetical: this development profile has
		// DBReV at modlist line 2 and DBVO 2 at line 5, both enabled. DBReV's DLL
		// loads and accepts the listener either way, so Present() is true even when
		// the other framework is the one actually voicing the player — and it is
		// also true with DBReV installed and its voice pack simply set to "off".
		//
		// Committing to the event path in those cases would mean waiting forever
		// for a message nobody is going to send, and the mouth would never move at
		// all. So callers treat DBReV as authoritative only ONCE IT HAS SPOKEN, and
		// use their old path until then. One line of the old behaviour on a profile
		// that is misconfigured anyway, against a feature that silently does
		// nothing.
		[[nodiscard]] static bool EverSpoke() noexcept;

		// Dropped on load and on a fresh game, the same as every other piece of
		// per-conversation state. A line left open across a save reload would
		// otherwise sit there claiming the player is mid-sentence.
		static void Reset();
	};
}

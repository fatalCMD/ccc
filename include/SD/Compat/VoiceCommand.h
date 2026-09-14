#pragma once

namespace SD::Compat
{
	// Learn what the player is being made to say, by reading the command that
	// says it.
	//
	// THE PROBLEM THIS SOLVES, and it was measured rather than suspected.
	//
	// LipSync reads a line's real length out of the .fuz. To find the file it
	// guesses the name from the topic text and then tries that name in every pack
	// directory under Data/Sound/DBVO, taking the first one that exists. On a load
	// order with ONE pack installed that is right by construction. This profile has
	// FIFTEEN, and on 2026-08-18 a session playing `voicebella` measured 5.203s for
	// a line — which is `FearTcbVamp`'s recording of it. voicebella's is 3.947s.
	//
	// A different actor reading the same words at their own pace. That is not a
	// systematic error anything could correct for; it is simply the wrong file.
	//
	// WHY NOT JUST READ THE VOICE MOD'S CONFIG. Because there is nothing to read.
	// DBVO 1, DBVO 2 and DBReV each persist the selection differently, and DBVO 2's
	// `selected_voice_pack.json` is not written anywhere in this install — searched
	// the whole tree, game folder and overwrite included. A detector that depends on
	// a file which may not exist fails exactly when it is needed.
	//
	// WHAT IS ALWAYS TRUE is that the line gets played by
	// `Player.SpeakSound "DBVO/<pack>/<line>.fuz"`. Both DBVO generations build that
	// string — it is in DBVO 2's DLL verbatim — and DBReV logs the identical call.
	// The path is the answer, so this reads the path.
	//
	// HOW, and this is deliberately the least invasive option available. Not a
	// detour on the script compiler, which would sit in front of every console
	// command any mod ever issues. The console command table is data: each entry
	// carries a function pointer, and this swaps the one belonging to SpeakSound
	// for a wrapper that notes the pack and calls the original. Nothing else in the
	// game changes, and a build where the command cannot be found simply does not
	// patch anything.
	//
	// --- AND THE SAME CALL ANSWERS THREE MORE QUESTIONS -----------------------
	//
	// The first version of this kept the pack segment and threw the rest of the
	// string away. The rest of the string is the whole line, and on a profile
	// without DBReV it is the only place these are written down:
	//
	//   1. THE EXACT FILE. Everything downstream of the filename guess goes away:
	//      no Sanitize / SanitizeAlnum heuristics, no probing directories, and no
	//      "no .fuz matched" — which was 14 of 30 lines in the 2026-08-18 log.
	//
	//   2. THE START OF THE LINE. SpeakSound is the call that starts the audio, so
	//      this fires at the line's edge instead of a frame or two later, when a
	//      sound handle happens to appear on the player.
	//
	//   3. THAT THE PLAYER SPOKE AT ALL. Handle polling cannot tell the player's
	//      dialogue line from any other sound playing on the player, so SD drove
	//      the mouth through lines with no recording behind them. A line nobody
	//      asked SpeakSound to play is a line the player did not say.
	//
	// This is the same shape DBReV's API gives, minus the end event and the
	// post-line delay, reconstructed from a call every DBVO generation already
	// makes. DBReV stays the better source where it exists; this is what the other
	// profiles get.
	class VoiceCommand
	{
	public:
		// From kDataLoaded, once the command table exists.
		static void Install();

		// The pack last seen playing, e.g. "voicebella". Empty until a line has
		// actually been spoken — there is no way to know before then, and guessing
		// is what this exists to stop.
		[[nodiscard]] static std::string Pack();

		// One player line, as the SpeakSound call that plays it described it.
		struct Line
		{
			// The argument verbatim, e.g. "DBVO/voicebella/What_is_it_.fuz".
			// Relative to Data/Sound, which is where the engine resolves it from.
			std::string path;

			// The pack segment of the same string, e.g. "voicebella". Carried
			// alongside so a consumer that wants both does not parse it twice.
			std::string pack;
		};

		// The start of a player line, if one has arrived since the last call.
		// Consuming.
		//
		// Main thread only. Returns false when nothing new has arrived, which on a
		// profile where EverHeard() is true is the positive statement "the player
		// is not speaking" — the signal handle polling could never give.
		[[nodiscard]] static bool TakeLineStart(Line& a_out);

		// Has a SpeakSound on the PLAYER been seen this session?
		//
		// THE SAME GUARD DBReV'S EverSpoke IS, and for the same reason. Consumers
		// must not commit to this path until it has actually produced a line: the
		// command may not exist to patch, the voice mod may route around it, and a
		// consumer waiting for an event that never arrives is a mouth that never
		// moves. False means every caller keeps its existing behaviour untouched —
		// the sound-handle polling, the filename guess, the timeout backstops.
		//
		// Latched, and deliberately NOT cleared by Reset: what it answers is "does
		// this load order announce the player's lines", and that does not stop
		// being true because someone loaded a save.
		[[nodiscard]] static bool EverHeard() noexcept;

		// Drop any line that has not been consumed. Called on load, alongside
		// DBReV::Reset, so a line left pending across a save does not open in the
		// new one.
		static void Reset();
	};
}

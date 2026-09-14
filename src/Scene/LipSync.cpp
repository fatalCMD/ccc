#include "SD/Scene/LipSync.h"

#include "SD/Compat/DBReV.h"
#include "SD/Compat/VoiceCommand.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/ExpressionModel.h"
#include "SD/Scene/Interface.h"
#include "SD/Scene/Performance.h"

#include <cmath>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace SD::Scene
{
	namespace
	{
		// Skyrim's facegen rig carries 16 phoneme slots, in the order the Creation
		// Kit lists them:
		//
		//   0 Aah  1 BigAah  2 BMP  3 ChJSh  4 DST  5 Eee  6 Eh  7 FV
		//   8 I    9 K      10 N   11 Oh    12 OohQ 13 R  14 Th 15 W
		//
		// Slot 0 is the jaw hinge and is the one confirmed by direct experiment:
		// forced to 1.0 through the morph hook it held the mouth completely open.
		constexpr std::uint32_t kSlotCount = 16;
		constexpr std::uint32_t kSlotJaw = 0;

		// The shape table. Each entry is how far the jaw drops plus ONE lip slot.
		//
		// The point of the table is the spread of jaw values. A mouth that opens the
		// same way every syllable is chewing; what makes speech read as speech is
		// that most of it barely opens the jaw at all and the LIPS do the work —
		// rounding for Oh and W, spreading for Eee, closing outright for BMP. Note
		// how many entries here sit at or under 0.30 of jaw. The old version ran
		// every syllable at 0.55-1.00 on the hinge alone.
		struct Shape
		{
			float         jaw;
			std::uint32_t slot;
			float         weight;
		};

		constexpr Shape kShapes[] = {
			{ 0.80f, 1, 0.55f },   // BigAah  - the wide one, used sparingly
			{ 0.55f, 6, 0.50f },   // Eh
			{ 0.30f, 5, 0.75f },   // Eee     - spread, jaw nearly shut
			{ 0.26f, 8, 0.60f },   // I
			{ 0.50f, 11, 0.80f },  // Oh      - rounded
			{ 0.24f, 12, 0.85f },  // OohQ    - tight round
			{ 0.16f, 15, 0.70f },  // W       - pursed
			{ 0.04f, 2, 0.90f },   // BMP     - lips closed. Speech needs these.
			{ 0.20f, 7, 0.65f },   // FV
			{ 0.28f, 10, 0.50f },  // N
			{ 0.36f, 3, 0.55f },   // ChJSh
			{ 0.12f, 14, 0.45f },  // Th
			{ 0.34f, 4, 0.50f },   // DST
			{ 0.30f, 9, 0.45f },   // K
			{ 0.22f, 13, 0.55f },  // R
		};

		// --- Text-driven shapes ------------------------------------------------
		//
		// The random table above is the FALLBACK, used only when the line's text
		// could not be read. When it can, the mouth forms the actual words.
		//
		// This is a grapheme-to-viseme pass, not a pronunciation dictionary: it
		// maps letter clusters straight to mouth positions. English spelling being
		// what it is, it gets some words wrong — "though" and "tough" do not end
		// the same way and this treats them as if they might. It will not survive a
		// frame-by-frame comparison with the recording. What it does is put the
		// closures on the m's and p's, the rounding on the o's and w's and the
		// spread on the ee's, which is the entire difference between a mouth saying
		// something and a mouth chewing.
		//
		// Named shapes, so the table below reads as speech rather than as numbers.
		// The lip weights are deliberately near the top of their range and the jaw
		// deliberately low on everything except the open vowels.
		//
		// A jaw hinge is a big, obvious morph; a lip rounding is a subtle one. Give
		// them comparable weights and the jaw wins every frame and every shape reads
		// as "mouth open a bit", which is what "no Oohs, no Fs" means. The shape has
		// to out-vote the hinge to be seen at all.
		// Two rests, because a gap between words is not a gap between sentences.
		//
		// One near-zero rest for both meant the jaw slammed shut at every space —
		// several times a second on ordinary speech — which is half of the flapping
		// on its own. People do not close their mouth between the words of a
		// phrase; they close it at the end of one, and on actual closures like m
		// and p, which the table already handles.
		constexpr Shape kRest = { 0.02f, 0, 0.00f };   // sentence punctuation
		constexpr Shape kSoftRest = { 0.15f, 0, 0.00f };  // between words
		constexpr Shape kAah = { 0.78f, 1, 0.55f };
		constexpr Shape kEh = { 0.48f, 6, 0.65f };
		constexpr Shape kEee = { 0.22f, 5, 0.95f };
		constexpr Shape kIh = { 0.22f, 8, 0.80f };
		constexpr Shape kOh = { 0.44f, 11, 1.00f };
		constexpr Shape kOoh = { 0.16f, 12, 1.00f };
		constexpr Shape kWuh = { 0.10f, 15, 0.95f };
		constexpr Shape kBmp = { 0.01f, 2, 1.00f };
		constexpr Shape kFv = { 0.10f, 7, 0.95f };
		constexpr Shape kTh = { 0.10f, 14, 0.70f };
		constexpr Shape kCh = { 0.28f, 3, 0.80f };
		constexpr Shape kDst = { 0.24f, 4, 0.70f };
		constexpr Shape kKg = { 0.26f, 9, 0.60f };
		constexpr Shape kNl = { 0.22f, 10, 0.70f };
		constexpr Shape kRr = { 0.18f, 13, 0.80f };

		// Vowels are held roughly twice as long as consonants, which is most of
		// what makes a rhythm sound like language. Scaled by kSpeechRate.
		// Raised ~18% on 2026-08-13. The measured scale factors across a real
		// conversation ran x1.15, x1.49 and x0.97 — centred well above 1.0, meaning
		// the estimate was consistently fast and every line was being stretched to
		// compensate. Scaling still corrects each line exactly; this just stops it
		// starting from a biased guess, which matters on the lines where no .fuz
		// matches and the estimate is all there is.
		// Lowered again 2026-08-13. The 18% raise was calibrated on one early run
		// whose scale factors happened to sit above 1.0; the next run, with the
		// filename fix in and every line matching, ran x0.75-0.98 — the estimate
		// was consistently too SLOW. Conversational English is around twelve to
		// fifteen mouth positions a second, and these are now in that range rather
		// than under ten.
		//
		// With syllables capped at no stretch, these genuinely set the articulation
		// speed on any line that has slack, so they matter again.
		constexpr float kVowelSeconds = 0.070f;
		constexpr float kConsonantSeconds = 0.038f;
		constexpr float kWordGapSeconds = 0.048f;
		constexpr float kPunctuationSeconds = 0.150f;

		// The mouth trails its target by roughly one smoothing time constant, which
		// is a systematic ~55ms of lateness on top of however long it takes to
		// notice the sound handle. Both are constant, so both can simply be led.
		// This is the last of the "a TAD bit off" once the length itself is right.
		constexpr float kLeadSeconds = 0.075f;

		// One dial over the whole sequence. Raise it if the mouth consistently
		// finishes before the audio does; lower it if it runs past.
		constexpr float kSpeechRate = 1.0f;

		struct Phone
		{
			Shape shape;
			float seconds;
		};

		std::vector<Phone> phones;
		std::size_t        phoneCursor{ 0 };
		float              phoneRemaining{ 0.0f };
		bool               fromText{ false };

		// The measured length of this line and how far the estimate had to be
		// stretched to reach it. Both logged, because a scale that sits far from
		// 1.0 every line means the per-phoneme constants are wrong and should be
		// moved rather than corrected for on every line.
		float lineSeconds{ 0.0f };
		float lineScale{ 1.0f };
		float lineGapScale{ 1.0f };

		// How long one shape holds when there is no text to work from. Speech runs
		// roughly eight to twelve mouth positions a second.
		constexpr float kShapeHoldMin = 0.070f;
		constexpr float kShapeHoldMax = 0.135f;

		// Approach time toward the current target. THE single biggest difference
		// between this reading as speech and as a mechanism: real lips have mass and
		// never snap between positions, and the previous version wrote the raw
		// envelope every frame with no interpolation at all.
		// Lowered from 0.055 on 2026-08-13: "it should react a bit faster". Lips do
		// have mass, but 55ms of it visibly softened the consonants — and the
		// closures are exactly what makes a mouth look like it is saying words.
		// ASYMMETRIC, and this is why the Oohs and the Fs were missing.
		//
		// One symmetric constant at 32ms against a 38ms consonant means the shape
		// only ever reaches about two thirds of the way before the next one starts
		// pulling it elsewhere — and the distinctive ones are exactly the short
		// ones. Rounding for "ooh", the lip-bite for "f", the closure for "m": all
		// consonant-length, all smoothed into a general jaw waggle. Speeding the
		// speech up made it worse, because the shapes got shorter.
		//
		// Real lips snap into a position and relax out of it. So the attack is fast
		// enough that a 38ms consonant genuinely arrives, and the release stays slow
		// enough that the mouth does not chatter between them.
		constexpr float kAttackSeconds = 0.014f;
		constexpr float kReleaseSeconds = 0.045f;

		// THE JAW IS NOT A LIP, and giving it the lips' attack is what made it flap.
		//
		// A 14ms attack is right for a lip shape: rounding and closures are small,
		// fast movements and they have to arrive inside a 38ms consonant or they
		// are not seen at all. The jaw is the largest thing on the face and it has
		// real mass — snapping it to a new opening every 40-70ms reads as chewing
		// however correct the sequence underneath is.
		//
		// So the hinge gets its own, much slower approach and the lips keep theirs.
		// The shapes still form; the thing carrying them stops chattering.
		constexpr float kJawAttackSeconds = 0.055f;
		constexpr float kJawReleaseSeconds = 0.075f;

		// Roughly one shape in seven is a rest rather than a phoneme, which is what
		// puts the gaps between words in. The old version made every gap a full
		// closure of the jaw on a hard threshold, which is the chomp.
		constexpr int kRestOneIn = 7;

		// A line that outlives this is a handle that never cleared, not speech.
		//
		// Director::VoicePlaying already carries this scar — "some voices never
		// release their handle" — and guards it with a six-second ceiling plus
		// retiring the handle when the NPC answers. LipSync needs its own, because
		// without one a stuck handle leaves the mouth flapping for the rest of the
		// conversation, which is a far worse artefact than the still mouth it is
		// meant to fix. Measured lines run 0.70s to 4.45s, so eight is clear of any
		// real line while still catching a stuck one quickly.
		constexpr float kMaxLineSeconds = 8.0f;

		bool  enabled{ false };
		float strength{ 0.55f };

		// THE DIAL IS A JAW DIAL. It was being applied to the lips as well, and
		// that is why the mouth read as stiff.
		//
		// LipSync.h has always said this scales "the jaw opening", and the shape
		// table is built on the same premise: the jaw stays low on almost every
		// entry and the LIPS carry the identity of the sound, which is why Ooh and
		// W and F sit at 0.95-1.00 while their jaw values sit at 0.10-0.24.
		//
		// Sample then multiplied all sixteen slots by the dial. At the default 67
		// that turns a full-weight Ooh into 0.67 and an F into 0.64 — the two most
		// recognisable lip shapes in the table, both cut by a third, by a control
		// whose whole purpose is to stop the jaw hanging open. Reported exactly
		// that way: "not enough lip movement, the oos, ffs, LLs".
		//
		// So the jaw keeps the linear dial and the lips get a gentler curve. Still
		// controlled, still silent at zero, but no longer paying for the jaw's
		// restraint: at 67 the lips now land at 0.82 rather than 0.67.
		float lipStrength{ 0.74f };

		// Kept alongside the float rather than derived back from it. Round-tripping
		// 0..100 through a 0..1 float and back is what makes a slider creep by one
		// as it is dragged.
		int   strengthPercent{ 55 };

		bool  engaged{ false };

		// State for the line currently being spoken.
		bool  lineActive{ false };
		std::uint64_t voiceLineSerial{ 0 };
		std::string voiceLineText;
		std::string highlightedTopic;
		bool  driving{ false };
		bool  overran{ false };
		float lineElapsed{ 0.0f };

		// HOW LONG THE MOUTH SHOULD MOVE, when something authoritative said so.
		//
		// This is the fix for the defect that made every line look wrong. LipSync
		// has always known the line's length — it measured 1.49s off the .fuz for
		// "What is it?" — and never used it to STOP. Ending the line was left to
		// the sound handle going idle, and the handle does not go idle: in the
		// 2026-08-18 log every single line, measured or estimated, ran to the
		// eight-second stuck-handle timeout instead. A 1.49s line with the mouth
		// moving for 8s is not a timing inaccuracy, it is the mouth still going
		// five seconds after the voice stopped.
		//
		// DBReV's audioSeconds is the moment the audio ends, so it is the moment
		// the mouth ends. 0 means nothing authoritative was supplied and the old
		// kMaxLineSeconds backstop is all there is.
		float audioWindow{ 0.0f };

		// The sound handle this line is running on, and the one whose line has
		// already been played to its end. Only the handle-polling path uses these:
		// DBReV sends one start per line and cannot retrigger.
		std::uint32_t activeVoiceID{ RE::BSSoundHandle::kInvalidID };
		std::uint32_t consumedVoiceID{ RE::BSSoundHandle::kInvalidID };

		// The mouth, and where it is heading. Plain arrays rather than atomics:
		// both this and FaceGen's hook run on the main thread, Update earlier in the
		// frame than the morph pass, which is the ordering the whole design rests on.
		float current[kSlotCount]{};
		float target[kSlotCount]{};
		float shapeRemaining{ 0.0f };

		std::atomic_bool writing{ false };


		// Deterministic, but seeded per line from the voice handle so two lines do
		// not get the same mouth. A fixed sequence would be visible within a
		// conversation.
		std::uint32_t rng{ 1 };

		// Counts lines within the session, purely to keep the seed moving on the
		// event-driven path where there is no handle id to borrow. Wrapping is
		// fine and expected; it is a seed, not an identity.
		std::uint32_t lineOrdinal{ 0 };

		[[nodiscard]] std::uint32_t NextRandom()
		{
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			return rng;
		}

		[[nodiscard]] float RandomUnit()
		{
			return static_cast<float>(NextRandom() % 10000u) / 10000.0f;
		}

		// Observed, reported, and deliberately NOT acted on. See the note in Update.
		float windowMin{ 0.0f };
		float windowMax{ 0.0f };
		bool  windowSampled{ false };

		Log::OnceFlag verdictReported;

		[[nodiscard]] RE::HighProcessData* HighOf(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process ? process->high : nullptr;
		}

		// Is a voice line playing on this actor right now?
		//
		// soundHandles ONLY. An earlier version fell back to `voiceTimer > 0` and
		// that fallback broke the entire feature: measured, voiceTimer on the player
		// reads a constant 295.77 for a whole session — 461.17 in the 2026-08-12
		// logs — and is not a countdown of the current line. So the fallback was
		// permanently true, the line never ended, and the whole conversation was
		// decided by one evaluation taken before the player had said anything.
		//
		// The handle is the real signal and every log since confirms it: the
		// player's snd0 goes 4294967295/0 (kInvalidID, idle) -> 200/1 (kPlaying)
		// exactly when a line runs.
		//
		// Also deliberately NOT voiceState/voiceTimeElapsed. Those sit at 0x000 and
		// 0x014 beside currentShout and voiceRecoveryTime and belong to the shout
		// system; the face probe prints them as though they were dialogue fields,
		// which is why they read a flat 0 through every line ever logged here.
		// Returns the playing handle's id, or kInvalidID. The id doubles as the seed
		// for this line's shape sequence.
		[[nodiscard]] std::uint32_t PlayingVoiceID(RE::HighProcessData* a_high)
		{
			if (!a_high) {
				return RE::BSSoundHandle::kInvalidID;
			}

			for (const auto& handle : a_high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					return handle.soundID;
				}
			}

			return RE::BSSoundHandle::kInvalidID;
		}

		[[nodiscard]] RE::BSFaceGenAnimationData* FaceOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetFaceGenAnimationData() : nullptr;
		}

		// Peak of the phoneme channel, or a negative value if it could not be read.
		//
		// The distinction matters and has burned this investigation once already:
		// the old probe collapsed "flat", "null", "zero slots" and "absurd count"
		// into a single 0.0f, and a whole conclusion was built on the result. A
		// channel that cannot be read must never be scored as a channel at rest.
		[[nodiscard]] float PeakPhoneme(RE::BSFaceGenAnimationData* a_data)
		{
			if (!a_data) {
				return -1.0f;
			}

			const auto& channel = a_data->phenomeKeyFrame;
			if (!channel.values || channel.count == 0 || channel.count > 256) {
				return -1.0f;
			}

			float peak = 0.0f;
			for (std::uint32_t i = 0; i < channel.count; ++i) {
				peak = std::max(peak, std::fabs(channel.values[i]));
			}
			return peak;
		}

		void Push(const Shape& a_shape, float a_seconds)
		{
			phones.push_back({ a_shape, a_seconds * kSpeechRate });
		}

		// --- The line's real length, from the file that is being played ----------
		//
		// Estimated timing was the remaining complaint, and it cannot be tuned away:
		// a per-phoneme guess drifts against a recording of a human being, and the
		// longer the line the further it drifts. The recording knows how long it is.
		//
		// The layout was verified by hand on a real pack file:
		//
		//   00  "FUZE"
		//   04  uint32 version (1)
		//   08  uint32 lip data size
		//   12  <lip data>            - FaceFX, deliberately not parsed
		//   12+size  RIFF/XWMA        - the audio, whose dpds chunk has the length
		//
		// Duration comes from the RIFF, not from the FaceFX blob, precisely so that
		// nothing here depends on decoding an undocumented format.

		// Spaces and characters Windows will not put in a filename become
		// underscores. EVERYTHING ELSE IS KEPT.
		//
		// Read off the shipped packs rather than reasoned about, after the first
		// build mapped all punctuation to underscore and missed every line
		// containing an apostrophe:
		//
		//   Why_don't_you_charge_a_flat_fee_like_other_mercs_.fuz   ' kept, ? -> _
		//   We_shall_meet_again_in_battle,_then..fuz                , and . kept
		//   There's_no_time_for_this!.fuz                           ! kept
		//
		// So the rule is not "punctuation is unsafe", it is "this has to survive
		// NTFS" — which also explains the doubled dot on a line ending in one.
		// A path from a UTF-8 string, meaning it, rather than from bytes the ANSI
		// codepage will guess at.
		//
		// Everything narrow that reaches this file comes from the GAME — topic text
		// off Scaleform, a voice pack folder read out of the SpeakSound call — and
		// all of it is UTF-8. Constructing std::filesystem::path from a plain
		// std::string does NOT treat it that way: on Windows it widens through the
		// system ANSI codepage, so a Japanese topic or a Japanese voice pack folder
		// is reinterpreted as Shift-JIS and addresses a file that does not exist.
		//
		// It never crashed — narrow-to-wide substitutes rather than failing, which
		// is why this hid behind the throwing conversion in Logging.cpp rather than
		// being found with it. It just quietly missed every lookup, so every line a
		// non-English player spoke fell back to the text-length estimate instead of
		// being measured off the recording. Reported as part of the Unicode crash
		// and worth fixing with it.
		//
		// char8_t is what makes the constructor mean UTF-8; the cast is the
		// standard-sanctioned way to say so, since the bytes are already correct.
		[[nodiscard]] std::filesystem::path Utf8Path(std::string_view a_text)
		{
			return std::filesystem::path{
				std::u8string_view{ reinterpret_cast<const char8_t*>(a_text.data()), a_text.size() }
			};
		}

		[[nodiscard]] std::string Sanitize(std::string_view a_text)
		{
			constexpr std::string_view kIllegal = "<>:\"/\\|?*";

			std::string out;
			out.reserve(a_text.size());
			for (const char c : a_text) {
				const bool unsafe = c == ' ' || static_cast<unsigned char>(c) < 0x20 ||
					kIllegal.find(c) != std::string_view::npos;
				out.push_back(unsafe ? '_' : c);
			}
			return out;
		}

		// The older rule, kept as a second guess only.
		//
		// If a pack was built by a different tool, or a future DBVO changes its
		// mind, one extra exists() per pack is a much cheaper failure than falling
		// back to estimated timing on every line.
		[[nodiscard]] std::string SanitizeAlnum(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size());
			for (const char c : a_text) {
				const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9');
				out.push_back(alnum ? c : '_');
			}
			return out;
		}

		// Seconds of audio in a .fuz, or 0 if it cannot be read.
		[[nodiscard]] float FuzSeconds(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return 0.0f;
			}

			char header[12]{};
			if (!file.read(header, sizeof(header)) ||
				std::memcmp(header, "FUZE", 4) != 0) {
				return 0.0f;
			}

			std::uint32_t lipSize = 0;
			std::memcpy(&lipSize, header + 8, sizeof(lipSize));

			const auto total = std::filesystem::file_size(a_path);
			const auto audioAt = static_cast<std::uintmax_t>(12) + lipSize;
			if (audioAt + 44 >= total) {
				return 0.0f;
			}

			file.seekg(static_cast<std::streamoff>(audioAt), std::ios::beg);

			// RIFF header, then walk chunks for 'fmt ' and 'data'. Walking rather
			// than assuming fixed offsets: XWMA carries a 'dpds' chunk between them
			// that a fixed layout would read straight through.
			char riff[12]{};
			if (!file.read(riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0) {
				return 0.0f;
			}

			std::uint16_t channels = 0;
			std::uint32_t sampleRate = 0;
			std::uint32_t bytesPerSecond = 0;
			std::uint16_t bitsPerSample = 0;
			std::uint32_t dataBytes = 0;
			std::uint32_t decodedBytes = 0;  // last dpds entry; 0 when there is no dpds

			for (int guard = 0; guard < 32; ++guard) {
				char id[4]{};
				std::uint32_t size = 0;
				if (!file.read(id, 4) || !file.read(reinterpret_cast<char*>(&size), 4)) {
					break;
				}

				if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
					char fmt[16]{};
					if (!file.read(fmt, sizeof(fmt))) {
						break;
					}
					std::memcpy(&channels, fmt + 2, sizeof(channels));
					std::memcpy(&sampleRate, fmt + 4, sizeof(sampleRate));
					std::memcpy(&bytesPerSecond, fmt + 8, sizeof(bytesPerSecond));
					std::memcpy(&bitsPerSample, fmt + 14, sizeof(bitsPerSample));
					file.seekg(size - 16, std::ios::cur);
				} else if (std::memcmp(id, "dpds", 4) == 0 && size >= 4 && (size % 4) == 0) {
					// "Decoded packet cumulative data size": one running total per
					// encoded packet, so the LAST entry is the size of the whole
					// stream once decoded to PCM. That is the number that gives the
					// real duration. See the note above the return.
					file.seekg(size - 4, std::ios::cur);
					if (!file.read(reinterpret_cast<char*>(&decodedBytes), 4)) {
						break;
					}
					if (size & 1) {
						file.seekg(1, std::ios::cur);
					}
				} else if (std::memcmp(id, "data", 4) == 0) {
					dataBytes = size;
					break;
				} else {
					file.seekg(size + (size & 1), std::ios::cur);  // chunks are word-aligned
				}
			}

			// XWMA IS COMPRESSED, AND nAvgBytesPerSec DOES NOT DESCRIBE IT.
			//
			// This was wrong for the whole life of the file and measurably so. The
			// old formula was dataBytes / nAvgBytesPerSec, which is the size of the
			// COMPRESSED payload over a NOMINAL declared byte rate, and in these
			// packs the two do not correspond. Verified by hand against DBReV, which
			// reports the same files to the millisecond:
			//
			//   You_father_was_a_traditionalist_      old 2.973s   dpds 1.950s
			//   I_imagine_he_didn't_react_well.       old 2.230s   dpds 1.950s
			//   Given_your_family_history,_...        old 4.832s   dpds 3.947s
			//
			// Every line was being stretched to somewhere between 15% and 52% longer
			// than the recording. That is where "the timing is off" came from, and
			// it is also why the logged pause scale sat pinned at its 2.2 clamp so
			// often — the sequence was being asked to fill time the audio never had.
			//
			// dpds is the authority: its last entry is the decoded PCM size, and
			// decoded PCM has a fixed rate, so the division is exact.
			const std::uint32_t frameBytes =
				static_cast<std::uint32_t>(channels) * (bitsPerSample / 8u);

			if (decodedBytes > 0 && sampleRate > 0 && frameBytes > 0) {
				return static_cast<float>(decodedBytes) /
				       static_cast<float>(sampleRate * frameBytes);
			}

			// No dpds means the payload is not XWMA — plain PCM, where the byte rate
			// is real and the original formula is exactly right. Kept rather than
			// replaced: this is the correct answer for that case, not a guess.
			if (bytesPerSecond == 0 || dataBytes == 0) {
				return 0.0f;
			}

			return static_cast<float>(dataBytes) / static_cast<float>(bytesPerSecond);
		}

		// Every voice pack directory, enumerated once.
		//
		// Cheaper and sturdier than reading whichever JSON the installed voice mod
		// keeps its selected pack in: DBVO 1, DBVO 2 and DBReV each store that
		// differently, and all three lay the audio out identically underneath.
		[[nodiscard]] const std::vector<std::filesystem::path>& VoicePacks()
		{
			static std::vector<std::filesystem::path> packs = [] {
				std::vector<std::filesystem::path> found;
				std::error_code ec;
				const std::filesystem::path root{ "Data/Sound/DBVO" };
				for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
					if (!ec && entry.is_directory(ec)) {
						found.push_back(entry.path());
					}
				}
				Log::Info(Log::Category::kStaging,
					"LipSync: {} voice pack folder(s) under Data/Sound/DBVO."sv, found.size());
				return found;
			}();
			return packs;
		}

		// The spoken length of this topic, or 0 if the file cannot be found.
		[[nodiscard]] float MeasureTopic(std::string_view a_topic)
		{
			if (a_topic.empty()) {
				return 0.0f;
			}

			const std::string names[]{ Sanitize(a_topic) + ".fuz",
				SanitizeAlnum(a_topic) + ".fuz" };
			std::error_code ec;

			// THE PACK ACTUALLY BEING PLAYED COMES FIRST, and on a multi-pack load
			// order it is the difference between a measurement and a coincidence.
			//
			// The loop below takes the first pack on disk that happens to hold a
			// file of the right name. With fifteen packs installed that is very
			// often somebody else's recording of the same line: measured 2026-08-18,
			// a session playing voicebella timed a line at 5.203s off FearTcbVamp's
			// copy when voicebella's own is 3.947s. Different actor, different pace,
			// and no amount of scaling recovers it.
			//
			// Compat::VoiceCommand reads the pack out of the SpeakSound call that
			// plays the line, so this is the real answer rather than a better guess.
			// It is empty until the first line of the session has been spoken, and
			// the general search below is what runs until then.
			if (const std::string active = Compat::VoiceCommand::Pack(); !active.empty()) {
				const std::filesystem::path root =
					std::filesystem::path{ "Data/Sound/DBVO" } / Utf8Path(active);

				for (const auto& name : names) {
					const auto candidate = root / Utf8Path(name);
					if (std::filesystem::exists(candidate, ec)) {
						const float seconds = FuzSeconds(candidate);
						if (seconds > 0.05f) {
							return seconds;
						}
					}
				}

				// Fall through deliberately. A pack that does not carry this line is
				// the ordinary case — no pack covers every topic — and a wrong-actor
				// length still beats no length at all, which drops the line to a
				// pure text estimate.
			}

			for (const auto& name : names) {
				for (const auto& pack : VoicePacks()) {
					const auto candidate = pack / Utf8Path(name);
					if (std::filesystem::exists(candidate, ec)) {
						const float seconds = FuzSeconds(candidate);
						if (seconds > 0.05f) {
							return seconds;
						}
					}
				}
			}

			return 0.0f;
		}

		// The spoken length of the EXACT file, with nothing guessed at any stage.
		//
		// MeasureTopic above is a filename heuristic looking for a plausible match;
		// this is the string the voice mod handed to the engine, resolved the same
		// way the engine resolves it — relative to Data/Sound. It cannot land on
		// another actor's recording of the same words, and it cannot miss a line
		// whose punctuation the sanitizers happen to mangle differently.
		[[nodiscard]] float MeasureVoiceFile(std::string_view a_path)
		{
			if (a_path.empty()) {
				return 0.0f;
			}

			const auto file = std::filesystem::path{ "Data/Sound" } / Utf8Path(a_path);

			std::error_code ec;
			if (!std::filesystem::exists(file, ec)) {
				// A pack shipped inside a BSA, most likely, since a path the engine
				// played must resolve somewhere. Nothing to measure, so the caller
				// falls back to its text estimate — the same place an unmatched
				// filename left it, except that the line itself is still known to be
				// real and its shapes are still the right words.
				return 0.0f;
			}

			const float seconds = FuzSeconds(file);
			return seconds > 0.05f ? seconds : 0.0f;
		}

		// The words, recovered from the filename the voice mod chose for them.
		//
		// A last resort with exactly the standing of DBReV's topicKey, and the same
		// losses: the name was built by replacing every space and everything NTFS
		// rejects with an underscore, so the words come back and the punctuation
		// does not. Worth having anyway — no text at all drops the line to a
		// synthesized cadence with no relation to what was said.
		[[nodiscard]] std::string KeyFromPath(std::string_view a_path)
		{
			const auto slash = a_path.find_last_of("/\\");
			auto       name = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);

			if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) {
				name = name.substr(0, dot);
			}

			std::string out{ name };
			for (char& c : out) {
				if (c == '_') {
					c = ' ';
				}
			}

			return out;
		}

		// Per-line variation on the command path, where there is no handle id and no
		// topic index to seed from. The path is the one thing that is different for
		// every line and the same for a line repeated, so it is mixed with the line
		// ordinal rather than used alone.
		[[nodiscard]] std::uint32_t PathHash(std::string_view a_text) noexcept
		{
			std::uint32_t hash = 2166136261u;
			for (const char c : a_text) {
				hash ^= static_cast<std::uint8_t>(c);
				hash *= 16777619u;
			}
			return hash;
		}

		// Turn the line into a sequence of mouth positions.
		//
		// Digraphs are tested before single letters, because "sh", "th" and "ch"
		// are each one mouth position and spelling them out as two would put a
		// tongue-tip shape where the lips should be doing the work. Silent trailing
		// "e" is dropped for the same reason: "mine" ending on an Eee is the single
		// most obvious wrong shape this could produce.
		// a_measured is the line's real length in seconds when the caller already
		// knows it — DBReV hands that over with the line — or 0 to go and find the
		// .fuz ourselves. See the note at MeasureTopic: the search is a filename
		// guess and it missed on 14 of 30 logged lines.
		void BuildFromText(std::string_view a_text, float a_measured)
		{
			phones.clear();
			phoneCursor = 0;
			phoneRemaining = 0.0f;

			const auto lower = [](char c) {
				return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
			};
			const auto isAlpha = [](char c) {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			};

			for (std::size_t i = 0; i < a_text.size();) {
				const char c = lower(a_text[i]);

				if (!isAlpha(c)) {
					// Sentence punctuation is a real pause; a space is a short one.
					if (c == '.' || c == ',' || c == '?' || c == '!' || c == ';' || c == ':') {
						Push(kRest, kPunctuationSeconds);
					} else if (c == ' ') {
						Push(kSoftRest, kWordGapSeconds);
					}
					++i;
					continue;
				}

				const char n = (i + 1 < a_text.size()) ? lower(a_text[i + 1]) : '\0';

				// Two-letter clusters that are one mouth position.
				if (n != '\0') {
					const auto pair = [&](char x, char y) { return c == x && n == y; };

					if (pair('t', 'h')) { Push(kTh, kConsonantSeconds); i += 2; continue; }
					if (pair('s', 'h') || pair('c', 'h')) { Push(kCh, kConsonantSeconds); i += 2; continue; }
					if (pair('p', 'h')) { Push(kFv, kConsonantSeconds); i += 2; continue; }
					if (pair('w', 'h')) { Push(kWuh, kConsonantSeconds); i += 2; continue; }
					if (pair('c', 'k')) { Push(kKg, kConsonantSeconds); i += 2; continue; }
					if (pair('n', 'g') || pair('k', 'n')) { Push(kNl, kConsonantSeconds); i += 2; continue; }
					if (pair('q', 'u')) { Push(kKg, kConsonantSeconds); Push(kWuh, kConsonantSeconds); i += 2; continue; }
					if (pair('o', 'o') || pair('o', 'u') || pair('e', 'w')) { Push(kOoh, kVowelSeconds); i += 2; continue; }
					if (pair('e', 'e') || pair('e', 'a')) { Push(kEee, kVowelSeconds); i += 2; continue; }
					if (pair('o', 'w')) { Push(kOh, kVowelSeconds); i += 2; continue; }
					if (pair('a', 'i') || pair('a', 'y')) { Push(kAah, kVowelSeconds); i += 2; continue; }
					if (pair('o', 'a')) { Push(kOh, kVowelSeconds); i += 2; continue; }
				}

				// A trailing silent "e" — "mine", "have", "there".
				const bool wordEnd = (i + 1 >= a_text.size()) || !isAlpha(a_text[i + 1]);
				if (c == 'e' && wordEnd && i > 0 && isAlpha(a_text[i - 1])) {
					++i;
					continue;
				}

				switch (c) {
				case 'a': Push(kAah, kVowelSeconds); break;
				case 'e': Push(kEh, kVowelSeconds); break;
				case 'i': Push(kIh, kVowelSeconds); break;
				case 'o': Push(kOh, kVowelSeconds); break;
				case 'u': Push(kOoh, kVowelSeconds); break;
				case 'y': Push(kEee, kVowelSeconds); break;

				case 'm': case 'b': case 'p': Push(kBmp, kConsonantSeconds); break;
				case 'f': case 'v':           Push(kFv, kConsonantSeconds); break;
				case 'w':                     Push(kWuh, kConsonantSeconds); break;
				case 'r':                     Push(kRr, kConsonantSeconds); break;
				case 'n': case 'l':           Push(kNl, kConsonantSeconds); break;
				case 'k': case 'g': case 'q': case 'x': Push(kKg, kConsonantSeconds); break;
				case 'j':                     Push(kCh, kConsonantSeconds); break;
				case 'd': case 't': case 's': case 'z': case 'c':
					Push(kDst, kConsonantSeconds); break;

				// 'h' alone is breath: no mouth position of its own.
				case 'h': break;
				default: break;
				}
				++i;
			}

			// Stretch the sequence onto the recording.
			//
			// The shapes come from the text and the LENGTH comes from the file, so
			// the mouth finishes exactly when the voice does. Without this the
			// per-phoneme estimate drifts against a human being reading at their own
			// pace, and it drifts further the longer the line — which is the
			// "timing is off" that no constant could fix.
			//
			// Clamped because a runaway scale is worse than an estimate: a topic
			// whose text and audio genuinely disagree (a truncated subtitle, the
			// wrong file matched) should degrade to a slightly-off mouth, not to one
			// holding a single shape for four seconds.
			// DBReV's number is authoritative: it is measured off the same FUZ
			// header by the mod that is about to play the file, so there is no
			// filename to guess and no pack to search. Only fall back to finding
			// the file ourselves when nothing supplied it.
			const float measured = a_measured > 0.0f ? a_measured : MeasureTopic(a_text);
			if (measured <= 0.0f) {
				lineSeconds = 0.0f;
				return;
			}

			// NO PHONEMES IS NOT NO MEASUREMENT, and these used to be the same
			// early return.
			//
			// `phones` comes out empty whenever the words cannot be read as letters,
			// and the whole of a non-Latin localisation is that case: the builder
			// above walks a-z, so a Japanese, Russian or Chinese subtitle produces
			// nothing at all. Throwing lineSeconds away with the phonemes meant a
			// measurement that had ALREADY BEEN TAKEN — off the real .fuz, by the
			// search directly above — was discarded because the text beside it was
			// in the wrong alphabet.
			//
			// What that cost: audioWindow at the caller falls back to DBReV's
			// audioSeconds, and then to the whole conversation's length. On a DBVO
			// profile without DBReV there is no audioSeconds, so every line a
			// non-English player spoke ran the mouth on the CONVERSATION window —
			// the five-times overrun this file spent a week removing, still fully
			// present for anyone not playing in English.
			//
			// The shapes and the length are two separate answers to two separate
			// questions. Losing the shapes costs the words; it does not cost the
			// clock. The mouth falls back to the synthesized cadence, which is what
			// it should do without text, and it now stops when the voice does.
			if (phones.empty()) {
				lineSeconds = measured;

				// Neither scale means anything with nothing to scale, and stale
				// values from the previous line would be worse than none.
				lineScale = 1.0f;
				lineGapScale = 1.0f;
				return;
			}

			// SLACK GOES INTO THE PAUSES, NOT INTO THE SYLLABLES.
			//
			// Uniform scaling was the remaining inaccuracy, and the logged factors
			// say why: long lines came out at x0.73-0.90 and short ones at
			// x1.48-1.81. A recording carries leading and trailing silence plus
			// whatever pauses the actor took, and that overhead is roughly FIXED —
			// so on a short line it dominates, and stretching every vowel to absorb
			// it produces a mouth moving in slow motion through "Alright."
			//
			// People do not slow their articulation to fill time; they pause. So
			// when the file is longer than the words, the syllables stretch a
			// little and the gaps take the rest. When it is shorter, everything
			// compresses together — speech genuinely does speed up.
			float speech = 0.0f;
			float gaps = 0.0f;
			for (const auto& phone : phones) {
				(phone.shape.weight > 0.0f ? speech : gaps) += phone.seconds;
			}

			const float estimated = speech + gaps;
			if (estimated <= 0.01f) {
				// Same split as above, plus one more step. A sequence that sums to
				// nothing cannot be scaled onto the recording, and scheduling it
				// unscaled would flash every position through in a single frame and
				// then hold a still mouth for the rest of the line — worse than
				// having no shapes at all. So the shapes go, which drops the caller
				// to the synthesized cadence, and the measurement stays.
				phones.clear();
				lineSeconds = measured;
				lineScale = 1.0f;
				lineGapScale = 1.0f;
				return;
			}

			const float ratio = measured / estimated;
			float       speechScale = std::clamp(ratio, 0.4f, 2.5f);
			float       gapScale = speechScale;

			// Only when there is slack to place, and only when there are gaps to
			// place it in. A line of one word with no punctuation has neither.
			if (ratio > 1.0f && gaps > 0.02f) {
				// SYLLABLES NEVER STRETCH. Was 1.12, and that cap is why it still
				// read as slow.
				//
				// Because every line is scaled to fill its measured length, the base
				// durations do not set the speed — measured/count does. Making the
				// constants faster just changes the scale factor and lands in the
				// same place. The ONLY way to articulate faster is to stop filling
				// the whole file with syllables, and put the difference where a
				// recording actually keeps it: in the silence at either end and the
				// pauses between phrases.
				constexpr float kMaxSyllableStretch = 1.0f;
				speechScale = std::min(ratio, kMaxSyllableStretch);

				// AND THE PAUSES DO NOT ABSORB EVERYTHING EITHER.
				//
				// Letting them take all the slack produced x2.91 and x3.66 in the
				// log, which is a third of a second of dead air between ordinary
				// words — the mouth stops dead mid-sentence and starts again.
				//
				// A recording's spare time is not mostly between words, it is the
				// silence at the ends: the actor breathes in before the first
				// syllable and the file runs on after the last. So the gaps take a
				// modest share and WHATEVER IS LEFT IS SIMPLY NOT SCHEDULED — the
				// sequence finishes early and the mouth rests shut until the handle
				// clears, which is exactly what the tail of a recording looks like.
				constexpr float kMaxGapStretch = 2.2f;
				gapScale = std::clamp((measured - speech * speechScale) / gaps, 1.0f, kMaxGapStretch);
			}

			for (auto& phone : phones) {
				phone.seconds *= (phone.shape.weight > 0.0f) ? speechScale : gapScale;
			}

			lineSeconds = measured;
			lineScale = speechScale;
			lineGapScale = gapScale;
		}

		// Choose the next mouth position.
		//
		// Stress varies per shape rather than per line. Uniform amplitude is the
		// other half of why a synthesized mouth reads as machinery: real speech has
		// loud syllables and swallowed ones, and the difference between them is much
		// larger than the difference between two adjacent phonemes.
		void ChooseShape()
		{
			shapeRemaining = kShapeHoldMin + RandomUnit() * (kShapeHoldMax - kShapeHoldMin);

			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				target[i] = 0.0f;
			}

			if (static_cast<int>(NextRandom() % static_cast<std::uint32_t>(kRestOneIn)) == 0) {
				return;  // a rest: everything eases toward closed
			}

			const auto& shape = kShapes[NextRandom() % (sizeof(kShapes) / sizeof(kShapes[0]))];
			const float stress = 0.55f + RandomUnit() * 0.45f;

			target[kSlotJaw] = shape.jaw * stress;
			if (shape.slot < kSlotCount) {
				target[shape.slot] = shape.weight * stress;
			}
		}

		void EndLine()
		{
			// Closing the mouth is a WRITE, and writes belong to FaceGen. Easing the
			// target to zero and letting the approach run keeps every write in this
			// feature going through the one point in the frame nothing else follows,
			// and closes the mouth over about 50ms rather than snapping it shut.
			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				target[i] = 0.0f;
			}

			lineActive = false;
			overran = false;
			lineElapsed = 0.0f;
			audioWindow = 0.0f;
			shapeRemaining = 0.0f;
			windowSampled = false;
			windowMin = 0.0f;
			windowMax = 0.0f;
			// `driving` deliberately stays set until the mouth has actually eased
			// shut; see Update. Clearing it here would abandon the face mid-shape.

		}
	}

	void LipSync::Configure(bool a_enabled, int a_strength)
	{
		const bool wasEnabled = enabled;

		enabled = a_enabled;
		strengthPercent = std::clamp(a_strength, 0, 100);
		strength = static_cast<float>(strengthPercent) / 100.0f;

		// sqrt, so the curve is above the line everywhere between the ends and
		// meets it at both: 0 is still a closed mouth and 100 is still the table's
		// own weights, unchanged. Nothing is being amplified past what the shapes
		// were authored at — the lips just stop being scaled down with the hinge.
		lipStrength = std::sqrt(strength);

		if (!enabled) {
			writing.store(false, std::memory_order_relaxed);
		}

		// Switched on after startup — from the menu, or from an ini edited between
		// conversations. Without this the toggle writes the file, reports itself
		// enabled, and does nothing at all until the game is restarted, because the
		// hook it writes through is only installed at load. Install is idempotent
		// and returns immediately if it already ran.
		if (enabled && !wasEnabled && !FaceGen::Installed()) {
			FaceGen::Install();
			Log::Info(Log::Category::kStaging,
				"LipSync: enabled after startup; installing the morph hook now."sv);
		}

		// Every call: a OnceFlag here means
		// a menu toggle mid-session leaves the log still asserting the state it had
		// at load, which is worse than not logging it.
		Log::Info(Log::Category::kStaging,
			"LipSync: synthesis {}, strength {:.2f} (lips {:.2f})."sv,
			enabled ? "ENABLED"sv : "disabled"sv, strength, lipStrength);
	}

	bool LipSync::Enabled() noexcept
	{
		return enabled;
	}

	int LipSync::StrengthPercent() noexcept
	{
		return strengthPercent;
	}

	void LipSync::Engage()
	{
		engaged = true;
		highlightedTopic.clear();
		verdictReported.Reset();

		// A fresh conversation retires nothing. Handle ids are per-sound and a new
		// line gets a new one, but carrying a retirement across conversations would
		// mean one unlucky id collision silently costs a line its mouth.
		activeVoiceID = RE::BSSoundHandle::kInvalidID;
		consumedVoiceID = RE::BSSoundHandle::kInvalidID;
	}

	void LipSync::Release()
	{
		// EndLine only eases the target to zero; the approach in Update carries the
		// face the rest of the way and clears `driving` when it gets there. Update
		// keeps running after a conversation closes — it is ahead of the staging
		// gate — so that tail completes. Snapping every slot to zero here instead
		// would put a visible jump on the last frame of every conversation.
		EndLine();
		highlightedTopic.clear();
		engaged = false;
	}

	void LipSync::Update(float a_delta)
	{
		// Camera handoff still needs voice timing when all face effects are off.
		if (!engaged && !enabled && !Performance::ExpressionsEnabled() && !lineActive && !driving) return;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		// --- WHERE THE LINE'S EDGES COME FROM ---------------------------------
		//
		// Three sources, in order of how much they actually know.
		//
		// DBReV broadcasts a start the instant it asks the engine to speak, and an
		// end exactly once when the dialogue advances. Both are statements of fact
		// from the mod that is playing the file, and it is the only source that
		// knows the post-line delay. Where it exists it is used for everything and
		// nothing else is consulted.
		//
		// Failing that, the SpeakSound call itself. Every DBVO generation plays the
		// player's line with `Player.SpeakSound "DBVO/<pack>/<line>.fuz"`, and
		// Compat::VoiceCommand is already sitting on that command to learn the pack.
		// The rest of the same string is the exact file and the moment it started —
		// a start event and a real measurement on a profile that has neither. There
		// is no end event on this path, so the mouth still closes on the measured
		// length, which is now measured off the right recording every time.
		//
		// Failing both — no voice mod, or one that routes around the command — the
		// handle polling below is the original code path, backstops and all.
		//
		// THE SECOND DEFECT THE TOP TWO CLOSE. Handle polling cannot tell the
		// player's own dialogue line from any other sound playing on the player, so
		// on the 14 logged lines with no recording in the pack — modded-follower
		// dialogue the vanilla-line pack has never had — the mouth moved anyway,
		// through silence. A source that announces lines and announces nothing is
		// stating that the player did not speak, and the mouth now stays shut.
		//
		// EverSpoke and EverHeard rather than Present and Install: both mods can be
		// loaded while something else voices the player, and waiting for events that
		// will never arrive would leave the mouth permanently still. Each commits
		// only from the first line it has actually seen.
		if (!lineActive && Interface::ReadDialoguePhase().phase == Interface::MenuPhase::kTopicList) {
			highlightedTopic = Interface::ReadSelectedTopic();
		}
		const bool eventDriven = Compat::DBReV::Present() && Compat::DBReV::EverSpoke();
		const bool commandDriven = !eventDriven && Compat::VoiceCommand::EverHeard();

		bool          startNow = false;
		std::uint32_t seed = 0;
		float         supplied = 0.0f;
		float         suppliedTotal = 0.0f;
		std::string   suppliedKey;

		// Which of them supplied the length, for the log. Meaningless while
		// `supplied` is zero, and never read then.
		std::string_view suppliedFrom{};

		if (eventDriven) {
			// End first, then start. A superseded line's end and its replacement's
			// start can land in the same frame, in either dispatch order, and
			// closing before opening gives the same correct result for both.
			std::uint32_t reason = 0;
			if (Compat::DBReV::TakeLineEnd(reason) && lineActive) {
				// Normally a no-op: the mouth has already closed itself at
				// audioSeconds and this arrives after the post-line delay. It earns
				// its place on a SKIP, which arrives early and is the one case the
				// audio window cannot see coming.
				Log::Info(Log::Category::kStaging,
					"LipSync: line ended ({}) at {:.2f}s."sv,
					Compat::DBReV::EndReasonName(reason), lineElapsed);
				EndLine();
			}

			Compat::DBReV::Line line;
			if (Compat::DBReV::TakeLineStart(line)) {
				if (lineActive) {
					EndLine();  // superseded without an end having arrived yet
				}
				startNow = true;
				supplied = line.audioSeconds;
				suppliedFrom = "from DBReV"sv;
				suppliedTotal = line.totalSeconds;
				suppliedKey = line.topicKey;

				// NOT the topic index on its own, which is where this first landed
				// and it was wrong: the index is a position in the dialogue list,
				// so it is usually 0-5 and the top option is 0 every single time.
				// Seeding from it hands the same mouth sequence to every line
				// clicked in the same slot, which is precisely the "fixed sequence
				// visible within a conversation" the handle id was chosen to avoid.
				//
				// An ordinal that counts lines restores the variation, and mixing
				// the index in keeps two different topics apart within one turn.
				seed = (++lineOrdinal * 2654435761u) ^ (line.topicIndex + 1u);
			}
		} else if (commandDriven) {
			// NO END EVENT ON THIS PATH, and nothing here pretends otherwise.
			//
			// SpeakSound says only that a line has started. The mouth closes on the
			// audio window as it always has — but that window is now the length of
			// the file the command named, rather than of whichever same-named file
			// turned up first across fifteen pack directories.
			//
			// So there is no TakeLineEnd counterpart above: a skip is not announced,
			// and a line cut short by the player clicking on still runs its measured
			// length. That is the one thing DBReV does that this cannot, and it is a
			// far smaller artefact than the five-times overrun it replaces.
			//
			// ONE KNOWN SEAM, on a DBReV profile only and at most once per session.
			// DBReV issues the same SpeakSound it broadcasts, so if the command runs
			// before its message is dispatched, the FIRST line of the session opens
			// here and is then superseded by the event a frame or two later — the
			// mouth restarts, under 100ms in. From the second line EverSpoke is
			// latched, eventDriven wins outright and this branch is never entered
			// again. Not worth machinery to remove; worth knowing when reading a log
			// where line one looks like it started twice.
			Compat::VoiceCommand::Line line;
			if (Compat::VoiceCommand::TakeLineStart(line)) {
				if (lineActive) {
					EndLine();  // a topic clicked before the last line finished
				}

				startNow = true;
				supplied = MeasureVoiceFile(line.path);
				suppliedFrom = "from the file SpeakSound named"sv;

				// No totalSeconds to have. The post-line delay is a setting inside
				// the voice mod and nothing outside it can read one — which is only
				// a loss for Director, and Director is not on this path.
				suppliedKey = KeyFromPath(line.path);

				seed = (++lineOrdinal * 2654435761u) ^ PathHash(line.path);
			}
		} else {
			const std::uint32_t voiceID = PlayingVoiceID(HighOf(player));
			const bool          playing = voiceID != RE::BSSoundHandle::kInvalidID;

			if (!playing) {
				if (lineActive) {
					EndLine();
				}

				// The handle went idle, so whatever comes next is genuinely a new
				// line rather than the same one still hanging around.
				consumedVoiceID = RE::BSSoundHandle::kInvalidID;
			}

			// A HANDLE WHOSE LINE HAS ALREADY BEEN PLAYED OUT DOES NOT START ANOTHER.
			//
			// This is the retrigger that made every line speak twice. The audio
			// window closes the line at its measured end — but the sound handle
			// does not go idle then, and on this engine often never does. So the
			// next frame saw `playing && !lineActive`, called that a new line, and
			// ran the whole phoneme sequence again. Measured 2026-08-18: every line
			// logged twice, the second exactly one window after the first —
			// 1.95s -> +1.97s, 3.95s -> +3.98s. Reported as the mouth carrying on
			// after the voice stopped, which is exactly what it was.
			//
			// Director::VoicePlaying carries the same scar and solves it the same
			// way, with `retiredVoiceID`.
			startNow = playing && !lineActive && voiceID != consumedVoiceID;
			seed = voiceID;

			if (startNow) {
				activeVoiceID = voiceID;
			}
		}

		if (startNow) {
			lineActive = true;
			driving = true;  // from the first frame; see below
			overran = false;
			lineElapsed = 0.0f;
			windowSampled = false;
			windowMin = 0.0f;
			windowMax = 0.0f;

			// Zero and one are both degenerate for xorshift, hence the salt.
			rng = seed ? (seed * 2654435761u) | 1u : 1u;
			shapeRemaining = 0.0f;

			// The words, read from the movie's own list. Captured HERE, at the
			// instant the voice starts, because that is when the highlight still
			// holds the line being spoken.
			//
			// Still read from the movie whatever announced the line. DBReV carries
			// `topicKey` and the command path can read the filename back, but both
			// of those are the SANITIZED form used to build a filename — spaces and
			// anything NTFS rejects already replaced by underscores — and
			// punctuation is exactly what the phoneme builder reads to place its
			// pauses.
			//
			// So it is the second choice, not the first. It is still much better
			// than nothing: mangled punctuation costs the pauses, whereas no text
			// at all drops the whole line to a synthesized cadence with no relation
			// to the words. That fallback fired in the 2026-08-18 log.
			std::string topic = Interface::ReadSelectedTopic();
			if (topic.empty()) topic = highlightedTopic;
			highlightedTopic.clear();
			if (topic.empty() && !suppliedKey.empty()) {
				topic = suppliedKey;
				Log::Info(Log::Category::kStaging,
					"LipSync: topic unreadable from the movie; falling back to {} \"{}\"."sv,
					eventDriven ? "DBReV's key"sv : "the filename"sv, topic);
			}

			voiceLineText = topic;
			++voiceLineSerial;
			BuildFromText(topic, supplied);
			fromText = !phones.empty();

			// WHEN THE MOUTH STOPS, and this is set AFTER BuildFromText on purpose.
			//
			// The first cut of this took the window from DBReV alone, which quietly
			// left the DBVO path exactly as broken as it found it — and it did not
			// need to be. `lineSeconds` is whatever measurement actually landed,
			// from either source: DBReV's audioSeconds when it supplied one, and
			// MeasureTopic's own reading of the .fuz when it did not. On a DBVO
			// profile that second one is a real measurement off the real file, and
			// LipSync has been computing it all along purely to scale phonemes with.
			//
			// So the five-times overrun is fixed for DBVO users too, on every line
			// where the filename guess lands. That was roughly half of them in the
			// 2026-08-18 log, and half is a great deal better than none.
			//
			// AND THE GUESS IS NO LONGER WHAT DBVO USERS DEPEND ON. Once SpeakSound
			// has named a line, `supplied` is a measurement off the exact file and
			// the other half of those lines land too. MeasureTopic's heuristics stay
			// for the first line of a session, when nothing has been named yet, and
			// for a profile whose voice mod never touches the command.
			//
			// AND IT IS FIXED FOR NON-ENGLISH PLAYERS, which it was not until
			// BuildFromText stopped throwing the measurement away whenever the words
			// produced no phonemes. Every line of a Japanese, Russian or Chinese
			// game took that path, so `lineSeconds` was always 0 here and this
			// expression always fell through to the two behind it. See the note at
			// the phones.empty() branch in BuildFromText.
			//
			// suppliedTotal is the last resort: DBReV said it could not measure the
			// audio, so the conversation window is the only number anyone has.
			audioWindow = lineSeconds > 0.0f ? lineSeconds :
			              (supplied > 0.0f ? supplied : suppliedTotal);

			// Start the schedule already a lead's worth in, so the smoothed mouth
			// arrives with the audio rather than just behind it.
			phoneRemaining = -kLeadSeconds;

			if (fromText) {
				if (lineSeconds > 0.0f) {
					Log::Info(Log::Category::kStaging,
						"LipSync: \"{}\" - {} positions over {:.2f}s {} (syllables x{:.2f}, pauses x{:.2f})."sv,
						topic, phones.size(), lineSeconds,
						supplied > 0.0f ? suppliedFrom : "from the .fuz"sv,
						lineScale, lineGapScale);
				} else {
					// Why there is no measurement, named per source. "No .fuz
					// matched" is a filename guess that missed and belongs only to
					// the polling path; the other two knew exactly which file it was
					// and still could not read it.
					const std::string_view why =
						eventDriven   ? "DBReV could not measure the file"sv :
						commandDriven ? "the file SpeakSound named could not be measured"sv :
						                "no .fuz matched"sv;

					Log::Info(Log::Category::kStaging,
						"LipSync: \"{}\" - {} positions, ESTIMATED length ({})."sv,
						topic, phones.size(), why);
				}
			} else if (verdictReported.Take()) {
				// WHY THERE ARE NO SHAPES, AND WHETHER THERE IS STILL A CLOCK.
				//
				// This used to be one line saying the topic could not be read, and
				// that is only one of the two ways to get here. Text that reads
				// perfectly well but is not written in a-z produces exactly the same
				// empty sequence — which is EVERY line of a non-English game — so
				// the log named the wrong cause for the largest group of players it
				// applied to, and named it once, quietly, as a fallback rather than
				// as a fault.
				//
				// The window is reported with it because that is the half that was
				// broken and is now fixed. A synthesized cadence with a measured end
				// is a mouth that stops when the voice does; the same cadence
				// running on the conversation window is the overrun.
				const std::string_view clock = audioWindow <= 0.0f ?
					"nothing to stop it but the stuck-line backstop"sv :
					lineSeconds > 0.0f ?
					"stopping on the measured length of the recording"sv :
					"stopping on the conversation window - no file was measured"sv;

				if (topic.empty()) {
					Log::Info(Log::Category::kStaging,
						"LipSync: no topic text readable; synthesized cadence, {}."sv, clock);
				} else {
					Log::Info(Log::Category::kStaging,
						"LipSync: \"{}\" holds no letters the phoneme builder reads - a non-Latin "
						"localisation does exactly this; synthesized cadence, {}."sv,
						topic, clock);
				}
			}
		}

		if (lineActive) {
			lineElapsed += a_delta;


			// THE AUDIO HAS STOPPED, SO THE MOUTH STOPS. This is the whole point.
			//
			// Deliberately audioSeconds and NOT totalSeconds, which is the trap the
			// author flags: totalSeconds adds the user's post-line delay, and a load
			// order running the 1510ms he quotes would leave the character mouthing
			// silently for a second and a half after the voice ended. That is the
			// same artefact as the eight-second overrun, just smaller.
			//
			// EndLine eases the target shut rather than snapping it, so this is the
			// mouth closing on the last syllable, not cutting off at it.
			if (audioWindow > 0.0f && lineElapsed >= audioWindow) {
				// Retire the handle as well as the line. Without this the still-
				// playing handle immediately looks like a fresh line; see the note
				// on consumedVoiceID.
				consumedVoiceID = activeVoiceID;
				EndLine();
			}
		}

		if (lineActive && !overran && lineElapsed > kMaxLineSeconds) {
			// The last-resort backstop, and it should now be unreachable on a DBReV
			// profile with a measurable file: the audio window closes the line long
			// before this. It still guards the DBVO handle-polling path, where a
			// handle that never cleared was the norm rather than the exception, and
			// a DBReV line whose file could not be measured at all.
			overran = true;
			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				target[i] = 0.0f;
			}
			Log::Warn(Log::Category::kStaging,
				"LipSync: the player's line has run past {:.0f}s with no end. Treating it as stuck and closing the mouth."sv,
				kMaxLineSeconds);
		}

		if (!driving) return;

		// DRIVING STARTS ON THE FIRST FRAME OF THE LINE, and the observation window
		// is now purely a report.
		//
		// The original held off for 0.20s to check whether the engine was already
		// animating the mouth, and stood down if the phoneme channel moved. That
		// guard was dropped once — activity there turned out to be Conditional
		// Expressions and Expressive Facial Animation, not lipsync — but the WAIT
		// was left behind, which cost a fifth of a second of stillness at the start
		// of every line for a decision that no longer gets made.
		//
		// The 2026-08-13 run closes it properly: across a whole conversation the
		// player's channel lit exactly one slot, held a constant 0.050 through the
		// spoken line, and belongs to those two mods. Nothing else drives this
		// mouth, so there is nothing to defer to and no reason to be late.
		if (enabled && lineActive && !windowSampled) {
			const float peak = PeakPhoneme(FaceOf(player));
			if (peak >= 0.0f) {
				windowSampled = true;
				windowMin = peak;
				windowMax = peak;
				if (verdictReported.Take()) {
					Log::Info(Log::Category::kStaging,
						"LipSync: driving the player's mouth. Channel was at {:.3f} on entry - reported, not acted on."sv,
						peak);
				}
			}
		}

		if (lineActive && !overran) {
			if (fromText) {
				phoneRemaining -= a_delta;
				while (phoneRemaining <= 0.0f && phoneCursor < phones.size()) {
					const auto& phone = phones[phoneCursor++];
					phoneRemaining += phone.seconds;

					for (std::uint32_t i = 0; i < kSlotCount; ++i) {
						target[i] = 0.0f;
					}

					// A little stress variation, kept narrow. The shapes are the
					// words now, so this is emphasis rather than invention, and a
					// wide range would distort the very thing being got right.
					const float stress = 0.78f + RandomUnit() * 0.22f;
					target[kSlotJaw] = phone.shape.jaw * stress;
					if (phone.shape.slot < kSlotCount && phone.shape.weight > 0.0f) {
						target[phone.shape.slot] = phone.shape.weight * stress;
					}
				}

				// The words ran out before the audio did — the estimate was short,
				// or the recording has a tail. Rest rather than invent more.
				if (phoneCursor >= phones.size() && phoneRemaining <= 0.0f) {
					for (std::uint32_t i = 0; i < kSlotCount; ++i) {
						target[i] = 0.0f;
					}
				}
			} else {
				shapeRemaining -= a_delta;
				if (shapeRemaining <= 0.0f) {
					ChooseShape();
				}
			}
		}

		// Exponential approach, framerate-independent, per direction. This is what
		// turns a sequence of poses into a mouth.
		const float attack = 1.0f - std::exp(-a_delta / kAttackSeconds);
		const float release = 1.0f - std::exp(-a_delta / kReleaseSeconds);
		const float jawAttack = 1.0f - std::exp(-a_delta / kJawAttackSeconds);
		const float jawRelease = 1.0f - std::exp(-a_delta / kJawReleaseSeconds);

		float peak = 0.0f;
		for (std::uint32_t i = 0; i < kSlotCount; ++i) {
			const bool  opening = target[i] > current[i];
			const bool  jaw = i == kSlotJaw;
			const float rate = jaw ? (opening ? jawAttack : jawRelease) :
			                         (opening ? attack : release);

			current[i] += (target[i] - current[i]) * rate;
			peak = std::max(peak, current[i]);
		}

		// Once the line is over AND the mouth has actually eased shut, stop writing
		// and hand the channel back. Stopping the instant the handle cleared would
		// leave the last shape frozen on the face.
		if (!lineActive && peak < 0.002f) {
			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				current[i] = 0.0f;
			}
			driving = false;
		}

		writing.store(enabled && driving, std::memory_order_relaxed);

	}

	bool LipSync::PlayerSpeaking() noexcept
	{
		return lineActive && !overran;
	}

	std::uint64_t LipSync::PlayerLineSerial() noexcept
	{
		return voiceLineSerial;
	}

	std::string_view LipSync::PlayerLineText() noexcept
	{
		return voiceLineText;
	}

	float LipSync::PlayerLineDuration() noexcept
	{
		return audioWindow;
	}

	float LipSync::PlayerLineElapsed() noexcept
	{
		return lineElapsed;
	}

	void LipSync::OnResponse()
	{
		// A reply retires stale kPlaying handles; they cannot restart the old line.
		consumedVoiceID = PlayingVoiceID(HighOf(RE::PlayerCharacter::GetSingleton()));
		EndLine();
		highlightedTopic.clear();
	}

	bool LipSync::Sample(float* a_out, std::uint32_t a_count) noexcept
	{
		if (!a_out || !writing.load(std::memory_order_relaxed)) {
			return false;
		}

		const std::uint32_t count = std::min(a_count, kSlotCount);
		for (std::uint32_t i = 0; i < count; ++i) {
			// The hinge takes the dial as written; the lips take the softer curve.
			// See lipStrength.
			const float scale = (i == kSlotJaw) ? strength : lipStrength;
			a_out[i] = std::clamp(current[i] * scale, 0.0f, 1.0f);
		}
		return true;
	}
}

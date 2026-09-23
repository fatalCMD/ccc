#include "SD/Camera/Director.h"

#include "SD/Camera/Presets.h"
#include "SD/Camera/PlayerVoiceHandoff.h"
#include "SD/Camera/ReactionShots.h"
#include "SD/Camera/ReplyBoundary.h"
#include "SD/Camera/Shot.h"
#include "SD/Camera/ShotAngles.h"
#include "SD/Camera/ShotSelection.h"
#include "SD/Camera/Space.h"
#include "SD/Camera/VisibilityRecovery.h"
#include "SD/Compat/DBReV.h"
#include "SD/Compat/ImprovedCamera.h"
#include "SD/Compat/SmoothCam.h"
#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"
#include "SD/Core/Text.h"
#include "SD/Dialogue/MenuWatch.h"
#include "SD/Dialogue/Session.h"
#include "SD/Render/Letterbox.h"
#include "SD/Runtime.h"
#include "SD/Scene/Focus.h"
#include "SD/Scene/Interface.h"
#include "SD/Scene/KeyLight.h"
#include "SD/Scene/LightRig.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/Presence.h"

#include <chrono>

namespace SD::Camera
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		bool              staging{ false };
		RE::ActorHandle   subject{};
		float             side{ 1.0f };

		// Whether Close owes the player their first-person view back.
		//
		// Recorded at Open rather than worked out at Close, because by then the
		// only evidence left is a third-person camera, and that looks identical
		// whether this mod put the player there or they walked up that way.
		bool              returnToFirstPerson{ false };

		enum class ViewMode { kCinematic, kFirstPersonFallback };
		ViewMode viewMode{ ViewMode::kCinematic };
		bool fallbackRequested{ false };
		bool fallbackThirdPersonOwed{ false };
		bool suspendedInFallback{ false };
		bool suspendedThirdPersonOwed{ false };
		bool restoreThirdPersonPending{ false };
		VisibilityRecovery visibilityRecovery{};
		std::atomic<bool> protectionSettingsDirty{ false };
		bool visibilityFailedObservation{ false };
		std::optional<Subjects> frameSubjects;
		Pose protectedPose{};
		ShotType protectedShot{ ShotType::kTwoShot };
		Clock::time_point visibilityCheckedAt{};
		Clock::time_point protectedRetryAt{};
		RE::NiPoint3 checkedNpc{}, checkedPlayer{}, checkedCamera{};
		float checkedLens{ 0.0f };
		std::uint32_t fallbackTurn{ 0 };
		std::uint32_t fallbackCue{ 0 };

		struct ProtectedSearch
		{
			std::array<ShotType, static_cast<std::size_t>(ShotType::kCount)> order{};
			std::size_t count{ 0 }, cursor{ 0 };
			bool active{ false }, recovery{ false };
			std::uint32_t turn{ 0 };
			Framing framingAtStart{ Framing::kAuto };
			Pose best{};
			ShotType bestType{ ShotType::kTwoShot };
		};
		ProtectedSearch protectedSearch{};

		// ---- SUSPENDED, RATHER THAN FINISHED ---------------------------------
		//
		// A conversation whose screen was taken by an inventory, a container, a
		// barter window or any other menu that owns the frame is still THERE
		// underneath it: the session is live, the partner has not changed, and the
		// cinematic is owed back the moment the menu closes. A conversation that
		// ENDED is not, and putting bars back over an empty street would be worse
		// than never restoring them.
		//
		// Nothing else in this file could tell the two apart. Close() is the same
		// call either way, and Runtime opens once per partner, so a release taken
		// for a menu had to either be permanent or be undone by something that
		// remembered why it happened. This is that memory, and it is what the
		// reported inventory regression turned on.
		bool       suspendedForMenu{ false };
		RE::FormID suspendedPartner{ 0 };

		// AND WHICH CONVERSATION, not just which person.
		//
		// The partner's form id cannot tell two consecutive conversations with one
		// actor apart, which is the whole reason Runtime keys on a serial as well.
		// Without this a session that ended and restarted with the same person
		// while a menu owned the screen came back as a resume and inherited the
		// previous conversation's angle and debts. Zero is "nothing suspended".
		std::uint32_t suspendedSerial{ 0 };

		// Held ACROSS the suspension rather than paid out at it.
		//
		// Close() hands first person back if this mod took it, which is right at
		// the end of a conversation and wrong in the middle of one: it would drop a
		// first-person player into their own eyes for as long as the inventory was
		// open, throw the camera back at them, and then — because the flag is
		// cleared on the way out — leave them in third person when the conversation
		// really ended. Recorded here and re-owed on the resume.
		bool suspendedFirstPersonOwed{ false };

		// The angle that was on screen. Restored on the resume so coming back from
		// a trade returns to the shot the conversation was on, rather than drawing
		// a fresh opener and reading as a second conversation starting.
		ShotType suspendedShot{ ShotType::kTwoShot };

		// WHICH CONVERSATION IS STAGED, from Session's own count.
		//
		// The partner's form id was the only thing Open compared, and two
		// conversations with one person are one id — so a second conversation with
		// somebody who never stopped talking met the "already staged" guard and was
		// silently dropped. Zero is "nothing staged", which no live session ever
		// is: Session counts from one.
		std::uint32_t stagedSerial{ 0 };

		ShotType          currentShot{ ShotType::kTwoShot };
		ShotType          previousShot{ ShotType::kTwoShot };
		Clock::time_point shotSince{};
		Clock::time_point stagingSince{};
		bool              haveShot{ false };

		bool              npcSpeaking{ false };
		bool              wasSpeaking{ false };
		bool              pendingSpeaking{ false };
		Clock::time_point pendingSince{};
		Clock::time_point turnBeganAt{};

		// Lines the NPC has delivered in this conversation. The anchored
		// environmental shot only becomes available once a couple have passed â€”
		// it is a change of pace, and there is nothing to change pace from at the
		// start of an exchange.
		std::uint32_t linesThisTurn{ 0 };

		// Long enough to swallow the gap between two responses, short enough that a
		// genuine handover still feels immediate.
		constexpr float kTurnDebounceSeconds = 0.40f;

		// How long the spent topic list is held before it starts fading, once the
		// turn has passed to the NPC.
		//
		// Made a dial, at 2.4s default, after a player building a Baldur's Gate
		// style setup reported the options taking too long to go. They were right
		// and there was nothing they could set: iListReturnDelay governs the list
		// coming BACK, and this — the only number that governs it leaving — was a
		// constant. 2.4 plus the 0.55 fade is very nearly three seconds of spent
		// options sitting over the start of the reply.
		//
		// 0 gives the immediate fade that style wants; the default is unchanged, so
		// nobody who has not asked for it sees a difference.
		float           choiceFadeDelay{ 2.4f };

		// How long the spent list takes to go, once it starts going.
		//
		// Split from the fade-IN below and shortened to a quarter second, because
		// they are not the same gesture. Options leaving is the acknowledgement that
		// a choice was made and wants to be quick — the slower it is, the more it
		// reads as the interface lagging behind the player rather than responding to
		// them. Options arriving is an invitation and can afford to ease.
		//
		// One constant used to serve both at 0.55s, which made the acknowledgement
		// twice as slow as it needed to be and was reported as exactly that.
		float           choiceFadeOut{ 0.25f };

		// The ease-in, unchanged. Also still the timebase for the return branch.
		constexpr float kChoiceFadeSeconds = 0.55f;

		// WHAT USED TO BE HERE: bHideChoicesOnGreeting, which let the opening
		// greeting hide the topic list. Added on request and removed one build
		// later, on the same requester's judgement, which was right.
		//
		// It reinstated the oldest bug in this file. Accept commits the HIGHLIGHTED
		// topic without consulting the cursor and this mod does not gate dialogue
		// input, so a hidden list is still a live list: every blind selection ever
		// reported here happened on the approach to an NPC, where the click meant to
		// start the conversation landed on topic one. Shipping a switch whose
		// documentation has to end with "do not click through a greeting" is
		// shipping the bug with instructions for triggering it.
		//
		// The need behind the request is real and is met elsewhere — see the
		// last-response return below, which is what the CRPG feel actually wants:
		// the options come back DURING the NPC's final line rather than after it.
		// Do not re-add this without first closing the Accept path.

		// How long the player's turn must hold before the list is allowed back.
		// Longer than the gap between two responses in one reply, shorter than any
		// real pause for the player to read. See the note in the "your turn" branch.
		// Lowered from 0.65 and made a dial. Reported as "there's like a delay
		// before the dialogue menu comes back", which it plainly was.
		//
		// 0.65 was sized to cover the gap BETWEEN two responses of one reply, back
		// when a gap was the only thing distinguishable from a finished turn. That
		// is no longer what holds the list down — choiceSpentThisTurn does, and it
		// keys off the choice being spoken rather than off a stopwatch. So this is
		// free to be short again, and only has to stop the list twitching on a
		// single dropped frame of speech.
		float listReturnDelay{ 0.20f };

		// WHAT USED TO BE HERE: the dead man, which forced the topic list back on
		// screen after four seconds hidden with nobody speaking.
		//
		// It existed because the machine above it was known to strand, and it was
		// itself a stopwatch, so it fired in the middle of a four-second voiced line
		// and brought the list up under the player's own delivery. Both halves are
		// gone: the phase is engine-driven and cannot latch, and an unreadable movie
		// fails open at full opacity instead.
		//
		// Nothing replaced it. See the note where the timer would have gone, in the
		// topic list block: a stopwatch cannot tell a stuck movie from a long
		// monologue, so there is no timer here at all.

		// Last logged movie phase, so transitions are reported rather than every
		// frame. No longer a prototype: this is what drives the topic list.
		Scene::Interface::MenuPhase lastMoviePhase{ Scene::Interface::MenuPhase::kUnknown };

		// The player's voice handle, timed and retired here rather than believed.
		// See the note at VoicePlaying for why both are needed.
		std::uint32_t     voiceHandleID{ RE::BSSoundHandle::kInvalidID };
		std::uint32_t     retiredVoiceID{ RE::BSSoundHandle::kInvalidID };
		Clock::time_point voiceHandleSince{};
		std::uint16_t cueIntensity{ 50 };

		// WHAT USED TO BE HERE: cueEmotion, and a table that turned the line's
		// emotion and intensity into a change of lighting — a harder, colder key on
		// an angry line, fill and warmth on a happy one.
		//
		// It worked. It went because nobody asked the mod to have opinions about
		// their conversations, and every one it has is another thing to understand
		// before the lights can be switched on. The emotion data is still read and
		// still drives shot choice and faces, which is where it earns its place.

		// What SetLook was last handed, so it is called on a change and not every
		// frame. A lamp arriving from nothing resets its own fade, so re-announcing
		// an unchanged look would hold every cross-fade permanently at frame one.
		int lastLookApplied{ -1 };

		// Whether each angle gets its own look and nudge, and the global nudge that
		// applies either way.
		bool lightPerShot{ false };
		int  lightOffsetX{ 0 };
		int  lightOffsetY{ 0 };
		int  lightOffsetZ{ 0 };

		std::uint32_t cueCount{ 0 };
		bool          roomy{ true };
		RE::NiPoint3  openDirection{ 1.0f, 0.0f, 0.0f };
		float         openDistance{ 400.0f };
		float         screenAspect{ 1.78f };

		// WHAT KIND OF SPACE THIS IS, from the twelve probes that were already
		// being cast and thrown away.
		//
		// The three signatures are genuinely different rooms and want different
		// vocabularies: two long bearings and ten short ones is a corridor, where a
		// wide shot down the length works and one across it does not; all twelve
		// middling is an ordinary room; all twelve long is a market square. Until
		// now all three were the same single bool, decided from two bearings that
		// were not even among these.
		enum class Space : std::uint8_t
		{
			kTight,  // a corridor, a stairwell, a small cell
			kRoom,   // an ordinary interior
			kOpen    // a hall, a square, outdoors
		};

		Space roomSpace{ Space::kRoom };

		[[nodiscard]] std::string_view SpaceName(Space a_space) noexcept
		{
			switch (a_space) {
			case Space::kTight: return "tight"sv;
			case Space::kOpen:  return "open"sv;
			default:            return "room"sv;
			}
		}

		// Clear world units above the conversation. Zero means unmeasured, which
		// every rise treats as "no clamp" — the behaviour before there was a probe.
		float ceilingRoom{ 0.0f };

		// Whether `side` has been committed for this conversation. A first decision
		// and a re-decision are not the same question; see ChooseSide.
		bool sideCommitted{ false };

		// LIVE CONTROL, SET FROM THE INPUT THREAD AND DRAINED ON THE TICK.
		//
		// Atomics because nothing else here is thread-safe and the input sink runs
		// on the game's input thread, not the camera's. The requests carry no
		// payload — pressing the framing key twice before a frame runs is one
		// cycle, not two, which is the right answer for a key a player is tapping.
		std::atomic<bool> requestCut{ false };
		std::atomic<bool> requestFraming{ false };

		// Consumed by the cut gate on the frame after the key. Not an atomic
		// because by the time it is set the request has already crossed onto this
		// thread; it exists so a forced cut survives being asked for on a frame
		// where the gate could not run.
		bool forcedCut{ false };

		Framing framing{ Framing::kAuto };

		// A FRAMING OVERRIDE LASTS ONE TURN, THEN HANDS BACK.
		//
		// It used to hold until the conversation ended or the key was pressed
		// again, and that is the wrong shape for what people reach for it to do.
		// The reported case: the NPC is mid-line, you press to look at yourself,
		// you read the options and pick one — and then the camera stays on you
		// through their reply, waiting for a second press nobody thinks to make.
		// The override stopped being a choice about this moment and became a mode
		// you were stuck in.
		//
		// One turn is exactly right, and the reason it works is that AUTO agrees
		// with the override for the rest of the turn it was set in. Press "show me"
		// during their line: it releases when that line ends, at which point auto
		// puts the camera on you anyway for your turn — so the release is invisible
		// — and then their reply moves it back, which is the thing that was stuck.
		//
		// Counted in turns rather than timed, because a turn is the unit the player
		// is actually thinking in and a clock would expire mid-line or outlast a
		// short one.
		std::uint32_t turnSerial{ 0 };
		std::uint32_t framingUntilTurn{ 0 };

		// The player's turn belongs to the player. See Tunables for the reasoning;
		// mirrored here because Coverage() runs in the picker's inner loop.
		//
		// DECLARED HERE, ahead of SubjectIsNpc, because that is now a reader. It
		// used to sit with the rest of the mirrored dials further down, which was
		// fine while only the picker consulted it — and the fact that the one
		// function named "who does the camera belong on" could not see it is a
		// fair summary of the bug that moved it.
		bool coverPlayerTurn{ true };
		PlayerVoiceHandoff playerVoiceHandoff;

		// Seconds; see Tunables::playerVoiceHold. Applied to the handoff on the
		// tick, since ApplyTunables runs on the menu thread.
		float playerVoiceHoldSeconds{ 0.0f };

		ReactionShots           reactionShots;
		ReactionShots::Settings reactionSettings{};

		// Topic-pick detection for replyBoundary: the last values the tick saw.
		ReplyBoundary               replyBoundary;
		Scene::Interface::MenuPhase lastPickPhase{ Scene::Interface::MenuPhase::kUnknown };
		std::uint64_t               lastPlayerLineSerial{ 0 };

		[[nodiscard]] bool ReactionActive()
		{
			return reactionSettings.enabled && reactionShots.Active();
		}

		// WHO THE CAMERA BELONGS ON THIS INSTANT.
		//
		// Every subject test in the file used to read npcSpeaking directly, which
		// was correct while "whoever is talking" was the only possible answer. With
		// a framing override there are two answers and they can disagree, so the
		// question is asked once, here, and the six places that need it agree by
		// construction rather than by everybody remembering to check the same pair
		// of things.
		//
		// kRoom is not expressible as a person and is handled separately at each
		// site — it does not force a side of the exchange, it removes the question.
		[[nodiscard]] bool SubjectIsNpc()
		{
			switch (framing) {
			case Framing::kThem: return true;
			case Framing::kYou:  return false;

			// AND "CUT TO YOU ON YOUR TURN", WHICH THIS USED TO IGNORE.
			//
			// coverPlayerTurn reached only two places — whether a neutral counts as
			// stranded over your turn, and whether a neutral is acceptable coverage
			// of it — so switching it off made a two-shot ALLOWED on your turn
			// without ever making a shot of you unwanted. Any enabled player angle
			// that placed well still won, and the camera still cut to you the
			// moment the NPC stopped. Reported, and the Shots page had been
			// promising otherwise in as many words: "Turn at least one on, or
			// switch off 'Cut To You On Your Turn'". That sentence is only true if
			// the setting removes the need for a player angle, so it removes it.
			//
			// Off means the subject simply does not change hands when they stop
			// talking. Neutrals are already welcome in that state, so a turn is
			// covered by holding them or going wide — never by cutting to you.
			//
			// A reaction overrides normal coverage, but not manual framing above.
			default:
				if (ReactionActive()) {
					return false;
				}
				return npcSpeaking || playerVoiceHandoff.Active() || !coverPlayerTurn;
			}
		}

		[[nodiscard]] bool FramingIsRoom()
		{
			return framing == Framing::kRoom;
		}

		[[nodiscard]] bool Drawable(ShotType a_type);
		[[nodiscard]] bool NeedsRoom(ShotType a_type);

		// IS THERE ANY SHOT THIS FRAMING COULD ACTUALLY USE?
		//
		// Asked before the cycle key commits to one, and the reason is a live
		// performance bug rather than tidiness. A framing with nothing enabled
		// behind it makes wrongSubject true on every frame of the conversation, at
		// the quarter-second turn floor — so the cut block runs sixty times a
		// second, draws four candidates, walks the ladder, fails every one of them
		// on the subject test, and finds nothing. It LOOKS like it worked, because
		// failing to cut leaves the previous angle up, and it is quietly the most
		// expensive thing the mod can be made to do.
		//
		// Cheaper to refuse to enter the state than to detect it afterwards.
		[[nodiscard]] bool FramingViable(Framing a_framing)
		{
			if (a_framing == Framing::kAuto) {
				return true;
			}

			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
				const auto type = static_cast<ShotType>(i);

				// THE SAME TEST THE PICKERS USE, INCLUDING THE ROOM GATE.
				//
				// Drawable alone is not enough and the difference is reachable: a
				// player who has switched off every neutral except the master shot
				// has one entry standing behind "the room", and the master needs
				// somewhere to stand. In a corridor the picker filters it out, the
				// weighted draw comes back empty, and the framing is viable on
				// paper and unsatisfiable in fact — which is precisely the
				// every-frame search this function exists to prevent.
				if (!Drawable(type) || (NeedsRoom(type) && !roomy)) {
					continue;
				}

				const bool neutral = IsNeutral(type);
				switch (a_framing) {
				case Framing::kRoom:
					if (neutral) {
						return true;
					}
					break;
				case Framing::kThem:
					if (!neutral && FavoursNpc(type)) {
						return true;
					}
					break;
				case Framing::kYou:
					if (!neutral && !FavoursNpc(type)) {
						return true;
					}
					break;
				default:
					return true;
				}
			}

			return false;
		}

		// The next framing the player could actually get. Skips the ones with
		// nothing enabled behind them rather than stopping on them, so the key
		// always changes something — a press that appears to do nothing is
		// indistinguishable from a key that is not bound.
		// THE FIRST PRESS MUST VISIBLY CHANGE SOMETHING.
		//
		// Cycling the enum in order sent auto -> them, and during the NPC's line
		// "them" is exactly what auto was already showing. So the key that is meant
		// to switch who you are looking at did nothing at all on the press that
		// mattered, and only worked on the second — which reads as a dropped input,
		// not as a cycle.
		//
		// From auto it now jumps to the side that is NOT on screen. Press it while
		// they talk and you are looking at yourself; press it while you are choosing
		// and you are looking at them. After that it walks the enum, so the room is
		// still one more press away.
		[[nodiscard]] Framing NextFraming(Framing a_from)
		{
			auto candidate = a_from;
			for (int step = 0; step < 4; ++step) {
				candidate = candidate == Framing::kAuto ?
					(npcSpeaking ? Framing::kYou : Framing::kThem) :
					static_cast<Framing>((static_cast<std::uint8_t>(candidate) + 1u) % 4u);

				if (FramingViable(candidate)) {
					return candidate;
				}
			}
			return Framing::kAuto;
		}

		// The last angle that actually placed, per subject, stored as an offset so
		// it still tracks a subject who moves.
		struct RememberedPose
		{
			Pose         pose{};
			RE::NiPoint3 anchor{};
			ShotType     type{ ShotType::kCount };
		};

		RememberedPose lastNpcPose{};
		RememberedPose lastPlayerPose{};
		Clock::time_point enabledRetryAt{};
		bool nativeView{ false };

		// What the current shot settled on, held until the next cut.
		//
		// Reset by watching shotSince rather than at each of the four sites that
		// start a shot — Open's two-shot, the opener, the open-shot hold and the
		// ordinary cut. Four reset sites is four chances to add a fifth and forget,
		// and this file's history is mostly that failure.
		float             heldSweep{ kUnheld };
		float             heldStandoff{ kUnheld };

		// The room the shot measured on the frame it cut, kept for its life.
		//
		// COMMITTED ONCE AND NEVER REVISED, which is the opposite of how heldSweep
		// and heldStandoff are maintained, and the difference is not an oversight.
		// Those two are re-committed every frame from a pose that re-derived them,
		// so writing them back is a no-op that keeps one rule. This one is the
		// input to the decision not to measure again: a frame that is holding
		// reports the held value straight back out of Solve, so re-committing it
		// would be laundering a remembered number into a fresh one, and the first
		// frame that measured would stop being the only one that did.
		float heldRoom{ kUnheld };

		Clock::time_point heldSince{};

		constexpr const char* kIniPath = ".\\Data\\SKSE\\Plugins\\SD.ini";

		// A cut wants a reason. These are the two that exist in a Skyrim
		// conversation: a new line beginning, and the turn passing between the
		// player's menu and the NPC's reply.
		bool              cueSinceCut{ false };
		bool              turnSinceCut{ false };
		Clock::time_point replyStartedAt{};

		// The player's beat: how long the camera stays on the player once the NPC
		// begins replying.
		//
		// Without a player-voice mod the reply starts immediately, so this only
		// needs to be long enough that the cut does not land on the same frame.
		// With DBVO the player actually speaks first and the NPC's cue arrives
		// afterwards â€” which the cue signal already handles on its own â€” so the
		// beat mainly buys a moment for the delivery to register.
		float playerBeatSeconds{ 0.45f };

		// Detecting DBVO by its DLL does not work, and quietly did nothing.
		//
		// Current DBVO ships as an ESP plus Papyrus and voice packs â€” there is no
		// DLL to find. GetModuleHandleW returned null on a profile with DBVO
		// enabled and 140-odd voice packs installed, so the player beat stayed at
		// the unvoiced default of 45 and the camera cut off the player's line
		// part-way through. Exactly the users this was written for got the wrong
		// timing, and nothing in the log said so.
		//
		// The plugin is the real signal. The DLL check is kept in case a build ever
		// ships one, but the ESP is what actually resolves.
		[[nodiscard]] bool DetectPlayerVoice()
		{
			constexpr std::array kModules{
				L"DBVO.dll",
				L"DragonbornVoiceOver.dll",
				L"DBVO_SE.dll",
			};
			for (const auto* name : kModules) {
				if (::GetModuleHandleW(name)) {
					Log::Info(Log::Category::kCamera, "Player voice detected by DLL."sv);
					return true;
				}
			}

			// LookupModByName covers both a full ESP and an ESL-flagged one; the
			// light-mod table is separate, so it is asked as well.
			if (auto* handler = RE::TESDataHandler::GetSingleton()) {
				constexpr std::array kPlugins{ "DBVO.esp"sv, "DBVO.esl"sv };
				for (const auto& name : kPlugins) {
					if (handler->LookupModByName(name) || handler->LookupLoadedLightModByName(name)) {
						Log::Info(Log::Category::kCamera, "Player voice detected by plugin {}."sv, name);
						return true;
					}
				}
			}

			Log::Info(Log::Category::kCamera, "No player-voice mod detected; using the unvoiced beat."sv);
			return false;
		}

		Clock::time_point lastFrameAt{};
		bool              haveFrameTime{ false };

		// The player's own field of view, and the one this conversation composes
		// against.
		//
		// Sampled on idle frames rather than read off the camera at Open, and that
		// distinction is the whole reason this is two variables. Shots ask for a
		// narrower lens; if the value a conversation starts from were read back off
		// a camera SD had already narrowed — because a previous Close was skipped
		// by a crash, a load or a force-exit — every conversation would start from
		// the last one's tightest shot and the field of view would walk itself shut
		// over an evening's play.
		//
		// WHICH idle frames is decided in SampleCameraRest, with the aim and the
		// zoom, and not by this variable's reader. It is not enough to be unstaged:
		// other mods write worldFOV too, and the frames just after a menu closes
		// are the ones where they are mid-flight.
		//
		// A reading under the floor is refused for the same reason: it is the stuck
		// state itself, and believing it is how the fault re-latches.
		constexpr float kSaneMinFov = 45.0f;
		float           restingFov{ 0.0f };
		float           baseFov{ 75.0f };

		// THE THIRD-PERSON CAMERA AS THE PLAYER HAD IT, FOR THE SAME REASON AND BY
		// THE SAME METHOD.
		//
		// Reported as: "after the dialogue ends, the third person camera snaps to a
		// different angle, most of it were looking down completely."
		//
		// The snap is not this mod moving the camera. It is this mod STOPPING. SD
		// stamps the camera node every frame from a pose of its own, so for the
		// length of a conversation nobody sees where the engine thinks the camera
		// is — and the engine spends that whole time aiming its own dialogue camera
		// at the person being spoken to, pitching down at a seated blacksmith or a
		// child and pulling the zoom in. Vanilla eases out of that when the menu
		// closes and the player watches it happen. Here the last stamped frame is
		// followed immediately by the engine's, so the ease is not seen; its
		// STARTING POSE is, as a cut, and that pose is a camera looking at the
		// floor.
		//
		// So the fix is not to move anything. It is to put back what the player had
		// before the conversation touched it, which is what Close() already claims
		// in as many words and until now did for the field of view alone.
		//
		// translation and rotation are deliberately NOT in here. They are the
		// state's record of where the camera physically is, and restoring a pose
		// captured before the conversation would drop the camera wherever the
		// player was standing when it began — which after a walk-and-talk is a
		// different room. The engine recomputes both from the fields below.
		struct CameraRest
		{
			RE::NiPoint2 freeRotation{};        // yaw, pitch — the one that reads as "looking down"
			RE::NiPoint3 posOffsetExpected{};
			RE::NiPoint3 posOffsetActual{};
			float        targetZoomOffset{ 0.0f };
			float        currentZoomOffset{ 0.0f };
			float        savedZoomOffset{ 0.0f };
			float        pitchZoomOffset{ 0.0f };
			float        targetYaw{ 0.0f };
			float        currentYaw{ 0.0f };
			bool         freeRotationEnabled{ false };
		};

		CameraRest cameraRest{};
		bool       cameraRestPrimed{ false };

		// IS THIS CLOSE A SUSPENSION OR AN ENDING? See Close(), which is the only
		// reader, and OnScreenTaken, which is the only writer.
		//
		// The distinction exists because of a SmoothCam report: exiting a trade
		// menu left somebody's SmoothCam settings wrong. A suspension is coming
		// straight back, so the view does not need handing back across it — and
		// handing it back means writing nine fields of the third-person camera
		// state, one of which is the persistent zoom.
		bool suspendingForMenu{ false };

		// When a screen-owning menu last LET GO, for the settle window in
		// SampleCameraRest. Epoch means never, which reads as long ago and is what
		// an untouched session should see.
		//
		// WRITTEN FROM THE MENU EVENT, NOT FROM THE TICK, and that is a fix rather
		// than a preference. This was `screenTakenAt`, stamped by SampleCameraRest
		// on any frame that saw a menu open — which for a pausing menu is a frame
		// that never happens, because the frame source stops with the game. The
		// newest value it could ever hold was the moment the menu opened, so a
		// container held open for longer than the window left it already expired on
		// the first frame back, and the settle it promised had never once run.
		// MenuWatch delivers the real edge; see Director::OnScreenReleased.
		Clock::time_point screenReleasedAt{};

		// How long after a screen-owning menu closes before the camera's resting
		// state is worth reading again.
		//
		// SmoothCam re-establishes its own interpolation when the world starts
		// again, and for the first frames after a menu the third-person state holds
		// values that are on their way somewhere rather than at rest. Half a second
		// is comfortably past that and far below the time it takes a player to walk
		// out of a shop and start a conversation.
		constexpr float kRestSettleSeconds = 0.5f;

		// Set when the dialogue menu closes; the camera is handed back shortly
		// after unless the menu comes straight back.
		bool              releasePending{ false };
		Clock::time_point releaseSince{};

		// The direction dials.
		//
		// These were constants, and they are the numbers that actually decide how
		// the mod feels â€” everything else is plumbing. They are re-read per
		// conversation in Open(), which gives hot-reload for free: edit the ini,
		// start the next conversation, see the change.
		//
		// Defaults below are the shipped 1.0 values and are what every comment in
		// this file was written against. Changing a default silently invalidates
		// the reasoning next to it.

		// Raised after the cutting read as snapping rather than direction.
		//
		// Coverage cuts in a conversation scene sit somewhere around three to eight
		// seconds; at just over one second the camera is not directing, it is
		// channel-hopping. The floor is now long enough that a cut has to be earned.
		float minShotSeconds{ 2.4f };

		// Whoever is talking gets the camera, and short lines are common. This
		// floor only exists so a cut cannot land on the same frame as the previous
		// one; it is not a pacing control.
		float minTurnSeconds{ 0.25f };


		// The ceiling is a backstop, not a metronome. Cuts are meant to land on
		// motivated moments â€” a new line, the turn changing â€” and this only exists
		// so a shot cannot sit forever through a long silence.
		float maxShotSeconds{ 9.0f };

		// PER LINE ANGLE CHANGE: the cut cadence, counted in lines.
		//
		// The floors above answer "may the camera cut yet"; this answers "should
		// it". The camera holds a setup until it has seen this many eligible lines
		// and then takes a new angle. min and max differ so the count is re-rolled
		// after each cut and the rhythm stops being countable; min > max is swapped
		// rather than rejected. 3-6 is what ships.
		std::uint32_t cutEveryMin{ 3 };
		std::uint32_t cutEveryMax{ 6 };

		// Whether the line count is allowed to ask for an angle at all.
		//
		// The FIRST of two independent cut modes, and the pair are genuinely
		// independent: per-line only cuts on the count, timed only cuts on the
		// clock, both let either ask, and neither manufactures nothing at all —
		// which is a legitimate configuration, because coverage is not a cut mode.
		// See the tick for how the three tests are kept apart.
		bool perLineAngleChange{ true };

		// bCutOnLineEnd IS GONE, and with it the third edge in every exchange.
		//
		// A Skyrim exchange has three edges close together and all three used to
		// cut: the NPC starts talking, the NPC stops talking, the player picks a
		// topic and the NPC starts again. The middle one was always the weak
		// member — nothing has happened except that a sentence finished — and on a
		// short line the three landed inside about two seconds, which reads as the
		// camera being restless rather than directing.
		//
		// The shot a line was framed on is now held through the pause after it,
		// always. See holdingThroughPause in the tick, which is the whole of the
		// enforcement and is no longer conditional on anything.
		//
		// The start-of-line edge is untouched where it is COVERAGE — the camera
		// moving because it is on the wrong person — and that is deliberately not
		// the same rule wearing a different name: coverage only ever moves the
		// camera TO whoever has started speaking, and can never fire on a line
		// ending.

		// Whether a line has enough in it to be worth a new setup.
		//
		// The cadence dials count lines and treat them all as equal, and they are
		// not. "Yes.", "Hmm.", "Need something?" and a forty-word explanation are
		// one line each, so a run of one-word acknowledgements gets the same fresh
		// angle per line that a speech does â€” the camera works hardest exactly
		// where there is least to look at.
		//
		// Word count is the signal because it is the only measure of a line's size
		// available at the moment the decision has to be made. Duration would be
		// better and is unknowable: the engine announces a line when it starts and
		// the length is only settled once it has finished, which is several cuts
		// too late. The authored subtitle is sitting on the response already.
		bool          holdOnShortLines{ true };
		std::uint32_t shortLineWords{ 4 };

		// TIMED ANGLE CHANGE: whether the staleness ceiling is allowed to fire, per
		// half of the exchange. Both OFF is the shipped behaviour, which makes
		// every cut in the mod a motivated one.
		bool timedCutsWhileSpeaking{ false };
		bool timedCutsWhileChoosing{ false };

		// Open straight onto whoever is already speaking.
		//
		// Walking up to somebody who greets you produced three setups inside a
		// couple of seconds: the two-shot the conversation opens on, a cut to the
		// speaker, and a cut to the player when they finished. The middle one is
		// the one nobody asked for â€” by the time it lands the line is half over,
		// so the establishing shot established nothing and the cut away from it
		// read as the camera changing its mind.
		//
		// With this on, a conversation that opens with someone talking starts on
		// them. No two-shot, no cut across the greeting, and the next move is the
		// handover when they stop: two setups for the whole exchange.
		//
		// Only applies when somebody IS speaking as the conversation opens. Open
		// into silence and the two-shot is still right â€” there is no line to cut
		// across, and a shot of the NPC while nobody talks is the wrong subject by
		// the tick's own test, which would cut to the player immediately. That is
		// what the two-shot being neutral is for.

		// The line rule and the crowd test. See Tunables for both; mirrored here
		// because Solve reads them off Subjects for every candidate it judges.
		bool enforceLine{ true };
		bool true180{ false };
		bool avoidCrowds{ true };

		// Set when true180 changes mid-conversation; handled on the tick.
		std::atomic<bool> lineRuleChanged{ false };

		// Whether a shot stops re-probing once it has cut. See Tunables.
		//
		// Mirrored here like the two above, but it does NOT reach Subjects the same
		// way they do. They are handed to every candidate; this one is handed only
		// to the shot being rendered, and only once that shot has something held to
		// render from. See where subjects.holdPlacement is filled.
		bool holdPlacement{ false };
		std::atomic<bool> protectSubject{ false };
		std::atomic<bool> firstPersonFallback{ true };

		// Whether the topic list fades out under the NPC's line. See Tunables.
		bool fadeTopicList{ true };

		// bLetterbox, mirrored out of Tunables. See Tunables for why the bars
		// stopped being a startup decision.
		bool letterboxWanted{ true };

		// THE LIST IS NEVER HIDDEN BEFORE THE ENGINE HAS DECLARED IT LIVE ONCE.
		//
		// A hidden list is still a live one: a click during a fade reaches Accept,
		// and Accept commits the HIGHLIGHTED topic without consulting the cursor.
		// This mod no longer stops that, by choice — see Runtime's note.
		//
		// What it can do is refuse to hide anything the player has not already seen.
		// Every blind selection ever logged here happened on the APPROACH to an NPC:
		// walk up, a greeting plays, the list fades under it, and the click that was
		// meant to open the conversation lands on topic one.
		//
		// This was playerHasChosen, inferred from a turn edge, and the inference is
		// what made it wrong: it survived into the next conversation still armed, so
		// `fading` was true on the opening frame and the list stayed hidden through a
		// whole greeting. Asked of the movie instead, it is one unambiguous question —
		// has eMenuState reported topicList yet.
		bool listWasLive{ false };

		// interfaceEngaged IS GONE. It recorded whether DriveInterface had run last
		// frame, purely to catch the falling edge: turn off both bHideInterface and
		// bFadeTopicList mid-conversation and the driver stopped being called, so
		// nothing was left to hand the topic list and the speaker name back and the
		// list sat at alpha 0 — invisible and still committing on Accept — until
		// Close. With the HUD hide no longer a setting the driver runs on every
		// staged frame, so there is no edge to catch.

		// The speaker-name hide as the ini and the menu last stated it, against
		// Interface's own copy as last APPLIED.
		//
		// Split in two because the writer and the applier are different threads.
		// ApplyTunables runs from the settings panel, which draws off the Present
		// hook; Suppress, RestoreHud and every _visible write behind them belong to
		// the tick. So the panel writes a bool and raises a flag, and
		// SyncInterfaceSettings does the Scaleform work one frame later.
		//
		// It used to be three of these — the HUD hide and the wholesale mode had one
		// each — and both of those are settings no longer. See Tunables.
		bool wantHideSpeakerName{ true };
		bool interfaceDirty{ true };

		// The animator, and the whole of the topic list's state.
		//
		// listWanted is the target the phase last asked for, listEdgeAt is when it
		// changed, and choiceEaseFrom is the alpha it was leaving. Nothing here is
		// derived from anything; the phase decides and these three carry the ease.
		bool              listWanted{ true };
		Clock::time_point listEdgeAt{};
		float             choiceAlpha{ 100.0f };
		float             choiceEaseFrom{ 100.0f };

		// The skip autopsy that used to live here is gone; it answered its question.
		//
		// Measured 2026-08-09, and worth keeping because it closes a line of enquiry
		// rather than opening one: stopping a line's audio does NOT shorten the line.
		// A 5.84s line cut at 0.89s still ended at 5.84s, and across that whole
		// window every field on the speaker's HighProcessData sat flat — voiceTimer,
		// voiceTimeElapsed, voiceRecoveryTime, soundDelay all 0.00, closeDialogueTimer
		// and clearTalkToListTimer constant. Nothing reachable on the actor holds a
		// line open. The only thing that moved was MenuTopicManager::currentTopicInfo
		// going null at line end, which is the engine reporting the result, not the
		// clock driving it.
		//
		// So a press cannot end a line early from out here. Do not try again without
		// a new mechanism; this was measured, not reasoned.

		// bFadeAfterPlayerLine. See Tunables for why this is a new key rather than
		// a rename of the flag whose stored meaning was the inverse of its label.
		bool fadeAfterPlayerLine{ true };

		// WHEN THE HOLD FOR THE PLAYER'S LINE BEGAN, AND THE CEILING ON IT.
		//
		// The hold pins the fade clock for as long as the player's voice is
		// playing, so it is exactly as reliable as the signal that says the line
		// ended. Under DBReV that signal is an event and is trustworthy; under
		// DBVO it is a sound handle, and a handle that never leaves kPlaying would
		// pin the clock for the rest of the conversation and leave a spent topic
		// list sitting at full opacity with nothing to take it down.
		//
		// So the hold is bounded. VoicePlaying has its own twenty-second ceiling on
		// the handle path and none at all on the event path; this is the one that
		// covers both, and it is measured from the moment the hold started rather
		// than from the line, so a missing end signal costs one delay and not a
		// conversation.
		Clock::time_point playerLineHeldSince{};
		bool              playerLineHeld{ false };

		// Fifteen seconds. Comfortably past any authored player line — the longest
		// in the vanilla topic set runs about nine — and short enough that a stuck
		// signal is a hesitation rather than a broken interface.
		constexpr float kMaxPlayerLineHold = 15.0f;

		// Said once per conversation, not once per session and not once per frame.
		// Reset in Open, so a profile where this fires every time says so every
		// time rather than only the first.
		Log::OnceFlag playerLineHoldExpired;

		// Whether the dialogue menu is up AND this mod intends to hide the topic
		// list during it. Read by the click guard on the input thread.
		//
		// The guard used to gate on Staging() alone, and that leaves a hole exactly
		// where the bug was reported: the dialogue menu opens and the greeting
		// starts BEFORE Director::Open runs — measured at about 120ms — so a press
		// in that window found the guard inert and fell straight through to Accept,
		// which commits topic zero. Mid-conversation presses were blocked correctly
		// the whole time, which is why the log looked healthy.
		std::atomic_bool dialogueMenuUp{ false };

		// bEnabled, mirrored out of Tunables by ApplyTunables and read by Runtime.
		//
		// Starts false and is filled by the LoadSettings that runs before anything
		// can stage. Runtime used to keep its own copy of this and push it in
		// through SetDirecting, which meant two owners for one flag — the shape of
		// every unfixable-looking settings bug in this file's history.
		bool directing{ false };


		// Diagnostic: hold a frontal close on the PLAYER when a conversation opens,
		// in milliseconds. 0 is off. Reasoning at HoldingOpenShot below.
		int               openShotHold{ 0 };
		Clock::time_point openShotUntil{};

		// Openers, for when the above is on.
		//
		// Hand-picked rather than drawn from the coverage pool, because the pool
		// is judged against room that has not been measured yet: ChooseSide and
		// the clearance probe run on the first frame of staging, not in Open, so
		// `roomy` is still describing the previous conversation. Every setup here
		// is tight or medium, needs no space to speak of, and frames the NPC â€”
		// safe in a corridor, a cellar, or the middle of a field.
		// Once each, like the coverage pools. This list used to name the shoulder
		// shot twice to favour it, which was harmless while the opener was drawn
		// uniformly and is not now that it is drawn by weight — a duplicate here
		// would double a slider behind the player's back, which is the exact thing
		// the weighting was rewritten to stop. All five are staples or near it, so
		// AuthoredWeight already favours the shoulder over the accents.
		constexpr std::array kOpeners{
			ShotType::kOverPlayerShoulder,
			ShotType::kMediumNpc,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kCloseUp,
		};

		// The same, on the other side of the eyeline.
		//
		// A conversation that opens in SILENCE has to open somewhere, and with the
		// establishing two-shot gone the only honest answer is the player: nobody
		// is speaking, so nobody else is the subject. Mirrors kOpeners setup for
		// setup, so the opening angle is the same size and shape whichever side of
		// the exchange it lands on.
		constexpr std::array kPlayerOpeners{
			ShotType::kOverNpcShoulder,
			ShotType::kMediumPlayer,
			ShotType::kDirtyPlayer,
			ShotType::kThreeQuarterPlayer,
			ShotType::kClosePlayer,
		};

		// Words in the authored subtitle.
		//
		// Whitespace runs, not std::isspace â€” passing a plain char to isspace is
		// undefined for anything above 0x7F, and Skyrim's dialogue is localised.
		// Punctuation is deliberately not stripped: "Yes." and "Yes" should both
		// count as one word, and they do.
		// ...AND THE HALF OF THE WORLD THAT DOES NOT USE SPACES.
		//
		// Kana and the CJK ideographs run together — 少し待ってくれ is seven
		// characters, three words and no spaces at all — so the whitespace rule
		// counts a whole sentence as one. With bHoldOnShortLines on and the floor
		// at four, that classified EVERY line in a Japanese or Chinese game as too
		// trivial to cut on, and the camera quietly stopped cutting.
		//
		// Hangul and Cyrillic are deliberately absent. Korean and Russian space
		// their words the way English does, so the ordinary path already counts
		// them correctly and adding them here would double-count.
		[[nodiscard]] bool UnspacedScript(char32_t a_cp) noexcept
		{
			return (a_cp >= 0x3040 && a_cp <= 0x30FF && a_cp != 0x30FB) ||  // kana, minus the middle dot
				(a_cp >= 0x3400 && a_cp <= 0x4DBF) ||                       // CJK extension A
				(a_cp >= 0x4E00 && a_cp <= 0x9FFF) ||                       // CJK unified
				(a_cp >= 0xF900 && a_cp <= 0xFAFF) ||                       // CJK compatibility
				(a_cp >= 0xFF66 && a_cp <= 0xFF9D);                         // halfwidth katakana
		}

		// The marks that end a run without being part of one.
		//
		// ASCII punctuation is NOT here, on purpose — that is the "Yes." rule above
		// — but its full-width cousins have to be, because they do not hang off the
		// end of a word the way a Latin full stop does. Counting 。 as a word of
		// its own would make はい。 two.
		[[nodiscard]] bool UnspacedBreak(char32_t a_cp) noexcept
		{
			return (a_cp >= 0x3000 && a_cp <= 0x303F) ||   // 、。「」《》 and the ideographic space
				a_cp == 0x30FB ||                          // ・
				(a_cp >= 0xFF01 && a_cp <= 0xFF0F) ||      // ！ through ／
				(a_cp >= 0xFF1A && a_cp <= 0xFF20) ||      // ： through ＠, which is where ？ lives
				a_cp == 0x2026;                            // …
		}

		// Characters per word in a script that does not space them.
		//
		// A Chinese word averages about one and a half characters and a Japanese
		// bunsetsu about two and a half, so two splits the difference. It does not
		// need to be better than that: the only question ever asked of the answer
		// is whether the line clears a floor of four.
		//
		// Measured against the shipped code on 2026-08-28: はい。 comes out at 1,
		// わかった。 at 2, 少し待ってくれ。 at 4, 好的。 at 1,
		// 我不知道你在说什么。 at 5. Which is the same shape the English count
		// has — interjections under the floor, sentences over it.
		constexpr std::uint32_t kUnspacedCharsPerWord = 2;

		// The two rules compose rather than compete, which is what a line mixing
		// scripts needs — a Japanese subtitle quoting a shout in Latin, say. A run
		// of unspaced characters is measured by its length; everything else is
		// still measured by the spaces around it.
		[[nodiscard]] std::uint32_t WordCount(const char* a_text)
		{
			if (!a_text) {
				return 0;
			}

			const std::string_view text{ a_text };

			std::uint32_t words = 0;
			bool          inWord = false;
			std::uint32_t unspacedRun = 0;

			// Rounds UP, so a single kanji is a word rather than nothing. A line one
			// character long is still a line somebody said.
			const auto flush = [&]() {
				if (unspacedRun > 0) {
					words += (unspacedRun + kUnspacedCharsPerWord - 1) / kUnspacedCharsPerWord;
					unspacedRun = 0;
				}
			};

			for (std::size_t pos = 0; pos < text.size();) {
				const char32_t cp = Text::NextCodepoint(text, pos);

				if (UnspacedScript(cp)) {
					// Closes any Latin word standing open, so the "Fus" in
					// Fusロ・ダー is not swallowed by the kana after it.
					inWord = false;
					++unspacedRun;
					continue;
				}

				flush();

				const bool space = cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
					UnspacedBreak(cp);
				if (space) {
					inWord = false;
				} else if (!inWord) {
					inWord = true;
					++words;
				}
			}

			flush();
			return words;
		}

		// Lines delivered on the current setup, and the number this setup is
		// holding for. Re-rolled at every cut.
		std::uint32_t linesSinceCut{ 0 };
		std::uint32_t cutEveryTarget{ 1 };

		[[nodiscard]] std::uint32_t NextRandom();

		[[nodiscard]] std::uint32_t RollCutEvery()
		{
			const std::uint32_t lo = std::min(cutEveryMin, cutEveryMax);
			const std::uint32_t hi = std::max(cutEveryMin, cutEveryMax);
			return hi <= lo ? lo : lo + (NextRandom() % (hi - lo + 1));
		}

		// The live copy, so the menu can read back what is actually in force rather
		// than re-parsing the ini and hoping the two agree.
		Tunables tunables{};

		// Defined below, with the pools it inspects. Declared here because
		// ReadTuning is what has just handed every shot a weight and is therefore
		// the honest place to say which of those weights can do nothing.
		void AuditPools();

		// Read every dial that changes how direction feels, once per conversation.
		void ReadTuning()
		{
			const Config::ReadScope settings;
			Tunables read{};
			read.minShotTime = Config::Int("Direction", "iMinShotTime", 240);
			read.minTurnTime = Config::Int("Direction", "iMinTurnTime", 25);
			read.maxShotTime = Config::Int("Direction", "iMaxShotTime", 900);
			// iLensBias is GONE, and so is every field that carried it. It was a
			// single global shift across forty-three setups whose lens choices are
			// most of what distinguishes them from each other, so it flattened the
			// thing it was adjusting. Per-setup FOV replaces it and reaches further
			// in both directions. Any value left in an old ini is simply not read.
			// EVERY FALLBACK BELOW IS THE CLOSE PRESET'S VALUE. See Tunables: the
			// initialisers, this list, config/SD.ini and Camera::kCloseStyle are
			// four statements of one set of numbers, and a fresh install has to
			// come up reading Close or the Presets page shows nothing ticked.
			read.letterbox = Config::Bool("Direction", "bLetterbox", true);
			read.letterboxHeight = Config::Int("Direction", "iLetterboxHeight", 120);
			read.cutEveryMin = Config::Int("Direction", "iCutEveryMin", 3);
			read.cutEveryMax = Config::Int("Direction", "iCutEveryMax", 6);
			read.perLineAngleChange = Config::Bool("Direction", "bPerLineAngleChange", true);
			read.holdOnShortLines = Config::Bool("Direction", "bHoldOnShortLines", true);
			read.shortLineWords = Config::Int("Direction", "iShortLineWords", 4);
			read.timedCutsWhileSpeaking = Config::Bool("Direction", "bTimedCutsWhileSpeaking", false);
			read.timedCutsWhileChoosing = Config::Bool("Direction", "bTimedCutsWhileChoosing", false);
			// bCutOnLineEnd is deliberately not read. A stale key in an old user
			// ini is ignored rather than migrated; there is nothing left for it to
			// mean. See the note where the flag used to live.
			read.enabled = Config::Bool("Direction", "bEnabled", true);
			read.coverPlayerTurn = Config::Bool("Direction", "bCoverPlayerTurn", true);
			read.enforceLine = Config::Bool("Direction", "bEnforceLine", true);
			read.true180 = Config::Bool("Direction", "bTrue180", false);
			read.avoidCrowds = Config::Bool("Direction", "bAvoidCrowds", true);
			read.holdPlacement = Config::Bool("Direction", "bHoldPlacement", false);
			read.protectSubject = Config::Bool("Direction", "bKeepSubjectVisible", false);
			read.firstPersonFallback = Config::Bool("Direction", "bFirstPersonFallback", true);
			read.fadeTopicList = Config::Bool("Direction", "bFadeTopicList", true);

			// The screen-furniture flags, read HERE rather than once at startup.
			// ReadTuning runs at load and again at every conversation open, so
			// hand-editing the ini now reaches them the same way it reaches every
			// other dial — and the menu reaches them through ApplyTunables without
			// waiting for either.
			//
			// bHideInterface and bHideHudWholesale used to be read here too. Both
			// are gone rather than pinned to a default: the HUD hide is what the mod
			// does, and the wholesale mode was only ever the price of a hide that
			// could not name what it was hiding. A value left in anyone's ini for
			// either key is simply not read.
			read.hideSpeakerName = Config::Bool("Direction", "bHideSpeakerName", true);
			read.fadeAfterPlayerLine = Config::Bool("Direction", "bFadeAfterPlayerLine", true);

			read.poseMode = Config::Int("Diagnostics", "iPoseMode", 0);
			openShotHold = std::clamp(Config::Int("Diagnostics", "iOpenShotHold", 0), 0, 10000);
			listReturnDelay =
				std::clamp(Config::Int("Direction", "iListReturnDelay", 20), 0, 200) / 100.0f;
			// Hundredths, like its sibling above. Kept in Tunables rather than as a
			// bare float so the menu's slider can read live state; see the note on
			// the field.
			read.choiceFadeDelay = std::clamp(Config::Int("Direction", "iChoiceFadeDelay", 150), 0, 600);
			choiceFadeDelay = static_cast<float>(read.choiceFadeDelay) / 100.0f;
			read.choiceFadeTime = std::clamp(Config::Int("Direction", "iChoiceFadeTime", 200), 5, 200);
			choiceFadeOut = static_cast<float>(read.choiceFadeTime) / 100.0f;
			// iListenerGaze and iSpeakerGaze are not read. The Eye Contact section
			// they belonged to is gone and the gaze model with it; 1.4 leaves head
			// tracking to the game.

			// The only dial whose default is not a constant. DetectPlayerVoice is a
			// function-local static so the lookup happens once, not per conversation.
			static const bool playerVoiced = DetectPlayerVoice();
			read.playerBeat = Config::Int("Direction", "iPlayerBeat", playerVoiced ? 90 : 45);
			read.playerVoiceHold = std::clamp(Config::Int("Direction", "iPlayerVoiceHold", 0), 0, 300);

			read.reactionShots = Config::Bool("Direction", "bReactionShots", false);
			read.reactionEvery = std::clamp(Config::Int("Direction", "iReactionEvery", 3), 1, 10);
			read.reactionChance = std::clamp(Config::Int("Direction", "iReactionChance", 50), 0, 100);

			Director::ApplyTunables(read);

			// Read here rather than in Open(), which does not run at all when
			// [Direction] bEnabled=0 â€” the very configuration the probe has to
			// report in. ReadTuning is reached from LoadSettings at startup, so
			// the flag is live either way.
			Scene::Performance::ConfigureProbe(Config::Bool("Diagnostics", "bLogFaceAnim", false));

			// Re-read with the rest, so the forced viseme can be switched on and off
			// against the same face in one session. The hook itself is installed at
			// startup and cannot be; see Runtime.
			Scene::FaceGen::SetForcedViseme(Config::Int("Diagnostics", "iForceViseme", -1));

			// Re-read per conversation so the toggle and the strength can be changed
			// without a restart. The hook itself installs once, at startup, so
			// turning this on mid-session needs one — hence the (restart) note on the
			// ini key.
			Scene::LipSync::Configure(Config::Bool("Performance", "bSynthLipSync", true),
				Config::Int("Performance", "iLipSyncStrength", 55));

			// Restore full expressions independently of the optional mouth/brow layer.
			Scene::Performance::Configure(Config::Bool("Performance", "bExpressions", true), false);

			// The forced-expression discriminator, re-read with the rest so it can
			// be switched on and off against the same face in one session.
			Scene::Performance::SetForcedExpression(
				Config::Int("Diagnostics", "iForceExpression", -1));

			// The shot list, read straight into Shot rather than through Tunables.
			//
			// Forty-odd flags do not belong in a struct the settings menu copies
			// by value every frame it is open, and unlike a dial there is nothing
			// to convert â€” the ini stores exactly what the picker asks for.
			//
			// EVERY FALLBACK COMES FROM THE SHIPPED PRESET, for the setups that
			// preset uses, and from the shot table for the ones it does not.
			//
			// The split matters. A setup Close switches on has to fall back to
			// CLOSE'S numbers or a fresh install reports drift against the very
			// look it is supposed to be running. A setup Close leaves off has no
			// opinion from any preset — drift never looks at a disabled setup's
			// dials — so it falls back to what the table authored, which is what
			// the per-angle Default button puts back and what the player sees the
			// moment they switch it on.
			const auto& shipped = DefaultPreset();

			std::uint32_t off = 0;
			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
				const auto type = static_cast<ShotType>(i);
				const bool shippedOn = PresetUses(shipped, type);

				const bool on = Config::Bool("Shots", Key(type), shippedOn);
				Shot::SetEnabled(type, on);
				Shot::SetWeight(type,
					Config::Int("Shots", WeightKey(type),
						shippedOn ? PresetWeight(shipped, type) : Camera::AuthoredWeight(type)));
				Shot::SetLens(type, Config::Int("Shots", LensKey(type),
									  shippedOn ? PresetLens(shipped, type) :
												  static_cast<int>(Camera::AuthoredLens(type))));

				// The move, and the one migration this needed.
				//
				// Before 1.3 a setup's move belonged to the table and the only
				// thing the player could say about it was iNameZoom: 0 authored, 1
				// zoom in, 2 zoom out. That choice is real tuning and should not be
				// thrown away because the storage changed, so a saved 1 or 2 becomes
				// the matching move and anything else falls through to what the
				// table ships.
				const auto shippedMotion = PresetMotion(shipped, type);

				const int legacyZoom = Config::Int("Shots", ZoomKey(type), 0);
				const auto seeded = legacyZoom == 1 ? Move::kZoomIn :
					legacyZoom == 2                 ? Move::kZoomOut :
					shippedOn                       ? shippedMotion.move :
					                                  Shot::AuthoredMove(type);

				const int stored = Config::Int("Shots", MoveKey(type),
					static_cast<int>(seeded));
				Shot::SetMove(type, stored >= 0 && stored < static_cast<int>(Move::kCount) ?
									   static_cast<Move>(stored) :
									   Shot::AuthoredMove(type));

				Shot::SetMoveAmount(type, Config::Int("Shots", MoveAmountKey(type),
											  shippedOn ? shippedMotion.amount :
														  Shot::AuthoredMoveAmount(type)));
				Shot::SetMoveTime(type, Config::Int("Shots", MoveTimeKey(type),
											shippedOn ? shippedMotion.time :
														Shot::AuthoredMoveTime(type)));

				// The lighting look and this angle's own nudge.
				//
				// READ EVEN WHEN PER-ANGLE LIGHTING IS OFF. It costs four profile
				// reads against the two hundred and thirty already happening here,
				// and it means switching the option on takes effect on the next
				// conversation rather than needing a second one to populate itself.
				//
				// A name that no longer matches anything falls back to what the
				// setup ships under rather than to the global default, and that
				// distinction is the whole reason this is three lines instead of
				// one: a typo in a hand-edited file should cost the player the one
				// angle they mistyped, not silently relight it as whatever the
				// mod's fallback happens to be. The name is logged so it is
				// findable.
				const char* shippedLight =
					shippedOn ? PresetLight(shipped, type) : AuthoredLight(type);
				const auto wanted = Config::String("Shots", LightKey(type), shippedLight);
				int look = Scene::FindLook(wanted);
				if (look < 0) {
					look = Scene::FindLook(AuthoredLight(type));
					Log::Warn(Log::Category::kStaging,
						"{} names lighting look '{}', which does not exist; using '{}'."sv,
						Name(type), wanted, AuthoredLight(type));
				}
				Shot::SetLight(type, look >= 0 ? look : Scene::DefaultLook());

				Shot::SetLightOffset(type,
					Config::Int("Shots", LightXKey(type), 0),
					Config::Int("Shots", LightYKey(type), 0),
					Config::Int("Shots", LightZKey(type), 0));

				off += on ? 0u : 1u;
			}

			if (off > 0) {
				Log::Info(Log::Category::kCamera, "{} of {} shots disabled by settings."sv,
					off, static_cast<std::uint32_t>(ShotType::kCount));
			}

			AuditPools();
		}

		// How long a closed dialogue menu is tolerated before the camera goes back.
		//
		// Not zero: the menu genuinely closes and reopens inside a single
		// conversation â€” observed at 12:54:49 closing and 12:54:50 reopening while
		// the NPC was still the active speaker. Releasing on the first close would
		// drop the camera mid-exchange. Short enough that walking away feels
		// instant, long enough to ride out that blip.
		constexpr float kReleaseGraceSeconds = 0.35f;

		// The speaker handle is briefly unset while a conversation is being set up,
		// so the exit test has to wait for staging to settle before believing it.
		constexpr float kExitCheckDelay = 0.6f;

		constexpr std::uint16_t kCloseUpIntensity = 100;

		// Deterministic, self-contained, and good enough to stop the rotation
		// looking mechanical. Seeded from the cue count so a reload of the same
		// conversation does not reproduce an identical shot list.
		std::uint32_t rngState{ 0x9E3779B9u };

		[[nodiscard]] std::uint32_t NextRandom()
		{
			rngState = rngState * 1664525u + 1013904223u;
			return rngState >> 16;
		}

		[[nodiscard]] float SecondsSince(Clock::time_point a_when)
		{
			return std::chrono::duration<float>(Clock::now() - a_when).count();
		}

		// Is the player looking at a dialogue menu right now?
		//
		// The one question that separates "the player has walked away" from "the
		// topic manager has no live speaker at this instant", which are not the same
		// thing and were being answered by the same test. See the exit check in
		// Tick for the measurement that showed the difference.
		[[nodiscard]] bool DialogueMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		// A head position that does not twitch.
		//
		// The pose was previously derived from the "NPC Head [Head]" bone, which is
		// exactly wrong for a camera: that bone carries breathing, idle sway,
		// weapon shifts and every gesture in the animation. Anchoring to it made
		// the camera inherit all of it, so the frame jittered in sympathy with the
		// subject and no shot ever felt locked off.
		//
		// The actor's root translate does not animate. Eye height is sampled from
		// the head bone once, then reused, which keeps the framing at head level
		// without inheriting the head's motion.
		// Eye height comes from Anatomy now, which finds a head on rigs that do not
		// use the humanoid node name and clamps over a range a dragon and a chicken
		// both fit inside. The old lookup was "NPC Head [Head]" or nothing, and
		// nothing meant the previous value survived — so a creature was framed at
		// whatever height the last person had been.
		[[nodiscard]] std::optional<RE::NiPoint3> StablePoint(RE::Actor* a_actor, const Anatomy& a_body)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return std::nullopt;
			}

			// Root plus the head's captured offset, turned back into world space by
			// the actor's CURRENT heading.
			//
			// This used to be base.x, base.y, base.z + eyeHeight — the head's height
			// over the root's position — which is exact for anything that stands
			// upright and badly wrong for anything that does not. A dragon's root is
			// at its hips and its head is a few hundred units in front, so every
			// shot framed the middle of the animal at head height and the close-ups
			// framed it from about a foot away.
			//
			// The offset is the one captured at measurement, so this still does not
			// inherit the head bone's own animation. Only the heading is live, which
			// is what lets the anchor follow a creature turning to face you.
			const RE::NiPoint3 base = root->world.translate;
			const float        heading = a_actor->GetAngleZ();
			const float        c = std::cos(heading);
			const float        s = std::sin(heading);

			return RE::NiPoint3{
				base.x + a_body.headOffset.x * c - a_body.headOffset.y * s,
				base.y + a_body.headOffset.x * s + a_body.headOffset.y * c,
				base.z + a_body.eyeHeight
			};
		}

		struct Anchor
		{
			RE::NiPoint3 position{};
			bool         primed{ false };
		};

		Anchor playerAnchor{};
		Anchor npcAnchor{};

		// Measured at conversation open and again on a posture change, which are
		// the two moments the answer can change. Not per frame: it walks a
		// skeleton, and the numbers it produces are properties of the creature
		// rather than of the moment.
		Anatomy playerBody{};
		Anatomy npcBody{};

		// Eye height is sampled once and then trusted, which is right for a subject
		// who stays on their feet and wrong for one who does not.
		//
		// Talk to someone asleep and the first sample is taken while they are lying
		// down: head and root sit at nearly the same z, so the measured height falls
		// to the 50-unit clamp floor. The engine then has them get up. The root
		// translate follows them perfectly â€” that part was never broken â€” but the
		// stale 50 keeps the frame pinned around their waist for the rest of the
		// conversation. Sitting subjects who stand have the same problem, halved.
		//
		// Posture is the signal. When it changes the body has changed shape, so the
		// height is re-measured and the blocking re-solved.
		using Posture = RE::SIT_SLEEP_STATE;
		Posture playerPosture{ Posture::kNormal };
		Posture npcPosture{ Posture::kNormal };
		bool    posturePrimed{ false };

		[[nodiscard]] Posture PostureOf(RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			return state ? state->GetSitSleepState() : Posture::kNormal;
		}

		[[nodiscard]] std::string_view PostureName(Posture a_posture)
		{
			switch (a_posture) {
			case Posture::kNormal:              return "standing"sv;
			case Posture::kWantToSit:           return "about to sit"sv;
			case Posture::kWaitingForSitAnim:   return "sitting down"sv;
			case Posture::kIsSitting:           return "seated"sv;
			case Posture::kWantToStand:         return "standing up"sv;
			case Posture::kWantToSleep:         return "about to sleep"sv;
			case Posture::kWaitingForSleepAnim: return "lying down"sv;
			case Posture::kIsSleeping:          return "asleep"sv;
			case Posture::kWantToWake:          return "waking"sv;
			default:                            return "unknown"sv;
			}
		}

		// How quickly the anchor chases the subject. Low on purpose: a camera
		// operator holds their blocking while someone shifts their weight and only
		// re-frames when they actually go somewhere.
		constexpr float kFollowRate = 2.2f;

		void Follow(Anchor& a_anchor, const RE::NiPoint3& a_target, float a_delta)
		{
			if (!a_anchor.primed) {
				a_anchor.position = a_target;
				a_anchor.primed = true;
				return;
			}

			const float k = 1.0f - std::exp(-std::max(a_delta, 0.0f) * kFollowRate);
			a_anchor.position.x += (a_target.x - a_anchor.position.x) * k;
			a_anchor.position.y += (a_target.y - a_anchor.position.y) * k;
			a_anchor.position.z += (a_target.z - a_anchor.position.z) * k;
		}

		[[nodiscard]] RE::NiPoint3 Cross(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}

		[[nodiscard]] RE::NiPoint3 Normalized(const RE::NiPoint3& a_v, bool& a_ok)
		{
			const float length = a_v.Length();
			a_ok = length > 1.0e-3f;
			return a_ok ? RE::NiPoint3{ a_v.x / length, a_v.y / length, a_v.z / length } : RE::NiPoint3{};
		}

		// How the camera transform is written. 0 is the shipped behaviour.
		//
		// THE PLAYER LIPSYNC BUG IS OPEN. Read LIPSYNC.md before touching this.
		//
		// An earlier version of this comment declared the bug SOLVED and blamed a
		// hardcoded `time = 0.0f` on the update traversal below. That was wrong; it
		// came from one uncontrolled observation on a different NPC and did not
		// survive a controlled A/B. Modes 0 and 3 differ ONLY in that field and
		// both fail. The zero was worth removing on its own terms â€” a hardcoded
		// time is meaningless â€” but it fixed nothing.
		//
		// The matrix that comment printed was build A's numbering, from the build
		// deployed at 23:58 on 2026-08-02, in which mode 0 was `time = 0` and mode
		// 3 was `time = delta`. It survives here only as the record of what build A
		// meant, because nothing else does â€” this tree has no version control and
		// build A's DLL was overwritten. In the CURRENT numbering, below, 0 is the
		// real frame time and 3 puts the zero back.
		//
		// What the modes are actually good for now:
		//
		//   mode  local  world  Update  time   flags   camera   lips
		//   0     yes    yes    yes     delta  0x2000  moves    NO
		//   1     yes    yes    no      -      -       DEAD     yes
		//   2     no     yes    yes     delta  0x2000  DEAD     yes
		//   3     yes    yes    yes     0.0    0x2000  moves    NO
		//   4     yes    yes    yes     0.0    0x0000  moves    NO
		//   5     yes    yes    yes     delta  0x2000  moves    NO   (+ThirdPersonState)
		//
		// MODE 2 IS THE INTERESTING ONE AND IT HAS BEEN MISREAD. It is not "the
		// same as bEnabled=0". `staging` is still true in mode 2: the director
		// runs, the letterbox is up, Performance drives gaze and expressions,
		// Presence writes the player's headtrack graph variable, KeyLight aims,
		// Interface fades the topic list, and this function still runs the SAME
		// update traversal over the SAME subtree with the SAME NiUpdateData the
		// same number of times per frame. The single machine-level difference
		// between mode 0 and mode 2 is the value written to `local` â€” and
		// therefore where the camera ends up.
		//
		// That makes mode 2 the control the investigation kept saying it needed,
		// and it has already been run. It says: the write mechanism is innocent.
		// Not the traversal, not the flags, not NiUpdateData::time, not
		// reentrancy from inside ThirdPersonState::Update, not anything SD does to
		// the player besides move the camera. Whatever breaks the player's mouth
		// reads the camera's final transform and acts on it.
		//
		// Two things follow that are worth writing down before the next attempt:
		//
		//   - The flags have never been tested correctly. RE::NiUpdateData::Flag
		//     defines kDirty = 1<<0 and kDisableCollision = 8193 (0x2001), which
		//     INCLUDES kDirty. The 0x2000 below is kDisableCollision with kDirty
		//     stripped, and is not a value the engine names. Mode 4 tested 0x0000.
		//     Nobody has passed 0x2001. Mode 3 vs 4 does show flags changing
		//     nothing, so this is tidiness, not a lead.
		//   - The `world` write is a dead store. Mode 2 proves it: writing world
		//     and running the traversal leaves the camera where the engine put it,
		//     so Update recomputes world from parent->world * local and discards
		//     what was written here. Only `local` and the traversal move anything.
		//
		//   0  normal                                 [shipped]
		//   1  no update traversal at all             [CAMERA DEAD]
		//   2  world written, local left alone        [CAMERA DEAD â€” the control]
		//   3  time = 0
		//   4  time = 0 and no flags
		//   5  normal, plus syncing ThirdPersonState
		int poseMode{ 0 };

		// Diagnostic: hold a frontal close on the PLAYER for the opening of a
		// conversation, in milliseconds. 0 is off.
		//
		// Measured 2026-08-03, and this is the only lever SD has on it. Whether the
		// player's facegen morph gets applied is decided ONCE, around the moment a
		// conversation opens, from where the camera is relative to their head â€” and
		// then held for the whole exchange. Two conversations with the same NPC,
		// differing only in where the camera sat beforehand:
		//
		//   run-up headFwd -0.72 (face away)   -> phoneme mean 0.145, max 0.550
		//   run-up headFwd +0.34 (face toward) -> phoneme mean 0.068, max 0.300
		//
		// Orientation DURING the conversation does nothing; the two runs averaged
		// +0.09 and +0.12 and behaved completely differently. So the question is
		// whether SD, which takes the camera a few milliseconds after the engine
		// opens the conversation, is early enough to be what that decision reads.
		// If it is, opening on the player's face and cutting away should flip it.
		// If the engine has already decided by then, this will do nothing, and
		// that is worth knowing just as clearly.
		//
		// Declared with the other dials near the top; only the reasoning lives here.
		[[nodiscard]] bool HoldingOpenShot()
		{
			return openShotHold > 0 && Clock::now() < openShotUntil;
		}

		[[nodiscard]] std::string_view PoseModeName(int a_mode) noexcept
		{
			switch (a_mode) {
			case 0:  return "normal"sv;
			case 1:  return "no traversal"sv;
			case 2:  return "world only"sv;
			case 3:  return "time = 0"sv;
			case 4:  return "time = 0, no flags"sv;
			case 5:  return "normal + ThirdPersonState synced"sv;
			default: return "unknown"sv;
			}
		}

		// Rotation matrix to quaternion, Shepperd's method.
		//
		// Needed only for mode 5, because ThirdPersonState stores its rotation as
		// a quaternion while the camera node stores a matrix, and the two have to
		// be made to agree.
		[[nodiscard]] RE::NiQuaternion QuaternionFrom(const RE::NiMatrix3& a_m)
		{
			RE::NiQuaternion q{};
			const float      trace = a_m.entry[0][0] + a_m.entry[1][1] + a_m.entry[2][2];

			if (trace > 0.0f) {
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				q.w = 0.25f * s;
				q.x = (a_m.entry[2][1] - a_m.entry[1][2]) / s;
				q.y = (a_m.entry[0][2] - a_m.entry[2][0]) / s;
				q.z = (a_m.entry[1][0] - a_m.entry[0][1]) / s;
			} else if (a_m.entry[0][0] > a_m.entry[1][1] && a_m.entry[0][0] > a_m.entry[2][2]) {
				const float s = std::sqrt(1.0f + a_m.entry[0][0] - a_m.entry[1][1] - a_m.entry[2][2]) * 2.0f;
				q.w = (a_m.entry[2][1] - a_m.entry[1][2]) / s;
				q.x = 0.25f * s;
				q.y = (a_m.entry[0][1] + a_m.entry[1][0]) / s;
				q.z = (a_m.entry[0][2] + a_m.entry[2][0]) / s;
			} else if (a_m.entry[1][1] > a_m.entry[2][2]) {
				const float s = std::sqrt(1.0f + a_m.entry[1][1] - a_m.entry[0][0] - a_m.entry[2][2]) * 2.0f;
				q.w = (a_m.entry[0][2] - a_m.entry[2][0]) / s;
				q.x = (a_m.entry[0][1] + a_m.entry[1][0]) / s;
				q.y = 0.25f * s;
				q.z = (a_m.entry[1][2] + a_m.entry[2][1]) / s;
			} else {
				const float s = std::sqrt(1.0f + a_m.entry[2][2] - a_m.entry[0][0] - a_m.entry[1][1]) * 2.0f;
				q.w = (a_m.entry[1][0] - a_m.entry[0][1]) / s;
				q.x = (a_m.entry[0][2] + a_m.entry[2][0]) / s;
				q.y = (a_m.entry[1][2] + a_m.entry[2][1]) / s;
				q.z = 0.25f * s;
			}
			return q;
		}

		[[nodiscard]] RE::ThirdPersonState* ThirdPersonStateNow()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return nullptr;
			}

			// The state OBJECT, not the active one. Asking for it by slot rather
			// than through currentState is what lets the restore below run from
			// Close() on a frame the camera has already left third person — on a
			// horse, in a killcam, back in first person. Writing fields on a state
			// that is not running is harmless and is exactly what should happen:
			// they are what it will come back to.
			auto* slot = camera->GetRuntimeData().cameraStates[RE::CameraState::kThirdPerson].get();
			return slot ? static_cast<RE::ThirdPersonState*>(slot) : nullptr;
		}

		// Take the reading while it is still the player's own.
		//
		// THE ONE PLACE THAT DECIDES WHETHER NOW IS A GOOD MOMENT, for all three of
		// the lens, the aim and the zoom. The field of view used to be sampled in
		// Tick under `!staging` alone, on the reasoning that nothing but SD moves
		// it — see the block further down for why that was wrong and what it cost.
		//
		// `!staging` is not early enough on its own: the engine starts swinging the
		// camera at the speaker the moment the dialogue menu opens, and that is
		// roughly 120ms BEFORE Open() runs, so an unstaged frame can already be
		// showing the dialogue camera. Sample there and the mod faithfully restores
		// a pose the player never chose.
		//
		// Two gates rather than one: the menu being up, and the session being
		// active. Either can lead the other depending on how the conversation
		// started, and a reading taken one frame late is still the resting value
		// because the engine EASES into its dialogue aim over a good fraction of a
		// second rather than snapping to it.
		void SampleCameraRest()
		{
			if (dialogueMenuUp.load(std::memory_order_relaxed) ||
				Dialogue::Session::GetSingleton().Active()) {
				return;
			}

			// A THIRD GATE, AND IT IS THE ONE THE SMOOTHCAM REPORT NEEDED.
			//
			// This reading is taken every unstaged frame and stamped back into the
			// third-person camera at the end of the next conversation — including
			// savedZoomOffset, which is the game's PERSISTENT third-person zoom.
			// So a bad reading does not stay a bad reading: it is written into a
			// field the camera keeps, and whatever is managing that camera carries
			// it forward from there.
			//
			// The frames after a screen-owning menu closes are exactly where a bad
			// reading comes from. The tick does not run while the game is paused,
			// so the first frame back is the first sample in however long the
			// player spent in a shop — and it lands while the camera is still
			// settling out of the menu rather than at rest. Take it there and the
			// value that gets stamped back later is a transient nobody ever saw.
			//
			// Two gates, for the two shapes the problem has. A menu that owns the
			// screen right now is never sampled at all; and for half a second after
			// the last one let go, neither is anything else.
			if (Dialogue::MenuWatch::ScreenTaken()) {
				return;
			}
			if (SecondsSince(screenReleasedAt) < kRestSettleSeconds) {
				return;
			}

			// THE LENS, WHICH USED TO BE SAMPLED A FEW LINES EARLIER AND OUTSIDE
			// EVERY GATE ABOVE.
			//
			// It sat in Tick with one test on it — a floor of kSaneMinFov — on the
			// reasoning that nothing but SD ever moves the field of view, so any
			// unstaged frame would do. That reasoning is wrong twice. ENB presets,
			// SmoothCam's own offsets and Improved Camera's first-person lens all
			// write worldFOV, and the frames just after a menu closes are the worst
			// of them: the tick does not run while the game is paused, so the first
			// frame back is the first sample in however long the player spent in a
			// shop, taken while somebody else is still settling.
			//
			// That is the same fault the aim below is gated against, on the field
			// that gate did not originally cover — and it is worse here than a bad
			// frame, because restingFov is not only stamped back at the end of the next
			// conversation, it is also baseFov: the lens every shot without an
			// opinion of its own composes against. One transient read at a shop
			// door and the next conversation is framed on it.
			//
			// The floor stays. It answers a different question — whether the value
			// is SD's own narrowed lens leaking back in after a Close that never
			// ran — and neither test makes the other redundant.
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->GetRuntimeData2().worldFOV >= kSaneMinFov) {
				restingFov = cam->GetRuntimeData2().worldFOV;
			}

			auto* state = ThirdPersonStateNow();
			if (!state) {
				return;
			}

			cameraRest.freeRotation = state->freeRotation;
			cameraRest.posOffsetExpected = state->posOffsetExpected;
			cameraRest.posOffsetActual = state->posOffsetActual;
			cameraRest.targetZoomOffset = state->targetZoomOffset;
			cameraRest.currentZoomOffset = state->currentZoomOffset;
			cameraRest.savedZoomOffset = state->savedZoomOffset;
			cameraRest.pitchZoomOffset = state->pitchZoomOffset;
			cameraRest.targetYaw = state->targetYaw;
			cameraRest.currentYaw = state->currentYaw;
			cameraRest.freeRotationEnabled = state->freeRotationEnabled;
			cameraRestPrimed = true;
		}

		// A HAND-BACK THAT IS OWED BUT HAS NOT BEEN PAID.
		//
		// Deliberately NOT part of the suspension. A suspension answers "may this
		// conversation resume", and it is spent the moment that question is
		// settled; this answers "does the camera still need putting back", which
		// outlives it whenever the camera was refused at the moment of asking.
		// Conflating the two is what let a transient refusal leave the player on a
		// cinematic lens permanently.
		bool              handBackPending{ false };
		Clock::time_point handBackSince{};

		// How often the queued hand-back is re-attempted.
		//
		// Not every frame: SmoothCam::Acquire logs on refusal, and a consumer that
		// holds the camera for a while would otherwise produce a warning per frame
		// for as long as it did. A second is far below the time it takes anyone to
		// notice a lens, and far above the cost of asking.
		constexpr float kHandBackRetrySeconds = 1.0f;

		// The lens, put back the way the player had it.
		//
		// Split out of Close so AbandonSuspension can pay it as well. A suspension
		// skips both this and RestoreCameraRest — the conversation is coming
		// straight back — and AbandonSuspension is the branch where it does not,
		// so it owes both. See the call sites, which carry the argument.
		void RestoreFieldOfView()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || restingFov <= 1.0f) {
				return;
			}

			if (std::abs(cam->GetRuntimeData2().worldFOV - restingFov) > 0.01f) {
				Log::Info(Log::Category::kCamera,
					"Restoring field of view {:.1f} -> {:.1f}."sv, cam->GetRuntimeData2().worldFOV, restingFov);
			}
			cam->GetRuntimeData2().worldFOV = restingFov;
		}

		// Hand the view back pointing where it was pointing.
		//
		// Every field written here was written by the ENGINE during the
		// conversation, not by this mod — SD drives the camera node and touches
		// none of this. That is the point: the engine's dialogue aim is a
		// transition the player is supposed to watch, and staging replaced watching
		// it with cutting to the middle of it.
		//
		// applyOffsets and toggleAnimCam are left alone on purpose. They gate
		// whether the offsets and an animation-driven camera apply at all, and are
		// owned by systems that have nothing to do with a conversation ending —
		// restoring a stale bool there could suppress a camera somebody else is
		// legitimately running.
		void RestoreCameraRest()
		{
			if (!cameraRestPrimed) {
				return;
			}

			auto* state = ThirdPersonStateNow();
			if (!state) {
				return;
			}

			const RE::NiPoint2 was = state->freeRotation;
			const float        zoomWas = state->currentZoomOffset;
			const bool         freeWas = state->freeRotationEnabled;

			state->freeRotation = cameraRest.freeRotation;

			// ONE DIRECTION ONLY, and this is the one field here that gets a rule
			// of its own. Free rotation off means the camera follows the player,
			// which is the resting behaviour and always safe to return to. Turning
			// it back ON because it happened to be on when the reading was taken —
			// the player was holding free-look as they walked up — would hand back a
			// camera detached from a key nobody is pressing any more.
			if (!cameraRest.freeRotationEnabled) {
				state->freeRotationEnabled = false;
			}

			state->posOffsetExpected = cameraRest.posOffsetExpected;
			state->posOffsetActual = cameraRest.posOffsetActual;

			// THE ZOOM, WHICH IS NOT OURS TO PUT BACK WHEN IMPROVED CAMERA IS HERE.
			//
			// These four fields are the aim's poor relations for everybody else and
			// the whole mechanism for Improved Camera: they are how it moves between
			// first and third person, written every frame of a transition it owns.
			// Stamping a reading taken before the conversation into the middle of
			// that is a second author on one animation, and it reads as the view
			// pumping in and out — which is exactly how it was reported.
			//
			// SmoothCam gets to keep the restore because SmoothCam gets an actual
			// handshake: by the time this runs it has been told the camera is coming
			// back and it has not started interpolating yet. Improved Camera
			// publishes no such interface, so the only way not to fight it is not to
			// write. See Compat::ImprovedCamera.
			//
			// The aim is still handed back, and that is deliberate rather than an
			// oversight of scope. freeRotation is the engine's dialogue camera
			// pitched down at whoever was being spoken to — the "looking at the
			// floor" report — and Improved Camera does not drive it. Losing that fix
			// to be safe about a field nobody complained about would be trading one
			// bug for another.
			const bool restoreZoom = !Compat::ImprovedCamera::Present();
			if (restoreZoom) {
				state->targetZoomOffset = cameraRest.targetZoomOffset;
				state->currentZoomOffset = cameraRest.currentZoomOffset;
				state->savedZoomOffset = cameraRest.savedZoomOffset;
				state->pitchZoomOffset = cameraRest.pitchZoomOffset;
			}

			state->targetYaw = cameraRest.targetYaw;
			state->currentYaw = cameraRest.currentYaw;

			// ONE LINE, EVERY CONVERSATION, WHETHER OR NOT ANYTHING MOVED.
			//
			// This started as a change-gated line and that was the wrong instinct.
			// The fix above is a hypothesis — that what the player sees at the end
			// of a conversation is the engine's dialogue aim, held in freeRotation —
			// and a line that prints only when the hypothesis is right cannot tell
			// anybody it is wrong. A conversation where the pitch did NOT move is
			// the interesting one: it says the snap is coming from somewhere else,
			// and the two other places it could come from are on the same line.
			//
			// The player's own pitch is READ and not written. If that is the number
			// that is wrong, this mod is not the thing that should be fixing it, but
			// it is the thing that should say so.
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const float playerPitch = player ? player->data.angle.x : 0.0f;

			Log::Info(Log::Category::kCamera,
				"Handing the view back | free rot {} | pitch {:+.3f} -> {:+.3f} | yaw {:+.3f} -> {:+.3f} "
				"| zoom {:.2f} -> {:.2f}{} | player pitch {:+.3f} (radians, not touched)."sv,
				freeWas ? (cameraRest.freeRotationEnabled ? "on"sv : "on -> off"sv) : "off"sv,
				was.y, cameraRest.freeRotation.y,
				was.x, cameraRest.freeRotation.x,
				zoomWas, restoreZoom ? cameraRest.currentZoomOffset : zoomWas,
				restoreZoom ? ""sv : " (not written; Improved Camera owns it)"sv,
				playerPitch);
		}

		// Take the camera, put the view back, give it back. Or do none of it.
		//
		// The whole of the ownership contract in one place, so the two callers —
		// AbandonSuspension and the queued retry in Tick — cannot drift apart on
		// it. Returns false when the camera was refused, having written nothing:
		// that is the case the caller has to keep owing rather than forget.
		[[nodiscard]] bool TryHandBackView()
		{
			if (!Compat::SmoothCam::Acquire()) {
				return false;
			}

			RestoreFieldOfView();
			RestoreCameraRest();
			if (restoreThirdPersonPending) {
				if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInFirstPerson()) {
					camera->ForceThirdPerson();
				}
				restoreThirdPersonPending = false;
			}
			Compat::SmoothCam::Release();
			return true;
		}

		void ApplyPose(RE::NiNode* a_root, const Pose& a_pose, float a_delta,
			RE::ThirdPersonState* a_state)
		{
			bool       ok = false;
			const auto forward = Normalized(
				{ a_pose.lookAt.x - a_pose.position.x,
					a_pose.lookAt.y - a_pose.position.y,
					a_pose.lookAt.z - a_pose.position.z },
				ok);
			if (!ok) {
				return;
			}

			// Match the visibility projection for a directly overhead smart shot.
			const RE::NiPoint3 worldUp = protectSubject && std::abs(forward.z) > 0.999999f ?
				RE::NiPoint3{ 0.0f, 1.0f, 0.0f } : RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(forward, worldUp), ok);
			if (!ok) {
				return;
			}
			const auto up = Cross(right, forward);

			// Columns are right / forward / up. Measured, not assumed: across six
			// staged conversations the engine's own matrix gave col1 . view = +0.997.
			RE::NiMatrix3 rotation{};
			rotation.entry[0][0] = right.x;  rotation.entry[0][1] = forward.x;  rotation.entry[0][2] = up.x;
			rotation.entry[1][0] = right.y;  rotation.entry[1][1] = forward.y;  rotation.entry[1][2] = up.y;
			rotation.entry[2][0] = right.z;  rotation.entry[2][1] = forward.z;  rotation.entry[2][2] = up.z;

			a_root->world.translate = a_pose.position;
			a_root->world.rotate = rotation;

			// Mode 2 leaves local alone. world is what the renderer reads; local
			// is what a parent's update would recompute world FROM, so writing
			// both is belt and braces on a node with no meaningful parent.
			if (poseMode != 2) {
				a_root->local.translate = a_pose.position;
				a_root->local.rotate = rotation;
			}

			if (poseMode == 1) {
				return;  // no update traversal at all
			}

			RE::NiUpdateData update{};
			update.time = (poseMode == 3 || poseMode == 4) ? 0.0f : a_delta;
			update.flags = static_cast<RE::NiUpdateData::Flag>(poseMode == 4 ? 0x0000 : 0x2000);
			a_root->Update(update);

			// Mode 5: tell the engine where its own camera actually is.
			//
			// This is the difference between SD and every offset-based camera, and
			// the current best candidate for the lipsync bug. IACC adjusts the
			// engine's camera parameters and lets the engine compute the final
			// transform, so the engine's own record stays true. SD computes the
			// transform itself and stamps it onto the node afterwards â€” leaving
			// ThirdPersonState::translation and ::rotation describing the vanilla
			// position, somewhere behind the player's head.
			//
			// Anything the engine decides from its OWN record rather than by
			// reading the node therefore decides it against a camera that is not
			// where the camera is. "Is the player's face worth animating" is
			// exactly the sort of question that would be answered that way, and it
			// would answer no, every frame, while the real camera sits in a
			// close-up of that face.
			//
			// Note this is additive: the node write above still happens. The point
			// is to stop the two records disagreeing, not to move the camera by a
			// different route.
			if (poseMode == 5 && a_state) {
				a_state->translation = a_pose.position;
				a_state->rotation = QuaternionFrom(rotation);
			}
		}

		// Shots appropriate to each half of the exchange.
		//
		// The speaking pool favours the listener's point of view, because that is
		// where the player is standing. The waiting pool is deliberately wider and
		// more varied: it covers the silence while the player reads the topic list,
		// which is the longest and least eventful part of any conversation and the
		// part that most needs the camera to be doing something.
		// Weighted heavily toward the shoulder shots.
		//
		// The earlier pools gave side-by-side framings â€” two-shot, profile, wide â€”
		// roughly even odds with the shoulder coverage, so most of a conversation
		// played out with both parties in frame and neither of them the subject.
		// That is establishing coverage, not the scene. It belongs at the top and
		// occasionally as a breath, not as the default.
		// Accents only. Coverage is chosen directly by Canonical(), not drawn from
		// a pool, so a weighting can no longer be defeated by the picker.
		// Full coverage of the NPC at deliberately different distances, so a new
		// line means a new size as well as a new angle: shoulder, tight, clean
		// medium, low, full figure, across the room.
		//
		// This is drawn from on *every* line rather than occasionally. The previous
		// version returned canonical coverage four times in five, and since a cut
		// only happens when the chosen shot differs from what is on screen, the
		// camera simply held the same angle line after line.
		// Weighted toward close coverage: a conversation scene lives on the face of
		// whoever is speaking, and the wide options are a change of pace, not the
		// staple.
		// EVERY SETUP APPEARS EXACTLY ONCE. The baseline that used to be expressed
		// by listing the staples twice now lives in AuthoredWeight, so the number on
		// the slider is the number that competes. See the note there.
		//
		// kExtremeClose is in this list because it was in no pool at all, which made
		// its weight slider the one control in the mod that could do nothing:
		// Shot::Weight() is consulted only by the walk over these arrays. It reached
		// the screen through the intensity override and the fallback ladder, both
		// weight-blind, so a session with it weighted 69 — the highest in this pool
		// — drew it zero times in 22 cuts while a shot weighted 34 was drawn four.
		// The override still earns it on loud lines; weight 0 restores exactly the
		// old behaviour of intensity-or-nothing.
		constexpr std::array kNpcCoverage{
			ShotType::kCloseUp,
			ShotType::kCloseLow,
			ShotType::kCloseHigh,
			ShotType::kCloseProfile,
			ShotType::kMediumNpc,
			ShotType::kOverPlayerShoulder,
			ShotType::kLowAngle,
			// The dirty single and the three-quarter are the two most common angles
			// in filmed dialogue and the set had neither. They ship as staples in
			// AuthoredWeight rather than as accents.
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kMediumProfile,

			// The OTS height variants. An over-the-shoulder is the staple of screen
			// dialogue and the pool carried exactly one of them, so "the mod's most
			// characteristic angle" and "a single repeated setup" were the same
			// thing. They are accents against the shoulder shot itself, which is
			// where the staple weighting sits.
			ShotType::kOverPlayerShoulderLow,
			ShotType::kOverPlayerShoulderHigh,

			ShotType::kExtremeClose,
		};

		// Only offered once a couple of lines have gone by â€” a change of pace needs
		// something to change from.
		//
		// A STRICT SUPERSET of kNpcCoverage: everything the early pool can draw,
		// plus the wider vocabulary below. That is the invariant, and it is worth
		// stating because the alternative is a list with unexplained holes in it —
		// a setup missing from here has a weight that means one thing on the first
		// line of a turn and nothing from the second on, which is not a behaviour
		// anybody would choose deliberately and is very easy to arrive at by
		// forgetting. Two setups were in that state and neither was on purpose.
		// AuditPools reports the early-only set, so a new hole announces itself.
		constexpr std::array kNpcLateCoverage{
			ShotType::kCloseUp,
			ShotType::kCloseLow,
			ShotType::kCloseProfile,
			ShotType::kCloseHigh,
			ShotType::kCloseWide,
			ShotType::kMediumNpc,
			ShotType::kOverPlayerShoulder,
			ShotType::kLowAngle,
			ShotType::kLongNpc,
			ShotType::kDistant,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kMediumProfile,
			ShotType::kLowProfile,
			ShotType::kOverhead,
			ShotType::kOverPlayerShoulderLow,
			ShotType::kOverPlayerShoulderHigh,
			ShotType::kOverPlayerShoulderWide,

			// In BOTH npc pools, or the dial it was given only works on short turns.
			//
			// Adding it to kNpcCoverage alone fixed the reported fault — the setup
			// had been in no pool at all — but left it drawable on the first line of
			// a turn and undrawable from the second on, which is a weight that means
			// one thing early and nothing late. Measured on the session that proved
			// the fix: six cuts to it, four of them on ordinary lines, and every one
			// necessarily from the early pool.
			//
			// The player's side never had this problem. kExtremeClosePlayer sits in
			// kPlayerCoverage, which is the only pool the player's turn draws from,
			// so it has always been reachable for the whole of that turn. This is the
			// NPC side matching it.
			//
			// A change of pace is exactly what a late turn is for, and the tightest
			// setup in the mod is a change of pace. The intensity override still
			// earns it on loud lines either way; weight 0 still means never.
			ShotType::kExtremeClose,
		};

		// Every entry here frames the player.
		//
		// This pool had three entries against the NPC's ten, and that asymmetry is
		// almost certainly the "not enough angles" report: the NPC side already
		// rotated through close, medium, low and shoulder variants while every
		// reverse shot came back to one of the same three setups. Since a cut only
		// happens when the pick differs from what is on screen, a pool of three
		// also means one pick in three is silently discarded.
		// Each setup once; the staples carry their weighting in AuthoredWeight.
		constexpr std::array kPlayerCoverage{
			ShotType::kOverNpcShoulder,
			ShotType::kMediumPlayer,
			ShotType::kClosePlayer,

			// An accent rather than a staple. The NPC's extreme is rationed by
			// intensity, which the player's side has no equivalent of — the authored
			// emotion belongs to the line being spoken, and during the player's turn
			// that is nobody's. Shipping it at the ordinary tier against the staples'
			// doubled one is the rarity that used to come from pool frequency, and
			// the weight dial is there for anyone who wants more or none.
			ShotType::kExtremeClosePlayer,

			ShotType::kDirtyPlayer,
			ShotType::kThreeQuarterPlayer,
			ShotType::kPlayerProfile,
			ShotType::kPlayerLow,
			ShotType::kHighAngle,

			// The reverse now has the same vocabulary as the shot it answers:
			// matched OTS heights, a full figure and an overhead. Without these
			// the player side could only reply to a wide angle on the NPC with a
			// tighter one, and a reverse that does not match reads as a different
			// scene rather than the other half of this one.
			ShotType::kOverNpcShoulderLow,
			ShotType::kOverNpcShoulderHigh,
			ShotType::kOverNpcShoulderWide,
			ShotType::kLongPlayer,
			ShotType::kPlayerOverhead,
		};

		// The room rather than either party. Alternated with player coverage while
		// a topic list is up, so a long pause to read becomes a slow look around
		// the space instead of a held shot of somebody standing still.
		constexpr std::array kEnvironmental{
			ShotType::kWide,
			ShotType::kDistant,
			ShotType::kProfile,
			ShotType::kTwoShot,

			// Sizes and heights the six above never reached. The set was three
			// two-shots at three heights plus a wide, a distant and a profile â€”
			// varied in elevation and almost fixed in scale, so "the room" always
			// came back at about the same size.
			ShotType::kMaster,
			ShotType::kGroundLevel,
			ShotType::kDistantLow,
		};

		// Name any setup no pool can draw, once per run.
		//
		// Shot::Weight() is consulted in exactly one place — the weighted walk in
		// Coverage() — so a setup absent from every pool has a weight slider that
		// cannot do anything, and nothing anywhere says so. kExtremeClose was in
		// that state on its own: 38 of 39 setups reachable, one not, and the one
		// not happened to be the tightest shot in the mod. It cost a session of
		// "the weight system isn't working", which was exactly true of that shot
		// and exactly false of the other 38.
		//
		// A list, not a bool. The next setup added to the enum will default to
		// being in no pool, and this is the line that will say so before anybody
		// has to measure a shot distribution to find out.
		//
		// The ladder and the intensity override can still reach these, which is why
		// this is worded as the weight being inert rather than the shot being
		// unreachable — the two are different and the difference is the bug.
		void AuditPools()
		{
			static Log::OnceFlag audited;
			if (!audited.Take()) {
				return;
			}

			std::array<bool, static_cast<std::size_t>(ShotType::kCount)> pooled{};
			const auto mark = [&](std::span<const ShotType> a_pool) {
				for (const auto type : a_pool) {
					const auto index = static_cast<std::size_t>(type);
					if (index < pooled.size()) {
						pooled[index] = true;
					}
				}
			};

			mark(kNpcCoverage);
			mark(kNpcLateCoverage);
			mark(kPlayerCoverage);
			mark(kEnvironmental);

			std::string orphans;
			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
				if (pooled[i]) {
					continue;
				}
				if (!orphans.empty()) {
					orphans += ", ";
				}
				orphans += Name(static_cast<ShotType>(i));
			}

			if (orphans.empty()) {
				Log::Info(Log::Category::kCamera,
					"Shot pools cover all {} setups; every weight is live."sv,
					static_cast<std::uint32_t>(ShotType::kCount));
			} else {
				Log::Warn(Log::Category::kCamera,
					"In no shot pool, so their weight sliders do nothing: {}. "
					"They can still arrive via the intensity override or the fallback ladder."sv,
					orphans);
			}

			// The late NPC pool must contain everything the early one does.
			//
			// A setup in the early pool and not the late one is drawable on the
			// first line of a turn and undrawable from the second on, so its weight
			// silently changes meaning partway through every speech. That is never
			// an intention; it is what forgetting to add a line to the second array
			// looks like, and it happened twice — to the tightest setup in the mod
			// and to the medium profile. Neither was visible without counting cuts.
			std::string earlyOnly;
			for (const auto type : kNpcCoverage) {
				if (std::find(kNpcLateCoverage.begin(), kNpcLateCoverage.end(), type) !=
					kNpcLateCoverage.end()) {
					continue;
				}
				if (!earlyOnly.empty()) {
					earlyOnly += ", ";
				}
				earlyOnly += Name(type);
			}

			if (!earlyOnly.empty()) {
				Log::Warn(Log::Category::kCamera,
					"In the early NPC pool but not the late one, so they stop being drawable "
					"after the first line of a turn: {}."sv,
					earlyOnly);
			}
		}

		// Shots that need somewhere to stand.
		//
		// Kept as one predicate rather than an inline list because the list was
		// already wrong before anything was added to it: it named kWide, kDistant,
		// kLongNpc and the anchored pair, but not kProfile or kOverhead, which need
		// as much room. Every wide setup added since would have had to be
		// remembered here, and the failure is silent â€” a camera in a wall, in a
		// corridor, with nothing in the log to say which shot did it.
		[[nodiscard]] bool NeedsRoom(ShotType a_type)
		{
			switch (a_type) {
			case ShotType::kWide:
			case ShotType::kDistant:
			case ShotType::kDistantLow:
			case ShotType::kLongNpc:
			case ShotType::kLongPlayer:
			case ShotType::kMaster:
			case ShotType::kGroundLevel:
			case ShotType::kOverPlayerShoulderWide:
			case ShotType::kOverNpcShoulderWide:
			case ShotType::kOverhead:
			case ShotType::kPlayerOverhead:
				return true;
			default:
				return false;
			}
		}

		// A cue that landed before there was a conversation to give it to.
		//
		// The derived facts are kept rather than the DialogueResponse pointer.
		// The response belongs to the topic info and outliving this handoff is not
		// something it promises, and there is nothing here that needs it â€” word
		// count, intensity and emotion are the whole of what a cue contributes.
		struct PendingCue
		{
			RE::ActorHandle   speaker{};
			std::uint32_t     words{ 0 };
			std::uint16_t     intensity{ 50 };
			std::uint32_t     emotion{ 0 };
			std::string       text;  // response storage can expire before greeting replay
			Clock::time_point at{};
			bool              valid{ false };
		};

		PendingCue pendingCue{};

		// How stale a stashed cue may be before Open() refuses it.
		//
		// The measured gap is about 120ms. This is generous against that because
		// the cost of being wrong is asymmetric: too short drops the greeting
		// again, too long is caught by the speaker check below it. LineWatch hooks
		// every Character, so idle chatter from a passing guard lands here too and
		// the identity test is what actually rejects it.
		constexpr float kGreetingGrace = 1.5f;

		// Applies a line to the direction state. Shared by the live path and by
		// the greeting Open() replays.
		void ApplyCue(RE::Actor* a_speaker, std::uint32_t a_words, std::uint16_t a_intensity,
			std::uint32_t a_emotion, std::string_view a_text);

		// Enabled AND weighted above zero.
		//
		// WEIGHT 0 HAS TO MEAN NEVER, or it means nothing. It used to be honoured
		// by the pool walk alone, and the pool walk is not where most of a
		// conversation comes from once the enabled set is thin. A setup dialled to
		// zero still arrived through the canonical fallback, the intensity
		// override, the establishing shot, the opening shot and the no-shot-fits
		// ladder — five routes, none of which had ever heard of the weight.
		//
		// One predicate for every one of them, so "how often is this drawn" and
		// "may this be drawn at all" cannot drift apart again.
		// AN OVER-THE-SHOULDER NEEDS A SHOULDER, AND NOT THE SUBJECT'S.
		//
		// These setups stand behind the other party and put them in the corner of
		// frame, so the body that has to be humanoid is the one in the FOREGROUND —
		// which is whoever the shot is not about. kOverPlayerShoulder frames the
		// NPC over the player's shoulder and is fine whatever the NPC is;
		// kOverNpcShoulder frames the player over the NPC's, and if the NPC is a
		// dragon there is no shoulder there to stand behind. The old code drew both
		// regardless and the second put the camera inside a wing.
		//
		// Asked of the skeleton via Anatomy rather than of the size, because a
		// giant has arms and is still the wrong thing to shoot over.
		[[nodiscard]] bool Composable(ShotType a_type)
		{
			if (!OverShoulder(a_type)) {
				return true;
			}
			return FavoursNpc(a_type) ? playerBody.shoulder : npcBody.shoulder;
		}

		[[nodiscard]] bool Drawable(ShotType a_type)
		{
			return Shot::Enabled(a_type) && Shot::Weight(a_type) > 0 && Composable(a_type);
		}

		// Draw one setup from a pool in proportion to its weight.
		//
		// Templated on the predicate rather than taking a std::function because it
		// runs inside the picker; the eligibility test differs per caller (the
		// coverage pools reject the current shot and the room-hungry setups, the
		// opener does not) and only the weighting is common.
		//
		// Returns nothing when no entry qualifies, which the caller must answer for
		// itself — silently substituting a shot here is what a fallback is for, and
		// two of them disagreeing is what put the wrong angle on screen before.
		template <typename Predicate>
		[[nodiscard]] std::optional<ShotType> WeightedPick(
			std::span<const ShotType> a_pool, Predicate a_eligible)
		{
			std::uint32_t total = 0;
			for (const auto type : a_pool) {
				if (a_eligible(type)) {
					total += static_cast<std::uint32_t>(Shot::Weight(type));
				}
			}

			if (total == 0) {
				return std::nullopt;
			}

			std::uint32_t roll = NextRandom() % total;
			for (const auto type : a_pool) {
				if (!a_eligible(type)) {
					continue;
				}
				const auto weight = static_cast<std::uint32_t>(Shot::Weight(type));
				if (roll < weight) {
					return type;
				}
				roll -= weight;
			}

			return std::nullopt;
		}

		// The total weight a pool can currently field, for deciding BETWEEN pools.
		[[nodiscard]] std::uint32_t PoolWeight(std::span<const ShotType> a_pool)
		{
			std::uint32_t total = 0;
			for (const auto type : a_pool) {
				if (Drawable(type) && !(NeedsRoom(type) && !roomy)) {
					total += static_cast<std::uint32_t>(Shot::Weight(type));
				}
			}
			return total;
		}

		constexpr auto kAllShots = [] {
			std::array<ShotType, static_cast<std::size_t>(ShotType::kCount)> types{};
			for (std::size_t i = 0; i < types.size(); ++i) {
				types[i] = static_cast<ShotType>(i);
			}
			return types;
		}();

		// Prefer the requested subject, then neutral coverage, then the other
		// subject if that is all the user enabled. An empty set has no default.
		[[nodiscard]] std::optional<ShotType> Canonical()
		{
			const bool wantNpc = SubjectIsNpc();
			const auto preferred = wantNpc ? ShotType::kOverPlayerShoulder : ShotType::kOverNpcShoulder;
			return BestAvailable(std::span{ kAllShots }, Drawable, [&](ShotType type) {
				if (FramingIsRoom() && type == ShotType::kTwoShot) {
					return 5.0f;
				}
				if (type == preferred) {
					return 4.0f;
				}
				const float preference = IsNeutral(type) ? 1.0f :
					(FavoursNpc(type) == wantNpc ? 2.0f : 0.0f);
				return preference + FillOf(type);
			});
		}

		// Used sparingly, as a change of pace within one side of the exchange.
		// A different shot of the correct subject. Rejects the shot already on
		// screen so a new line always brings a new angle and a new size.
		[[nodiscard]] std::optional<ShotType> Coverage()
		{
			// Two lines into a speech, not three.
			//
			// This is what reaches the wider half of the NPC vocabulary — the full
			// figure, the anchored angles, the long shot across the room. Measured
			// against real conversations a turn is usually one or two lines, so a
			// threshold of three almost never fired and that half of the table was
			// very nearly dead code.
			// THE PLAYER TOOK THE DECISION. HONOUR IT EXACTLY.
			//
			// A framing override that holds "mostly" is worse than none at all: the
			// camera wanders off the thing they pressed a key to look at, and there
			// is no way to tell that from the mod ignoring the key. So a forced
			// framing suppresses the environmental roll below as well as the side
			// choice — asking for THEM and getting a master shot every third line
			// is the same failure in a different costume.
			if (FramingIsRoom()) {
				const auto room = WeightedPick(kEnvironmental, [&](ShotType a_type) {
					return a_type != currentShot && Drawable(a_type) &&
						!(NeedsRoom(a_type) && !roomy);
				});
				return room ? room : Canonical();
			}

			const bool wantNpc = SubjectIsNpc();
			const bool forced = framing != Framing::kAuto;

			std::span<const ShotType> pool = linesThisTurn >= 2 ?
				std::span<const ShotType>{ kNpcLateCoverage } :
				std::span<const ShotType>{ kNpcCoverage };

			if (forced) {
				// `pool` already holds the NPC side, early or late by how many
				// lines this turn has run. Leave that alone — a forced framing is
				// about WHO, not about narrowing the vocabulary on them.
				if (!wantNpc) {
					pool = std::span<const ShotType>{ kPlayerCoverage };
				}
			} else if (wantNpc) {
				// The room, as a change of pace inside the NPC's line.
				//
				// Reserving the player's turn for the player took the environmental
				// pool's only home away, and every wide, master, distant and
				// anchored setup lives there and nowhere else. So switching
				// coverage on made all of them unreachable, and the room-heavy
				// presets came out as nothing but singles — which is the opposite
				// of what they are for.
				//
				// They belong on this side anyway. A neutral has no wrong subject,
				// and a cut out to the space across somebody's speech is the breath
				// it was always meant to be; it was only ever on the player's turn
				// because that turn had nothing else to fill it.
				//
				// Rolled rather than counted: Coverage() is called more than once
				// per cut whenever a candidate is rejected, so a counter here would
				// advance by the number of REJECTIONS and the ratio would mean
				// nothing. That exact bug has already been fixed once on this
				// function's other pool.
				//
				// WEIGHTED AGAINST THE COVERAGE POOL rather than a flat one-in-three.
				//
				// The hardcoded third was the last place a weight could be overruled,
				// and it was the loudest: weight only ever discriminated WITHIN a
				// pool, so whichever room setups were left on split a third of every
				// NPC line between them no matter how low they were dialled. With one
				// room shot enabled it took that third on its own — measured, a setup
				// weighted 11, the lowest value on anything, taking 14% of all cuts.
				//
				// Comparing the two pools' totals makes the room's share what the room
				// shots are actually worth. Turn them all down and the camera stays on
				// faces; turn them up and it breathes more. Zero room weight now means
				// zero room shots, which is the answer the dial was always giving and
				// never getting. The shipped defaults land at roughly the same third
				// this replaces, so nothing moves for anyone who has not touched it.
				// Unconditional now, where it used to require coverPlayerTurn.
				//
				// With the dial off, this block was skipped and the room could not
				// appear during the NPC's line at all — so the ONLY place a room shot
				// could land was the player's turn, via the alternation below. That is
				// why turning coverage off read as "the camera is on the room half the
				// time, and always at the worst moment".
				const auto roomWeight = PoolWeight(kEnvironmental);
				const auto coverWeight = PoolWeight(pool);
				const auto pot = roomWeight + coverWeight;
				if (pot > 0 && (NextRandom() % pot) < roomWeight) {
					pool = std::span<const ShotType>{ kEnvironmental };
				}
			} else {
				// WEIGHTED, NOT ALTERNATED. This is the fix for "half the time it is
				// the room" and it is the same roll the NPC's side now uses.
				//
				// The old line read `(!coverPlayerTurn && alternateNeutral)` and
				// flipped the flag on every call. The comment above claims that gives
				// player, room, player — and it does not, because Coverage() is called
				// up to FIVE times per cut: once at the top of the cut block and once
				// per rejected candidate, four times over. So the flag's state at the
				// moment a shot was accepted is the parity of how many candidates
				// happened to be rejected first, which is a coin toss. The 2026-08
				// note about this ("evaluated once per cut, the alternation is the one
				// described") fixed the sixty-times-a-second case and missed the
				// retry loop directly below it.
				//
				// The consequence is the reported one, and it is the worst possible
				// shape for a weight dial: the room pool won half of every player turn
				// no matter what any of its setups were worth. A master weighted 11
				// and an extreme close-up weighted 80 were decided by a coin, and the
				// weights only ever discriminated WITHIN whichever pool won it.
				//
				// Comparing the two pools' live totals makes one weight scale span
				// both, which is what a weight is for: turn the room shots down and
				// the camera stays on faces, turn them to zero and they never appear.
				pool = std::span<const ShotType>{ kPlayerCoverage };

				// A reaction always shows the player, never the room.
				if (!coverPlayerTurn && !turnSinceCut && !ReactionActive()) {
					// Never on the FIRST cut of the player's turn, which is the one
					// piece of the old alternation worth keeping. The first shot after
					// a reply ends has to be the player, so the choice registers before
					// the camera goes wandering. turnSinceCut is true exactly until the
					// first cut of a turn is committed and is only read here, so it
					// survives the retry loop that the flag it replaces did not.
					const auto roomWeight = PoolWeight(kEnvironmental);
					const auto coverWeight = PoolWeight(kPlayerCoverage);
					const auto pot = roomWeight + coverWeight;
					if (pot > 0 && (NextRandom() % pot) < roomWeight) {
						pool = std::span<const ShotType>{ kEnvironmental };
					}
				}
			}

			// A weighted draw rather than rejection sampling.
			//
			// This used to pick uniformly and retry up to fourteen times, which had
			// two faults. It could exhaust its attempts on a pool that is mostly
			// disabled and fall through to Canonical(), reading as the variety
			// having gone; and it had nowhere to put a per-shot weight, because a
			// uniform draw over a pool can only express weights as repeated entries.
			// Walking the pool once cannot fail to find an eligible entry when one
			// exists.
			//
			// The repetition that used to be the baseline is gone from the pools and
			// lives in AuthoredWeight now, so the weight read here is the whole of
			// what decides frequency and it is the number on the slider.
			const auto pick = WeightedPick(pool, [&](ShotType a_type) {
				// A setup switched off, or dialled to zero, must be unreachable, or
				// the setting is a suggestion rather than a switch. A wide or distant
				// shot in a corridor is a camera in a wall.
				return a_type != currentShot && Drawable(a_type) &&
					!(NeedsRoom(a_type) && !roomy);
			});

			return pick ? pick : Canonical();
		}

		// The direction with the most open space around the conversation.
		//
		// Probed rather than assumed, so "somewhere else in the room" means a place
		// that exists. Sampled once per conversation: re-probing every frame would
		// let the distant shot wander as people move.
		void ProbeOpenDirection(const RE::NiPoint3& a_player, const RE::NiPoint3& a_npc)
		{
			const RE::NiPoint3 midpoint{
				(a_player.x + a_npc.x) * 0.5f, (a_player.y + a_npc.y) * 0.5f, (a_player.z + a_npc.z) * 0.5f
			};

			constexpr int kSamples = 12;

			// How far out the probe is willing to look for room.
			//
			// This was 560, and it was the real ceiling on every distant setup in
			// the mod: the scene shots take min(what the framing wants, this × 0.9),
			// so nothing could ever stand further back than about 500 units however
			// open the ground was. A long lens across a market square asked for
			// 1150 and got 500, which is a medium — the shot was not being composed
			// short, it was being cut off at the knees by the measurement.
			//
			// Raised well past anything an interior can return. Indoors this still
			// answers with the wall it finds, which is the self-limiting part: a
			// corridor reports a corridor and the shot places accordingly. It is
			// only outdoors, and in the larger city and cathedral spaces, that the
			// extra reach is there to be used — which is exactly where a distant
			// shot is worth having.
			constexpr float kReach = 2600.0f;
			constexpr float kTwoPi = 6.28318530718f;

			// ALL TWELVE ARE KEPT NOW. Eleven of them used to be measured and
			// dropped on the floor, while `roomy` — the flag gating every wide,
			// master, distant, full-figure and overhead in the mod — was decided
			// somewhere else entirely, from two bearings at right angles to the
			// eyeline.
			//
			// Two perpendicular bearings are close to the worst possible pair of
			// samples. Stand in a long hall talking to somebody DOWN its length and
			// both of them hit the side walls a few feet away, so the mod concludes
			// there is no room and switches off half its vocabulary in one of the
			// most photogenic spaces in the game. These twelve already knew better.
			float best = -1.0f;
			float total = 0.0f;
			int   longBearings = 0;

			for (int i = 0; i < kSamples; ++i) {
				const float angle = kTwoPi * (static_cast<float>(i) / static_cast<float>(kSamples));
				const RE::NiPoint3 direction{ std::cos(angle), std::sin(angle), 0.0f };

				const float room = Clearance(midpoint, direction, kReach);
				total += room;
				if (room >= 500.0f) {
					++longBearings;
				}
				if (room > best) {
					best = room;
					openDirection = direction;
					openDistance = room;
				}
			}

			const float mean = total / static_cast<float>(kSamples);

			// A wide shot needs ONE place to stand, not a room that is open in
			// every direction — which is exactly what the old max-of-two test could
			// not express. The corridor case passes on its long axis, as it should.
			roomy = openDistance >= 320.0f;

			// The classification is the mean, not the max: the max is already
			// openDistance and says only that some direction is clear. A corridor
			// and a square can share a max and are not the same room.
			roomSpace = mean >= 620.0f ? Space::kOpen :
					mean >= 210.0f ? Space::kRoom :
									 Space::kTight;

			// ONE RAY UP, and it is the first thing in the mod ever to look there.
			//
			// `rise` is the only column in the shot table in absolute world units,
			// several setups lift the camera a long way on it, and the only thing
			// that has kept an overhead indoors is a per-build framing constant that
			// was added after one went through a ceiling. Measured once here, from
			// the same midpoint as everything else, and clamped against in Solve.
			constexpr float kUpReach = 520.0f;
			ceilingRoom = Clearance(midpoint, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, kUpReach);

			Log::Info(Log::Category::kStaging,
				"Space probed: widest {:.0f}u, mean {:.0f}u, {} of {} bearings long, {:.0f}u overhead -> {} space; wide shots {}."sv,
				openDistance, mean, longBearings, kSamples, ceilingRoom,
				SpaceName(roomSpace), roomy ? "allowed"sv : "suppressed"sv);
		}

		// Line cadence and reaction counts start over with each NPC reply.
		void ResetReplyCounters()
		{
			if (reactionShots.Active()) {
				Log::Info(Log::Category::kContinuity, "Reaction over; a new reply started."sv);
			}
			reactionShots.Reset();
			linesSinceCut = 0;
			cutEveryTarget = RollCutEvery();
			linesThisTurn = 0;
		}

		// Full-intensity lines count toward a reaction but can't be one, since
		// Choose() gives those to the NPC close-up.
		void UpdateReaction(bool a_eligible, std::uint16_t a_intensity)
		{
			const bool wasActive = reactionShots.Active();
			const bool wasOwed = reactionShots.Owed();
			const std::uint32_t roll = reactionSettings.enabled ? NextRandom() % 100u : 0u;
			reactionShots.OnNpcLine(reactionSettings, a_eligible, a_intensity < kCloseUpIntensity,
				framing == Framing::kAuto, roll);

			if (reactionShots.Active()) {
				Log::Info(Log::Category::kContinuity, "Reaction: this line plays on you."sv);
			} else if (wasActive) {
				Log::Info(Log::Category::kContinuity, "Reaction over; back to them."sv);
			}
			if (reactionShots.Owed() && !wasOwed) {
				Log::Info(Log::Category::kContinuity,
					"Reaction rolled ({}% after {} line(s)); next line plays on you."sv,
					reactionSettings.chance, reactionSettings.every);
			}
		}

		// Feeds topic picks to replyBoundary: the menu's click edge, or a voiced
		// player line starting.
		void TrackTopicPicks(const Scene::Interface::DialoguePhase& a_phase, float a_delta)
		{
			using Phase = Scene::Interface::MenuPhase;
			replyBoundary.Advance(a_delta);

			const bool clicked = a_phase.valid && a_phase.phase == Phase::kTopicClicked &&
				lastPickPhase != Phase::kTopicClicked;
			if (a_phase.valid) {
				lastPickPhase = a_phase.phase;
			}

			const auto serial = Scene::LipSync::PlayerLineSerial();
			const bool spoke = serial != lastPlayerLineSerial;
			lastPlayerLineSerial = serial;

			if (clicked || spoke) {
				replyBoundary.OnPick();
			}
		}

		void ApplyCue(RE::Actor* a_speaker, std::uint32_t a_words, std::uint16_t a_intensity,
			std::uint32_t a_emotion, std::string_view a_text)
		{
			// A cue is unambiguous â€” the engine is handing over the line being
			// spoken â€” so it settles the debounce rather than writing npcSpeaking
			// behind its back.
			//
			// But a cue is NOT a turn change. A topic info can hold several
			// responses spoken back to back, and flagging each of them as a turn
			// dropped the cut floor from 2.4s to 0.25s for the whole reply, so the
			// camera cut every quarter second. That was the bouncing, and it had
			// nothing to do with the room â€” which is exactly why it happened
			// outdoors too.
			const bool alreadySpeaking = npcSpeaking;
			const bool alreadyCoveredReply = playerVoiceHandoff.Active() &&
				framing == Framing::kAuto && !IsNeutral(currentShot) && FavoursNpc(currentShot);

			// The NPC answering retires the player's voice handle. See VoicePlaying:
			// some voices never release theirs, and a handle that claims to still be
			// playing after it has been replied to kept the topic list hidden.
			retiredVoiceID = voiceHandleID;

			npcSpeaking = true;
			wasSpeaking = true;
			pendingSpeaking = true;
			pendingSince = Clock::now() - std::chrono::milliseconds(500);

			// Is there enough of this line to be worth a new setup?
			//
			// A zero count is not a short line, it is an unmeasured one â€” a
			// response with no authored subtitle still has audio behind it, and
			// treating "cannot tell" as "not worth it" would silently stop the
			// camera cutting on entire mods that ship voice without text.
			// Unmeasurable fails open.
			//
			// Intensity outranks length, and the exception matters more than it
			// looks. Word count is a proxy for how much a line has to say, and the
			// one place that proxy inverts is the line delivered at full force:
			// "No!", "Enough!", "Get out." are the shortest lines in the game and
			// the most deserving of a cut in it. Choose() already reserves the
			// tightest setup in the mod for exactly these, so a floor that filtered
			// them would have suppressed the mod's best moment and kept every cut
			// on the exposition around it.
			//
			// Cheap because it is rare: measured across 96,081 authored responses,
			// 76-83% carry an intensity of exactly 50 â€” a default nobody touched â€”
			// so this only admits a line the writer deliberately marked.
			const bool worthCutting =
				!holdOnShortLines ||
				a_words == 0 ||
				a_words >= shortLineWords ||
				a_intensity >= kCloseUpIntensity;

			// WHAT USED TO BE HERE: clearing `establishing` once a line was worth
			// cutting for, so the opening two-shot handed over.
			//
			// There is no opening two-shot to hand over from. The camera is already
			// on whoever is speaking from the first frame of the conversation, so
			// there is nothing for a line starting to release.

			// Suppressed on BOTH edges, including the turn.
			//
			// Gating only the new-line edge would have been nearly inert: at the
			// default cadence of one, the first line of every reply cuts as a turn
			// change and only lines two onward arrive as new lines, so the shortest
			// replies in the game â€” the single "Yes." that is the whole answer â€”
			// would have been exactly the ones that still cut.
			//
			// Safe to suppress the turn because it is not the mechanism that keeps
			// the camera on the speaker. wrongSubject in the tick does that, it is
			// judged independently of any of this, and it cuts on the turn floor.
			// Worst case here is that a trivial line does not earn a *fresh* angle;
			// it can never leave the camera watching the wrong person.
			if (!worthCutting) {
				Log::Info(Log::Category::kContinuity,
					"Line of {} word(s) is under the {}-word floor; holding {}."sv,
					a_words, shortLineWords, Name(currentShot));
			}

			if (!alreadySpeaking) {
				// The early handoff already covered this turn. A later cadence cut
				// keeps the ordinary shot floor, rather than cutting again at 0.25s.
				if (worthCutting && !alreadyCoveredReply) {
					turnSinceCut = true;
				}
				replyStartedAt = Clock::now();
				turnBeganAt = replyStartedAt;
				linesThisTurn = 0;
			}

			if (replyBoundary.OnLine(alreadySpeaking)) {
				ResetReplyCounters();
			}
			// Every line, short ones included: a new line is what ends a reaction.
			UpdateReaction(worthCutting, a_intensity);

			cueIntensity = a_intensity;
			Scene::Performance::OnLine(a_speaker, a_emotion, a_intensity, a_text);

			if (worthCutting) {
				cueSinceCut = true;
			}

			// ELIGIBLE LINES ONLY, WHICH IS WHAT THE CONTROL SAYS IT COUNTS.
			//
			// This used to count every line, on the argument that a short one still
			// went by and skipping it would hide a run of terse lines from the
			// tally. That argument was written when the cadence shipped at 1 and
			// the turn cut on every line anyway, so the difference was invisible.
			//
			// At 3 to 6 it is not. "Ignore Short Lines" has to mean ignored: a run
			// of "Yes." and "Hmm." should not walk the camera toward its next angle,
			// because the whole point of the floor is that there was nothing in
			// those lines worth cutting for. linesThisTurn is a different question —
			// how much of this reply has gone by — and still counts everything.
			++linesThisTurn;
			if (worthCutting) {
				++linesSinceCut;
			}
			rngState ^= ++cueCount * 0x85EBCA6Bu;
		}

		[[nodiscard]] std::optional<ShotType> Choose()
		{
			// WHAT USED TO BE HERE: the establishing window, returning kTwoShot for
			// the first iEstablishTime of every conversation.
			//
			// Gone with the shot. Open() now puts the camera on whoever is speaking
			// on the first frame, so there is no window to be inside and nothing
			// here that outranks coverage.

			// A turn change returns to coverage. This is shot/reverse-shot, and the
			// previous version could not do it: the picker refused to repeat the
			// current or previous shot, so whenever coverage was on screen it was
			// forbidden from staying there and got forced onto a lateral angle. The
			// logged distribution showed the result â€” profile and wide together
			// outnumbering both shoulder shots despite being weighted against 4:1.
			// An intensity the writer actually raised â€” 5-8% of lines. Worth
			// overriding the rotation for.
			if (npcSpeaking && cueIntensity >= kCloseUpIntensity &&
				currentShot != ShotType::kCloseUp && currentShot != ShotType::kExtremeClose) {
				// The extreme is the rarer of the two deliberately. Intensity 100 is
				// already only 5-8% of lines; taking a third of those puts the
				// tightest setup in the mod at roughly 2%, which is the difference
				// between a shot that lands and a mannerism.
				//
				// Both are checked against the enable flags, and if the player has
				// turned off both this drops through to ordinary coverage rather
				// than forcing either. An override is still a shot, and a shot
				// switched off should not come back because the line was loud.
				const bool extreme = NextRandom() % 3u == 0u;
				if (extreme && Drawable(ShotType::kExtremeClose)) {
					return ShotType::kExtremeClose;
				}
				if (Drawable(ShotType::kCloseUp)) {
					return ShotType::kCloseUp;
				}
				if (Drawable(ShotType::kExtremeClose)) {
					return ShotType::kExtremeClose;
				}
			}

			// A turn change, a new line, or a shot that has sat too long all mean
			// the same thing: find a different angle on whoever is talking.
			return Coverage();
		}

		// WHAT USED TO BE HERE: Usable(), `return Solve(...).valid`.
		//
		// One bool, and it was the entire question the picker ever asked about a
		// room. An angle shoved against a wall at the distance floor, swung eighty
		// degrees off its own bearing, with a market stall across half the frame,
		// answered it exactly as well as one standing in open space at the size it
		// asked for — so in a cramped interior the camera took whichever of those
		// the dice offered first.
		//
		// Placement() below replaces it and is a superset: a shot that will not
		// place scores -1, which is the same information, and everything above zero
		// is what `valid` could never express.

		// ONE SOLVE PER SHOT TYPE PER CUT, and the reason it is worth a cache.
		//
		// A cut evaluates up to four weighted draws and, if none of them place, a
		// ladder of every enabled setup on the correct side. Those two overlap
		// heavily — the draws are drawn FROM the set the ladder walks — and a draw
		// can repeat a type it has already offered. Every one of those repeats was
		// a fresh Solve, and a Solve is a sweep of raycast bundles.
		//
		// Sound because `subjects` does not change inside one cut evaluation: it is
		// built once per frame and the held sweep, the progress and the frame delta
		// are all filled in AFTERWARDS, on the render path. A cache that outlived
		// this block would be wrong for exactly that reason, so it is a local and
		// dies with the decision it was built for.
		struct ScoreCache
		{
			std::array<float, static_cast<std::size_t>(ShotType::kCount)> quality{};
			std::array<bool, static_cast<std::size_t>(ShotType::kCount)>  known{};
		};

		// How well this setup places here, or -1 if it does not place at all.
		//
		// Negative rather than zero for the refusal, so "cannot be placed" always
		// loses to "placed badly" — a shot that resolves is never worse than one
		// that does not, however poor its score.
		[[nodiscard]] float Placement(ShotType a_type, const Subjects& a_subjects, ScoreCache& a_cache)
		{
			const auto index = static_cast<std::size_t>(a_type);
			if (!Drawable(a_type) || index >= a_cache.known.size()) {
				return -1.0f;
			}

			if (!a_cache.known[index]) {
				const auto pose = Solve(a_type, a_subjects);
				a_cache.quality[index] = pose.valid ? pose.quality : -1.0f;
				a_cache.known[index] = true;
			}

			return a_cache.quality[index];
		}

		// Good enough to stop looking.
		//
		// The old loop took the first candidate that PLACED, which is why a shot
		// jammed against a wall at the distance floor beat one standing in open
		// space: both answered the same yes. Comparing all four draws fixes that
		// and costs three extra solves on the common path, where the first draw was
		// already fine — so a draw that got essentially the shot it asked for still
		// ends the search immediately, and the full comparison only runs when the
		// room is genuinely making the mod compromise.
		constexpr float kGoodEnough = 0.82f;

		// Is this actor audibly saying something right now?
		//
		// Asked of the actor's own sound handles rather than of MenuTopicManager,
		// and that is the entire point: under a player-voice mod the engine reports
		// nobody speaking for the whole of the player's turn, because the topic is
		// not committed until their spoken line has finished. The sound is playing
		// the whole time and can simply be looked at.
		// A PLAYING SOUND HANDLE THAT NEVER STOPS BEING ONE.
		//
		// BSSoundHandle::state is an ASSUMED state, and on some voices it is simply
		// never updated. Measured 2026-08-10 on modded followers (River, Katana):
		// the player's handle sat in kPlaying for 84 and 97 seconds, and was only
		// ever displaced by the next line starting — the log line reads "STARTED
		// (replacing 9409 after 97.48s)". Every vanilla NPC tested released cleanly
		// in about two seconds, which is exactly why this survived a whole evening
		// of testing against Farkas and Brill.
		//
		// Untreated it latches: the topic list fade asks "is the player speaking",
		// gets a permanent yes from the first line onward, and the list fades out
		// and never comes back. Reported as the tree disappearing after two entries
		// and refusing to return while waiting to answer.
		//
		// So the handle is timed here rather than believed. A voiced dialogue line
		// materially longer than this is not a line, it is a handle nobody cleared;
		// the measured ones run about two seconds, so the ceiling is generous by
		// five times over and still bounded.
		//
		// A CEILING ALONE IS NOT ENOUGH, measured 09:16:26. The handle was 2.6s old
		// when the NPC finished answering — well inside any plausible line length —
		// so a timeout could not tell it from a real one, the list stayed down, and
		// the dead man had to force it back four seconds later.
		//
		// What settles it is not time but the conversation: once the NPC has
		// answered, the player's line is over, whatever its handle still claims.
		// So a cue RETIRES the current handle, and a retired id never counts as
		// speech again however long it sits in kPlaying. The ceiling stays as a
		// backstop for a line nobody ever answers.
		//
		// One shared set of fields, because there is exactly one caller and it asks
		// about the player. A second caller for another actor would need its own.
		// Raised from 6.0 on 2026-08-13. It was a backstop against a handle stuck
		// in kPlaying, and it was also cutting real speech off: a long player line
		// runs past six seconds, at which point this declared the voice finished
		// while it was still audible. The topic list then left the "you are
		// speaking" branch mid-line and eased back in — reported exactly that way.
		//
		// A cue already retires the handle when the NPC answers, which is the
		// reliable end. This only has to catch a line nobody ever replies to, so it
		// belongs far past any real one rather than just past the average.
		constexpr float kMaxVoiceSeconds = 20.0f;

		[[nodiscard]] bool VoicePlaying(RE::Actor* a_actor)
		{
			// DBReV answers this directly, and its answer is the CONVERSATION
			// window rather than the audio one — open from the start of the line
			// until the dialogue actually advances, post-line delay included.
			//
			// That is the right window for this caller and the wrong one for
			// LipSync, which stops the mouth when the audio stops. The two are
			// separated deliberately; see Compat::DBReV::Line.
			//
			// Everything below — the handle scan, the retired id, the twenty-second
			// ceiling — is the DBVO path and is untouched.
			//
			// EverSpoke, not Present: DBReV loading does not mean DBReV is the mod
			// voicing this player. See Compat::DBReV::EverSpoke.
			if (Compat::DBReV::Present() && Compat::DBReV::EverSpoke()) {
				return Compat::DBReV::Speaking();
			}

			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			auto* high = process ? process->high : nullptr;
			if (!high) {
				return false;
			}

			std::uint32_t playing = RE::BSSoundHandle::kInvalidID;
			for (auto& handle : high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					playing = handle.soundID;
					break;
				}
			}

			if (playing == RE::BSSoundHandle::kInvalidID) {
				voiceHandleID = RE::BSSoundHandle::kInvalidID;
				return false;
			}

			// Answered already. The NPC replying is proof the line finished, and no
			// amount of kPlaying afterwards makes it true again.
			if (playing == retiredVoiceID) {
				return false;
			}

			// A different id is a genuinely new line, whatever the old one claimed.
			if (playing != voiceHandleID) {
				voiceHandleID = playing;
				voiceHandleSince = Clock::now();
				return true;
			}

			return SecondsSince(voiceHandleSince) < kMaxVoiceSeconds;
		}

		// The rendered aspect ratio, which decides how tall a subject appears.
		//
		// Skyrim reports a horizontal field of view, so framing by height is wrong
		// without this. Read from the swap chain rather than assumed, because an
		// ultrawide display makes every vertical estimate badly wrong.
		[[nodiscard]] float ScreenAspect()
		{
			auto* manager = RE::BSGraphics::Renderer::GetSingleton();
			if (!manager) {
				return 1.78f;
			}

			auto* swapChain = reinterpret_cast<IDXGISwapChain*>(manager->GetRuntimeData().renderWindows[0].swapChain);
			if (!swapChain) {
				return 1.78f;
			}

			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(swapChain->GetDesc(&desc)) || desc.BufferDesc.Height == 0) {
				return 1.78f;
			}

			return std::clamp(
				static_cast<float>(desc.BufferDesc.Width) / static_cast<float>(desc.BufferDesc.Height),
				1.0f, 3.0f);
		}

		// Which side of the eyeline the conversation gets staged from.
		//
		// The first version compared the camera's existing position against the
		// eyeline. That was wrong in a way that only showed up in play: the player
		// starts a conversation facing the NPC, so the camera sits almost exactly
		// *on* the line and the projection is near zero. The sign was noise, and
		// noise with a positive bias â€” so every shot landed on the right, forever.
		//
		// The room decides instead. Probe both directions and take the side with
		// more space, which also keeps the camera out of walls for free. When both
		// are equally open it is a genuine coin flip, so conversations vary.
		void ChooseSide(const RE::NiPoint3& a_player, const RE::NiPoint3& a_npc)
		{
			bool       ok = false;
			const auto axis = Normalized({ a_npc.x - a_player.x, a_npc.y - a_player.y, a_npc.z - a_player.z }, ok);
			if (!ok) {
				side = 1.0f;
				return;
			}

			const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(axis, worldUp), ok);
			if (!ok) {
				side = 1.0f;
				return;
			}

			const RE::NiPoint3 midpoint{
				(a_player.x + a_npc.x) * 0.5f,
				(a_player.y + a_npc.y) * 0.5f,
				(a_player.z + a_npc.z) * 0.5f
			};
			const RE::NiPoint3 leftward{ -right.x, -right.y, -right.z };

			constexpr float kProbe = 260.0f;
			const float     roomRight = Clearance(midpoint, right, kProbe);
			const float     roomLeft = Clearance(midpoint, leftward, kProbe);

			// A FIRST DECISION AND A RE-DECISION ARE NOT THE SAME QUESTION.
			//
			// This runs again whenever somebody sits down or stands up, and it used
			// to re-decide on the same 40-unit margin it used to decide. Forty units
			// is nothing — a chair moving, a follower shifting their feet — so an
			// NPC taking a seat mid-conversation could flip the camera to the far
			// side of the eyeline. That is the one thing screen grammar genuinely
			// forbids: the two of you appear to swap places on the cut.
			//
			// Crossing is not forbidden outright, because a posture change really
			// can open a side that was solid before. It just has to be an obvious
			// win rather than a coin toss, and the coin toss for near-equal room is
			// gone entirely once a side is committed — there is nothing to gain from
			// re-rolling a decision that is already on screen.
			constexpr float kMeaningful = 40.0f;
			constexpr float kToFlip = 190.0f;

			const float wasSide = side;
			const float margin = sideCommitted ? kToFlip : kMeaningful;

			if (roomRight > roomLeft + margin) {
				side = 1.0f;
			} else if (roomLeft > roomRight + margin) {
				side = -1.0f;
			} else if (!sideCommitted) {
				side = (NextRandom() & 1u) ? 1.0f : -1.0f;
			}
			// else: keep the side already on screen.

			// WHAT USED TO BE HERE: roomy, decided from these two bearings.
			//
			// It now comes out of ProbeOpenDirection, which casts twelve of them
			// over the full compass and was already discarding eleven. See the note
			// there; this pair could not see a hall it was standing in the middle of.

			Log::Info(Log::Category::kStaging,
				"Side {}: {} (room left {:.0f}u, right {:.0f}u)."sv,
				!sideCommitted			   ? "chosen"sv :
					side == wasSide		   ? "held"sv :
											 "FLIPPED"sv,
				side < 0.0f ? "left"sv : "right"sv, roomLeft, roomRight);

			sideCommitted = true;
		}

		// Apply what the ini or the menu last asked for. Runs every frame, does
		// nothing on all but the one after a change.
		//
		// Ahead of the staging check in Tick, not behind it, because it has to be
		// right BEFORE a conversation opens: Open() calls Suppress immediately, and
		// Suppress reads hideSpeakerName on its way through.
		void SyncInterfaceSettings()
		{
			if (!interfaceDirty) {
				return;
			}
			interfaceDirty = false;

			// Reconciles itself against a live conversation: SetHideSpeakerName puts
			// the name back when it is switched off, which nothing else would do
			// until the conversation ended.
			Scene::Interface::SetHideSpeakerName(wantHideSpeakerName);
		}

		// THE SCREEN FURNITURE, DRIVEN FROM THE ALWAYS-ON TICK.
		//
		// Moved out of OnThirdPersonUpdate, which is a camera hook and therefore
		// silent on any frame the camera is not in third person. See the note left
		// at the old site for what that cost.
		//
		// The topic list follows the engine's own dialogue phase. That is the whole
		// feature.
		//
		// WHAT USED TO BE HERE: four hundred lines that reconstructed "whose turn is
		// it" out of nine proxies — npcSpeaking, the player's sound handles,
		// Session::Speaking(), response counts, a row fingerprint, which side the
		// camera happened to be on, five timers and four latches — and then animated
		// the list against the answer. Every one of those proxies has its own
		// paragraph in this file's history explaining the conversation it lost. They
		// were all standing in for a single question, and DialogueMenu_mc has been
		// answering that question directly the whole time, in a log line nothing
		// acted on.
		//
		// eMenuState runs greeting -> topicList -> topicClicked -> transitioning ->
		// topicList, and topicList means exactly "the options are live, and they are
		// the new ones". Reading it instead of guessing at it closes, by
		// construction rather than by another special case:
		//
		//   - stale rows. Phase 1 is only reached AFTER the rebuild, because the
		//     rebuild is the transitioning step. The fingerprint test is gone.
		//   - the greeting flash. The greeting is its own phase, so there is nothing
		//     to suppress and no open grace to size.
		//   - the player-voice hole. The phase sits at topicClicked for the whole of
		//     the player's turn, including under DBVO, and nothing below reads
		//     currentTopicInfo — which is null across all of it.
		//   - the list waiting on the camera. The return was timed from
		//     playerSideSince, so a held master shot pinned it. Nothing here knows
		//     what the camera is doing.
		//   - the dead man, which existed only because the machine above it was
		//     known to strand.
		//
		// What is left is an animator with no opinions. All four SD.ini dials keep
		// the meaning their comments give them, because all four were always
		// output-side: two delays and two durations.
		void DriveInterface()
		{
			// THE GATE THAT USED TO BE HERE, AND THE BUG IT HAD.
			//
			// This was `if (hideInterface)`, so bFadeTopicList lived INSIDE bHideInterface
			// and could not run without it. Switch the HUD hide off — which was a reasonable
			// thing to want, since wholesale took quest updates with it — and the fade went
			// with it, silently: the key still read as 1, ApplyTunables still applied it, and
			// the only symptom was a topic list that never moved.
			//
			// Diagnosed 2026-08-19 off a log holding two complete conversations and not one
			// "Menu phase" line. That absence is the fingerprint: the phase log fires on
			// every change and the probe warns when it cannot resolve, so no line of either
			// kind means the block never executed, rather than executing and finding nothing.
			//
			// There is no gate at all now. The HUD hide is not a setting, so this function
			// has a reason to run on every staged frame, and interfaceEngaged — which existed
			// only to catch the falling edge of both features being switched off — has gone
			// with it. Close() is the one place the choices are handed back.

			// Hold what Suppress() took away.
			//
			// A single write at the top of a conversation is one the game undoes at its
			// leisure — HUDMenu's _visible is rewritten by the engine and by HUD mods on
			// their own schedule — and a menu that OPENS midway through was never in the
			// map when that write ran. The topic list has enforced its own _visible every
			// frame for exactly this reason since 2026-08-09; this is that lock applied to
			// the rest of the screen.
			//
			// Suppress() first, and every frame, because it is idempotent and because
			// it can FAIL: it warns "HUD movie unavailable" and returns without marking
			// the conversation suppressed, which used to mean nothing tried again for
			// the rest of it. Called from here it simply retries until the movie is
			// there, and costs one branch once it is.
			Scene::Interface::Suppress();
			Scene::Interface::Enforce();

			// Still routed through SetChoiceAlpha even when the fade is off, because
			// the speaker-name hide rides inside it. Not calling it would quietly
			// switch bHideSpeakerName off.
			const auto moviePhase = Scene::Interface::ReadDialoguePhase();

			if (moviePhase.valid && moviePhase.phase != lastMoviePhase) {
				lastMoviePhase = moviePhase.phase;
				Log::Info(Log::Category::kStaging,
					"Menu phase: {}"sv, Scene::Interface::Name(moviePhase.phase));
			}

			// FAIL OPEN. A movie that does not publish eMenuState gets its list left
			// alone at full opacity, rather than driven from an inference — the
			// inference is precisely what was removed here, and a replacer movie is
			// not a reason to bring it back.
			const bool listLive = !moviePhase.valid ||
				moviePhase.phase == Scene::Interface::MenuPhase::kTopicList;

			// The list is never hidden before the engine has declared it live once.
			//
			// This is playerHasChosen's job and it is kept, because the hazard behind
			// it is real and unchanged by any of this: Accept commits the HIGHLIGHTED
			// topic without consulting the cursor, and this mod does not gate dialogue
			// input. A list hidden on the approach to an NPC is a list that the click
			// meant to start the conversation selects blind. See the note where
			// bHideChoicesOnGreeting was removed, which is the same bug.
			//
			// Unlike playerHasChosen this asks the movie rather than inferring a
			// selection from a turn edge, so it cannot arm late, arm twice, or — as
			// measured on the opening frame of a conversation — survive into the next
			// one still armed.
			if (listLive) {
				listWasLive = true;
			}

			const bool fading = fadeTopicList && listWasLive;

			// The target, and the moment it last changed. Everything after this is
			// easing between two numbers.
			const bool wantList = !fading || listLive;
			if (wantList != listWanted) {
				listWanted = wantList;
				listEdgeAt = Clock::now();
				choiceEaseFrom = choiceAlpha;
			}

			// FADE AFTER PC LINE. The only place a sound handle is still read.
			//
			// ON means: the engine took the list down at the click, but the player's
			// own voiced line is still running, so hold what is on screen and start
			// the delay when that line actually ENDS. The clock is pinned rather
			// than the alpha forced, so this delays an output and cannot strand
			// anything — the movie's phase still owns the target.
			//
			// WHERE THE END COMES FROM, in order of authority:
			//
			//   DBReV broadcasts the line's start and its end, and the end carries a
			//   reason — completed, skipped, superseded — so a skipped line releases
			//   the hold on the frame it was skipped. VoicePlaying prefers this
			//   whenever ReVoiced has actually spoken once.
			//
			//   DBVO 1 and 2, and vanilla, have no such event. VoicePlaying falls
			//   back to the player's own sound handles, which is the observation
			//   this mod has always used: a handle in kPlaying that is neither
			//   retired by an NPC's reply nor older than the twenty-second ceiling.
			//
			// AND THE FAIL-SAFE UNDER BOTH. Either source can in principle never say
			// the line finished — a handle stuck in kPlaying, an event lost to a
			// mod that force-closed the menu — and a hold that waits forever is a
			// spent topic list nailed to the screen. The bound is measured from the
			// moment the hold started rather than from the line, so a missing end
			// costs one delay and not a conversation.
			const bool wantHold = !wantList && fadeAfterPlayerLine;
			bool       holdForVoice = false;

			if (wantHold && VoicePlaying(RE::PlayerCharacter::GetSingleton())) {
				if (!playerLineHeld) {
					playerLineHeld = true;
					playerLineHeldSince = Clock::now();
				}

				holdForVoice = SecondsSince(playerLineHeldSince) < kMaxPlayerLineHold;

				if (!holdForVoice && playerLineHoldExpired.Take()) {
					Log::Warn(Log::Category::kStaging,
						"Your line has been reported as playing for {:.0f}s with no end; "
						"releasing the topic list rather than holding it any longer."sv,
						kMaxPlayerLineHold);
				}
			} else {
				// Released on the frame the voice stops, and on every frame the
				// list is wanted back. Both are resets: the next line has to start
				// its own hold rather than inherit this one's clock, which is what
				// makes a skipped line behave like a finished one.
				playerLineHeld = false;
			}

			if (holdForVoice) {
				listEdgeAt = Clock::now();
			} else {
				// iListReturnDelay / iChoiceFadeDelay before it starts moving,
				// kChoiceFadeSeconds / iChoiceFadeTime to move. Arriving is an
				// invitation and can afford to ease; leaving is an acknowledgement and
				// wants to be quick.
				const float delay = wantList ? listReturnDelay : choiceFadeDelay;
				const float span = std::max(wantList ? kChoiceFadeSeconds : choiceFadeOut, 0.01f);
				const float since = SecondsSince(listEdgeAt) - delay;

				if (since > 0.0f) {
					const float t = std::clamp(since / span, 0.0f, 1.0f);
					const float target = wantList ? 100.0f : 0.0f;
					choiceAlpha = choiceEaseFrom + (target - choiceEaseFrom) * t;
				}
			}

			// NO STALENESS TIMER, and that is deliberate.
			//
			// A draft of this warned when the list had been down longer than N
			// seconds. It cannot work: a stuck movie and a one-minute monologue look
			// identical to a stopwatch, which is the exact confusion the dead man
			// was built out of. The phase log above is the better instrument — if the
			// list is ever stranded, the last line it printed names the phase it
			// stranded in, and the absence of any line after it is the evidence.
			Scene::Interface::SetChoiceAlpha(choiceAlpha);
		}

		// Dialogue and anatomy updates must continue while the camera is in first person.
		[[nodiscard]] std::optional<Subjects> SampleSubjects(float a_delta)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto speakerPtr = subject.get();
			if (!player || !speakerPtr) {
				return std::nullopt;
			}
			const auto npcNow = PostureOf(speakerPtr.get());
			const auto playerNow = PostureOf(player);
			bool       postureChanged = false;
			if (!posturePrimed) {
				posturePrimed = true;
			} else if (npcNow != npcPosture || playerNow != playerPosture) {
				postureChanged = true;
				Log::Info(Log::Category::kStaging, "Posture changed ({} -> {}); re-measuring and re-staging."sv,
					PostureName(npcNow == npcPosture ? playerPosture : npcPosture),
					PostureName(npcNow == npcPosture ? playerNow : npcNow));
			}
			npcPosture = npcNow;
			playerPosture = playerNow;

			if (postureChanged) {
				protectedPose = {};
				protectedSearch = {};
				visibilityFailedObservation = false;
				// Snap rather than chase. Follow() eases toward the target, and easing
				// from a bed to a standing head drags the camera up through the mattress
				// and whatever is above it. Standing up is a cut, not a move.
				playerAnchor.primed = false;
				npcAnchor.primed = false;

				// Re-run ChooseSide and the clearance probe. The room around a subject
				// lying in a bed alcove is not the room around them once they are on
				// their feet beside it, and the old open direction can point into a wall.
				haveShot = false;

				// All three held values are relative to things that are about to change.
				// heldSweep is an offset off the eyeline signed by `side`, and on the
				// scene path an offset off openDirection — ChooseSide can flip the first
				// and ProbeOpenDirection can move the second, either of which would
				// re-apply the same number to a different base and throw the camera
				// across the room. This is already declared a cut rather than a move, so
				// the shot re-chooses its angle exactly as it would at any other cut.
				//
				// heldRoom belongs to the bearing heldSweep names, so it cannot outlive
				// it. A posture change is the case that makes this concrete rather than
				// tidy: somebody getting out of a bed is the one moment the room around
				// them genuinely changes, and it is the last measurement anyone should
				// be holding on to.
				heldSweep = kUnheld;
				heldStandoff = kUnheld;
				heldRoom = kUnheld;
			}

			const bool firstSample = !playerAnchor.primed;

			// Measured on the same edge the anchors are primed on, and again whenever a
			// posture change forces a re-stage. Both of those already mean "everything
			// about this subject is being taken again"; a creature's size is exactly
			// that kind of fact.
			if (firstSample || !npcBody.measured || !playerBody.measured) {
				npcBody = Measure(speakerPtr.get());
				playerBody = Measure(player);

				// The player is the ruler. Fitted against their own radius they come
				// out at exactly 1.0, so an ordinary conversation is framed with the
				// same numbers the shot table was written against no matter what a
				// world bound radius turns out to be in absolute terms — and the NPC is
				// sized relative to a person actually standing there.
				const float reference = playerBody.measured ? playerBody.radius : 0.0f;
				Fit(playerBody, reference);
				Fit(npcBody, reference);
			}

			const auto npcTarget = StablePoint(speakerPtr.get(), npcBody);
			const auto playerTarget = StablePoint(player, playerBody);
			if (!npcTarget || !playerTarget) {
				return std::nullopt;
			}

			const float delta = std::clamp(a_delta, 0.0f, 0.1f);

			Follow(playerAnchor, *playerTarget, delta);
			Follow(npcAnchor, *npcTarget, delta);

			// Faces and eyes, driven off the turn as it stood this frame.
			Scene::Performance::Update(delta, npcSpeaking);

			// Ahead of nothing in particular, but every frame: the gaze model above
			// only moves an OFFSET, and an offset is meaningless if the target it
			// hangs off has been taken back by a package or a combat check.
			Scene::Presence::Update(delta);

			// Debounced, and this is the fix for the camera flicking to a random angle
			// between lines.
			//
			// currentTopicInfo goes null for a moment between two responses in the same
			// exchange, and again after a player-voice line before the NPC's cue lands.
			// Read raw, that reads as the turn passing â€” so the camera cut away to the
			// other party and cut straight back. A turn has to hold before it counts.
			const bool rawSpeaking = Dialogue::Session::GetSingleton().Speaking();
			if (rawSpeaking != pendingSpeaking) {
				pendingSpeaking = rawSpeaking;
				pendingSince = Clock::now();
			}

			const bool settled = SecondsSince(pendingSince) >= kTurnDebounceSeconds;
			if (settled && pendingSpeaking != wasSpeaking) {
				npcSpeaking = pendingSpeaking;
				wasSpeaking = npcSpeaking;

				// The one place a turn is declared to have changed. Everything that
				// wants to expire after "one exchange" counts this rather than keeping
				// its own idea of whose turn it is — which is how the mod ended up with
				// nine proxies for that question once already.
				++turnSerial;

				// ONLY ONE EDGE OF THE TURN MEANS ANYTHING NOW.
				//
				// A turn passing INTO speech marks the turn, which is what lets a
				// cut landing here use the short floor rather than the full one; the
				// cut itself still has to be asked for by the line count, by a timer,
				// or by coverage. Note the start edge is normally set in OnCue, which
				// sees the cue before the debounce settles; this branch still has to
				// allow it for the case where a line begins without a cue reaching us.
				//
				// A turn passing OUT of speech marks nothing at all. That was
				// bCutOnLineEnd and it is gone.
				if (npcSpeaking) {
					turnSinceCut = true;
				} else {
					// The pending line cue expires with the line it belonged to.
					//
					// Without this the hold leaks: a line shorter than iMinShotTime
					// leaves cueSinceCut set because the floor blocked its cut, and
					// that motivation survives into the silence and fires the moment
					// the floor elapses. The result is the cut arriving a second or
					// two AFTER the NPC stopped talking, which is more jarring than
					// the cut it was suppressed to prevent, not less.
					cueSinceCut = false;

					// The reply is over; drop any reaction state with it.
					reactionShots.Reset();

					Log::Info(Log::Category::kContinuity,
						"Line ended; holding {} through the pause."sv,
						Name(currentShot));
				}

				turnBeganAt = Clock::now();
				if (npcSpeaking) {
					replyStartedAt = turnBeganAt;
				}
			}

			if (!haveShot) {
				ChooseSide(playerAnchor.position, npcAnchor.position);
				ProbeOpenDirection(playerAnchor.position, npcAnchor.position);
				screenAspect = ScreenAspect();
				Log::Info(Log::Category::kStaging, "Framing against aspect {:.2f}."sv, screenAspect);
				haveShot = true;
			}

			Subjects subjects{};
			subjects.playerHead = playerAnchor.position;
			subjects.npcHead = npcAnchor.position;
			// Composed against the captured lens, NEVER against a live read.
			//
			// SD writes worldFOV every frame now, so reading it back here would feed
			// the last frame's shot into this frame's solve: a shot that narrows the
			// lens would then solve a shorter standoff for the same fill, narrow again,
			// and creep tighter every frame for as long as it was held. Shots that name
			// their own lens supply it themselves; this is only the fallback for the
			// ones with no opinion.
			subjects.fovDegrees = baseFov > 1.0f ? baseFov : 75.0f;
			subjects.aspect = screenAspect;
			subjects.side = side;
			subjects.openDirection = openDirection;
			subjects.openDistance = openDistance;
			subjects.npc = npcBody;
			subjects.player = playerBody;
			subjects.ceiling = ceilingRoom;
			subjects.enforceLine = enforceLine || true180;  // true180 implies enforceLine
			subjects.true180 = true180;
			subjects.avoidCrowds = avoidCrowds;

			// The two participants, so the crowd probe can tell them from bystanders.
			// Everyone else is fair game, followers included — a follower standing at
			// your elbow is the single body most often in shot.
			subjects.npcId = speakerPtr->GetFormID();
			subjects.playerId = player->GetFormID();
			subjects.progress = 0.0f;  // candidates are judged at the moment they open
			subjects.delta = delta;
			subjects.protectSubject = protectSubject;
			subjects.cropFractionPerEdge = letterboxWanted ?
				static_cast<float>(std::clamp(tunables.letterboxHeight, 0, 300)) / 1000.0f : 0.0f;
			if (protectSubject) {
				subjects.npcSight = MeasureSightTarget(speakerPtr.get(), npcAnchor.position, npcBody.scale);
				subjects.playerSight = MeasureSightTarget(player, playerAnchor.position, playerBody.scale);
			}
			return subjects;
		}

		[[nodiscard]] double VisibilityTime()
		{
			return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
		}

		[[nodiscard]] float SeparationSquared(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const auto d = a - b;
			return d.x * d.x + d.y * d.y + d.z * d.z;
		}

		// Legacy placement recovery uses the same named setups and settings as
		// normal selection. It cannot manufacture an unlisted emergency single.
		[[nodiscard]] std::optional<std::pair<ShotType, Pose>> EnabledFallback(Subjects subjects)
		{
			subjects.heldSweep = kUnheld;
			subjects.heldStandoff = kUnheld;
			subjects.heldRoom = kUnheld;
			subjects.holdPlacement = false;
			subjects.progress = 0.0f;
			std::array<Pose, kAllShots.size()> poses{};
			const auto selected = BestAvailable(std::span{ kAllShots }, Drawable, [&](ShotType type) {
				auto& pose = poses[static_cast<std::size_t>(type)];
				pose = Solve(type, subjects);
				if (!pose.valid) {
					return -1.0f;
				}
				const bool preferred = FramingIsRoom() ? IsNeutral(type) :
					(!IsNeutral(type) && FavoursNpc(type) == SubjectIsNpc());
				return pose.quality + (preferred ? 2.0f : (IsNeutral(type) ? 1.0f : 0.0f));
			});
			if (!selected) {
				return std::nullopt;
			}
			return std::pair{ *selected, poses[static_cast<std::size_t>(*selected)] };
		}

		void UseNativeView()
		{
			if (!nativeView) {
				RestoreFieldOfView();
				RestoreCameraRest();
				nativeView = true;
			}
		}

		[[nodiscard]] bool Visible(const Pose& pose)
		{
			return pose.valid && pose.visibility.state == SightState::kClear &&
				pose.lensClearance == SightState::kClear;
		}

		[[nodiscard]] bool ProtectedSubject(ShotType type)
		{
			if (!Drawable(type)) {
				return false;
			}
			if (FramingIsRoom()) {
				return IsNeutral(type);
			}
			if (IsNeutral(type)) {
				return framing == Framing::kAuto && SubjectIsNpc();
			}
			return FavoursNpc(type) == SubjectIsNpc();
		}

		void StartProtectedSearch(bool recovery)
		{
			protectedSearch = {};
			protectedSearch.active = true;
			protectedSearch.recovery = recovery;
			protectedSearch.turn = turnSerial;
			protectedSearch.framingAtStart = framing;
			std::array<bool, static_cast<std::size_t>(ShotType::kCount)> added{};
			const auto add = [&](ShotType type) {
				const auto i = static_cast<std::size_t>(type);
				if (i < added.size() && !added[i] && ProtectedSubject(type) &&
					(recovery || type != currentShot)) {
					added[i] = true;
					protectedSearch.order[protectedSearch.count++] = type;
				}
			};
			if (recovery) {
				add(currentShot);
			}
			for (int i = 0; i < 4; ++i) {
				if (const auto choice = Choose()) {
					add(*choice);
				}
			}
			// A weighted permutation covers every remaining allowed setup once.
			// Room heuristics cannot prove that a narrow, face-visible view fails.
			for (;;) {
				std::uint32_t total = 0;
				for (std::size_t i = 0; i < added.size(); ++i) {
					const auto type = static_cast<ShotType>(i);
					if (!added[i] && ProtectedSubject(type) && (recovery || type != currentShot)) {
						total += static_cast<std::uint32_t>(std::max(Shot::Weight(type), 1));
					}
				}
				if (total == 0) {
					break;
				}
				auto draw = NextRandom() % total;
				for (std::size_t i = 0; i < added.size(); ++i) {
					const auto type = static_cast<ShotType>(i);
					if (added[i] || !ProtectedSubject(type) || (!recovery && type == currentShot)) {
						continue;
					}
					const auto weight = static_cast<std::uint32_t>(std::max(Shot::Weight(type), 1));
					if (draw < weight) {
						add(type);
						break;
					}
					draw -= weight;
				}
			}
		}

		void AttachSight(Subjects& subjects, SightContext& context)
		{
			context = BuildSightContext();
			subjects.sightContext = &context;
			subjects.protectSubject = true;
			subjects.checkVisibility = true;
		}

		// At most two complete angle sweeps per update. Candidates carried across
		// updates are checked against today's subjects and actors before use.
		[[nodiscard]] std::optional<std::pair<ShotType, Pose>> SearchProtected(Subjects subjects)
		{
			if (!protectedSearch.active) {
				return std::nullopt;
			}
			if (protectedSearch.turn != turnSerial || protectedSearch.framingAtStart != framing) {
				StartProtectedSearch(protectedSearch.recovery);
			}
			subjects.heldSweep = kUnheld;
			subjects.heldStandoff = kUnheld;
			subjects.heldRoom = kUnheld;
			subjects.holdPlacement = false;
			subjects.progress = 0.0f;
			subjects.requireFullFace = viewMode == ViewMode::kFirstPersonFallback;
			if (protectedSearch.best.valid) {
				CheckVisibility(protectedSearch.bestType, protectedSearch.best, subjects);
				if (!Visible(protectedSearch.best) || !ProtectedSubject(protectedSearch.bestType)) {
					protectedSearch.best = {};
				}
			}
			for (int budget = 0; budget < 2 && protectedSearch.cursor < protectedSearch.count; ++budget) {
				const auto type = protectedSearch.order[protectedSearch.cursor++];
				if (!ProtectedSubject(type)) {
					continue;
				}
				const auto pose = Solve(type, subjects);
				const bool recoveryClear = viewMode != ViewMode::kFirstPersonFallback || pose.visibility.face >= 0.99f;
				if (Visible(pose) && recoveryClear && (!protectedSearch.best.valid || pose.quality > protectedSearch.best.quality)) {
					protectedSearch.best = pose;
					protectedSearch.bestType = type;
				}
				if (protectedSearch.best.valid && protectedSearch.best.quality >= kGoodEnough) {
					break;
				}
			}
			const bool finished = protectedSearch.cursor >= protectedSearch.count;
			if (protectedSearch.best.valid && (finished || protectedSearch.cursor >= 6 ||
				protectedSearch.best.quality >= kGoodEnough)) {
				protectedSearch.active = false;
				return std::pair{ protectedSearch.bestType, protectedSearch.best };
			}
			if (finished) {
				protectedSearch.active = false;
				protectedRetryAt = Clock::now();
			}
			return std::nullopt;
		}

		bool CommitProtected(ShotType type, const Pose& pose)
		{
			if (!ProtectedSubject(type) || !Visible(pose)) {
				return false;
			}
			Log::Info(Log::Category::kContinuity,
				"Visible shot: {} -> {} (face {:.0f}%, quality {:.2f}, adjustment {:+.1f}deg)."sv,
				Name(currentShot), Name(type), pose.visibility.face * 100.0f, pose.quality, pose.sweep);
			previousShot = currentShot;
			currentShot = type;
			shotSince = Clock::now();
			heldSince = shotSince;
			heldSweep = pose.sweep;
			heldStandoff = pose.standoff;
			heldRoom = pose.room;
			protectedPose = pose;
			protectedShot = type;
			visibilityCheckedAt = {};
			visibilityRecovery.Reset();
			visibilityFailedObservation = false;
			protectedSearch = {};
			cueSinceCut = false;
			turnSinceCut = false;
			forcedCut = false;
			linesSinceCut = 0;
			cutEveryTarget = RollCutEvery();
			return true;
		}

		[[nodiscard]] std::optional<Pose> ProtectedFrame(Subjects subjects, bool ordinaryCut)
		{
			if (viewMode != ViewMode::kCinematic || fallbackRequested) {
				return std::nullopt;
			}
			if (!Drawable(currentShot)) {
				protectedPose = {};
			}
			const bool hadPose = ReusableShot(protectedPose.valid, protectedShot, currentShot, Drawable);
			Pose pose{};
			if (hadPose) {
				subjects.heldSweep = heldSweep;
				subjects.heldStandoff = heldStandoff;
				subjects.heldRoom = heldRoom;
				subjects.holdPlacement = heldRoom > kUnheld;
				subjects.progress = std::clamp(SecondsSince(shotSince) /
					std::max(static_cast<float>(Shot::MoveTime(currentShot)) / 100.0f, 0.1f), 0.0f, 1.0f);
				subjects.checkVisibility = false;
				pose = Solve(currentShot, subjects);
			}
			const bool moved = SeparationSquared(subjects.npcSight.head, checkedNpc) > 64.0f ||
				SeparationSquared(subjects.playerSight.head, checkedPlayer) > 64.0f ||
				SeparationSquared(pose.position, checkedCamera) > 64.0f || std::abs(pose.lens - checkedLens) > 1.0f;
			const bool check = !hadPose || !pose.valid || moved || SecondsSince(visibilityCheckedAt) >= 0.10f;
			SightContext context{};
			if (check || ordinaryCut || protectedSearch.active) {
				AttachSight(subjects, context);
			}
			bool recovery = !hadPose;
			if (check && hadPose) {
				CheckVisibility(currentShot, pose, subjects);
				visibilityCheckedAt = Clock::now();
				checkedNpc = subjects.npcSight.head;
				checkedPlayer = subjects.playerSight.head;
				checkedCamera = pose.position;
				checkedLens = pose.lens;
				const bool clear = Visible(pose);
				visibilityFailedObservation = !clear;
				recovery = visibilityRecovery.Observe(VisibilityTime(), clear,
					pose.visibility.severe || pose.lensClearance == SightState::kBlocked,
					pose.visibility.state == SightState::kUnknown, hadPose);
				if (clear) {
					protectedPose = pose;
				} else {
					// Bounded grace keeps a brief crossing from causing a cut. This is
					// the last verified absolute pose, never an unchecked actor offset.
					pose = protectedPose;
				}
			} else if (hadPose && pose.valid) {
				// Reuse only the observation; the next check follows the moved lens.
				pose.visibility = protectedPose.visibility;
				pose.lensClearance = protectedPose.lensClearance;
				if (visibilityFailedObservation) {
					pose = protectedPose;
				}
			}
			if (recovery && protectedSearch.active && !protectedSearch.recovery) {
				StartProtectedSearch(true);
			}
			if (!protectedSearch.active && (recovery || ordinaryCut) &&
				(SecondsSince(protectedRetryAt) >= 0.5f || ordinaryCut || !hadPose)) {
				StartProtectedSearch(recovery);
				forcedCut = false;
				cueSinceCut = false;
				turnSinceCut = false;
			}
			if (protectedSearch.active) {
				if (!subjects.sightContext) {
					AttachSight(subjects, context);
				}
				if (const auto candidate = SearchProtected(subjects)) {
					if (CommitProtected(candidate->first, candidate->second)) {
						return candidate->second;
					}
				}
			}
			if (recovery && firstPersonFallback) {
				fallbackRequested = true;
				return std::nullopt;
			}
			return hadPose ? std::optional{ pose.valid ? pose : protectedPose } : std::nullopt;
		}

		void TickProtected(const std::optional<Subjects>& subjects)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || Dialogue::MenuWatch::ScreenTaken()) {
				return;
			}
			if (fallbackRequested) {
				fallbackRequested = false;
				if (protectSubject && firstPersonFallback && camera->IsInThirdPerson() &&
					Compat::SmoothCam::Holding()) {
					RestoreFieldOfView();
					RestoreCameraRest();
					viewMode = ViewMode::kFirstPersonFallback;
					fallbackThirdPersonOwed = !returnToFirstPerson;
					visibilityRecovery.EnterFallback(VisibilityTime());
					fallbackTurn = turnSerial;
					fallbackCue = cueCount;
					StartProtectedSearch(true);
					camera->ForceFirstPerson();
					Compat::SmoothCam::Release();
					Log::Info(Log::Category::kCamera,
						"Subject visibility lost; first-person coverage while dialogue continues."sv);
				}
			}
			if (viewMode != ViewMode::kFirstPersonFallback) {
				return;
			}
			// A player/engine state change owns its view. Never force it every tick.
			if (!camera->IsInFirstPerson()) {
				(void)visibilityRecovery.Ready(VisibilityTime(), false, false);
				return;
			}
			if (!protectSubject || !firstPersonFallback) {
				if (Compat::SmoothCam::Acquire()) {
					viewMode = ViewMode::kCinematic;
					fallbackThirdPersonOwed = false;
					camera->ForceThirdPerson();
					protectedPose = {};
					protectedSearch = {};
				}
				return;
			}
			if (!subjects) {
				(void)visibilityRecovery.Ready(VisibilityTime(), false, false);
				return;
			}
			if (SecondsSince(visibilityCheckedAt) < 0.10f) {
				return;
			}
			if (SecondsSince(visibilityCheckedAt) > 0.30f) {
				(void)visibilityRecovery.Ready(VisibilityTime(), false, false);
			}
			visibilityCheckedAt = Clock::now();
			auto fresh = *subjects;
			SightContext context{};
			AttachSight(fresh, context);
			if (!protectedSearch.active && !protectedSearch.best.valid && SecondsSince(protectedRetryAt) >= 0.5f) {
				StartProtectedSearch(true);
			}
			if (protectedSearch.active) {
				(void)SearchProtected(fresh);
			}
			if (protectedSearch.best.valid) {
				CheckVisibility(protectedSearch.bestType, protectedSearch.best, fresh);
				if (!Visible(protectedSearch.best) || protectedSearch.best.visibility.face < 0.99f ||
					!ProtectedSubject(protectedSearch.bestType)) {
					protectedSearch.best = {};
				}
			}
			const bool clear = Visible(protectedSearch.best) && protectedSearch.best.visibility.face >= 0.99f;
			const bool boundary = !Dialogue::Session::GetSingleton().Speaking() || forcedCut ||
				turnSerial != fallbackTurn || cueCount != fallbackCue;
			if (visibilityRecovery.Ready(VisibilityTime(), clear, boundary) && Compat::SmoothCam::Acquire()) {
				const auto selected = protectedSearch.bestType;
				const auto pose = protectedSearch.best;
				if (!CommitProtected(selected, pose)) {
					Compat::SmoothCam::Release();
					return;
				}
				viewMode = ViewMode::kCinematic;
				fallbackThirdPersonOwed = false;
				camera->ForceThirdPerson();
				Log::Info(Log::Category::kCamera, "Subject clear; cinematic coverage resumed."sv);
			}
			fallbackTurn = turnSerial;
			fallbackCue = cueCount;
		}

	}

	void Director::Open(RE::Actor* a_speaker, bool a_restaging)
	{
		if (!a_speaker) {
			return;
		}
		const auto openingStarted = Clock::now();
		const Config::ReadScope settings;

		// IS THIS A RESUME, OR A NEW CONVERSATION THAT HAPPENS TO FOLLOW ONE?
		//
		// Asked first, and only ONCE per suspension: whichever answer it gives, the
		// suspension is spent by the end of this function. A resume comes back to
		// the shot it left and re-owes the first-person view; anything else is an
		// ordinary open, and the suspension is dropped with a line saying so rather
		// than left armed to surprise a later conversation.
		// BOTH HALVES OF THE KEY, because an actor is not a conversation.
		//
		// This compared the form id alone, while the comment above the suspension
		// fields claimed it recorded "which conversation this was". Runtime already
		// learned that lesson — it keys on (partner, serial) precisely because two
		// consecutive conversations with one person share an id — and the
		// suspension path had quietly reintroduced the ambiguity it was fixed for.
		//
		// Left as it was, a session that ends and restarts with the same actor
		// while the screen is owned comes back as a "resume": it would inherit the
		// previous conversation's angle, skip the settings re-read that a genuine
		// new conversation gets, and spend the first-person debt under an identity
		// that did not earn it.
		const auto openSerial = Dialogue::Session::GetSingleton().ConversationSerial();

		const bool resuming = suspendedForMenu &&
			suspendedPartner == a_speaker->GetFormID() &&
			suspendedSerial == openSerial;

		const ShotType resumeShot = suspendedShot;

		// TAKEN FROM THE SUSPENSION WHETHER OR NOT THIS IS A RESUME.
		//
		// A first-person player whose conversation was suspended is standing in
		// third person because this mod put them there, and the record of that is
		// the only thing that knows. If the screen comes back into a DIFFERENT
		// conversation — a new partner, or the same one on a new serial — the old
		// code dropped the record and the new open could not rediscover it: its
		// IsInFirstPerson test is false, because the player is already in third.
		// The debt evaporated and they stayed in third person for good.
		//
		// The file already carries exactly this debt across the equivalent seam for
		// a direct partner change, a few lines down. This is the same seam with a
		// menu in the middle of it.
		const bool suspensionOwesFirstPerson = suspendedForMenu && suspendedFirstPersonOwed;
		const bool resumeFallback = resuming && suspendedInFallback;
		const bool suspensionOwesThirdPerson = suspendedForMenu && suspendedThirdPersonOwed;

		if (suspendedForMenu && !resuming) {
			Log::Info(Log::Category::kCamera,
				"Cinematic resume skipped: [{:08X}] was suspended, but this is a new "
				"conversation with [{:08X}]."sv,
				suspendedPartner, a_speaker->GetFormID());
		}

		// SPENT ONLY ONCE THE CAMERA IS ACTUALLY IN HAND — see the Acquire below.
		//
		// This used to be cleared here, on the reasoning that no path out of the
		// function should leave it armed. The path it did not consider is the one
		// that fails: SmoothCam can REFUSE, Open returns early, and by then the
		// suspension had already been thrown away. The first-person debt lived on
		// only in a local, so a player who talks to people in first person was left
		// standing in third with nothing left that knew they were owed anything —
		// and Runtime records the conversation as opened either way, so nothing
		// tried again.
		//
		// That refusal is not hypothetical now. Improved Camera asks SmoothCam for
		// the same camera, so a load order with both has a real path to it.
		//
		// The stale case the old comment was actually about is handled here instead,
		// where it belongs: a suspension that turns out to belong to a DIFFERENT
		// conversation is dropped immediately, because nothing below can use it.
		if (suspendedForMenu && !resuming) {
			suspendedForMenu = false;
			suspendedPartner = 0;
			suspendedSerial = 0;
			suspendedFirstPersonOwed = false;
		}

		// ALREADY STAGING MEANS THE PLAYER WALKED STRAIGHT INTO A SECOND
		// CONVERSATION, and this used to be a silent no-op.
		//
		// Session restarts itself inside a single frame on a partner change —
		// Exit() then Enter(), both before Runtime looks again — so Active() is
		// never observed false and Runtime's close branch never runs. Runtime calls
		// Open() for the new partner instead, that call met a `staging` guard here,
		// returned having done nothing, and Runtime then set openedFor and believed
		// it had opened.
		//
		// Every per-conversation field therefore stayed on the person who had been
		// left behind. `subject` kept the camera staging on them, and
		// playerHasChosen stayed true — so `fading` was armed on the opening frame,
		// alpha started at 0, and with the new NPC greeting the camera favours them
		// and the return branch is held off. The topic list stayed hidden through
		// the whole greeting instead of being up with it, which is the reported
		// "the dialogue menu doesn't pop up when you start a conversation".
		//
		// Closed and reopened rather than patched up in place, so the reset block
		// below stays the single definition of what a conversation starts as. A
		// second, partial reset alongside it is how this class of bug gets rewritten
		// instead of fixed.
		// Seeded from the suspension: a menu that took the screen recorded the debt
		// instead of paying it, and this is where it comes back.
		// WHICH CONVERSATION THIS IS, and it is not the same question as who it is
		// with. See Session::ConversationSerial. Read once at the top of this
		// function, because the resume decision needs the same answer.
		const auto sessionSerial = openSerial;

		bool firstPersonOwed = suspensionOwesFirstPerson;
		if (staging) {
			// A REPEAT CALL FOR THE CONVERSATION ALREADY ON SCREEN. Reopening would
			// restart the shot for no reason.
			//
			// SAME ACTOR IS NOT ENOUGH, and that is the half this used to get wrong.
			// Leaving a conversation while the NPC is still talking and immediately
			// walking back into one is the same actor and a genuinely new
			// conversation — Session ends the old one and starts a new one, Runtime
			// asks for a stage, and this guard threw it away because the form id
			// had not changed. Everything per-conversation then stayed on the
			// exchange that had already finished.
			if (subject.get().get() == a_speaker && sessionSerial == stagedSerial) {
				return;
			}

			// Held across the seam. Close hands first person back and the block
			// further down would take it again on the same frame — one frame of the
			// player's own eyes, for anyone who talks to people in first person.
			firstPersonOwed = firstPersonOwed || returnToFirstPerson;
			returnToFirstPerson = false;
			Close();
		}

		stagedSerial = sessionSerial;

		if (!resumeFallback && !Compat::SmoothCam::Acquire()) {
			// THE SUSPENSION SURVIVES A REFUSAL, and that is the whole point of not
			// having spent it above.
			//
			// Declining to stage is correct — another plugin holds the camera and
			// fighting it is worse than doing nothing. What is not correct is losing
			// the player's first-person view along with the cinematic. Left armed,
			// the debt is still owed to somebody: Runtime calls AbandonSuspension
			// when the conversation ends, and that pays it.
			if (resuming) {
				Log::Warn(Log::Category::kCamera,
					"Resume declined: the camera is held by another plugin. The suspension for "
					"[{:08X}] stays armed so the view is still handed back when this "
					"conversation ends."sv,
					suspendedPartner);
			}
			return;
		}

		const bool keepThirdPerson = suspensionOwesThirdPerson || restoreThirdPersonPending;
		if (!resumeFallback) {
			// This newly acquired conversation now owns the final restoration.
			// A deferred debt from the preceding scene must not change its view later.
			restoreThirdPersonPending = false;
			handBackPending = false;
		}

		// The conversation is handled, so the suspension has been honoured and is spent.
		// Everything the resume needed was copied into locals above.
		suspendedForMenu = false;
		suspendedPartner = 0;
		suspendedSerial = 0;
		suspendedFirstPersonOwed = false;

		subject = a_speaker->GetHandle();
		viewMode = resumeFallback ? ViewMode::kFirstPersonFallback : ViewMode::kCinematic;
		fallbackThirdPersonOwed = resumeFallback && suspensionOwesThirdPerson;
		fallbackRequested = false;
		protectedPose = {};
		protectedSearch = {};
		frameSubjects.reset();
		visibilityCheckedAt = {};
		protectedRetryAt = {};
		visibilityRecovery.Reset();
		visibilityFailedObservation = false;
		suspendedInFallback = false;
		suspendedThirdPersonOwed = false;
		stagingSince = Clock::now();
		shotSince = stagingSince;
		currentShot = ShotType::kTwoShot;
		previousShot = ShotType::kTwoShot;
		haveShot = false;

		// The eyeline belongs to the conversation, so a new conversation gets to
		// pick freely. Left set, the first ChooseSide of the next scene would find
		// itself already "committed" to whichever side the last one ended on and
		// need a 190-unit win to move — carrying one room's geography into the next.
		sideCommitted = false;
		ceilingRoom = 0.0f;
		roomSpace = Space::kRoom;

		// The hotkey overrides are scoped to ONE conversation and reset here.
		//
		// A framing forced on the guard at the gate has nothing to say about the
		// jarl you walk in to see next, and a camera handed back because one scene
		// was being filmed badly must not stay handed back silently for every scene
		// after it. The settings are where a lasting preference lives; these are
		// the thing you reach for once and forget you touched.
		framing = Framing::kAuto;
		forcedCut = false;
		requestCut.store(false, std::memory_order_relaxed);
		requestFraming.store(false, std::memory_order_relaxed);

		// Both reset together or the first override of a new conversation expires
		// on the frame it is set: a stale serial from the last scene will not match
		// whatever this one starts counting from.
		turnSerial = 0;
		framingUntilTurn = 0;
		playerVoiceHandoff.Reset(Scene::LipSync::PlayerLineSerial());
		reactionShots.Reset();
		replyBoundary.Reset();
		lastPickPhase = Scene::Interface::MenuPhase::kUnknown;
		lastPlayerLineSerial = Scene::LipSync::PlayerLineSerial();

		npcSpeaking = false;
		wasSpeaking = false;
		cueIntensity = 50;
		lastLookApplied = -1;
		releasePending = false;
		cueSinceCut = false;
		turnSinceCut = false;

		// The hold on the spent topic list is per conversation. A line left open by
		// a menu close, a load, or a voice mod that never announced its end must
		// not pin the next conversation's fade before it has started.
		playerLineHeld = false;
		playerLineHeldSince = stagingSince;
		playerLineHoldExpired.Reset();

		// Reset with everything else. Left stale from the previous conversation,
		// the debounce believed a turn was already mid-flight and fired a spurious
		// change on the first frame.
		pendingSpeaking = false;
		pendingSince = stagingSince;
		turnBeganAt = stagingSince;
		linesThisTurn = 0;

		// Per conversation, not per session. Walking from one NPC to the next has to
		// watch the engine declare its list live again before anything is allowed to
		// hide it — the approach is where the blind clicks live.
		listWasLive = false;
		listWanted = true;
		listEdgeAt = stagingSince;
		choiceAlpha = 100.0f;
		choiceEaseFrom = 100.0f;

		// A handle retired by the last conversation must not silence the first line
		// of this one, and ids are reused.
		voiceHandleID = RE::BSSoundHandle::kInvalidID;
		retiredVoiceID = RE::BSSoundHandle::kInvalidID;

		lastNpcPose = {};
		lastPlayerPose = {};
		enabledRetryAt = {};
		nativeView = false;

		playerAnchor = {};
		npcAnchor = {};

		// Both of these carried over from the previous conversation. A measurement
		// taken off a seated blacksmith should not decide how the next person is
		// framed, and a stale posture makes the first frame look like a change.
		//
		// It matters more now than it did: with creatures in play a stale body is
		// not a slightly wrong eye height, it is a dragon's framing applied to the
		// chicken you turned around and spoke to.
		playerBody = {};
		npcBody = {};
		posturePrimed = false;

		haveFrameTime = false;
		rngState ^= a_speaker->GetFormID() * 2654435761u;

		// Staging cannot work from inside the player's head, so first person has to
		// go. Until now that was a one-way trip: somebody who plays the whole game
		// in first person came out of every conversation in third and had to put
		// themselves back, every time. Vanilla does not do this — vanilla talks to
		// people with your own eyes — so the third-person view is entirely this
		// mod's doing and handing it back is this mod's job.
		//
		// The flag is only set when Open is what took first person away. Another
		// camera mod that forced third person before this ran owns its own restore,
		// and two mods both deciding they are owed a view change is how a player
		// ends up snapping between them at the end of every conversation.
		returnToFirstPerson = false;
		if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInFirstPerson()) {
			returnToFirstPerson = !keepThirdPerson && Config::Bool("Direction", "bRestoreFirstPerson", true);
			if (!resumeFallback) {
				camera->ForceThirdPerson();
			}
		}

		// Carried over a partner change. The camera is already in third person
		// because the conversation just left was holding it there, so the check
		// above cannot see that the player is still owed first person — and without
		// this, walking from one NPC to the next quietly cancels the restore for the
		// rest of the exchange.
		if (firstPersonOwed) {
			returnToFirstPerson = true;
		}

		// Fix the lens this conversation composes against.
		//
		// If no idle frame has been sampled yet — a conversation opened before the
		// tracker ever ran — fall back on the camera, but not blindly: a reading
		// under the floor is the stuck state rather than a preference, and taking
		// it would re-latch it. 75 is wrong for somebody who plays wider, but it is
		// wrong for one conversation instead of for the session.
		if (restingFov < 1.0f) {
			auto*       cam = RE::PlayerCamera::GetSingleton();
			const float reading = cam ? cam->GetRuntimeData2().worldFOV : 0.0f;
			if (reading >= kSaneMinFov) {
				restingFov = reading;
			} else {
				restingFov = 75.0f;
				Log::Warn(Log::Category::kCamera,
					"No idle field-of-view sample yet and the camera reads {:.1f}, under the {:.0f} "
					"floor. Using 75 for this conversation."sv,
					reading, kSaneMinFov);
			}
		}
		baseFov = restingFov;

		staging = true;

		// letterboxWanted rather than true: the bars are a setting now, and one the
		// player can have switched off. ReadTuning below refreshes it from the ini
		// for the next conversation; this one uses whatever is currently live,
		// which is what the menu last set.
		Render::Letterbox::SetVisible(letterboxWanted);

		// Headtracking is the one thing this mod does to the player that a
		// conventional offset camera does not, which makes it the first thing to
		// rule out when something about the player's animation misbehaves and
		// swapping to Alternate Conversation Camera "fixes" it. It writes a
		// behaviour-graph variable and a dialogue headtrack target on an actor the
		// engine never normally treats as a dialogue participant.
		//
		// Off means genuinely untouched: no graph variable, no headtrack target,
		// nothing to restore. Presence::Release stays unconditional because it
		// no-ops unless Engage actually ran.
		if (Config::Bool("Performance", "bHeadtracking", true)) {
			Scene::Presence::Engage(a_speaker);
		} else {
			Log::Info(Log::Category::kStaging,
				"Headtracking disabled by ini; the player's animation graph is not touched."sv);
		}
		// bHoldPlayerFace is retired: the player's head keeps whatever draw flag
		// the game gives it. Expressions use their own independent toggle.
		Scene::Performance::HoldPlayerFace(false);
		// Configure profiles once, before Engage consumes them. ReadTuning also
		// configures performance; a resume keeps its existing shot settings.
		if (!resuming && !a_restaging) {
			ReadTuning();
		} else {
			Scene::Performance::Configure(Config::Bool("Performance", "bExpressions", true), false);
		}
		Scene::Performance::Engage(a_speaker);
		Scene::FaceGen::Begin(a_speaker);
		Scene::LipSync::Engage();

		// AFTER ReadTuning, not before it. bHideSpeakerName is still a tunable and
		// Suppress reads it on its way through, so suppressing first would hide the
		// name — or not — under the PREVIOUS conversation's answer, and an edit made
		// between two conversations would appear to take two to land.
		Scene::Interface::Suppress();

		// FOUR SETTINGS AND A NUDGE. That is the whole of the lighting surface.
		//
		// It was two hundred and fifty keys — ten rigs at three lamps at seven
		// values, plus a table of what every emotion did to them. All of it worked
		// and none of it was usable, because the feature is "faces stop looking
		// flat" and every control past that point is one more reason not to turn it
		// on. See LightRig.h.
		Scene::KeyLight::Configure(
			Config::Bool("Lighting", "bLights", false),
			Config::Int("Lighting", "iBrightness", 100),
			Config::Int("Lighting", "iRed", 255),
			Config::Int("Lighting", "iGreen", 246),
			Config::Int("Lighting", "iBlue", 234),
			Config::Bool("Lighting", "bShadows", false),
			Config::Int("Lighting", "iFadeTime", 18));

		lightPerShot = Config::Bool("Lighting", "bPerShot", false);
		lightOffsetX = Config::Int("Lighting", "iOffsetX", 0);
		lightOffsetY = Config::Int("Lighting", "iOffsetY", 0);
		lightOffsetZ = Config::Int("Lighting", "iOffsetZ", 0);

		// Forced back to "nothing has been applied" so the first cut of a new
		// conversation always sets its look. Without it the guard would see the
		// same value the last conversation ended on and skip the call, leaving the
		// lamps dark for a whole scene.
		lastLookApplied = -1;

		Scene::KeyLight::Engage();

		// WITH PER-ANGLE LIGHTING OFF — which is how it ships — the look is set
		// exactly once here and never touched again.
		//
		// The render path only re-sets it when the player has asked for a different
		// look per angle, so the common case costs one call per conversation rather
		// than a comparison every frame.
		if (!lightPerShot) {
			const int look = Scene::FindLook(Config::String("Lighting", "sLook", "natural"));
			Scene::KeyLight::SetLook(look >= 0 ? look : Scene::DefaultLook());
			Scene::KeyLight::SetOffset(lightOffsetX, lightOffsetY, lightOffsetZ);
			lastLookApplied = look;
		}

		Scene::Focus::Configure(
			Config::Bool("Lighting", "bDepthOfField", false),
			Config::Int("Lighting", "iBlur", 55) / 100.0f);
		Scene::Focus::Engage();

		// The player beat moved into ReadTuning above with the rest of the dials â€”
		// a player-voice mod changes the rhythm, but that is a default, not a
		// separate mechanism, and having it read in two places meant a menu edit
		// was silently overwritten here on the next conversation.
		Log::Info(Log::Category::kCamera, "Staging opened on {}{}."sv,
			a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>",
			poseMode == 0 ? ""sv : " [iPoseMode NOT 0 â€” diagnostic camera write in force]"sv);

		// Logged per conversation, not once at startup.
		//
		// The mode is changeable from the menu between conversations, so a single
		// startup line would be wrong for every conversation after the first. That
		// is not hypothetical: the log from the session that found this bug could
		// not say which mode any given conversation ran under, and the answer had
		// to be asked for rather than read.
		// Unconditional, including mode 0.
		//
		// This used to fire only when the mode was non-zero, so a mode-0 run
		// produced no line at all and "no mode line" meant either mode 0 or an ini
		// that never loaded. LIPSYNC.md Â§1's protocol switches modes five times in
		// one session and scores three of those runs at mode 0 â€” every one of
		// which would have been silent. A diagnostic that omits the control is not
		// a diagnostic.
		Log::Info(Log::Category::kCamera, "iPoseMode={} ({}){}"sv,
			poseMode, PoseModeName(poseMode),
			poseMode == 1 || poseMode == 2 ?
				" â€” CAMERA WILL NOT MOVE; a lipsync result here is not a fix."sv :
				""sv);

		// Replay the greeting that beat us here.
		//
		// Last in Open() on purpose: ApplyCue reads holdOnShortLines and
		// shortLineWords, and ReadTuning above is what makes them current. Run
		// before it and the first line of every conversation would be judged
		// against the previous conversation's dials.
		//
		// Both tests matter. The clock rejects a cue left over from a conversation
		// that never opened; the identity check rejects the far more likely case,
		// since LineWatch hooks every Character in the cell and a guard muttering
		// nearby lands in the same slot.
		if (pendingCue.valid) {
			const auto stashed = pendingCue.speaker.get();
			const bool fresh = SecondsSince(pendingCue.at) <= kGreetingGrace;
			const bool sameSpeaker = stashed && stashed.get() == a_speaker;

			if (fresh && sameSpeaker) {
				Log::Info(Log::Category::kDialogue,
					"Greeting of {} word(s) arrived {:.0f}ms before staging; applying it now."sv,
					pendingCue.words, SecondsSince(pendingCue.at) * 1000.0f);
				ApplyCue(a_speaker, pendingCue.words, pendingCue.intensity, pendingCue.emotion, pendingCue.text);
			}

			pendingCue = {};
		}

		// OPEN ON WHOEVER IS SPEAKING. THERE IS NO ESTABLISHING SHOT ANY MORE.
		//
		// The two-shot used to hold the top of every conversation for iEstablishTime
		// and it is gone entirely — not defaulted off, removed. It was a shot of two
		// people standing apart, played over the start of a line, and its only
		// honest use was the silent opening, which is rare. Every other opening it
		// cost an angle and a second and a half before the mod did the one thing it
		// exists to do.
		//
		// So the answer is the same one coverage gives everywhere else: the camera
		// belongs on whoever is talking. If nobody is, it belongs on the player,
		// because there is no third party for it to be about.
		//
		// A RESUME ASKS THE SESSION, BECAUSE NO CUE IS COMING FOR A LINE THAT NEVER
		// STOPPED.
		//
		// npcSpeaking is reset to false at the top of Open and put back only by a
		// replayed cue. That is right for a conversation starting, and wrong for one
		// resuming, because LineWatch deliberately does not re-announce a response
		// that is still the active one:
		//
		//     if (a_response == activeResponse) { ++activeCalls; return; }
		//
		// So an NPC line that began before the container was opened and is still
		// running when it closes produces no cue at all. Open would conclude nobody
		// is speaking, pick a player-facing opener, and — because the stale-shot
		// test below compares against exactly this flag — throw away the NPC angle
		// the resume exists to preserve. Worse, npcSpeaking is also what Tick's
		// coverage rule reads for who the camera belongs on, so the mistake would
		// outlive Open and cut the camera off the speaker a quarter second later.
		//
		// Session::Speaking is the authoritative live answer and is not derived from
		// the cue edge: it is set when a line starts and cleared when it ends. Asked
		// rather than carried across the suspension on purpose — the line may
		// genuinely have finished while the menu was open, and a snapshot taken on
		// the way in would insist it had not.
		if (resuming) {
			npcSpeaking = Dialogue::Session::GetSingleton().Speaking();
		}

		// AFTER THE CUE REPLAY, because that is what sets npcSpeaking. Before it,
		// nobody is talking yet and a greeting would open on the player.
		{
			// Weighted like every other draw, rather than eight uniform tries.
			//
			// The retry loop ignored the weight entirely and could still come up
			// empty, so the first shot of a conversation — the one the player is
			// guaranteed to see — was the one place a slider was silently overruled.
			// Falling back to Canonical() when nothing in the opener set is drawable
			// is the same answer the loop gave, arrived at without the coin flips;
			// Canonical already answers for the correct side of the exchange.
			const auto pool = npcSpeaking ?
				std::span<const ShotType>{ kOpeners } :
				std::span<const ShotType>{ kPlayerOpeners };

			const auto opening = WeightedPick(pool, Drawable);
			const auto opener = (opening ? opening : Canonical()).value_or(ShotType::kCount);

			currentShot = opener;
			previousShot = opener;
			shotSince = Clock::now();

			// The opening shot IS the first cut, so nothing is owed one.
			//
			// Without this the greeting's own turn edge is still pending and fires
			// a beat later â€” landing the camera on a second angle of the same
			// person, mid-hello. That is the third setup this option exists to
			// remove, just relocated: the two-shot would be gone and the count
			// would still be three.
			turnSinceCut = false;
			cueSinceCut = false;
			linesSinceCut = 0;

			if (opener != ShotType::kCount) {
				Log::Info(Log::Category::kContinuity,
					"Opening on {}: {}."sv, npcSpeaking ? "the speaker"sv : "you (nobody speaking)"sv, Name(opener));
			} else {
				Log::Info(Log::Category::kContinuity, "No enabled opening shot; using camera fallback."sv);
			}
		}

		// A RESUME IS NOT AN OPENING, AND MUST NOT LOOK LIKE ONE.
		//
		// Everything above has just re-primed the conversation from scratch, which
		// is right — Open is the single definition of what a staged conversation
		// starts as, and a partial re-prime alongside it is how this file's worst
		// bugs were written. What is wrong for a resume is the last step of it: a
		// fresh opener draw. Coming back from a merchant's inventory onto a
		// different angle reads as a second conversation beginning, and it is the
		// "smoothly returns to the cinematic" half of the reported regression.
		//
		// So the angle, and only the angle, is put back — WHEN IT IS STILL AN ANGLE
		// ON THE RIGHT PERSON.
		//
		// That qualifier is a bug fix, and the paragraph that used to sit here
		// claimed the opposite: that restarting the shot clock gave the restored
		// setup its full minimum before anything could cut. It does not, and the
		// reason is thirty lines away in Tick — the floor is
		//
		//     (turnSinceCut || wrongSubject) ? minTurnSeconds : minShotSeconds
		//
		// and a restored shot that favours the wrong person IS wrongSubject. The
		// floor collapses from 2.4s to 0.25s and coverage cuts off it immediately.
		//
		// Measured, closing a container twice in one conversation with Lydia:
		//
		//     Opening on you (nobody speaking): Close Up
		//     Cinematic resumed ... back on Extreme Close Up
		//     Cut: Extreme Close Up (them) -> Close Up (you) after 0.25s
		//
		// Both times, a quarter second apart. The player closes a menu, lands on an
		// angle framing a silent NPC, and is snapped off it before they can read
		// it — which is worse than the fresh opener this was written to avoid,
		// because it is two angles instead of one. Reported as the camera going
		// crazy and picking angles at random, and "random" is exactly right: which
		// pair you get depends on what happened to be on screen when the menu
		// opened.
		//
		// The opener chosen above is already correct for the state the conversation
		// is actually in — it draws from kOpeners or kPlayerOpeners depending on
		// npcSpeaking — so when the stored angle disagrees with it, the opener is
		// simply left alone. Continuity is kept in the case that motivated it, which
		// is closing a merchant's stock while the line is still running: the NPC is
		// speaking, the stored shot favours them, and it goes back untouched.
		//
		// Neutral setups are exempt from the test. A two-shot frames both people, so
		// it cannot favour the wrong one, and it is the correct thing to come back
		// to from either state.
		if (resuming) {
			const bool staleSubject =
				!IsNeutral(resumeShot) && FavoursNpc(resumeShot) != npcSpeaking;

			if (!Drawable(resumeShot)) {
				Log::Info(Log::Category::kCamera, "Stored shot {} is disabled; selecting enabled coverage."sv, Name(resumeShot));
			} else if (staleSubject) {
				Log::Info(Log::Category::kCamera,
					"Cinematic resumed on {} [{:08X}]; kept the opening {} rather than {}, which "
					"frames {} and would have been cut off in {:.2f}s."sv,
					a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>",
					a_speaker->GetFormID(), Name(currentShot), Name(resumeShot),
					FavoursNpc(resumeShot) ? "them"sv : "you"sv, minTurnSeconds);
			} else {
				currentShot = resumeShot;
				previousShot = resumeShot;
				shotSince = Clock::now();

				Log::Info(Log::Category::kCamera,
					"Cinematic resumed on {} [{:08X}], back on {}."sv,
					a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>",
					a_speaker->GetFormID(), Name(currentShot));
			}
		}

		// Last, so it wins over the opening shot chosen above.
		if (openShotHold > 0 && Drawable(ShotType::kClosePlayer)) {
			currentShot = ShotType::kClosePlayer;
			previousShot = currentShot;
			shotSince = Clock::now();
			openShotUntil = Clock::now() + std::chrono::milliseconds(openShotHold);

			turnSinceCut = false;
			cueSinceCut = false;
			linesSinceCut = 0;

			Log::Warn(Log::Category::kContinuity,
				"iOpenShotHold={}ms â€” forcing a frontal close on the PLAYER to open. "
				"Diagnostic for the lipsync latch; 0 restores normal opening."sv,
				openShotHold);
		}
		if (resumeFallback) {
			visibilityRecovery.EnterFallback(VisibilityTime());
			fallbackTurn = turnSerial;
			fallbackCue = cueCount;
		}
		Log::Info(Log::Category::kCamera, "Conversation setup completed in {:.2f}ms ({})."sv,
			std::chrono::duration<float, std::milli>(Clock::now() - openingStarted).count(),
			resuming ? "resume"sv : a_restaging ? "recovery"sv : "new conversation"sv);
	}

	void Director::LoadSettings()
	{
		ReadTuning();
	}

	Tunables Director::GetTunables()
	{
		return tunables;
	}

	void Director::ApplyTunables(const Tunables& a_tunables)
	{
		tunables = a_tunables;

		const auto seconds = [](int a_hundredths) {
			return static_cast<float>(std::max(0, a_hundredths)) / 100.0f;
		};

		minShotSeconds = seconds(tunables.minShotTime);
		minTurnSeconds = seconds(tunables.minTurnTime);
		playerBeatSeconds = seconds(tunables.playerBeat);
		playerVoiceHoldSeconds = seconds(tunables.playerVoiceHold);
		reactionSettings = { tunables.reactionShots, tunables.reactionEvery, tunables.reactionChance };

		// A ceiling below the floor would make every shot instantly stale, and the
		// camera would then cut on the floor alone â€” which is exactly the
		// channel-hopping the floor was raised to stop. Clamped rather than
		// trusted, because a menu makes this trivially easy to do by accident.
		maxShotSeconds = std::max(seconds(tunables.maxShotTime), minShotSeconds);

		cutEveryMin = static_cast<std::uint32_t>(std::clamp(tunables.cutEveryMin, 1, 20));
		cutEveryMax = static_cast<std::uint32_t>(std::clamp(tunables.cutEveryMax, 1, 20));
		perLineAngleChange = tunables.perLineAngleChange;
		holdOnShortLines = tunables.holdOnShortLines;
		shortLineWords = static_cast<std::uint32_t>(std::clamp(tunables.shortLineWords, 1, 40));
		timedCutsWhileSpeaking = tunables.timedCutsWhileSpeaking;
		timedCutsWhileChoosing = tunables.timedCutsWhileChoosing;
		directing = tunables.enabled;
		coverPlayerTurn = tunables.coverPlayerTurn;
		enforceLine = tunables.enforceLine;
		if (tunables.true180 != true180 && staging) {
			lineRuleChanged.store(true, std::memory_order_relaxed);
		}
		true180 = tunables.true180;
		avoidCrowds = tunables.avoidCrowds;

		// Deliberately not paired with a heldRoom reset.
		//
		// Turning this on mid-conversation takes effect on the next frame, holding
		// whatever the current shot last measured, which is a real measurement of
		// the bearing it is still on. Turning it off takes effect on the next frame
		// too, because the probes simply resume. Clearing heldRoom here would only
		// force one extra measurement of a bearing that is about to be measured
		// every frame anyway, and it would do it from the settings panel's thread.
		holdPlacement = tunables.holdPlacement;
		const bool wantProtection = tunables.protectSubject && !holdPlacement;
		if (wantProtection != protectSubject) {
			protectionSettingsDirty.store(true, std::memory_order_relaxed);
		}
		protectSubject = wantProtection;
		firstPersonFallback = tunables.firstPersonFallback;
		if (tunables.protectSubject && holdPlacement) {
			Log::Warn(Log::Category::kCamera,
				"Keep Subject Visible is inactive: Ignore Obstructions Mid-Shot takes priority. Disable the hold to enable protection."sv);
		}
		tunables.protectSubject = wantProtection;

		fadeTopicList = tunables.fadeTopicList;
		fadeAfterPlayerLine = tunables.fadeAfterPlayerLine;

		// THE SCREEN FURNITURE IS RECORDED HERE AND APPLIED ON THE TICK.
		//
		// Deliberately not applied inline, even though inline is shorter and was
		// what this first did. ApplyTunables is called from the settings panel,
		// which draws from the Present hook — so an inline SetHideSpeakerName would
		// reach into Scaleform from the render path, and could land in the middle of
		// the same frame's DriveInterface. Storing a bool and letting the tick act on
		// it keeps every GFx call in one place, on the thread that already owns them,
		// exactly one frame later.
		if (tunables.hideSpeakerName != wantHideSpeakerName) {
			wantHideSpeakerName = tunables.hideSpeakerName;
			interfaceDirty = true;
		}
		// The live seconds value the fade actually reads, kept in step with the
		// hundredths the menu and the ini speak in. Applied here rather than only
		// in ReadTuning so dragging the slider is felt in the conversation that is
		// already open, which is the only way to judge it.
		choiceFadeDelay = static_cast<float>(std::clamp(tunables.choiceFadeDelay, 0, 600)) / 100.0f;
		choiceFadeOut = static_cast<float>(std::clamp(tunables.choiceFadeTime, 5, 200)) / 100.0f;
		poseMode = std::clamp(tunables.poseMode, 0, 5);

		// Only re-roll if the current target has fallen outside the new range.
		// Re-rolling on every apply would restart the count each time a slider
		// moved, so dragging one would hold the camera on its shot indefinitely.
		if (cutEveryTarget < std::min(cutEveryMin, cutEveryMax) ||
			cutEveryTarget > std::max(cutEveryMin, cutEveryMax)) {
			cutEveryTarget = RollCutEvery();
		}

		Render::Letterbox::SetBarFraction(
			static_cast<float>(std::clamp(tunables.letterboxHeight, 0, 300)) / 1000.0f);

		// LIVE, AND ONLY WHILE STAGED.
		//
		// Ticking the box mid-conversation has to put the bars up on the spot, and
		// unticking it has to take them down — that is the whole reason this
		// stopped being a startup read. Outside a conversation nothing is asked
		// for: SetVisible(true) here would letterbox the world.
		letterboxWanted = tunables.letterbox;
		if (staging) {
			Render::Letterbox::SetVisible(letterboxWanted);
		}

		// Performance::SetGaze IS NO LONGER CALLED. The two dials that fed it went
		// with the Eye Contact section, and the gaze model itself is switched off —
		// see the Configure(false, false, ...) in ReadTuning.
	}

	void Director::Close()
	{
		if (!staging) {
			return;
		}

		const bool ownedCamera = Compat::SmoothCam::Holding();
		if (viewMode == ViewMode::kFirstPersonFallback && fallbackThirdPersonOwed && !suspendingForMenu) {
			restoreThirdPersonPending = true;
			if (!TryHandBackView()) {
				handBackPending = true;
				handBackSince = Clock::now();
			}
		}
		viewMode = ViewMode::kCinematic;
		fallbackRequested = false;
		fallbackThirdPersonOwed = false;
		protectedPose = {};
		protectedSearch = {};
		frameSubjects.reset();
		visibilityRecovery.Reset();
		staging = false;
		playerVoiceHandoff.Reset(Scene::LipSync::PlayerLineSerial());
		reactionShots.Reset();
		replyBoundary.Reset();
		subject = {};
		haveShot = false;
		npcSpeaking = false;
		releasePending = false;

		// Nothing is staged, so nothing has a serial. Cleared rather than left
		// holding the last one, because a suspend closes and a resume opens the
		// SAME conversation — the serial has not moved — and Open must not mistake
		// that for the repeat call it is guarding against.
		stagedSerial = 0;

		// The hold on the spent topic list belongs to the conversation, not to the
		// mod. Left set, a line that was still playing when the screen was taken
		// would pin the next conversation's fade before it had started.
		playerLineHeld = false;

		Render::Letterbox::SetVisible(false);
		Scene::Presence::Release();
		Scene::Performance::Release();
		Scene::FaceGen::End();
		Scene::LipSync::Release();
		Scene::Interface::Restore();
		Scene::KeyLight::Release();
		Scene::Focus::Release();

		// EVERY WRITE TO THE CAMERA HAPPENS BEFORE SMOOTHCAM IS HANDED IT BACK.
		//
		// This block used to sit AFTER Compat::SmoothCam::Release(), and that is a
		// reported bug rather than a tidiness point. ReleaseCameraControl is this
		// mod saying "the camera is yours again"; the next three things it did were
		// write the field of view, stamp nine fields of the third-person camera
		// state, and change the camera state outright. For however many frames
		// that took, two plugins believed they owned one camera, and the one that
		// had just been told it did was the one being written over.
		//
		// Reordering costs nothing and removes the whole class: SmoothCam now
		// receives a camera already back at rest, and there is no window in which
		// Scene Director touches a transform it has given away.
		//
		// Guarded on the camera still being in third person for the state change.
		// Between Open and here the player may have gone back to first person
		// themselves, or the engine may have taken the camera somewhere this mod
		// has no business overriding — a killcam, vanity, a mount, furniture.
		//
		// The lens is unconditional rather than "if a shot narrowed it": the last
		// shot on screen may well have had an opinion, and a field of view left
		// narrowed after the camera has been handed back is the single most visible
		// way this mod could break somebody's game.
		//
		// AND NOT ACROSS A SUSPENSION EITHER, for the same reason the aim below is
		// not — which is the half of that argument this line was missing.
		//
		// The suspension deliberately stops handing the aim and zoom back for a
		// menu, because the conversation is coming straight back and there is
		// nothing to put back.
		// The lens was left writing unconditionally, and it is the most visible of
		// the three: a shot on 37.6 degrees snapping to the player's 80 on the
		// frame before the container draws is a hard zoom out, and the world behind
		// the menu is frozen on it for as long as the menu is open. Reported as the
		// camera freezing and zooming out before the menu appeared.
		//
		// Leaving it alone makes the suspension consistent — the pose stays, the
		// zoom stays, and now the lens that composed them stays too — and it
		// removes the matching re-narrow on the way back in.
		//
		// The debt still falls due if the conversation ENDED while the menu was
		// open. That is AbandonSuspension, which pays the aim and now pays this.
		if (!suspendingForMenu && ownedCamera) {
			RestoreFieldOfView();
		}

		// THE AIM, WHICH IS THE OTHER HALF OF THE SAME PROMISE — AND NOT ACROSS A
		// SUSPENSION.
		//
		// What a player notices at the end of a conversation is being dropped into
		// the engine's own dialogue camera, pitched down at the head of whoever
		// they were talking to, because that is where the engine's record has been
		// all along while SD was stamping the node. So the resting state is put
		// back, and that is nine fields of ThirdPersonState — among them
		// savedZoomOffset, which is the game's PERSISTENT third-person zoom.
		//
		// A suspension is not the end of a conversation. Opening a merchant's stock
		// hands the screen over for a moment and the camera is coming straight
		// back, so there is nothing to put back and nothing to put it back for: the
		// engine's dialogue aim has not moved, the menu is drawn over a frozen
		// frame, and two frames later Open() takes the camera again. Writing the
		// persistent zoom on the way into a trade and again on the way out is pure
		// churn against whatever else manages that camera, and it is the churn the
		// SmoothCam report was about.
		//
		// Before ForceFirstPerson below, so the write lands while third person is
		// still the state the camera is in. It would be correct either way; this
		// way it is also correct for the mods that watch the state change.
		if (!suspendingForMenu && ownedCamera) {
			RestoreCameraRest();
		}

		// Also skipped across a suspension, and for the same reason the debt is
		// held rather than paid — see OnScreenTaken. Dropping a first-person player
		// into their own eyes for the length of a trade and hauling them back out
		// afterwards is two camera state changes nobody asked for.
		if (returnToFirstPerson) {
			returnToFirstPerson = false;
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInThirdPerson()) {
				camera->ForceFirstPerson();
				Log::Info(Log::Category::kCamera, "Returned the player to first person."sv);
			}
		}

		// LAST. Everything above is Scene Director putting down what it picked up;
		// this is the line that says so to whoever is waiting for it.
		Compat::SmoothCam::Release();

		Log::Info(Log::Category::kCamera, "Staging closed{}."sv,
			suspendingForMenu ? " for a menu; the view is left where it is"sv : ""sv);
	}

	void Director::OnDialogueMenu(bool a_opening)
	{
		// AHEAD of the staging check, deliberately, and this is the fix for the
		// selection on first approach.
		//
		// The dialogue menu opens and the greeting starts BEFORE Director::Open
		// runs — measured at roughly 120ms — so for that window staging is false.
		// The guard gated on Staging() alone, found it false, and let the press
		// through to Accept, which commits topic zero without consulting the
		// cursor. Every press after Open was blocked correctly, which is exactly
		// why the log looked healthy while the first click of a conversation kept
		// selecting the first option.
		//
		// The choices-visible flag that used to be published alongside this is gone
		// with the guard that read it. Nothing outside the tick needs to know what
		// the topic list is doing any more, because the topic list is now whatever
		// eMenuState says it is at the moment it is asked.
		dialogueMenuUp.store(a_opening, std::memory_order_relaxed);

		if (!staging) {
			return;
		}

		if (a_opening) {
			releasePending = false;
			return;
		}

		// The player has left, or the menu is blinking between topics. Start the
		// clock rather than deciding now.
		releasePending = true;
		releaseSince = Clock::now();
	}

	// A MENU CAN TAKE THE SCREEN, AND THAT IS WHERE THE SCENE USED TO DIE.
	//
	// Training, barter, a gift, a book, "let me see what you have" — every one of
	// those opens a menu carrying kPausesGame, and a paused game stops
	// PlayerCharacter::Update, which is Tick, which is where every release
	// condition in this file lives. Including the one written for precisely this
	// case. "Another menu took the screen; releasing" has never once run for a
	// menu that pauses the game: by the time the menu exists, the tick that would
	// notice it has already stopped. Letterbox found this first and retracts from
	// the present hook instead; this is the rest of the same lesson.
	//
	// So the scene did not end, it FROZE — and it froze holding things that only
	// it would ever put back. The topic list is the one that was reported. At the
	// instant a topic opens a menu the movie's phase is kTopicClicked, which is
	// exactly the fade's cue to take the list down, so the last write before the
	// freeze is _alpha 0 and _visible false. Nothing hands that back while the
	// menu is up, and DriveInterface reasserts it the moment the tick resumes
	// because the phase has not moved. The player comes out of the training menu
	// into a conversation with no visible options and a camera that has been let
	// go, and the only key that does anything is Tab.
	//
	// MenuOpenCloseEvent is the one callback that still arrives once the game is
	// paused, so the decision is taken here rather than one frame too late.
	//
	// RELEASED IN FULL AND RE-PRIMED IN FULL, WITH ONE THING CARRIED ACROSS.
	//
	// The release is Close() — the whole of it, so the camera, the lens, the bars,
	// the HUD and the topic list are all the game's again before the menu draws a
	// pixel. That is what the inventory report was about: a header cannot be
	// clipped by a frame that is not there, and every element the item menus
	// borrow from the HUD is back on screen before they ask for it. The return is
	// Open() — the whole of that too, so no piece of per-conversation state has a
	// second owner. This file has a paragraph in Open() about what happened the
	// last time something reset half of it on its own.
	//
	// What is carried across is the three things a resume cannot re-derive and
	// must not get wrong: which conversation this was, which angle was on screen,
	// and whether the player is still owed first person. See the suspension block
	// at the top of this file. Everything else is rebuilt.
	//
	// A SUSPENSION IS NOT A PROMISE. The conversation may have ended while the
	// menu was up — the NPC walked off, the trade was the last thing the topic
	// did, the player loaded a save. Runtime is what notices, and it calls
	// AbandonSuspension rather than letting a stale resume put bars over an empty
	// street.
	void Director::OnScreenTaken(std::string_view a_menu)
	{
		if (!staging) {
			return;
		}

		// EVERYTHING THE RESUME NEEDS, RECORDED BEFORE THE RELEASE TAKES IT AWAY.
		//
		// Read off live state rather than passed in, because the caller is a menu
		// event and knows nothing about the conversation underneath it.
		const auto partner = subject.get();
		suspendedPartner = partner ? partner->GetFormID() : 0;
		suspendedSerial = Dialogue::Session::GetSingleton().ConversationSerial();
		suspendedShot = currentShot;
		suspendedFirstPersonOwed = returnToFirstPerson;
		suspendedInFallback = viewMode == ViewMode::kFirstPersonFallback;
		suspendedThirdPersonOwed = fallbackThirdPersonOwed;
		suspendedForMenu = suspendedPartner != 0;

		// Cleared BEFORE Close so the release does not pay the debt.
		//
		// Close hands first person back if this mod took it. Correct at the end of
		// a conversation, wrong in the middle of one: the player would sit in their
		// own eyes for the length of the trade and be thrown back into third person
		// afterwards, and the flag would be spent, so the view they were actually
		// owed would never arrive.
		returnToFirstPerson = false;

		// AHEAD OF Close, AND NOT THE SAME CALL.
		//
		// Close eases the bars out over a third of a second, which is right at the
		// end of a conversation and wrong here: the menu draws on the very next
		// frame, and a third of a second of black across the top of an inventory is
		// a clipped header. This takes them off now.
		Render::Letterbox::Retract();

		// Named, because the next report of this should arrive already diagnosed.
		Log::Info(Log::Category::kCamera,
			"Cinematic suspended: '{}' took the screen{}."sv, a_menu,
			suspendedForMenu ? " (resume armed)"sv : " (no partner recorded; not resuming)"sv);

		// TELL Close WHAT KIND OF CLOSE THIS IS.
		//
		// A flag rather than an argument because Close() is the public entry point
		// three other places call, and none of them is a suspension. Cleared on the
		// way out rather than inside Close, so the pairing is visible in one place
		// and an early return in Close cannot leave it set.
		suspendingForMenu = true;
		Close();
		suspendingForMenu = false;

		// AND ASK RUNTIME TO STAGE IT AGAIN WHEN THE SCREEN COMES BACK.
		//
		// Without this the release is permanent. Runtime opens a conversation once,
		// keyed on the partner's form id, and deliberately does not reopen while
		// that key still matches — the branch that used to reopen on any release
		// fought the exit path and produced open/release/open once a second through
		// an NPC's trailing line. Close() does not clear the key, so a release
		// taken here would simply mean no camera for the rest of the conversation.
		//
		// Clearing the key rather than adding a second reopen condition keeps that
		// branch exactly as it is: this is the same "a new partner is the only
		// reason to open" rule, told that the partner it has recorded is no longer
		// staged.
		//
		// There IS a second reopen condition now — Runtime's Stranded(), for a
		// release taken while the player was still in the dialogue menu — and it
		// deliberately does not cover this path. A suspension refuses it outright,
		// because a suspension already knows exactly what it is owed and clearing
		// the key here says so precisely. Two mechanisms answering for one release
		// is what this file spends most of its paragraphs regretting.
		Runtime::RearmConversation();
	}

	void Director::OnScreenReleased()
	{
		// Unconditional, and deliberately not gated on staging.
		//
		// The reading this protects is taken while NOTHING is staged, so gating on
		// staging would skip the write on exactly the frames it exists for. It is
		// also correct for a menu opened with no conversation anywhere near: the
		// camera is settling then too, and half a second of not sampling costs
		// nothing.
		screenReleasedAt = Clock::now();
	}

	bool Director::Suspended() noexcept
	{
		return suspendedForMenu;
	}

	RE::FormID Director::SuspendedFor() noexcept
	{
		return suspendedPartner;
	}

	void Director::AbandonSuspension(bool a_handBackView)
	{
		// A load also cancels a deferred fallback restoration when no menu was
		// suspended. No debt from the outgoing save may be paid in the new one.
		if (!a_handBackView) {
			restoreThirdPersonPending = false;
			handBackPending = false;
		}
		if (!suspendedForMenu) {
			return;
		}

		Log::Info(Log::Category::kCamera,
			"Cinematic resume skipped: the conversation with [{:08X}] ended while the "
			"menu was open."sv,
			suspendedPartner);

		const bool owedFirstPerson = suspendedFirstPersonOwed;
		const bool owedThirdPerson = suspendedThirdPersonOwed;

		suspendedForMenu = false;
		suspendedPartner = 0;
		suspendedSerial = 0;
		suspendedFirstPersonOwed = false;
		suspendedInFallback = false;
		suspendedThirdPersonOwed = false;

		if (!a_handBackView) {
			restoreThirdPersonPending = false;
			// A LOAD. Everything is dropped and NOTHING is written — including any
			// hand-back left pending from before, whose saved aim and zoom belong to
			// the world being torn down. See the caller in Runtime.
			handBackPending = false;
			return;
		}

		// THE HALF OF Close() THE SUSPENSION SKIPPED.
		//
		// Suspending for a menu leaves the camera's resting aim and zoom alone,
		// because the conversation is coming straight back and that write touches
		// the game's persistent third-person zoom. This is the branch where it does
		// not come back — the player walked out of the trade and the conversation
		// had already ended — so the debt falls due here or nowhere, and "nowhere"
		// means being left in the engine's dialogue aim, pitched at somebody who is
		// no longer being spoken to.
		//
		// TAKEN BACK BEFORE ANY OF IT IS WRITTEN, AND HANDED OVER AGAIN AFTER.
		//
		// The comment that used to sit here claimed nothing held camera control at
		// this point and that this was "the same order Close() itself now uses".
		// Both halves were wrong, and together they inverted the one invariant
		// the write-ordering rule above establishes.
		//
		// Close() DOES release SmoothCam — that is its last act — and it ran on the
		// way INTO the suspension, which may have been a minute ago. So by the time
		// this branch is reached SmoothCam has owned and been actively driving the
		// camera for the whole length of the menu, and these writes land on nine
		// fields of a third-person state it is interpolating, plus the lens. That is
		// not the same order as Close(): Close writes and then releases, this
		// released and then wrote. It is precisely the write-after-handoff that
		// corrupted people's SmoothCam settings in the first place, reintroduced
		// through the one path that skips Close's own ordering.
		//
		// So the debt is paid the way any other camera work in this mod is: ask for
		// the camera, write, give it back. The reacquisition can be refused — another
		// consumer may have taken it while the menu was up — and a refusal is
		// honoured rather than written through, because a camera this mod has been
		// told it may not touch is a camera it does not touch.
		//
		// The lens is in here too. A suspension leaves the shot's narrow lens in
		// place because the shot is coming back; here it is not, and a player left
		// on 37 degrees after walking away from a trade is the single most visible
		// way this mod could break their game.
		// AND A REFUSAL DEFERS THE DEBT RATHER THAN CANCELLING IT.
		//
		// Refusing to write a camera this mod does not own is right. Forgetting
		// that it still owes the write is not, and that is what the first version
		// of this did: the suspension had already been cleared above, so Runtime
		// would never call here again, and a momentary refusal became permanent
		// state — the player left holding a 37-degree shot lens and the engine's
		// dialogue aim, pointed at somebody they finished talking to.
		//
		// "Can the conversation resume" and "is a hand-back still owed" are two
		// different questions and now have two different flags. The suspension is
		// spent; the debt survives, and Tick retries it on an idle frame once the
		// session is over, no menu owns the screen, and the camera can be had.
		restoreThirdPersonPending = restoreThirdPersonPending || owedThirdPerson;
		if (!TryHandBackView()) {
			handBackPending = true;
			handBackSince = Clock::now();

			Log::Warn(Log::Category::kCamera,
				"Conversation ended behind a menu, but the camera is held by another plugin. "
				"Nothing written; the hand-back is queued and will be retried."sv);
		}

		// OUTSIDE the ownership block on purpose. This is the player's point of
		// view, not a transform: it is not SmoothCam's to grant and refusing to
		// return somebody to their own eyes because a third mod holds the camera
		// would be the wrong call. Nothing here writes the third-person state.
		if (owedFirstPerson) {
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInThirdPerson()) {
				camera->ForceFirstPerson();
				Log::Info(Log::Category::kCamera,
					"Returned the player to first person after the menu."sv);
			}
		}
	}

	void Director::OnCue(RE::Actor* a_speaker, RE::DialogueResponse* a_response)
	{
		if (!a_response) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (a_speaker && player && a_speaker == static_cast<RE::Actor*>(player)) {
			return;
		}

		// THIRD PARTIES DO NOT DRIVE THIS CONVERSATION.
		//
		// LineWatch hooks UpdateInDialogue on VTABLE_Character, which is every NPC
		// in the cell, and this only ever filtered out the player. So a guard
		// muttering across the market, a follower's idle comment, or two NPCs
		// talking to each other all arrived here as though the person in front of
		// the player had spoken — and were applied in full: npcSpeaking set, the
		// player's voice handle retired, cueIntensity overwritten with a stranger's
		// emotion, and a cut motivated by a line that is not in this scene.
		//
		// The worst of it lands on the topic list. A cue arriving while the choices
		// are up passes the turn to "the NPC", which the fade reads as the player
		// having selected something — choiceSpentThisTurn is set and the list eases
		// away while they are still reading it. In a town that fires every few
		// seconds, and it looks exactly like the menu misbehaving on its own.
		//
		// Open() already applies this same test to the stashed greeting, and says
		// why. The live path simply never got it.
		//
		// Logged rather than dropped silently: if a scene ever speaks to the player
		// through an actor who is not the menu's partner, this line is what will say
		// so. Session watches MenuTopicManager::speaker and restarts on a partner
		// change, so the ordinary multi-NPC case arrives as a new conversation
		// rather than as a stray cue.
		if (staging) {
			auto partner = subject.get();
			if (!partner || partner.get() != a_speaker) {
				const char* who = a_speaker ? a_speaker->GetName() : nullptr;
				Log::Info(Log::Category::kDialogue,
					"Ignored a line from {} — not the partner this conversation is staged on."sv,
					(who && *who) ? who : "<unnamed>");
				return;
			}
		}

		const std::uint32_t words = WordCount(a_response->text.c_str());
		const auto          intensity = a_response->percent;
		const auto          emotion =
			static_cast<std::uint32_t>(a_response->animFaceArchType.underlying());

		// The greeting arrives before the camera does.
		//
		// Measured, not assumed: across every conversation in the log the opening
		// response cue lands about 120ms BEFORE Director::Open runs â€”
		//
		//   [21:51:38.611] Cue 26 | Katana | "Hi."
		//   [21:51:38.728] Staging opened on Katana.
		//
		// so a `!staging` guard here dropped the first line of every conversation
		// in the game on the floor. Everything downstream then went wrong at once:
		// the word floor never saw the greeting, so a two-word hello still earned
		// a cut; establishing was never cleared; the intensity stayed at its
		// default; and the turn was left to be rediscovered by the debounced
		// session watcher, which knows a line started but not what is in it.
		//
		// Stashed rather than applied, because at this moment there is no
		// conversation to apply it to. Open() consumes it once staging exists and
		// the dials have been read.
		if (!staging) {
			pendingCue.speaker = a_speaker ? a_speaker->GetHandle() : RE::ActorHandle{};
			pendingCue.words = words;
			pendingCue.intensity = intensity;
			pendingCue.emotion = emotion;
			pendingCue.text = a_response->text.c_str();
			pendingCue.at = Clock::now();
			pendingCue.valid = true;
			return;
		}

		ApplyCue(a_speaker, words, intensity, emotion, a_response->text.c_str());
	}

	void Director::Tick(float a_delta)
	{
		// THE SCREEN LIST RECONCILES AGAINST THE ENGINE, EVERY FRAME.
		//
		// MenuWatch's record of which menus own the screen is kept by matching open
		// events against close events, and a record kept that way is one dropped
		// event away from being wrong forever. Wrong in the direction of "still
		// taken" means no conversation ever stages again, which is a far worse
		// failure than the one this path exists to fix. Ahead of the staging check
		// because the state it repairs is read by Runtime, which does not care
		// whether anything is staged.
		Dialogue::MenuWatch::Reconcile();

		// Ahead of the staging check, on purpose.
		//
		// The open question is whether SD's own camera is what stops the player's
		// face animating. IACC does not stop it, so IACC is the control â€” and the
		// way to run that control is [Direction] bEnabled=0, which means the
		// director never opens and never stages. A probe behind the staging check
		// would fall silent in exactly the configuration being measured.
		//
		// Tick runs every frame from the always-on frame source whatever the
		// camera is doing, so the reading is comparable across both.
		// Not behind the session check either, and that was the gap.
		//
		// The reported behaviour is that where the camera sits BEFORE the
		// conversation opens decides whether the player's mouth works for the whole
		// of it. A probe that only samples once a session is live cannot see the
		// moment being described. Worse, it looks as though it can: the first run
		// of this probe produced samples timestamped before a conversation opened,
		// and every one of them belonged to the PREVIOUS conversation, which is not
		// a baseline â€” it is the thing under test.
		//
		// So it free-runs now. Outside a conversation Partner() is empty and the
		// NPC half simply does not report.
		{
			auto& session = Dialogue::Session::GetSingleton();
			auto  partner = session.Active() ? session.Partner().get() : RE::NiPointer<RE::Actor>{};
			Scene::Performance::Probe(partner.get(), a_delta);
		}

		Scene::FaceGen::Tick(a_delta);

		// Before the morph pass, which is the point. Director::Tick runs off
		// PlayerCharacter::Update — part of the actor update, ahead of the traversal
		// that consumes the channel — so the value FaceGen writes in the hook is
		// this frame's, not last frame's.
		Scene::LipSync::Update(a_delta);

		// The player's expression rides the same timing, and is here rather than in
		// Performance::Update for the same reason the probe is: Update sits at the
		// bottom of the staging path, behind the camera's anchor resolution, and
		// returns early whenever either anchor fails. An expression envelope driven
		// from there stalls on exactly the frames the camera is struggling. It also
		// has to keep running after Release so the ease-out completes and the
		// override flag gets handed back.
		Scene::Performance::UpdateFace(a_delta);

		// Ahead of the staging check on purpose. Open() suppresses the HUD on the
		// frame it stages, reading settings this applies — so a change made while
		// standing in front of an NPC has to have landed before the conversation
		// starts, not on the first frame after it.
		SyncInterfaceSettings();

		// THE HOTKEY REQUESTS, DRAINED ONTO THIS THREAD.
		//
		// Here rather than in OnThirdPersonUpdate for the reason Release and the
		// interface driver both have a paragraph about: the camera hook stops
		// firing the moment the camera leaves third person, and a key pressed in
		// that window would either be lost or fire late. Tick is the always-on one.
		//
		// Ahead of the staging check but consumed only when staged, so a key
		// pressed outside a conversation clears itself rather than lying in wait
		// for the next one to open and immediately cutting it.
		{
			const bool wantCut = requestCut.exchange(false, std::memory_order_relaxed);
			const bool wantFraming = requestFraming.exchange(false, std::memory_order_relaxed);
			const bool lineRuleFlipped = lineRuleChanged.exchange(false, std::memory_order_relaxed);

			if (staging) {
				if (wantFraming) {
					framing = NextFraming(framing);

					// A framing change IS a cut. Choosing a new subject and then
					// leaving the old one on screen for the rest of the minimum
					// hold is the press appearing to do nothing.
					forcedCut = true;

					// Expires at the END of the turn it was set in. See the note on
					// framingUntilTurn: pressing again inside the same turn cycles
					// the choice and re-arms it, so holding a framing across an
					// exchange is still possible — it just has to be asked for
					// rather than being what happens by default.
					framingUntilTurn = turnSerial;

					Log::Info(Log::Category::kContinuity,
						"Framing forced to {} by hotkey, for the rest of this turn."sv,
						FramingLabel(framing));

					char note[64]{};
					std::snprintf(note, sizeof(note), "Camera: %.*s",
						static_cast<int>(FramingLabel(framing).size()), FramingLabel(framing).data());
					// CommonLibSSE-NG 7 dropped RE::DebugNotification. This is the same engine
					// function it wrapped, reached by the address library IDs it used.
					static REL::Relocation<void (*)(const char*, const char*, bool)> showNotification{
						REL::RelocationID(52050, 52933)
					};
					showNotification(note, nullptr, true);
				}

				if (wantCut) {
					forcedCut = true;
				}

				// true180 changed mid-conversation. Only the player's shots move, so
				// cut if one is on screen. Clearing heldSince makes it re-pick its
				// angle if the cut finds nothing better.
				if (lineRuleFlipped) {
					lastPlayerPose = {};
					if (!FavoursNpc(currentShot)) {
						forcedCut = true;
						heldSince = {};
					}
				}

				// THE OVERRIDE HANDS ITSELF BACK when the turn it was set in ends.
				//
				// Checked here rather than at the turn edge itself, and that is
				// deliberate: the edge lives in OnThirdPersonUpdate, which stops
				// firing the moment the camera leaves third person. An override
				// stranded by that would outlive its turn by the whole of a mount,
				// a menu or a killcam — the same failure Release and DriveInterface
				// each have a paragraph about. Tick is the always-on one.
				//
				// No forced cut on the way out. Auto agrees with the override for
				// the rest of the turn it was set in, so at this point the camera is
				// already where auto would put it; the next real cut moves it. A cut
				// here would be the camera lurching for a reason the player cannot
				// see, which is the opposite of handing back quietly.
				if (framing != Framing::kAuto && turnSerial != framingUntilTurn) {
					Log::Info(Log::Category::kContinuity,
						"Framing hold on {} expired with the turn; back to automatic."sv,
						FramingLabel(framing));
					framing = Framing::kAuto;
				}
			}
		}

		// Keep a reading of the player's own camera while nothing is staged: the
		// lens, the aim and the zoom.
		//
		// `!staging` is the coarse half of the question and it is not enough on its
		// own — the engine starts swinging at the speaker before Open() runs, and a
		// menu leaves every one of these values mid-flight for the first frames
		// after it closes. The rest of the gates live in SampleCameraRest, which is
		// now the single place that decides whether this moment is worth reading.
		//
		// The field of view used to be read here instead, ahead of that call and
		// outside all of it. See SampleCameraRest for what that cost.
		if (!staging) {
			// A HAND-BACK THE CAMERA WAS REFUSED FOR, RETRIED UNTIL IT LANDS.
			//
			// See AbandonSuspension. A conversation that ended behind a menu owes
			// the player their lens and their aim back, and the plugin that held the
			// camera at that moment may not hold it a second later. Retried here
			// rather than abandoned, because the alternative is leaving somebody on
			// a cinematic lens for the rest of the session.
			//
			// Three conditions, and they are the ones that make the write safe
			// rather than merely possible: nothing is staged (this branch), no
			// conversation is running, and no menu owns the screen.
			//
			// No early return. SampleCameraRest below may run on the same frame and
			// that is harmless — it would read back exactly the values just written
			// — and returning here would skip the topic-list debt underneath it,
			// which is owed on every idle frame.
			if (handBackPending && SecondsSince(handBackSince) >= kHandBackRetrySeconds) {
				handBackSince = Clock::now();

				if (!Dialogue::Session::GetSingleton().Active() &&
					!Dialogue::MenuWatch::ScreenTaken() && TryHandBackView()) {
					handBackPending = false;
					Log::Info(Log::Category::kCamera,
						"Queued hand-back completed; the lens and the resting view are back."sv);
				}
			}

			SampleCameraRest();

			// THE ONE THING A CLOSED DIRECTOR STILL OWES.
			//
			// Close() hands the topic list back, and that hand-back can fail: it
			// writes into the dialogue movie, and Close is reached from paths where
			// there is no dialogue movie to write into yet. A list left at alpha 0
			// with _visible false is invisible and still commits on Accept, and
			// nothing was retrying it, because everything that drives the list runs
			// only while staging.
			//
			// Free when there is no debt — one bool, tested and returned on.
			Scene::Interface::ReleaseChoices();
			return;
		}

		// Leaving a conversation should hand the camera straight back. Waiting for
		// the session to end meant waiting for the NPC to finish their line, which
		// left the camera locked on someone the player had already walked away from.
		if (releasePending && SecondsSince(releaseSince) >= kReleaseGraceSeconds) {
			Close();
			return;
		}

		// Anything that pauses the game has taken over the screen â€” an inventory,
		// the map, or another mod's own camera session such as OPS. Staging over
		// the top of it would fight for the same camera and leave bars across
		// somebody else's menu.
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			Log::Info(Log::Category::kCamera, "Another menu took the screen; releasing."sv);
			Close();
			return;
		}

		// The player has walked out, even though the NPC is still talking.
		//
		// This is the precise signal, and it is why the menu-close grace was not
		// enough on its own: leaving mid-line closes the menu, but the menu then
		// *reopens* for the trailing dialogue, which cancelled the pending release
		// and held the camera until the NPC finally shut up.
		//
		// MenuTopicManager tells the two apart. While the player is in the
		// conversation `speaker` is valid; the moment they leave it goes null and
		// only `lastSpeaker` remains, carrying the line still being spoken. A null
		// speaker means the scene is over regardless of who is still talking.
		//
		// AND THE DIALOGUE MENU HAS TO BE DOWN. That is the half this was missing,
		// and it is the whole of the "talking to somebody too fast switches the mod
		// off" report.
		//
		// A null speaker does NOT mean the player has left. It means the topic
		// manager has no live speaker at this instant, which is also true in the gap
		// between one conversation and the next with the same person — commit a
		// topic while a line is still running, or press activate again a moment
		// after the last exchange ended, and the engine drops `speaker`, closes the
		// menu, reopens it, and takes as long as it likes to hand the handle back.
		// Measured 2026-09-04, on a shopkeeper spoken to twice in quick succession:
		//
		//     09:38:55.962  Dialogue Menu CLOSE | manager: speaker, talking
		//     09:38:55.989  Player left the conversation; releasing
		//     09:38:56.615  Dialogue Menu OPEN  | manager: lastSpeaker only, silent
		//     09:39:04.993  ... speaker back; session restarts, camera returns
		//
		// Nine seconds of live dialogue menu with the camera handed back, the bars
		// gone and the HUD restored, and the player standing still the whole time
		// (the face probe holds dist=162 and inView=+0.73 across every frame of it).
		// From the outside that is the mod switching itself off mid-conversation,
		// and it was permanent for the rest of that conversation: Runtime records
		// one open per conversation, so nothing re-staged until Session manufactured
		// a new serial, which it can only do once the speaker handle comes back.
		//
		// The menu is the thing that can tell the two apart. Skyrim disables the
		// movement controls for as long as the dialogue menu is up — Improved
		// Camera's whole scripted-third-person conflict is built on that fact, and
		// the compatibility warning a few hundred lines away says so — so a player
		// looking at a dialogue menu has not walked away from anybody. They cannot.
		// The release exists for "the camera is locked on someone the player has
		// already walked away from", and walking is exactly what the menu forbids.
		//
		// What this costs: leaving mid-line, if the engine really does reopen the
		// menu for the trailing farewell, now keeps the camera on that line instead
		// of cutting out of it. That is the direction this mod already leans —
		// Session carries the session through the tail on lastSpeaker for the same
		// reason, because cutting away on the goodbye is the most visible failure
		// available. The menu closing still hands the camera straight back, both
		// here and through the release grace above.
		//
		// Asked of the engine rather than of the dialogueMenuUp latch next door.
		// That latch is kept by pairing open events against close events, and a
		// record kept that way is one dropped event away from being wrong forever —
		// which here would be a camera that can never be released at all, a far
		// worse failure than the one being fixed. MenuWatch::Reconcile reached the
		// same conclusion about the same class of record and asks ui the same way.
		if (SecondsSince(stagingSince) > kExitCheckDelay && !DialogueMenuOpen()) {
			if (auto* manager = RE::MenuTopicManager::GetSingleton()) {
				const bool inConversation = static_cast<bool>(manager->speaker.get());
				if (!inConversation) {
					Log::Info(Log::Category::kCamera, "Player left the conversation; releasing."sv);
					Close();
					return;
				}
			}
		}

		// The subject is gone â€” unloaded, dead, or a cell change took them.
		if (!subject.get()) {
			Close();
			return;
		}

		// Combat ends a scene. Nothing about a directed conversation camera is
		// survivable once someone draws a weapon.
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->IsInCombat()) {
			Log::Info(Log::Category::kCamera, "Combat started; releasing."sv);
			Close();
			return;
		}

		// The dialogue menu is gone and the session has ended. This is the
		// backstop for every exit path that does not fire a menu-close event.
		if (!Dialogue::Session::GetSingleton().Active() && !releasePending) {
			Close();
		}

		// LAST, AND BEHIND A FRESH staging CHECK. Every branch above this can call
		// Close(), which clears staging and hands the screen back — driving the
		// list on the far side of that would write to a movie the conversation has
		// already finished with.
		//
		// Here rather than in OnThirdPersonUpdate because the screen has to stay
		// clear for the whole conversation, not for the part of it the camera
		// happens to spend in third person.
		if (staging) {
			if (protectionSettingsDirty.exchange(false, std::memory_order_relaxed)) {
				protectedPose = {};
				protectedSearch = {};
				visibilityFailedObservation = false;
				visibilityCheckedAt = {};
				protectedRetryAt = {};
				if (viewMode == ViewMode::kFirstPersonFallback) {
					visibilityRecovery.EnterFallback(VisibilityTime());
				} else {
					visibilityRecovery.Reset();
				}
			}
			frameSubjects = SampleSubjects(a_delta);
			const auto dialoguePhase = Scene::Interface::ReadDialoguePhase();
			const bool choosing = dialoguePhase.valid && !dialoguePhase.lineInFlight &&
				dialoguePhase.phase == Scene::Interface::MenuPhase::kTopicList &&
				!Compat::DBReV::Speaking();
			// Timers run on frame time, capped so a hitch can't skip a hold, and
			// paused while the game is.
			const float frameDelta = std::clamp(a_delta, 0.0f, 0.25f);

			const bool wasHandingOff = playerVoiceHandoff.Active();
			const bool wasHolding = playerVoiceHandoff.Holding();
			playerVoiceHandoff.SetDelay(playerVoiceHoldSeconds);
			playerVoiceHandoff.Update(Scene::LipSync::PlayerLineSerial(),
				Scene::LipSync::PlayerSpeaking(), npcSpeaking, choosing, frameDelta);
			if (!wasHolding && !wasHandingOff && playerVoiceHandoff.Holding()) {
				Log::Info(Log::Category::kContinuity,
					"Player voice ended; holding on you for {:.2f}s."sv, playerVoiceHoldSeconds);
			}
			if (!wasHandingOff && playerVoiceHandoff.Active()) {
				Log::Info(Log::Category::kContinuity,
					"Player voice ended; framing the NPC before the reply."sv);
			}

			TrackTopicPicks(dialoguePhase, frameDelta);

			if (protectSubject && !frameSubjects && viewMode == ViewMode::kCinematic) {
				fallbackRequested = firstPersonFallback;
			}
			TickProtected(frameSubjects);
			DriveInterface();
		}
	}

	void Director::OnThirdPersonUpdate(RE::ThirdPersonState* a_state)
	{
		if (!staging || !a_state || viewMode != ViewMode::kCinematic || fallbackRequested) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* root = camera ? camera->cameraRoot.get() : nullptr;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  speakerPtr = subject.get();
		if (!root || !player || !speakerPtr) {
			return;
		}

		// A subject who gets out of bed is a new set-up, not a subject who moved.
		//
		// Getting up runs through several states (asleep -> waking -> standing up ->
		// standing) and re-measuring on each one is deliberate: the height converges
		// as the animation plays instead of latching onto a half-risen pose.
		if (!frameSubjects) {
			return;
		}
		auto subjects = *frameSubjects;
		const float delta = subjects.delta;

		const float held = SecondsSince(shotSince);

		// TIMED ANGLE CHANGE. The staleness ceiling, and the only cut in the mod
		// driven purely by a clock — the other mode waits for lines.
		//
		// Governed separately for the two halves of the exchange because they are
		// different problems. While the NPC talks the camera has a subject and a
		// reason to be where it is, and a timed cut across a speech is the mod
		// interrupting itself. While the player reads a topic list nothing is
		// happening at all, and the ceiling is the only thing that stops the
		// camera sitting on a silent face indefinitely. Both ship off.
		const bool timedAllowed = npcSpeaking ? timedCutsWhileSpeaking : timedCutsWhileChoosing;
		const bool stale = timedAllowed && held >= maxShotSeconds;

		// PER LINE ANGLE CHANGE. The setup holds until it has seen as many eligible
		// lines as this cadence rolled, which is what "change angle every 3 to 6
		// lines" means.
		//
		// THE TURN IS NOT A SECOND WAY IN, and that is the fix rather than an
		// omission. This read `turnSinceCut || (cueSinceCut && ...)`, and
		// turnSinceCut is set on every line that opens a reply — so in an ordinary
		// back-and-forth exchange, where each of the NPC's answers is its own turn,
		// the first clause fired every single time and the cadence dials could
		// never be felt. Setting them to 3-6 and watching the camera cut on every
		// line is how that was found.
		//
		// The turn still selects the SHORT floor below, so a cut that lands on one
		// is not held back by the full minimum. What it may no longer do is ask for
		// a cut on its own account. Coverage — being on the wrong person — is the
		// one thing that still can, and it is judged separately and cannot fire on
		// a line ending.
		const bool motivated = perLineAngleChange && cueSinceCut &&
			linesSinceCut >= cutEveryTarget;

		// Hold the player through the start of the reply. Cutting away the instant
		// the NPC opens their mouth throws away the beat where the player's choice
		// registers â€” and with a player-voice mod installed, throws away their line.
		// A voiced line already supplied its player beat. Do not add the old
		// reply-start delay after its audio-end handoff; keep it for unvoiced turns.
		// For voiced lines the iPlayerVoiceHold window takes its place.
		const bool inPlayerBeat = npcSpeaking && !playerVoiceHandoff.Active() &&
			(playerVoiceHandoff.Holding() ||
				std::chrono::duration<float>(Clock::now() - replyStartedAt).count() < playerBeatSeconds);

		// Coverage is not negotiable, and is not subject to the timer.
		//
		// Whoever is talking gets the camera. If the shot on screen frames the
		// wrong person, that is a mistake to correct now â€” snapping is better than
		// holding a shot of a silent listener while someone else speaks. The
		// minimum exists to stop accent cuts strobing; it was never meant to veto
		// the most important cut in the scene, which is exactly what it did when a
		// 0.70s line expired before the 2.4s floor elapsed.
		// A neutral shot is never the wrong subject: it frames the space, not a
		// person. That is what lets the camera hold an environmental angle through
		// a topic list instead of being dragged back to the player every frame â€”
		// but the moment the NPC speaks, they are the subject and it must move.
		//
		// holdingThroughPause IS GONE, WITH THE SETTING THAT NEEDED IT.
		//
		// It read `!cutOnLineEnd && !npcSpeaking`, and it existed only to make the
		// OFF state of that setting mean anything: without it, the frame after the
		// NPC stopped talking the shot they were framed in became the wrong subject
		// by definition, so coverage moved the camera anyway and the toggle changed
		// which branch fired and nothing else.
		//
		// With the setting gone, keeping the hold unconditionally would have been
		// the opposite mistake and a far worse one. The exchange this mod exists to
		// shoot is shot and reverse shot: the NPC finishes, the turn is the
		// player's, the player is now the subject, and the camera goes to them.
		// Hold that off permanently and the camera never leaves the NPC, the whole
		// player side of every preset's shot list is unreachable, and Close ships
		// two setups of the player that can never be drawn.
		//
		// So the pause is not held and the coverage rule below is what handles the
		// handover. That is NOT the deleted option under another name, and the
		// difference is worth being precise about:
		//
		//   bCutOnLineEnd was a PACING rule. It made a line ending motivate a cut
		//   on its own account, whoever the camera was already on — so a line
		//   finishing produced a fresh angle of the SAME person, and spent the
		//   cadence doing it.
		//
		//   Coverage is a CORRECTION. It fires only when the shot on screen frames
		//   the wrong party, it always lands on the other one, and it never spends
		//   the line count (see the reset at the cut). A conversation with the
		//   camera already on the right person sees nothing from it.

		// WHO THIS FRAME WANTS THE CAMERA ON. Asked once; every test below reads it.
		const bool wantNpc = SubjectIsNpc();
		const bool wantRoom = FramingIsRoom();
		const bool forcedFraming = framing != Framing::kAuto;

		// A neutral parked over the player's own turn counts as the wrong subject
		// too, once coverage of that turn is enforced. Without this the correction
		// is only half applied: the picker would stop CHOOSING the room, but a room
		// shot already on screen when the turn passed would sit there on the full
		// shot floor rather than being moved off on the turn floor.
		//
		// It is switched off under a forced framing, where it would be answering a
		// question the player has already answered by hand: with the camera pinned
		// to the room, a neutral over their turn is the whole point.
		//
		// During a reaction a neutral is wrong even with coverPlayerTurn off.
		const bool neutralStranded = (coverPlayerTurn || ReactionActive()) &&
			!forcedFraming && !wantNpc && IsNeutral(currentShot);

		// A forced framing makes "wrong subject" mean something stricter, and it
		// has to: the correction below is the only thing that moves the camera off
		// a shot it is already sitting on. Without this the key would change what
		// gets CHOSEN at the next cut and leave the current angle up for its full
		// minimum, which reads as the press having been ignored.
		const bool wrongSubject = wantRoom ?
			!IsNeutral(currentShot) :
			(forcedFraming ?
					(IsNeutral(currentShot) || FavoursNpc(currentShot) != wantNpc) :
					(neutralStranded ||
						(!IsNeutral(currentShot) && FavoursNpc(currentShot) != wantNpc)));

		// A key press outranks the hold floor. That is the entire point of it:
		// the floor exists so accent cuts cannot strobe, and a player asking for a
		// different angle by hand is not an accent cut.
		const bool forceNow = forcedCut || !Drawable(currentShot);

		const float floor = (turnSinceCut || wrongSubject) ? minTurnSeconds : minShotSeconds;

		Pose smartFrame{};
		if (protectSubject) {
			const bool ordinaryCut = (motivated || stale || wrongSubject || forceNow) &&
				(held >= floor || forceNow) && (!inPlayerBeat || forceNow) &&
				(!HoldingOpenShot() || forceNow);
			const auto result = ProtectedFrame(subjects, ordinaryCut);
			if (!result) {
				if (!fallbackRequested) {
					UseNativeView();
				}
				return;
			}
			smartFrame = *result;
		}

		// A cut needs both a reason and enough time on the current shot. Without
		// the reason test the camera cut whenever the random pick disagreed with
		// what was on screen, which is why it read as snapping.
		//
		// THE ESTABLISHING CLAUSE IS GONE FROM HERE TOO, and the bug it was fixing
		// went with the shot rather than being carried forward.
		//
		// It read `!Establishing()`, and it had to: Choose() returning kTwoShot
		// during the window was the correct answer and was thrown away, because
		// kTwoShot was also what Open() started on — so the candidate loop saw
		// next == currentShot, called that a rejection four times over, and fell
		// through to the ladder, whose first entry is kCloseUp. Every conversation
		// in the log opened the same way because of it:
		//
		//   two-shot -> close-up after 1.36s ... (speaking, turn, intensity 50)
		//   two-shot -> close-up after 1.37s ... (speaking, turn, intensity 50)
		//
		// Open() now starts on a weighted draw from the speaker's own opener pool,
		// so there is no single shot the picker is guaranteed to re-offer and no
		// window to protect.
		if (!protectSubject && (motivated || stale || wrongSubject || forceNow) &&
			(held >= floor || forceNow) && (!inPlayerBeat || forceNow)) {
			// Chosen here rather than every frame, and that is a fix, not a tidy-up.
			//
			// Choose() was evaluated unconditionally on the way into this test, so
			// Coverage() ran about sixty times a second whether or not a cut was
			// coming, and the pool choice was decided by the parity of the frame
			// counter at the instant a cut happened to land.
			//
			// Moving it here fixed the per-frame half of that. It did NOT fix the
			// retry loop immediately below, which calls Choose() up to four more
			// times per cut — so any state Coverage() mutated per call was still a
			// coin toss, just a quieter one. That is what finally removed the
			// alternation in favour of a weighted roll; see Coverage().
			ScoreCache cache{};

			ShotType next = currentShot;
			float    bestQuality = -1.0f;
			bool     found = false;

			for (int attempt = 0; attempt < 4; ++attempt) {
				const auto choice = Choose();
				if (!choice || !Drawable(*choice)) {
					continue;
				}
				const auto candidate = *choice;
				// A candidate that frames the wrong person is never acceptable,
				// however good its sightline.
				//
				// Asked via IsNeutral rather than by naming two shots. The old list
				// admitted kTwoShot and kDistant but not kWide, kProfile or
				// the anchored pair, even though wrongSubject above already treats every
				// neutral as valid â€” so the two tests disagreed about the same
				// shots, and the laterals could only ever be reached through the
				// no-turn path. The new two-shot variants need this to be right.
				// A neutral is normally acceptable coverage for either party,
				// because there is no wrong subject to be on. That stops being true
				// for the player's own turn once coverage of it is enforced: a
				// two-shot IS a legitimate answer there, and accepting it is
				// precisely how the camera ends up never cutting to the player in a
				// wide-heavy set. During the NPC's line a neutral stays welcome.
				// A neutral is no longer a free pass once the player has named a
				// subject by hand. It never was the WRONG subject, which is exactly
				// how a wide shot came to be an acceptable answer to "show me
				// them" — there is no person in it to be wrong about.
				const bool neutralAllowed = !forcedFraming && wantNpc;
				const bool correctSubject = wantRoom ?
					IsNeutral(candidate) :
					(FavoursNpc(candidate) == wantNpc || (IsNeutral(candidate) && neutralAllowed));

				if (candidate == currentShot || !correctSubject) {
					continue;
				}

				// BEST OF THE DRAWS, NOT FIRST THAT PLACES.
				//
				// The weighting has already done its job by the time a type is
				// offered here — Choose() drew it in proportion to the player's own
				// slider. What is left to decide is which of the offers the ROOM
				// suits, and that is the question `valid` could never answer.
				const float quality = Placement(candidate, subjects, cache);
				if (quality > bestQuality) {
					bestQuality = quality;
					next = candidate;
					found = quality >= 0.0f;
				}

				if (bestQuality >= kGoodEnough) {
					break;
				}
			}

			// Nothing in the rotation fits. Walk a ladder ordered by how much room
			// each shot needs, tightest first, because a small room rules out the
			// wide ones and nothing else.
			if (!found) {
				// Every ENABLED setup that frames the right person, tightest first.
				//
				// This was four hardcoded shots tested only for whether they would
				// place — the old Usable() asked Solve() whether the geometry works and
				// never consulted the enable flags. So the ladder handed back a
				// close-up, a dirty single, a medium or a shoulder shot regardless
				// of what the player had switched off, and since the rotation fails
				// often once most setups are disabled, that fallback WAS the
				// conversation. Turn everything off but the extreme close-up and
				// the camera alternated between two angles that were both off.
				//
				// Tightest first for the reason the old ladder was ordered that
				// way: a tight setup needs the least room, so in a cramped interior
				// it is the one most likely to place. Sorted rather than
				// max-scanned so Placement — which raycasts — is called on the
				// candidates most likely to work rather than on all forty, and so
				// the bounded look-ahead below spends its budget where the answers
				// are.
				std::array<ShotType, static_cast<std::size_t>(ShotType::kCount)> ladder{};
				std::size_t                                                      count = 0;

				for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
					const auto candidate = static_cast<ShotType>(i);
					if (candidate == currentShot || !Drawable(candidate)) {
						continue;
					}
					if (wantRoom ? !IsNeutral(candidate) : FavoursNpc(candidate) != wantNpc) {
						continue;
					}
					ladder[count++] = candidate;
				}

				std::sort(ladder.begin(), ladder.begin() + static_cast<std::ptrdiff_t>(count),
					[](ShotType a_lhs, ShotType a_rhs) { return FillOf(a_lhs) > FillOf(a_rhs); });

				// Best of the first few that place, rather than the first.
				//
				// Bounded on purpose, and the bound is the difference between this
				// and the draws above. The ladder can be forty entries long and it
				// runs precisely when the room is defeating everything, which is
				// also when each solve is at its most expensive — so it stops as
				// soon as it has a few real options to choose between, or on one
				// good enough not to need the comparison.
				//
				// Ordered tightest-first, so the ones it examines are the ones most
				// likely to place in a cramped room anyway.
				constexpr int kLadderLooks = 6;

				int   examined = 0;
				float ladderBest = -1.0f;

				for (std::size_t i = 0; i < count; ++i) {
					const float quality = Placement(ladder[i], subjects, cache);
					if (quality < 0.0f) {
						continue;  // will not place here at all
					}

					if (quality > ladderBest) {
						ladderBest = quality;
						next = ladder[i];
						found = true;
					}

					if (ladderBest >= kGoodEnough || ++examined >= kLadderLooks) {
						break;
					}
				}

				bestQuality = ladderBest;
			}

			// The forced opening shot outranks the cut policy until it expires.
			// Without this the first timed cut lands inside the hold window and the
			// experiment measures nothing.
			if (found && !forceNow && HoldingOpenShot()) {
				found = false;
			}

			// The request is spent whether or not it found anywhere to go. A press
			// that survived into the next frame would fire again the moment the
			// room opened up, which is a cut the player asked for once arriving
			// twice — and arriving the second time for no reason they can see.
			forcedCut = false;

			if (found && Drawable(next)) {
				// The lens and the move are named here because they are the two
				// things a distribution of shot names cannot tell you. Two cut
				// lines reading "close-up" and "close-wide" say nothing about
				// whether the camera actually did anything different; the same two
				// reading "50deg locked" and "88deg zoom-in" say all of it, and
				// that is the log somebody tuning this has to read.
				// Quality is named here for the same reason the lens and the move
				// are: a distribution of shot names cannot tell you whether the
				// camera is getting the shots it asked for or scraping into them.
				// Two cuts both reading "close-up" mean different things at 0.94
				// and at 0.31, and the second is the one worth chasing.
				Log::Info(Log::Category::kContinuity,
					"Cut: {} ({}) -> {} ({}) [{:.0f}deg {} q{:.2f} {}] after {:.2f}s / {} line(s) ({}, {}, intensity {})."sv,
					Name(currentShot), SubjectName(currentShot),
					Name(next), SubjectName(next),
					LensOf(next), MoveName(next),
					bestQuality < 0.0f ? 0.0f : bestQuality, SpaceName(roomSpace),
					held, linesSinceCut,
					npcSpeaking ? "speaking"sv : "waiting"sv,
					// The reason, in the order the test above asks for it, so a log
					// line names the mechanism that actually fired rather than
					// whichever flag happened to be set alongside it.
					forceNow    ? "by hand"sv :
						wrongSubject ? "coverage"sv :
						motivated    ? "line count"sv :
						stale        ? "timer"sv :
									   "unknown"sv,
					cueIntensity);
				previousShot = currentShot;
				currentShot = next;
				shotSince = Clock::now();

				// WHAT USED TO BE HERE: the room shots' swing, rolled per cut from
				// iRoomSwing and held for the life of the shot.
				//
				// It was a randomiser standing in for a decision, which is the thing
				// this whole pass is removing. Each room shot already carries its own
				// angle off the open direction in the table — the master at 24, the
				// ground-level at -20, the distant-low at 32 — and if a player wants
				// one of them to move around the space, an orbit is now something
				// they ask that shot for by name.
				//
				// The reasoning it was written under still holds and is worth
				// keeping: anything decided per shot must be decided AT THE CUT, not
				// inside Solve, which runs every frame. See heldSweep.

				// A HANDOVER IS NOT AN ANGLE CHANGE, AND MUST NOT SPEND THE BUDGET.
				//
				// The cadence counts how many LINES a setup holds before the camera
				// finds a new angle. Cutting because the camera was on the wrong
				// person is a different mechanism entirely — that is shot/reverse
				// shot, and it fires whether or not the cadence is due.
				//
				// The effect of letting it reset the tally was that handovers ate
				// the whole budget. Every exchange hands over twice, so at "every 3
				// to 6 lines" the counter was knocked back to zero before it ever
				// reached three: the angle never changed on cadence, and the dials
				// appeared to do nothing at any value above one.
				//
				// The test used to be `!turnSinceCut`, which named the same idea
				// through the flag that happened to carry it at the time. It stopped
				// being right the moment the turn stopped motivating cuts: the
				// handover to the PLAYER sets no turn flag at all, so a pure
				// coverage cut passed the test and reset the count. Asked directly
				// now — did the thing that asked for this cut care about the tally.
				//
				// Re-rolled here rather than at the top of the next line. Rolling
				// per line would resample the range every time and average out to
				// the middle, which is not a varied rhythm — it is a fixed one with
				// noise.
				if (motivated || stale || forceNow) {
					linesSinceCut = 0;
					cutEveryTarget = RollCutEvery();
				}
			}
			cueSinceCut = false;
			turnSinceCut = false;
		}

		// The push-in runs on its own clock, not the staleness ceiling.
		//
		// Progress was normalised against kMaxShotSeconds (9s) while shots actually
		// last two to four, so a typical line only ever reached about a fifth of
		// the move â€” a 14% dolly delivering 5%, which is below the threshold at
		// which anyone notices a camera is moving at all.
		// Timed against THIS shot's own duration, not one global window. A slow
		// creeping master and a quick push on a reaction are different lengths of
		// move, and one number could only ever serve one of them.
		const float moveSeconds =
			std::max(static_cast<float>(Shot::MoveTime(currentShot)) / 100.0f, 0.1f);
		subjects.progress = std::clamp(SecondsSince(shotSince) / moveSeconds, 0.0f, 1.0f);

		// The angle and standoff this shot committed to, handed back to Solve.
		//
		// Set HERE and nowhere earlier, which is what keeps candidate evaluation
		// honest: Subjects is constructed fresh every frame further up, so every
		// Placement() call above this line still runs the full sweep. Only the shot
		// being rendered holds its angle, which is the distinction between choosing
		// a shot and drifting through one.
		if (shotSince != heldSince) {
			heldSince = shotSince;
			heldSweep = kUnheld;
			heldStandoff = kUnheld;
			heldRoom = kUnheld;
		}
		subjects.delta = delta;
		subjects.heldSweep = heldSweep;
		subjects.heldStandoff = heldStandoff;
		subjects.heldRoom = heldRoom;

		// THE SETTING IS GATED ON HAVING SOMETHING TO HOLD, AND THAT GATE IS WHAT
		// KEEPS THE CUT HONEST.
		//
		// heldRoom is cleared three lines up on the first frame of every shot, so
		// this is false exactly then, whatever the player has the setting on. The
		// first frame therefore runs the full sweep, the wall margin, the crowd
		// test and the refusal — the camera is placed with everything the mod knows
		// how to check — and only the frames after it hold. "Ignore obstructions"
		// can never become "walk into a wall at the cut", because the frame that
		// chooses where to stand is not one of the frames that can hold.
		//
		// It also cannot leak into candidate scoring. Every Placement() call is
		// above this line and builds Subjects fresh, so a candidate is judged on
		// measurements taken now, not on a room left over from the shot it would
		// replace.
		subjects.holdPlacement = holdPlacement && heldRoom > kUnheld;

		// THE TOPIC LIST AND THE HUD WERE DRIVEN FROM HERE, AND HERE IS THE ONE PLACE
		// THEY MUST NOT BE. They live in DriveInterface now, called from Tick.
		//
		// OnThirdPersonUpdate is ThirdPersonState::Update. It stops firing the instant
		// the camera leaves third person — first person, a mount, furniture, a killcam,
		// another menu — and it returns early a few lines above whenever the camera
		// root, the player or the subject is momentarily unresolvable. Every one of
		// those is a frame on which the list freezes at whatever opacity it was easing
		// through, and on which a HUD element that came back stays back.
		//
		// Release learned this the hard way and the note is still in this file: it was
		// decided in this function too, and a force-exit left the player walking around
		// staged with letterbox bars across the screen. Anything that must hold for the
		// whole of a conversation belongs on the always-on tick. What is left here is
		// the camera, which is the one thing that genuinely wants a camera hook.

		auto pose = protectSubject ? smartFrame : Solve(currentShot, subjects);

		if (!pose.valid && !protectSubject) {
			const auto& remembered = FavoursNpc(currentShot) ? lastNpcPose : lastPlayerPose;
			if (ReusableShot(remembered.pose.valid, remembered.type, currentShot, Drawable)) {
				// Reuse the exact shot's composition and lens, translated with its
				// subject. A different shot's offset is not this shot's fallback.
				const auto anchor = FavoursNpc(currentShot) ? npcAnchor.position : playerAnchor.position;
				const RE::NiPoint3 shift{ anchor.x - remembered.anchor.x,
					anchor.y - remembered.anchor.y, anchor.z - remembered.anchor.z };
				pose = remembered.pose;
				pose.position += shift;
				pose.lookAt += shift;
			} else if (SecondsSince(enabledRetryAt) >= 0.5f) {
				enabledRetryAt = Clock::now();
				if (const auto fallback = EnabledFallback(subjects); fallback && Drawable(fallback->first)) {
					Log::Info(Log::Category::kContinuity, "Enabled fallback: {} -> {}."sv,
						Name(currentShot), Name(fallback->first));
					previousShot = currentShot;
					currentShot = fallback->first;
					pose = fallback->second;
					shotSince = Clock::now();
					heldSince = shotSince;
					heldSweep = kUnheld;
					heldStandoff = kUnheld;
					heldRoom = kUnheld;
				}
			}
		}

		if (!pose.valid || !Drawable(currentShot) || !std::isfinite(pose.sweep) ||
			(pose.sweep > kUnheld && !ShotAngles::AllowedAdjustment(pose.sweep))) {
			UseNativeView();
			return;
		}

		const bool poseOnNpc = FavoursNpc(currentShot);
		auto& slot = poseOnNpc ? lastNpcPose : lastPlayerPose;
		slot.pose = pose;
		slot.anchor = poseOnNpc ? npcAnchor.position : playerAnchor.position;
		slot.type = currentShot;
		if (pose.sweep > kUnheld) {
			if (heldSweep <= kUnheld) {
				Log::Info(Log::Category::kContinuity, "Placed {}: adjustment {:+.1f}deg."sv,
					Name(currentShot), pose.sweep);
			}
			heldSweep = pose.sweep;
		}
		if (pose.standoff > kUnheld) {
			heldStandoff = pose.standoff;
		}
		if (heldRoom <= kUnheld && pose.room > kUnheld) {
			heldRoom = pose.room;
		}

		if (pose.valid && Drawable(currentShot)) {
			nativeView = false;
			ApplyPose(root, pose, delta, a_state);

			// The lens, written every frame rather than once at the cut.
			//
			// The engine sets worldFOV itself on camera-state changes and other
			// mods write it too, so a value stamped once at the cut is quietly
			// reverted part way through a shot — and a field of view that drifts
			// back mid-take is worse than one that was never narrowed. A shot with
			// no opinion gets baseFov, which restores the player's own value rather
			// than assuming a number.
			camera->GetRuntimeData2().worldFOV = pose.lens > 1.0f ? pose.lens : baseFov;

			// A DIFFERENT LOOK PER ANGLE, IF THE PLAYER ASKED FOR ONE.
			//
			// Skipped entirely otherwise: the look was set when the conversation
			// opened and nothing here can change it, so the default path does no
			// per-frame work at all.
			//
			// Resolved on the frame rather than at the cut because a shot can
			// outlive the line that motivated it — a cadence of three to six lines
			// keeps one angle across several of them — and the guard is what keeps
			// that from re-announcing an unchanged look sixty times a second, which
			// would hold every cross-fade permanently at its first frame.
			if (lightPerShot) {
				const int look = Shot::LightOf(currentShot) >= 0 ?
									 Shot::LightOf(currentShot) :
									 Scene::FindLook(AuthoredLight(currentShot));

				if (look != lastLookApplied) {
					Scene::KeyLight::SetLook(look);
					lastLookApplied = look;
				}

				// The angle's own nudge, ADDED to the global one rather than
				// replacing it — so the two dials in [Lighting] stay a master
				// adjustment and an angle only has to say how it differs.
				Scene::KeyLight::SetOffset(
					lightOffsetX + Shot::LightOffsetX(currentShot),
					lightOffsetY + Shot::LightOffsetY(currentShot),
					lightOffsetZ + Shot::LightOffsetZ(currentShot));
			}

			// The look flips with the camera's side of the eyeline, so a key stays
			// on the same side of the FRAME through a cut instead of walking across
			// the face every time the coverage reverses.
			// Uses this shot's own sign, which differs from `side` for the
			// player's shots under true180.
			Scene::KeyLight::SetSide(SideFor(currentShot, subjects));

			// Key whoever the shot is actually on, not whoever is speaking. On a
			// reaction shot the subject is the listener, and lighting the speaker
			// instead would light someone who is off screen.
			Scene::KeyLight::Aim(pose.position, pose.lookAt, delta);
		}
	}

	std::string_view FramingLabel(Framing a_framing) noexcept
	{
		switch (a_framing) {
		case Framing::kThem: return "them"sv;
		case Framing::kYou:  return "you"sv;
		case Framing::kRoom: return "the room"sv;
		default:             return "automatic"sv;
		}
	}

	// All three set an atomic and return. See the note in Director.h: they are
	// called from the input thread, and everything they are asking for happens on
	// the next Tick.
	void Director::RequestCut() noexcept
	{
		requestCut.store(true, std::memory_order_relaxed);
	}

	void Director::RequestFraming() noexcept
	{
		requestFraming.store(true, std::memory_order_relaxed);
	}

	Framing Director::CurrentFraming() noexcept
	{
		return framing;
	}

	bool Director::Directing() noexcept
	{
		return directing;
	}

	bool Director::DialogueMenuUp() noexcept
	{
		return dialogueMenuUp.load(std::memory_order_relaxed);
	}

	bool Director::Staging() noexcept
	{
		return staging;
	}
}

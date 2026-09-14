#include "SD/Camera/Presets.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/LightRig.h"

namespace SD::Camera
{
	namespace
	{
		// THREE LOOKS, AND THE REASON THERE ARE NOT FIVE.
		//
		// There were five, each carrying twelve to sixteen setups, and they blurred
		// into one another. Two causes, and neither was the writing:
		//
		//   A LIST OF SIXTEEN IS NOT A LOOK. Past about ten setups a preset stops
		//   having a character and starts being a sample of the whole table — and
		//   the more of the table two presets each take, the more they overlap. Up
		//   Close and Over the Shoulder shared eight setups between them.
		//
		//   CUTTING EVERY LINE FLATTENS WHATEVER IS LEFT. All five shipped a cadence
		//   of one, so every look changed angle on every line and the rhythm — the
		//   thing you actually feel — was identical across all of them.
		//
		// So: three looks, two to four setups per side of the exchange, and a
		// cadence between two and five lines. Each one is now short enough to
		// describe in a sentence, which is the real test of whether it is a look at
		// all.
		//
		// Every list is built in PAIRS. A reverse shot answers the shot it follows
		// at a matching size, and the surest way to lose that is a list that stocks
		// one side of the eyeline better than the other: the camera comes back to
		// the same angle every time the turn passes, and the look reads as thin on
		// exactly half of the conversation.

		// ---- Standard -------------------------------------------------------
		//
		// Shoulder, single, reverse, and a shot of the pair to breathe. The way
		// films have shot conversations for a century, and the reason you never
		// notice it.
		constexpr std::array kStandard{
			ShotType::kOverPlayerShoulder, ShotType::kCloseUp,
			ShotType::kMediumNpc, ShotType::kThreeQuarterNpc,

			ShotType::kOverNpcShoulder, ShotType::kClosePlayer,
			ShotType::kMediumPlayer, ShotType::kThreeQuarterPlayer,

			ShotType::kTwoShot, ShotType::kProfile,
		};

		// ---- Close ----------------------------------------------------------
		//
		// THE SHIPPED LOOK, and the shortest list in the mod: six setups.
		//
		// Rebuilt for 1.4 and cut from ten. A conversation is two faces, and this
		// look says so — a shoulder to place the two of you, a close single on each
		// side, an extreme on each side for the lines somebody means, and exactly
		// one shot of the room, drawn rarely, so the eye has somewhere to land
		// without the look stopping being close.
		//
		// EVERY DEFAULT IN THE MOD POINTS AT THIS LIST. See DefaultPreset: the
		// settings loader falls back to these setups' own lenses, weights, moves
		// and lighting, so a fresh profile reads as Close with nothing in
		// SD_user.ini at all.
		constexpr std::array kClose{
			ShotType::kOverPlayerShoulder, ShotType::kCloseUp, ShotType::kExtremeClose,

			ShotType::kClosePlayer, ShotType::kExtremeClosePlayer,

			ShotType::kDistant,
		};

		// ---- Room -----------------------------------------------------------
		//
		// Where this is happening, first; who is saying it, second.
		//
		// THE MEDIUM AND THE THREE-QUARTER ARE LOAD-BEARING, not filler. Every
		// other setup in this list is gated behind `roomy`, so in a corridor or a
		// small cell they are all switched off at once — and without a pair of
		// angles that need no room, this preset would collapse to a single
		// repeated shot per side in exactly the interiors most of Skyrim is made
		// of. They are the floor under the look, weighted low so they stay out of
		// the way anywhere the room actually opens up.
		constexpr std::array kRoom{
			ShotType::kLongNpc, ShotType::kOverhead,
			ShotType::kMediumNpc, ShotType::kThreeQuarterNpc,

			ShotType::kLongPlayer, ShotType::kPlayerOverhead,
			ShotType::kMediumPlayer, ShotType::kThreeQuarterPlayer,

			ShotType::kMaster, ShotType::kWide,
			ShotType::kDistant, ShotType::kTwoShot,
		};

		// ---- The glass ------------------------------------------------------
		//
		// The column that separates one look from another, and the one a preset
		// could not reach before it existed: two presets drawing the same setup
		// were shooting it identically whatever their descriptions claimed.
		//
		// Standard holds 50-72, Close 40-62, Room 45-95. Every setup a preset uses
		// is listed even where the number matches the table, so each block reads as
		// a complete statement rather than a diff nobody can check without the
		// table open beside it — and a static_assert below enforces it.
		//
		// THE CEILING THIS AUTHORING RESPECTS. Distance is solved for the fill AT
		// THE CHOSEN LENS and then floored at the subject's minimum, so a tight
		// fill on wide glass asks to stand closer than the floor allows, gets
		// clamped, and renders LOOSER than authored. Roughly: 66 degrees at 0.85
		// fill, 80 at 0.68, no constraint below 0.46. Over-the-shoulders are exempt
		// — their standoff is forced past the other participant regardless.

		constexpr std::array kStandardLens{
			Lens{ ShotType::kOverPlayerShoulder, 55 }, Lens{ ShotType::kOverNpcShoulder, 55 },
			Lens{ ShotType::kCloseUp, 50 },            Lens{ ShotType::kClosePlayer, 50 },
			Lens{ ShotType::kMediumNpc, 62 },          Lens{ ShotType::kMediumPlayer, 62 },
			Lens{ ShotType::kThreeQuarterNpc, 58 },    Lens{ ShotType::kThreeQuarterPlayer, 58 },
			Lens{ ShotType::kTwoShot, 72 },            Lens{ ShotType::kProfile, 60 },
		};

		// Long glass throughout — 35 to 50 — which is what makes this look
		// compressed and flattering rather than merely near. The extremes must not
		// be raised past 66: they ask for 0.85 of frame height, and on wider glass
		// they clamp at the distance floor and render looser than the setups they
		// are supposed to be the tightest of.
		//
		// The distant shot is on the same glass as the extreme close and that is
		// the point: a long lens across a room compresses it, so the one wide in
		// this look belongs to it rather than reading as a different mod.
		constexpr std::array kCloseLens{
			Lens{ ShotType::kOverPlayerShoulder, 40 },
			Lens{ ShotType::kCloseUp, 50 },
			Lens{ ShotType::kExtremeClose, 40 },
			Lens{ ShotType::kClosePlayer, 40 },
			Lens{ ShotType::kExtremeClosePlayer, 35 },
			Lens{ ShotType::kDistant, 40 },
		};

		// The distant shot is the odd one out at 45 and that is deliberate: a long
		// lens across a room compresses it and reads as surveillance, which is the
		// one image in this look that is not simply "wide".
		constexpr std::array kRoomLens{
			Lens{ ShotType::kMaster, 95 },             Lens{ ShotType::kWide, 88 },
			Lens{ ShotType::kDistant, 45 },            Lens{ ShotType::kTwoShot, 80 },
			Lens{ ShotType::kLongNpc, 80 },            Lens{ ShotType::kLongPlayer, 80 },
			Lens{ ShotType::kOverhead, 85 },           Lens{ ShotType::kPlayerOverhead, 85 },
			Lens{ ShotType::kMediumNpc, 70 },          Lens{ ShotType::kMediumPlayer, 70 },
			Lens{ ShotType::kThreeQuarterNpc, 72 },    Lens{ ShotType::kThreeQuarterPlayer, 72 },
		};

		// ---- How often ------------------------------------------------------
		//
		// The table's weights are authored for the whole vocabulary — eight staples
		// at 100, everything else at 50 — and that ratio means nothing once a
		// preset has cut the list to ten. Weight also decides whether the room
		// appears AT ALL: Coverage rolls the environmental pool's total against the
		// active coverage pool's total, so the share of a conversation given to the
		// space is the sum of what its setups are worth.

		constexpr std::array kStandardWeight{
			Weight{ ShotType::kOverPlayerShoulder, 100 }, Weight{ ShotType::kOverNpcShoulder, 100 },
			Weight{ ShotType::kCloseUp, 80 },             Weight{ ShotType::kClosePlayer, 80 },
			Weight{ ShotType::kThreeQuarterNpc, 60 },     Weight{ ShotType::kThreeQuarterPlayer, 60 },
			Weight{ ShotType::kMediumNpc, 45 },           Weight{ ShotType::kMediumPlayer, 45 },
			Weight{ ShotType::kTwoShot, 25 },             Weight{ ShotType::kProfile, 15 },
		};

		// THE EXTREMES OUTWEIGH THE CLOSES, WHICH LOOKS BACKWARDS AND IS NOT.
		//
		// Both extremes are gated on an intensity the writer raised by hand, which
		// the emotion scan puts at 5-8% of authored responses — so the weight is
		// not competing across the whole conversation, only across the handful of
		// lines that reach the gate at all. Weighted level with the closes they
		// would lose most of those, and the one moment this look exists for would
		// come up about as often as an accent.
		//
		// The room is at 10 against a coverage total of 250, so roughly one line in
		// twenty-six goes wide. That is a place for the eye to land, not a habit.
		constexpr std::array kCloseWeight{
			Weight{ ShotType::kOverPlayerShoulder, 34 },
			Weight{ ShotType::kCloseUp, 36 },
			Weight{ ShotType::kExtremeClose, 75 },
			Weight{ ShotType::kClosePlayer, 30 },
			Weight{ ShotType::kExtremeClosePlayer, 75 },
			Weight{ ShotType::kDistant, 10 },
		};

		// The room outweighs the faces, which is the whole claim of the look and
		// the thing the old version of this preset got backwards — it shipped its
		// master as an accent and its close-up as a staple, so the preset named for
		// the room spent most of every speech on somebody's face.
		constexpr std::array kRoomWeight{
			Weight{ ShotType::kMaster, 85 },            Weight{ ShotType::kWide, 80 },
			Weight{ ShotType::kLongNpc, 70 },           Weight{ ShotType::kLongPlayer, 70 },
			Weight{ ShotType::kDistant, 55 },           Weight{ ShotType::kTwoShot, 45 },
			Weight{ ShotType::kOverhead, 40 },          Weight{ ShotType::kPlayerOverhead, 40 },
			Weight{ ShotType::kThreeQuarterNpc, 40 },   Weight{ ShotType::kThreeQuarterPlayer, 40 },
			Weight{ ShotType::kMediumNpc, 35 },         Weight{ ShotType::kMediumPlayer, 35 },
		};

		// ---- What moves -----------------------------------------------------
		//
		// The half of a look the shot list cannot express. Each preset states a
		// baseline and then names its exceptions; anything unlisted takes the
		// baseline.

		// Locked off, with two exceptions that mean something BECAUSE everything
		// else is still. Both close-ups creep, so the pair still match.
		constexpr std::array kStandardMotion{
			Motion{ ShotType::kCloseUp, Move::kPushIn, 14, 500 },
			Motion{ ShotType::kClosePlayer, Move::kPushIn, 14, 500 },
			Motion{ ShotType::kTwoShot, Move::kDrift, 10, 700 },
		};

		// FIVE OF THE SIX MOVE, AND NO TWO OF THEM THE SAME WAY.
		//
		// The shoulder is the only setup here on sticks, and it is the one the eye
		// uses to place the two of you — a shot that establishes should not also be
		// drifting. Everything after it is a face, and each gets a different kind
		// of pressure: the NPC single creeps physically closer, the extreme
		// magnifies without moving, the reverse pulls its lens back off the player
		// as they choose, their extreme drives in hard, and the one wide breathes
		// outward so the room opens rather than closes.
		//
		// THE INERT PAIR ON THE LOCKED SETUP IS CANONICAL AND MUST NOT DRIFT.
		// Amount 0 and time 400 are what a shot with no move stores. Nothing reads
		// either — a locked setup moves nowhere over any duration — but preset
		// comparison reads both, so leaving them unstated would mean the same look
		// applied twice reported different drift depending on what the setup
		// happened to be carrying beforehand.
		constexpr std::array kCloseMotion{
			Motion{ ShotType::kOverPlayerShoulder, Move::kLocked, 0, 400 },
			Motion{ ShotType::kCloseUp, Move::kPushIn, 40, 800 },
			Motion{ ShotType::kExtremeClose, Move::kZoomIn, 40, 900 },
			Motion{ ShotType::kClosePlayer, Move::kZoomOut, 20, 500 },
			Motion{ ShotType::kExtremeClosePlayer, Move::kPushIn, 75, 700 },
			Motion{ ShotType::kDistant, Move::kPullOut, 55, 800 },
		};

		// The wides move and the singles do not, which is the inverse of Standard
		// and is what makes the room feel like the subject.
		constexpr std::array kRoomMotion{
			Motion{ ShotType::kMaster, Move::kCraneUp, 40, 900 },
			Motion{ ShotType::kWide, Move::kPullOut, 16, 800 },
			Motion{ ShotType::kLongNpc, Move::kCraneUp, 25, 700 },
			Motion{ ShotType::kLongPlayer, Move::kCraneUp, 25, 700 },
			Motion{ ShotType::kTwoShot, Move::kDrift, 12, 800 },
		};

		[[nodiscard]] constexpr bool Names(std::span<const ShotType> a_shots, ShotType a_type)
		{
			for (const auto shot : a_shots) {
				if (shot == a_type) {
					return true;
				}
			}
			return false;
		}

		// A SETUP WITH NO LENS NAMED FOR IT falls through to whatever the table
		// ships, which is very often the wrong glass for the look. Adding a shot to
		// a list and forgetting its lens would put it on screen at 72 degrees in
		// the middle of a look that runs at 45, and nothing anywhere would say so.
		//
		// The same hole exists for weight and is worse: an unnamed setup inherits
		// the table's staple-or-accent tier, authored against all thirty-nine and
		// arbitrary inside a list of ten.
		template <typename T>
		[[nodiscard]] constexpr bool EveryShotIsNamed(
			std::span<const ShotType> a_shots, std::span<const T> a_entries)
		{
			for (const auto shot : a_shots) {
				bool found = false;
				for (const auto& entry : a_entries) {
					if (entry.shot == shot) {
						found = true;
						break;
					}
				}
				if (!found) {
					return false;
				}
			}
			return true;
		}

		// AN EXCEPTION NAMING A SETUP THE PRESET DOES NOT USE is dead weight that
		// reads as intent. Both lookups only run for shots the preset switched on,
		// so a stale entry does nothing at all while still describing a move to
		// anybody reading it.
		template <typename T>
		[[nodiscard]] constexpr bool AllNamedAreUsed(
			std::span<const ShotType> a_shots, std::span<const T> a_entries)
		{
			for (const auto& entry : a_entries) {
				if (!Names(a_shots, entry.shot)) {
					return false;
				}
			}
			return true;
		}

		// THE ONE RULE FOR EVERY WORD BELOW: somebody who has never been on a film
		// set has to be able to read it. Say what will be on screen, not what the
		// technique is called.
		//
		// The long descriptions are gone. A look that needs four paragraphs to
		// explain itself is not a look, and the three below are each short enough
		// to say in a sentence — which is the test they were rebuilt to pass.
		//
		// ---- The light ------------------------------------------------------
		//
		// The fourth axis, and the one that finally makes two presets drawing the
		// same setup at the same lens different images.
		//
		// ANYTHING ABSENT TAKES WHAT THE SETUP SHIPS UNDER, which is why these are
		// short. A preset only speaks up where it disagrees with the shot table,
		// and most of the time it does not: a close-up wants Soft under nearly
		// every look, and restating that in three places would be three chances to
		// drift.
		//
		// Standard names nothing at all, deliberately. It is the look the authored
		// defaults describe, and a table asserting them again would be a copy that
		// silently stops matching the day one of them changes.
		constexpr std::array<Light, 0> kStandardLight{};

		// Close is warm and it is soft, and it holds that through the whole
		// conversation — a look this tight has nothing else in frame to carry a
		// mood, so the light on the face has to be it. Firelight on the singles is
		// the strongest statement any of these three makes, and it is what stops
		// the preset reading as merely "the same shots, nearer".
		//
		// The extremes stay Hard. They are gated to intensity-100 lines and there
		// is no gentle version of one.
		constexpr std::array kCloseLight{
			Light{ ShotType::kOverPlayerShoulder, "soft" },
			Light{ ShotType::kCloseUp, "soft" },
			Light{ ShotType::kClosePlayer, "soft" },
			Light{ ShotType::kExtremeClose, "hard" },
			Light{ ShotType::kExtremeClosePlayer, "hard" },

			// Nothing on the one wide. A key aimed at a head from across a room
			// lights a speck and spills over everything between it and the lens,
			// which is the one way a lighting feature announces itself as a mod.
			Light{ ShotType::kDistant, "off" },
		};

		// Room lights almost nothing, and that is the entire point rather than an
		// omission. This look is about the space, the space is already lit by the
		// people who built it, and a key light on a figure thirty feet away is a
		// bright patch on a floor that announces a mod is running. The two setups
		// that do get anything are the ones where a face is still large enough to
		// be worth modelling.
		constexpr std::array kRoomLight{
			Light{ ShotType::kMaster, "off" },
			Light{ ShotType::kWide, "off" },
			Light{ ShotType::kDistant, "off" },
			Light{ ShotType::kOverhead, "off" },
			Light{ ShotType::kPlayerOverhead, "off" },
			Light{ ShotType::kLongNpc, "natural" },
			Light{ ShotType::kLongPlayer, "natural" },
			Light{ ShotType::kTwoShot, "natural" },
			Light{ ShotType::kMediumNpc, "natural" },
			Light{ ShotType::kMediumPlayer, "natural" },
			Light{ ShotType::kThreeQuarterNpc, "natural" },
			Light{ ShotType::kThreeQuarterPlayer, "natural" },
		};

		// THE CLOSE STYLE IS THE MOD'S DEFAULTS, WRITTEN ONCE.
		//
		// Named rather than spelled inline because three other places have to agree
		// with it exactly: Camera::Tunables' field initialisers, the fallbacks in
		// Director::ReadTuning, and config/SD.ini. A fresh install reads all three
		// and has to land on numbers this struct would report zero drift against,
		// or the Presets page opens with nothing ticked.
		//
		//                            min   max  cutMin cutMax  perLine short  spk    choose
		constexpr Style kCloseStyle{ 240, 900, 3, 6, true, true, false, false };

		constexpr std::array kAll{
			Preset{ "standard", "Standard",
				"Over the shoulder, then close on whoever is talking.",
				"Ten angles, mostly locked off. Both close-ups creep slowly forward "
				"and the shot of the pair drifts; everything else is on sticks.",
				Style{ 240, 800, 2, 3, true, true, false, false },
				kStandard,
				Move::kLocked, 0, 400, kStandardMotion, kStandardLens, kStandardWeight, kStandardLight },

			Preset{ "close", "Close",
				"Tight on faces, and never far from one.",
				"Six angles: a shoulder to place you, a close single and an extreme "
				"on each side, and one shot from across the room, drawn rarely.",
				kCloseStyle,
				kClose,
				Move::kLocked, 0, 400, kCloseMotion, kCloseLens, kCloseWeight, kCloseLight },

			Preset{ "room", "Room",
				"Where you are, more than who is talking.",
				"Masters, wides and full figures on very wide glass, with cranes on "
				"the biggest of them. Holds each angle longer and cuts less often.",
				Style{ 300, 1000, 3, 5, true, true, false, false },
				kRoom,
				Move::kLocked, 0, 600, kRoomMotion, kRoomLens, kRoomWeight, kRoomLight },
		};

		// The index of "close" in kAll, checked rather than trusted. Reordering the
		// table without moving this is a build error rather than a mod that ships
		// pointing its defaults at the wrong look.
		constexpr std::size_t kDefaultPresetIndex = 1;
		static_assert(kDefaultPresetIndex < kAll.size() &&
				std::string_view{ kAll[kDefaultPresetIndex].key } == "close",
			"DefaultPreset must be Close: the shipped ini, the C++ fallbacks and the "
			"menu reset are all written to agree with it.");

		// And the style it carries has to be the one written above rather than a
		// copy that drifted.
		static_assert(kAll[kDefaultPresetIndex].style.cutEveryMin == kCloseStyle.cutEveryMin &&
				kAll[kDefaultPresetIndex].style.cutEveryMax == kCloseStyle.cutEveryMax &&
				kAll[kDefaultPresetIndex].style.perLineAngleChange &&
				kAll[kDefaultPresetIndex].style.holdOnShortLines &&
				!kAll[kDefaultPresetIndex].style.timedCutsWhileSpeaking &&
				!kAll[kDefaultPresetIndex].style.timedCutsWhileChoosing,
			"The shipped defaults are per-line angle changes at 3-6 lines, short lines "
			"ignored, and both timers off.");

		static_assert(EveryShotIsNamed<Lens>(kStandard, kStandardLens) &&
				EveryShotIsNamed<Lens>(kClose, kCloseLens) &&
				EveryShotIsNamed<Lens>(kRoom, kRoomLens),
			"Every setup a preset shoots with needs its lens named, or it silently "
			"takes the shot table's default in the middle of a look that does not "
			"want it.");

		static_assert(EveryShotIsNamed<Weight>(kStandard, kStandardWeight) &&
				EveryShotIsNamed<Weight>(kClose, kCloseWeight) &&
				EveryShotIsNamed<Weight>(kRoom, kRoomWeight),
			"Every setup a preset shoots with needs its how-often named, or it "
			"inherits a staple-or-accent tier authored against all thirty-nine "
			"setups and meaningless inside a list of ten.");

		static_assert(AllNamedAreUsed<Lens>(kStandard, kStandardLens) &&
				AllNamedAreUsed<Lens>(kClose, kCloseLens) &&
				AllNamedAreUsed<Lens>(kRoom, kRoomLens),
			"A preset names a lens for a setup it does not use. Nothing will read "
			"it.");

		static_assert(AllNamedAreUsed<Weight>(kStandard, kStandardWeight) &&
				AllNamedAreUsed<Weight>(kClose, kCloseWeight) &&
				AllNamedAreUsed<Weight>(kRoom, kRoomWeight),
			"A preset names a how-often for a setup it does not use. Nothing will "
			"read it.");

		static_assert(AllNamedAreUsed<Motion>(kStandard, kStandardMotion) &&
				AllNamedAreUsed<Motion>(kClose, kCloseMotion) &&
				AllNamedAreUsed<Motion>(kRoom, kRoomMotion),
			"A preset names a move for a setup it does not use. Nothing will read "
			"it.");

		// Every shot key a preset could switch on or off. A preset writes the whole
		// table, not just its own list, or applying a narrow one after a broad one
		// would leave the broad one's extras enabled and the two would blur into
		// each other — which is its own version of "they all feel the same".
		[[nodiscard]] bool InList(std::span<const ShotType> a_shots, ShotType a_type)
		{
			for (const auto shot : a_shots) {
				if (shot == a_type) {
					return true;
				}
			}
			return false;
		}

		// What this preset wants this shot doing: its own named exception if it has
		// one, otherwise the look's baseline.
		//
		// One function so apply and drift cannot disagree. They did once, on the
		// shot list, and a preset that applied one set and was then measured against
		// another can never report itself as active.
		[[nodiscard]] Motion MotionFor(const Preset& a_preset, ShotType a_type)
		{
			return PresetMotion(a_preset, a_type);
		}
	}

	Motion PresetMotion(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& m : a_preset.motion) {
			if (m.shot == a_type) {
				return m;
			}
		}
		return Motion{ a_type, a_preset.baseMove, a_preset.baseAmount, a_preset.baseTime };
	}

	// Falls back to the TABLE rather than to some per-preset baseline, and there
	// is no useful baseline to fall back to instead: a single number applied to
	// every setup is exactly the global lens bias that was removed, and it
	// flattens the difference between a close-up and a master rather than
	// expressing anything about a look.
	int PresetLens(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& l : a_preset.lenses) {
			if (l.shot == a_type) {
				return std::clamp(l.degrees, kMinLens, kMaxLens);
			}
		}
		return static_cast<int>(AuthoredLens(a_type));
	}

	int PresetWeight(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& w : a_preset.weights) {
			if (w.shot == a_type) {
				return std::clamp(w.weight, 0, 100);
			}
		}
		return AuthoredWeight(a_type);
	}

	const char* PresetLight(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& l : a_preset.lights) {
			if (l.shot == a_type && l.look && *l.look) {
				return l.look;
			}
		}
		return AuthoredLight(a_type);
	}
	std::span<const Preset> AllPresets()
	{
		return kAll;
	}

	const Preset& DefaultPreset()
	{
		return kAll[kDefaultPresetIndex];
	}

	bool PresetUses(const Preset& a_preset, ShotType a_type)
	{
		return InList(a_preset.shots, a_type);
	}

	const Preset* FindPreset(std::string_view a_key)
	{
		for (const auto& preset : kAll) {
			if (a_key == preset.key) {
				return &preset;
			}
		}
		return nullptr;
	}

	// MEASURED AGAINST LIVE STATE, NEVER AGAINST THE INI, AND THAT IS BOTH A
	// PERFORMANCE FIX AND A CORRECTNESS ONE.
	//
	// This used to ask Config for every value, which is a profile read apiece and
	// a hooked file operation apiece under Mod Organizer's virtual filesystem.
	// That was tolerable at eight dials plus a flag per shot. It stopped being
	// tolerable the moment the move started counting: three more reads for every
	// enabled shot, times forty-odd shots, times five presets, is comfortably
	// eight hundred file operations every refresh — and the Presets page went
	// visibly sluggish the build that landed.
	//
	// Live state answers all of it from arrays. It is also the more honest
	// question: the page is telling somebody how far their CAMERA is from a look,
	// and the camera runs on what is loaded, not on what is on disk.
	int PresetDrift(const Preset& a_preset)
	{
		const auto& s = a_preset.style;
		const auto  live = Director::GetTunables();
		int         drift = 0;

		const auto differs = [&drift](auto a_have, auto a_want) {
			if (a_have != a_want) {
				++drift;
			}
		};

		differs(live.minShotTime, s.minShotTime);
		differs(live.maxShotTime, s.maxShotTime);
		differs(live.cutEveryMin, s.cutEveryMin);
		differs(live.cutEveryMax, s.cutEveryMax);
		differs(live.perLineAngleChange, s.perLineAngleChange);
		differs(live.holdOnShortLines, s.holdOnShortLines);
		differs(live.timedCutsWhileSpeaking, s.timedCutsWhileSpeaking);
		differs(live.timedCutsWhileChoosing, s.timedCutsWhileChoosing);

		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			const auto type = static_cast<ShotType>(i);
			const bool want = InList(a_preset.shots, type);
			differs(Shot::Enabled(type), want);

			// A shot this preset does not use has no opinion about how it moves.
			// Counting the move on a switched-off setup would report drift for a
			// setting that cannot affect anything, which is how a preset ends up
			// looking modified for reasons nobody can find on screen.
			if (!want) {
				continue;
			}

			// The move counts, and it has to. Without it a preset read as
			// unmodified after every move in it had been retuned, because the only
			// thing the check knew about was which shots were switched on.
			const auto wanted = MotionFor(a_preset, type);
			differs(Shot::MoveOf(type), wanted.move);
			differs(Shot::MoveAmount(type), wanted.amount);
			differs(Shot::MoveTime(type), wanted.time);

			// And the glass, for exactly the same reason. It is the axis these
			// looks now differ on most, so a check blind to it would call two
			// presets identical at the point they are furthest apart.
			differs(Shot::Lens(type), PresetLens(a_preset, type));

			// And how often. A look whose room shots have been dialled to zero
			// is not that look any more, however intact its shot list is.
			differs(Shot::Weight(type), PresetWeight(a_preset, type));

			// And how it is lit. Compared as indices rather than as names because
			// that is what is live — a name comparison would report drift for a
			// difference in spelling that resolves to the same rig.
			differs(Shot::LightOf(type), Scene::FindLook(PresetLight(a_preset, type)));
		}

		return drift;
	}

	const Preset* ActivePreset()
	{
		for (const auto& preset : kAll) {
			if (PresetDrift(preset) == 0) {
				return &preset;
			}
		}
		return nullptr;
	}

	void ApplyPreset(const Preset& a_preset)
	{
		const auto& s = a_preset.style;

		Config::SetInt("Direction", "iMinShotTime", s.minShotTime);
		Config::SetInt("Direction", "iMaxShotTime", s.maxShotTime);
		Config::SetInt("Direction", "iCutEveryMin", s.cutEveryMin);
		Config::SetInt("Direction", "iCutEveryMax", s.cutEveryMax);
		Config::SetBool("Direction", "bPerLineAngleChange", s.perLineAngleChange);

		// bCoverPlayerTurn is deliberately NOT written here. Every preset needs the
		// player's own turn covered — that is not a stylistic axis, it is the
		// difference between a dialogue camera and a landscape camera — so it stays
		// a single global setting rather than something five presets each assert.
		// Applying a preset must never quietly turn it off.
		Config::SetBool("Direction", "bHoldOnShortLines", s.holdOnShortLines);
		Config::SetBool("Direction", "bTimedCutsWhileSpeaking", s.timedCutsWhileSpeaking);
		Config::SetBool("Direction", "bTimedCutsWhileChoosing", s.timedCutsWhileChoosing);

		int on = 0;
		int moving = 0;
		int narrowest = kMaxLens;
		int widest = kMinLens;
		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			const auto type = static_cast<ShotType>(i);
			const bool want = InList(a_preset.shots, type);
			Config::SetBool("Shots", Key(type), want);
			Shot::SetEnabled(type, want);
			if (!want) {
				continue;
			}
			++on;

			// The half of a look the shot list could never say.
			//
			// Written only for setups the preset actually uses: stamping a move onto
			// a switched-off shot would quietly overwrite whatever the player had
			// tuned there, and they would find it changed the next time they enabled
			// it with nothing on screen to explain why.
			const auto m = MotionFor(a_preset, type);
			Config::SetInt("Shots", MoveKey(type), static_cast<int>(m.move));
			Config::SetInt("Shots", MoveAmountKey(type), m.amount);
			Config::SetInt("Shots", MoveTimeKey(type), m.time);
			Shot::SetMove(type, m.move);
			Shot::SetMoveAmount(type, m.amount);
			Shot::SetMoveTime(type, m.time);
			if (m.move != Move::kLocked && m.amount > 0) {
				++moving;
			}

			// The glass. Written for every setup the preset uses, including the
			// ones it does not name — those get the table's own lens put back,
			// so applying a look twice with a different one in between gives the
			// same result both times. A preset that only wrote the lenses it
			// mentioned would inherit the previous look's numbers on everything
			// else and could never be reproduced.
			const int lens = PresetLens(a_preset, type);
			Config::SetInt("Shots", LensKey(type), lens);
			Shot::SetLens(type, lens);
			narrowest = std::min(narrowest, lens);
			widest = std::max(widest, lens);

			// And how often it comes up, which is what decides how much of the
			// look each of its angles actually is — including, for the room
			// setups, whether the camera ever leaves the two faces at all.
			const int weight = PresetWeight(a_preset, type);
			Config::SetInt("Shots", WeightKey(type), weight);
			Shot::SetWeight(type, weight);

			// And how it is lit. Written as a name and resolved to an index in the
			// same breath, so a preset takes hold on the conversation it was
			// pressed in rather than on the one after — LoadSettings below would
			// get there eventually, but a player pressing four looks in a row is
			// judging each by the last one's lighting until it does.
			const char* rig = PresetLight(a_preset, type);
			Config::SetString("Shots", LightKey(type), rig);
			const int rigIndex = Scene::FindLook(rig);
			Shot::SetLight(type, rigIndex >= 0 ? rigIndex : Scene::DefaultLook());
		}

		// Push the dials live as well as writing them. Without this the preset
		// would only take hold on the next conversation, and somebody trying four
		// of them in a row would be judging each one by the last one's timing.
		Director::LoadSettings();

		// The lens range goes in the line because it is the half of a look that
		// cannot be inferred from anything else here. Two presets with the same
		// shot count and the same cutting are still nothing alike at 40 degrees
		// and at 100, and this is the only place the log would ever say so.
		Log::Info(Log::Category::kCamera,
			"Preset '{}' applied: {} setup(s) on, {} of them moving, {}-{} degrees, "
			"shot {:.2f}-{:.2f}s."sv,
			a_preset.key, on, moving, narrowest, widest,
			static_cast<double>(s.minShotTime) / 100.0,
			static_cast<double>(s.maxShotTime) / 100.0);
	}

	// ---- Your own presets -------------------------------------------------
	//
	// Stored as one string per slot rather than as two hundred keys.
	//
	// A snapshot is eight dials plus four numbers for each of forty-odd shots, and
	// spelling that out would put six hundred lines of machine data into a file
	// whose whole value is that a person can open it and read it. Nobody hand-edits
	// a saved snapshot; they save it again. So the readable half — the name — is
	// its own key, and the payload is one line that says up front it is not for
	// editing.
	//
	// Versioned because it will change. A shot added to the middle of the enum
	// would silently shift every entry after it, so the reader checks the version
	// and refuses rather than applying a scrambled set. Refusing to load somebody's
	// saved look is a disappointment; loading it wrong is a bug report.
	namespace
	{
		// A SLOT MUST HOLD EVERYTHING A PRESET CAN WRITE, or it cannot reproduce
		// the look it was taken from: it comes back wearing whatever the last
		// preset applied left behind on the fields it forgot.
		//
		// 2 added the per-setup lens. 3 added how-often, when presets learned to
		// set that too — and that one matters more than it looks, because the
		// room's share of a conversation is the sum of its setups' weights. A
		// slot saved off Show the Room and restored without them would come back
		// as a shot list full of wides that the camera almost never cuts to.
		//
		// OLDER PAYLOADS STILL LOAD. Each version simply carries fewer numbers
		// per setup, and the ones it does not carry are LEFT AS THEY ARE rather
		// than guessed at — a slot saved before a field existed has no opinion
		// about it, and stamping the table's defaults over the player's tuning
		// would be inventing one.
		constexpr int         kSlotVersion = 3;
		constexpr std::size_t kSlotStride = 6;  // enabled, move, amount, time, lens, weight

		// What each version wrote per setup. Index is the version.
		constexpr std::array<std::size_t, 4> kStrideForVersion{ 0, 4, 5, kSlotStride };

		[[nodiscard]] const char* SlotNameKey(int a_index)
		{
			static std::array<std::string, kCustomSlots> cache;
			auto&                                       key = cache[static_cast<std::size_t>(a_index)];
			if (key.empty()) {
				key = "sSlot" + std::to_string(a_index + 1) + "Name";
			}
			return key.c_str();
		}

		[[nodiscard]] const char* SlotDataKey(int a_index)
		{
			static std::array<std::string, kCustomSlots> cache;
			auto&                                       key = cache[static_cast<std::size_t>(a_index)];
			if (key.empty()) {
				key = "sSlot" + std::to_string(a_index + 1);
			}
			return key.c_str();
		}

		// LIGHTING IS SAVED BESIDE THE PAYLOAD RATHER THAN INSIDE IT.
		//
		// The slot payload is a comma-separated list of INTEGERS with a stride per
		// version, and adding the rig to it would mean storing an ordinal — which
		// is the one thing the rig table is specifically designed not to need. A
		// shot names its rig so the table can be reordered and extended freely; a
		// slot that stored index 4 would quietly relight itself the first time a
		// rig was inserted above it, and it would do so silently and for good.
		//
		// So the names go in their own key, comma-separated, one per setup in shot
		// order. An absent key is an older slot and means "this slot has nothing to
		// say about lighting" — which is left alone rather than stamped with
		// defaults, exactly as the stride guard treats a payload that predates the
		// lens and the weight.
		[[nodiscard]] const char* SlotLightKey(int a_index)
		{
			static std::array<std::string, kCustomSlots> cache;
			auto&                                       key = cache[static_cast<std::size_t>(a_index)];
			if (key.empty()) {
				key = "sSlot" + std::to_string(a_index + 1) + "Lights";
			}
			return key.c_str();
		}

		[[nodiscard]] std::vector<std::string> SplitNames(std::string_view a_text, char a_delim)
		{
			std::vector<std::string> out;
			std::size_t              start = 0;
			while (start <= a_text.size()) {
				const auto end = a_text.find(a_delim, start);
				const auto piece = a_text.substr(start,
					end == std::string_view::npos ? std::string_view::npos : end - start);
				out.emplace_back(piece);
				if (end == std::string_view::npos) {
					break;
				}
				start = end + 1;
			}
			return out;
		}

		[[nodiscard]] bool ValidSlot(int a_index)
		{
			return a_index >= 0 && a_index < kCustomSlots;
		}

		// Everything a slot holds, from LIVE state, as one string.
		//
		// One function for both jobs: writing a slot, and asking whether a slot is
		// the thing currently running. If those two ever built the string
		// differently, a slot could never report itself as in use — the same trap
		// apply and drift fell into on the shot list.
		[[nodiscard]] std::string Snapshot()
		{
			const auto  live = Director::GetTunables();
			std::string data = std::to_string(kSlotVersion);

			const auto add = [&data](int a_value) {
				data += ',';
				data += std::to_string(a_value);
			};

			add(live.minShotTime);
			add(live.maxShotTime);
			add(live.cutEveryMin);
			add(live.cutEveryMax);
			// Slot ordinal 5 held bCutOnLineEnd up to 1.3 and holds
			// bPerLineAngleChange from 1.4. The slot version is unchanged on
			// purpose: the field is still one boolean at the same offset, and every
			// value stored in it is still a legal value for the new one — a slot
			// saved with line-end cuts on comes back with per-line angle changes on,
			// which is the closer of the two readings of "this look cut on lines".
			add(live.perLineAngleChange ? 1 : 0);
			add(live.holdOnShortLines ? 1 : 0);
			add(live.timedCutsWhileSpeaking ? 1 : 0);
			add(live.timedCutsWhileChoosing ? 1 : 0);

			// The shot count goes in so the reader can tell a truncated line from
			// one written by a build with a different table.
			add(static_cast<int>(ShotType::kCount));

			for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
				const auto type = static_cast<ShotType>(i);
				add(Shot::Enabled(type) ? 1 : 0);
				add(static_cast<int>(Shot::MoveOf(type)));
				add(Shot::MoveAmount(type));
				add(Shot::MoveTime(type));
				add(Shot::Lens(type));
				add(Shot::Weight(type));
			}

			return data;
		}

		[[nodiscard]] std::vector<int> SplitInts(std::string_view a_text, char a_sep)
		{
			std::vector<int> out;
			int              value = 0;
			bool             any = false;
			bool             negative = false;
			for (const char c : a_text) {
				if (c == a_sep) {
					out.push_back(negative ? -value : value);
					value = 0;
					any = false;
					negative = false;
				} else if (c == '-' && !any) {
					negative = true;
				} else if (c >= '0' && c <= '9') {
					value = value * 10 + (c - '0');
					any = true;
				}
			}
			out.push_back(negative ? -value : value);
			return out;
		}
	}

	CustomSlot ReadCustomSlot(int a_index)
	{
		CustomSlot slot{};
		if (!ValidSlot(a_index)) {
			return slot;
		}

		slot.index = a_index;
		slot.name = Config::String("CustomPresets", SlotNameKey(a_index), "");
		slot.used = !slot.name.empty() &&
			!Config::String("CustomPresets", SlotDataKey(a_index), "").empty();
		return slot;
	}

	void SaveCustomSlot(int a_index, std::string_view a_name)
	{
		if (!ValidSlot(a_index)) {
			return;
		}

		const auto data = Snapshot();
		Config::SetString("CustomPresets", SlotDataKey(a_index), data.c_str());
		Config::SetString("CustomPresets", SlotNameKey(a_index), std::string{ a_name }.c_str());

		// The rigs, by name, in shot order. See SlotLightKey.
		std::string lights;
		const auto  rigs = Scene::AllLooks();
		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			if (i != 0) {
				lights += ',';
			}
			const auto type = static_cast<ShotType>(i);
			const int  rig = Shot::LightOf(type);
			lights += rig >= 0 && static_cast<std::size_t>(rig) < rigs.size() ?
						  rigs[static_cast<std::size_t>(rig)].key :
						  AuthoredLight(type);
		}
		Config::SetString("CustomPresets", SlotLightKey(a_index), lights.c_str());

		Log::Info(Log::Category::kCamera,
			"Saved your settings to slot {} as '{}'."sv, a_index + 1, a_name);
	}

	void ApplyCustomSlot(int a_index)
	{
		if (!ValidSlot(a_index)) {
			return;
		}

		const auto raw = Config::String("CustomPresets", SlotDataKey(a_index), "");
		if (raw.empty()) {
			return;
		}

		const auto values = SplitInts(raw, ',');

		// 1 version + 8 dials + 1 count, then a fixed run per shot.
		constexpr std::size_t kHeader = 10;
		const int  version = values.empty() ? 0 : values[0];
		const bool known = version > 0 &&
			static_cast<std::size_t>(version) < kStrideForVersion.size();
		if (values.size() < kHeader || !known) {
			Log::Warn(Log::Category::kCamera,
				"Slot {} was saved by a different version and cannot be applied."sv,
				a_index + 1);
			return;
		}

		const std::size_t stride = kStrideForVersion[static_cast<std::size_t>(version)];

		// TWO SEPARATE FAILURES, SAID SEPARATELY.
		//
		// These were one condition with one message, and it read as nonsense the
		// first time it fired: "Slot 1 holds 39 shot(s) against this build's 39".
		// Both numbers agreed, because the half that had actually failed was the
		// length check underneath — the value came back truncated by the config
		// reader — and the message could only describe the other half.
		//
		// A diagnostic that names the wrong cause is worse than no diagnostic. It
		// sent the first look for this bug at the shot table, which was fine.
		const auto count = static_cast<std::size_t>(values[9]);
		if (count != static_cast<std::size_t>(ShotType::kCount)) {
			Log::Warn(Log::Category::kCamera,
				"Slot {} was saved with {} shots and this build has {}; not applied."sv,
				a_index + 1, count, static_cast<std::size_t>(ShotType::kCount));
			return;
		}

		const auto needed = kHeader + count * stride;
		if (values.size() < needed) {
			Log::Warn(Log::Category::kCamera,
				"Slot {} is short: {} value(s) read, {} expected, {} characters on disk. "
				"The line was truncated rather than mis-saved."sv,
				a_index + 1, values.size(), needed, raw.size());
			return;
		}

		Config::SetInt("Direction", "iMinShotTime", values[1]);
		Config::SetInt("Direction", "iMaxShotTime", values[2]);
		Config::SetInt("Direction", "iCutEveryMin", values[3]);
		Config::SetInt("Direction", "iCutEveryMax", values[4]);
		Config::SetBool("Direction", "bPerLineAngleChange", values[5] != 0);
		Config::SetBool("Direction", "bHoldOnShortLines", values[6] != 0);
		Config::SetBool("Direction", "bTimedCutsWhileSpeaking", values[7] != 0);
		Config::SetBool("Direction", "bTimedCutsWhileChoosing", values[8] != 0);

		// bCoverPlayerTurn is left alone here for exactly the reason ApplyPreset
		// leaves it alone: it is not a style, it is the difference between a
		// dialogue camera and a landscape camera.

		int on = 0;
		for (std::size_t i = 0; i < count; ++i) {
			const auto type = static_cast<ShotType>(i);
			const auto base = kHeader + i * stride;

			const bool enabled = values[base] != 0;
			const int  move = std::clamp(values[base + 1], 0, static_cast<int>(Move::kCount) - 1);
			const int  amount = std::clamp(values[base + 2], 0, 100);
			const int  time = std::clamp(values[base + 3], 30, 900);

			Config::SetBool("Shots", Key(type), enabled);
			Config::SetInt("Shots", MoveKey(type), move);
			Config::SetInt("Shots", MoveAmountKey(type), amount);
			Config::SetInt("Shots", MoveTimeKey(type), time);

			Shot::SetEnabled(type, enabled);
			Shot::SetMove(type, static_cast<Move>(move));
			Shot::SetMoveAmount(type, amount);
			Shot::SetMoveTime(type, time);

			// Each field only if the payload actually carried it. Anything a
			// older slot never saved is left exactly as it is.
			if (stride > 4) {
				const int lens = std::clamp(values[base + 4], kMinLens, kMaxLens);
				Config::SetInt("Shots", LensKey(type), lens);
				Shot::SetLens(type, lens);
			}

			if (stride > 5) {
				const int weight = std::clamp(values[base + 5], 0, 100);
				Config::SetInt("Shots", WeightKey(type), weight);
				Shot::SetWeight(type, weight);
			}

			on += enabled ? 1 : 0;
		}

		// The rigs, if this slot carried any.
		//
		// Applied only when the list is the length this build expects. A slot saved
		// against a different shot count would otherwise assign rigs to setups by
		// position, which is the one way to get lighting that is wrong on every
		// angle at once and consistent enough to look deliberate.
		const auto rawLights = Config::String("CustomPresets", SlotLightKey(a_index), "");
		if (!rawLights.empty()) {
			const auto names = SplitNames(rawLights, ',');
			if (names.size() == static_cast<std::size_t>(ShotType::kCount)) {
				for (std::size_t i = 0; i < names.size(); ++i) {
					const auto type = static_cast<ShotType>(i);
					const int  rig = Scene::FindLook(names[i]);
					if (rig < 0) {
						continue;  // an unknown rig leaves that setup as it was
					}
					Config::SetString("Shots", LightKey(type), names[i].c_str());
					Shot::SetLight(type, rig);
				}
			} else {
				Log::Warn(Log::Category::kCamera,
					"Slot {} carries {} lighting rig(s) against this build's {}; lighting not applied."sv,
					a_index + 1, names.size(), static_cast<std::size_t>(ShotType::kCount));
			}
		}

		// Live as well as written, the same as a built-in preset. Trying three
		// saved looks in a row and judging each by the last one's timing is the
		// bug this avoids.
		Director::LoadSettings();

		// A LEGACY SLOT REWRITES ITSELF ONCE, THE MOMENT IT IS USED.
		//
		// ActiveCustomSlot answers by comparing the stored line against a fresh
		// snapshot, and a snapshot carries every field the current version knows
		// about — so an older payload can never equal one, and the slot could be
		// running and still never say "in use". Re-saving settles it: what goes
		// back is exactly what was just applied, plus whatever was already live
		// on the fields that payload never had an opinion about.
		if (stride < kSlotStride) {
			SaveCustomSlot(a_index, Config::String("CustomPresets", SlotNameKey(a_index), ""));
			Log::Info(Log::Category::kCamera,
				"Slot {} was saved by an older build and has been re-saved, keeping "
				"whatever was live for the settings it did not carry."sv,
				a_index + 1);
		}

		Log::Info(Log::Category::kCamera,
			"Applied your slot {} ('{}'): {} setup(s) on."sv, a_index + 1,
			Config::String("CustomPresets", SlotNameKey(a_index), ""), on);
	}

	int ActiveCustomSlot()
	{
		const auto now = Snapshot();
		for (int i = 0; i < kCustomSlots; ++i) {
			const auto stored = Config::String("CustomPresets", SlotDataKey(i), "");
			if (!stored.empty() && stored == now &&
				!Config::String("CustomPresets", SlotNameKey(i), "").empty()) {
				return i;
			}
		}
		return -1;
	}

	void RenameCustomSlot(int a_index, std::string_view a_name)
	{
		if (!ValidSlot(a_index)) {
			return;
		}
		Config::SetString("CustomPresets", SlotNameKey(a_index), std::string{ a_name }.c_str());
	}

	void DeleteCustomSlot(int a_index)
	{
		if (!ValidSlot(a_index)) {
			return;
		}
		// All three, or a name with no payload reads as a slot that is in use and
		// applies nothing when pressed — and a lighting list left behind would be
		// picked up whole by whatever look is saved into the slot next.
		Config::SetString("CustomPresets", SlotDataKey(a_index), "");
		Config::SetString("CustomPresets", SlotNameKey(a_index), "");
		Config::SetString("CustomPresets", SlotLightKey(a_index), "");
		Log::Info(Log::Category::kCamera, "Cleared your slot {}."sv, a_index + 1);
	}

	void ApplyPendingPreset()
	{
		const auto wanted = Config::String("Presets", "sApply", "");
		if (wanted.empty()) {
			return;
		}

		const auto* preset = FindPreset(wanted);
		if (!preset) {
			Log::Warn(Log::Category::kCamera,
				"[Presets] sApply names '{}', which is not a preset. Ignored."sv, wanted);
			Config::SetString("Presets", "sApply", "");
			return;
		}

		ApplyPreset(*preset);

		// Cleared so this is a one-shot rather than a mode. Left set, it would
		// overwrite whatever the player tuned afterwards on every single launch,
		// and the edits would appear to simply not save.
		Config::SetString("Presets", "sApply", "");
	}
}

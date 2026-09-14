#include "SD/Camera/Shot.h"
#include "SD/Camera/ShotAngles.h"

#include "SD/Camera/Space.h"

namespace SD::Camera
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDeg = kPi / 180.0f;

		// WHAT USED TO BE HERE: kSubjectExtent (42), kMinSubjectDistance (68) and
		// kProbeStart (48), the three numbers that described a standing human and
		// were applied to everything.
		//
		// They live in Anatomy now, as the humanoid defaults, and arrive per subject
		// scaled by what was actually measured. Nothing about a person changed — a
		// human still measures 1.0 and gets 42, 68 and 48 — but a dragon no longer
		// gets them too. See Anatomy.h for what each one is for; the probe is the
		// one whose being wrong stopped the mod working at all rather than merely
		// framing badly.

		// How far the camera stops short of whatever is behind it.
		//
		// Lowered from 26: combined with the floor it demanded 108 units of clear
		// space before any shot would place, which ordinary interiors do not offer
		// in most directions, so composed shots failed constantly and the emergency
		// single took over.
		constexpr float kWallMargin = 14.0f;

		// How wide the camera is, for the far end of a probe bundle.
		//
		// Left where it is rather than raised alongside the margin above: the
		// margin is axial clearance and the bundle is lateral, so between them the
		// lens now has room on both counts where before it had a single line down
		// the middle and 14 units of nose.
		constexpr float kCameraRadius = 18.0f;

		// Continuity limits depend on the camera's anchor. The shot's own
		// adjustment window is intersected with these limits by ShotAngles.
		constexpr float kLineFloorSubject = 8.0f;
		constexpr float kLineFloorMidpoint = 34.0f;

		// The lens palette, in degrees of HORIZONTAL field of view.
		//
		// These are the real axis of difference between two shots of the same
		// person at the same size. A face at 40 degrees is compressed, flattened
		// and separated from its background; the same face at 95 has a nose
		// reaching for the lens and half the room behind it. Skyrim's default sits
		// around 75-90 depending on the player's own settings, so anything under
		// 60 reads immediately as "a camera", which is the point.
		constexpr float kLensTele = 40.0f;      // across the room; surveillance
		constexpr float kLensLong = 50.0f;      // portrait glass; compresses
		constexpr float kLensPortrait = 60.0f;  // flattering but not obviously long
		constexpr float kLensNormal = 72.0f;    // close to what the player already has
		constexpr float kLensWide = 88.0f;      // involved, a little distended
		constexpr float kLensVeryWide = 100.0f; // the room swallows the people

		struct ShotSpec
		{
			bool  onNpc;         // who the shot is about
			float fill;          // fraction of frame HEIGHT the subject should occupy
			float angleDeg;      // degrees off the eyeline; larger steps further to the side
			float rise;          // world units above the subject's eye level
			bool  overShoulder;  // stand behind the other person, putting them in the corner

			Anchor anchor;
			Aim    aim;
			Move   move;

			// Horizontal field of view for this setup. Distance is then solved for
			// the fill AT THIS LENS, so fill still means what it says: the subject
			// occupies the same slice of frame either way, and the lens changes
			// what that slice looks like rather than how big it is.
			float lens;

			// Units depend on move: a fraction of the standoff for push/pull, a
			// fraction of the lens for zoom, world units for crane and tilt,
			// degrees for drift.
			float moveAmount;

			// Where the subject sits IN FRAME, normalised to the half-frame, so
			// 0.33 is a third of the way out from centre. Positive headroom puts
			// them ABOVE centre — which is where a face belongs, eyes on the upper
			// third. Positive lookRoom puts them to the side, leaving the space
			// they are looking into open in front of them.
			//
			// Nothing in the mod did this before: every shot aimed dead at a head,
			// so every shot put a head in the exact middle of frame. That alone
			// makes forty setups look like one.
			float headroom;
			float lookRoom;
		};

		// The shot table.
		//
		// Read down the lens and move columns rather than across a row: those two
		// are what make one setup a different image from another, and they are the
		// two the old table did not have. Within the close range alone this now
		// runs from a locked 50mm-equivalent portrait to a 88-degree push-in that
		// puts the lens close enough to distort — two shots the old table
		// described with nearly the same four numbers.
		[[nodiscard]] constexpr ShotSpec SpecFor(ShotType a_type)
		{
			using A = Anchor;
			using M = Move;

			switch (a_type) {
			// ---- NPC coverage -------------------------------------------------
			//                                     onNpc  fill   angle   rise   OTS   anchor      aim         move          lens            amt    head   look
			case ShotType::kOverPlayerShoulder: return { true,  0.42f, 20.0f,   6.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.12f, 0.22f };
			case ShotType::kCloseUp:            return { true,  0.68f, 26.0f,   2.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.16f, 0.14f };
			case ShotType::kMediumNpc:          return { true,  0.40f, 34.0f,   4.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   8.0f,  0.14f, 0.16f };
			case ShotType::kLongNpc:            return { true,  0.16f, 42.0f,  16.0f, false, A::kSubject, Aim::kSubject, M::kCraneUp,  kLensWide,     34.0f, -0.10f, 0.12f };
			case ShotType::kLowAngle:           return { true,  0.55f, 30.0f, -26.0f, false, A::kSubject, Aim::kSubject, M::kTiltUp,   kLensWide,     18.0f, 0.10f, 0.14f };
			case ShotType::kCloseProfile:       return { true,  0.62f, 72.0f,   1.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.14f, 0.20f };
			case ShotType::kCloseLow:           return { true,  0.66f, 44.0f, -18.0f, false, A::kSubject, Aim::kSubject, M::kPushIn,   kLensPortrait, 0.14f, 0.12f, 0.14f };
			case ShotType::kCloseHigh:          return { true,  0.60f, 50.0f,  26.0f, false, A::kSubject, Aim::kSubject, M::kTiltDown, kLensNormal,   14.0f, 0.10f, 0.14f };

			// A loose single on a WIDE lens, which is the whole reason it exists
			// separately from the close-up above: same person, same side of the
			// eyeline, and an entirely different image because of the glass.
			case ShotType::kCloseWide:          return { true,  0.30f, 58.0f,   8.0f, false, A::kSubject, Aim::kSubject, M::kPushIn,   kLensWide,     0.12f, 0.08f, 0.18f };

			// Tighter than any other setup. 0.85 fill crops below the chin, which
			// is the point — it is only offered on an intensity-100 line, and the
			// emotion scan puts those at 5-8% of dialogue.
			//
			// On the LONGEST lens in the table, and that is what makes it work at
			// all. It was on the widest, on the reasoning that a wide lens close to
			// a face is uncomfortable and this is the shot allowed to be — but a
			// wide lens has to get physically close to fill a frame, and 0.85 at 88
			// degrees solves to 47 units, which is inside the 68-unit floor that
			// keeps the lens out of somebody's chest. Clamped to the floor it
			// rendered at about 0.60 fill: the setup named "extreme" was LOOSER on
			// screen than the plain close-up, whose 0.68 on a long lens solves to
			// 119 and is delivered exactly.
			//
			// At 40 degrees the same 0.85 solves to 121 — clear of the floor, so
			// the fill is actually delivered — and the compression does the work
			// the proximity was supposed to: the face fills the frame, flattened
			// and cut off from its background.
			//
			// LOCKED OFF as of 2026-08-13, and it used to push in at 0.18 — the
			// largest move in the table. Reported as "extreme close-up is zooming
			// in with zoom off", which it was not: a push-in is a dolly and
			// survived the zoom removal because it is a different move. It read as
			// a zoom anyway, and on the tightest framing in the mod it should: the
			// shot is already at the end of its travel, so closing another quarter
			// of the standoff has nowhere to go and nothing to reveal. Anyone who
			// wants motion here can ask for it on the setup's own row.
			case ShotType::kExtremeClose:       return { true,  0.85f, 22.0f,   1.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensTele,     0.00f, 0.10f, 0.08f };

			// Dirty singles. overShoulder is what keeps the listener in the corner
			// of frame; the tight fill is what separates these from the wide OTS.
			case ShotType::kDirtyNpc:           return { true,  0.58f, 15.0f,   3.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.14f, 0.24f };
			case ShotType::kThreeQuarterNpc:    return { true,  0.46f, 40.0f,   5.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   10.0f, 0.14f, 0.18f };
			case ShotType::kMediumProfile:      return { true,  0.34f, 66.0f,   4.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.12f, 0.22f };
			case ShotType::kLowProfile:         return { true,  0.50f, 78.0f, -22.0f, false, A::kSubject, Aim::kSubject, M::kTiltUp,   kLensWide,     16.0f, 0.08f, 0.20f };
			case ShotType::kOverhead:           return { true,  0.28f, 40.0f,  62.0f, false, A::kSubject, Aim::kSubject, M::kCraneDown, kLensWide,    26.0f, 0.00f, 0.10f };

			// Height on an over-the-shoulder costs more rise than it does on a
			// clean single, and the reason is the shoulder. These stand a body's
			// length further back than a single does — the OTS rule below forces
			// the standoff past the other participant — so a given rise subtends a
			// much shallower angle from there. The +-20 that reads as a definite
			// tilt on kCloseLow barely registers here, hence -26 and 34.
			case ShotType::kOverPlayerShoulderLow:  return { true,  0.46f, 22.0f, -26.0f, true, A::kSubject, Aim::kSubject, M::kPushIn,   kLensPortrait, 0.12f, 0.10f, 0.22f };
			case ShotType::kOverPlayerShoulderHigh: return { true,  0.40f, 24.0f,  34.0f, true, A::kSubject, Aim::kSubject, M::kTiltDown, kLensNormal,   16.0f, 0.10f, 0.22f };
			case ShotType::kOverPlayerShoulderWide: return { true,  0.24f, 28.0f,  12.0f, true, A::kSubject, Aim::kSubject, M::kPullOut,  kLensWide,     0.14f, 0.02f, 0.18f };

			// ---- Player coverage ----------------------------------------------
			case ShotType::kOverNpcShoulder:    return { false, 0.40f, 20.0f,   6.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.12f, 0.22f };
			case ShotType::kMediumPlayer:       return { false, 0.38f, 34.0f,   4.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   8.0f,  0.14f, 0.16f };
			case ShotType::kHighAngle:          return { false, 0.34f, 30.0f,  38.0f, false, A::kSubject, Aim::kSubject, M::kCraneDown, kLensNormal,  24.0f, 0.06f, 0.14f };
			case ShotType::kClosePlayer:        return { false, 0.60f, 26.0f,   2.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.16f, 0.14f };

			// Mirrors kExtremeClose exactly, including the long lens. See the note
			// there for why the tightest setup in the table is on the NARROWEST
			// glass: a wide lens has to get physically close to fill a frame, and
			// 0.85 fill at 88 degrees solves inside the 68-unit floor, so the shot
			// named "extreme" renders LOOSER than the plain close-up.
			case ShotType::kExtremeClosePlayer: return { false, 0.85f, 22.0f,   1.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensTele,     0.00f, 0.10f, 0.08f };
			case ShotType::kDirtyPlayer:        return { false, 0.55f, 15.0f,   3.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.14f, 0.24f };
			case ShotType::kPlayerProfile:      return { false, 0.40f, 70.0f,   2.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.12f, 0.22f };
			case ShotType::kPlayerLow:          return { false, 0.52f, 32.0f, -20.0f, false, A::kSubject, Aim::kSubject, M::kTiltUp,   kLensWide,     16.0f, 0.10f, 0.14f };
			case ShotType::kThreeQuarterPlayer: return { false, 0.44f, 40.0f,   5.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   10.0f, 0.14f, 0.18f };

			// Deliberately matched to their NPC-side counterparts — same size,
			// same height, same lens, same move. A reverse shot is supposed to
			// match the shot it answers; matched glass is what makes a pair of
			// angles read as one conversation rather than two separate cameras.
			case ShotType::kOverNpcShoulderLow:  return { false, 0.44f, 22.0f, -26.0f, true, A::kSubject, Aim::kSubject, M::kPushIn,   kLensPortrait, 0.12f, 0.10f, 0.22f };
			case ShotType::kOverNpcShoulderHigh: return { false, 0.38f, 24.0f,  34.0f, true, A::kSubject, Aim::kSubject, M::kTiltDown, kLensNormal,   16.0f, 0.10f, 0.22f };
			case ShotType::kOverNpcShoulderWide: return { false, 0.24f, 28.0f,  12.0f, true, A::kSubject, Aim::kSubject, M::kPullOut,  kLensWide,     0.14f, 0.02f, 0.18f };
			case ShotType::kLongPlayer:          return { false, 0.16f, 42.0f,  16.0f, false, A::kSubject, Aim::kSubject, M::kCraneUp,  kLensWide,     34.0f, -0.10f, 0.12f };
			case ShotType::kPlayerOverhead:      return { false, 0.28f, 40.0f,  62.0f, false, A::kSubject, Aim::kSubject, M::kCraneDown, kLensWide,   26.0f, 0.00f, 0.10f };

			// ---- Neutral: the pair ---------------------------------------------
			//
			// Anchored to the MIDPOINT rather than to a head. These used to reach
			// the midpoint by a side effect — a 90-degree lateral made the old
			// solver aim between the two — which meant "frame both of them" and
			// "stand side-on" could never be asked for separately.
			case ShotType::kTwoShot:            return { true,  0.20f, 90.0f,   8.0f, false, A::kMidpoint, Aim::kMidpoint, M::kDrift,   kLensNormal,   7.0f,  0.06f, 0.00f };
			case ShotType::kProfile:            return { true,  0.30f, 90.0f,   2.0f, false, A::kMidpoint, Aim::kMidpoint, M::kLocked,  kLensLong,     0.00f, 0.08f, 0.00f };

			// ---- Neutral: the room ---------------------------------------------
			//
			// Anchored to the SCENE. These do not hook to anybody's head: they take
			// the direction the room actually opens in, stand off in it, and let
			// the two participants fall where they fall in the frame. Negative
			// headroom is what puts them in the lower third with the space above
			// them, which is the difference between a shot of a room with a
			// conversation in it and a badly framed two-shot.
			// angleDeg here is degrees off the OPEN direction, not off the eyeline —
			// see the scene branch in Solve. Spreading them means three room shots
			// look at the same conversation from three corners instead of all
			// stacking up along the one open axis.
			case ShotType::kWide:               return { true,  0.11f,   0.0f,  24.0f, false, A::kScene, Aim::kScene, M::kPullOut,  kLensWide,     0.14f, -0.24f, 0.10f };
			case ShotType::kMaster:             return { true,  0.075f, 24.0f,  76.0f, false, A::kScene, Aim::kScene, M::kCraneUp,  kLensVeryWide, 46.0f, -0.30f, 0.14f };
			case ShotType::kGroundLevel:        return { true,  0.24f,  -20.0f, -46.0f, false, A::kScene, Aim::kMidpoint, M::kTiltUp, kLensVeryWide, 24.0f, -0.12f, 0.00f };

			// The long lens across the room, and the sharpest contrast in the
			// table: kMaster and kDistant are both wide framings of the same two
			// people, and they look nothing alike. One opens the space up, this
			// one crushes it flat and reads as watching from a distance.
			case ShotType::kDistant:            return { true,  0.09f,   0.0f,  28.0f, false, A::kScene, Aim::kMidpoint, M::kLocked, kLensTele,     0.00f, -0.08f, 0.00f };
			case ShotType::kDistantLow:         return { true,  0.10f,  32.0f, -24.0f, false, A::kScene, Aim::kMidpoint, M::kDrift,  kLensLong,     6.0f,  -0.06f, 0.00f };

			default:                            return { true,  0.42f, 20.0f,   6.0f, true,  A::kSubject, Aim::kSubject, M::kLocked, kLensPortrait, 0.00f, 0.12f, 0.18f };
			}
		}

		[[nodiscard]] float Length(const RE::NiPoint3& a_v)
		{
			return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z);
		}

		[[nodiscard]] RE::NiPoint3 Normalized(const RE::NiPoint3& a_v, bool& a_ok)
		{
			const float length = Length(a_v);
			a_ok = length > 1.0e-3f;
			return a_ok ? RE::NiPoint3{ a_v.x / length, a_v.y / length, a_v.z / length } : RE::NiPoint3{};
		}

		[[nodiscard]] RE::NiPoint3 Cross(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}

		[[nodiscard]] RE::NiPoint3 RotateAboutZ(const RE::NiPoint3& a_v, float a_radians)
		{
			const float c = std::cos(a_radians);
			const float s = std::sin(a_radians);
			return { a_v.x * c - a_v.y * s, a_v.x * s + a_v.y * c, a_v.z };
		}

		// Distance at which a a_extent-tall subject fills a_fill of the frame height.
		//
		// This is the whole correction. Skyrim reports a horizontal field of view,
		// so the vertical has to be derived through the aspect ratio before it can
		// say anything about how tall something appears.
		// The subject's own size, not a constant.
		//
		// kSubjectExtent and kMinSubjectDistance are now the HUMANOID values, kept
		// only as the defaults inside Anatomy. Passing the body in is what makes a
		// close-up on a dragon frame its head rather than a nostril: fill is a
		// fraction of the thing, and the thing is not always 42 units across.
		[[nodiscard]] float DistanceForFill(float a_fill, float a_fovDegrees, float a_aspect,
			const Anatomy& a_body)
		{
			// kMinLens..kMaxLens, the same pair Solve clamps to — see the note on
			// them. If this one were tighter the standoff would be solved for one
			// field of view and the frame rendered at another, and the subject
			// would come out larger than the fill asked for, which for the tightest
			// setup means straight through the distance floor.
			const float horizontal = std::clamp(a_fovDegrees,
									 static_cast<float>(kMinLens),
									 static_cast<float>(kMaxLens)) * kDeg;
			const float aspect = std::clamp(a_aspect, 1.0f, 3.0f);
			const float vertical = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);

			const float fill = std::clamp(a_fill, 0.04f, 0.95f);
			const float halfAngle = vertical * fill * 0.5f;
			const float t = std::tan(halfAngle);
			if (!(t > 1.0e-4f)) {
				return 200.0f;
			}
			// The ceiling is deliberately far past anything an interior can use.
			// A long lens on a small fill legitimately asks for well over a
			// thousand units, and the old 900 quietly refused it — so the widest,
			// longest setups in the table all solved to the same clamped distance
			// and arrived looking like each other.
			// The ceiling scales too. A dragon framed at a wide fill legitimately
			// asks for further away than any human shot ever does, and the old flat
			// 2400 would clamp exactly the setups that need the room most.
			return std::clamp((a_body.extent * 0.5f) / t,
				a_body.minDistance, 2400.0f * std::max(a_body.scale, 1.0f));
		}

		[[nodiscard]] float Ease(float a_t)
		{
			const float t = std::clamp(a_t, 0.0f, 1.0f);
			return 1.0f - (1.0f - t) * (1.0f - t);
		}

		// WHAT USED TO BE HERE: kMoveReference / dollyFraction, the one global
		// movement dial, referenced against a shipped 0.17.
		//
		// It scaled forty-three shots' moves together, which is the same mistake
		// the global lens shift was: one number adjusting every setup flattens the
		// difference between them. Amount is per setup now, on a 0-100 scale that
		// means the same thing for every move, and so is duration.

		// Shifts every shot's lens by a fixed number of degrees. The one dial that
		// changes the whole look of a conversation without touching a single
		// framing: run the same coverage on longer glass and it reads as observed
		// and composed, run it wider and it reads as involved and close.

		// Which setups the picker may draw. Every shot ships on; the player turns
		// off the ones they do not want to see.
		// How often a shot is drawn relative to its neighbours, 0-100.
		//
		// Separate from `enabled` rather than folded into it as "weight 0 is off",
		// so that switching a setup off and back on returns it to the frequency it
		// had rather than silently rewriting it. Weight 0 and disabled do now mean
		// the same thing at every draw site — see the note on Enabled — but they
		// remain two settings because they are two intentions.
		//
		// Seeded per shot rather than filled flat. The baseline that used to live
		// in the pools as duplicate entries lives here now; AuthoredWeight says
		// which setups carry it and why.
		struct SelectionSettings
		{
			std::array<std::atomic<bool>, static_cast<std::size_t>(ShotType::kCount)> enabled{};
			std::array<std::atomic<std::uint8_t>, static_cast<std::size_t>(ShotType::kCount)> weights{};

			SelectionSettings()
			{
				for (std::size_t i = 0; i < enabled.size(); ++i) {
					enabled[i].store(true, std::memory_order_relaxed);
					weights[i].store(static_cast<std::uint8_t>(AuthoredWeight(static_cast<ShotType>(i))),
						std::memory_order_relaxed);
				}
			}
		};
		// The settings panel and camera hooks can access these concurrently.
		SelectionSettings selectionSettings{};

		// The live field of view for each setup, in degrees. A TRUE value, seeded
		// from the table.
		//
		// This used to be an override with 0 meaning "keep what the table
		// authored", and the sentinel was the problem: the authored lens differs
		// per setup — 88 on the wides, 50 on the portraits, 40 on the extreme
		// close — so a slider sitting at 0 told you nothing about what the shot was
		// actually shooting at, and two setups reading 0 were shooting 38 degrees
		// apart. Seeding each entry with its own authored value makes the number on
		// screen the number in use.
		std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> lensDegrees = [] {
			std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i] = static_cast<std::uint8_t>(SpecFor(static_cast<ShotType>(i)).lens);
			}
			return out;
		}();

		// WHAT 100 ON THE AMOUNT SLIDER MEANS, per move.
		//
		// The table authored each amount in whatever unit its move needed: a
		// fraction of the standoff for a push, a fraction of the lens for a zoom,
		// world units for a crane or tilt, degrees for a drift. That is fine for a
		// table and useless for a control — the same "14" was a seventh of the
		// standoff on one setup and a seventh of a degree of arc on another.
		//
		// So the slider is a percentage of the full travel of whichever move is
		// selected, and these are the full travels. Now 60 means the same amount of
		// movement whatever the shot is doing, and switching a setup from a push to
		// an orbit keeps its intensity instead of jumping.
		//
		// The world-unit entries are multiplied by the subject's scale at solve
		// time, so a crane on a dragon rises in proportion to the dragon.
		[[nodiscard]] constexpr float FullScale(Move a_move)
		{
			switch (a_move) {
			case Move::kPushIn:
			case Move::kPullOut:    return 0.40f;   // fraction of the standoff
			case Move::kZoomIn:
			case Move::kZoomOut:    return 0.40f;   // fraction of the lens
			case Move::kCraneUp:
			case Move::kCraneDown:
			case Move::kTiltUp:
			case Move::kTiltDown:   return 120.0f;  // world units
			case Move::kTruckLeft:
			case Move::kTruckRight: return 120.0f;  // world units
			case Move::kDrift:
			case Move::kOrbitLeft:
			case Move::kOrbitRight: return 45.0f;   // degrees of arc
			case Move::kLocked:
			default:                return 0.0f;
			}
		}

		// The table's own amount, expressed on the 0-100 slider. This is what the
		// per-shot Default button restores to, and what a fresh install starts at,
		// so the authored rhythm survives the move becoming a setting.
		[[nodiscard]] constexpr int AuthoredStrength(ShotType a_type)
		{
			const auto  spec = SpecFor(a_type);
			const float full = FullScale(spec.move);
			if (!(full > 0.0f)) {
				// A locked setup has no authored amount to convert. It still needs a
				// sensible number sitting under the slider for the moment somebody
				// switches it to a move that does something.
				return 35;
			}
			const float pct = (spec.moveAmount / full) * 100.0f;
			return static_cast<int>(pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct));
		}

		// How long a move takes, in hundredths of a second. The old global default.
		constexpr int kDefaultMoveTime = 420;

		std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> moveChoice = [] {
			std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i] = static_cast<std::uint8_t>(SpecFor(static_cast<ShotType>(i)).move);
			}
			return out;
		}();

		std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> moveStrength = [] {
			std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i] = static_cast<std::uint8_t>(AuthoredStrength(static_cast<ShotType>(i)));
			}
			return out;
		}();

		std::array<std::uint16_t, static_cast<std::size_t>(ShotType::kCount)> moveTime = [] {
			std::array<std::uint16_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			out.fill(static_cast<std::uint16_t>(kDefaultMoveTime));
			return out;
		}();

		// WHICH LIGHTING RIG EACH SETUP IS SHOT UNDER, as an index into
		// Scene::AllRigs.
		//
		// Stored as an index and written to the ini as a NAME, which is the same
		// split the presets use and for the same reason: an ordinal in a settings
		// file points at whatever now sits at that position, so adding a rig in the
		// middle of the table would silently relight every setup below it.
		//
		// -1 means "nothing has resolved this yet". It is not a legal value to
		// light with — Director fills the array from the ini before the first cut —
		// but it has to be distinguishable from rig 0, which is a real rig that
		// happens to be Off. A setup that never got read must fall back to what it
		// ships as, not to darkness.
		std::array<std::int8_t, static_cast<std::size_t>(ShotType::kCount)> lightRig = [] {
			std::array<std::int8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			out.fill(static_cast<std::int8_t>(-1));
			return out;
		}();

		// This setup's own light nudge, in camera space. Zero is the common case by
		// a wide margin, so these are only ever written by somebody who has gone
		// looking for them.
		std::array<std::int16_t, static_cast<std::size_t>(ShotType::kCount)> lightOffX{};
		std::array<std::int16_t, static_cast<std::size_t>(ShotType::kCount)> lightOffY{};
		std::array<std::int16_t, static_cast<std::size_t>(ShotType::kCount)> lightOffZ{};

		// How much room exists in one direction, measured from the subject.
		//
		// Cast outward from the subject rather than inward from the camera: a ray
		// beginning inside a wall usually reports no hit at all, so a camera buried
		// in masonry passes an inward test cleanly.
		// THE PROBE START IS THE ONE THAT BREAKS CREATURES OUTRIGHT.
		//
		// kProbeStart exists because a ray cast from inside an actor's own
		// collision hits them immediately and reports no room in any direction. 48
		// units clears a person. It does not clear a dragon by any margin at all —
		// so every direction came back blocked, every composed shot failed, and the
		// emergency single ran the entire conversation. That is not a framing
		// nicety; it is the difference between the mod working and not.
		// What one direction offers: how far the camera may stand, and how much of
		// the shot's width was actually unobstructed getting there.
		//
		// `clear` used to be thrown away because there was nothing to spend it on.
		// It is what tells a wide shot with a market stall across a third of the
		// frame from the identical wide shot with nothing in it — two placements
		// that were, until the score existed, the same answer.
		struct Room
		{
			float distance{ 0.0f };
			float clear{ 1.0f };
		};

		[[nodiscard]] Room RoomAlong(const RE::NiPoint3& a_subject, const RE::NiPoint3& a_direction,
			float a_rise, float a_wanted, float a_probeStart, float a_extent)
		{
			if (a_wanted <= a_probeStart) {
				return { a_wanted, 1.0f };
			}

			const RE::NiPoint3 from{
				a_subject.x + a_direction.x * a_probeStart,
				a_subject.y + a_direction.y * a_probeStart,
				a_subject.z + a_direction.z * a_probeStart + a_rise * 0.35f
			};
			const RE::NiPoint3 to{
				a_subject.x + a_direction.x * a_wanted,
				a_subject.y + a_direction.y * a_wanted,
				a_subject.z + a_direction.z * a_wanted + a_rise
			};

			// Wide at the subject, narrow at the camera. The near spread is half the
			// subject's own extent, so the cone contains their silhouette and
			// anything crossing it is crossing frame; the far spread is the camera's
			// own body, which is what decides whether there is room to stand.
			const float nearSpread = std::max(a_extent * 0.5f, 12.0f);
			const auto  probe = CastBundle(from, to, nearSpread, kCameraRadius);

			if (!probe.valid) {
				return { a_wanted, 1.0f };  // no physics world; do not veto every shot
			}
			if (probe.clear >= 1.0f) {
				return { a_wanted, 1.0f };
			}

			// The margin scales with the subject for the same reason `rise` does:
			// it is in world units, and every other absolute number in this file
			// was already made per-subject when creatures arrived. A dragon's
			// camera stopping four inches off a wall is four inches at ITS scale.
			const float margin = kWallMargin * std::max(a_extent / 42.0f, 1.0f);
			return {
				std::max(a_probeStart + probe.distance - margin, 0.0f),
				probe.clear
			};
		}

		// A placement proposal on the centre sightline only. A side ray through
		// harmless foreground must not shorten a protected shot. This deliberately
		// does not certify visibility: final face samples and the local lens volume
		// still decide whether the composed camera can be used.
		[[nodiscard]] Room NarrowRoom(const RE::NiPoint3& a_subject, const RE::NiPoint3& a_direction,
			float a_rise, float a_wanted, float a_probeStart, float a_extent)
		{
			if (a_wanted <= a_probeStart) {
				return { a_wanted, 1.0f };
			}
			const RE::NiPoint3 from{
				a_subject.x + a_direction.x * a_probeStart,
				a_subject.y + a_direction.y * a_probeStart,
				a_subject.z + a_direction.z * a_probeStart + a_rise * (a_probeStart / a_wanted)
			};
			const RE::NiPoint3 to{
				a_subject.x + a_direction.x * a_wanted,
				a_subject.y + a_direction.y * a_wanted,
				a_subject.z + a_direction.z * a_wanted + a_rise
			};
			const auto probe = Cast(from, to);
			if (!probe.valid || !probe.hit) {
				return { a_wanted, 1.0f };
			}
			const float fullLength = Length({ to.x - from.x, to.y - from.y, to.z - from.z });
			const float fraction = fullLength > 0.001f ? std::clamp(probe.distance / fullLength, 0.0f, 1.0f) : 0.0f;
			const float margin = kWallMargin * std::max(a_extent / 42.0f, 1.0f);
			return { std::max(a_probeStart + (a_wanted - a_probeStart) * fraction - margin, 0.0f), 0.0f };
		}

		// The room along a bearing: measured, or remembered from the cut.
		//
		// EVERY RoomAlong CALL ON A SOLVE PATH GOES THROUGH HERE, and that is the
		// only thing keeping the setting honest. A path that probed directly would
		// still correct the camera mid-shot, and it would do it on one axis while
		// the others held — which reads worse than either behaviour on its own.
		//
		// The remembered `clear` is 1.0f rather than what was measured. Clear feeds
		// Score, Score feeds Pose::quality, and quality is read by exactly one
		// caller — Placement, judging candidates at a cut, where holdPlacement is
		// false by construction. A held frame's quality is therefore never read,
		// and carrying a second field to make an unread number accurate would be
		// paying for a fiction.
		[[nodiscard]] Room RoomHere(const Subjects& a_subjects, const RE::NiPoint3& a_subject,
			const RE::NiPoint3& a_direction, float a_rise, float a_wanted, float a_probeStart,
			float a_extent)
		{
			if (a_subjects.holdPlacement && a_subjects.heldRoom > kUnheld) {
				return a_subjects.heldRoom >= kOpenRoom ?
					Room{ a_wanted, 1.0f } :
					Room{ a_subjects.heldRoom, 1.0f };
			}
			return RoomAlong(a_subject, a_direction, a_rise, a_wanted, a_probeStart, a_extent);
		}

		// What Pose::room should report. See kOpenRoom for why an unobstructed
		// bearing is not reported as its own distance.
		//
		// A HELD FRAME REPORTS THE HELD VALUE, not what RoomHere just handed back.
		// The two differ, and only in the case that would matter: RoomHere answers
		// a held kOpenRoom with the distance the shot asked for THIS frame, which
		// is right for solving the standoff and wrong for remembering, because a
		// move that has pulled out since the cut would turn "nothing is in the way"
		// into a cap at wherever it had got to. Reporting the held value keeps
		// Pose::room true on every frame rather than only on the one the director
		// happens to read.
		[[nodiscard]] float RememberRoom(const Subjects& a_subjects, float a_room, float a_clear)
		{
			if (a_subjects.holdPlacement && a_subjects.heldRoom > kUnheld) {
				return a_subjects.heldRoom;
			}
			return a_clear >= 1.0f ? kOpenRoom : a_room;
		}

		// Where the subject lands in frame, once the camera is placed.
		//
		// Returns the aim point, which is NOT the subject: to put a face in the
		// upper third the camera has to point below it. Everything in the mod used
		// to aim dead at a head, so every setup — all forty-odd of them — put its
		// subject in the exact centre of the screen. That is a large part of why
		// they read as one shot.
		[[nodiscard]] RE::NiPoint3 Compose(const RE::NiPoint3& a_position, const RE::NiPoint3& a_target,
			const RE::NiPoint3& a_facing, float a_lensDeg, float a_aspect, float a_headroom, float a_lookRoom)
		{
			bool       ok = false;
			const auto forward = Normalized(
				{ a_target.x - a_position.x, a_target.y - a_position.y, a_target.z - a_position.z }, ok);
			if (!ok) {
				return a_target;
			}

			const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(forward, worldUp), ok);
			if (!ok) {
				return a_target;  // looking straight down; no meaningful horizon to compose against
			}
			const auto up = Cross(right, forward);

			const float distance = Length(
				RE::NiPoint3{ a_target.x - a_position.x, a_target.y - a_position.y, a_target.z - a_position.z });

			const float horizontal = std::clamp(a_lensDeg, 30.0f, 120.0f) * kDeg;
			const float aspect = std::clamp(a_aspect, 1.0f, 3.0f);
			const float vertical = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);

			const float halfWidth = distance * std::tan(horizontal * 0.5f);
			const float halfHeight = distance * std::tan(vertical * 0.5f);

			// Which way the subject is looking, in screen terms. Derived rather
			// than passed in as a sign, because the camera legally stands on either
			// side of the eyeline — hardcoding it would put the look-room BEHIND
			// the subject's head every time the 180-degree rule flipped.
			const float facing = right.x * a_facing.x + right.y * a_facing.y + right.z * a_facing.z;
			const float lookSign = facing > 0.0f ? -1.0f : 1.0f;

			const float offsetX = a_lookRoom * lookSign * halfWidth;
			const float offsetY = a_headroom * halfHeight;

			return {
				a_target.x - right.x * offsetX - up.x * offsetY,
				a_target.y - right.y * offsetX - up.y * offsetY,
				a_target.z - right.z * offsetX - up.z * offsetY
			};
		}

		// HOW GOOD A PLACEMENT IS, 0..1, from the three ways it can be a compromise.
		//
		// size   — it had to stand closer than the framing asked for, so the subject
		//          is bigger than the shot intended. This is the one that changes
		//          what the shot IS: a close-up that had to come in another third is
		//          an extreme close-up nobody chose. Weighted highest.
		// clear  — the view down the chosen bearing is partly blocked, by geometry
		//          or by somebody standing in it.
		// angle  — it had to step away from its own authored bearing to find room.
		//          Limited to twelve degrees; larger changes must use another
		//          enabled shot rather than relabel a different composition.
		//
		// Multiplied by nothing and floored at nothing. A refused shot never reaches
		// here — `valid` stays false and quality stays zero — so every number this
		// returns describes a placement that genuinely works, and the caller is
		// choosing between working shots rather than filtering broken ones.
		[[nodiscard]] float Score(float a_wanted, float a_used, float a_clear, float a_offset)
		{
			const float size = a_wanted > 1.0f ?
				std::clamp(a_used / a_wanted, 0.0f, 1.0f) : 1.0f;
			const float clear = std::clamp(a_clear, 0.0f, 1.0f);
			const float angle = 1.0f - std::clamp(std::abs(a_offset) / ShotAngles::kMaxAdjustment, 0.0f, 1.0f);

			return std::clamp(0.45f * size + 0.32f * clear + 0.23f * angle, 0.0f, 1.0f);
		}

		// WHAT IS ACTUALLY VISIBLE FROM THE FINISHED POSE, 0..1.
		//
		// Everything above this point reasons about a BEARING — how much room there
		// is along a direction out from the subject. That covers the sightline for
		// a plain single by coincidence, because the camera ends up on that exact
		// line. Three things break the coincidence, and none of them were checked:
		//
		//   A SLIDE moves the camera sideways after the bearing was probed, and is
		//   the one move that does not re-aim at the subject afterwards. The shot
		//   was validated at a position the camera no longer occupies.
		//
		//   AN OVER-THE-SHOULDER is only an over-the-shoulder if the shoulder is in
		//   frame, and the shoulder belongs to the person the shot is NOT about.
		//   Nothing has ever asked whether that body is visible; a beam across it
		//   turns the setup into a slightly off-centre close-up for no reason the
		//   player can see.
		//
		//   A TWO-SHOT needs both people. Same test, same omission.
		//
		// Cast FROM the person TOWARD the camera in every case. The inward
		// direction is unreliable — a ray beginning inside a wall reports no hit,
		// so a camera buried in masonry passes cleanly.
		//
		// Penalties multiply rather than veto. A shot that has lost its foreground
		// body is a worse shot, not an impossible one, and in a room where it is
		// the best available it should still be reachable.
		[[nodiscard]] float Visibility(const Pose& a_pose, const RE::NiPoint3& a_subject,
			const RE::NiPoint3& a_other, const ShotSpec& a_spec, float a_truck,
			const Subjects& a_subjects)
		{
			// A held shot does not ask, and the reason is the same one RoomHere
			// gives: this is three raycasts and a walk of every actor in the cell,
			// spent entirely on Pose::quality, which no caller reads on a frame
			// where holdPlacement is set. Left running it would also be the last
			// per-frame probe standing — the camera would hold its distance and
			// still be scored against a guard walking past, which is work done to
			// reach a number that is thrown away.
			if (a_subjects.holdPlacement) {
				return 1.0f;
			}

			float sight = 1.0f;

			if (a_truck != 0.0f && !Clear(a_subject, a_pose.position)) {
				sight *= 0.35f;
			}

			const bool needsOther = a_spec.overShoulder ||
				a_spec.anchor != Anchor::kSubject || a_spec.aim != Aim::kSubject;
			if (needsOther && !Clear(a_other, a_pose.position)) {
				sight *= a_spec.overShoulder ? 0.5f : 0.6f;
			}

			if (a_subjects.avoidCrowds) {
				// Reach is the width of the shot near the lens, which is what
				// decides how much of the frame a body standing there takes. A
				// person at arm's length from the camera fills it; the same person
				// beside the subject is a detail in the background.
				constexpr float kCrowdReach = 70.0f;
				const float     crowd = Crowding(a_subject, a_pose.position,
						a_subjects.npcId, a_subjects.playerId, kCrowdReach);
				sight *= 1.0f - 0.75f * crowd;
			}

			return std::clamp(sight, 0.0f, 1.0f);
		}

		// How fast the camera is allowed to move back OUT, in units per second.
		//
		// RoomAlong is a single ray, which is a knife edge: a railing, a chair back
		// or a passing NPC's drawn weapon flips it fully on and off rather than
		// degrading. Wired straight into position, that reads as the camera zooming
		// in and out as things pass — the reported spazzing, and it is a pop rather
		// than a move because ApplyPose writes the result to the node with no
		// smoothing of any kind.
		//
		// ASYMMETRIC, AND THE ASYMMETRY IS THE WHOLE POINT. Pulling IN is never
		// limited: this load order runs No Camera Collision, so RoomAlong is the
		// only thing keeping the lens out of masonry and a rate limit on the way in
		// would let the camera sit inside a wall for the duration of the ramp.
		// Coming back out has no such urgency, so it is paced and the transient
		// becomes a shallow dip instead of a snap.
		//
		// Well clear of the dolly, which is the other thing that legitimately moves
		// the standoff: iDollyAmount=26 over iDollyWindow=420 is 26% of the standoff
		// across 4.2s, under 20 u/s on a typical shot. A limit that fought the dolly
		// would flatten the one move the shots actually author.
		constexpr float kStandoffRecovery = 300.0f;

		// Slides the whole camera sideways without turning it.
		//
		// Position AND aim move by the same vector, and that is the entire
		// difference between a slide and an orbit: the camera keeps looking in
		// exactly the direction it already was, so the subject drifts across frame
		// and eventually out of it. Every other move here ends with the aim
		// recomposed onto the subject; this one must not, which is why it is
		// applied out here after composition rather than in the move switch.
		void ApplyTruck(Pose& a_pose, float a_units)
		{
			if (a_units == 0.0f) {
				return;
			}

			bool       ok = false;
			const auto forward = Normalized(
				{ a_pose.lookAt.x - a_pose.position.x,
					a_pose.lookAt.y - a_pose.position.y,
					a_pose.lookAt.z - a_pose.position.z },
				ok);
			if (!ok) {
				return;
			}

			const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(forward, worldUp), ok);
			if (!ok) {
				return;
			}

			a_pose.position.x += right.x * a_units;
			a_pose.position.y += right.y * a_units;
			a_pose.position.z += right.z * a_units;
			a_pose.lookAt.x += right.x * a_units;
			a_pose.lookAt.y += right.y * a_units;
			a_pose.lookAt.z += right.z * a_units;
		}

		// The first frame of a shot is unheld, so a cut lands at its true distance
		// immediately. A cut is supposed to be instant; only the correction is not.
		[[nodiscard]] float LimitStandoff(const Subjects& a_subjects, float a_wanted)
		{
			if (a_subjects.heldStandoff <= kUnheld || !(a_subjects.delta > 0.0f)) {
				return a_wanted;
			}
			if (a_wanted <= a_subjects.heldStandoff) {
				return a_wanted;  // in, at full speed, always
			}
			return std::min(a_wanted, a_subjects.heldStandoff + kStandoffRecovery * a_subjects.delta);
		}
	}

	// Display names, and they are display names only — Key() below is the settings
	// contract and none of this touches it.
	//
	// WRITTEN FOR SOMEBODY WHO HAS NEVER BEEN ON A FILM SET. Every name here says
	// what will be on screen; none of them says what the technique is called. The
	// trade names went one at a time and each for a reason a player could feel:
	//
	//   Dirty Single       -> Over The Shoulder (Tight). It IS an over-the-
	//                         shoulder, just close enough that only an edge of
	//                         the other person is left. "Dirty" says nothing at
	//                         all unless you already know, and it sat alone
	//                         instead of joining the family it belongs to.
	//   Medium             -> Head And Shoulders. Medium what? The old name is
	//                         only meaningful against the sizes either side of
	//                         it, which are not on screen next to it.
	//   Low Angle          -> From Below, and High Angle -> From Above. Same
	//                         image, no vocabulary.
	//   Long               -> Full Figure. "Long" describes the lens to a crew
	//                         and the framing to nobody else.
	//   Profile            -> Side On.
	//   Master / Ground    -> The Whole Room / From The Floor.
	//   Two-Shot           -> Both Of You. The brief for this pass named it as
	//                         the example of a word to assume nobody knows.
	//
	// THE SHOULDER SHOTS ARE NAMED BY WHOSE SHOULDER IT IS, which is what makes
	// the two halves of the exchange tellable apart at all. "Over Your Shoulder"
	// is a shot OF THEM; the camera is behind you. That was the single most
	// confusable pair in the old set — the label said "Over The Shoulder" on both
	// sides of the eyeline and only the panel heading disambiguated.
	//
	// SEVEN NAMES ARE STILL DELIBERATELY DUPLICATED across the two sides: Close
	// Up, Extreme Close Up, Head And Shoulders, Three Quarters, From Below, Full
	// Figure and From High Above. Leave them alone. Those pairs are the SAME
	// framing on opposite sides of the eyeline — that is what a reverse shot is,
	// and matching names are how the Shots page shows that the pair go together.
	// The panel headings say which side you are reading, and the log prints
	// SubjectName beside the name, so neither surface is actually ambiguous.
	std::string_view Name(ShotType a_type) noexcept
	{
		switch (a_type) {
		// Shots of the NPC. The camera is behind the PLAYER for the shoulder
		// four, which is why they carry the player's pronoun.
		case ShotType::kOverPlayerShoulder:     return "Over Your Shoulder"sv;
		case ShotType::kOverPlayerShoulderLow:  return "Over Your Shoulder (Low)"sv;
		case ShotType::kOverPlayerShoulderHigh: return "Over Your Shoulder (High)"sv;
		case ShotType::kOverPlayerShoulderWide: return "Over Your Shoulder (Wide)"sv;
		case ShotType::kDirtyNpc:           return "Over Your Shoulder (Tight)"sv;
		case ShotType::kThreeQuarterNpc:    return "Three Quarters"sv;
		case ShotType::kCloseUp:            return "Close Up"sv;
		case ShotType::kExtremeClose:       return "Extreme Close Up"sv;
		case ShotType::kCloseLow:           return "Close Up (Low)"sv;
		case ShotType::kCloseHigh:          return "Close Up (High)"sv;
		case ShotType::kCloseProfile:       return "Close Up (Side On)"sv;
		case ShotType::kCloseWide:          return "Close Up (Wide)"sv;
		case ShotType::kMediumNpc:          return "Head And Shoulders"sv;
		case ShotType::kMediumProfile:      return "Head And Shoulders (Side On)"sv;
		case ShotType::kLowAngle:           return "From Below"sv;
		case ShotType::kLowProfile:         return "From Below (Side On)"sv;
		case ShotType::kLongNpc:            return "Full Figure"sv;
		case ShotType::kOverhead:           return "From High Above"sv;

		// Shots of the player.
		case ShotType::kOverNpcShoulder:        return "Over Their Shoulder"sv;
		case ShotType::kOverNpcShoulderLow:     return "Over Their Shoulder (Low)"sv;
		case ShotType::kOverNpcShoulderHigh:    return "Over Their Shoulder (High)"sv;
		case ShotType::kOverNpcShoulderWide:    return "Over Their Shoulder (Wide)"sv;
		case ShotType::kDirtyPlayer:        return "Over Their Shoulder (Tight)"sv;
		case ShotType::kThreeQuarterPlayer: return "Three Quarters"sv;
		case ShotType::kClosePlayer:        return "Close Up"sv;
		case ShotType::kExtremeClosePlayer: return "Extreme Close Up"sv;
		case ShotType::kMediumPlayer:       return "Head And Shoulders"sv;
		case ShotType::kPlayerProfile:      return "Side On"sv;
		case ShotType::kPlayerLow:          return "From Below"sv;
		case ShotType::kHighAngle:          return "From Above"sv;
		case ShotType::kLongPlayer:         return "Full Figure"sv;
		case ShotType::kPlayerOverhead:     return "From High Above"sv;

		// Shots of the pair, and of the room. "Profile" used to appear here AND
		// on the player's list for two entirely different shots — one of a person
		// and one of both of them — which is the one duplicate that was never
		// defensible.
		case ShotType::kTwoShot:            return "Both Of You"sv;
		case ShotType::kProfile:            return "Both Of You (Side On)"sv;
		case ShotType::kWide:               return "Wide"sv;
		case ShotType::kMaster:             return "The Whole Room"sv;
		case ShotType::kGroundLevel:        return "From The Floor"sv;
		case ShotType::kDistant:            return "From Far Off"sv;
		case ShotType::kDistantLow:         return "From Far Off (Low)"sv;
		default:                            return "unknown"sv;
		}
	}

	// WHAT USED TO BE HERE: Description(), one plain line per setup saying what
	// would be on screen — "Their face fills most of the frame."
	//
	// Written for a Shots page whose rows opened onto an explanation, and removed
	// with that explanation. Nothing displays it now: a look is chosen on the
	// Presets page in one tick, and the Shots page is a list of names for
	// switching angles on and off. Names carry it, which is why they were made
	// plain in the same pass.

	// Who the shot is of, in one word.
	//
	// Exists because Name() is now written for a menu where a heading already
	// says which side of the exchange you are looking at. The log has no such
	// heading, and "Close-up -> Close-up" would be a real cut between two
	// different angles that reads as no cut at all.
	std::string_view SubjectName(ShotType a_type) noexcept
	{
		if (IsNeutral(a_type)) {
			return "room"sv;
		}
		return FavoursNpc(a_type) ? "them"sv : "you"sv;
	}

	// Never rename one of these. They are keys in the player's settings file, and
	// a rename silently re-enables whatever the player turned off.
	const char* Key(ShotType a_type) noexcept
	{
		switch (a_type) {
		case ShotType::kOverPlayerShoulder: return "bOverPlayerShoulder";
		case ShotType::kOverNpcShoulder:    return "bOverNpcShoulder";
		case ShotType::kCloseUp:            return "bCloseUp";
		case ShotType::kMediumNpc:          return "bMediumNpc";
		case ShotType::kMediumPlayer:       return "bMediumPlayer";
		case ShotType::kLongNpc:            return "bLongNpc";
		case ShotType::kCloseProfile:       return "bCloseProfile";
		case ShotType::kCloseLow:           return "bCloseLow";
		case ShotType::kCloseHigh:          return "bCloseHigh";
		case ShotType::kCloseWide:          return "bCloseWide";
		case ShotType::kTwoShot:            return "bTwoShot";
		case ShotType::kProfile:            return "bProfile";
		case ShotType::kLowAngle:           return "bLowAngle";
		case ShotType::kHighAngle:          return "bHighAngle";
		case ShotType::kWide:               return "bWide";
		case ShotType::kDistant:            return "bDistant";
		case ShotType::kExtremeClose:       return "bExtremeClose";
		case ShotType::kExtremeClosePlayer: return "bExtremeClosePlayer";
		case ShotType::kDirtyNpc:           return "bDirtyNpc";
		case ShotType::kThreeQuarterNpc:    return "bThreeQuarterNpc";
		case ShotType::kMediumProfile:      return "bMediumProfile";
		case ShotType::kLowProfile:         return "bLowProfile";
		case ShotType::kOverhead:           return "bOverhead";
		case ShotType::kClosePlayer:        return "bClosePlayer";
		case ShotType::kDirtyPlayer:        return "bDirtyPlayer";
		case ShotType::kPlayerProfile:      return "bPlayerProfile";
		case ShotType::kPlayerLow:          return "bPlayerLow";
		case ShotType::kThreeQuarterPlayer: return "bThreeQuarterPlayer";
		case ShotType::kOverPlayerShoulderLow:  return "bOverPlayerShoulderLow";
		case ShotType::kOverPlayerShoulderHigh: return "bOverPlayerShoulderHigh";
		case ShotType::kOverPlayerShoulderWide: return "bOverPlayerShoulderWide";
		case ShotType::kOverNpcShoulderLow:  return "bOverNpcShoulderLow";
		case ShotType::kOverNpcShoulderHigh: return "bOverNpcShoulderHigh";
		case ShotType::kOverNpcShoulderWide: return "bOverNpcShoulderWide";
		case ShotType::kLongPlayer:         return "bLongPlayer";
		case ShotType::kPlayerOverhead:     return "bPlayerOverhead";
		case ShotType::kMaster:             return "bMaster";
		case ShotType::kGroundLevel:        return "bGroundLevel";
		case ShotType::kDistantLow:         return "bDistantLow";
		default:                            return "bUnknown";
		}
	}

	namespace
	{
		// Derived from Key() rather than more switches of forty-three literals.
		// Several hand-maintained tables of the same names are one rename away from
		// a setting that saves to one key and loads from another.
		// THE PREFIX IS A PARAMETER because one key in this family is not an
		// integer.
		//
		// Everything a setup stores has been a number until now, so the "i" was
		// baked in. The lighting rig is a NAME — see lightRig above for why an
		// ordinal could not carry it — and an ini whose string values are spelled
		// with an integer prefix is a small lie that costs nothing to avoid.
		const char* TypedKey(ShotType a_type, const char* a_prefix, const char* a_suffix,
			std::array<std::string, static_cast<std::size_t>(ShotType::kCount)>& a_cache)
		{
			if (a_cache[0].empty()) {
				for (std::size_t i = 0; i < a_cache.size(); ++i) {
					const char* base = Key(static_cast<ShotType>(i));
					a_cache[i] = std::string{ a_prefix } +
								 (base && *base ? base + 1 : "Unknown") + a_suffix;
				}
			}

			const auto index = static_cast<std::size_t>(a_type);
			return index < a_cache.size() ? a_cache[index].c_str() : "iUnknown";
		}

		const char* SuffixedKey(ShotType a_type, const char* a_suffix,
			std::array<std::string, static_cast<std::size_t>(ShotType::kCount)>& a_cache)
		{
			return TypedKey(a_type, "i", a_suffix, a_cache);
		}
	}

	const char* WeightKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Weight", cache);
	}

	const char* LensKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Fov", cache);
	}

	const char* MoveKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Move", cache);
	}

	const char* MoveAmountKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "MoveAmount", cache);
	}

	const char* MoveTimeKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "MoveTime", cache);
	}

	// Read once, to migrate. See the note on SetMove.
	const char* ZoomKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Zoom", cache);
	}

	const char* LightKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return TypedKey(a_type, "s", "Light", cache);
	}

	const char* LightXKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "LightX", cache);
	}

	const char* LightYKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "LightY", cache);
	}

	const char* LightZKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "LightZ", cache);
	}

	const char* AuthoredLight(ShotType a_type) noexcept
	{
		// HOW THESE WERE CHOSEN, because "which rig suits a close-up" is not a
		// question with an obvious answer and the table below is otherwise just
		// thirty-nine assertions.
		//
		// ONE PRINCIPLE DOES MOST OF THE WORK: a face-modelling rig is only worth
		// running on a shot where the face is big enough to be modelled. A key
		// aimed at a head from across a room lights a speck and spills over
		// everything between, so the wides and the masters ship on Natural or on
		// nothing at all. This is the same shape as the lens table — the setups
		// differ most where the subject is largest — and it is why the room shots
		// look untouched, which is correct: the room was already lit by the people
		// who built it.
		//
		// THE SECOND PRINCIPLE IS THAT ANGLE AND RIG SHOULD AGREE. A profile is
		// already a shot about the shape of a head, so it gets Rembrandt, which is
		// about exactly that. A low angle is already a shot about somebody having
		// the advantage, so it gets Hard. The extreme closes get Hard on both sides
		// because they are gated to intensity-100 lines and there is no such thing
		// as a gentle one.
		//
		// Every one of these is a default. The point of the per-setup key is that
		// none of it has to be agreed with.
		switch (a_type) {
		// The tightest and the lowest. Both are angles about somebody having the
		// advantage, and Hard is the look about the same thing. The extremes are
		// gated to intensity-100 lines anyway, and there is no gentle version of
		// one of those.
		case ShotType::kExtremeClose:
		case ShotType::kExtremeClosePlayer:
		case ShotType::kCloseLow:
		case ShotType::kLowAngle:
		case ShotType::kLowProfile:
		case ShotType::kPlayerLow:
			return "hard";

		// The close range, where a face is large enough for a fill to be worth
		// having.
		case ShotType::kCloseUp:
		case ShotType::kClosePlayer:
		case ShotType::kCloseHigh:
		case ShotType::kHighAngle:
		case ShotType::kCloseProfile:
		case ShotType::kProfile:
		case ShotType::kDirtyNpc:
		case ShotType::kDirtyPlayer:
			return "soft";

		// The room, and the shots that are mostly room. Nothing here is a portrait
		// and a key on any of them is a bright patch on a floor.
		case ShotType::kDistant:
		case ShotType::kDistantLow:
		case ShotType::kMaster:
			return "off";

		default:
			return "natural";
		}
	}

	bool FavoursNpc(ShotType a_type) noexcept
	{
		return SpecFor(a_type).onNpc;
	}

	std::string_view MoveName(ShotType a_type) noexcept
	{
		switch (SpecFor(a_type).move) {
		case Move::kLocked:    return "locked"sv;
		case Move::kPushIn:    return "push-in"sv;
		case Move::kPullOut:   return "pull-out"sv;
		case Move::kCraneUp:   return "crane-up"sv;
		case Move::kCraneDown: return "crane-down"sv;
		case Move::kTiltUp:    return "tilt-up"sv;
		case Move::kTiltDown:  return "tilt-down"sv;
		case Move::kDrift:     return "drift"sv;
		case Move::kZoomIn:    return "zoom-in"sv;
		case Move::kZoomOut:   return "zoom-out"sv;
		default:               return "locked"sv;
		}
	}

	float LensOf(ShotType a_type) noexcept
	{
		// The LIVE lens, not the authored one. The cut log prints this, and a log
		// reporting the table's value while the camera shot something else would be
		// the least useful kind of wrong. AuthoredLens() is there for the original.
		return static_cast<float>(Shot::Lens(a_type));
	}

	float FillOf(ShotType a_type) noexcept
	{
		return SpecFor(a_type).fill;
	}

	bool OverShoulder(ShotType a_type) noexcept
	{
		return SpecFor(a_type).overShoulder;
	}

	bool IsNeutral(ShotType a_type) noexcept
	{
		switch (a_type) {
		case ShotType::kTwoShot:
		case ShotType::kProfile:
		case ShotType::kWide:
		case ShotType::kDistant:
		case ShotType::kMaster:
		case ShotType::kGroundLevel:
		case ShotType::kDistantLow:
			return true;
		default:
			return false;
		}
	}

	void Shot::SetEnabled(ShotType a_type, bool a_enabled) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < selectionSettings.enabled.size()) {
			selectionSettings.enabled[index].store(a_enabled, std::memory_order_relaxed);
		}
	}

	bool Shot::Enabled(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < selectionSettings.enabled.size() &&
			selectionSettings.enabled[index].load(std::memory_order_relaxed);
	}

	void Shot::SetWeight(ShotType a_type, int a_weight) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < selectionSettings.weights.size()) {
			selectionSettings.weights[index].store(static_cast<std::uint8_t>(std::clamp(a_weight, 0, 100)),
				std::memory_order_relaxed);
		}
	}

	int Shot::Weight(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < selectionSettings.weights.size() ?
			selectionSettings.weights[index].load(std::memory_order_relaxed) : 0;
	}

	void Shot::SetLens(ShotType a_type, int a_degrees) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < lensDegrees.size()) {
			lensDegrees[index] =
				static_cast<std::uint8_t>(std::clamp(a_degrees, kMinLens, kMaxLens));
		}
	}

	int Shot::Lens(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lensDegrees.size() ?
			lensDegrees[index] :
			static_cast<int>(SpecFor(a_type).lens);
	}

	float AuthoredLens(ShotType a_type) noexcept
	{
		return SpecFor(a_type).lens;
	}

	int AuthoredWeight(ShotType a_type) noexcept
	{
		// The eight setups the pools used to list twice, and nothing else. Kept as
		// its own switch rather than a thirteenth column on ShotSpec: that table is
		// positional aggregate initialisation across thirty-nine cases, and adding
		// a field to it to say "50" thirty-one times would be the most error-prone
		// way available to express a two-value fact.
		//
		// These are the staples of filmed dialogue — the shoulder pair, the two
		// mediums, the two dirty singles, the two three-quarters, and the close-up
		// on the speaker. Everything else in the mod is an accent against them.
		switch (a_type) {
		case ShotType::kCloseUp:
		case ShotType::kMediumNpc:
		case ShotType::kOverPlayerShoulder:
		case ShotType::kDirtyNpc:
		case ShotType::kThreeQuarterNpc:
		case ShotType::kMediumPlayer:
		case ShotType::kOverNpcShoulder:
		case ShotType::kDirtyPlayer:
			return kDefaultWeight * 2;
		default:
			return kDefaultWeight;
		}
	}

	void Shot::SetMove(ShotType a_type, Move a_move) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < moveChoice.size() && a_move < Move::kCount) {
			moveChoice[index] = static_cast<std::uint8_t>(a_move);
		}
	}

	Move Shot::MoveOf(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < moveChoice.size() ? static_cast<Move>(moveChoice[index]) : Move::kLocked;
	}

	void Shot::SetMoveAmount(ShotType a_type, int a_strength) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < moveStrength.size()) {
			moveStrength[index] = static_cast<std::uint8_t>(std::clamp(a_strength, 0, 100));
		}
	}

	int Shot::MoveAmount(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < moveStrength.size() ? moveStrength[index] : 0;
	}

	void Shot::SetMoveTime(ShotType a_type, int a_hundredths) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < moveTime.size()) {
			moveTime[index] = static_cast<std::uint16_t>(std::clamp(a_hundredths, 30, 900));
		}
	}

	int Shot::MoveTime(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < moveTime.size() ? moveTime[index] : kDefaultMoveTime;
	}

	void Shot::SetLight(ShotType a_type, int a_look) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < lightRig.size()) {
			lightRig[index] = static_cast<std::int8_t>(std::clamp(a_look, -1, 127));
		}
	}

	int Shot::LightOf(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightRig.size() ? lightRig[index] : -1;
	}

	void Shot::SetLightOffset(ShotType a_type, int a_x, int a_y, int a_z) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < lightOffX.size()) {
			lightOffX[index] = static_cast<std::int16_t>(std::clamp(a_x, -400, 400));
			lightOffY[index] = static_cast<std::int16_t>(std::clamp(a_y, -400, 400));
			lightOffZ[index] = static_cast<std::int16_t>(std::clamp(a_z, -400, 400));
		}
	}

	int Shot::LightOffsetX(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightOffX.size() ? lightOffX[index] : 0;
	}

	int Shot::LightOffsetY(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightOffY.size() ? lightOffY[index] : 0;
	}

	int Shot::LightOffsetZ(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightOffZ.size() ? lightOffZ[index] : 0;
	}

	Move Shot::AuthoredMove(ShotType a_type) noexcept
	{
		return SpecFor(a_type).move;
	}

	int Shot::AuthoredMoveAmount(ShotType a_type) noexcept
	{
		return AuthoredStrength(a_type);
	}

	int Shot::AuthoredMoveTime(ShotType) noexcept
	{
		return kDefaultMoveTime;
	}

	std::string_view MoveLabel(Move a_move) noexcept
	{
		switch (a_move) {
		case Move::kLocked:     return "Locked off"sv;
		case Move::kPushIn:     return "Push in"sv;
		case Move::kPullOut:    return "Pull out"sv;
		case Move::kCraneUp:    return "Crane up"sv;
		case Move::kCraneDown:  return "Crane down"sv;
		case Move::kTiltUp:     return "Tilt up"sv;
		case Move::kTiltDown:   return "Tilt down"sv;
		case Move::kDrift:      return "Drift"sv;
		case Move::kZoomIn:     return "Zoom in"sv;
		case Move::kZoomOut:    return "Zoom out"sv;
		case Move::kOrbitLeft:  return "Orbit left"sv;
		case Move::kOrbitRight: return "Orbit right"sv;
		case Move::kTruckLeft:  return "Slide left"sv;
		case Move::kTruckRight: return "Slide right"sv;
		default:                return "?"sv;
		}
	}


	void CheckVisibility(ShotType a_type, Pose& a_pose, const Subjects& a_subjects)
	{
		a_pose.visibility = {};
		a_pose.lensClearance = SightState::kUnknown;
		if (!a_pose.valid) {
			return;
		}

		SightContext localContext{};
		const SightContext* context = a_subjects.sightContext;
		if (!context) {
			localContext = BuildSightContext();
			context = &localContext;
		}
		a_pose.lensClearance = LensClearance(a_pose.position, *context);
		if (a_pose.lensClearance == SightState::kBlocked) {
			a_pose.visibility.state = SightState::kBlocked;
			a_pose.visibility.severe = true;
			return;
		}

		const auto spec = SpecFor(a_type);
		const float lens = a_pose.lens > 0.0f ? a_pose.lens : a_subjects.fovDegrees;
		const auto& subject = spec.onNpc ? a_subjects.npcSight : a_subjects.playerSight;
		a_pose.visibility = SubjectVisibility(a_pose.position, a_pose.lookAt, lens,
			a_subjects.aspect, a_subjects.cropFractionPerEdge, subject, *context);

		// A two-person or room setup must actually show both faces. The listener
		// in an over-the-shoulder remains optional foreground: only covering the
		// primary subject's protected samples can disqualify that composition.
		if (spec.anchor != Anchor::kSubject || spec.aim != Aim::kSubject) {
			const auto& other = spec.onNpc ? a_subjects.playerSight : a_subjects.npcSight;
			const auto sight = SubjectVisibility(a_pose.position, a_pose.lookAt, lens,
				a_subjects.aspect, a_subjects.cropFractionPerEdge, other, *context);
			a_pose.visibility.face = std::min(a_pose.visibility.face, sight.face);
			a_pose.visibility.torso = std::min(a_pose.visibility.torso, sight.torso);
			a_pose.visibility.severe = a_pose.visibility.severe || sight.severe;
			if (sight.state == SightState::kBlocked || a_pose.visibility.state == SightState::kBlocked) {
				a_pose.visibility.state = SightState::kBlocked;
			} else if (sight.state != SightState::kClear || a_pose.visibility.state != SightState::kClear) {
				a_pose.visibility.state = SightState::kUnknown;
			}
		}
		if (a_pose.lensClearance != SightState::kClear && a_pose.visibility.state == SightState::kClear) {
			a_pose.visibility.state = SightState::kUnknown;
		}
	}

	Pose Solve(ShotType a_type, const Subjects& a_subjects)
	{
		Pose       pose{};
		if (!Shot::Enabled(a_type) || Shot::Weight(a_type) <= 0) {
			return pose;
		}
		// A carried placement cannot reintroduce a wide sweep through the held
		// branch, which deliberately skips candidate generation.
		if (!std::isfinite(a_subjects.heldSweep) ||
			(a_subjects.heldSweep > kUnheld && !ShotAngles::AllowedAdjustment(a_subjects.heldSweep))) {
			return pose;
		}
		const auto spec = SpecFor(a_type);

		const RE::NiPoint3 subject = spec.onNpc ? a_subjects.npcHead : a_subjects.playerHead;
		const RE::NiPoint3 other = spec.onNpc ? a_subjects.playerHead : a_subjects.npcHead;

		// Whose measurements the framing uses.
		//
		// A subject-anchored setup takes its own subject's. A midpoint or scene
		// setup frames both parties, so it takes the LARGER of the two — a
		// two-shot of a person and a dragon composed against the person is a
		// two-shot of a person and a shin.
		const Anatomy& body = spec.anchor == Anchor::kSubject ?
			(spec.onNpc ? a_subjects.npc : a_subjects.player) :
			(a_subjects.npc.extent >= a_subjects.player.extent ? a_subjects.npc : a_subjects.player);

		const RE::NiPoint3 midpoint{
			(a_subjects.playerHead.x + a_subjects.npcHead.x) * 0.5f,
			(a_subjects.playerHead.y + a_subjects.npcHead.y) * 0.5f,
			(a_subjects.playerHead.z + a_subjects.npcHead.z) * 0.5f
		};

		// The base direction is from the subject toward the person they are
		// talking to — look back along it and you see their face.
		bool       ok = false;
		const auto toOther = Normalized(
			{ other.x - subject.x, other.y - subject.y, other.z - subject.z }, ok);
		if (!ok) {
			return pose;
		}

		const float separation = Length(
			RE::NiPoint3{ other.x - subject.x, other.y - subject.y, other.z - subject.z });

		// The move, eased across the life of the shot and scaled by the player's
		// movement dial.
		const float p = Ease(a_subjects.progress);

		// The shot's own glass. Falling back to whatever the player is running
		// means a shot with no opinion still composes against the right frame.
		// Clamped to the range DistanceForFill actually solves over, BEFORE the
		// distance is taken from it.
		//
		// That function clamps its own argument to 40-120 internally, so a base
		// lens below 40 — now reachable, since the extreme close-up sits exactly on
		// 40 and a preset may bias downward from there — would have the standoff
		// solved for one field of view and the frame rendered at another. The
		// subject comes out larger than the fill asked for, which for the tightest
		// setup in the table means straight through the distance floor.
		//
		// The post-move clamp below stays wider on purpose: a zoom is allowed to
		// narrow the RENDERED lens past this, because by then the standoff is
		// already fixed and magnifying is the whole point.
		// The setup's live field of view, which is its authored one until somebody
		// moves it. The "shot has no opinion, use the player's own FOV" branch that
		// used to sit here is gone: every entry in the table names a real lens from
		// the palette, so it was unreachable.
		float lens = static_cast<float>(
			std::clamp(Shot::Lens(a_type), kMinLens, kMaxLens));

		// What this build wants doing differently. A humanoid gets 1.0, 1.0, 0 and
		// nothing below changes for it — this is the path every ordinary
		// conversation takes and the table was authored against it exactly.
		const auto tuning = Tuning(body.build);

		// Distance is solved at the shot's BASE lens, before any zoom is applied,
		// so a zoom magnifies away from the size the shot asked for rather than
		// starting somewhere else and arriving at it.
		//
		// The fill is capped by the build. This is what answers the reported shot:
		// kExtremeClose asks for 0.85 of frame height, which is a face cropped
		// below the chin on a person and the underside of the jaw on a dragon.
		float distance = DistanceForFill(
			std::min(spec.fill, tuning.fillCap), lens, a_subjects.aspect, body);

		// Scaled with the subject, because rise is in WORLD UNITS — and then held
		// back by the build, because it should not scale all the way. Everything
		// else in the table is a fraction or a degree and scales itself; this
		// column and this column alone is absolute.
		float rise = spec.rise * body.scale * tuning.riseScale;

		// HELD UNDER THE CEILING THAT WAS ACTUALLY MEASURED.
		//
		// Only upward. A low angle ducking below the eyeline has a floor to worry
		// about, and the floor is where the people are standing — it cannot be a
		// surprise the way a beam over a bed alcove can.
		//
		// riseScale above stays exactly as it is. It is a FRAMING judgement — a
		// dragon reads better from at or below its head, whatever the room allows —
		// and this is a physical limit. Rolling one into the other is how a
		// framing constant came to be the only thing keeping the camera indoors.
		if (rise > 0.0f && a_subjects.ceiling > 1.0f) {
			constexpr float kCeilingMargin = 24.0f;
			rise = std::min(rise, std::max(a_subjects.ceiling - kCeilingMargin, 0.0f));
		}

		// Stepped further off the eyeline on a long head. Signed with the angle so
		// a shot that already steps left steps further left rather than crossing.
		float angle = spec.angleDeg +
			(spec.angleDeg < 0.0f ? -tuning.angleBias : tuning.angleBias);
		float aimLift = 0.0f;

		// THE MOVE IS THE PLAYER'S, NOT THE TABLE'S.
		//
		// The table still chooses what each setup ships doing, and that default is
		// what AuthoredMove restores — but what runs here is whatever is configured.
		// The amount is a 0-100 strength against this move's own full travel, so
		// switching a setup from a push to an orbit keeps its intensity, and a
		// locked setup switched to anything at all actually moves. Under the old
		// scheme it did not: a locked setup carries an authored amount of zero, and
		// the zoom override had to smuggle in a hardcoded 0.12 to paper over it.
		const Move  move = Shot::MoveOf(a_type);
		const float travel = FullScale(move) *
			(static_cast<float>(Shot::MoveAmount(a_type)) / 100.0f);

		// World-unit moves scale with the subject for the same reason rise does.
		// Fractions and degrees are already relative and must not be touched.
		const float units = travel * body.scale * tuning.riseScale;

		// Signed lateral slide, applied after the aim is composed. Zero for every
		// other move. See the note where it is used.
		float truck = 0.0f;

		switch (move) {
		case Move::kLocked:                                     break;
		case Move::kPushIn:    distance *= 1.0f - travel * p;   break;
		case Move::kPullOut:   distance *= 1.0f + travel * p;   break;
		case Move::kZoomIn:    lens *= 1.0f - travel * p;       break;
		case Move::kZoomOut:   lens *= 1.0f + travel * p;       break;
		case Move::kCraneUp:   rise += units * p;               break;
		case Move::kCraneDown: rise -= units * p;               break;
		case Move::kTiltUp:    aimLift = units * p;             break;
		case Move::kTiltDown:  aimLift = -units * p;            break;

		// An orbit arcs the camera around the subject while still pointing at them.
		// That is what the angle already does — it is the direction from the anchor
		// out to the camera — so an orbit is a change to it over the life of the
		// shot, and the aim recomposes from the new position automatically.
		//
		// kDrift is the same move without a stated direction, kept so saved configs
		// keep working.
		case Move::kDrift:
		case Move::kOrbitRight: angle += travel * p;            break;
		case Move::kOrbitLeft:  angle -= travel * p;            break;

		// A slide does NOT recompose. Deferred out of this switch entirely because
		// it is the one move that cannot be expressed as a change to the standoff,
		// the rise, the angle or the lens — those all feed a pose that is then
		// aimed at the subject, and the whole point of a slide is that the aim
		// stays put while the camera leaves.
		case Move::kTruckLeft:  truck = -units * p;             break;
		case Move::kTruckRight: truck = units * p;              break;
		default:                                                break;
		}

		lens = std::clamp(lens, 30.0f, 120.0f);
		pose.lens = lens;

		// An over-the-shoulder has to stand beyond the other person, or there is no
		// shoulder in the corner of the frame to justify the name.
		if (spec.overShoulder) {
			distance = std::max(distance, separation + 60.0f);
		}

		// Where the camera STANDS and what it POINTS AT are two questions now.
		// They used to be one — a 90-degree lateral was the only way to ask for
		// the midpoint, which meant "frame both of them" could not be requested
		// without also standing side-on.
		const RE::NiPoint3 anchorPoint = spec.anchor == Anchor::kSubject ? subject : midpoint;
		const RE::NiPoint3 aimPoint = spec.aim == Aim::kSubject ? subject : midpoint;
		const RE::NiPoint3 target{ aimPoint.x, aimPoint.y, aimPoint.z + 3.0f + aimLift };

		if (a_subjects.protectSubject) {
			// Compose and verify each bearing before choosing it. A blocked roomy
			// angle must not hide a readable neighbour of the same enabled setup.
			// Keep this branch separate so saved legacy placement retains its exact
			// bundle scoring and hold-placement behavior.
			const bool held = a_subjects.heldSweep > kUnheld;
			const bool check = !held || a_subjects.checkVisibility;
			SightContext localContext{};
			Subjects checkedSubjects = a_subjects;
			if (check && !checkedSubjects.sightContext) {
				localContext = BuildSightContext();
				checkedSubjects.sightContext = &localContext;
			}

			const bool scene = spec.anchor == Anchor::kScene;
			const float wanted = scene ?
				std::clamp(std::min(distance, a_subjects.openDistance * 0.9f), 200.0f, 2400.0f) : distance;
			const auto baseDirection = scene ? a_subjects.openDirection : toOther;
			ShotAngles::Candidates candidates{};
			std::size_t count = 1;
			if (held) {
				candidates[0] = angle + a_subjects.heldSweep;
			} else {
				const float floor = spec.anchor == Anchor::kMidpoint ? kLineFloorMidpoint : kLineFloorSubject;
				count = ShotAngles::MakeCandidates(angle, floor, a_subjects.enforceLine && !scene, candidates);
			}

			Pose best{};
			Pose rejected{};
			float bestScore = -1.0f;
			for (std::size_t i = 0; i < count; ++i) {
				const float offset = candidates[i] - angle;
				const auto direction = RotateAboutZ(baseDirection, candidates[i] * kDeg * (scene ? 1.0f : a_subjects.side));
				const auto compose = [&](float available, float rememberedRoom) {
					const float use = LimitStandoff(a_subjects,
						std::max(std::min(wanted, available), body.minDistance));
					Pose result{};
					result.position = {
						anchorPoint.x + direction.x * use,
						anchorPoint.y + direction.y * use,
						anchorPoint.z + direction.z * use + rise
					};
					result.lookAt = Compose(result.position, target, toOther, lens, a_subjects.aspect,
						spec.headroom, spec.lookRoom);
					ApplyTruck(result, truck);
					result.lens = lens;
					result.sweep = offset;
					result.standoff = use;
					result.room = rememberedRoom;
					result.valid = true;
					if (check) {
						CheckVisibility(a_type, result, checkedSubjects);
					}
					const float sight = check ?
						0.8f * result.visibility.face + 0.2f * result.visibility.torso : 1.0f;
					result.quality = Score(wanted, use, sight, offset);
					return result;
				};
				// The stable placement anchor and the final optical sightline differ,
				// especially during a slide. Test the actual requested pose first.
				// A proposal ray through harmless foreground must never veto it.
				const float room = held && a_subjects.holdPlacement && a_subjects.heldRoom > kUnheld ?
					a_subjects.heldRoom : kOpenRoom;
				auto candidate = compose(room >= kOpenRoom ? wanted : room, room);
				if (held) {
					// Placement is deliberately retained even on a blocked/unknown
					// reading so Director can time a cut without camera pumping.
					return candidate;
				}
				if ((candidate.visibility.state != SightState::kClear ||
					(a_subjects.requireFullFace && candidate.visibility.face < 0.99f)) && spec.anchor == Anchor::kSubject) {
					const auto shorter = NarrowRoom(anchorPoint, direction, rise, wanted, body.probeStart, body.extent);
					if (shorter.distance >= body.minDistance && shorter.distance < wanted - 1.0f) {
						candidate = compose(shorter.distance, shorter.distance);
					}
				}
				if (candidate.visibility.state != SightState::kClear ||
					(a_subjects.requireFullFace && candidate.visibility.face < 0.99f)) {
					rejected = candidate;
					rejected.valid = false;
					rejected.quality = 0.0f;
					continue;
				}
				if (candidate.quality > bestScore) {
					bestScore = candidate.quality;
					best = candidate;
				}
				// The bounded sweep costs at most nine placements. A near-perfect
				// admitted angle already beats any materially different composition.
				if (bestScore >= 0.985f) {
					break;
				}
			}
			return best.valid ? best : rejected;
		}

		// The room, not the people.
		//
		// This is the placement the mod did not have. Every other setup here hangs
		// off somebody's head node and can only ever be a different radius around
		// it; these start from the direction the space actually opens in, stand off
		// in it, and let the participants land where they land in the frame.
		//
		// angleDeg is read differently on this path — degrees off the OPEN
		// direction rather than off the eyeline — so several room shots can look
		// at the same conversation from genuinely different corners instead of all
		// lining up along the one open axis.
		//
		// The 180-degree rule is deliberately not applied. These are establishing
		// shots with no subject to be on the wrong side of, and forcing them onto
		// the sanctioned side would throw away half the room.
		if (spec.anchor == Anchor::kScene) {
			// What the framing wants, capped by what the room actually has. The
			// ceiling matches DistanceForFill's: outdoors and in the big interiors
			// the probe returns real distance and a long lens gets to use it, while
			// a corridor still reports a corridor and pulls the shot in.
			const float reach = std::clamp(std::min(distance, a_subjects.openDistance * 0.9f),
				200.0f, 2400.0f);

			// Try the swung angle first, then walk back toward the open direction.
			//
			// The swing is what stops the long lens standing in the same spot every
			// time it is chosen. This is the one axis in the mod where the camera had
			// no reason to be anywhere in particular: the open direction is a
			// measurement rather than a composition, so every room shot lining up
			// along it meant the widest setups all shared one vantage point.
			//
			// Walked back rather than simply refused, because the open direction is
			// where the room demonstrably IS. A sixty-degree swing may well face a
			// wall, and giving up there would make the setting look like it disabled
			// the shot rather than moved it.
			// A room shot stands where the space opens, at its own authored angle
			// off that direction, and that is now the whole of it.
			//
			// The randomised swing this used to walk back through is gone with
			// iRoomSwing; the loop stays as a loop of one so the held-angle
			// bookkeeping below is identical on both paths and there is one shape to
			// reason about rather than two.
			const std::array<float, 1> swings{
				a_subjects.heldSweep > kUnheld ? a_subjects.heldSweep : 0.0f
			};

			for (const float swing : swings) {
				const auto  direction = RotateAboutZ(a_subjects.openDirection, (angle + swing) * kDeg);
				const Room  room = RoomHere(a_subjects, anchorPoint, direction, rise, reach,
					body.probeStart, body.extent);
				if (room.distance < body.minDistance) {
					continue;
				}

				const float use = LimitStandoff(a_subjects, std::min(reach, room.distance));
				pose.position = {
					anchorPoint.x + direction.x * use,
					anchorPoint.y + direction.y * use,
					anchorPoint.z + direction.z * use + rise
				};
				pose.lookAt = Compose(pose.position, target, toOther, lens, a_subjects.aspect,
					spec.headroom, spec.lookRoom);
				ApplyTruck(pose, truck);
				pose.sweep = swing;
				pose.standoff = use;
				pose.room = RememberRoom(a_subjects, room.distance, room.clear);
				pose.valid = true;

				// A room shot is about the space, so a body crossing it costs less
				// than it would on a single — but both people still have to be
				// visible in it or it is a wide shot of a wall.
				const float sight = Visibility(pose, subject, other, spec, truck, a_subjects);
				pose.quality = Score(reach, use, room.clear * sight, swing);
				return pose;
			}

			return pose;
		}

		// Sweep for an angle with room, keeping the subject the size the shot asked
		// for. Distance is only reduced as a last resort, and never past the floor.
		//
		// Search only small adjustments around this shot's intended bearing.
		// The same hard limit applies with the line rule on or off.
		const float lineFloor = spec.anchor == Anchor::kMidpoint ?
			kLineFloorMidpoint : kLineFloorSubject;

		float        bestRoom = 0.0f;
		float        bestClear = 1.0f;
		float        bestOffset = 0.0f;
		RE::NiPoint3 bestDirection = RotateAboutZ(toOther, angle * kDeg * a_subjects.side);

		if (a_subjects.heldSweep > kUnheld) {
			// The shot already chose its angle. Whether the room along it is
			// re-measured is now the player's call — see Subjects::holdPlacement —
			// but the CHOICE is not remade either way. See Subjects::heldSweep.
			//
			// Left on, re-measuring is what solves the standoff against a room that
			// has since changed, and what refuses the shot when somebody has stood
			// in it. Turned off, both of those stop happening on purpose: the shot
			// holds the frame it cut on.
			bestOffset = a_subjects.heldSweep;
			bestDirection = RotateAboutZ(toOther, (angle + bestOffset) * kDeg * a_subjects.side);
			const Room held = RoomHere(a_subjects, anchorPoint, bestDirection, rise, distance,
				body.probeStart, body.extent);
			bestRoom = held.distance;
			bestClear = held.clear;
		} else {
			// BEST, NOT FIRST-THAT-FITS, and the early break is gone with it.
			//
			// The old loop stopped at the first bearing with enough room, which is
			// the same "accept anything legal" the picker did one level up: a
			// bearing that clears by a unit with a railing across half the frame
			// ended the search, and the bearing a few degrees on with the whole
			// room in front of it was never measured. Scoring makes that visible,
			// so it is worth the remaining probes to find it.
			//
			// The budget is bounded the other way instead: candidates are
			// de-duplicated by ShotAngles, so a setup pinned near the floor
			// probes four or five bearings rather than nine.
			ShotAngles::Candidates candidates{};
			const std::size_t                     count =
				ShotAngles::MakeCandidates(angle, lineFloor, a_subjects.enforceLine, candidates);

			float bestScore = -1.0f;

			for (std::size_t i = 0; i < count; ++i) {
				const float swept = candidates[i] * kDeg * a_subjects.side;
				const auto  direction = RotateAboutZ(toOther, swept);
				const Room  room = RoomAlong(anchorPoint, direction, rise, distance, body.probeStart, body.extent);

				if (room.distance < body.minDistance) {
					continue;  // cannot stand here at all
				}

				const float offset = candidates[i] - angle;
				const float score = Score(distance, std::min(distance, room.distance), room.clear, offset);
				if (score > bestScore) {
					bestScore = score;
					bestRoom = room.distance;
					bestClear = room.clear;
					bestOffset = offset;
					bestDirection = direction;
				}
			}

			// Nothing placed. Fall through to the refusal below with bestRoom still
			// zero rather than reporting the last measurement, which would let a
			// blocked bearing past the floor test on its own numbers.
			if (bestScore < 0.0f) {
				bestRoom = 0.0f;
			}
		}

		// Nowhere in the sweep has room for this shot without putting the lens
		// inside somebody. Refusing is correct — the caller will try a tighter one,
		// and a tighter shot needs less room, so the search converges.
		//
		// On a HELD angle this is also the escape hatch: if the one direction the
		// shot committed to becomes genuinely blocked, refusing hands the director
		// its remembered pose, which holds still. That is the right failure. The old
		// behaviour — quietly re-solving to whichever neighbour had room this frame
		// — is the swinging itself.
		if (bestRoom < body.minDistance) {
			return pose;
		}

		// A max, not a clamp, AND THAT IS A BUG FIX RATHER THAN A TIDY-UP.
		//
		// This was std::clamp(min(distance, bestRoom), body.minDistance, distance),
		// whose bounds can invert. DistanceForFill floors `distance` at
		// body.minDistance, but the move applied above scales it back DOWN: a
		// push-in takes up to forty per cent off, so a setup solving near the floor
		// arrives here asking for less than it. kCloseLow is the worked example —
		// 0.66 fill on a 60-degree lens solves to about 100 units against a floor
		// of 68, and Amount at 100 on the Shots page takes it to 60. The clamp is
		// then called with lo=68 and hi=60, which the standard leaves undefined and
		// a checked build asserts on.
		//
		// The upper bound was never load-bearing: min(distance, bestRoom) cannot
		// exceed `distance`, so that arm of the clamp was unreachable in every case
		// where the bounds were valid. Dropping it changes no result — including in
		// the inverted case, where MSVC's release clamp returns lo and so does this
		// — and leaves the floor saying the one thing it was there to say.
		const float use = LimitStandoff(a_subjects,
			std::max(std::min(distance, bestRoom), body.minDistance));

		pose.position = {
			anchorPoint.x + bestDirection.x * use,
			anchorPoint.y + bestDirection.y * use,
			anchorPoint.z + bestDirection.z * use + rise
		};
		pose.lookAt = Compose(pose.position, target, toOther, lens, a_subjects.aspect,
			spec.headroom, spec.lookRoom);
		ApplyTruck(pose, truck);
		pose.sweep = bestOffset;
		pose.standoff = use;
		pose.room = RememberRoom(a_subjects, bestRoom, bestClear);
		pose.valid = true;

		// Scored against what the shot ASKED for, not against what it settled on.
		// `use` is already the compromise; measuring it against itself would give
		// every placement full marks and there would be nothing to choose between.
		const float sight = Visibility(pose, subject, other, spec, truck, a_subjects);
		pose.quality = Score(distance, use, bestClear * sight, bestOffset);
		return pose;
	}
}

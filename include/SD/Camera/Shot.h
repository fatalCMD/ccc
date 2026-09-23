#pragma once

#include "SD/Camera/Anatomy.h"
#include "SD/Camera/Space.h"

namespace SD::Camera
{
	enum class ShotType : std::uint8_t
	{
		// NPC coverage — used while they are talking.
		kOverPlayerShoulder = 0,  // their face, your shoulder in the corner
		kCloseUp,                 // their face fills the frame
		kMediumNpc,               // head and shoulders, clean
		kLongNpc,                 // full figure, room around them
		kLowAngle,                // close, from below eye level

		// Variation inside the close range, where a conversation scene spends most
		// of its time. Without these, "close on the speaker" is a single repeated
		// setup no matter how often it is chosen.
		kCloseProfile,            // tight and side-on, their face in profile
		kCloseLow,                // tight, below the eyeline, looking up
		kCloseHigh,               // tight, above the eyeline, looking down
		kCloseWide,               // loose single, shoulders and some room

		// Reserved for a line the writer actually raised — see the emotion scan.
		// Tighter than kCloseUp, and the only setup that crops below the chin.
		kExtremeClose,

		// A close single with a sliver of the listener still in frame. "Dirty" in
		// the trade, because the shot is not clean of the other person. It is the
		// workhorse of screen dialogue and the tree had no equivalent: kCloseUp is
		// clean, and the over-the-shoulders sit much wider.
		kDirtyNpc,

		// Between an over-the-shoulder and a profile. The single most common angle
		// in filmed conversation and the most obvious gap in the original set.
		kThreeQuarterNpc,

		// Side-on variants at sizes the close range did not cover.
		kMediumProfile,
		kLowProfile,

		// Steeply above, looking down. Distinct from kCloseHigh, which is a tilt;
		// this reads as a vantage point.
		kOverhead,

		// Height and size variants on the over-the-shoulder.
		//
		// The set had exactly one OTS per side, which is a strange gap given it is
		// the most used setup in filmed dialogue — every other family here has
		// three or four sizes. A conversation cut only between one OTS and a pile
		// of clean singles reads as though the mod does not really own the angle.
		kOverPlayerShoulderLow,   // ducked under the shoulder, looking up
		kOverPlayerShoulderHigh,  // raised above it, looking down
		kOverPlayerShoulderWide,  // loose, both bodies and the room around them

		// Player coverage — used while it is your turn.
		//
		// This side of the exchange had three setups against the NPC's nine, which
		// is why the rotation felt thin: every reverse shot came back to one of the
		// same three. The additions below mirror the NPC vocabulary so a reverse is
		// as varied as the shot it answers.
		kOverNpcShoulder,
		kMediumPlayer,
		kHighAngle,
		kClosePlayer,

		// The player's counterpart to kExtremeClose, and gated the same way: only
		// offered on an intensity-100 line, which the emotion scan puts at 5-8% of
		// dialogue. Added 2026-08-13; the player's side had every other member of
		// the close range and not this one.
		kExtremeClosePlayer,

		kDirtyPlayer,
		kPlayerProfile,
		kPlayerLow,
		kThreeQuarterPlayer,

		// The reverse side, brought level with the NPC's.
		//
		// The NPC had a full figure and an overhead and the player had neither, so
		// the two halves of the exchange could not be cut against each other at
		// matching sizes — the reverse was always tighter than the shot it
		// answered.
		kOverNpcShoulderLow,
		kOverNpcShoulderHigh,
		kOverNpcShoulderWide,
		kLongPlayer,
		kPlayerOverhead,

		// Neutral.
		//
		// The two-shot's height and width variants used to sit here — low, high and
		// wide. They are gone rather than switched off: a two-shot is a shot of two
		// people standing apart, and tilting or widening it only ever adds more
		// space between them. There is no room in which those read as anything but
		// a worse version of the plain two-shot below.
		kTwoShot,
		kProfile,
		kWide,
		kDistant,

		// The room, at sizes and heights the neutrals above did not reach.
		//
		// kMaster is the whole space from up and back — the shot a scene opens on
		// in a film and the widest setup here. kGroundLevel is its opposite, down
		// near the floor looking up at two people standing over the lens.
		kMaster,
		kGroundLevel,
		kDistantLow,

		// The anchored pair — tucked against found geometry, a wall or a pillar —
		// used to live here. Removed, and the surface-probing anchor with them.
		//
		// The idea was that shooting from against something reads as a camera a
		// person placed. In play it did not: the probe finds a surface, but a
		// surface at the right distance is usually just a wall in a direction the
		// room shots could already have used, so the result was another wide from
		// another corner and nothing about it said "against" anything. The long
		// lens does the same job with a reason you can see in the image, which is
		// the compression. Recoverable from history if that changes.

		kCount
	};

	// How the camera's POSITION is derived.
	//
	// The solver used to have exactly one rule — orbit the subject's head at a
	// radius derived from how big they should look — and that single rule is why
	// forty-odd setups read as the same shot at different distances. Every angle
	// was a point on one cylinder around one head.
	enum class Anchor : std::uint8_t
	{
		kSubject,   // orbit the person the shot is about
		kMidpoint,  // orbit the point between the two of them
		kScene,     // placed against the room, hooked to nobody
	};

	// What the camera POINTS at, which is a separate question from where it
	// stands. Standing across the room and aiming at a face is a different shot
	// from standing in the same spot and aiming at the space — and until these two
	// were split, no shot in the mod could do the second one.
	enum class Aim : std::uint8_t
	{
		kSubject,
		kMidpoint,
		kScene,  // the space itself; the participants sit off-centre by composition
	};

	// What the camera DOES across the life of the shot.
	//
	// Every shot used to share one global push-in, which is worse than no movement
	// at all: identical motion on every setup is another thing making them all
	// feel the same. Note kPushIn and kZoomIn are genuinely different images —
	// a dolly changes perspective, a zoom does not.
	// WHAT A SETUP DOES WHILE IT IS ON SCREEN.
	//
	// No longer a property of the shot table. The table's choice is now only the
	// DEFAULT — every setup's move is a setting the player picks, and the ini
	// remembers it. Presets set them the same way a person would.
	//
	// Ordering is the ini's storage format, so entries are only ever APPENDED. A
	// value inserted in the middle silently rewrites everybody's saved choices
	// into whatever now sits at that index.
	enum class Move : std::uint8_t
	{
		kLocked,     // on sticks; nothing moves. Makes the moving shots read.
		kPushIn,
		kPullOut,
		kCraneUp,    // the camera physically rises
		kCraneDown,
		kTiltUp,     // the aim rises; the camera stays put
		kTiltDown,
		kDrift,      // a slow lateral arc around the anchor
		kZoomIn,     // the lens tightens; perspective does not change
		kZoomOut,

		// THE TWO WAYS TO GO SIDEWAYS, and they are different images.
		//
		// An ORBIT arcs around the subject and keeps pointing at them, so they stay
		// exactly where they are in frame and the background slides behind them.
		// It is the shot that makes a room feel three-dimensional.
		//
		// A TRUCK slides the camera sideways without turning to follow, so the
		// subject drifts across the frame and out of it. No anchor at all, which is
		// what makes it feel like a camera on rails rather than one aimed at
		// somebody.
		//
		// kDrift is the old name for an orbit and is kept for saved configs. It has
		// no direction of its own; these do.
		kOrbitLeft,
		kOrbitRight,
		kTruckLeft,
		kTruckRight,

		kCount
	};

	// For the menu and the log. One short phrase, no enum ordinals.
	[[nodiscard]] std::string_view MoveLabel(Move a_move) noexcept;

	[[nodiscard]] std::string_view Name(ShotType a_type) noexcept;

	[[nodiscard]] bool             FavoursNpc(ShotType a_type) noexcept;

	// What this shot does, for the log and the menu. Named rather than numeric
	// because "crane-up" in a cut line is worth more than an enum ordinal.
	// Who the shot is of — "them", "you" or "room" — in one word.
	//
	// Name() is written for a menu whose headings already say which side of the
	// exchange a group belongs to, so the names themselves do not repeat it. The
	// log has no headings, and without this a genuine cut between two different
	// angles could read as "Close-up -> Close-up".
	[[nodiscard]] std::string_view SubjectName(ShotType a_type) noexcept;

	[[nodiscard]] std::string_view MoveName(ShotType a_type) noexcept;
	[[nodiscard]] float            LensOf(ShotType a_type) noexcept;

	// How much of the frame height the subject fills. Higher is tighter, and a
	// tighter setup needs less room — which is why the fallback search orders its
	// candidates by this: in a cramped interior the tight ones are the ones that
	// will still place.
	[[nodiscard]] float FillOf(ShotType a_type) noexcept;

	// The ini key this shot is enabled and disabled under, in [Shots].
	//
	// Separate from Name() on purpose. Name() is display text and appears in the
	// log, where it is free to be renamed for clarity; a key is a contract with
	// the player's settings file and must never change once shipped. Returned as
	// a plain pointer rather than a view because it goes straight to the profile
	// API, which needs a terminator.
	[[nodiscard]] const char* Key(ShotType a_type) noexcept;

	// The ini key carrying this setup's weight. Distinct from Key(), which is the
	// on/off, so a load order tuned before weights existed keeps its choices.
	[[nodiscard]] const char* WeightKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* LensKey(ShotType a_type) noexcept;

	// iNameMove / iNameMoveAmount / iNameMoveTime. ZoomKey is kept for one job
	// only: reading a pre-1.3 config once so an existing zoom choice survives.
	[[nodiscard]] const char* MoveKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* MoveAmountKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* MoveTimeKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* ZoomKey(ShotType a_type) noexcept;

	// sNameLight — the lighting look this setup is shot under, by NAME.
	//
	// The only per-setup key in the mod that is not a number, and deliberately so:
	// a look is chosen from a table that may be extended, and an ordinal in a
	// settings file would point at whatever later occupies that slot. Prefixed s
	// rather than i so the ini does not describe a string as an integer.
	//
	// ONLY CONSULTED WHEN [Lighting] bPerShot IS ON. Off — which is how it ships —
	// every angle uses the one look the player picked, and none of these are read.
	[[nodiscard]] const char* LightKey(ShotType a_type) noexcept;

	// iNameLightX / Y / Z — this setup's own nudge, in camera space, on top of the
	// global one. Same axes as Scene::KeyLight::SetOffset: across the frame,
	// toward or past the subject, and down or up.
	[[nodiscard]] const char* LightXKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* LightYKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* LightZKey(ShotType a_type) noexcept;

	// The look this setup ships under, as a Scene::LookSpec key. Resolved to an
	// index by whoever is doing the reading — returned as a name so this header
	// stays free of a dependency on the lighting model, which is a Scene concern
	// and has no business being visible to the camera solver.
	[[nodiscard]] const char* AuthoredLight(ShotType a_type) noexcept;

	// The ordinary tier. An accent setup ships here; a staple ships at twice it.
	inline constexpr int kDefaultWeight = 50;

	// What this setup's weight ships at, and the reason the slider can now be
	// believed.
	//
	// The pools used to carry their own baseline by LISTING A SHOT TWICE, and the
	// picker multiplied the player's weight by that repetition. So the number on
	// the slider was never the number that competed: a staple set to 34 beat an
	// accent set to 50, because the staple was silently doubled. Nothing said so,
	// and there is no reading of "how often it is drawn" under which that is true.
	//
	// The repetition moved here instead. Every pool now lists every setup exactly
	// once, the weight is used exactly as it reads, and the old baseline survives
	// as the DEFAULT: the eight staples ship at 100 and everything else at 50,
	// which is the same 2:1 the duplicate entries expressed. Leave every slider
	// alone and the camera behaves as it always did; move one and it does exactly
	// what the number says.
	//
	// 100 and 50 rather than 2 and 1 because the slider is 0-100 and the ratio is
	// what matters, not the units.
	[[nodiscard]] int AuthoredWeight(ShotType a_type) noexcept;

	// The field-of-view range every setup is tunable across.
	//
	// The maths degenerates outside it rather than these being taste: below about
	// twenty the tangent throws the standoff past anything an interior can hold,
	// and above a hundred and fifty the frame is a fisheye with no usable subject
	// size. DistanceForFill clamps to the same pair, and the two MUST agree — if
	// they diverge the standoff is solved for one field of view and the frame
	// rendered at another.
	inline constexpr int kMinLens = 20;
	inline constexpr int kMaxLens = 150;

	// What the table authored for this setup, ignoring any tuning. The menu's
	// reset needs it and the number is worth being able to get back to.
	[[nodiscard]] float AuthoredLens(ShotType a_type) noexcept;

	// Shots that belong to neither party — they frame the space, or both people in
	// it. They are always valid coverage, because there is no wrong subject to be
	// on, which is what lets the camera sit on the room while the player reads a
	// topic list without being dragged back.
	[[nodiscard]] bool IsNeutral(ShotType a_type) noexcept;

	// Whether this setup stands behind the other party and puts them in the
	// corner of frame.
	//
	// Exposed for one reason: an over-the-shoulder needs a shoulder, and the
	// shoulder belongs to whoever is NOT the subject. Framing a dragon over the
	// player's shoulder is fine; framing the player over a dragon's is not a shot.
	[[nodiscard]] bool OverShoulder(ShotType a_type) noexcept;

	// "Nothing is being held here; search, and report what you find."
	//
	// A real sentinel rather than 0, because 0 is a legitimate value for both
	// fields that use it: it is the centre of the angle sweep and it is what a
	// standoff reads as before the first frame of a shot has solved.
	inline constexpr float kUnheld = -1.0e9f;

	// "Measured, and nothing was in the way as far as the shot asked."
	//
	// A held room has to be able to say that, and a distance cannot. RoomAlong
	// probes out to the distance the shot wants and no further, so an unobstructed
	// bearing comes back reporting exactly that distance — which is a measurement
	// of the probe, not of the room. Remembering it as a number and capping the
	// rest of the shot with it would stop every pull-out dead at the distance the
	// cut happened to ask for, which is a move the player authored being silently
	// deleted by a wall that is not there.
	//
	// So a clear bearing is remembered as this instead, and a held frame treats it
	// the way RoomAlong treats a clear probe: the shot gets what it asks for.
	inline constexpr float kOpenRoom = 1.0e9f;

	struct Pose
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 lookAt{};
		bool         valid{ false };

		// Visibility is independent of placement. A held pose can still be
		// placeable while a moving body covers its subject, and the director needs
		// that result to apply persistence without silently moving the camera.
		SubjectSight visibility{};
		SightState   lensClearance{ SightState::kUnknown };

		// What this solve settled on, so the director can hold it for the life of
		// the shot. kUnheld means this path does not participate — the legacy
		// An invalid solve leaves both alone, so a failed frame cannot
		// overwrite the composed shot's held angle with a compass bearing.
		float sweep{ kUnheld };
		float standoff{ kUnheld };

		// HOW MUCH ROOM THE CHOSEN BEARING HAD, so the cut can be the only frame
		// that measures it. kOpenRoom means nothing was in the way; see there.
		//
		// Reported on every frame but committed on one: the director keeps the
		// first answer for the life of the shot and never revises it. That
		// direction matters, because a held frame does not re-probe and so cannot
		// produce an honest number to revise it with.
		float room{ kUnheld };

		// HOW WELL THIS SHOT PLACED, 0..1. Zero on a refusal.
		//
		// The reason this exists: `valid` was the only thing the picker ever asked,
		// so an angle shoved against a wall at the distance floor, swung eighty
		// degrees off its own bearing, with a market stall across half the frame,
		// was accepted on exactly the same terms as one standing in open space at
		// the size it asked for. The mod already worked all of that out and threw
		// it away one line later.
		//
		// Three things go in, and they are the three ways a placement is a
		// compromise rather than the shot: it had to come CLOSER than the framing
		// wanted, it had to STEP AWAY from its own angle, or the view down it is
		// partly BLOCKED. See Score() in Shot.cpp for the weighting and why.
		float quality{ 0.0f };

		// Horizontal field of view this shot wants, in degrees. Zero means the
		// shot has no opinion and the player's own value should stand.
		//
		// The lens is a shot property, not a global. Two setups at the SAME
		// subject size and the same angle are different images on a 45 and on a
		// 95 — one compresses and flatters, the other distends and involves. It
		// is the strongest single axis of difference available here, and without
		// it every shot in the mod was on the same prime.
		float lens{ 0.0f };
	};

	struct Subjects
	{
		RE::NiPoint3 playerHead{};
		RE::NiPoint3 npcHead{};
		float        fovDegrees{ 75.0f };
		float        aspect{ 1.78f };

		// Which side of the eyeline the camera may stand on — the 180-degree rule.
		float side{ 1.0f };

		// 0..1 through the current shot, driving the push-in.
		float progress{ 0.0f };

		RE::NiPoint3 openDirection{ 1.0f, 0.0f, 0.0f };
		float        openDistance{ 400.0f };

		// Seconds since the last frame. Only the standoff limiter uses it.
		float delta{ 0.0f };

		// HOW MUCH CLEAR SPACE IS ABOVE THE CONVERSATION, in world units over the
		// head anchors. Measured once when the conversation stages.
		//
		// Nothing in the mod ever looked up. `rise` is the one column in the shot
		// table in absolute world units, several setups lift the camera a long way
		// on it, and the only thing that kept an overhead indoors was
		// Anatomy::Tuning::riseScale — a hand-tuned constant added because a
		// 62-unit lift became 220 on a dragon and went through a ceiling. That is a
		// constant standing in for a measurement, and it is wrong again the moment
		// the room is a different shape.
		//
		// A large value means open sky and no clamp. Zero means the probe found
		// nothing and every rise stands as authored, which is the old behaviour.
		float ceiling{ 0.0f };

		// The two participants, so the crowd test can tell them from bystanders.
		// Nobody else is skipped: a follower standing at your elbow is exactly the
		// body most often in shot.
		RE::FormID npcId{ 0 };
		RE::FormID playerId{ 0 };

		// Whether the camera is held to one side of the eyeline. See kLineFloor.
		bool enforceLine{ true };

		// Keep both subjects' shots on one side of the eyeline.
		// See ShotAngles::LineSide.
		bool true180{ false };

		// Whether a body across the sightline costs a shot points. Off, the crowd
		// probe is not run at all, which is also the cheaper path.
		bool avoidCrowds{ true };

		// Opt-in subject protection replaces the old full-width clearance score.
		// The actor snapshot may be shared across all candidates in a decision.
		// Animated sight targets are separate from the stable composition anchors.
		bool                protectSubject{ false };
		bool                requireFullFace{ false };  // returning from first person
		bool                checkVisibility{ true };
		const SightContext* sightContext{ nullptr };
		SightTarget         npcSight{};
		SightTarget         playerSight{};
		float               cropFractionPerEdge{ 0.0f };

		// WHO IS BEING FRAMED, MEASURED RATHER THAN ASSUMED.
		//
		// The shot table's fills, floors and probe distances were all written
		// against a standing human, and Solve used them as constants. A shot picks
		// its subject with spec.onNpc, so the numbers have to be picked the same
		// way — and the scene and midpoint setups, which frame both parties, take
		// the larger of the two or they compose a dragon as though it were the
		// person standing next to it.
		Anatomy npc{};
		Anatomy player{};

		// THE ANGLE THE SWEEP SETTLED ON, HELD FOR THE LIFE OF THE SHOT.
		//
		// Exactly the reasoning sceneSwing gives directly above, applied to the
		// thing that actually caused the reported swinging. Solve's angle sweep is a
		// discrete argmax over raycast clearance with no memory of last frame's
		// pick, so two candidates with near-equal room trade places frame to frame
		// and the camera jumps between them — 36 degrees at a time, since the sweep
		// steps 0, +/-18, +/-36, +/-58, +/-80. All it takes is a subject shifting a
		// few units so that +18 stops clipping a door frame as -18 starts.
		//
		// The angle is an editorial choice and belongs to the cut. Holding it is not
		// smoothing: the search still runs in full when the shot is chosen, and it
		// runs in full for every CANDIDATE, because Subjects is constructed fresh
		// each frame and only the render path fills these in.
		//
		// If the held angle later becomes genuinely blocked, RoomAlong drops below
		// the floor and Solve refuses — which the director already degrades well,
		// by holding the last good pose rather than by finding a new angle without
		// a cut.
		float heldSweep{ kUnheld };

		// Last frame's solved standoff, for the asymmetric rate limit. See
		// LimitStandoff in Shot.cpp for why it is asymmetric.
		float heldStandoff{ kUnheld };

		// THE ROOM THIS SHOT MEASURED WHEN IT CUT, AND WHETHER TO TRUST IT INSTEAD
		// OF MEASURING AGAIN.
		//
		// heldSweep already stops the camera CHOOSING a new angle mid-shot. It does
		// not stop it re-solving the distance along the one it kept: RoomAlong runs
		// every frame, so a pillar crossing the bundle or somebody walking behind
		// the lens pulls the camera in and the rate limiter walks it back out. That
		// is a correction nobody asked for to a shot that was already right, and it
		// is the whole of "the camera moves when someone walks past".
		//
		// With holdPlacement set, a shot that has already placed uses the room it
		// measured at the cut and casts nothing. The cut itself is unaffected —
		// holdPlacement is false on the first frame of every shot by construction,
		// so the full sweep, the wall margin and the refusal all still decide where
		// the camera may stand. What is given up is the correction afterwards,
		// which is exactly the trade the setting names.
		//
		// Two consequences worth being explicit about, because both are the point
		// rather than a defect. A held shot cannot be refused by geometry that
		// arrives after it, so it holds its frame through a passing cart. And a
		// subject who walks somewhere new takes the camera with them at a fixed
		// bearing and distance, through whatever is between — the shot follows the
		// people, not the room.
		//
		// Subject protection also uses this to freeze placement, but its explicit
		// visibility check remains independent and can request an obstruction cut.
		bool  holdPlacement{ false };
		float heldRoom{ kUnheld };
	};

	// Solves a camera pose.
	//
	// Distance is derived from how much of the frame the *subject* should occupy,
	// not from how far apart the two participants happen to be standing. That
	// distinction is the difference between a close-up and a shoulder shot with a
	// narrow field of view: the previous model solved every shot from the head
	// separation, so a "close-up" placed the camera behind the listener and ended
	// up further from the face than a medium.
	//
	// In subject-protection mode an unheld sweep admits only verified clear
	// poses. Held poses retain their placement and report visibility separately.
	[[nodiscard]] Pose Solve(ShotType a_type, const Subjects& a_subjects);

	// The angle sign Solve uses for this setup (+1 or -1). The key light needs it
	// to stay on the camera's side of the line.
	[[nodiscard]] float SideFor(ShotType a_type, const Subjects& a_subjects) noexcept;

	// Recheck the actual lens and required subjects without choosing a bearing
	// or changing placement/quality. Explicit checks always run, even when a
	// per-frame Solve skips them via checkVisibility or holds placement.
	void CheckVisibility(ShotType a_type, Pose& a_pose, const Subjects& a_subjects);

	// The one thing about composition that is worth exposing.
	//
	// Everything else here is geometry the player has no useful opinion about —
	// fill fractions and rise values are what make a close-up a close-up. The
	// move is different: it is the part of a shot that moves, so it is the part
	// anyone notices as a preference.
	class Shot
	{
	public:
		// WHAT USED TO BE HERE: Configure(dollyFraction), the global "how much of
		// each shot's own move actually happens" dial, and its partner iDollyWindow
		// for duration.
		//
		// One number moving forty-three shots together is the same mistake the
		// global lens shift was: it flattens the very thing it adjusts. Both are
		// per-setup now, alongside the move itself.

		// Which setups the picker is allowed to draw.
		//
		// Unknown shot IDs are disabled. Zero weight also excludes a setup
		// from selection, solving and cached-pose reuse.
		static void               SetEnabled(ShotType a_type, bool a_enabled) noexcept;
		[[nodiscard]] static bool Enabled(ShotType a_type) noexcept;

		// How often this setup is drawn relative to the others in its pool, 0-100.
		//
		// The pools weight themselves by repeating entries, which is coarse and
		// invisible: a shot appears four times or once and there is no way to ask
		// for three. This multiplies that baseline, so the pool composition still
		// carries the intent and the dial adjusts it.
		static void              SetWeight(ShotType a_type, int a_weight) noexcept;
		[[nodiscard]] static int Weight(ShotType a_type) noexcept;

		// Field of view for this setup alone, in degrees. A TRUE value in
		// kMinLens..kMaxLens, seeded from the table.
		//
		// There is no sentinel and no "as authored" state to read past. It used to
		// be an override where 0 meant "keep the table's choice", and because the
		// table's choice differs per setup — 88 on the wides, 50 on the portraits,
		// 40 on the extreme close — two setups both reading 0 were shooting 38
		// degrees apart. The number on screen is now the number in use.
		static void              SetLens(ShotType a_type, int a_degrees) noexcept;
		[[nodiscard]] static int Lens(ShotType a_type) noexcept;

		// WHAT THIS SETUP DOES, chosen by the player. Seeded from the table.
		//
		// Replaces SetZoom/Zoom, which was a three-state override (authored, zoom
		// in, zoom out) bolted onto a move the table owned. There is no override
		// and no "as authored" sentinel any more, for the same reason the lens
		// stopped having one: a control whose value you cannot read off the screen
		// is a control nobody trusts.
		//
		// A saved iNameZoom of 1 or 2 migrates to kZoomIn/kZoomOut on first load.
		static void               SetMove(ShotType a_type, Move a_move) noexcept;
		[[nodiscard]] static Move MoveOf(ShotType a_type) noexcept;

		// HOW MUCH OF THAT MOVE HAPPENS, 0-100, and it means the same thing for
		// every move.
		//
		// The table's own amounts were in whatever unit the move needed — a
		// fraction of the standoff for a push, world units for a crane, degrees for
		// a drift — which is fine for a table nobody reads and useless on a slider.
		// 100 is the full travel of whichever move is selected; see kFullScale.
		//
		// That normalisation is also what makes the move switchable at all: a
		// locked setup carries an amount of zero, so without it, choosing Orbit on
		// one would move the camera exactly nowhere and read as a broken control.
		static void              SetMoveAmount(ShotType a_type, int a_strength) noexcept;
		[[nodiscard]] static int MoveAmount(ShotType a_type) noexcept;

		// How long the move takes to complete, in hundredths of a second.
		// Per setup, replacing the global iDollyWindow.
		static void              SetMoveTime(ShotType a_type, int a_hundredths) noexcept;
		[[nodiscard]] static int MoveTime(ShotType a_type) noexcept;

		// WHICH LOOK THIS SETUP RUNS, as an index into Scene::AllLooks.
		//
		// An index here and a name in the file. -1 means nothing has resolved it
		// yet, which is distinct from look 0 — look 0 is a real look that happens
		// to be Off, and a setup that was never read must fall back to what it
		// ships as rather than to darkness.
		static void              SetLight(ShotType a_type, int a_look) noexcept;
		[[nodiscard]] static int LightOf(ShotType a_type) noexcept;

		// This setup's own light nudge, in camera space. Added to the global one
		// rather than replacing it, so the global dials stay a master adjustment
		// and an angle only has to say how it differs.
		static void              SetLightOffset(ShotType a_type, int a_x, int a_y, int a_z) noexcept;
		[[nodiscard]] static int LightOffsetX(ShotType a_type) noexcept;
		[[nodiscard]] static int LightOffsetY(ShotType a_type) noexcept;
		[[nodiscard]] static int LightOffsetZ(ShotType a_type) noexcept;

		// What the table ships this setup at, for the per-shot Default button.
		[[nodiscard]] static Move AuthoredMove(ShotType a_type) noexcept;
		[[nodiscard]] static int  AuthoredMoveAmount(ShotType a_type) noexcept;
		[[nodiscard]] static int  AuthoredMoveTime(ShotType a_type) noexcept;
	};
}

#pragma once

namespace SD::Camera
{
	// What kind of thing is being framed.
	//
	// Not a taxonomy — the mod does not care whether something is a dragon or a
	// mammoth. It cares about the three questions that change how a shot is
	// composed: how big is it, how high is its head, and does it have a shoulder
	// to shoot over.
	enum class Build : std::uint8_t
	{
		kHumanoid,  // the assumption every constant in this mod was written under
		kBeast,     // quadrupeds and the like: a head, no usable shoulder
		kLarge,     // dragons, giants, mammoths
		kSmall      // chickens, rabbits, skeevers, mudcrabs
	};

	[[nodiscard]] std::string_view Name(Build a_build) noexcept;

	// Per-build framing adjustments, applied on top of whatever the shot asked
	// for. This is where "a dragon is not a person wearing a dragon costume"
	// lives: scaling the numbers gets the camera to the right DISTANCE, and these
	// get it to the right PLACE.
	struct BuildTuning
	{
		// The tightest a shot may frame this build.
		//
		// The measured failure: kExtremeClose asks for 0.85 of frame height, which
		// on a person is a face cropped below the chin and is the whole point of
		// the setup. A dragon's head is a long wedge on a neck, and 0.85 of it is
		// an eye socket — or, at scale 1.00 as first shipped, the underside of the
		// jaw. Big heads want the whole head.
		float fillCap;

		// How much of the scaled rise column to actually apply.
		//
		// rise is in world units and now scales with the subject, but it should not
		// scale all the way: a 62-unit overhead on a person becomes a couple of
		// hundred units on a dragon, which is above the roof of most of the places
		// you meet one. Large builds also simply read better from at or below the
		// head — looking DOWN on a dragon throws away the only thing that makes it
		// worth pointing a camera at.
		float riseScale;

		// Degrees added to the shot's angle off the eyeline.
		//
		// A long snout shot dead-on is a nostril with an animal behind it. Stepping
		// further round puts the length of the head across frame, which is the
		// three-quarter view every dragon that has ever been drawn well uses.
		float angleBias;
	};

	[[nodiscard]] BuildTuning Tuning(Build a_build) noexcept;

	// Everything about a subject that the framing constants used to assume.
	//
	// The whole shot table is written against a standing human: a 42-unit
	// head-and-shoulders, an eye 120 units up, a 68-unit floor before the lens is
	// inside a pauldron, and a probe that starts 48 units out to clear the actor's
	// own collision. Point any of that at a dragon and every number is wrong in
	// the same direction — most visibly the probe, which starts INSIDE the animal
	// and reports no room in any direction, so every composed shot fails and the
	// emergency single runs the whole conversation.
	//
	// These are those constants, measured per subject instead of assumed.
	struct Anatomy
	{
		// Above the actor's root translate. Sampled from the head node once and
		// then held, exactly as the humanoid path always did — the root does not
		// animate and the head node does.
		float eyeHeight{ 120.0f };

		// WHERE THE HEAD IS, NOT JUST HOW HIGH IT IS.
		//
		// The humanoid path anchored at the root's X and Y with the head's Z, which
		// is exact for anything that stands upright: the head is directly above the
		// feet. On a dragon the root is at the hips and the head is several hundred
		// units FORWARD of it, so root-XY-plus-head-Z lands in the middle of the
		// animal — which is precisely the "close-up of its crotch" this produced.
		//
		// Stored in the ACTOR'S OWN FRAME, rotated out by their heading, so it can
		// be re-applied to the root each frame as they turn. Captured once, like
		// the height, so the anchor still does not inherit the head bone's
		// breathing, sway and gesture — which is the whole reason the root is the
		// base in the first place.
		RE::NiPoint3 headOffset{};

		// What a fill fraction is measured against: roughly the head and shoulders
		// of a human, scaled. This is the number that makes a close-up a close-up,
		// and it is why an unscaled close-up on a dragon frames a nostril.
		float extent{ 42.0f };

		// The hard floor on standoff, scaled. Never violated.
		float minDistance{ 68.0f };

		// Where a clearance ray starts, measured out from the subject. Must clear
		// the subject's OWN collision or the ray hits them immediately and reports
		// a wall in every direction.
		float probeStart{ 48.0f };

		// The FRAMING factor, relative to a standing human. 1.0 is a person.
		//
		// Deliberately not the bulk ratio. Head size grows far slower than body
		// bulk — a dragon is roughly nine times a person's bounding sphere and its
		// head is nowhere near nine times a person's head — so applying the bulk
		// ratio to the framing numbers puts the camera out past the wings. This is
		// bulk raised to a fractional power, calibrated on the one dragon that has
		// been measured; see kFrameExponent.
		float scale{ 1.0f };

		// MEASURED IN GAME: ALWAYS ZERO. Kept, logged, and used for nothing.
		//
		// Scaling off the head node's own bound was the obvious idea and it cannot
		// work: a head node is a BONE, and bones carry no geometry. The head mesh
		// is a skinned shape parented to the body, so the bone's world bound is
		// empty. Every actor measured r0.0 — dragons and people alike — which meant
		// every subject fell back to scale 1.0 and a dragon was framed with a
		// person's numbers. Left in place so nobody spends another evening on it.
		float headRadius{ 0.0f };

		// The whole-body bound, and the signal that actually works. Measured 2026-08-15:
		// people 70-106, a dragon 679.
		float radius{ 0.0f };

		// The body-bound ratio, deadbanded so ordinary human variation reads as 1.0.
		// Drives classification only.
		float bulk{ 1.0f };

		Build build{ Build::kHumanoid };

		// Whether there is a shoulder to shoot over. Set by Fit, because it needs
		// the size as well as the rig — a giant has arms and is still the wrong
		// thing to stand behind.
		bool shoulder{ true };

		// Raw findings from Measure, which Fit reads.
		bool rigged{ true };  // the skeleton has an upper-arm node
		bool head{ true };    // a head node was found at all

		// False when the actor had no 3D to measure. Everything above is then the
		// humanoid default, which is the right guess and should still be logged.
		bool measured{ false };

		// For the log only: which name the head was found under, and whose body
		// this is. A creature that frames oddly is then one line to diagnose.
		std::string_view via{ "default"sv };
		std::string_view name{ "?"sv };
	};

	// Measures an actor: eye height, bound radius, whether the rig has an arm.
	// Cheap enough for a conversation open and a posture change, which is the only
	// place it is called — it walks the skeleton.
	//
	// The derived numbers (scale, extent, floor, probe) are NOT filled in here.
	// See Fit.
	[[nodiscard]] Anatomy Measure(RE::Actor* a_actor);

	// Turns a raw measurement into framing numbers, against a reference radius.
	//
	// SPLIT FROM Measure SO THE PLAYER CAN BE THE REFERENCE, which removes the
	// only guess in this file from the path that matters. "How many world units is
	// a person" is not written down anywhere and was going to be a constant here;
	// get that constant wrong by a third and every ordinary conversation in the
	// game is reframed, which is a far worse failure than a badly framed dragon.
	//
	// The player is a known humanoid standing in front of us in every single
	// conversation, so they are the ruler: fit them against their own radius and
	// they come out at exactly 1.0 whatever the absolute numbers turn out to be,
	// and the NPC is measured against them. Pass a reference of 0 to fall back to
	// the built-in estimate, which is only reached when the player has no 3D.
	void Fit(Anatomy& a_body, float a_referenceRadius);
}

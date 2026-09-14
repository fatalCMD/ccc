#pragma once

#include "SD/Camera/SightGeometry.h"

#include <vector>

namespace SD::Camera
{
	struct SubjectSight
	{
		SightState state{ SightState::kUnknown };
		float face{ 0.0f };
		float torso{ 0.0f };
		bool severe{ false };  // no face samples readable, or center outside the frame
	};

	struct SightTarget
	{
		RE::NiPoint3 head{};
		float scale{ 1.0f };
		RE::FormID id{ 0 };
		bool valid{ false };
	};

	struct SightCapsule
	{
		RE::NiPoint3 from{};
		RE::NiPoint3 to{};
		float radius{ 0.0f };
	};

	struct SightActor
	{
		RE::FormID id{ 0 };
		std::array<SightCapsule, 2> capsules{};
		std::size_t count{ 0 };
		RE::NiPoint3 boundCenter{};
		float boundRadius{ 0.0f };
	};

	// A short-lived main-thread snapshot. Build once for a candidate search or
	// monitor update; do not retain it across frames or cell changes.
	struct SightContext
	{
		RE::bhkWorld* world{ nullptr };
		std::vector<SightActor> actors;
		bool valid{ false };
	};

	[[nodiscard]] SightContext BuildSightContext();
	[[nodiscard]] SightTarget MeasureSightTarget(RE::Actor* a_actor,
		const RE::NiPoint3& a_fallbackHead, float a_scale);

	// Five face rays converge at the actual lens. Four clear samples including
	// the center admit a shot; two upper-chest samples affect preference only.
	// lens is horizontal FOV in degrees; crop is the fraction removed PER edge.
	[[nodiscard]] SubjectSight SubjectVisibility(const RE::NiPoint3& a_camera,
		const RE::NiPoint3& a_lookAt, float a_lens, float a_aspect, float a_crop,
		const SightTarget& a_target, const SightContext& a_context);

	// Independent small local lens volume, including actor bodies. Unlike the
	// legacy placement bundle, foreground away from the lens is not tested.
	[[nodiscard]] SightState LensClearance(const RE::NiPoint3& a_camera,
		const SightContext& a_context);

	struct Probe
	{
		bool  valid{ false };     // false when no physics world was reachable
		bool  hit{ false };
		float distance{ 0.0f };   // world units to the hit, or the full ray length
	};

	// Casts a sightline ray between two world points.
	[[nodiscard]] Probe Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);

	// Is the view between these two points unobstructed?
	//
	// Load orders commonly run No Camera Collision, which stops the engine pushing
	// the camera out of geometry. That is convenient for an absolute-pose camera —
	// nothing fights the pose — but it means nothing catches a shot placed inside a
	// wall either. Occlusion has to be checked here or not at all.
	//
	// CAST FROM THE SUBJECT TOWARD THE CAMERA, never the other way. A ray that
	// begins inside a wall reports no hit, so a camera buried in masonry passes an
	// inward test cleanly and the check silently inverts.
	[[nodiscard]] bool Clear(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);

	// How much open space extends from a point along a direction, up to a_max.
	// Used to find which side of a conversation the room is actually on.
	[[nodiscard]] float Clearance(const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, float a_max);

	// THREE RAYS ACROSS THE WIDTH OF THE SHOT, NOT ONE DOWN THE MIDDLE.
	//
	// A camera does not see a line, it sees a cone, and a single ray is a knife
	// edge: a railing, a chair back or a passing NPC's drawn weapon flips it fully
	// on and off rather than degrading. That flicker is what the standoff rate
	// limiter in Shot.cpp was built to smooth over, which treats the symptom.
	//
	// The bundle is a truncated cone. It is wide at the SUBJECT end — roughly their
	// own silhouette, so anything intruding there is intruding into frame — and
	// narrow at the CAMERA end, where the width is the camera's own body. One
	// volume therefore answers both questions that matter: is the shot's foreground
	// clear, and is there room to stand.
	//
	// Horizontal only, and deliberately. Pillars, railings, doorframes, market
	// stalls and people are all horizontal intrusions; the vertical is already
	// covered by the ray's own rise interpolation at one end and the ceiling clamp
	// at the other. Five rays cost two-thirds more for the axis that almost never
	// decides anything.
	struct Bundle
	{
		bool  valid{ false };
		float distance{ 0.0f };  // to the NEAREST hit across the bundle, or the full length
		float clear{ 1.0f };     // 0..1, the fraction of rays that reached the far end
	};

	[[nodiscard]] Bundle CastBundle(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		float a_spreadNear, float a_spreadFar);

	// How much of this sightline other people are standing in. 0 is nobody.
	//
	// The Havok probes above run on the line-of-sight layer, which excludes actors
	// by design — a ray that starts at somebody's head would otherwise hit that
	// head immediately and report a wall in every direction. The side effect is
	// that a third NPC between the camera and the speaker does not exist, and
	// taverns, markets and jarls' courts are exactly where conversations happen.
	//
	// Answered geometrically rather than with a raycast: a horizontal distance from
	// the segment, against the actor's own bound. No physics call, so this is cheap
	// enough to ask once per candidate.
	//
	// Returns a penalty rather than a veto. A body three hundred units out
	// clipping the edge of frame is a dirty foreground, which is sometimes the
	// better shot; it should lose points, not be forbidden.
	[[nodiscard]] float Crowding(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		RE::FormID a_ignoreA, RE::FormID a_ignoreB, float a_radius);
}

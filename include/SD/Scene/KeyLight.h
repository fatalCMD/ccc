#pragma once

#include "SD/Scene/LightRig.h"

namespace SD::Scene
{
	// THE LAMPS, PLACED.
	//
	// This was one point light on whoever the camera was looking at, and the
	// argument for it still holds: a bounded radius with zero ambient buys
	// separation without shipping a form. What it did not survive was contact with
	// a lit room. A single key darkens what it fails to reach, and in a tavern
	// with its own lamps it fails to reach nothing — so the face got brighter and
	// nothing behind it fell away. Separation is a rim's job. Hence three.
	//
	// The three are not a setting and are not exposed. See LightRig.h for why the
	// version that exposed them was rebuilt.
	class KeyLight
	{
	public:
		static void Engage();
		static void Release();

		// Everything the player sets, in one call, re-read when a conversation
		// opens.
		//
		// a_brightness scales every lamp. a_red/green/blue are the colour, 0-255
		// each, applied across the whole look — lamps no longer carry their own.
		//
		// COLOUR AND BRIGHTNESS ARE SEPARATE AND MUST STAY SEPARATE. The colour is
		// the lamp's diffuse, which saturates; how much light there is comes from
		// fade. Folding brightness into the colour would cap it at white and make
		// every bright light a pale one.
		//
		// a_shadows makes every lamp a shadow-caster. Off by default and ini-only:
		// it is the most expensive thing in this file by a wide margin, and it is
		// not a question worth putting to somebody who just wants the light on.
		static void Configure(bool a_enabled, int a_brightness, int a_red, int a_green, int a_blue,
			bool a_shadows, int a_fadeHundredths);

		// Which of the looks in AllLooks() is running.
		//
		// Called when a conversation opens, and again on a cut when the player has
		// asked for a different look per angle. Lamps are created and destroyed
		// here as the look requires them — a lamp at zero intensity is genuinely
		// absent, not merely dark, which is what lets Hard cost two lights instead
		// of three.
		static void SetLook(int a_look);

		// NUDGE THE WHOLE RIG, IN CAMERA SPACE.
		//
		// World axes would have been easier and would have been wrong: a light
		// pinned to world X swings around the subject as the conversation turns,
		// so a setting that looked right in one doorway is lighting the back of
		// somebody's head in the next.
		//
		// These are relative to the shot instead, which is the frame of reference
		// the player is actually looking at:
		//
		//   X  left and right ACROSS THE FRAME
		//   Y  toward the camera, or past the subject away from it
		//   Z  down and up
		//
		// So "move it left a bit" stays left a bit through a cut, a reverse, and a
		// conversation that walks across a room. World units; 0 is whatever the
		// look authored.
		static void SetOffset(int a_x, int a_y, int a_z);

		// WHICH SIDE OF THE EYELINE THE CAMERA IS STANDING ON.
		//
		// Looks author a key at a positive angle, which is one side of the face.
		// Left alone that side is fixed in the WORLD, so cutting to the reverse
		// walks the key across to the other cheek and the room appears to have
		// relit itself between two lines. Passing the shot's side flips the look
		// with the camera, which is what a crew does and the reason coverage cuts
		// together at all.
		static void SetSide(float a_side);

		// Placed relative to the camera and the subject, so the look keys the face
		// the shot is on rather than lighting the room. a_delta drives the
		// cross-fade between looks.
		static void Aim(const RE::NiPoint3& a_camera, const RE::NiPoint3& a_subject, float a_delta);

		[[nodiscard]] static bool Engaged() noexcept;

		// How many lamps are registered with the shadow scene right now. For the
		// menu's readout: a look that silently resolves to nothing is otherwise
		// indistinguishable from one that is working.
		[[nodiscard]] static int ActiveLamps() noexcept;
	};
}

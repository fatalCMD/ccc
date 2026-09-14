#pragma once

#include "SD/Camera/Shot.h"

namespace SD::Camera
{
	// Named ways of shooting a conversation.
	//
	// The previous attempt at this bundled cut timings and a subset of the shot
	// list, and every preset felt like every other one. It could not have felt
	// otherwise: at that point every shot in the mod was solved by one rule —
	// orbit a head at a radius derived from how big it should look — so choosing a
	// different SUBSET of those shots changed the rhythm of the coverage and never
	// the look of it. A preset can only be as different as the vocabulary it
	// selects from.
	//
	// So a preset now names three things the old one had no way to say: the glass
	// how much the camera moves (moveAmount), and which families of
	// setup are in play — where the families themselves now differ by lens, by
	// what they anchor to, and by what they do across a take.
	//
	// A preset is applied once, writes every key it covers to SD.ini, and then
	// stops existing. There is no mode to be in and nothing consults it
	// afterwards, so everything stays individually editable and changing something
	// a preset set cannot leave the mod in a half-state.
	struct Style
	{
		int minShotTime;
		int maxShotTime;
		int cutEveryMin;
		int cutEveryMax;

		// lensBias and dollyAmount are both gone, and for one reason: a single
		// number nudging forty-three setups together flattens the very thing it is
		// adjusting. Glass is a property of each setup now, and so is movement.
		//
		// A preset says what it wants through WHICH setups it draws from and what
		// it asks each of them to DO — see motion below. Choosing the long-lens
		// entries and locking them off is a stronger and more honest statement than
		// nudging every shot in the mod a few degrees.

		// cutOnLineEnd IS GONE from here with the setting itself. perLineAngleChange
		// replaces it, and it is not the same axis: the old flag said whether a
		// line ENDING was worth a cut, this one says whether the line COUNT above
		// is allowed to ask for one at all.
		bool perLineAngleChange;
		bool holdOnShortLines;
		bool timedCutsWhileSpeaking;
		bool timedCutsWhileChoosing;
	};

	// What a preset asks one setup to do while it is on screen.
	//
	// This is the half of a look that the shot list alone could never express.
	// Kinetic and Observed can draw from overlapping vocabularies and still be
	// nothing like each other, because one orbits and cranes through every take
	// and the other never moves at all — and until now a preset had no way to say
	// so. It described itself as "cranes, drifts and tilts" in its own blurb while
	// setting a single global number that scaled whatever the table happened to
	// have authored.
	struct Motion
	{
		ShotType shot;
		Move     move;
		int      amount;  // 0-100 of that move's own travel
		int      time;    // hundredths of a second
	};

	// THE GLASS, PER SETUP, PER LOOK — the axis a preset could not reach until
	// now.
	//
	// Every look inherited the table's lens for every shot it used, so two
	// presets drawing the same setup were shooting it identically no matter what
	// their blurbs claimed. That is the largest difference available between two
	// images of the same person at the same size: a face at 40 degrees is
	// compressed and lifted off its background, and the same face at 88 has the
	// camera close enough to lengthen a nose. From a Distance and Up Close now
	// share several setups and look nothing alike, which is the whole point.
	//
	// Kept as its own list rather than as two more fields on Motion, because the
	// two questions are independent: most setups here change glass without
	// changing what they do, and folding them together would mean restating a
	// look's baseline move on every entry that only wanted a different lens.
	//
	// THE CEILING THIS AUTHORING HAS TO RESPECT. Distance is solved for the
	// fill AT THE CHOSEN LENS and then floored at the subject's minimum, so a
	// tight fill on a wide lens asks to stand closer than the floor allows,
	// gets clamped, and renders LOOSER than the setup was authored for — the
	// exact trap kExtremeClose fell into on the widest glass. Roughly, the
	// vertical field of view times the fill must stay under 34 degrees; in
	// practice that is a lens ceiling near 66 degrees at 0.85 fill, 80 at 0.68,
	// and no constraint at all below about 0.46. Over-the-shoulders are exempt
	// — their standoff is forced past the other participant regardless.
	struct Lens
	{
		ShotType shot;
		int      degrees;  // a REAL horizontal field of view, kMinLens..kMaxLens
	};

	// HOW OFTEN EACH SETUP IS DRAWN, PER LOOK.
	//
	// The shot table's weights are authored for the whole thirty-nine-setup
	// vocabulary — eight staples at 100 and everything else at 50 — and that
	// ratio means nothing once a preset has cut the vocabulary down to twelve.
	// Two looks left it undercutting the thing they exist to do:
	//
	//   Show the Room shipped its master and its wide as accents and its
	//   close-up as a staple, so the preset named for the room spent most of
	//   every speech on faces, with the tightest angle in its list drawn more
	//   often than any wide in it.
	//
	//   From a Distance drew its long-lens close-up exactly as often as
	//   everything else, when a close single from across a room is punctuation
	//   in that look and not its habit.
	//
	// WEIGHT ALSO DECIDES WHETHER THE ROOM APPEARS AT ALL, which is what makes
	// this more than an ordering knob. Coverage() rolls the environmental pool's
	// total weight against the active coverage pool's total, so the share of a
	// conversation given to the space is the sum of what its setups are worth.
	// A preset that cannot set weights cannot say how much of itself is room —
	// the single loudest thing four of these five looks have to express.
	struct Weight
	{
		ShotType shot;
		int      weight;  // 0-100, as the slider reads it. 0 is never.
	};

	// HOW EACH SETUP IS LIT, PER PRESET.
	//
	// ONLY REACHES ANYTHING WHEN [Lighting] bPerShot IS ON. Off — which is how it
	// ships — every angle runs the one look the player chose, and these are
	// written to the ini but never read. That is deliberate rather than sloppy: a
	// preset should describe itself completely, so that switching per-angle
	// lighting on later reveals a coherent set rather than whatever the defaults
	// happened to be.
	//
	// A look KEY rather than an index, matching what the ini stores and for the
	// same reason — see Shot::LightKey. It also keeps this header from needing to
	// know how many looks there are.
	struct Light
	{
		ShotType    shot;
		const char* look;  // a Scene::LookSpec key
	};

	struct Preset
	{
		// The ini token. A contract once shipped, exactly like a shot key — a
		// rename silently stops an existing [Presets] sApply line from resolving.
		const char* key;

		const char* name;     // menu label
		const char* summary;  // one line, under the label
		const char* detail;   // what the camera will actually do

		Style                     style;
		std::span<const ShotType> shots;  // everything absent from this is switched off

		// What every setup in this preset does unless `motion` overrides it, and
		// how hard. A look's baseline: Observed locks everything off and names no
		// exceptions; Intimate pushes in on everything and names a few.
		Move baseMove;
		int  baseAmount;
		int  baseTime;

		// The exceptions, by name. Anything not listed takes the baseline above.
		std::span<const Motion> motion;

		// What each setup is shot on. Anything absent takes the table's own
		// lens, and applying the preset puts it back there — the same rule the
		// move follows, so a look is reproducible rather than dependent on
		// whatever the last one left behind.
		std::span<const Lens> lenses;

		// How often each setup comes up. Same fallback rule as the lens.
		std::span<const Weight> weights;

		// What each setup is lit with. Same fallback rule again: anything absent
		// takes what the setup ships under, so applying a look twice with a
		// different one in between gives the same result both times.
		std::span<const Light> lights;
	};

	[[nodiscard]] std::span<const Preset> AllPresets();
	[[nodiscard]] const Preset*           FindPreset(std::string_view a_key);

	// THE LOOK A CLEAN INSTALL COMES UP IN, AND THE SOURCE OF EVERY SHOT DEFAULT.
	//
	// Close, for 1.4. This is not decoration: the settings loader uses it as the
	// FALLBACK for every per-setup key, so a fresh profile with no SD_user.ini and
	// a shipped SD.ini that only lists the on/off flags still comes up with the
	// six Close setups at Close's own lenses, weights, moves and lighting.
	//
	// The alternative was to seed the shot table's own authored values and rely on
	// config/SD.ini spelling out sixty-odd keys to override them. That works right
	// up until one of those lines is missing or mistyped, at which point the
	// Presets page reports drift the player cannot see the cause of and no preset
	// shows as active at all. One authority, consulted by everybody.
	[[nodiscard]] const Preset& DefaultPreset();

	// Whether this preset switches this setup on. Exposed for the same reason:
	// the loader needs it and reimplementing the search would be a second answer
	// to a question that must only have one.
	[[nodiscard]] bool PresetUses(const Preset& a_preset, ShotType a_type);

	// What this preset asks this shot to do: its named exception if it has one,
	// otherwise the look's baseline. Exposed so the menu can describe a preset
	// without reimplementing the lookup and drifting out of step with it.
	[[nodiscard]] Motion PresetMotion(const Preset& a_preset, ShotType a_type);

	// What this preset shoots this setup on, in degrees. Its named choice if it
	// has one, otherwise what the table ships. Same contract as PresetMotion and
	// for the same reason: apply, drift and the menu must all ask one function.
	[[nodiscard]] int PresetLens(const Preset& a_preset, ShotType a_type);

	// How often this preset draws this setup. Same contract again.
	[[nodiscard]] int PresetWeight(const Preset& a_preset, ShotType a_type);

	// What this preset lights this setup with, as a rig key. Same contract again.
	// Never null.
	[[nodiscard]] const char* PresetLight(const Preset& a_preset, ShotType a_type);

	// How many settings differ between a preset and what is live right now.
	//
	// Derived, never stored, and that is the point. A preset is applied and then
	// stops existing, so "which one is selected" has no answer unless it is worked
	// out from the settings themselves. Storing the last one applied would be
	// easier and would start lying the moment anybody moved a slider.
	[[nodiscard]] int PresetDrift(const Preset& a_preset);

	// The preset the current settings match exactly, or nullptr for a custom set.
	[[nodiscard]] const Preset* ActivePreset();

	// Write every key the preset covers to SD.ini and push it live.
	//
	// Deliberately touches nothing outside its own remit. The letterbox height,
	// the gaze split and the diagnostic pose mode are not style — two are taste
	// and one is a debugging lever — so a preset leaves them exactly as it found
	// them.
	void ApplyPreset(const Preset& a_preset);

	// ---- Your own presets -------------------------------------------------
	//
	// Three slots the player fills from whatever is live. Deliberately a
	// different mechanism from the five built-in looks rather than an extension
	// of them: a built-in preset is a designed vocabulary — a shot list chosen to
	// go together and a move given to each one — and there is no honest way for a
	// Save button to produce one of those. What a Save button can do perfectly is
	// take a snapshot, and calling a snapshot by the same name as a designed look
	// would set an expectation neither can meet.
	//
	// So they are stored differently, listed separately, and coloured
	// differently, and the menu says which is which.
	inline constexpr int kCustomSlots = 3;

	struct CustomSlot
	{
		int         index{ 0 };  // 0-based
		std::string name;        // empty means the slot is free
		bool        used{ false };
	};

	[[nodiscard]] CustomSlot ReadCustomSlot(int a_index);

	// Which slot the live settings match exactly, or -1.
	//
	// Takes precedence over ActivePreset in the menu. A slot is something the
	// player deliberately saved and deliberately pressed; if their own look
	// happens to also equal a built-in one, theirs is the answer they want to see.
	[[nodiscard]] int ActiveCustomSlot();

	// Snapshot everything a preset covers into a slot: the cutting dials, and
	// every shot's on/off, move, amount, duration, lens and how-often.
	//
	// Tracks exactly what a preset covers, and has to. A slot that saved less
	// than a preset writes cannot reproduce its own look — the fields it skipped
	// come back holding whatever the last preset applied left behind. That is
	// why this list has grown twice, alongside the two things presets learned to
	// set. Anything added to ApplyPreset belongs here in the same commit.
	void SaveCustomSlot(int a_index, std::string_view a_name);

	void ApplyCustomSlot(int a_index);
	void RenameCustomSlot(int a_index, std::string_view a_name);
	void DeleteCustomSlot(int a_index);

	// Consume [Presets] sApply, if it names one.
	//
	// The in-game menu needs SKSE Menu Framework, and a load order without it gets
	// no menu at all; without this the whole feature would be unreachable for
	// those players. Clearing the key after applying is what makes it a one-shot
	// rather than a mode — leave it set and it would overwrite the player's own
	// edits on every launch.
	void ApplyPendingPreset();
}

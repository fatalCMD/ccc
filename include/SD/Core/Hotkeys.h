#pragma once

namespace SD::Core
{
	// Keys that do something while a conversation is on screen.
	//
	// THIS IS NOT THE DIALOGUE INPUT HANDLER, AND THE DISTINCTION MATTERS.
	//
	// HANDOFF.md section 2 records that a dialogue input handler was built here,
	// proven to work, and then deliberately removed, with the note "do not rebuild
	// this without the user asking for it by name". That handler CONSUMED presses:
	// its whole job was to stop a left click reaching the control map's Accept
	// binding, which commits the highlighted topic without consulting the cursor.
	//
	// Nothing here consumes anything. ProcessEvent always returns kContinue, reads
	// the button state and passes the event on untouched, so it cannot interact
	// with dialogue input at all — there is no code path from this file to the
	// topic list. Do not add one. If a future feature genuinely needs to swallow a
	// press, that is the removed handler being rebuilt and it needs asking for by
	// name, in its own file, not smuggled in behind a hotkey.
	//
	// Keyboard only, deliberately. The mouse buttons are the ones tangled up in the
	// topic list, and a gamepad has no free buttons during dialogue worth taking.
	class Hotkeys
	{
	public:
		// The two bindable actions. Both ship unassigned.
		//
		// WHAT USED TO BE HERE: a third, "give the camera back for this
		// conversation". Turning the mod off is a checkbox now — a key is the right
		// control for something you do mid-scene and undo a moment later, and
		// enabling a mod is neither.
		enum class Action : std::uint8_t
		{
			kNextAngle,  // cut now, ignoring the hold floor
			kFraming,    // cycle automatic / them / you / the room
			kCount
		};

		static void Install();

		// Re-read the bindings from the ini, after something else has written one.
		static void Refresh();

		// The scan code bound to this action, or 0 for unassigned.
		[[nodiscard]] static std::uint32_t Binding(Action a_action) noexcept;

		// Bind, or pass 0 to clear. Writes the ini and takes effect on the next
		// press — a rebind that needs a restart is the thing this project keeps
		// shipping by accident.
		static void SetBinding(Action a_action, std::uint32_t a_code);

		// ANY KEY, CAPTURED FROM THE GAME'S OWN INPUT RATHER THAN FROM THE PANEL.
		//
		// The obvious way to write a capture widget is to ask ImGui which key is
		// down. It does not work here: ImGui's key enum has no relationship to the
		// DirectX scan codes the game reports and the ini stores, so it needs a
		// translation table that is wrong in exactly the places nobody tests — dead
		// keys, international layouts, the numpad.
		//
		// This captures from the same event stream that will later fire the hotkey,
		// so whatever it records is by construction the code that will match. The
		// sink is registered unconditionally for this reason: capture has to work
		// when nothing is bound yet, which is always the first time.
		static void Arm(Action a_action);
		static void Cancel();

		// Offer a scan code to whatever is waiting for one. True if it was taken.
		//
		// TWO CALLERS, AND BOTH ARE NEEDED. The game's own input sink sees keys
		// during play; the settings framework intercepts input while its menu is
		// open and hands events to a callback of its own. A capture widget lives
		// inside that menu, so the game sink alone cannot be relied on to see the
		// press that binds a key — which would be a capture control that works
		// everywhere except the one place it is used.
		//
		// Idempotent: the first caller clears the armed flag, so a key that reaches
		// both routes is taken once.
		static bool OfferKey(std::uint32_t a_code);

		// Which action is waiting for a key, if any.
		[[nodiscard]] static bool Capturing() noexcept;
		[[nodiscard]] static bool CapturingFor(Action a_action) noexcept;

		// Called from the panel each frame while armed. Applies a captured key and
		// returns true on the frame it lands.
		//
		// Polled rather than applied inside the sink because SetBinding writes the
		// ini, and the input thread is not where a file write belongs.
		static bool PollCapture();

		// A readable name for a scan code: "F7", "Numpad 3", "Left Bracket".
		// Returns "Not assigned" for 0 and "Key 137" for anything unrecognised —
		// never an empty string, so a binding always shows as something.
		[[nodiscard]] static std::string_view KeyName(std::uint32_t a_code) noexcept;

		[[nodiscard]] static bool AnyBound() noexcept;
	};
}

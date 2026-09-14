#pragma once

namespace SD::Render
{
	// Black bars across the top and bottom of the frame while a conversation is
	// staged.
	//
	// This is the only part of Scene Director that draws anything. Everything else
	// moves a camera and reads dialogue state, which is why the mod could not
	// conflict on render state until now — so this layer is deliberately small,
	// backs up every piece of device state it touches, and disables itself
	// permanently on the first failure rather than risking the frame.
	class Letterbox
	{
	public:
		static void Install();
		static void Shutdown();

		// Bars ease toward this over roughly a third of a second.
		static void SetVisible(bool a_visible);

		// TAKE THE BARS OFF THE SCREEN THIS FRAME, WITHOUT THE EASE.
		//
		// SetVisible(false) is the right call at the end of a conversation, where a
		// third of a second of bars sliding away is the point. It is the wrong one
		// when a menu is about to draw over the frame: an inventory whose header
		// spends a third of a second under a black bar is a clipped header, which
		// is precisely the report this exists to answer.
		//
		// Safe from the game thread. It sets an atomic the draw reads; the next
		// SetVisible(true) clears it, so a resumed conversation eases back in
		// normally rather than snapping.
		static void Retract();

		// A MENU OWNS THE SCREEN, OR IT DOES NOT.
		//
		// The draw used to answer this for itself, with RE::UI::GameIsPaused(), and
		// that is a different and much broader question than the one it wanted.
		// numPausesGame counts the CONSOLE, and it counts any overlay a mod puts up
		// that happens to freeze time — including SKSE Menu Framework's own settings
		// panel, whose FreezeTimeOnMenu setting is shipped as true by more than one
		// mod that bundles it.
		//
		// So the bars were forced off screen for as long as the panel that
		// configures them was open. Every change applied live and none of it could
		// be seen, which is exactly how it was reported: "no matter what I set".
		//
		// MenuWatch already answers the narrower question, and answers it with the
		// overlay exemption the Console has always had. This is that answer, pushed
		// to the render thread, which cannot ask it any other way.
		static void SetScreenTaken(bool a_taken);

		// Height of each bar as a fraction of screen height. 0.115 is 2.35:1 on a
		// 16:9 frame. Read on the render thread, so this is safe to call from the
		// game thread at any time.
		static void SetBarFraction(float a_fraction);

		[[nodiscard]] static bool Installed() noexcept;
	};
}

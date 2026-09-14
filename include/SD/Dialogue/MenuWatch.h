#pragma once

namespace SD::Dialogue
{
	// An independent read on "is the player in dialogue".
	//
	// Session polls MenuTopicManager from inside the camera hook. This watches the
	// menu stack and is driven by the engine instead, which makes it the control
	// in the experiment: if a dialogue menu opens and Scene Director's frame count
	// has not moved since the previous menu event, the frame source is not
	// per-frame and the hook is wrong — a conclusion the first run's single
	// "first frame observed" line could not support either way.
	//
	// The two sources are also expected to genuinely disagree, and that disagreement
	// is worth recording: the menu closes while an NPC is still delivering a
	// farewell, and a forcegreet opens a conversation with no menu at all.
	class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static void Register();

		// A menu that pauses the game is open, so the screen is not this mod's.
		//
		// THE ONLY COPY OF THIS ANSWER. It is kept here rather than in the director
		// because here is the one place still running while the game is paused —
		// the director's tick is not, which is the whole reason this exists.
		//
		// Runtime asks before opening a conversation. Without that a scene staged
		// on the frame a pausing menu was taking the screen would freeze staged and
		// hold the topic list hidden for as long as the menu was up.
		[[nodiscard]] static bool ScreenTaken() noexcept;

		// Drop the record if the engine says nothing is pausing the game.
		//
		// Called from the director's tick, which by definition only runs while the
		// game is running. A record built from paired events can be left holding a
		// menu whose close was never delivered — a load, a mod force-closing a menu
		// — and that would be permanent: no conversation would stage again.
		// numPausesGame is the authority, costs one read, and is asked rather than
		// trusted to agree.
		static void Reconcile();

		MenuWatch(const MenuWatch&) = delete;
		MenuWatch(MenuWatch&&) = delete;
		MenuWatch& operator=(const MenuWatch&) = delete;
		MenuWatch& operator=(MenuWatch&&) = delete;

	protected:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent*               a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source) override;

	private:
		MenuWatch() = default;
		~MenuWatch() override = default;

		[[nodiscard]] static MenuWatch* GetSingleton();
	};
}

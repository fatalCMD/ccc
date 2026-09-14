#include "SD/Dialogue/MenuWatch.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Logging.h"
#include "SD/Core/Tick.h"
#include "SD/Render/Letterbox.h"
#include "SD/Runtime.h"

namespace SD::Dialogue
{
	namespace
	{
		std::uint64_t lastReportedFrame{ 0 };

		bool IsDialogueMenu(const RE::BSFixedString& a_name)
		{
			const char* raw = a_name.c_str();
			return raw && std::string_view{ raw } == RE::DialogueMenu::MENU_NAME;
		}

		// The pausing menus currently open, by name.
		//
		// Kept as names rather than as a bare count because the flag can only be
		// asked on the way IN. On the close event the menu is on its way out of the
		// map and GetMenu may already return nothing, so "was this one of mine"
		// has to be answered from what was recorded when it opened. Anything else
		// decrements on menus it never counted and the total drifts.
		//
		// Small by construction — the deepest real stack is a book or a container
		// opened out of a barter menu — so a vector is the right shape and a scan
		// is cheaper than a hash.
		std::vector<std::string> pausingMenus;

		// PUBLISHED TO THE RENDER THREAD, because it cannot ask this question.
		//
		// The letterbox is drawn from the Present hook, which keeps running while
		// the game thread is stopped — so it has to decide for itself whether the
		// bars belong on screen. It used to decide with RE::UI::GameIsPaused(),
		// which answers a broader question than the one it meant and answers YES
		// for the Console and for SKSE Menu Framework's own settings panel. See
		// Letterbox::SetScreenTaken for what that cost.
		//
		// This list is the narrower answer and already carries the Console
		// exemption, so it is simply pushed across whenever it changes. Called from
		// every place that can change it and nowhere else.
		// The last published answer, so the falling edge can be spotted.
		bool screenWasTaken{ false };

		void PublishScreenTaken()
		{
			const bool taken = !pausingMenus.empty();
			Render::Letterbox::SetScreenTaken(taken);

			// AND THE FALLING EDGE GOES TO THE DIRECTOR, because it is the only
			// place that edge exists.
			//
			// The director will not sample the camera's resting state for half a
			// second after a menu lets go. It used to time that from its own tick,
			// which sees nothing at all while a pausing menu is up — so the freshest
			// thing it knew was when the menu OPENED, and a container held open for
			// longer than the window made the window a no-op. This callback is the
			// close itself, delivered from the one place still running.
			//
			// Here rather than in the close branch below so that Reconcile's sweep —
			// which clears menus whose close event never arrived — publishes the
			// same edge. One writer, both paths.
			if (screenWasTaken && !taken) {
				Camera::Director::OnScreenReleased();
			}
			screenWasTaken = taken;
		}

		// THE ONE EXCEPTION, AND WHY IT IS NAMED RATHER THAN GUESSED AT.
		//
		// The console pauses the game like any other menu, so it stops the tick and
		// would otherwise end the scene. It should not, and the difference is not
		// taste: every menu a dialogue topic opens takes the CONVERSATION with it —
		// the movie's phase moves on, the topic list is spent, the engine hands
		// dialogue to the sub-menu — and the console takes nothing. It is an
		// overlay over a scene that is still exactly where it was, so freezing
		// through it and carrying on is correct, and releasing would put a cut in
		// the middle of somebody looking something up.
		//
		// This is the whole list. A second entry needs the same argument made for
		// it, not a resemblance to this one.
		constexpr std::array kOverlayMenus{
			"Console"sv,
			"Console Native UI Menu"sv,
		};

		// Whether the menu now opening is one that owns the screen.
		//
		// TWO TESTS, AND THE SECOND ONE IS THE INVENTORY REGRESSION.
		//
		// The first is kPausesGame, read off the live menu because a menu's flags
		// belong to the menu and not to a list this mod would have to keep in step
		// with every mod that adds one. It covers the player inventory, containers,
		// barter, gifts, magic, favourites, training, books, the map and the
		// journal — everything a dialogue topic can put in front of you and
		// everything the player can open on their own.
		//
		// The second is kInventoryItemMenu, and without it the set has a hole
		// exactly where the reports were. CraftingMenu carries kInventoryItemMenu
		// and NOT kPausesGame — it is the one item menu in the game that leaves the
		// world running — so a smithing or enchanting screen opened from a topic
		// passed the first test and the cinematic stayed up over it, bars and all,
		// with the HUD elements those menus borrow still hidden. The Present hook's
		// own retraction reads GameIsPaused and had the same hole, which is why the
		// bars were still on screen rather than merely late.
		//
		// Asked as a flag rather than by name so a replacer or a mod-added item
		// menu is covered by construction. The flag bits are read straight off
		// menuFlags: several of IMenu's accessors are wired to the wrong enumerator
		// in this CommonLibSSE, and PausesGame is the only one of them that is
		// correct.
		bool TakesScreen(const RE::BSFixedString& a_name)
		{
			const char* raw = a_name.c_str();
			if (raw && std::find(kOverlayMenus.begin(), kOverlayMenus.end(),
							 std::string_view{ raw }) != kOverlayMenus.end()) {
				return false;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			auto menu = ui->GetMenu(a_name);
			if (!menu) {
				return false;
			}

			return menu->PausesGame() ||
				menu->menuFlags.any(RE::UI_MENU_FLAGS::kInventoryItemMenu);
		}
	}

	bool MenuWatch::ScreenTaken() noexcept
	{
		return !pausingMenus.empty();
	}

	void MenuWatch::Reconcile()
	{
		if (pausingMenus.empty()) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}

		// ASKED PER MENU, NOT OFF THE PAUSE COUNTER.
		//
		// This used to clear the whole record the moment GameIsPaused went false,
		// on the reasoning that a record built from paired events can be left
		// holding a menu whose close was never delivered — a load, a mod
		// force-closing something — and that would be permanent, because no
		// conversation would ever stage again.
		//
		// The reasoning is right and the instrument was wrong, and it broke the
		// moment a menu that does NOT pause the game joined the set. CraftingMenu
		// is exactly that: the record would be taken on the open event and then
		// thrown away by the very next tick, because the game was still running —
		// and Runtime would stage a conversation behind a smithing screen.
		//
		// Whether a menu is open is a question ui can answer directly, so it is
		// asked directly. Same guarantee against a lost close event, no dependence
		// on what the menu does to the clock.
		std::erase_if(pausingMenus, [ui](const std::string& a_name) {
			if (ui->IsMenuOpen(a_name)) {
				return false;
			}
			Log::Info(Log::Category::kDialogue,
				"'{}' is recorded as owning the screen but is no longer open; clearing."sv,
				a_name);
			return true;
		});

		PublishScreenTaken();
	}

	MenuWatch* MenuWatch::GetSingleton()
	{
		static MenuWatch instance;
		return &instance;
	}

	void MenuWatch::Register()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			Log::Error(Log::Category::kDialogue, "UI singleton unavailable; menu watch not registered."sv);
			return;
		}

		ui->AddEventSink<RE::MenuOpenCloseEvent>(GetSingleton());
		Log::Info(Log::Category::kDialogue, "Menu watch registered (engine-driven, independent of the frame hook)."sv);
	}

	RE::BSEventNotifyControl MenuWatch::ProcessEvent(
		const RE::MenuOpenCloseEvent*               a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source)
	{
		(void)a_source;

		if (!a_event) {
			return RE::BSEventNotifyControl::kContinue;
		}

		if (!IsDialogueMenu(a_event->menuName)) {
			// EVERY OTHER MENU, WHICH THIS USED TO DROP ON THE FLOOR.
			//
			// The filter above was the whole body of this function's guard, so the
			// one callback in this mod that still runs while the game is paused
			// listened for exactly one menu and ignored the rest. The menus it was
			// ignoring are the ones a dialogue topic opens — training, barter,
			// gifts, a book — and every one of those stops the frame source, which
			// is where the director's own "another menu took the screen" release
			// lives. It could not fire, so a topic that opened a menu left the
			// scene frozen with the topic list hidden and the camera let go.
			const char* raw = a_event->menuName.c_str();
			if (!raw || !*raw) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const std::string name{ raw };
			const auto        known = std::find(pausingMenus.begin(), pausingMenus.end(), name);

			if (a_event->opening) {
				// Guarded against a repeat open: a menu re-registering without an
				// intervening close would otherwise be recorded twice and the screen
				// would never be handed back.
				if (known == pausingMenus.end() && TakesScreen(a_event->menuName)) {
					pausingMenus.push_back(name);
					PublishScreenTaken();
					Camera::Director::OnScreenTaken(name);
				}
			} else if (known != pausingMenus.end()) {
				// Nothing to tell the director. It released when the screen was
				// taken; Runtime stages the conversation again on the next tick,
				// which is the first one after the game starts running.
				pausingMenus.erase(known);
				PublishScreenTaken();
			}

			return RE::BSEventNotifyControl::kContinue;
		}

		Camera::Director::OnDialogueMenu(a_event->opening);

		const auto frames = Runtime::FrameCount();
		const auto since = frames - lastReportedFrame;
		lastReportedFrame = frames;

		// Report what the polled source believes at the exact moment the engine
		// says the menu changed. If these two disagree the log says so here, rather
		// than the director quietly trusting the wrong one later.
		std::string_view speakerState = "no manager"sv;
		std::string_view topicState = "no manager"sv;
		if (auto* manager = RE::MenuTopicManager::GetSingleton()) {
			const bool haveSpeaker = static_cast<bool>(manager->speaker.get());
			const bool haveLast = static_cast<bool>(manager->lastSpeaker.get());
			speakerState = haveSpeaker ? "speaker"sv : (haveLast ? "lastSpeaker only"sv : "none"sv);
			topicState = manager->currentTopicInfo ? "talking"sv : "silent"sv;
		}

		Log::Info(Log::Category::kDialogue,
			"Dialogue Menu {} | {} ticks total (+{} since last menu event) | manager: {}, {}"sv,
			a_event->opening ? "OPEN "sv : "CLOSE"sv, frames, since, speakerState, topicState);

		// Only meaningful when the frame source is actually installed; with it off
		// by default a zero delta is expected, not a finding.
		if (since == 0 && Core::Tick::Installed()) {
			Log::Error(Log::Category::kDialogue,
				"Frame source did not advance between menu events — PlayerCamera::Update is not a per-frame tick."sv);
		}

		return RE::BSEventNotifyControl::kContinue;
	}
}

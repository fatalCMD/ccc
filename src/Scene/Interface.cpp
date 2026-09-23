#include "SD/Scene/Interface.h"
#include "SD/Scene/DialogueDisplayOverride.h"

#include "SD/Core/Logging.h"

namespace SD::Scene
{
	namespace
	{
		bool suppressed{ false };

		// What this mod took away, at the two levels it can take anything from.
		//
		// Two lists rather than one because the restore has to go back through the
		// same parent it came from, and the parents are found differently: the base
		// is one GetVariable, its children are GetMembers off that. Keeping paths
		// instead would re-parse a string per child per frame for no gain.
		std::vector<std::string> hidden;      // children of HUDMovieBaseInstance
		std::vector<std::string> hiddenRoot;  // children of _root that are not it

		Log::OnceFlag            inventoryReported;

		// Once per conversation, not once per attempt. Suppress() is retried every
		// frame until it takes, and a warning on each of those would bury the log
		// under the one conversation it went wrong in.
		Log::OnceFlag            hudUnavailableReported;

		// Same reasoning for the topic list's hand-back, which is now retried from
		// the tick for as long as it keeps failing.
		Log::OnceFlag            releaseFailedReported;

		// A path inside DialogueMenu that is looked for until it is found, then
		// remembered. See Resolve, below, for why it is not looked for just once.
		struct PathProbe
		{
			std::string path;
			int         attempts{ 0 };
			bool        settled{ false };
		};

		// Two seconds at 60fps. Long enough to cover the menu building itself,
		// short enough that a movie without the path is written off inside the
		// first conversation.
		constexpr int kProbeAttempts = 120;

		// THE LINE. Everything else under HUDMovieBaseInstance goes.
		//
		// The scene is worth protecting from FURNITURE — the compass, the meters,
		// the crosshair, the activate prompt, the clock — all of which are
		// permanently on screen and say nothing about this moment. These two are
		// the opposite: they are transient, they are the game telling the player
		// that something just happened, and swallowing one means the information is
		// GONE rather than deferred. The movie is what queues a notification; hide
		// the movie and it is never drawn and never comes back. Reported as items
		// and quests not showing up during conversations.
		//
		//   MessagesBlock           — the notification stack, whose one child is
		//                             MessageText. Items received, gold, skill
		//                             increases, "you cannot carry any more", and
		//                             every other ShowMessage/ShowNotification.
		//   QuestUpdateBaseInstance — more than its name says, and released for all
		//                             of it. Its children are ObjectiveLineInstance,
		//                             LevelUpTextInstance, LevelMeterBaseInstance
		//                             and ShoutTextInstance: quest starts and
		//                             objectives, levelling up, and learning a
		//                             shout word. Everything the game announces.
		//
		// Both names, and both child lists, are read straight out of the three
		// hudmenu.swf files on this profile by parsing their PlaceObject tags. All
		// three place them at the same depths with the same character ids — every
		// HUD replacer worth the name is an edit of vanilla's movie, and it cannot
		// rename these without breaking the engine, which drives them by name from
		// C++ (see RE::HUDObject::HudComponents, where kQuestUpdateBaseInstance is
		// spelled out).
		//
		// SUBTITLES ARE NOT HERE, and that is not an oversight. HUDMenu's
		// SubtitleTextHolder carries AMBIENT lines — passers-by, guards, the second
		// NPC in the room. A staged conversation's own subtitle is drawn by
		// DialogueMenu, which this never touches, so releasing HUDMenu's would not
		// add the line being spoken; it would add everybody else's on top of it.
		//
		// Adding to this list is all that is needed to protect something else.
		constexpr std::array kReleased{
			"MessagesBlock"sv,
			"QuestUpdateBaseInstance"sv,
		};

		// The backstop, for a movie the depth walk cannot read.
		//
		// The walk below is the primary discovery and this list is not consulted
		// unless it comes back short — but "unless" is doing real work, because
		// getInstanceAtDepth is an AS2 built-in and a movie is entitled to be
		// stranger than the three on this profile.
		//
		// The first block is vanilla's roster, verified identical across SkyHUD,
		// Edge UI and Edge UI Explorer Addon by parsing their PlaceObject tags. The
		// second is the old shipped guess, kept because it costs one HasMember each
		// and covers a HUD of some other lineage that the walk also failed on.
		constexpr std::array kKnownChildren{
			"TutorialLockInstance"sv,
			"LocationLockBase"sv,
			"CompassShoutMeterHolder"sv,
			"Crosshair"sv,
			"StealthMeterInstance"sv,
			"SubtitleTextHolder"sv,
			"RolloverName_mc"sv,
			"RolloverInfo_mc"sv,
			"GrayBarInstance"sv,
			"ActivateButton"sv,
			"WeightTranslated"sv,
			"ValueTranslated"sv,
			"FloatingQuestMarkerInstance"sv,
			"FavorBackButtonBase"sv,
			"Health"sv,
			"ChargeMeters"sv,
			"ChargeMeterBaseAlt"sv,
			"Magica"sv,  // spelled this way in the swf; not a typo here
			"Stamina"sv,
			"BottomLeftLockInstance"sv,
			"BottomRightLockInstance"sv,
			"ArrowInfoInstance"sv,
			"TimeDisplay"sv,
			"EnemyHealth_mc"sv,
			"TopLeftRefInstance"sv,
			"BottomRightRefInstance"sv,
			"ConfigWarning"sv,

			"CompassShoutMeterHolder_mc"sv,
			"Health_mc"sv,
			"Magicka_mc"sv,
			"Stamina_mc"sv,
			"LevelMeter_mc"sv,
			"ShoutMeter_mc"sv,
			"WeaponChargeMeters_mc"sv,
			"RolloverText_mc"sv,
			"RolloverButtonHolder_mc"sv,
			"FavorRolloverText_mc"sv,
			"ActivateButtonHolder_mc"sv,
			"ActivateButtonArt_mc"sv,
			"CrosshairInstance"sv,
			"CrosshairAlert"sv,
			"SneakAnim"sv,
			"SneakAnimInstance"sv,
			"TopMeters_mc"sv,
			"BottomBar_mc"sv,
			"LeftMeters_mc"sv,
			"RightMeters_mc"sv,
		};

		[[nodiscard]] bool Released(std::string_view a_name)
		{
			return std::find(kReleased.begin(), kReleased.end(), a_name) != kReleased.end();
		}

		// Menus that must survive. Everything else open during a conversation is an
		// overlay the scene does not want.
		//
		// Compass Navigation Overhaul and True HUD draw their own menus rather than
		// living inside HUDMenu, which is why hiding CompassShoutMeterHolder left a
		// compass on screen. Unlike a movie's members, the menu table *is*
		// enumerable, so this can be done properly instead of by guessing names.
		constexpr std::array kKeepMenus{
			"Dialogue Menu"sv,
			"HUD Menu"sv,
			"Console"sv,
			"Console Native UI Menu"sv,
			"Cursor Menu"sv,
			"Fader Menu"sv,
			"Loading Menu"sv,
			"Main Menu"sv,
			"LoadWaitSpinner"sv,
			"Top Menu"sv,
			"Overlay Menu"sv,
			"Overlay Interaction Menu"sv,
		};

		std::vector<std::string> hiddenMenus;

		// Foreign menus with one part the player should still see during a
		// conversation. Every child of `parent` except `keep` is faded instead of
		// the whole movie being hidden.
		//
		// TrueHUD draws all its widgets in one menu. Recent Loot (items received)
		// is alone in TrueHUD_PartialVisibilityWidgets, which TrueHUD itself keeps
		// up while the dialogue menu is open, hiding its bars.
		struct PartialKeep
		{
			std::string_view menu;
			const char*      parent;
			std::string_view keep;
		};

		constexpr std::array kPartialKeeps{
			PartialKeep{ "TrueHUD"sv, "_root.TrueHUD", "TrueHUD_PartialVisibilityWidgets"sv },
		};

		struct FadedChild
		{
			std::string menu;
			std::string path;
			double      alpha;  // restored on release
		};

		std::vector<FadedChild>  fadedChildren;
		std::vector<std::string> partialMenus;

		// The opacity below which the list is pulled out of Scaleform hit testing.
		//
		// This was 95, and that is the whole of what "it does not fade, it just
		// snaps" was. _visible=false removes an object from RENDERING as well as
		// from hit testing, so writing it at 95 meant the ease was only ever seen
		// across its top five percent: the list vanished at 95 on the way out and
		// popped back to full at 95 on the way in. Both directions read as a cut
		// because both directions were one.
		//
		// 95 was the right number for a different job. It marked "readable enough
		// to choose from" for the input guard, which had to agree with the Director
		// about the word visible or a press fell between the two definitions. That
		// guard no longer exists, and nothing now needs the list declared
		// unavailable while it is still plainly on screen.
		//
		// A list at 1.5% has told the player nothing and can safely stop catching
		// clicks. Everything above that is drawn, is legible, and stays clickable,
		// which is exactly vanilla's behaviour for a list you can see.
		constexpr float kListDrawnAlpha = 1.5f;

		PathProbe choiceProbe;

		// Frames between foreign-menu sweeps. Enforce() runs every staged frame,
		// but walking ui->menuMap is the one part of it worth throttling — roughly
		// four sweeps a second, which is far faster than a menu can open and be
		// noticed and far slower than it would cost per frame.
		constexpr int kSweepFrames = 15;
		int           sweepCountdown{ 0 };

		// The rows inside the topic list, read for the highlighted entry's text.
		//
		// Probed independently of choiceProbe, which is only ever driven from inside
		// the fade and therefore not at all when bFadeTopicList is off. Sharing one
		// would make reading the text depend on an unrelated feature being enabled.
		PathProbe                rowsProbe;

		// Vanilla BSScrollingList names. Confirmed present by string-scanning
		// Edge UI's dialoguemenu.swf, which is the live copy on this profile.
		// Hoisted out of ReadSelectedTopic so the fingerprint reader shares them.
		constexpr std::array kListPaths{
			"_root.DialogueMenu_mc.TopicListHolder.List_mc"sv,
			"_root.DialogueMenu_mc.TopicList.List_mc"sv,
			"_root.TopicListHolder.List_mc"sv,
			"_root.DialogueMenu_mc.List_mc"sv,
		};

		// THE LAST RESORT, AND NO LONGER A SETTING.
		//
		// bHideHudWholesale used to put this on and the player chose between a clean
		// frame and their notifications. Nobody should have to make that trade, and
		// the keep-list means nobody does — so the flag is now only ever raised by
		// Suppress giving up: a movie with no HUDMovieBaseInstance, or one whose
		// children could not be found by walk or by name. Hiding the root then
		// costs the notifications, which is bad, and leaves the whole HUD standing
		// in every shot, which is worse.
		//
		// It stays a separate flag from `hidden` because it is undone differently:
		// one _visible on the movie's root, not a walk back through named children.
		bool hudHidden{ false };

		// How many sweeps the fallback keeps asking for the children before it
		// accepts the answer. Eight sweeps is two seconds, the same window
		// kProbeAttempts gives every other probe in this file.
		constexpr int kRecoverySweeps = 8;
		int           recoverySweeps{ 0 };

		// The speaker's name, which is vanilla and not a UI replacer.
		//
		// Skyrim prints it beside the highlighted topic — "Arngeir" floating to the
		// left of the list — and it is a *sibling* of TopicListHolder rather than a
		// child of it. That is the whole bug: fading the topic list took the choices
		// away and left the name hanging in an otherwise empty, letterboxed frame.
		//
		// Both DialogueMenu replacers checked (Edge UI, Dragonborn Voice Over) keep
		// the vanilla `SpeakerName` member and its `SetSpeakerName` setter, so the
		// same path covers a replaced menu as well as a stock one.
		PathProbe   nameProbe;
		bool        hideSpeakerName{ true };

		// Resolve a display object inside DialogueMenu by probing a list of paths.
		// This CommonLibSSE still exposes GFxValue::ObjectVisitor with no method
		// that drives it, so a movie's members cannot be listed and named candidates
		// remain the only way in. Every path in this file goes through here so an
		// unresolved one is reported the same way for each.
		[[nodiscard]] bool ResolvePath(
			RE::GFxMovieView*                 a_view,
			std::span<const std::string_view> a_candidates,
			std::string&                      a_out,
			std::string_view                  a_what,
			bool                              a_logMiss)
		{
			std::string tried;
			for (const auto& candidate : a_candidates) {
				const std::string path{ candidate };
				RE::GFxValue      probe;
				if (a_view->GetVariable(&probe, path.c_str()) && probe.IsDisplayObject()) {
					a_out = path;
					Log::Info(Log::Category::kStaging, "{} found at {}."sv, a_what, a_out);
					return true;
				}
				if (!tried.empty()) {
					tried += ", ";
				}
				tried += path;
			}

			if (a_logMiss) {
				Log::Warn(Log::Category::kStaging, "No {} found. Tried: {}"sv, a_what, tried);
			}
			return false;
		}

		// True once the path is resolved and usable. False while it is still being
		// looked for AND once the attempts are spent — callers treat both the same,
		// because no path means no write either way.
		//
		// THE MISS USED TO LATCH ON THE FIRST FRAME, and that was silently costing
		// whole features. Every site here set its `resolved` flag BEFORE probing, so
		// one miss disabled that path for the rest of the session.
		//
		// One miss is exactly what the opening frames produce. The engine builds
		// TopicListHolder a few frames AFTER DialogueMenu opens — the comment in
		// SetChoiceAlpha about _visible being restored on rebuild is the same fact
		// seen from the other side — and SD's first staged frame lands inside that
		// gap often enough to matter. The movie had the path all along; it was asked
		// a frame early, once, and never asked again. The fade, the speaker-name
		// hide and ReadSelectedTopic all fail this way, and all three fail quietly:
		// the fade just never runs, which reads as the setting doing nothing.
		//
		// So a miss now costs one frame, not a session. Bounded, because a movie
		// that genuinely lacks the path must settle into "no" rather than probe
		// forever, and the warning is held back until the last attempt so the log
		// still gets exactly one line either way.
		[[nodiscard]] bool Resolve(
			PathProbe&                        a_probe,
			RE::GFxMovieView*                 a_view,
			std::span<const std::string_view> a_candidates,
			std::string_view                  a_what)
		{
			if (!a_probe.settled) {
				const bool lastChance = ++a_probe.attempts >= kProbeAttempts;
				if (ResolvePath(a_view, a_candidates, a_probe.path, a_what, lastChance)) {
					a_probe.settled = true;
				} else if (lastChance) {
					a_probe.settled = true;
					a_probe.path.clear();
				}
			}

			return !a_probe.path.empty();
		}

		// The menu clip, which owns eMenuState and bAllowProgress. One level above
		// the topic list, not inside it.
		constexpr std::array kMenuPaths{
			"_root.DialogueMenu_mc"sv,
			"_root.DialogueMenu"sv,
		};

		PathProbe menuProbe;

		constexpr std::array kNamePaths{
			"_root.DialogueMenu_mc.SpeakerName"sv,
			"_root.DialogueMenu_mc.SpeakerNameText"sv,
			"_root.DialogueMenu_mc.speakerName"sv,
			"_root.DialogueMenu_mc.NameText"sv,
			"_root.SpeakerName"sv,
		};

		// Keep the movie alive until its managed GFxValue is released. Comparison
		// includes both identities, since the same path can name a new clip.
		struct DialogueNode
		{
			RE::GPtr<RE::GFxMovieView> movie;
			RE::GFxValue value;

			bool operator==(const DialogueNode& a_other) const
			{
				return movie.get() == a_other.movie.get() && value == a_other.value;
			}
			bool ReadAlpha(double& a_out) const
			{
				RE::GFxValue actual;
				if (!value.IsDisplayObject() || !value.GetMember("_alpha", &actual) || !actual.IsNumber()) {
					return false;
				}
				a_out = actual.GetNumber();
				return true;
			}
			bool ReadVisible(bool& a_out) const
			{
				RE::GFxValue actual;
				if (!value.IsDisplayObject() || !value.GetMember("_visible", &actual) || !actual.IsBool()) {
					return false;
				}
				a_out = actual.GetBool();
				return true;
			}
			bool WriteAlpha(double a_alpha)
			{
				return value.IsDisplayObject() && value.SetMember("_alpha", RE::GFxValue{ a_alpha });
			}
			bool WriteVisible(bool a_visible)
			{
				return value.IsDisplayObject() && value.SetMember("_visible", RE::GFxValue{ a_visible });
			}
		};

		DialogueDisplayOverride<DialogueNode> choiceOverride;
		DialogueDisplayOverride<DialogueNode> nameOverride;
		RE::GPtr<RE::GFxMovieView> probedMovie;

		[[nodiscard]] RE::GPtr<RE::GFxMovieView> DialogueView()
		{
			auto* ui = RE::UI::GetSingleton();
			auto view = ui ? ui->GetMovieView(RE::DialogueMenu::MENU_NAME) : nullptr;
			if (view.get() != probedMovie.get()) {
				// Restore only retained objects in the outgoing movie, even if UI
				// no longer exposes it. No debt or failed path probe crosses movies.
				choiceOverride.Reset();
				nameOverride.Reset();
				choiceProbe = {};
				nameProbe = {};
				rowsProbe = {};
				menuProbe = {};
				releaseFailedReported.Reset();
				probedMovie = view;
			}
			return view;
		}

		void SetNameAlpha(float a_alpha)
		{
			auto view = DialogueView();
			if (!view || !Resolve(nameProbe, view.get(), kNamePaths, "speaker name"sv)) {
				return;
			}
			RE::GFxValue node;
			if (view->GetVariable(&node, nameProbe.path.c_str()) && node.IsDisplayObject()) {
				nameOverride.Bind({ view, node });
				nameOverride.SetAlpha(static_cast<double>(std::clamp(a_alpha, 0.0f, 100.0f)));
			}
		}

		[[nodiscard]] bool SetMovieVisible(RE::IMenu* a_menu, bool a_visible, bool a_onlyIfShown)
		{
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			if (!view) {
				return false;
			}

			RE::GFxValue root;
			if (!view->GetVariable(&root, "_root") || !root.IsDisplayObject()) {
				return false;
			}

			if (a_onlyIfShown) {
				RE::GFxValue visible;
				if (root.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
					return false;  // already hidden by someone else
				}
			}

			root.SetMember("_visible", RE::GFxValue{ a_visible });
			return true;
		}

		// Everything still on screen after the pass, so a straggler can be named
		// rather than hunted for in a screenshot.
		void LogRemainingMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			std::string remaining;
			for (auto& entry : ui->menuMap) {
				const char* raw = entry.first.c_str();
				if (!raw || !*raw || !entry.second.menu || !entry.second.menu->uiMovie) {
					continue;
				}

				RE::GFxValue root;
				RE::GFxValue visible;
				if (!entry.second.menu->uiMovie->GetVariable(&root, "_root") || !root.IsDisplayObject()) {
					continue;
				}
				if (root.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
					continue;
				}

				if (!remaining.empty()) {
					remaining += ", ";
				}
				remaining += raw;
			}

			Log::Info(Log::Category::kStaging, "Menus still visible: {}"sv,
				remaining.empty() ? "<none>"s : remaining);
		}

		// CAN THE PLAYER USE THIS MENU? IF SO IT IS NOT THIS MOD'S TO HIDE.
		//
		// The sweep below existed to clear ambient widgets — a compass replacer, a
		// stamina bar, a durability readout — and the thing standing between it and
		// everything else was kKeepMenus, a list of twelve names. That list is what
		// this mod must not touch; it was never a list of what the PLAYER might
		// need, and there is no version of it that could be. Any mod may register
		// any menu under any name.
		//
		// Reported against 1.3.5: a follower framework's dialogue option puts up a
		// confirmation box, the sweep did not recognise the name, and the box was
		// hidden and then held hidden by the reassert — leaving a conversation with
		// no options, no HUD and an invisible prompt waiting for an answer. It only
		// began in 1.3.5 because before that the whole sweep sat behind
		// bHideInterface, and the reporter had it off.
		//
		// A menu that pauses the game, claims modality, wants the cursor, or takes
		// the menu control context is a menu somebody is expected to answer. None
		// of those describe a widget: measured against every menu this mod has
		// actually hidden on this profile — TrueHUD, CastingBar, Durability Menu,
		// Floating Damage, BTPS, SkyParkour and seven status widgets — not one is
		// anything but a passive overlay.
		//
		// The flag bits are read straight off menuFlags rather than through IMenu's
		// accessors: several of those are wired to the wrong enumerator in this
		// CommonLibSSE (UsesCursor returns kUsesMenuContext, UsesMenuContext returns
		// kUsesMovementToDirection, and so on down the block). PausesGame is correct
		// and is the only one used elsewhere in this mod.
		//
		// The two failure directions are not equal, and that is the whole argument
		// for erring wide. Guess wrong here and a widget stays on screen through a
		// conversation. Guess wrong the other way and the player is staring at
		// somebody with no way to answer the question they were just asked.
		[[nodiscard]] bool PlayerFacing(RE::IMenu* a_menu)
		{
			using Flag = RE::UI_MENU_FLAGS;
			return a_menu && a_menu->menuFlags.any(
									 Flag::kPausesGame,
									 Flag::kModal,
									 Flag::kUsesCursor,
									 Flag::kUsesMenuContext,
									 Flag::kUpdateUsesCursor,
									 Flag::kAssignCursorToRenderer);
		}

		[[nodiscard]] bool EnumerateChildren(RE::GFxValue& a_parent, std::vector<std::string>& a_out);

		[[nodiscard]] const PartialKeep* FindPartialKeep(std::string_view a_menu)
		{
			const auto it = std::find_if(kPartialKeeps.begin(), kPartialKeeps.end(),
				[&](const PartialKeep& a_keep) { return a_keep.menu == a_menu; });
			return it != kPartialKeeps.end() ? &*it : nullptr;
		}

		// Fades every child of a_keep.parent except a_keep.keep. Uses _alpha, not
		// _visible, because TrueHUD sets _visible on these containers itself on
		// every menu change. Returns false if the parent can't be found, so the
		// caller can hide the whole movie instead.
		[[nodiscard]] bool FadeAllBut(RE::IMenu* a_menu, std::string_view a_menuName, const PartialKeep& a_keep)
		{
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			RE::GFxValue parent;
			if (!view || !view->GetVariable(&parent, a_keep.parent) || !parent.IsDisplayObject()) {
				return false;
			}

			std::vector<std::string> children;
			if (!EnumerateChildren(parent, children)) {
				return false;
			}

			for (const auto& child : children) {
				if (child == a_keep.keep) {
					continue;
				}

				auto path = std::string{ a_keep.parent } + "." + child;
				RE::GFxValue node;
				if (!view->GetVariable(&node, path.c_str()) || !node.IsDisplayObject()) {
					continue;
				}

				const bool known = std::any_of(fadedChildren.begin(), fadedChildren.end(),
					[&](const FadedChild& a_faded) { return a_faded.menu == a_menuName && a_faded.path == path; });
				if (!known) {
					RE::GFxValue alpha;
					const double saved = node.GetMember("_alpha", &alpha) && alpha.IsNumber() ? alpha.GetNumber() : 100.0;
					fadedChildren.push_back({ std::string{ a_menuName }, std::move(path), saved });
				}
				node.SetMember("_alpha", RE::GFxValue{ 0.0 });
			}
			return true;
		}

		void SetFadedAlpha(bool a_restore)
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			for (const auto& faded : fadedChildren) {
				auto         menu = ui->GetMenu(faded.menu);
				auto         view = menu ? menu->uiMovie : nullptr;
				RE::GFxValue node;
				if (view && view->GetVariable(&node, faded.path.c_str()) && node.IsDisplayObject()) {
					node.SetMember("_alpha", RE::GFxValue{ a_restore ? faded.alpha : 0.0 });
				}
			}
		}

		void SuppressForeignMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			std::string list;
			std::string spared;
			for (auto& entry : ui->menuMap) {
				const char* raw = entry.first.c_str();
				if (!raw || !*raw) {
					continue;
				}
				const std::string name{ raw };

				if (std::find(kKeepMenus.begin(), kKeepMenus.end(), std::string_view{ name }) != kKeepMenus.end()) {
					continue;
				}

				if (PlayerFacing(entry.second.menu.get())) {
					// Named in the log, and only when it is on screen: a menu the
					// player cannot see is not evidence of anything, and a list of
					// every dormant registration would be noise. If a widget ever
					// survives a conversation it should turn up here, which is the
					// one line needed to move it across.
					RE::GFxValue root;
					RE::GFxValue visible;
					auto         view = entry.second.menu ? entry.second.menu->uiMovie : nullptr;
					if (view && view->GetVariable(&root, "_root") && root.IsDisplayObject() &&
						(!root.GetMember("_visible", &visible) || !visible.IsBool() || visible.GetBool())) {
						if (!spared.empty()) {
							spared += ", ";
						}
						spared += name;
					}
					continue;
				}

				// Already held, either way: ReassertForeignMenus keeps it.
				const bool heldWhole = std::find(hiddenMenus.begin(), hiddenMenus.end(), name) != hiddenMenus.end();
				const bool heldPartly = std::find(partialMenus.begin(), partialMenus.end(), name) != partialMenus.end();
				if (heldPartly) {
					continue;
				}
				if (const auto* keep = FindPartialKeep(name); keep && !heldWhole &&
					FadeAllBut(entry.second.menu.get(), name, *keep)) {
					partialMenus.push_back(name);
					Log::Info(Log::Category::kStaging,
						"Foreign menu {}: kept {} visible, faded the rest."sv, name, keep->keep);
					continue;
				}

				if (!SetMovieVisible(entry.second.menu.get(), false, true)) {
					continue;
				}

				// Guarded because this now runs repeatedly rather than once.
				//
				// onlyIfShown already skips anything currently hidden, so a menu
				// this mod took away is passed over on the next sweep — but a menu
				// its owner shows again BETWEEN sweeps comes back through here, and
				// an unguarded push would file it twice. Restore() would then set it
				// visible twice, which is harmless, and the list would grow for as
				// long as the conversation ran, which is not.
				if (std::find(hiddenMenus.begin(), hiddenMenus.end(), name) == hiddenMenus.end()) {
					hiddenMenus.push_back(name);
				}

				if (!list.empty()) {
					list += ", ";
				}
				list += name;
			}

			if (!spared.empty()) {
				Log::Info(Log::Category::kStaging,
					"Left alone as player-facing: {}"sv, spared);
			}

			if (!list.empty()) {
				Log::Info(Log::Category::kStaging, "Foreign menus hidden: {}"sv, list);
			}
		}

		// Push the hide again on menus already taken, which the sweep above cannot
		// do: it asks onlyIfShown so it never touches something already hidden, and
		// everything here is already hidden — by us. Separating the two is what
		// lets one function find newcomers and the other hold what it has.
		void ReassertForeignMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			for (const auto& name : hiddenMenus) {
				if (auto menu = ui->GetMenu(name)) {
					static_cast<void>(SetMovieVisible(menu.get(), false, false));
				}
			}

			// Catches the menu being rebuilt mid-conversation, which resets alpha.
			SetFadedAlpha(false);
		}

		void RestoreForeignMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (ui) {
				for (const auto& name : hiddenMenus) {
					if (auto menu = ui->GetMenu(name)) {
						static_cast<void>(SetMovieVisible(menu.get(), true, false));
					}
				}
			}
			hiddenMenus.clear();

			SetFadedAlpha(true);
			fadedChildren.clear();
			partialMenus.clear();
		}

		[[nodiscard]] RE::GPtr<RE::IMenu> HudMenu()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
		}

		[[nodiscard]] bool AcquireBase(RE::GPtr<RE::IMenu>& a_menu, RE::GFxValue& a_base)
		{
			a_menu = HudMenu();
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			if (!view) {
				return false;
			}
			return view->GetVariable(&a_base, "_root.HUDMovieBaseInstance") && a_base.IsObject();
		}

		[[nodiscard]] bool AcquireRoot(RE::GPtr<RE::IMenu>& a_menu, RE::GFxValue& a_root)
		{
			a_menu = HudMenu();
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			if (!view) {
				return false;
			}
			return view->GetVariable(&a_root, "_root") && a_root.IsDisplayObject();
		}

		// WHERE A TIMELINE CHILD'S DEPTH LIVES.
		//
		// Flash puts objects placed on a timeline into a reserved band starting at
		// -16384, in the order the swf's PlaceObject tags give them. Vanilla's
		// hudmenu.swf places its twenty-nine children across swf depths 1 to 290,
		// which is -16383 to -16094 here. A thousand covers that four times over
		// and still costs one pass.
		constexpr std::int32_t kTimelineBase = -16384;
		constexpr std::int32_t kTimelineSpan = 1024;

		// The other band: anything attachMovie'd at runtime, which lands at zero and
		// above. Walked separately because its extent is only known by asking, and
		// bounded because the answer is somebody else's number.
		constexpr std::int32_t kAttachedCap = 256;

		// EVERY CHILD, BY ASKING THE MOVIE RATHER THAN BY GUESSING.
		//
		// GFxValue::ObjectInterface still exposes ObjVisitor with no method that
		// drives it, so members genuinely cannot be listed — which is what pushed
		// both previous versions of this file into writing names down in advance.
		//
		// But a display object does not have to be enumerated to be found. AS2's
		// MovieClip carries getInstanceAtDepth, and a depth is a number this side
		// can count through: walk the band, ask for whatever is standing at each
		// depth, read its _name. That is the enumeration, one Invoke at a time, and
		// it is correct for a movie nobody here has ever seen.
		//
		// Returns false only when the walk itself did not work — no hits at all —
		// which is the caller's cue to fall back rather than to believe in an empty
		// HUD.
		[[nodiscard]] bool EnumerateChildren(RE::GFxValue& a_parent, std::vector<std::string>& a_out)
		{
			const auto collect = [&](std::int32_t a_depth) {
				RE::GFxValue arg{ static_cast<double>(a_depth) };
				RE::GFxValue child;
				if (!a_parent.Invoke("getInstanceAtDepth", &child, &arg, 1) ||
					!child.IsDisplayObject()) {
					return;
				}

				RE::GFxValue name;
				if (!child.GetMember("_name", &name) || !name.IsString()) {
					return;
				}

				const char* raw = name.GetString();
				if (!raw || !*raw) {
					return;  // unnamed: nothing to hold it by, and nothing to restore
				}

				std::string named{ raw };
				if (std::find(a_out.begin(), a_out.end(), named) == a_out.end()) {
					a_out.push_back(std::move(named));
				}
			};

			const auto before = a_out.size();

			for (std::int32_t i = 0; i < kTimelineSpan; ++i) {
				collect(kTimelineBase + i);
			}

			// getNextHighestDepth answers zero when everything is on the timeline,
			// so the common case pays nothing for this second band at all.
			RE::GFxValue next;
			if (a_parent.Invoke("getNextHighestDepth", &next, nullptr, 0) && next.IsNumber()) {
				const auto top = static_cast<std::int32_t>(next.GetNumber());
				const auto end = std::min(top, kAttachedCap);
				for (std::int32_t d = 0; d < end; ++d) {
					collect(d);
				}

				// Said out loud rather than trimmed quietly: a capped walk is a walk
				// that may have left something on screen, and the number that would
				// have covered it is right here.
				if (top > kAttachedCap) {
					Log::Warn(Log::Category::kStaging,
						"HUD depth walk stopped at {} of {} attached depths; anything above may stay visible."sv,
						kAttachedCap, top);
				}
			}

			return a_out.size() > before;
		}

		// THE HIDE ITSELF: find the movie's children, take everything not released.
		//
		// Returns false when the movie could not be read at all — no base clip, or a
		// base clip whose children answered to neither the walk nor a name. Only the
		// caller knows what to do about that, and both callers do something
		// different, which is why this reports rather than decides.
		//
		// a_report gates the inventory logging so the recovery attempt in Enforce
		// can run quietly. It is the same work either way.
		[[nodiscard]] bool ApplyKeepList(bool a_report)
		{
			RE::GPtr<RE::IMenu> menu;
			RE::GFxValue        base;
			if (!AcquireBase(menu, base)) {
				return false;
			}

			hidden.clear();
			hiddenRoot.clear();

			// The walk first, then the names — and the names are not a fallback for
			// a SHORT walk, only for a failed one.
			//
			// A walk that finds twenty-nine children and a name list that knows
			// about forty-seven disagree constantly and correctly: this movie simply
			// does not have the other eighteen. Running both and taking the union
			// costs one GetMember each and covers the case where getInstanceAtDepth
			// skipped something — text fields are placed at depths like everything
			// else, but Flash's own documentation only promises MovieClips back.
			std::vector<std::string> children;
			const bool               walked = EnumerateChildren(base, children);

			for (const auto& known : kKnownChildren) {
				const std::string key{ known };
				if (std::find(children.begin(), children.end(), key) != children.end()) {
					continue;
				}

				RE::GFxValue member;
				if (base.GetMember(key.c_str(), &member) && member.IsDisplayObject()) {
					children.push_back(key);
				}
			}

			if (children.empty()) {
				return false;
			}

			std::string   kept;
			std::string   taken;
			std::uint32_t keptCount{ 0 };

			for (const auto& name : children) {
				if (Released(name)) {
					if (!kept.empty()) {
						kept += ", ";
					}
					kept += name;
					++keptCount;
					continue;
				}

				RE::GFxValue member;
				if (!base.GetMember(name.c_str(), &member) || !member.IsDisplayObject()) {
					continue;
				}

				// Leave anything already hidden alone, so restoring cannot switch on
				// an element the player or another mod deliberately turned off.
				RE::GFxValue visible;
				if (member.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
					continue;
				}

				member.SetMember("_visible", RE::GFxValue{ false });
				hidden.push_back(name);

				if (!taken.empty()) {
					taken += ", ";
				}
				taken += name;
			}

			// One level up, for whatever else is sharing the movie with the base
			// clip.
			//
			// Vanilla puts exactly one named child on _root and it is
			// HUDMovieBaseInstance, so this normally hides nothing at all. It exists
			// for the mod that attaches its widget beside the base rather than
			// inside it, which is a place the by-name pass could never have looked.
			RE::GPtr<RE::IMenu> rootMenu;
			RE::GFxValue        root;
			if (AcquireRoot(rootMenu, root)) {
				std::vector<std::string> siblings;
				static_cast<void>(EnumerateChildren(root, siblings));

				for (const auto& name : siblings) {
					if (name == "HUDMovieBaseInstance" || Released(name)) {
						continue;
					}

					RE::GFxValue member;
					if (!root.GetMember(name.c_str(), &member) || !member.IsDisplayObject()) {
						continue;
					}

					RE::GFxValue visible;
					if (member.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
						continue;
					}

					member.SetMember("_visible", RE::GFxValue{ false });
					hiddenRoot.push_back(name);

					if (!taken.empty()) {
						taken += ", ";
					}
					taken += "_root." + name;
				}
			}

			if (a_report && inventoryReported.Take()) {
				Log::Info(Log::Category::kStaging, "HUD children hidden ({}): {}"sv,
					walked ? "depth walk"sv : "name probe"sv, taken.empty() ? "<nothing>"s : taken);
				Log::Info(Log::Category::kStaging, "HUD children released: {}"sv,
					kept.empty() ? "<nothing>"s : kept);

				// Every released name this movie does not have, said once.
				//
				// The interesting failure is not a hidden element — those are named
				// above and can be moved across the line by editing kReleased. It is
				// a released one that was never there, because the symptom is a
				// notification quietly missing with nothing on screen to suggest
				// this mod had anything to do with it.
				for (const auto& release : kReleased) {
					const std::string key{ release };
					if (std::find(children.begin(), children.end(), key) == children.end()) {
						Log::Warn(Log::Category::kStaging,
							"Released element {} is not in this HUD; whatever it normally shows may be hidden with the rest."sv,
							key);
					}
				}
			}

			Log::Info(Log::Category::kStaging, "HUD suppressed; {} element(s) hidden, {} released."sv,
				hidden.size() + hiddenRoot.size(), keptCount);

			// NOT RE-WALKED ON THE SWEEP, and that is a decision rather than an
			// omission. Timeline children are fixed by the swf and cannot appear
			// later; a runtime attachMovie into HUDMenu happens when the movie is
			// built, which is before any conversation, so the walk here already has
			// it. A menu that opens mid-conversation is the case that does need
			// catching, and SuppressForeignMenus catches it.
			//
			// TRUE MEANS THE MOVIE WAS READ, NOT THAT SOMETHING WAS TAKEN.
			//
			// A HUD whose every element was already hidden by its owner — iHUD with
			// everything faded out, a replacer mid-rebuild — leaves both lists empty
			// and is nonetheless perfectly understood. Reporting that as a failure
			// would send the caller to the wholesale hide, which would take the
			// notifications away to solve a problem that does not exist.
			return true;
		}
	}

	void Interface::Suppress()
	{
		if (suppressed) {
			return;
		}

		// A full interval before the first re-sweep. This pass has just walked the
		// whole map, so the next frame has nothing new to find.
		sweepCountdown = kSweepFrames;

		// Before the HUD, because the branch below can return early and the name
		// lives in DialogueMenu, which survives either path.
		if (hideSpeakerName) {
			SetNameAlpha(0.0f);
		}

		if (!HudMenu()) {
			// Returns WITHOUT marking the conversation suppressed, on purpose: the
			// caller retries every frame until the movie turns up, which is how a HUD
			// that is not registered yet on the frame a conversation stages gets
			// hidden at all rather than staying up for the whole of it.
			//
			// Which is also why the warning is taken once per conversation and not
			// once per attempt. RestoreHud resets it.
			if (hudUnavailableReported.Take()) {
				Log::Warn(Log::Category::kStaging,
					"HUD movie unavailable; retrying until it appears."sv);
			}
			return;
		}

		// GIVING UP IS LOUD, IMMEDIATE, AND NOT FINAL.
		//
		// An empty child list is not an empty HUD. It is a movie this could not
		// read, and the choice is between leaving the whole HUD standing through
		// every shot and hiding the root, which costs the notifications. The root
		// goes, because a clean frame is what the mod is for — but the frame it was
		// asked on is not evidence about the frame after it, so Enforce keeps
		// asking, and hands the notifications back the moment the answer changes.
		if (!ApplyKeepList(true)) {
			if (auto hud = HudMenu(); hud && SetMovieVisible(hud.get(), false, true)) {
				hudHidden = true;
			}
			recoverySweeps = kRecoverySweeps;

			Log::Warn(Log::Category::kStaging,
				"No HUD children could be read; hiding the whole movie for now. "
				"Notifications and quest updates will not show while that holds."sv);
		}

		SuppressForeignMenus();
		LogRemainingMenus();

		suppressed = true;
	}

	void Interface::Enforce()
	{
		if (!suppressed) {
			return;
		}

		// The wholesale hide, re-pushed every frame with onlyIfShown OFF.
		//
		// It has to be off: the movie is already hidden — by us — so the guard that
		// stops this mod stealing something another mod hid would also stop it
		// holding what it took. The guard did its job once, at Suppress(), and
		// hudHidden is the record that it said yes.
		if (hudHidden) {
			static_cast<void>(SetMovieVisible(HudMenu().get(), false, false));
		}

		// The children, same reasoning. Only elements this mod actually hid are in
		// these two lists, so nothing here can turn off something that was already
		// off — and nothing here can reach a released element, which is why a
		// notification is free to show itself mid-conversation.
		if (!hidden.empty()) {
			RE::GPtr<RE::IMenu> menu;
			RE::GFxValue        base;
			if (AcquireBase(menu, base)) {
				for (const auto& name : hidden) {
					RE::GFxValue member;
					if (base.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
						member.SetMember("_visible", RE::GFxValue{ false });
					}
				}
			}
		}

		if (!hiddenRoot.empty()) {
			RE::GPtr<RE::IMenu> menu;
			RE::GFxValue        root;
			if (AcquireRoot(menu, root)) {
				for (const auto& name : hiddenRoot) {
					RE::GFxValue member;
					if (root.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
						member.SetMember("_visible", RE::GFxValue{ false });
					}
				}
			}
		}

		// Menus are the throttled half. Walking ui->menuMap is the only part of
		// this that scales with what else is installed, and a menu that opens
		// mid-conversation can afford to be caught a quarter of a second later.
		if (--sweepCountdown > 0) {
			return;
		}
		sweepCountdown = kSweepFrames;

		SuppressForeignMenus();   // newcomers
		ReassertForeignMenus();   // what we already hold

		// ASK AGAIN FOR THE CHILDREN, FOR AS LONG AS IT IS WORTH ASKING.
		//
		// Suppress hid the whole movie because it could not read it, and a movie is
		// entitled not to be readable on the frame a conversation stages: HUDMenu is
		// a menu like any other and need not be registered yet — after a cell load,
		// on a fast travel arrival, or while a HUD replacer rebuilds it — and a base
		// clip that has resolved may still be a frame away from having placed its
		// children.
		//
		// A wholesale hide taken on that frame used to be the answer for the rest of
		// the conversation, which is a notification lost to a timing accident. Here
		// it is a holding position: the keep-list is tried again every sweep, and the
		// frame it succeeds on is the frame the root comes back and the notifications
		// with it.
		//
		// Bounded, because a movie that genuinely cannot be read must settle rather
		// than pay for the walk four times a second forever. Two seconds of sweeps is
		// the same window every other probe in this file gets.
		// Gated on the countdown, NOT on hudHidden. The fallback sets both, but it
		// can only set hudHidden if there was a movie to hide — and the case that
		// most needs retrying is the one where there was not: HUDMenu registered but
		// its uiMovie or its base clip not resolving yet. That path leaves hudHidden
		// false with nothing hidden at all, which is the worst state to stop asking
		// in.
		if (recoverySweeps > 0) {
			--recoverySweeps;

			if (ApplyKeepList(false)) {
				if (hudHidden) {
					hudHidden = false;
					static_cast<void>(SetMovieVisible(HudMenu().get(), true, false));
				}
				recoverySweeps = 0;
				Log::Info(Log::Category::kStaging,
					"HUD children turned up late; the keep-list is on and notifications with it."sv);
			}
		}
	}

	bool Interface::SetChoiceAlpha(float a_alpha)
	{
		auto view = DialogueView();
		if (!view) {
			return false;
		}
		SetNameAlpha(hideSpeakerName ? 0.0f : a_alpha);

		// Only a positively identified topic holder may be faded. Never touch
		// the menu root, subtitles, list rows, or the movie's input/progress gates.
		constexpr std::array kTopicPaths{
			"_root.DialogueMenu_mc.TopicListHolder"sv,
			"_root.DialogueMenu_mc.TopicList"sv,
			"_root.DialogueMenu_mc.topicList"sv,
			"_root.TopicListHolder"sv,
			"_root.TopicList"sv,
		};
		if (!Resolve(choiceProbe, view.get(), kTopicPaths, "topic list"sv)) {
			return false;
		}
		RE::GFxValue node;
		if (!view->GetVariable(&node, choiceProbe.path.c_str()) || !node.IsDisplayObject()) {
			return false;
		}
		choiceOverride.Bind({ view, node });
		const float wanted = std::clamp(a_alpha, 0.0f, 100.0f);
		// Ending a fade releases only our writes. A new menu's initial alpha
		// and visibility belong to its construction/transition animation.
		const bool alphaDone = choiceOverride.SetAlpha(static_cast<double>(wanted));
		const bool visibleDone = choiceOverride.SetHidden(wanted < kListDrawnAlpha);
		return alphaDone && visibleDone;
	}

	std::string_view Interface::Name(MenuPhase a_phase)
	{
		switch (a_phase) {
		case MenuPhase::kGreeting:      return "greeting"sv;
		case MenuPhase::kTopicList:     return "topic list"sv;
		case MenuPhase::kTopicClicked:  return "topic clicked"sv;
		case MenuPhase::kTransitioning: return "transitioning"sv;
		default:                        return "unknown"sv;
		}
	}

	std::string Interface::ReadSelectedTopic()
	{
		auto view = DialogueView();
		if (!view) {
			return {};
		}

		if (!Resolve(rowsProbe, view.get(), kListPaths, "topic list rows"sv)) {
			return {};
		}

		RE::GFxValue list;
		if (!view->GetVariable(&list, rowsProbe.path.c_str()) || !list.IsObject()) {
			return {};
		}

		// selectedEntry first: it is what the row under the highlight actually
		// holds. entryList[selectedIndex] is the fallback for a list that exposes
		// the array but not the convenience accessor.
		RE::GFxValue entry;
		if (list.GetMember("selectedEntry", &entry) && entry.IsObject()) {
			RE::GFxValue text;
			if (entry.GetMember("text", &text) && text.IsString()) {
				return text.GetString();
			}
		}

		RE::GFxValue index;
		RE::GFxValue entries;
		if (list.GetMember("selectedIndex", &index) && index.IsNumber() &&
			list.GetMember("entryList", &entries) && entries.IsArray()) {
			const auto i = static_cast<std::uint32_t>(std::max(0.0, index.GetNumber()));
			RE::GFxValue row;
			if (i < entries.GetArraySize() && entries.GetElement(i, &row) && row.IsObject()) {
				RE::GFxValue text;
				if (row.GetMember("text", &text) && text.IsString()) {
					return text.GetString();
				}
			}
		}

		return {};
	}

	std::uint64_t Interface::ReadTopicListFingerprint()
	{
		auto view = DialogueView();
		if (!view) {
			return 0;
		}

		if (!Resolve(rowsProbe, view.get(), kListPaths, "topic list rows"sv)) {
			return 0;
		}

		RE::GFxValue list;
		if (!view->GetVariable(&list, rowsProbe.path.c_str()) || !list.IsObject()) {
			return 0;
		}

		RE::GFxValue entries;
		if (!list.GetMember("entryList", &entries) || !entries.IsArray()) {
			return 0;
		}

		const auto count = entries.GetArraySize();
		if (count == 0) {
			return 0;
		}

		// FNV-1a over the row texts, plus the row count. Cheap, and it does not
		// need to be cryptographic — the only question ever asked of it is
		// "are these the same rows as a moment ago", where the alternative to a
		// collision is a single frame of a stale list.
		std::uint64_t hash = 14695981039346656037ull;
		const auto    mix = [&hash](std::uint8_t a_byte) {
			hash ^= a_byte;
			hash *= 1099511628211ull;
		};

		mix(static_cast<std::uint8_t>(count));
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::GFxValue row;
			RE::GFxValue text;
			if (entries.GetElement(i, &row) && row.IsObject() &&
				row.GetMember("text", &text) && text.IsString()) {
				for (const char* c = text.GetString(); c && *c; ++c) {
					mix(static_cast<std::uint8_t>(*c));
				}
			}
			mix(0x1F);  // row separator, so ["ab","c"] and ["a","bc"] differ
		}

		// 0 is reserved for "could not be read", which callers must not confuse
		// with a legitimate hash — the same distinction LipSync's Sample lost and
		// spent a week paying for.
		return hash ? hash : 1;
	}

	Interface::DialoguePhase Interface::ReadDialoguePhase()
	{
		DialoguePhase out{};

		auto view = DialogueView();
		if (!view) {
			return out;  // no movie: not valid, caller falls back
		}

		if (!Resolve(menuProbe, view.get(), kMenuPaths, "dialogue menu"sv)) {
			return out;
		}

		RE::GFxValue node;
		if (!view->GetVariable(&node, menuProbe.path.c_str()) || !node.IsDisplayObject()) {
			return out;
		}

		// Both members are read independently. A movie that has one and not the
		// other is still worth half an answer, and saying so beats guessing.
		RE::GFxValue state;
		if (node.GetMember("eMenuState", &state) && state.IsNumber()) {
			switch (static_cast<int>(state.GetNumber())) {
			case 0:  out.phase = MenuPhase::kGreeting; break;
			case 1:  out.phase = MenuPhase::kTopicList; break;
			case 2:  out.phase = MenuPhase::kTopicClicked; break;
			case 3:  out.phase = MenuPhase::kTransitioning; break;
			default: out.phase = MenuPhase::kUnknown; break;
			}
			out.valid = true;
		}

		RE::GFxValue allow;
		if (node.GetMember("bAllowProgress", &allow) && allow.IsBool()) {
			out.lineInFlight = !allow.GetBool();
			out.valid = true;
		}

		return out;
	}

	void Interface::SetHideSpeakerName(bool a_hide)
	{
		if (hideSpeakerName == a_hide) {
			return;
		}
		hideSpeakerName = a_hide;

		// Switching it OFF has to put the name back here and now.
		//
		// The name is otherwise only ever written from inside SetChoiceAlpha, and
		// SetChoiceAlpha only runs while the fade or the HUD hide is on. Turn the
		// name hide off in the menu while both of those are off and nothing would
		// ever write 100 to it — the name would stay invisible for the rest of the
		// conversation, and the control would read as broken.
		if (!a_hide) {
			SetNameAlpha(100.0f);
		}
	}

	void Interface::RestoreHud()
	{
		// Armed for the next conversation whether or not this one suppressed
		// anything — RestoreHud is called on every close, and a warning owed to a
		// conversation that never got its HUD is owed again to the next one.
		hudUnavailableReported.Reset();

		if (!suppressed) {
			return;
		}
		suppressed = false;
		recoverySweeps = 0;
		RestoreForeignMenus();

		if (hudHidden) {
			hudHidden = false;
			auto* ui = RE::UI::GetSingleton();
			if (auto hud = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr) {
				static_cast<void>(SetMovieVisible(hud.get(), true, false));
			}
		}

		RE::GPtr<RE::IMenu> rootMenu;
		RE::GFxValue        root;
		if (AcquireRoot(rootMenu, root)) {
			for (const auto& name : hiddenRoot) {
				RE::GFxValue member;
				if (root.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
					member.SetMember("_visible", RE::GFxValue{ true });
				}
			}
		}
		hiddenRoot.clear();

		RE::GPtr<RE::IMenu> menu;
		RE::GFxValue        base;
		if (!AcquireBase(menu, base)) {
			hidden.clear();
			return;
		}

		for (const auto& name : hidden) {
			RE::GFxValue member;
			if (base.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
				member.SetMember("_visible", RE::GFxValue{ true });
			}
		}

		hidden.clear();
	}

	void Interface::ReleaseChoices()
	{
		// Observe replacement before considering a retry. Restoration itself
		// uses retained object handles, never a path in whichever movie is now up.
		static_cast<void>(DialogueView());
		const bool choicesDone = choiceOverride.Release();
		const bool nameDone = nameOverride.Release();
		if (!choicesDone || !nameDone) {
			if (releaseFailedReported.Take()) {
				Log::Warn(Log::Category::kStaging,
					"Dialogue UI restore pending on its original display object; retrying."sv);
			}
			return;
		}
		releaseFailedReported.Reset();
	}

	void Interface::Restore()
	{
		// Both halves, unconditionally. Each is idempotent and each guards itself,
		// which is the point of splitting them: Close() does not have to know which
		// features were on.
		RestoreHud();
		ReleaseChoices();
	}
}

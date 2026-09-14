#include "SD/Runtime.h"

#include "SD/Camera/Director.h"
#include "SD/Camera/Presets.h"
#include "SD/Compat/Conflicts.h"
#include "SD/Compat/DBReV.h"
#include "SD/Compat/ImprovedCamera.h"
#include "SD/Compat/SmoothCam.h"
#include "SD/Compat/VoiceCommand.h"
#include "SD/Core/Config.h"
#include "SD/Core/Hotkeys.h"
#include "SD/Core/Logging.h"
#include "SD/Core/Tick.h"
#include "SD/Dialogue/LineWatch.h"
#include "SD/Dialogue/MenuWatch.h"
#include "SD/Dialogue/Session.h"
#include "SD/Menu/Settings.h"
#include "SD/Render/Letterbox.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/LightRig.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/RegionalFace.h"
#include "SD/Scene/Interface.h"

#include <chrono>

namespace SD::Runtime
{
	namespace
	{
		std::atomic_bool ready{ false };
		Log::OnceFlag    firstFrame;
		Log::OnceFlag    waitingForLiveSpeaker;

		std::uint64_t windowFrames{ 0 };
		float         windowElapsed{ 0.0f };
		float         windowPeakDelta{ 0.0f };

		constexpr float kHeartbeatSeconds = 10.0f;


		// The partner this conversation was opened for, cleared only when the
		// session genuinely ends. One open per conversation, no retries — a retry
		// loop is what made the camera flicker in and out after an early exit.
		RE::FormID openedFor{ 0 };

		// AND WHICH CONVERSATION WITH THEM, because the form id alone was not an
		// identity and the difference was a reported bug.
		//
		// Talking to somebody, walking away while they are still speaking, and
		// walking straight back is two conversations with one id. The key matched,
		// so nothing staged, and the second one ran with no camera at all. Session
		// counts conversations now; this records which one was staged.
		std::uint32_t openedSerial{ 0 };

		using Clock = std::chrono::steady_clock;

		// When the last re-stage of an already-staged conversation was attempted.
		//
		// A re-stage is a retry, and a retry needs a floor or it becomes the
		// per-frame loop this block has a paragraph about. One second: an Open that
		// is refused costs one SmoothCam round trip and one warning line at that
		// rate, and whichever plugin is holding the camera is not going to let go
		// inside a frame.
		Clock::time_point lastRestage{};

		constexpr float kRestageRetrySeconds = 1.0f;

		[[nodiscard]] float SecondsSince(Clock::time_point a_when)
		{
			return std::chrono::duration<float>(Clock::now() - a_when).count();
		}

		bool ReadFlag(const char* a_section, const char* a_key, int a_default)
		{
			return Config::Bool(a_section, a_key, a_default != 0);
		}

		// A reopened menu can be stuck in its greeting while lastSpeaker still
		// carries the previous line. Wait for Session's live-speaker evidence;
		// menu presence alone is not permission to recover the old camera scene.
		[[nodiscard]] bool Stranded()
		{
			if (Camera::Director::Staging() || Camera::Director::Suspended() ||
				!Dialogue::Session::GetSingleton().PlayerEngaged()) {
				return false;
			}

			if (SecondsSince(lastRestage) < kRestageRetrySeconds) {
				return false;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || player->IsInCombat()) {
				return false;
			}

			// Asked of the engine rather than of Director::DialogueMenuUp(), which
			// is an event latch. Same reason the director's own exit check asks the
			// engine: a latch left stuck on would restage once a second forever.
			auto* ui = RE::UI::GetSingleton();
			return ui && !ui->GameIsPaused() && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}
	}

	std::uint64_t FrameCount() noexcept
	{
		return Core::Tick::Total();
	}

	void Initialize()
	{
		Log::LogLoadedModule();

		// Before any conversation, so the first line spoken in the session is
		// already identifying its voice pack. See Compat::VoiceCommand.
		Compat::VoiceCommand::Install();

		// Line detection first, and deliberately independent of the frame source.
		// UpdateInDialogue is driven by the engine and carries its own payload, so
		// it kept working through two frame sources that turned out not to tick.
		Dialogue::LineWatch::Install();
		Dialogue::MenuWatch::Register();

		// DIALOGUE INPUT IS NOT THIS MOD'S, and there is no longer any code here
		// that could make it so.
		//
		// An earlier build put a MenuEventHandler first in the MenuControls chain,
		// swallowed presses while the topic list was down, redirected them into
		// cutting the spoken line, and held the dialogue movie's own commit gate
		// shut. Every piece of it worked and was proven in play. It was removed
		// anyway, because a camera mod sitting in the input path of every dialogue
		// press makes dialogue feel like it belongs to the mod rather than to the
		// game — which is what it was reported as.
		//
		// The list still fades; that is a picture, not a control. A click during a
		// fade does exactly what vanilla does with it. See Director's note on
		// playerHasChosen for why the fade waits for the first selection.
		Log::Info(Log::Category::kDialogue,
			"Dialogue input untouched: no handler installed, no press intercepted."sv);

		Config::ReportSource();
		Compat::ReportKnownConflicts();

		// Next to the conflict report because it is the same question asked of the
		// same instrument — which camera mods are in this process — and it wants
		// the same answer time. Improved Camera is not a conflict, though: it is a
		// mod this one has to share a camera state with, and Detect is where that
		// sharing is decided. See Compat::ImprovedCamera.
		Compat::ImprovedCamera::Detect();

		Compat::SmoothCam::Request();

		// WHAT USED TO BE HERE: Scene::Rigs::Load(), which read two hundred and
		// fifty lighting keys out of the ini into a cache before the settings load
		// could resolve any angle's rig name against them.
		//
		// The looks are constants now. There is nothing to load, nothing to cache,
		// and no ordering to get wrong.

		// Before the menu, so the panel opens showing what is in the file rather
		// than the built-in defaults.
		Camera::Director::LoadSettings();

		// A preset asked for by name in the ini, for load orders with no SKSE Menu
		// Framework and therefore no menu to press a button in. Applied after the
		// settings load so it writes over them, and it clears the key on the way
		// out so it happens once rather than every launch.
		Camera::ApplyPendingPreset();

		// Re-read, because ApplyPendingPreset writes to the INI rather than to the
		// live tunables. Without this a preset named in sApply would sit in the file
		// unapplied until the first conversation re-ran ReadTuning.
		Camera::Director::LoadSettings();

		// Safe when the framework is absent — it checks for the DLL and returns.
		Menu::Settings::Register();

		// bHideInterface, bHideHudWholesale, bHideSpeakerName AND THE TOPIC-LIST
		// HOLD USED TO BE READ HERE, ONCE, AND THAT WAS THE WHOLE OF THEIR
		// SUPPORT.
		//
		// Four settings applied at startup and never again. The menu wrote them to
		// SD_user.ini, the Director kept the copy it had been handed at launch, and
		// the two disagreed for the rest of the session — so switching the HUD hide
		// on in game did nothing at all, and the panel admitted as much with a
		// caption telling the player to restart. A control that needs the game
		// restarted is a control that gets reported as broken, correctly.
		//
		// The two that remain are Tunables fields, filled by ReadTuning from the
		// same ini and pushed by the menu through ApplyTunables, which applies each
		// to the conversation already on screen. LoadSettings above has therefore
		// already read both; there is nothing left to do here.
		//
		// The other two are not settings at all now. THE OLD NOTE HERE ARGUED THE
		// TRADE AND IT WAS THE WRONG QUESTION. It said bHideHudWholesale had to
		// default off because hiding the whole of HUDMenu takes with it everything
		// the game uses to TELL the player something — quest updates, items
		// received, skill increases — which are lost rather than deferred, and that
		// anyone wanting the cleanest frame could switch it on and accept that.
		//
		// Nobody should have to accept it. The choice only existed because the
		// alternative was a list of element names guessed from vanilla, which
		// matched three of the twenty-nine children of the HUD actually installed
		// here. Scene::Interface walks the movie's own depths now, so it can take
		// everything it finds and release the two elements the notifications live
		// in. There is no trade left, so there is nothing to configure.

		// INSTALLED UNCONDITIONALLY, and bLetterbox is a live setting instead.
		//
		// The hook used to be gated on the key, which made the bars a restart-only
		// choice with no control in the menu — the same shape as bEnabled and
		// bHideInterface before them, and the same outcome: a switch nobody could
		// find and could not have used if they had. The bars are a Tunables field
		// now, applied to the conversation already on screen, and a retracted
		// letterbox costs one atomic read and an early return per present.
		Render::Letterbox::Install();

		// After LoadSettings, because it reads its bindings through the same
		// resolver, and before the ready flag, because a key pressed on the first
		// frame should work like any other.
		Core::Hotkeys::Install();

		// bEnabled IS NO LONGER READ HERE, and that is the fix rather than a move.
		//
		// It used to be read once into a file-static that gated the open/close
		// block below, with no menu control anywhere — so the only way to turn this
		// mod off was to quit Skyrim and edit a file. It is a Tunables field now,
		// filled by the LoadSettings call above and pushed live by the checkbox at
		// the top of the settings panel.
		Log::Info(Log::Category::kCamera, "Directed shots are {}."sv,
			Camera::Director::Directing() ? "ENABLED"sv : "disabled ([Direction] bEnabled=0)"sv);

		// Behind the same flag as the face probe: it is a diagnostic, and it patches
		// a vtable every head in the game goes through.
		//
		// iForceViseme installs it too. Requiring bLogFaceAnim as well would make
		// the forced test silently do nothing for anyone who set only the dial they
		// were told to set — this project's most repeated failure, and one that
		// costs a whole test cycle every time.
		// Synthesized lipsync writes THROUGH this hook, so it is no longer only a
		// diagnostic and the install condition has to include it. Missing this would
		// leave bSynthLipSync switched on and doing absolutely nothing, with a log
		// that says the feature is enabled — the exact failure this project has hit
		// more times than any other.
		Scene::LipSync::Configure(
			ReadFlag("Performance", "bSynthLipSync", 1),
			Config::Int("Performance", "iLipSyncStrength", 55));

		Scene::Performance::Configure(ReadFlag("Performance", "bExpressions", 1), false);
		Scene::RegionalFace::SetEnabled(ReadFlag("Performance", "bRegionalExpressions", 1));

		const int forcedViseme = Config::Int("Diagnostics", "iForceViseme", -1);
		if (ReadFlag("Diagnostics", "bLogFaceAnim", 0) || forcedViseme >= 0 ||
			Scene::LipSync::Enabled() ||
			Scene::Performance::ExpressionsEnabled() || Config::Int("Diagnostics", "iForceExpression", -1) >= 0) {
			Scene::FaceGen::Install();
		}
		Scene::FaceGen::SetForcedViseme(forcedViseme);

		if (!Scene::FaceGen::Installed()) {
			if (forcedViseme >= 0) {
				Log::Warn(Log::Category::kCore,
					"iForceViseme is set but the morph hook did not install, so nothing will be written."sv);
			}
			if (Scene::LipSync::Enabled()) {
				Log::Warn(Log::Category::kCore,
					"bSynthLipSync is on but the morph hook did not install, so the mouth will not be driven."sv);
			}
		}

		if (ReadFlag("Diagnostics", "bFrameSource", 1)) {
			Core::Tick::Install();
		} else {
			Log::Info(Log::Category::kCore,
				"Frame source disabled by ini. Line starts will still be observed; line ends will not."sv);
		}

		ready.store(true, std::memory_order_release);
		Log::Info(Log::Category::kCore, "Ready — observing dialogue, not yet directing it."sv);
	}

	void OnFrame(RE::PlayerCamera*, float a_delta)
	{
		if (!ready.load(std::memory_order_acquire)) {
			return;
		}

		if (firstFrame.Take()) {
			Log::Info(Log::Category::kCore, "First frame observed via '{}'."sv,
				Core::Tick::Name(Core::Tick::Primary()));
		}

		++windowFrames;
		windowElapsed += a_delta;
		windowPeakDelta = std::max(windowPeakDelta, a_delta);

		if (windowElapsed >= kHeartbeatSeconds) {
			const float rate = windowElapsed > 0.0f ? static_cast<float>(windowFrames) / windowElapsed : 0.0f;

			// Every candidate is reported, not just the winner. A source that ticks
			// at a plausible frame rate and one that ticks twice are both useful to
			// know about, and the difference is invisible unless both are counted.
			Log::Info(Log::Category::kCore,
				"Tick heartbeat: {:.1f}/s over {:.1f}s (peak delta {:.3f}s) | PlayerCamera={} PlayerCharacter={} ThirdPersonState={}"sv,
				rate, windowElapsed, windowPeakDelta,
				Core::Tick::Count(Core::Source::kPlayerCamera),
				Core::Tick::Count(Core::Source::kPlayerCharacter),
				Core::Tick::Count(Core::Source::kThirdPersonState));

			windowFrames = 0;
			windowElapsed = 0.0f;
			windowPeakDelta = 0.0f;
		}

		auto& session = Dialogue::Session::GetSingleton();
		session.OnFrame(a_delta);

		// Release conditions are checked here rather than in the camera hook, so
		// they still run when the camera state has changed out from under staging.
		Camera::Director::Tick(a_delta);

		// Staging follows the conversation, not the line. Opening per line would
		// re-negotiate with SmoothCam several times inside one exchange.
		if (Camera::Director::Directing()) {
			const bool       active = session.Active();
			auto             partner = session.Partner().get();
			const RE::FormID partnerID = partner ? partner->GetFormID() : 0;
			const auto       serial = session.ConversationSerial();

			// Has this conversation been staged at all yet?
			const bool fresh = (partnerID != openedFor || serial != openedSerial);

			// It has, and the director let go of it anyway. See Stranded.
			const bool restaging = active && !fresh && Stranded();

			if (!active || session.PlayerEngaged()) {
				waitingForLiveSpeaker.Reset();
			} else if (partner && !Camera::Director::Staging()) {
				auto* ui = RE::UI::GetSingleton();
				if (ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) && waitingForLiveSpeaker.Take()) {
					Log::Info(Log::Category::kCamera,
						"Dialogue menu reopened with only the previous speaker [{:08X}]; "
						"waiting for live dialogue before staging."sv, partnerID);
				}
			}

			if (!active) {
				if (openedFor != 0) {
					Camera::Director::Close();
					openedFor = 0;
					openedSerial = 0;
				}

				// SUSPENDED IS NOT THE SAME AS FINISHED, AND THIS IS WHERE THE TWO
				// PART COMPANY.
				//
				// A conversation whose screen was taken by an inventory or a barter
				// window left a resume armed. Most of the time the session is still
				// live underneath it and the next branch stages it again. Sometimes
				// it is not — the trade was the last thing the topic did, the NPC
				// walked off, the player loaded a save from the menu — and then the
				// armed resume has nothing to come back to.
				//
				// Dropped here rather than left to expire, so a later, unrelated
				// conversation with the same actor cannot inherit an angle and a
				// first-person debt from one that ended half an hour ago.
				if (Camera::Director::Suspended()) {
					Camera::Director::AbandonSuspension();
				}
			} else if (partner && (fresh || restaging) && !Dialogue::MenuWatch::ScreenTaken() &&
				(session.PlayerEngaged() || Camera::Director::Suspended())) {
				// NOT WHILE SOMEBODY ELSE HAS THE SCREEN.
				//
				// Staging behind a menu that pauses the game is staging into a
				// freeze: the frame source stops on the same flag, so whatever the
				// opening frame wrote — the letterbox out, the HUD hidden, the topic
				// list taken down — is held with nothing running that could put it
				// back. That is the state a training or barter topic used to leave
				// the player in, and it is reachable from this branch too, on the
				// frame between a menu opening and the tick noticing.
				//
				// The condition is only ever true for a frame or two in practice.
				// The tick does not run while the game is paused, so the first frame
				// that gets here after the menu closes is the one that stages.
				//
				// A NEW CONVERSATION IS THE USUAL REASON TO OPEN, and for a long
				// time it was the only one. Once a conversation had been staged it
				// was not staged again, whatever the director did next.
				//
				// This previously also reopened whenever the director was not
				// staging but the session was live, which fought the exit path
				// head-on: leaving early makes the director release itself while
				// the session is still running on the NPC's trailing line, so it
				// was reopened, released, reopened — once a second until the NPC
				// finally stopped talking.
				//
				// THE SECOND REASON IS Stranded(), and it does not bring that loop
				// back. The old reopen asked "is the session live", which the
				// trailing line answers yes to for as long as the NPC keeps
				// talking, while the release was asking a different question
				// entirely — so the two disagreed forever, once a second, and the
				// camera flickered.
				//
				// Recovery requires a live speaker as well as a live menu. A
				// trailing lastSpeaker is not enough, even when the movie has
				// reopened: that was the six-second greeting stall recorded on
				// Duraz. Refused acquisitions still retry at most once a second.
				//
				// WHAT CHANGED IS THE KEY, not the rule. It was the partner's form
				// id alone, which cannot tell two conversations with the same
				// person apart — so re-entering dialogue with somebody who was
				// still finishing their last line staged nothing at all. Session
				// counts conversations; either half of the key moving is a new one.

				// A SUSPENSION IS NOT OWED A RETURN TO A CONVERSATION THE PLAYER
				// HAS LEFT.
				//
				// Ending the conversation from inside a barter or inventory screen
				// — walking out, the trade being the last thing the topic did —
				// leaves the session alive on the NPC's trailing line, which is
				// correct and is not something to put bars back over. Without this
				// the resume would stage, the director's own exit check would
				// notice the null speaker a moment later, and the player would get
				// a flash of the cinematic on the way out of a menu.
				//
				// Marked as staged rather than left unhandled, so this is decided
				// once for the conversation instead of re-asked every frame until
				// the NPC stops talking.
				if (restaging) {
					lastRestage = Clock::now();
					Log::Info(Log::Category::kCamera,
						"Conversation {} with [{:08X}] is still on screen and nothing was staging it; "
						"taking it back."sv,
						serial, partnerID);
				}

				if (Camera::Director::Suspended() && !session.PlayerEngaged()) {
					Camera::Director::AbandonSuspension();
				} else {
					Camera::Director::Open(partner->As<RE::Actor>(), restaging);
				}

				openedFor = partnerID;
				openedSerial = serial;
			}
		}
	}

	void RearmConversation()
	{
		openedFor = 0;
		openedSerial = 0;
	}

	void OnGameLoaded()
	{
		// The save may have been written mid-conversation, in which case the
		// manager state it carries belongs to a session this build never opened.
		Dialogue::Session::GetSingleton().Abandon();
		firstFrame.Reset();
		Log::Info(Log::Category::kCore, "Game loaded; session state re-keyed. {} ticks so far."sv, FrameCount());
	}

	void AbandonForLoad()
	{
		// Hand the camera back synchronously. Anything deferred would not run until
		// after the load, which is how a player wakes up in the new save with the
		// camera still parked on an NPC who no longer exists.
		Camera::Director::Close();
		Scene::Performance::ResetForLoad();
		Scene::LipSync::Release();
		openedFor = 0;
		openedSerial = 0;

		// A resume armed for the outgoing world has nothing to come back to in the
		// new one, and the form id it is keyed on may well belong to somebody else
		// there.
		//
		// WITHOUT THE VIEW HAND-BACK. The resting aim and zoom it would write were
		// sampled in the world that is being torn down, and the camera state object
		// survives a load — so paying that debt here would carry the outgoing
		// save's third-person zoom into the incoming one.
		Camera::Director::AbandonSuspension(false);

		Dialogue::Session::GetSingleton().Abandon();

		// A line left open across a load would otherwise sit there claiming the
		// player is mid-sentence, holding the topic list up in the new save.
		Compat::DBReV::Reset();

		// Same for a line SpeakSound announced but nothing drained — it would open
		// in the new save, on a conversation that is not happening. The learned
		// pack survives on purpose; see VoiceCommand::Reset.
		Compat::VoiceCommand::Reset();
	}
}

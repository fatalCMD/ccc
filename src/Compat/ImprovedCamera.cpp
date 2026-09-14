#include "SD/Compat/ImprovedCamera.h"

#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"

namespace SD::Compat
{
	namespace
	{
		bool present{ false };
		bool asked{ false };

		// WHICH generation answered, because they do not share a config layout.
		//
		// Detecting both names and then reading one product's files is how a
		// diagnostic starts lying: the legacy build has no SKSE\Plugins\
		// ImprovedCameraSE\ folder at all, so a legacy-only install would have been
		// told to go and edit a path that does not exist. Null means nothing
		// matched.
		const wchar_t* matched{ nullptr };

		// Detected by loaded module, the same way Conflicts.cpp names its entries
		// and for the same reason: a plugin handle is load order and means nothing
		// to a reader, while a file name is something a user can find in their mod
		// manager and something a bug report can be checked against.
		//
		// BOTH SPELLINGS, because the mod has had two lives. The original ships
		// ImprovedCamera.dll; Improved Camera SE — the rewrite, and what nearly
		// everybody is actually running, including its NG builds — ships
		// ImprovedCameraSE.dll. Matching only the newer name would leave anyone on
		// the older one silently in the fighting path, which is the failure this
		// whole file exists to end.
		constexpr std::array kModules{
			L"ImprovedCameraSE.dll",
			L"ImprovedCamera.dll",
		};

		// WHERE IMPROVED CAMERA KEEPS THE ANSWER, WHICH IS NOT THE FILE NAMED AFTER
		// IT.
		//
		// ImprovedCameraSE.ini is the loader's own config — the window name, the
		// menu key, the supported exe versions — and the camera settings are not in
		// it. What it carries is [MODULE DATA] ProfileName, naming a file under
		// Profiles\ that holds the rest. It ships as Default.ini, and reading that
		// name rather than assuming it is the difference between diagnosing the
		// player in front of you and diagnosing a default they are not using.
		//
		// The name includes its own extension, so it is appended as-is.
		[[nodiscard]] std::wstring ProfilePath()
		{
			const auto loader = Config::DataPath(L"SKSE\\Plugins\\ImprovedCameraSE\\ImprovedCameraSE.ini");
			if (loader.empty()) {
				return {};
			}

			// THE LOADER HAS TO EXIST BEFORE ITS ANSWER MEANS ANYTHING.
			//
			// GetPrivateProfileStringW cannot fail here: handed a file that is not
			// there it returns the default, so "Default.ini" comes back whether the
			// installation says so or there is no installation of this generation at
			// all. Without this test the function happily built a plausible path out
			// of two guesses and the warning below printed it as the file to edit.
			std::error_code ec{};
			if (!std::filesystem::exists(std::filesystem::path{ loader }, ec) || ec) {
				return {};
			}

			wchar_t name[MAX_PATH]{};
			::GetPrivateProfileStringW(L"MODULE DATA", L"ProfileName", L"Default.ini",
				name, static_cast<DWORD>(std::size(name)), loader.c_str());

			if (!name[0]) {
				return {};
			}

			std::wstring relative = L"SKSE\\Plugins\\ImprovedCameraSE\\Profiles\\";
			relative += name;
			return Config::DataPath(relative);
		}

		// THE ONE SETTING THAT DECIDES WHETHER THESE TWO MODS CAN SHARE A CAMERA.
		//
		// Skyrim takes the player's movement controls away for the length of a
		// conversation, and that is one of the three tests Improved Camera uses to
		// decide a camera state is "scripted":
		//
		//     if (Helper::IsScripted() || !controlMap->IsMovementControlsEnabled() ||
		//         Helper::CorrectFurnitureIdle())
		//         m_ThirdPersonState = CameraThirdPerson::State::kScriptedEnter;
		//
		// So EVERY dialogue in the game is a scripted third-person event to it,
		// whether or not this mod is installed. That state takes its enable from
		// [EVENTS] bScripted, and with it on — the shipped default — a player who
		// was in first person gets Improved Camera's fake first person: it pins the
		// third-person zoom to its minimum every frame and translates the camera to
		// the player's head.
		//
		// Scene Director spends those same frames driving an absolute world pose
		// for a shot across the room. Both write the camera, neither yields, and
		// what the player sees is the view flipping between the angle and their own
		// eyes — reported as "it zooms in and out repeatedly".
		//
		// REPORTED RATHER THAN WORKED AROUND, and that is not laziness. There is
		// nothing to negotiate with on 1.1.x — measured on 1.1.2.4228, and later
		// generations are reported to publish one, so keep this scoped when the
		// support target moves. That build exports only the three SKSE
		// entry points, and where it does ask SmoothCam for the camera it discards
		// the answer — the refusal Scene Director's own hold produces is written as
		// `if (result == OK) {}` and falls through. Backing off on this mod's side
		// alone would simply mean no cinematic, silently, for a setting the player
		// does not know is on.
		//
		// WHAT bScripted=0 ACTUALLY COSTS, stated carefully because the first draft
		// of this said "conversations only" and that is not true. The key gates
		// Improved Camera's whole scripted-forced-third-person category — the test
		// above is three conditions, and dialogue is one of them — so turning it off
		// gives up its first-person handling for every event in that category, not
		// just for talking to people. Dialogue is merely the one that collides with
		// this mod, and the recommendation is worth making on those terms rather
		// than by understating the price.
		//
		// ONLY IN FIRST PERSON. The whole path above is inside `if (m_IsFirstPerson)`,
		// so a player who plays in third person is not affected and must not be told
		// they are — a warning that does not apply is how the ones that do get
		// ignored.
		void ReportScriptedEvent()
		{
			// THE LEGACY BUILD IS NOT ASKED ABOUT THE NG LAYOUT.
			//
			// Everything below — the loader file, the Profiles folder, the [EVENTS]
			// section, the bScripted key — is Improved Camera SE's schema. The
			// original Improved Camera is a different product with a different
			// layout, and reading one while detecting the other produces a confident
			// answer about a file that was never there. Said plainly instead.
			if (matched && std::wstring_view{ matched } == L"ImprovedCamera.dll") {
				Log::Warn(Log::Category::kCompat,
					"This is the original Improved Camera, not Improved Camera SE. Scene Director "
					"knows the SE configuration layout only, so it cannot tell you whether the "
					"conflicting scripted-camera setting is on. If conversations fight the camera "
					"in first person, look for that mod's scripted forced-third-person option."sv);
				return;
			}

			const auto profile = ProfilePath();
			if (profile.empty()) {
				Log::Warn(Log::Category::kCompat,
					"Improved Camera's profile could not be located, so its [EVENTS] bScripted "
					"setting is unknown. If conversations fight the camera in first person, set "
					"it to 0."sv);
				return;
			}

			// -1 as the default, because 0 is a real answer here and the absent case
			// needs telling apart from it: a missing profile means the diagnosis
			// failed, and reporting that as "configured correctly" would be worse
			// than saying nothing.
			const int scripted = ::GetPrivateProfileIntW(L"EVENTS", L"bScripted", -1, profile.c_str());

			if (scripted < 0) {
				Log::Warn(Log::Category::kCompat,
					"Improved Camera's profile was found but has no [EVENTS] bScripted key, so "
					"its setting is unknown. If conversations fight the camera in first person, "
					"set it to 0."sv);
				return;
			}

			if (scripted == 0) {
				Log::Info(Log::Category::kCompat,
					"Improved Camera's [EVENTS] bScripted is off. It leaves the camera alone "
					"during dialogue, and the two mods will not fight."sv);
				return;
			}

			Log::Warn(Log::Category::kCompat,
				"Improved Camera's [EVENTS] bScripted is ON. Skyrim disables movement controls "
				"during dialogue, which Improved Camera reads as a scripted third-person event, "
				"so IF YOU PLAY IN FIRST PERSON it will pin the zoom and pull the camera back to "
				"your head on every line while Scene Director is staging the shot. The view "
				"flips between the two. Setting bScripted=0 fixes it. Note what that costs: the "
				"key is Improved Camera's whole scripted-forced-third-person category, not a "
				"dialogue switch, so its first-person handling goes for every event in that "
				"category. Dialogue is the one that conflicts here."sv);

			// The path, because the file is not the one named after the mod and a
			// player told to edit "the Improved Camera ini" will open the wrong one.
			//
			// Log::Utf8, never path::string() — see the note on Utf8. This path is
			// under MO2's virtual Data folder, which is exactly where the non-Latin
			// crash came from last time.
			Log::Warn(Log::Category::kCompat, "The file to edit is: {}"sv, Log::Utf8(profile));
		}
	}

	void ImprovedCamera::Detect()
	{
		if (asked) {
			return;
		}
		asked = true;

		for (const auto* module : kModules) {
			if (::GetModuleHandleW(module) != nullptr) {
				present = true;
				matched = module;
				break;
			}
		}

		if (!present) {
			return;
		}

		// Said in full at startup, once, so the next report of this arrives already
		// diagnosed rather than as "the camera zooms in and out".
		Log::Info(Log::Category::kCompat,
			"Improved Camera is loaded. The third-person zoom will not be written back at the "
			"end of conversations: that zoom is how Improved Camera moves between first and "
			"third person, and a second author on it is what makes the view pump."sv);

		ReportScriptedEvent();
	}

	bool ImprovedCamera::Present() noexcept
	{
		return present;
	}
}

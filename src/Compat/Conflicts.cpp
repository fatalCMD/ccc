#include "SD/Compat/Conflicts.h"

#include "SD/Core/Logging.h"

namespace SD::Compat
{
	namespace
	{
		struct Known
		{
			const wchar_t*   module;
			std::string_view name;
			std::string_view effect;
			bool             fatal;
		};

		// Detected by loaded module rather than by plugin handle. Handles are
		// assigned in load order and mean nothing to a reader; a file name is
		// something a user can find in their mod manager.
		constexpr std::array kKnown{
			Known{ L"AlternateConversationCamera.dll", "Improved Alternate Conversation Camera"sv,
				"takes SmoothCam's camera during dialogue, so Scene Director will decline to stage every conversation"sv,
				true },
			Known{ L"CameraShake.dll", "Camera Noise"sv,
				"adds procedural shake on top of a directed pose, which makes composition impossible to judge"sv,
				false },
		};
	}

	void ReportKnownConflicts()
	{
		bool anyFatal = false;

		for (const auto& entry : kKnown) {
			if (::GetModuleHandleW(entry.module) == nullptr) {
				continue;
			}

			if (entry.fatal) {
				anyFatal = true;
				Log::Error(Log::Category::kCompat, "CONFLICT: {} is loaded — it {}. Disable it."sv,
					entry.name, entry.effect);
			} else {
				Log::Warn(Log::Category::kCompat, "{} is loaded — it {}."sv, entry.name, entry.effect);
			}
		}

		if (!anyFatal) {
			Log::Info(Log::Category::kCompat, "No blocking camera conflicts detected."sv);
		}
	}
}

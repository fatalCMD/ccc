#pragma once

namespace SD::Log
{
	enum class Category : std::uint8_t
	{
		kCore = 0,
		kDialogue,   // session edges, line starts, topic info
		kStaging,    // participants, composition solving
		kCamera,     // pose override, interpolation, collision
		kContinuity, // cut policy, the 180 line, shot selection
		kCompat,     // SmoothCam, IACC, other camera owners
		kRender,     // the letterbox layer: Present hook, device state
		kCount
	};

	[[nodiscard]] std::string_view Name(Category a_category) noexcept;

	// Per-frame code paths must report once, not every frame.
	class OnceFlag
	{
	public:
		[[nodiscard]] bool Take() noexcept { return !taken.exchange(true, std::memory_order_relaxed); }
		void Reset() noexcept { taken.store(false, std::memory_order_relaxed); }

	private:
		std::atomic_bool taken{ false };
	};

	template <class... Args>
	void Info(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::info("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Warn(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::warn("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Error(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::error("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Debug(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::debug("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	// Wide to UTF-8, for a path or any other wide text that is going to be READ in
	// a log line rather than opened.
	//
	// NEVER std::filesystem::path::string() for this. That converts through the
	// user's ANSI codepage and THROWS on anything the codepage cannot represent,
	// which is a crash this mod has already had once — from a module path under a
	// non-Latin folder, on a machine where the log directory itself was clean. See
	// LogLoadedModule, which is where that was found, and Compat::ImprovedCamera,
	// which prints another mod's ini path for the player to go and edit.
	//
	// Empty in or an unconvertible string gives empty out. It cannot throw: a
	// logger must never be able to take down the thing it reports on.
	[[nodiscard]] std::string Utf8(std::wstring_view a_text);

	void Setup(std::string_view a_pluginName);

	// Records the file Windows actually loaded. With more than one build of a
	// mod installed, "which DLL is running" is otherwise guesswork.
	void LogLoadedModule();
}

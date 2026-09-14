#include "SD/Core/Logging.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace SD::Log
{
	std::string_view Name(Category a_category) noexcept
	{
		switch (a_category) {
		case Category::kCore:       return "Core"sv;
		case Category::kDialogue:   return "Dialogue"sv;
		case Category::kStaging:    return "Staging"sv;
		case Category::kCamera:     return "Camera"sv;
		case Category::kContinuity: return "Continuity"sv;
		case Category::kCompat:     return "Compat"sv;
		case Category::kRender:     return "Render"sv;
		case Category::kCount:
		default:                    return "Unknown"sv;
		}
	}

	namespace
	{
		// NON-ASCII PATHS, AND THE CRASH THEY CAUSED FOR EVERY JAPANESE PLAYER.
		//
		// std::filesystem::path::string() converts wide to narrow through the
		// system ANSI codepage and THROWS std::system_error when a character has no
		// mapping — "No mapping for the Unicode character exists in the target
		// multi-byte code page". On a Japanese, Chinese or Korean Windows, the log
		// directory is under the user's Documents folder and the user folder is
		// very often their name in their own script, so the very first thing this
		// plugin did on startup was throw. Before any hook, any conversation, any
		// camera work. Reported 2026-08, and the same call had already been found
		// and fixed once in Config.cpp for the settings path — see the long note
		// there. This is the same bug in the one place that pass did not reach.
		//
		// Nothing here can throw now, and that is the more important half of the
		// fix: a logger is diagnostic equipment. It must never be able to take down
		// the thing it is supposed to be reporting on, whatever it finds on disk.

		// A narrow path the ANSI file APIs can actually OPEN, or empty if there
		// isn't one.
		//
		// UTF-8 is the right answer for text and the wrong answer here. spdlog in
		// this build is a COMPILED library whose filename_t is std::string, so its
		// file sink opens with the narrow CRT call, which interprets the name in
		// the ANSI codepage — hand it UTF-8 and it does not throw, it just fails to
		// find the file. Defining SPDLOG_WCHAR_FILENAMES would fix that properly
		// and cannot be done from here: it changes filename_t, and the library was
		// compiled without it.
		//
		// So: ask whether the ANSI form round-trips, and if it does not, fall back
		// to the 8.3 short name, which is ASCII by construction. That is what makes
		// a log land at all in a folder this process cannot otherwise name.
		[[nodiscard]] std::string AnsiPath(const std::wstring& a_path)
		{
			if (a_path.empty()) {
				return {};
			}

			const auto narrow = [](const std::wstring& a_wide) -> std::string {
				BOOL      lossy = FALSE;
				const int needed = ::WideCharToMultiByte(CP_ACP, 0, a_wide.c_str(),
					static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
				if (needed <= 0) {
					return {};
				}

				std::string out(static_cast<std::size_t>(needed), '\0');
				::WideCharToMultiByte(CP_ACP, 0, a_wide.c_str(), static_cast<int>(a_wide.size()),
					out.data(), needed, nullptr, &lossy);

				// `lossy` is the whole test. Any substituted character means the
				// name no longer addresses the file we were given, and opening it
				// would either fail or — worse — create a second file beside the
				// real one under a mangled name.
				return lossy ? std::string{} : out;
			};

			if (auto direct = narrow(a_path); !direct.empty()) {
				return direct;
			}

			// The 8.3 name. Requires the target to exist, which the DIRECTORY does
			// by this point and the log file may not — so the directory is shortened
			// and the filename, which this plugin chose and is ASCII, is put back.
			const std::filesystem::path full{ a_path };
			const std::wstring          parent = full.parent_path().wstring();
			if (parent.empty()) {
				return {};
			}

			const DWORD needed = ::GetShortPathNameW(parent.c_str(), nullptr, 0);
			if (needed == 0) {
				return {};  // 8.3 generation disabled on this volume
			}

			std::wstring shortParent(needed, L'\0');
			const DWORD  written = ::GetShortPathNameW(parent.c_str(), shortParent.data(), needed);
			if (written == 0 || written >= needed) {
				return {};
			}
			shortParent.resize(written);

			return narrow(shortParent + L'\\' + full.filename().wstring());
		}
	}

	std::string Utf8(std::wstring_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}

		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text.data(),
			static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		if (needed <= 0) {
			return {};
		}

		std::string out(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	void Setup(std::string_view a_pluginName)
	{
		auto path = logger::log_directory();
		if (!path) {
			// NOT report_and_fail, which puts up a message box and terminates.
			//
			// That is for something the plugin cannot run without, and this is a
			// log file. spdlog keeps a default logger when none is installed, so
			// every call site stays valid and the mod runs; the player loses the
			// diagnostics, not the session.
			return;
		}

		*path /= fmt::format("{}.log"sv, a_pluginName);

		// Created before the short-name lookup below, which can only shorten a
		// directory that exists.
		std::error_code error;
		std::filesystem::create_directories(path->parent_path(), error);

		// Keep one generation back. The log is truncated on every launch, which is
		// fine right up until the thing being diagnosed is a crash: the player
		// relaunches to check something and the record of the failed session is
		// gone. One extra file is the difference between reading what happened and
		// asking them to reproduce it.
		auto previous = *path;
		previous.replace_extension(".previous.log");
		std::filesystem::remove(previous, error);
		std::filesystem::rename(*path, previous, error);

		const std::string openable = AnsiPath(path->wstring());
		if (openable.empty()) {
			return;  // nowhere to write that this process can name. Not fatal.
		}

		// spdlog throws spdlog_ex when a file will not open, and a log that cannot
		// be created is still not a reason to lose the mod.
		std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
		try {
			sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(openable, true);
		} catch (...) {
			return;
		}

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
		const auto level = spdlog::level::trace;
#else
		const auto level = spdlog::level::info;
#endif
		log->set_level(level);
		log->flush_on(level);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v"s);
	}

	void LogLoadedModule()
	{
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LogLoadedModule),
				&module) ||
			!module) {
			return;
		}

		std::array<wchar_t, MAX_PATH> path{};
		const auto length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0 || length >= path.size()) {
			return;
		}

		// Utf8, not path::string(). This line is the second half of the same
		// crash: the module path is where MO2's virtual Data folder lives, and an
		// install under a non-ASCII folder threw here even when the log directory
		// itself was clean. It is text for a human to read, so UTF-8 is exactly
		// right — and it cannot throw.
		Info(Category::kCore, "Loaded from: {}"sv, Utf8(std::wstring{ path.data(), length }));
	}
}

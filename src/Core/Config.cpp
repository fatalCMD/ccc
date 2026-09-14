#include "SD/Core/Config.h"

#include "SD/Core/Logging.h"

namespace SD::Config
{
	namespace
	{
		// Absolute, derived from the running exe, rather than the relative paths
		// these used to be.
		//
		// GetPrivateProfile*/WritePrivateProfile* are documented to search the
		// *Windows* directory when handed anything that is not a full path. Reads
		// happened to work because the shipped ini's values match the built-in
		// defaults exactly, so a silent fallback and a successful read produce
		// identical behaviour and neither could be told from the other. Writes had
		// no such cover. Anchoring to the exe removes the ambiguity for both.
		//
		// WIDE THROUGHOUT, and that is the whole of a "settings no longer save" bug.
		//
		// This used to take the wide path from GetModuleFileNameW and immediately
		// narrow it with std::filesystem::path::string(), which converts to the
		// system ANSI codepage — then hand the result to the ...A profile calls. On
		// an ASCII install path that is invisible. On any install path carrying a
		// character the user's ANSI codepage cannot represent — an accented or
		// non-Latin Windows username is enough, and "D:\Games" is not the only place
		// people keep Skyrim — the conversion mangles the path, and every read and
		// every write then addresses a file that does not exist.
		//
		// The failure is total and silent in exactly the reported shape: reads miss,
		// so every setting falls back to its built-in default; writes go nowhere, so
		// nothing the player changes survives a restart. It reads as "the mod
		// ignores my ini" rather than as an encoding fault, which is why it earned
		// this comment instead of a one-line change.
		//
		// So the path stays wide from GetModuleFileNameW to the profile API and is
		// never round-tripped through a narrow string. Sections and keys are ASCII
		// by construction, but they are widened properly rather than cast.
		// Grown until it fits, rather than one MAX_PATH attempt.
		//
		// GetModuleFileNameW truncates instead of failing: it fills the buffer, sets
		// ERROR_INSUFFICIENT_BUFFER, and returns the size it was given. The previous
		// shape treated that as unrecoverable and fell back to ".", which is the one
		// value that must never be used here — a relative path sends the profile API
		// looking in the WINDOWS directory, so every read misses and every write is
		// either refused or lands in a file nothing will ever read.
		//
		// 260 characters is not a lot for a Skyrim install. A OneDrive-redirected
		// Documents folder under a long user name reaches it on its own, and this
		// machine's own Documents path is exactly that shape. Anyone past the limit
		// lost every setting, silently, and would have had no way to tell.
		[[nodiscard]] const std::wstring& GameRoot()
		{
			static const std::wstring root = [] {
				std::vector<wchar_t> buffer(MAX_PATH);
				for (;;) {
					const auto length = ::GetModuleFileNameW(nullptr, buffer.data(),
						static_cast<DWORD>(buffer.size()));
					if (length == 0) {
						return std::wstring{};  // caller reports; "." would be worse
					}
					if (length < buffer.size()) {
						return std::filesystem::path{ buffer.data() }.parent_path().wstring();
					}
					if (buffer.size() >= 32768) {
						return std::wstring{};  // past the extended-path ceiling
					}
					buffer.resize(buffer.size() * 2);
				}
			}();
			return root;
		}

		[[nodiscard]] const wchar_t* McmPath()
		{
			static const std::wstring path = GameRoot() + L"\\Data\\MCM\\Settings\\SceneDirector.ini";
			return path.c_str();
		}

		// The SHIPPED file: defaults and documentation. Replaced by every update.
		[[nodiscard]] const wchar_t* OwnPath()
		{
			static const std::wstring path = GameRoot() + L"\\Data\\SKSE\\Plugins\\SD.ini";
			return path.c_str();
		}

		// The USER's file, which the menu writes and no release ever ships.
		//
		// SD.ini used to be both of these at once, and that is the whole of "the new
		// version no longer saves my weight, FOV and zoom settings".
		//
		// Those three dials are written by the in-game menu and are deliberately not
		// listed in the shipped ini — it documents them in a comment instead — so
		// they were the only settings that existed purely as user data. Every other
		// key survives a rebuild of SD.ini looking untouched, because the shipped
		// value it comes back as is the same sane default it started from. The
		// per-shot keys do not come back at all. Anything that rewrites SD.ini from
		// the shipped copy therefore deletes exactly those and nothing else, which
		// is precisely the report: measured on a real tuning session, 93 keys in,
		// the same 14 gone. Installing an update over the top does this to every
		// user, once per release.
		//
		// So the two roles are separated. Reads fall MCM -> user -> shipped ->
		// built-in, writes go here, and an update may overwrite SD.ini as freely as
		// it likes without touching anything the player chose.
		//
		// No migration step is needed and none should be added. An existing install
		// has its values in SD.ini and an empty (or absent) user file, so SD.ini
		// still answers every read exactly as before; the first time a key is
		// changed in the menu it moves here and takes precedence from then on.
		// Hand-editing SD.ini also keeps working, for every key the menu has not
		// been used on.
		[[nodiscard]] const wchar_t* UserPath()
		{
			static const std::wstring path = GameRoot() + L"\\Data\\SKSE\\Plugins\\SD_user.ini";
			return path.c_str();
		}

		// THE SAME ROOT, FOR A FILE THIS MOD DOES NOT OWN. See Config.h.
		//
		// Not one of the three above, because those are fixed and this is not: it
		// exists so Compat::ImprovedCamera can read Improved Camera's profile and
		// report the one setting the two mods cannot share. Kept here rather than
		// copied over there so there is one answer to "where is the game", which is
		// the question the whole of the comment on GameRoot is about.
		[[nodiscard]] std::wstring DataPathImpl(std::wstring_view a_relative)
		{
			const auto& root = GameRoot();
			if (root.empty() || a_relative.empty()) {
				return {};
			}

			std::wstring path = root;
			path += L"\\Data\\";
			path += a_relative;
			return path;
		}

		// Section and key names, which are ASCII string literals at every call site.
		// Converted rather than cast so a non-ASCII byte would widen correctly
		// instead of silently truncating.
		[[nodiscard]] std::wstring Widen(const char* a_text)
		{
			if (!a_text || !*a_text) {
				return {};
			}

			const int needed = ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, nullptr, 0);
			if (needed <= 1) {
				return {};
			}

			std::wstring out(static_cast<std::size_t>(needed) - 1, L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, out.data(), needed);
			return out;
		}

		// Back to UTF-8 for String() and for the log. Values here are ASCII in
		// practice — sApply=classical and the like — but a path in a log line is
		// not, and that is the one place this has to be right.
		[[nodiscard]] std::string Narrow(const wchar_t* a_text, int a_length = -1)
		{
			if (!a_text || !*a_text) {
				return {};
			}

			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text, a_length,
				nullptr, 0, nullptr, nullptr);
			if (needed <= 0) {
				return {};
			}

			std::string out(static_cast<std::size_t>(needed), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_text, a_length, out.data(), needed, nullptr, nullptr);
			if (a_length == -1 && !out.empty() && out.back() == '\0') {
				out.pop_back();
			}
			return out;
		}

		// THREE BYTES THAT SILENTLY DELETE THE FIRST SECTION OF A SETTINGS FILE.
		//
		// GetPrivateProfile* recognises a UTF-16 byte order mark and no other. A
		// UTF-8 one — EF BB BF — is left in place as content, so it becomes part of
		// line one.
		//
		// WHICH LINE THAT IS DECIDES HOW MUCH IS LOST, and the scope is narrower
		// than it first looks. Measured, on files differing only in their first
		// line:
		//
		//   [Section] on line 1  -> that section never matches. Every key in it
		//                           falls through to the caller's default. Sections
		//                           after it parse normally.
		//   comment on line 1    -> the mark is absorbed by the comment. The whole
		//                           file reads correctly.
		//   blank line 1         -> likewise, reads correctly.
		//
		// So SD.ini is immune as it ships: its first line is "; Scene Director" and
		// its first section header is twenty lines down. SD_user.ini is NOT.
		// WritePrivateProfileString creates a file whose very first bytes are the
		// section header — verified, "[Directi..." — so a mark on the user file
		// loses whichever section is first, which is [Direction], which is most of
		// the dials. [Shots] and [CustomPresets] below it survive.
		//
		// That is the shape worth naming, because it is the confusing one: not "the
		// file does nothing" but "some of my settings revert and others do not",
		// with no pattern the player can see and nothing in the log to explain it.
		// Nothing errors, the file is plainly there, and ReportSource below reports
		// it present.
		//
		// Not hypothetical, though it needs a hand edit to happen at all: a player
		// chasing settings that will not stick opens SD_user.ini to look, and
		// Notepad++ and VS Code will both add a mark on save if that is how they
		// are configured. Being a per-editor setting is exactly why a thing like
		// this reaches SOME players and not others.
		//
		// A write does not rescue it either. WritePrivateProfileString cannot see
		// the shadowed header, so it APPENDS A SECOND [Direction] rather than
		// updating the first — also verified — and every value in the original is
		// stranded above it for good. Which is why EnsureRepaired is called from
		// the writers as well as the readers.
		//
		// Repaired rather than reported, and that is a choice about which failure
		// is kinder. A warning leaves the player holding a broken file and a log
		// line they will not find, and there is nothing delicate to preserve: a
		// leading UTF-8 mark in an ini is never intentional and never carries
		// meaning. Both files are checked, SD.ini included — it is immune only by
		// the accident of what is on its first line, and that is too thin a thing
		// to leave a known-broken encoding standing on.

		// Empty on any failure at all, which the caller reads as "nothing to
		// repair". A file that cannot be read is not one to start rewriting.
		//
		// The size ceiling is not defensive padding. A settings ini is twenty
		// kilobytes; anything past this is not one, and the cap is what stops a
		// wrong or corrupt path turning plugin load into a large allocation.
		[[nodiscard]] std::optional<std::string> ReadWholeFile(const wchar_t* a_path)
		{
			constexpr LONGLONG kSaneCeiling = 8LL * 1024LL * 1024LL;

			const HANDLE file = ::CreateFileW(a_path, GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return std::nullopt;
			}

			LARGE_INTEGER size{};
			if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > kSaneCeiling) {
				::CloseHandle(file);
				return std::nullopt;
			}

			std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
			DWORD       read = 0;
			const bool  ok = ::ReadFile(file, data.data(), static_cast<DWORD>(data.size()),
								 &read, nullptr) != FALSE &&
							read == data.size();
			::CloseHandle(file);

			if (!ok) {
				return std::nullopt;
			}
			return data;
		}

		[[nodiscard]] bool WriteWholeFile(const wchar_t* a_path, const void* a_data, std::size_t a_size)
		{
			const HANDLE file = ::CreateFileW(a_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}

			DWORD      written = 0;
			const bool ok = a_size == 0 ||
							(::WriteFile(file, a_data, static_cast<DWORD>(a_size), &written, nullptr) != FALSE &&
								written == a_size);
			::CloseHandle(file);
			return ok;
		}

		void RepairByteOrderMark(const wchar_t* a_path)
		{
			const auto content = ReadWholeFile(a_path);
			if (!content) {
				return;
			}

			constexpr std::string_view kUtf8Mark = "\xEF\xBB\xBF"sv;

			const std::string_view whole{ *content };
			if (!whole.starts_with(kUtf8Mark)) {
				return;
			}

			const std::string_view rest = whole.substr(kUtf8Mark.size());

			// ASCII STAYS ASCII, and that is a decision rather than the lazy path.
			//
			// Dropping three bytes leaves a file byte-identical to the one the
			// player is looking at in their editor, leaves it the plain text every
			// other tool that touches a Skyrim ini expects, and leaves Deploy's
			// merge reading exactly what it has always read.
			//
			// A file with real non-ASCII in it cannot be repaired that way. With
			// the mark gone the profile API reads it through the legacy codepage,
			// which is the precise conversion 1.3.4 was spent removing, and a
			// preset slot the player named in Japanese would come back mojibake.
			// That file is rewritten UTF-16LE instead, which the API reads natively
			// and losslessly and which WritePrivateProfileStringW will then go on
			// writing in the same encoding.
			const bool ascii = std::none_of(rest.begin(), rest.end(), [](char a_ch) {
				return static_cast<unsigned char>(a_ch) >= 0x80u;
			});

			bool        written = false;
			std::string how;

			if (ascii) {
				written = WriteWholeFile(a_path, rest.data(), rest.size());
				how = "removed it";
			} else {
				const int needed = rest.empty() ? 0 :
												  ::MultiByteToWideChar(CP_UTF8, 0, rest.data(),
													  static_cast<int>(rest.size()), nullptr, 0);
				if (needed > 0) {
					std::wstring wide(static_cast<std::size_t>(needed), L'\0');
					::MultiByteToWideChar(CP_UTF8, 0, rest.data(), static_cast<int>(rest.size()),
						wide.data(), needed);

					std::wstring out;
					out.reserve(wide.size() + 1);
					// The UTF-16 mark, which the API DOES read. Written as a number
					// rather than as the character it stands for, because the
					// character is invisible: a literal one in this file would be
					// three bytes nobody reviewing this line could see, which is
					// the same joke the bug is made of.
					out.push_back(static_cast<wchar_t>(0xFEFFu));
					out.append(wide);

					written = WriteWholeFile(a_path, out.data(), out.size() * sizeof(wchar_t));
				}
				how = "converted the file to UTF-16, because it contains non-ASCII text";
			}

			if (written) {
				Log::Warn(Log::Category::kCore,
					"{} began with a UTF-8 byte order mark. Windows does not recognise one in an "
					"ini, so if a [Section] header was the first line of the file, that section "
					"and every setting in it was being ignored. Repaired: {}. If you edit this "
					"file by hand, save it as ANSI or as UTF-8 WITHOUT a byte order mark."sv,
					Narrow(a_path), how);
			} else {
				Log::Error(Log::Category::kCore,
					"{} begins with a UTF-8 byte order mark and could not be rewritten "
					"(error {}). Windows does not recognise one in an ini, so if a [Section] "
					"header is the first line of the file, that section and every setting in it "
					"is being ignored. Re-save it as ANSI, or as UTF-8 without a byte order "
					"mark."sv,
					Narrow(a_path), ::GetLastError());
			}
		}

		// Once, and before anything can read or write.
		//
		// Hung off the readers rather than called from Runtime, because the order
		// those two run in is not this file's to depend on: ReportSource happens to
		// be the first thing Runtime asks for today, and a read landing before the
		// repair would answer from built-in defaults and be indistinguishable from
		// a correct answer. A magic static costs one atomic load per read after the
		// first and cannot be got out of order by anything.
		//
		// SD's own two files only. The MCM file belongs to MCM Helper, which writes
		// it itself and will not put a UTF-8 mark in it — and rewriting another
		// mod's settings file on the strength of a guess about its encoding is not
		// this plugin's business.
		void EnsureRepaired()
		{
			static const bool once = [] {
				if (!GameRoot().empty()) {
					RepairByteOrderMark(OwnPath());
					RepairByteOrderMark(UserPath());
				}
				return true;
			}();
			static_cast<void>(once);
		}

		// A sentinel no real setting uses, so "absent" can be told apart from a
		// legitimate zero. GetPrivateProfileInt cannot otherwise distinguish them,
		// and treating a missing MCM key as 0 would silently disable features for
		// anyone whose MCM had not been opened yet.
		constexpr int kAbsent = -999999;

		Log::OnceFlag sourceReported;

		[[nodiscard]] bool McmPresent()
		{
			return ::GetFileAttributesW(McmPath()) != INVALID_FILE_ATTRIBUTES;
		}

		SettingsCache& Cache()
		{
			static SettingsCache cache;
			return cache;
		}

		SettingsCache::Revision Revisions()
		{
			EnsureRepaired();
			SettingsCache::Revision revision{};
			const std::array paths{ McmPath(), UserPath(), OwnPath() };
			// A failed metadata query must not make stale values look unchanged.
			static std::uint32_t failedProbe = 0;
			for (std::size_t i = 0; i < paths.size(); ++i) {
				WIN32_FILE_ATTRIBUTE_DATA data{};
				if (::GetFileAttributesExW(paths[i], GetFileExInfoStandard, &data)) {
					revision[i] = { data.dwFileAttributes, data.ftLastWriteTime.dwHighDateTime,
						data.ftLastWriteTime.dwLowDateTime, data.nFileSizeHigh, data.nFileSizeLow, 0 };
				} else {
					const auto error = ::GetLastError();
					if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
						revision[i][5] = ++failedProbe;
					}
				}
			}
			return revision;
		}
	}

	ReadScope::ReadScope() : scope(Cache(), Revisions) {}

	std::wstring DataPath(std::wstring_view a_relative)
	{
		return DataPathImpl(a_relative);
	}

	int Int(const char* a_section, const char* a_key, int a_default)
	{
		return Cache().Int(a_section ? a_section : "", a_key ? a_key : "", a_default, [&] {
			EnsureRepaired();

			const auto section = Widen(a_section);
			const auto key = Widen(a_key);

			if (McmPresent()) {
				const int fromMcm = ::GetPrivateProfileIntW(section.c_str(), key.c_str(),
					kAbsent, McmPath());
				if (fromMcm != kAbsent) {
					return fromMcm;
				}
			}

			// The player's own choice outranks the shipped file. See UserPath.
			const int fromUser = ::GetPrivateProfileIntW(section.c_str(), key.c_str(),
				kAbsent, UserPath());
			if (fromUser != kAbsent) {
				return fromUser;
			}

			const int fromOwn = ::GetPrivateProfileIntW(section.c_str(), key.c_str(),
				kAbsent, OwnPath());
			return fromOwn != kAbsent ? fromOwn : a_default;
		});
	}

	bool Bool(const char* a_section, const char* a_key, bool a_default)
	{
		return Int(a_section, a_key, a_default ? 1 : 0) != 0;
	}

	namespace
	{
		// Reads one key, growing the buffer until the value actually fits.
		//
		// GetPrivateProfileString signals truncation by returning exactly size-1 and
		// gives no other warning, so a fixed buffer silently hands back a prefix.
		// That is harmless for every short value this file has ever read and fatal
		// for a saved preset slot, which is several hundred characters of numbers:
		// at the old 128 the value came back cut in half, parsed short, and the slot
		// refused to apply with a message blaming its shot count.
		//
		// Starts small because almost every caller is a word or two, and quadruples
		// rather than creeping so a long value costs one extra read rather than
		// twenty. The cap exists so a corrupt file cannot turn this into an
		// allocation loop.
		[[nodiscard]] std::wstring ReadProfileString(const wchar_t* a_section, const wchar_t* a_key,
			const wchar_t* a_default, const wchar_t* a_path)
		{
			std::vector<wchar_t> buffer(256);
			for (int attempt = 0; attempt < 6; ++attempt) {
				const auto length = ::GetPrivateProfileStringW(a_section, a_key, a_default,
					buffer.data(), static_cast<DWORD>(buffer.size()), a_path);

				if (length + 1 < buffer.size()) {
					return std::wstring{ buffer.data(), length };
				}
				buffer.resize(buffer.size() * 4);
			}
			return std::wstring{ buffer.data(), buffer.size() - 1 };
		}
	}

	std::string String(const char* a_section, const char* a_key, const char* a_default)
	{
		return Cache().String(a_section ? a_section : "", a_key ? a_key : "", a_default ? a_default : "", [&] {
			EnsureRepaired();

			// Same MCM-then-own precedence as Int. The sentinel trick is unnecessary
			// here: an absent key returns the default string, and there is no value a
			// caller could legitimately want that is indistinguishable from absence.
			const auto section = Widen(a_section);
			const auto key = Widen(a_key);

			if (McmPresent()) {
				const auto value = ReadProfileString(section.c_str(), key.c_str(), L"", McmPath());
				if (!value.empty()) {
					return Narrow(value.c_str(), static_cast<int>(value.size()));
				}
			}

			if (const auto value = ReadProfileString(section.c_str(), key.c_str(), L"", UserPath());
				!value.empty()) {
				return Narrow(value.c_str(), static_cast<int>(value.size()));
			}

			const auto fallback = Widen(a_default);
			const auto value =
				ReadProfileString(section.c_str(), key.c_str(), fallback.c_str(), OwnPath());
			return Narrow(value.c_str(), static_cast<int>(value.size()));
		});
	}

	void SetString(const char* a_section, const char* a_key, const char* a_value)
	{
		const auto write = Cache().Write();
		// Before the write, not only before reads. A write into a file with a UTF-8
		// mark still on it appends a DUPLICATE section rather than updating the one
		// already there — the API cannot see the original — so the file half-heals
		// and strands every value above the new section for good.
		EnsureRepaired();

		const auto section = Widen(a_section);
		const auto key = Widen(a_key);
		const auto value = Widen(a_value);

		if (!::WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), UserPath())) {
			Log::Warn(Log::Category::kCore, "Could not write [{}] {}={} to {} (error {})."sv,
				a_section, a_key, a_value, Narrow(UserPath()), ::GetLastError());
			return;
		}
		::WritePrivateProfileStringW(nullptr, nullptr, nullptr, UserPath());
	}

	void SetInt(const char* a_section, const char* a_key, int a_value)
	{
		const auto write = Cache().Write();
		// See SetString: a write lands in a duplicate section if the mark is still
		// there, which is worse than the read failure it comes with.
		EnsureRepaired();

		// Written to SD_user.ini, never to the MCM file.
		//
		// The MCM path is somebody else's to own — MCM Helper rewrites it wholesale
		// from its own state, so anything put there would be lost the next time a
		// menu was opened. SD_user.ini is this mod's own and is safe to edit in
		// place; SD.ini is not written either, because an update replaces it.
		//
		// Note the consequence: Int() consults MCM first, so if an MCM ever exists
		// and holds this key, a value written here will be read back shadowed. That
		// is correct precedence — the player's menu choice should beat a file — but
		// it means the in-game menu and an MCM must not both be live for the same
		// key without one deferring to the other.
		wchar_t text[32]{};
		std::swprintf(text, std::size(text), L"%d", a_value);

		const auto section = Widen(a_section);
		const auto key = Widen(a_key);

		if (!::WritePrivateProfileStringW(section.c_str(), key.c_str(), text, UserPath())) {
			Log::Warn(Log::Category::kCore, "Could not write [{}] {}={} to {} (error {})."sv,
				a_section, a_key, a_value, Narrow(UserPath()), ::GetLastError());
			return;
		}

		// Flush the profile cache. Windows buffers these writes and will happily
		// report success while the file on disk is unchanged, which is
		// indistinguishable from a setting that refuses to save.
		::WritePrivateProfileStringW(nullptr, nullptr, nullptr, UserPath());

		// Then read it straight back — THROUGH Int(), not off OwnPath directly.
		//
		// Two failures hide here and the narrower check only caught one.
		//
		// The first is the write not landing at all. This runs under Mod Organizer's
		// virtual filesystem, where Data\SKSE\Plugins is a mapped view rather than a
		// real directory, and a redirected write can succeed against a copy the next
		// read never sees.
		//
		// The second is the write landing and being SHADOWED. Int() consults the MCM
		// file first and this only ever writes SD.ini, so with an MCM present and
		// holding the key, the write succeeds, a read off SD.ini agrees with it, and
		// the value the mod actually uses never changes. Verified against OwnPath
		// that reads as healthy; verified against Int() it reads as what the player
		// is experiencing, which is a setting that will not stick.
		//
		// Costs one profile read per slider release, and turns either failure into a
		// line in the log instead of an afternoon.
		const int effective = Int(a_section, a_key, kAbsent);
		if (effective != a_value) {
			Log::Error(Log::Category::kCore,
				"Wrote [{}] {}={} to {} but the value in force is {}. {}"sv,
				a_section, a_key, a_value, Narrow(UserPath()),
				effective == kAbsent ? std::string{ "nothing" } : std::to_string(effective),
				McmPresent() ?
					"An MCM settings file exists and takes precedence over SD.ini, so it is "
					"shadowing this write — change the setting in the MCM, or remove "
					"Data/MCM/Settings/SceneDirector.ini."sv :
					"The file is not being persisted — check that it is writable and not "
					"locked by a mod manager."sv);
		}
	}

	void SetBool(const char* a_section, const char* a_key, bool a_value)
	{
		SetInt(a_section, a_key, a_value ? 1 : 0);
	}

	void ReportSource()
	{
		if (!sourceReported.Take()) {
			return;
		}

		// So the repair's own line lands ABOVE the settings-source line rather than
		// below it. A reader working out why a setting did nothing reads this file
		// top down, and "SD.ini present, reading from ..." directly above "and by
		// the way none of it was readable" is a log that answers itself.
		EnsureRepaired();

		if (GameRoot().empty()) {
			Log::Error(Log::Category::kCore,
				"Could not resolve the game directory from the running executable, so every "
				"settings path is relative and nothing will read or write correctly. No setting "
				"will persist in this session."sv);
		}

		const bool mcm = McmPresent();
		const bool own = ::GetFileAttributesW(OwnPath()) != INVALID_FILE_ATTRIBUTES;
		const bool user = ::GetFileAttributesW(UserPath()) != INVALID_FILE_ATTRIBUTES;

		// The resolved path is printed, not just present/absent.
		//
		// "SD.ini absent" on a machine where the file is plainly there is the whole
		// diagnosis of a path that resolved somewhere unexpected, and without the
		// path in the log there is no way to tell that from a missing install. This
		// line is what the next "my settings do not save" report should be answered
		// with before anything else is guessed at.
		Log::Info(Log::Category::kCore,
			"Settings source: MCM {}, SD_user.ini {}, SD.ini {}. Reading from {}."sv,
			mcm ? "present"sv : "absent"sv,
			user ? "present"sv : "absent"sv,
			own ? "present"sv : "absent"sv,
			Narrow(OwnPath()));

		if (!mcm && !own && !user) {
			Log::Warn(Log::Category::kCore, "No settings file found; built-in defaults in use."sv);
		}

		// Both present is the configuration in which the in-game menu silently does
		// nothing: it writes SD.ini, and every read comes off the MCM file first.
		if (mcm && own) {
			Log::Warn(Log::Category::kCore,
				"Both an MCM settings file and SD.ini are present. The MCM wins every key it "
				"carries, so changes made in the in-game menu will not take effect for those "
				"keys. MCM file: {}"sv,
				Narrow(McmPath()));
		}
	}
}

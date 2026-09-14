#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

// Config's production logger only needs the category label. Avoid SKSE startup.
namespace SD::Log { std::string_view Name(Category) noexcept { return "ConfigTest"; } }

namespace {
	void Expect(bool value, const char* why)
	{
		if (!value) { std::cerr << why << '\n'; std::exit(EXIT_FAILURE); }
	}
	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		Expect(file.good(), "fixture write");
	}
	void NativeWrite(const std::filesystem::path& path, const wchar_t* section,
		const wchar_t* key, const wchar_t* value)
	{
		Expect(::WritePrivateProfileStringW(section, key, value, path.c_str()) != FALSE, "native INI write");
		::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
	}
}

int main()
{
	namespace Config = SD::Config;
	const std::filesystem::path own = Config::DataPath(L"SKSE\\Plugins\\SD.ini");
	const auto root = own.parent_path().parent_path().parent_path().parent_path();
	Expect(root.parent_path().filename() == L"config-integration", "only use the dedicated test output directory");
	const std::filesystem::path user = Config::DataPath(L"SKSE\\Plugins\\SD_user.ini");
	const std::filesystem::path mcm = Config::DataPath(L"MCM\\Settings\\SceneDirector.ini");
	for (const auto& file : { own, user, mcm }) {
		std::filesystem::create_directories(file.parent_path());
		std::filesystem::remove(file);
	}
	WriteText(own, "\xEF\xBB\xBF[Example]\r\nValue=11\r\nZero=0\r\nNegative=-17\r\nHex=0x2A\r\nText=\"quoted text\"\r\n");
	// Preserve the existing repair and Windows UTF-16 string behavior.
	WriteText(user, std::string("\xFF\xFE", 2));
	NativeWrite(user, L"Example", L"Value", L"22");
	NativeWrite(user, L"Example", L"Unicode", L"\u65E5\u672C");
	const auto verify = [&] {
		Expect(Config::Int("Example", "Value", 0) == 22, "user outranks shipped defaults");
		Expect(Config::Int("Example", "Zero", 99) == 0, "native zero");
		Expect(Config::Int("Example", "Negative", 99) == -17, "native negative");
		Expect(Config::Int("Example", "Hex", 99) == 42, "native hexadecimal");
		Expect(Config::Int("Missing", "Value", 7) == 7, "missing default 7");
		Expect(Config::Int("Missing", "Value", 9) == 9, "missing default 9");
		Expect(Config::String("Example", "Text", "") == "quoted text", "native quoted text");
		Expect(Config::String("Example", "Unicode", "") == "\xE6\x97\xA5\xE6\x9C\xAC", "UTF-16 value returned as UTF-8");
	};
	verify();
	{ Config::ReadScope scope; verify(); }
	{ Config::ReadScope scope; verify(); }
	NativeWrite(mcm, L"Example", L"Value", L"33");
	{
		Config::ReadScope scope;
		Expect(Config::Int("Example", "Value", 0) == 33, "new MCM file takes precedence");
		Config::SetInt("Example", "Value", 44);
		Expect(Config::Int("Example", "Value", 0) == 33, "MCM still shadows menu writes");
	}
	std::filesystem::remove(mcm);
	{
		Config::ReadScope scope;
		Expect(Config::Int("Example", "Value", 0) == 44, "deleted MCM reveals user value");
		Config::SetInt("Example", "Value", 55);
		Expect(Config::Int("Example", "Value", 0) == 55, "same-batch menu write visible");
		const std::string longPreset(4096, '8');
		Config::SetString("CustomPresets", "Slot", longPreset.c_str());
		Expect(Config::String("CustomPresets", "Slot", "") == longPreset, "long preset roundtrip");
		Config::SetString("CustomPresets", "Slot", "");
		Expect(Config::String("CustomPresets", "Slot", "fallback") == "fallback", "cleared preset uses fallback");
	}
	NativeWrite(user, L"Example", L"Value", L"666");
	{ Config::ReadScope scope; Expect(Config::Int("Example", "Value", 0) == 666, "external INI edit refreshes next batch"); }
	std::filesystem::remove(user);
	{ Config::ReadScope scope; Expect(Config::Int("Example", "Value", 0) == 11, "deleted user falls through to shipped file"); }
	NativeWrite(own, L"Example", L"Value", L"7777");
	{ Config::ReadScope scope; Expect(Config::Int("Example", "Value", 0) == 7777, "shipped INI edits refresh too"); }

	// Compare actual Config calls outside MO2. This is not an in-game benchmark.
	std::string fixture = "[Benchmark]\r\n";
	std::vector<std::string> keys;
	for (int i = 0; i < 600; ++i) {
		keys.push_back("iKey" + std::to_string(i));
		fixture += keys.back() + "=" + std::to_string(i) + "\r\n";
	}
	WriteText(own, fixture);
	const auto batch = [&] {
		int sum = 0;
		for (const auto& key : keys) sum += Config::Int("Benchmark", key.c_str(), -1);
		Expect(sum == 179700, "benchmark reads retain values");
	};
	const auto measure = [&](bool cached) {
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < 10; ++i) {
			if (cached) { Config::ReadScope scope; batch(); }
			else batch();
		}
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 10;
	};
	{ Config::ReadScope scope; batch(); }
	const auto uncached = measure(false), cached = measure(true);
	std::cout << "600 real Config reads: uncached " << uncached << "ms; warm batch " << cached
		<< "ms (outside Skyrim/MO2). Native INI integration passed.\n";
	for (const auto& file : { own, user, mcm }) std::filesystem::remove(file);
}

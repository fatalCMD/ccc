#include "SD/Core/SettingsCache.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
	void Expect(bool value, const char* why)
	{
		if (!value) { std::cerr << why << '\n'; std::exit(EXIT_FAILURE); }
	}
}

int main()
{
	using SD::Config::SettingsCache;
	SettingsCache cache;
	SettingsCache::Revision revision{};
	int probes = 0, reads = 0, value = 7;
	const auto probe = [&] { ++probes; return revision; };
	const auto read = [&] { ++reads; return value; };
	{
		SettingsCache::Scope scope(cache, probe);
		Expect(cache.Int("Shots", "iLens", 50, read) == 7, "initial native value");
		{
			SettingsCache::Scope nested(cache, probe);
			Expect(cache.Int("Shots", "iLens", 50, read) == 7, "nested cached value");
		}
		Expect(probes == 1 && reads == 1, "nested batches do no extra I/O");
	}
	for (int i = 0; i < 100; ++i) {
		SettingsCache::Scope scope(cache, probe);
		Expect(cache.Int("Shots", "iLens", 50, read) == 7, "unchanged conversation reuses value");
	}
	Expect(reads == 1 && probes == 101, "one revision probe and zero native reads per warm batch");
	value = 8;
	Expect(cache.Int("Shots", "iLens", 50, read) == 8, "unbatched readers stay live");
	Expect(cache.Int("Shots", "iLens", 50, read) == 8 && reads == 3, "unbatched reads are not memoized");

	// Changes to any source, including absent -> present -> absent, invalidate.
	for (std::size_t source = 0; source < revision.size(); ++source) {
		for (const auto stamp : { 1u, 2u, 0u }) {
			revision[source][0] = stamp;
			++value;
			SettingsCache::Scope scope(cache, probe);
			Expect(cache.Int("Shots", "iLens", 50, read) == value, "source changes invalidate cached reads");
		}
	}
	{
		SettingsCache::Scope scope(cache, probe);
		Expect(cache.Int("Missing", "Key", 10, [] { return 10; }) == 10, "missing integer default");
		Expect(cache.Int("Missing", "Key", 20, [] { return 20; }) == 20, "same missing key can have a different default");
		Expect(cache.Int("Other", "Key", 10, [] { return -12; }) == -12, "section separates negative native values");
		Expect(cache.Int("Other", "Zero", 10, [] { return 0; }) == 0, "zero is a valid native value");
		Expect(cache.String("Missing", "Key", "one", [] { return std::string("one"); }) == "one", "string default");
		Expect(cache.String("Missing", "Key", "two", [] { return std::string("two"); }) == "two", "string default is part of identity");
		const std::string preset = std::string(8192, '9') + "\xE6\x97\xA5\xE6\x9C\xAC";
		Expect(cache.String("Presets", "Slot", "", [&] { return preset; }) == preset, "long UTF-8 preset preserved");
		Expect(cache.String("Presets", "Slot", "", [] { return std::string("wrong"); }) == preset, "long string cache hit");
		Expect(cache.String("Empty", "Value", "", [] { return std::string{}; }).empty(), "empty values cached");
		{
			const auto write = cache.Write();
			value = 101;
			Expect(cache.Int("Shots", "iLens", 50, read) == 101, "write readback cannot use pre-write value");
		}
		Expect(cache.String("Presets", "Slot", "", [] { return std::string("new"); }) == "new", "writes invalidate strings too");
	}

	// A settings panel writer must wait until a game-thread batch is complete.
	std::future<void> writer;
	std::promise<void> attempting;
	std::atomic_bool wrote = false;
	{
		SettingsCache::Scope scope(cache, probe);
		writer = std::async(std::launch::async, [&] {
			attempting.set_value();
			const auto write = cache.Write();
			value = 202;
			wrote = true;
		});
		attempting.get_future().wait();
		Expect(!wrote, "writer cannot interleave with a read batch");
		Expect(cache.Int("Shots", "iLens", 50, read) == 101, "batch sees consistent settings before write");
	}
	writer.get();
	{
		SettingsCache::Scope scope(cache, probe);
		Expect(cache.Int("Shots", "iLens", 50, read) == 202, "writer invalidates even when timestamps do not change");
	}
	try {
		SettingsCache::Scope scope(cache, []() -> SettingsCache::Revision { throw std::runtime_error("probe"); });
	} catch (const std::runtime_error&) {}
	const int before = probes;
	{
		SettingsCache::Scope scope(cache, probe);
		Expect(probes == before + 1, "failed probe does not strand scope depth or lock");
	}
	std::cout << "Settings cache: invalidation, defaults, nested batches, live reads and concurrent writes passed.\n";
}

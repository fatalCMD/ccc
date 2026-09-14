#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

namespace SD::Config
{
	// Memoize the existing Windows readers, including their caller's default.
	// Only explicit read batches use the cache. Other reads remain live, and
	// writers hold the same lock through invalidation, persistence and readback.
	class SettingsCache
	{
	public:
		using Revision = std::array<std::array<std::uint32_t, 6>, 3>;

		class Scope
		{
		public:
			template <class Probe>
			Scope(SettingsCache& a_cache, Probe&& a_probe) :
				cache(a_cache), lock(cache.mutex)
			{
				if (cache.depth == 0) {
					const auto observed = a_probe();
					if (!cache.initialized || observed != cache.revision) {
						cache.Clear();
						cache.revision = observed;
						cache.initialized = true;
					}
				}
				++cache.depth;
			}
			~Scope() { --cache.depth; }
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			SettingsCache& cache;
			std::unique_lock<std::recursive_mutex> lock;
		};

		template <class Read>
		int Int(const char* section, const char* key, int fallback, Read&& read)
		{
			const std::lock_guard lock(mutex);
			if (depth == 0) return read();
			const auto id = std::make_tuple(std::string(section), std::string(key), fallback);
			if (const auto found = integers.find(id); found != integers.end()) return found->second;
			const int value = read();
			integers.emplace(id, value);
			return value;
		}

		template <class Read>
		std::string String(const char* section, const char* key, const char* fallback, Read&& read)
		{
			const std::lock_guard lock(mutex);
			if (depth == 0) return read();
			const auto id = std::make_tuple(std::string(section), std::string(key), std::string(fallback));
			if (const auto found = strings.find(id); found != strings.end()) return found->second;
			auto value = read();
			strings.emplace(id, value);
			return value;
		}

		[[nodiscard]] std::unique_lock<std::recursive_mutex> Write()
		{
			std::unique_lock lock(mutex);
			Clear();
			return lock;
		}

	private:
		void Clear() { integers.clear(); strings.clear(); }
		std::recursive_mutex mutex;
		std::size_t depth{};
		bool initialized{};
		Revision revision{};
		std::map<std::tuple<std::string, std::string, int>, int> integers;
		std::map<std::tuple<std::string, std::string, std::string>, std::string> strings;
	};
}

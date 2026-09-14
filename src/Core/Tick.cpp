#include "SD/Core/Tick.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Logging.h"
#include "SD/Runtime.h"

#include <chrono>

namespace SD::Core
{
	namespace
	{
		constexpr auto kSourceCount = static_cast<std::size_t>(Source::kCount);

		std::atomic_bool installed{ false };

		std::array<std::atomic_uint64_t, kSourceCount> counts{};

		// The source currently dispatching. Only one does, so several live hooks
		// still produce exactly one OnFrame per frame.
		std::atomic<std::size_t> primary{ kSourceCount };

		constexpr auto kPreferred = static_cast<std::size_t>(Source::kPlayerCharacter);

		std::chrono::steady_clock::time_point lastFrame{};
		bool                                  haveLastFrame{ false };

		// a_delta is the engine's own delta where a source provides one, and 0 where
		// it does not — in which case wall time fills in.
		void Observe(Source a_source, float a_delta)
		{
			const auto index = static_cast<std::size_t>(a_source);
			counts[index].fetch_add(1, std::memory_order_relaxed);

			// Preference, not first-come.
			//
			// Measured: ThirdPersonState fires a couple of frames earlier and would
			// win a race, but it only runs while the third-person camera state is
			// active — it stops dead in first person and in menus. PlayerCharacter
			// runs in every state and is the only candidate that carries the engine's
			// own delta, so it takes the tick over whenever it appears.
			auto current = primary.load(std::memory_order_relaxed);
			if (current != index) {
				if (current != kSourceCount && index != kPreferred) {
					return;  // another source already drives the frame
				}
				primary.store(index, std::memory_order_relaxed);
				Log::Info(Log::Category::kCore, "Frame source '{}' is driving the tick."sv,
					Tick::Name(a_source));
			}

			float delta = a_delta;
			if (!(delta > 0.0f)) {
				const auto now = std::chrono::steady_clock::now();
				if (haveLastFrame) {
					delta = std::chrono::duration<float>(now - lastFrame).count();
				}
				lastFrame = now;
				haveLastFrame = true;
			}

			// A load screen, an alt-tab or a debugger break produces a delta no
			// smoothing constant survives. Clamp rather than skip — a skipped frame
			// leaves pose interpolation stalled, which reads as the camera sticking.
			delta = std::clamp(delta, 0.0f, 0.1f);

			Runtime::OnFrame(nullptr, delta);
		}

		struct PlayerCameraUpdate
		{
			static void thunk(RE::PlayerCamera* a_this)
			{
				func(a_this);
				Observe(Source::kPlayerCamera, 0.0f);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PlayerCharacterUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				Observe(Source::kPlayerCharacter, a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ThirdPersonStateUpdate
		{
			static void thunk(RE::ThirdPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next)
			{
				func(a_this, a_next);

				// The pose is written here rather than from the dispatched frame,
				// and after the original: the game's own camera work has finished
				// for this state, so this write is the last one and survives.
				Camera::Director::OnThirdPersonUpdate(a_this);

				Observe(Source::kThirdPersonState, 0.0f);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		constexpr std::size_t kTESCameraUpdate = 0x02;
		constexpr std::size_t kActorUpdate = 0xAD;
		constexpr std::size_t kCameraStateUpdate = 0x03;
	}

	void Tick::Install()
	{
		if (installed.load(std::memory_order_relaxed)) {
			return;
		}

		REL::Relocation<std::uintptr_t> camera{ RE::VTABLE_PlayerCamera[0] };
		PlayerCameraUpdate::func = camera.write_vfunc(kTESCameraUpdate, PlayerCameraUpdate::thunk);

		REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
		PlayerCharacterUpdate::func = player.write_vfunc(kActorUpdate, PlayerCharacterUpdate::thunk);

		REL::Relocation<std::uintptr_t> thirdPerson{ RE::VTABLE_ThirdPersonState[0] };
		ThirdPersonStateUpdate::func = thirdPerson.write_vfunc(kCameraStateUpdate, ThirdPersonStateUpdate::thunk);

		installed.store(true, std::memory_order_relaxed);
		Log::Info(Log::Category::kCore,
			"Frame source candidates installed: PlayerCamera::Update (02), PlayerCharacter::Update (0xAD), ThirdPersonState::Update (03)."sv);
	}

	bool Tick::Installed() noexcept
	{
		return installed.load(std::memory_order_relaxed);
	}

	bool Tick::Ticking() noexcept
	{
		return primary.load(std::memory_order_relaxed) != kSourceCount;
	}

	std::uint64_t Tick::Count(Source a_source) noexcept
	{
		const auto index = static_cast<std::size_t>(a_source);
		return index < kSourceCount ? counts[index].load(std::memory_order_relaxed) : 0;
	}

	std::uint64_t Tick::Total() noexcept
	{
		std::uint64_t total = 0;
		for (auto& count : counts) {
			total += count.load(std::memory_order_relaxed);
		}
		return total;
	}

	std::string_view Tick::Name(Source a_source) noexcept
	{
		switch (a_source) {
		case Source::kPlayerCamera:     return "PlayerCamera::Update"sv;
		case Source::kPlayerCharacter:  return "PlayerCharacter::Update"sv;
		case Source::kThirdPersonState: return "ThirdPersonState::Update"sv;
		case Source::kCount:
		default:                        return "none"sv;
		}
	}

	Source Tick::Primary() noexcept
	{
		return static_cast<Source>(primary.load(std::memory_order_relaxed));
	}
}

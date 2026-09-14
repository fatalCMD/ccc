#pragma once

namespace SD::Core
{
	// Scene Director's frame source — chosen by measurement, not by reading.
	//
	// The first attempt hooked PlayerCamera::Update on the strength of a header
	// comment placing TESCamera::Update at vfunc 02. The index is real; the
	// assumption that the game calls it every frame was not. It fired zero times
	// across a minute of play, and only ever fired at all when another mod called
	// camera->Update() explicitly.
	//
	// So rather than replace one guess with another, all three plausible sources
	// are installed together and counted separately. One run of the log then names
	// the source that actually ticks, and the rest can be dropped.
	enum class Source : std::size_t
	{
		kPlayerCamera = 0,     // TESCamera::Update, vfunc 02 — known dead, kept as the control
		kPlayerCharacter,      // Actor::Update(float), vfunc 0xAD — carries a real delta
		kThirdPersonState,     // TESCameraState::Update, vfunc 03 — where the pose must be written
		kCount
	};

	class Tick
	{
	public:
		static void Install();

		[[nodiscard]] static bool Installed() noexcept;

		// True once any source has actually fired. Distinct from Installed(): the
		// whole point of this rewrite is that those two came apart.
		[[nodiscard]] static bool Ticking() noexcept;

		[[nodiscard]] static std::uint64_t    Count(Source a_source) noexcept;
		[[nodiscard]] static std::uint64_t    Total() noexcept;
		[[nodiscard]] static std::string_view Name(Source a_source) noexcept;

		// The source currently driving Runtime::OnFrame, or kCount if none has
		// fired yet. The first source to fire claims it, so dispatch happens once
		// per frame even when several sources are live.
		[[nodiscard]] static Source Primary() noexcept;
	};
}

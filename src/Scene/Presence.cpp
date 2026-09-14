#include "SD/Scene/Presence.h"

#include "SD/Core/Logging.h"

namespace SD::Scene
{
	namespace
	{
		bool            engaged{ false };
		bool            restoreHeadTracking{ false };
		RE::ActorHandle partner{};
		float           graphCountdown{ 0.0f };
		Log::OnceFlag   reported;

		// The target is re-pushed every frame; the graph variable only a few times
		// a second. The first is a pointer write into HighProcessData and costs
		// nothing. The second walks the behaviour graph, and nothing else in this
		// mod writes to the graph per-frame — it does not need to be the first.
		constexpr float kGraphInterval = 0.2f;

		[[nodiscard]] RE::HighProcessData* HighOf(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process ? process->high : nullptr;
		}
	}

	void Presence::Engage(RE::Actor* a_npc)
	{
		if (engaged || !a_npc) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		// Two separate things have to be true. The graph variable permits the
		// animation graph to apply a head rotation at all; the target tells it
		// where to look. Setting only the target does nothing, which is the trap
		// that makes this look unfixable.
		bool previous = false;
		if (player->GetGraphVariableBool("bHeadTracking", previous)) {
			restoreHeadTracking = !previous;
		}
		player->SetGraphVariableBool("bHeadTracking", true);

		if (auto* high = HighOf(player)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, a_npc);
		}

		// The NPC looks back. They usually do this already, but a forcegreet or a
		// scene line can leave them facing where they were walking.
		if (auto* high = HighOf(a_npc)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, player);
		}

		engaged = true;
		partner = a_npc->GetHandle();
		graphCountdown = kGraphInterval;

		if (reported.Take()) {
			Log::Info(Log::Category::kStaging,
				"Headtracking engaged; player graph variable was {}."sv,
				restoreHeadTracking ? "off"sv : "already on"sv);
		}
	}

	void Presence::Update(float a_delta)
	{
		if (!engaged) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  npc = partner.get();
		if (!player || !npc) {
			return;
		}

		if (auto* high = HighOf(player)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, npc.get());
		}
		if (auto* high = HighOf(npc.get())) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, player);
		}

		graphCountdown -= a_delta;
		if (graphCountdown <= 0.0f) {
			graphCountdown = kGraphInterval;

			// The permission half. Other mods turn this off wholesale — combat
			// behaviour, mount transitions, and anything driving its own
			// headtracking — and with it false the target above is inert.
			player->SetGraphVariableBool("bHeadTracking", true);
		}
	}

	void Presence::Release()
	{
		if (!engaged) {
			return;
		}
		engaged = false;
		partner = {};

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		if (auto* high = HighOf(player)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, nullptr);
		}

		// Only put the graph variable back if it was this that turned it on.
		// Several mods in a heavy load order enable player headtracking wholesale,
		// and clearing it unconditionally would break them for the rest of the run.
		if (restoreHeadTracking) {
			player->SetGraphVariableBool("bHeadTracking", false);
			restoreHeadTracking = false;
		}
	}
}

#include "SD/Scene/RegionalFace.h"
#include "SD/Scene/RegionalGeometry.h"
#include "SD/Scene/Performance.h"
#include "SD/Core/Logging.h"

#include <mutex>

namespace SD::Scene
{
	namespace
	{
		struct Header
		{
			std::array<char, 8> magic{};
			std::uint32_t vertices{}, morphs{}, sourceBytes{}, reserved{};
			std::uint64_t sourceHash{};
		};
		static_assert(sizeof(Header) == 32);
		std::mutex mutex;
		RE::NiPointer<RE::BSFaceGenNiNode> boundHead;
		RE::NiPointer<RE::BSDynamicTriShape> boundShape;
		std::vector<Regional::Vertex> asset;
		std::vector<Regional::Undo> undo;
		void* lastBuffer{};
		float lastPeak{};
		std::uint64_t appliedFrames{};
		bool attemptedAsset{ false };
		std::atomic_bool enabled{ true };

		bool LoadAsset()
		{
			if (attemptedAsset) return !asset.empty();
			attemptedAsset = true;
			RE::BSResourceNiBinaryStream input("SKSE\\Plugins\\SceneDirector\\UpperFace\\HPHFemale.sduf");
			Header header;
			if (!input.good() || !input.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
				std::string_view(header.magic.data(), header.magic.size()) != "SDUF0001" ||
				header.vertices != 3832 || header.morphs != 7 || header.sourceBytes != 1284045) return false;
			RE::BSResourceNiBinaryStream source("meshes\\KL\\High Poly Head\\FemaleHead.tri");
			if (!source.good()) return false;
			std::uint64_t hash = 14695981039346656037ull;
			std::array<unsigned char, 8192> bytes{};
			for (std::uint32_t remaining = header.sourceBytes; remaining;) {
				const auto count = std::min(remaining, static_cast<std::uint32_t>(bytes.size()));
				if (!source.read(bytes.data(), count)) return false;
				for (std::uint32_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
				remaining -= count;
			}
			if (hash != header.sourceHash) return false;
			std::vector<Regional::Vertex> loaded(header.vertices);
			if (!input.read(reinterpret_cast<char*>(loaded.data()), static_cast<std::uint32_t>(loaded.size() * sizeof(Regional::Vertex)))) return false;
			for (const auto& v : loaded) {
				if (!std::isfinite(v.blinkLeft) || !std::isfinite(v.blinkRight) ||
					v.blinkLeft < 0 || v.blinkLeft > 1 || v.blinkRight < 0 || v.blinkRight > 1) return false;
				for (const auto& d : v.delta) for (float value : d) if (!std::isfinite(value) || std::abs(value) > 2) return false;
			}
			asset = std::move(loaded);
			return true;
		}

		// Discovery is bounded and runs on the game tick, never in the morph pass.
		RE::BSDynamicTriShape* FindHeadShape(RE::NiAVObject* object, unsigned depth, unsigned& visited)
		{
			if (!object || depth > 8 || ++visited > 128) return nullptr;
			if (auto* shape = object->AsDynamicTriShape()) {
				const auto* fod = shape->GetExtraData<RE::BSFaceGenBaseMorphExtraData>("FOD");
				if (shape->GetTrishapeRuntimeData().vertexCount == asset.size() && fod &&
					fod->modelVertexCount == asset.size()) return shape;
			}
			if (auto* node = object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					if (auto* found = FindHeadShape(child.get(), depth + 1, visited)) return found;
				}
			}
			return nullptr;
		}

		std::span<Regional::Position> Positions()
		{
			if (!boundShape) return {};
			auto& data = boundShape->GetDynamicTrishapeRuntimeData();
			if (!data.dynamicData || data.dataSize != asset.size() * sizeof(Regional::Position) ||
				boundShape->GetTrishapeRuntimeData().vertexCount != asset.size()) return {};
			return { static_cast<Regional::Position*>(data.dynamicData), asset.size() };
		}

		void Restore()
		{
			if (!boundShape) return;
			auto& data = boundShape->GetDynamicTrishapeRuntimeData();
			const RE::BSSpinLockGuard guard(data.lock);
			if (data.dynamicData == lastBuffer) Regional::Restore(Positions(), undo);
			else for (auto& u : undo) u.owned = false;
			lastBuffer = nullptr;
		}
	}

	void RegionalFace::Prepare(RE::Actor* player)
	{
		auto* head = player ? player->GetFaceNodeSkinned() : nullptr;
		{
			const std::lock_guard lock(mutex);
			if (boundHead.get() == head) return;
			Restore();
			boundShape.reset();
			boundHead.reset(head);
			lastPeak = 0;
			appliedFrames = 0;
		}
		if (!head) return;
		auto* base = player->GetActorBase();
		auto* part = base ? base->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kFace) : nullptr;
		if (!part || _stricmp(part->morphs[1].GetModel(), "KL\\High Poly Head\\FemaleHead.tri") != 0) return;
		// Resource I/O only here, outside both the render hook and its mutex.
		if (!LoadAsset()) {
			Log::Warn(Log::Category::kStaging, "Regional face unavailable: missing asset or source TRI mismatch; retaining upper-face modifiers."sv);
			return;
		}
		unsigned visited = 0;
		auto* shape = FindHeadShape(head, 0, visited);
		if (!shape) {
			Log::Warn(Log::Category::kStaging, "Regional face unavailable: no matching dynamic head geometry ({} vertices)."sv, asset.size());
			return;
		}
		{
			const std::lock_guard lock(mutex);
			boundShape.reset(shape);
			undo.assign(asset.size(), {});
		}
		Log::Info(Log::Category::kStaging, "Regional face bound: {} vertices, geometry '{}'; source verified, mouth region excluded."sv,
			asset.size(), shape->name.c_str());
	}

	void RegionalFace::Before(RE::BSFaceGenNiNode* node)
	{
		const std::lock_guard lock(mutex);
		if (node == boundHead.get()) Restore();
	}

	void RegionalFace::After(RE::BSFaceGenNiNode* node, bool suppress)
	{
		std::array<float, 8> weights{};
		const bool active = !suppress && Performance::SampleRegionalFace(weights) && enabled.load(std::memory_order_relaxed);
		const std::lock_guard lock(mutex);
		if (node != boundHead.get() || !boundShape) return;
		lastPeak = 0;
		if (!active) return;
		auto& data = boundShape->GetDynamicTrishapeRuntimeData();
		const RE::BSSpinLockGuard guard(data.lock);
		const auto positions = Positions();
		if (positions.empty()) return;
		float left = 0, right = 0;
		if (auto* animation = node->GetRuntimeData().animationData.get()) {
			const auto& modifiers = animation->modifier3;
			if (modifiers.values && modifiers.count >= 2) { left = modifiers.values[0]; right = modifiers.values[1]; }
		}
		lastPeak = Regional::Apply(positions, asset, undo, weights, left, right);
		lastBuffer = data.dynamicData;
		if (lastPeak > 0) ++appliedFrames;
	}

	void RegionalFace::Reset()
	{
		const std::lock_guard lock(mutex);
		Restore();
		boundShape.reset();
		boundHead.reset();
		undo.clear();
		lastPeak = 0;
		appliedFrames = 0;
	}

	void RegionalFace::SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }
	bool RegionalFace::Enabled() { return enabled.load(std::memory_order_relaxed); }

	void RegionalFace::Report()
	{
		float peak;
		std::uint64_t frames;
		bool bound;
		{
			const std::lock_guard lock(mutex);
			peak = lastPeak; frames = appliedFrames; bound = bool(boundShape);
		}
		Log::Info(Log::Category::kStaging, "Regional face | bound={} appliedFrames={} currentPeak={:.5f}"sv, bound, frames, peak);
	}
}

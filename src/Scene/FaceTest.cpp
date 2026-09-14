#include "SD/Scene/FaceTest.h"

#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/UpperFaceTest.h"

#include <mutex>

namespace SD::Scene
{
	namespace
	{
		std::mutex mutex;
		UpperFaceTest::Frame published;
		RE::NiPointer<RE::BSFaceGenNiNode> owner;
		std::uint32_t lastRequest{};
		float poll{}, elapsed{};
		int lastPhase{ -1 };
		std::uint64_t calls{};
		UpperFace::Shape finalPeak{};
		struct GeometryProbe
		{
			RE::NiPointer<RE::BSDynamicTriShape> shape;
			void* buffer{};
			std::vector<std::array<float, 4>> neutral;
			float peak{};
			std::uint64_t samples{};
		};
		std::vector<GeometryProbe> geometry;

		void Notify(const char* text)
		{
			// Same address-library call used by Director's existing framing notice.
			static REL::Relocation<void (*)(const char*, const char*, bool)> notify{
				REL::RelocationID(52050, 52933)
			};
			notify(text, nullptr, true);
		}

		void Discover(RE::NiAVObject* object, unsigned depth, unsigned& visited)
		{
			if (!object || depth > 8 || ++visited > 128 || geometry.size() >= 16) return;
			if (auto* shape = object->AsDynamicTriShape()) {
				const auto count = shape->GetTrishapeRuntimeData().vertexCount;
				const auto* fod = shape->GetExtraData<RE::BSFaceGenBaseMorphExtraData>("FOD");
				auto& data = shape->GetDynamicTrishapeRuntimeData();
				const RE::BSSpinLockGuard guard(data.lock);
				if (fod && count > 0 && count <= 20000 && fod->modelVertexCount == count &&
					data.dynamicData && data.dataSize == count * sizeof(std::array<float, 4>)) {
					GeometryProbe probe;
					probe.shape.reset(shape);
					probe.neutral.resize(count);
					geometry.push_back(std::move(probe));
				}
			}
			if (auto* node = object->AsNode()) for (auto& child : node->GetChildren())
				Discover(child.get(), depth + 1, visited);
		}

		void ReportPhase()
		{
			Log::Info(Log::Category::kStaging,
				"UPPER FACE TEST result | phase={} morphCalls={} finalPeak down={:.3f}/{:.3f} in={:.3f}/{:.3f} up={:.3f}/{:.3f} squint={:.3f}/{:.3f}; channel values, NOT render proof."sv,
				lastPhase, calls, finalPeak[0], finalPeak[1], finalPeak[2], finalPeak[3],
				finalPeak[4], finalPeak[5], finalPeak[6], finalPeak[7]);
			calls = 0;
			finalPeak = {};
			for (auto& probe : geometry) {
				Log::Info(Log::Category::kStaging,
					"UPPER FACE TEST geometry | phase={} shape={} samples={} peakLocalDelta={:.6f}; CPU vertices vs preceding neutral, includes native idle/blink; NOT GPU/render proof."sv,
					lastPhase, probe.shape->name.c_str(), probe.samples, probe.peak);
				probe.peak = 0;
				probe.samples = 0;
			}
		}

		void DescribeHead(RE::PlayerCharacter* player)
		{
			if (auto* base = player->GetActorBase()) {
				for (auto type : { RE::BGSHeadPart::HeadPartType::kFace,
					RE::BGSHeadPart::HeadPartType::kEyebrows, RE::BGSHeadPart::HeadPartType::kEyes }) {
					if (auto* part = base->GetCurrentHeadPartByType(type))
						Log::Info(Log::Category::kStaging,
							"UPPER FACE TEST asset | type={} id={:08X} model={} defaultTri={}"sv,
							static_cast<unsigned>(type), part->GetFormID(), part->GetModel(), part->morphs[1].GetModel());
				}
			}
			for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
				const auto slot = UpperFace::kSlots[i];
				const char* name = RE::BSFaceGenKeyframeMultiple::GetModifierName(slot);
				Log::Info(Log::Category::kStaging, "UPPER FACE TEST mapping | slot={} engine={} expected={}"sv,
					slot, name ? name : "MISSING", UpperFaceTest::kNames[i]);
			}
		}
	}

	void FaceTest::Tick(float delta)
	{
		if (!std::isfinite(delta) || delta <= 0) return;
		delta = std::min(delta, .1f);
		poll -= delta;
		bool start = false, stop = false;
		if (poll <= 0) {
			poll = .5f;
			static const auto path = Config::DataPath(L"SKSE\\Plugins\\SD_UpperFaceTest.ini");
			const auto request = path.empty() ? 0u : ::GetPrivateProfileIntW(L"Test", L"iRun", 0, path.c_str());
			start = request > 0 && request != lastRequest;
			stop = request == 0;
			lastRequest = request;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* face = player ? player->GetFaceNodeSkinned() : nullptr;
		auto* ui = RE::UI::GetSingleton();
		const bool paused = ui && ui->GameIsPaused();
		bool finished = false, began = false;
		{
			const std::lock_guard lock(mutex);
			if (published.active && (stop || face != owner.get())) {
				ReportPhase();
				published = {};
				owner.reset();
				finished = true;
			}
			if (start) {
				if (face && !paused) {
					elapsed = 0;
					lastPhase = -1;
					calls = 0;
					finalPeak = {};
					owner.reset(face);
					geometry.clear();
					unsigned visited = 0;
					Discover(face, 0, visited);
					published = UpperFaceTest::Sample(0);
					began = true;
				} else Log::Warn(Log::Category::kStaging,
					"UPPER FACE TEST request rejected: load a game and close pause/console menus, then use a new iRun number."sv);
			}
			if (published.active && !paused) {
				if (!began) elapsed += delta;
				const auto next = UpperFaceTest::Sample(elapsed);
				if (next.phase != lastPhase) {
					if (lastPhase >= 0) ReportPhase();
					lastPhase = next.phase;
					if (next.active) {
						const auto label = next.index < 0 ? "neutral (all eight test controls zero)" :
							UpperFaceTest::kNames[static_cast<std::size_t>(next.index)];
						if (next.index >= 0 || next.phase == 0) {
							const auto message = fmt::format("SD face test: {}", label);
							Notify(message.c_str());
						}
						Log::Info(Log::Category::kStaging,
							"UPPER FACE TEST | phase={} elapsed={:.2f}s {} value={:.1f}; native only, regional/listener layers suppressed."sv,
							next.phase, elapsed, label, next.index < 0 ? 0.0f : 1.0f);
					}
				}
				published = next;
				if (!next.active) { owner.reset(); finished = true; }
			}
		}
		if (began) {
			Log::Warn(Log::Category::kStaging,
				"UPPER FACE TEST started: 27 seconds, isolated native controls at 1.0. Camera and lip-sync unchanged. iRun=0 cancels."sv);
			DescribeHead(player);
		}
		if (finished) {
			// Hand back synchronously, including when the face is no longer rendered.
			FaceGen::ReleaseModifiers();
			Notify("SD face test finished; normal expressions restored.");
			Log::Info(Log::Category::kStaging, "UPPER FACE TEST stopped; one-shot request consumed, no automatic repeat."sv);
		}
	}

	bool FaceTest::Sample(UpperFace::Shape& values)
	{
		const std::lock_guard lock(mutex);
		values = published.values;
		return published.active;
	}

	void FaceTest::Record(RE::BSFaceGenNiNode* node)
	{
		const std::lock_guard lock(mutex);
		if (!published.active || node != owner.get()) return;
		++calls;
		if (auto* data = node->GetRuntimeData().animationData.get()) {
			const auto& keys = data->modifier3;
			for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
				const auto slot = UpperFace::kSlots[i];
				if (keys.values && slot < keys.count) finalPeak[i] = std::max(finalPeak[i], keys.values[slot]);
			}
		}
		for (auto& probe : geometry) {
			auto& data = probe.shape->GetDynamicTrishapeRuntimeData();
			const RE::BSSpinLockGuard guard(data.lock);
			if (!data.dynamicData || data.dataSize != probe.neutral.size() * sizeof(std::array<float, 4>)) continue;
			const auto* positions = static_cast<const std::array<float, 4>*>(data.dynamicData);
			if (published.index < 0) {
				std::copy_n(positions, probe.neutral.size(), probe.neutral.begin());
				probe.buffer = data.dynamicData;
			} else if (probe.buffer == data.dynamicData) {
				++probe.samples;
				for (std::size_t i = 0; i < probe.neutral.size(); ++i) {
					float square = 0;
					for (std::size_t axis = 0; axis < 3; ++axis) {
						const float diff = positions[i][axis] - probe.neutral[i][axis];
						square += diff * diff;
					}
					if (std::isfinite(square)) probe.peak = std::max(probe.peak, std::sqrt(square));
				}
			}
		}
	}

	void FaceTest::Cancel()
	{
		const std::lock_guard lock(mutex);
		published = {};
		owner.reset();
		geometry.clear();
	}
}

#include "SD/Camera/Anatomy.h"

#include "SD/Core/Logging.h"

namespace SD::Camera
{
	namespace
	{
		// The humanoid baseline. Every one of these is the value the shot table was
		// written against, and each is multiplied by the measured scale.
		constexpr float kHumanExtent = 42.0f;
		constexpr float kHumanMinDistance = 68.0f;
		constexpr float kHumanProbeStart = 48.0f;
		constexpr float kHumanEyeHeight = 120.0f;

		// The fallback reference. Reached only when the player has no body bound,
		// because normally the player IS the reference — see Fit. In the ballpark
		// of the measured player values (70-77) rather than invented.
		constexpr float kFallbackRadius = 75.0f;

		// WHAT A PERSON MEASURES, AND WHY THERE IS A BAND RATHER THAN A NUMBER.
		//
		// Body bound is a sphere around everything an actor wears and carries, so
		// it is a good size signal and a noisy one. Measured in this load order on
		// 2026-08-15: the player 70-77 across four conversations, and other people
		// 91, 95, 106 — a spread of about 1.4x from wardrobe alone. Scaling by that
		// framed an NPC in a fur collar looser than one in a shirt, which is the
		// head-and-chest shot that came back from the first test.
		//
		// So anything inside the band is simply a person. The signal is only
		// believed when it is unambiguous, and a dragon at 679 against a 77
		// reference — 8.8x — is not ambiguous.
		constexpr float kPersonBandLow = 0.60f;
		constexpr float kPersonBandHigh = 1.85f;

		// Head size grows much slower than body bulk.
		//
		// The dragon measured 8.8x a person's bounding sphere. Its head is not 8.8
		// times a person's head — closer to three or four — and since every framing
		// number here is about the head, applying the bulk ratio directly put the
		// standoff out past the wingtips. 0.6 turns 8.8x of bulk into 3.6x of
		// framing, which is about right for a dragon skull.
		//
		// One dragon is one data point. This is the number to move if large
		// creatures come out consistently too tight or too loose, and the log
		// prints both the bulk and the framing factor so the ratio can be checked
		// against whatever is actually on screen.
		constexpr float kFrameExponent = 0.6f;

		// Past these a subject stops being a person. Sits outside the person band
		// on both sides by construction — anything inside it has already been
		// pinned to exactly 1.0 and is a humanoid by definition.
		constexpr float kLargeBulk = 2.2f;
		constexpr float kSmallBulk = 0.5f;

		// Nothing sane is outside this, and a bad measurement must degrade to a
		// strange shot rather than to a camera two thousand units away.
		constexpr float kMinScale = 0.15f;
		constexpr float kMaxScale = 8.0f;

		// Exact head nodes, tried in order before anything is searched for.
		//
		// The first is the humanoid convention and covers every vanilla NPC. The
		// rest are the shapes creature skeletons in this load order take; the
		// substring search below is what covers the ones nobody has met yet.
		constexpr std::array kHeadNodes{
			"NPC Head [Head]"sv,
			"NPC Head"sv,
			"Head"sv,
			"HEAD"sv,
			"NPCHead"sv,
			"Head [Head]"sv,
		};

		// A rig with an arm has a shoulder to shoot over. Asked directly rather
		// than guessed from size, because "is this thing humanoid" and "is this
		// thing person-sized" are different questions and a giant answers them
		// differently.
		constexpr std::array kArmNodes{
			"NPC L UpperArm [LUar]"sv,
			"NPC R UpperArm [RUar]"sv,
			"NPC L Clavicle [LClv]"sv,
		};

		// Reported for the first few actors of a session, then quiet. Enough to
		// cover a conversation with something unusual without turning a long play
		// session into a log of every chicken in Whiterun.
		constexpr int kReportBudget = 12;
		int           reported{ 0 };

		// ASCII, deliberately. Skeleton node names are ASCII, and std::tolower is
		// locale-dependent — a Turkish locale lowercases 'I' to a dotless i and
		// stops "Head" matching "head", which is not a bug anybody would find.
		[[nodiscard]] constexpr char Lower(char a_c) noexcept
		{
			return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c - 'A' + 'a') : a_c;
		}

		[[nodiscard]] bool ContainsNoCase(std::string_view a_haystack, std::string_view a_needle)
		{
			if (a_needle.empty() || a_haystack.size() < a_needle.size()) {
				return false;
			}
			const auto limit = a_haystack.size() - a_needle.size();
			for (std::size_t i = 0; i <= limit; ++i) {
				bool match = true;
				for (std::size_t j = 0; j < a_needle.size(); ++j) {
					if (Lower(a_haystack[i + j]) != Lower(a_needle[j])) {
						match = false;
						break;
					}
				}
				if (match) {
					return true;
				}
			}
			return false;
		}

		// The highest node whose name looks like a head.
		//
		// Highest rather than first because a skeleton carries several candidates —
		// a neck, a headtracking marker, sometimes a helmet attachment — and on
		// anything that stands upright the actual head is the top of them. The
		// headtracking node is excluded by name: it exists on most rigs, it is
		// parented near the head, and it is not where the face is.
		void FindHead(RE::NiAVObject* a_object, RE::NiAVObject*& a_best, float& a_bestZ, int a_depth)
		{
			if (!a_object || a_depth > 12) {
				return;
			}

			if (const char* raw = a_object->name.c_str()) {
				const std::string_view name{ raw };
				if (ContainsNoCase(name, "head"sv) && !ContainsNoCase(name, "headtrack"sv)) {
					const float z = a_object->world.translate.z;
					if (!a_best || z > a_bestZ) {
						a_best = a_object;
						a_bestZ = z;
					}
				}
			}

			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					FindHead(child.get(), a_best, a_bestZ, a_depth + 1);
				}
			}
		}
	}

	BuildTuning Tuning(Build a_build) noexcept
	{
		switch (a_build) {
		// Dragons, giants, mammoths.
		//
		// fillCap 0.50: the whole head, never part of one. This is the number that
		// answers the reported shot — kExtremeClose asks for 0.85, and on a wedge
		// of a head on a long neck that is the underside of the jaw.
		//
		// riseScale 0.30: rise scales with the subject, but a dragon meeting you
		// indoors has a ceiling, and everything worth looking at is at or below the
		// head. A 62-unit overhead becomes 19 rather than 220.
		//
		// angleBias 16: off the snout. A long head shot dead-on is a nostril; a
		// three-quarter puts its length across the frame.
		case Build::kLarge: return { 0.50f, 0.30f, 16.0f };

		// Wolves, bears, sabre cats. Same long-head problem as a dragon in
		// miniature, and the same answer at lower strength. They are close to
		// person-sized, so the rise column is nearly right as authored.
		case Build::kBeast: return { 0.68f, 0.80f, 10.0f };

		// Chickens, rabbits, skeevers. Tight framing is fine and is most of what
		// makes them readable at all; nothing here needs holding back.
		case Build::kSmall: return { 0.90f, 1.00f, 0.0f };

		// Unchanged, and it must stay unchanged: this is the path every ordinary
		// conversation in the game takes, and the shot table was authored against
		// exactly these numbers.
		case Build::kHumanoid:
		default: return { 1.00f, 1.00f, 0.0f };
		}
	}

	std::string_view Name(Build a_build) noexcept
	{
		switch (a_build) {
		case Build::kHumanoid: return "humanoid"sv;
		case Build::kBeast:    return "beast"sv;
		case Build::kLarge:    return "large"sv;
		case Build::kSmall:    return "small"sv;
		default:               return "?"sv;
		}
	}

	Anatomy Measure(RE::Actor* a_actor)
	{
		Anatomy out{};

		auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
		if (!root) {
			// The humanoid default, unmeasured. Better than refusing to stage: the
			// previous behaviour for a creature was these same numbers, silently.
			return out;
		}

		const float rootZ = root->world.translate.z;

		// Named nodes first — one GetObjectByName is cheaper than a tree walk, and
		// an exact hit is not a guess.
		RE::NiAVObject* head = nullptr;
		std::string_view via = "search"sv;
		for (const auto& name : kHeadNodes) {
			if (auto* node = root->GetObjectByName(name)) {
				head = node;
				via = name;
				break;
			}
		}

		if (!head) {
			float bestZ = 0.0f;
			FindHead(root, head, bestZ, 0);
		}

		if (head) {
			// Clamped far wider than the humanoid path's 50..220: a dragon's head
			// sits well above that and a chicken's well below, and the old clamp is
			// exactly what would flatten both back to a person's height.
			const auto& hp = head->world.translate;
			out.eyeHeight = std::clamp(hp.z - rootZ, 6.0f, 900.0f);

			// The horizontal offset, taken out of world space into the actor's own
			// frame so it survives them turning. Zero-ish on anything upright,
			// which is why nothing needed it until a quadruped turned up.
			const float heading = a_actor->GetAngleZ();
			const float c = std::cos(-heading);
			const float s = std::sin(-heading);
			const float dx = hp.x - root->world.translate.x;
			const float dy = hp.y - root->world.translate.y;
			out.headOffset = {
				dx * c - dy * s,
				dx * s + dy * c,
				out.eyeHeight
			};

			out.headRadius = head->worldBound.radius;
		}

		out.radius = root->worldBound.radius;

		// Whether the rig has an arm. A separate question from size, and asked of
		// the skeleton rather than inferred, because "is this humanoid" and "is
		// this person-sized" have different answers on a giant.
		out.rigged = false;
		for (const auto& name : kArmNodes) {
			if (root->GetObjectByName(name)) {
				out.rigged = true;
				break;
			}
		}

		out.head = head != nullptr;
		out.via = via;
		out.name = a_actor->GetName() ? a_actor->GetName() : "?";
		out.measured = true;
		return out;
	}

	void Fit(Anatomy& a_body, float a_referenceRadius)
	{
		const bool  fallback = !(a_referenceRadius > 1.0f);
		const float reference = fallback ? kFallbackRadius : a_referenceRadius;

		const float raw = a_body.radius > 1.0f ? a_body.radius / reference : 1.0f;
		a_body.bulk = (raw > kPersonBandLow && raw < kPersonBandHigh) ?
			1.0f :
			std::clamp(raw, kMinScale, kMaxScale);

		// Bulk classifies; its fractional power frames. See kFrameExponent.
		a_body.scale = a_body.bulk == 1.0f ? 1.0f : std::pow(a_body.bulk, kFrameExponent);

		a_body.extent = kHumanExtent * a_body.scale;
		a_body.minDistance = kHumanMinDistance * a_body.scale;
		a_body.probeStart = kHumanProbeStart * a_body.scale;

		// Size decides first, and deliberately outranks the skeleton. A giant has
		// arms and a perfectly good shoulder, and shooting over it would still put
		// the camera thirty feet up looking at nothing.
		//
		// Asked of BULK rather than of the framing factor: the question here is
		// "what kind of animal is this", and that is the raw ratio.
		if (a_body.bulk >= kLargeBulk) {
			a_body.build = Build::kLarge;
		} else if (a_body.bulk <= kSmallBulk) {
			a_body.build = Build::kSmall;
		} else if (a_body.rigged && a_body.head) {
			a_body.build = Build::kHumanoid;
		} else {
			a_body.build = Build::kBeast;
		}

		// Only a humanoid rig at humanoid size has a shoulder worth shooting over.
		a_body.shoulder = a_body.rigged && a_body.build == Build::kHumanoid;

		if (reported < kReportBudget) {
			++reported;
			Log::Info(Log::Category::kStaging,
				"Body {}: {} | body r{:.0f} / ref {:.0f}{} -> bulk {:.2f} frame {:.2f} | "
				"head via {} at +{:.0f},{:.0f} up {:.0f} (bone r{:.1f}, always 0) | "
				"extent {:.0f} floor {:.0f} probe {:.0f} | fillCap {:.2f} rise x{:.2f} angle +{:.0f} | shoulder {}"sv,
				a_body.name, Name(a_body.build), a_body.radius, reference,
				fallback ? " (ESTIMATE - player unmeasurable)"sv : ""sv,
				a_body.bulk, a_body.scale, a_body.via,
				a_body.headOffset.x, a_body.headOffset.y, a_body.eyeHeight, a_body.headRadius,
				a_body.extent, a_body.minDistance, a_body.probeStart,
				Tuning(a_body.build).fillCap, Tuning(a_body.build).riseScale,
				Tuning(a_body.build).angleBias,
				a_body.shoulder ? "yes"sv : "no"sv);
		}
	}
}

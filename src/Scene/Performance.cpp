#include "SD/Scene/Performance.h"

#include "SD/Core/Logging.h"
#include "SD/Core/Config.h"
#include "SD/Dialogue/Session.h"
#include "SD/Scene/ExpressionModel.h"
#include "SD/Scene/UpperFace.h"
#include "SD/Scene/FaceGen.h"

#include "SD/Scene/LipSync.h"

#include <mutex>

namespace SD::Scene
{
	namespace
	{
		using namespace Expressions;

		struct Gaze
		{
			bool         averted{ false };
			float        remaining{ 1.0f };
			RE::NiPoint3 offset{};
		};

		Gaze            npcGaze{};
		Gaze            playerGaze{};
		RE::ActorHandle npcHandle{};
		bool            engaged{ false };

		std::atomic_bool wantExpressions{ true };
		bool  wantGaze{ true };
		bool  wantHoldFace{ true };
		constexpr float intensityScale = ExpressionProfiles::kIntensity;

		// Nodes whose draw flags this mod raised, and what they held before.
		//
		// Restoring the exact previous value rather than clearing the bits is not
		// fussiness. kAlwaysDraw and kHighDetail are both flags other mods set on
		// heads for their own reasons — a face-light mod, a head-mesh replacer, an
		// LOD tweak — and clearing them unconditionally at the end of every
		// conversation would break those quietly, for the rest of the run, in a way
		// that would never be traced back to a camera mod.
		struct HeldNode
		{
			RE::NiPointer<RE::NiAVObject> node{};
			std::uint32_t                 flags{ 0 };
		};

		std::vector<HeldNode> heldNodes{};
		Log::OnceFlag         holdReported;

		// The face node the hold was rooted at, kept so a swap can be noticed.
		//
		// The player's head is not a fixed object for the length of a conversation.
		// Measured 2026-08-10: in two of four conversations the engine destroyed and
		// rebuilt it mid-exchange — twice over inside one of them — while a headgear
		// mod took a helmet off. The rebuilt node arrives without the flags raised
		// below, heldNodes goes on pointing at a corpse, and ReleaseHeldNodes ends
		// the conversation by restoring a node that is no longer in the scene. The
		// hold silently stops holding anything, which is the exact condition
		// bHoldPlayerFace exists to prevent.
		//
		// It read as a mystery for a week because the FaceGen probe latches the same
		// pointer once and counts a rebuilt head as a head that stopped updating.
		// Comparing this against GetFaceNodeSkinned() each tick is what separates
		// the two.
		RE::NiPointer<RE::BSFaceGenNiNode> heldRoot{};

		// The two flags worth raising, and why these two.
		//
		// kAlwaysDraw (1 << 11) takes the node out of the culling decision, which
		// is the decision the reported behaviour points at: a head the camera is
		// not looking at is a head the engine has no reason to morph.
		//
		// kHighDetail (1 << 24) covers the other reading of the same observation —
		// that the gate is a level-of-detail choice rather than a visibility one.
		// Nobody has separated the two, and there is no cost to raising both: they
		// are draw hints on a single head for the length of a conversation.
		constexpr std::uint32_t kHoldFlags =
			static_cast<std::uint32_t>(RE::NiAVObject::Flag::kAlwaysDraw) |
			static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHighDetail);

		// The asymmetry that makes an exchange read as two people rather than two
		// headtrack targets: the listener holds eye contact, the speaker looks away
		// while assembling a sentence. Equal values collapse the model into a stare.
		float listenerHold{ 0.82f };
		float speakerHold{ 0.48f };

		// Rising-edge tracking for the player's own line; see the gaze step.
		bool playerWasTalking{ false };


		std::uint32_t rng{ 0x1F123BB5u };

		Log::OnceFlag gazeReported;

		// Game-thread envelopes. Only complete snapshots cross into the morph hook.
		struct FaceExpression
		{
			Shape base{};
			Shape current{};
			float age{ 0.0f };
			float quietFor{ 0.0f };
			bool ownsOverride{ false };
		};
		UpperFace::Performance playerUpperFace{};
		Listener::Performance playerListener{};
		FaceExpression npcExpression{};
		std::mutex expressionMutex;
		UpperFace::Shape publishedUpperFace{};
		UpperFace::Shape publishedRegionalFace{};
		Shape publishedNpc{};
		Shape publishedListener{};
		bool publishedListenerActive{ false };
		std::atomic_bool cinematicListening{ true };
		bool publishedPlayerActive{ false };
		bool publishedNpcActive{ false };
		std::atomic<std::uint32_t> playerLineEmotion{ kNeutral };
		std::atomic_int forcedExpression{ -1 };
		Reading playerReaction{};
		bool playerExprWasTalking{ false };
		std::uint64_t playerLineSerial{ 0 };
		constexpr float kReactionScale = 0.80f;

		void SetReading(FaceExpression& a_face, const Reading& a_reading, float a_scale)
		{
			const auto index = ExpressionFor(a_reading.emotion);
			const float strength = AuthoredStrength(index, a_reading.percent);
			a_face.base = MakeShape(index, strength * a_scale);
			a_face.quietFor = 0.0f;
			a_face.age = 0.0f;
		}

		void ApplyPlayerReading(const Reading& a_reading, std::string_view a_why,
			std::string_view a_detail, bool a_listening = false)
		{
			playerUpperFace.Begin(a_reading, a_listening, a_detail);
			playerLineEmotion.store(playerUpperFace.reading.emotion, std::memory_order_relaxed);
			Log::Info(Log::Category::kStaging,
				"Acting plan v2: {} | {} beat(s), estimated {:.2f}s | {}"sv,
				a_why, playerUpperFace.plan.count, playerUpperFace.duration, a_detail);
			for (std::size_t i = 0; i < playerUpperFace.plan.count; ++i) {
				const auto& beat = playerUpperFace.plan.beats[i];
				Log::Info(Log::Category::kStaging,
					"  Beat {}: {} tone={} strength={} evidence={} span={:.3f}-{:.3f} profile={}"sv,
					i + 1, Acting::Name(beat.action), beat.tone.emotion, beat.tone.percent,
					Acting::Name(beat.evidence), beat.begin, beat.end,
					ExpressionProfiles::kSections[static_cast<std::size_t>(Acting::ProfileFor(beat))]);
			}
		}

		[[nodiscard]] float NextUnit()
		{
			rng = rng * 1664525u + 1013904223u;
			return static_cast<float>((rng >> 16) & 0x7FFF) / 32767.0f;
		}

		[[nodiscard]] RE::HighProcessData* HighOf(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process ? process->high : nullptr;
		}

		// Is a voice line playing on this actor right now?
		//
		// soundHandles only. Deliberately NOT voiceState or voiceTimeElapsed: those
		// sit at 0x000 and 0x014 beside currentShout and voiceRecoveryTime and
		// belong to the shout system, which is why the face probe prints them as a
		// flat zero through every line ever logged here. voiceTimer is no better —
		// measured, it holds a constant for an entire session rather than counting
		// down a line.
		[[nodiscard]] bool VoiceHandlePlaying(RE::Actor* a_actor)
		{
			auto* high = HighOf(a_actor);
			if (!high) {
				return false;
			}

			for (const auto& handle : high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] RE::BSFaceGenAnimationData* FaceOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetFaceGenAnimationData() : nullptr;
		}

		void ClearFace(RE::Actor* a_actor)
		{
			if (auto* face = FaceOf(a_actor)) {
				face->ClearExpressionOverride();
			}
		}

		// Raise the draw flags on a node and everything under it.
		//
		// The whole subtree, not just the face node: the morph lands on the head's
		// geometry, and a culling decision is taken per drawn object. Raising the
		// flag on the parent alone leaves the children to be culled on their own
		// merits, which is the case that matters here.
		void HoldSubtree(RE::NiAVObject* a_object)
		{
			if (!a_object) {
				return;
			}

			auto& flags = a_object->GetFlags();
			heldNodes.push_back({ RE::NiPointer<RE::NiAVObject>{ a_object }, flags.underlying() });
			flags.set(static_cast<RE::NiAVObject::Flag>(kHoldFlags));

			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					HoldSubtree(child.get());
				}
			}
		}

		void ReleaseHeldNodes()
		{
			for (auto& held : heldNodes) {
				if (held.node) {
					held.node->GetFlags() = static_cast<RE::NiAVObject::Flag>(held.flags);
				}
			}
			heldNodes.clear();
			heldRoot.reset();
		}

		// Keep the player's head drawable for the length of the conversation.
		//
		// Refuses in first person for the obvious reason: forcing the third-person
		// head to always draw while the camera is inside it is how a mod gives
		// someone a view of the back of their own eyeballs.
		void ApplyFaceHold(RE::Actor* a_player)
		{
			if (!wantHoldFace || !heldNodes.empty() || !a_player) {
				return;
			}

			auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->IsInFirstPerson()) {
				return;
			}

			auto* face = a_player->GetFaceNodeSkinned();
			if (!face) {
				return;
			}

			HoldSubtree(face);
			heldRoot.reset(face);

			if (holdReported.Take()) {
				Log::Info(Log::Category::kStaging,
					"Holding the player's head in the drawn set ({} nodes) so it keeps animating off camera."sv,
					heldNodes.size());
			}
		}

		// Move the hold onto the new head when the engine swaps one in.
		//
		// Cheap enough to run every tick: one virtual call and a pointer compare on
		// the common path. The subtree walk only happens when the node has actually
		// changed, which is rare and is precisely the event worth paying for.
		//
		// A null target is a real state rather than a failure — first person and a
		// head with no 3D both land there, and ApplyFaceHold refuses both. Leaving
		// heldRoot null in that case means the compare mismatches again next tick,
		// so the hold comes back by itself when the player returns to third person
		// instead of staying dropped for the rest of the conversation.
		void RefreshFaceHold(RE::Actor* a_player)
		{
			if (!wantHoldFace || !a_player) {
				return;
			}

			auto* face = a_player->GetFaceNodeSkinned();
			if (face == heldRoot.get()) {
				return;
			}

			const bool hadHold = !heldNodes.empty();
			ReleaseHeldNodes();
			ApplyFaceHold(a_player);

			// Reported at warn because it is not routine, and because the FaceGen
			// probe's call counter goes flat on the same frame — without this line
			// that reads as the player's head having stopped being morphed.
			if (!heldNodes.empty()) {
				Log::Warn(Log::Category::kStaging,
					"The player's face node was replaced mid-conversation; hold re-applied to the new head ({} nodes)."sv,
					heldNodes.size());
			} else if (hadHold) {
				Log::Warn(Log::Category::kStaging,
					"The player's face node was replaced mid-conversation and the hold could not follow it."sv);
			}
		}

		bool  probeFace{ false };
		float probeCountdown{ 0.0f };

		// Sample every frame for the first seconds of a conversation.
		//
		// The reported A/B is whether the player's face was visible at the instant
		// the conversation opened: visible and the mouth animates for that whole
		// conversation, hidden and it never does. Whatever the engine decides, it
		// decides there. At 2 Hz that instant is one sample or none, which is why
		// four sessions of logs have plenty of data either side of it and nothing
		// at it. The burst falls back to the idle rate afterwards so the rest of
		// the conversation stays readable.
		float probeBurst{ 0.0f };

		constexpr float kProbeBurstSeconds = 2.5f;
		constexpr float kProbeIdleInterval = 0.5f;

		// The player's phoneme channel, tallied over one conversation.
		//
		// This exists because the channel turned out to read INVERTED, and once you
		// know that it is a usable oracle — which means nobody has to judge a mouth
		// by eye again, and that is what has cost this investigation every wrong
		// turn it has taken.
		//
		// phenomeKeyFrame holds values pending application. When whatever applies
		// the morph is running, it drains them; when it is not, they pile up. So a
		// HIGH reading means the mouth is NOT moving. Measured 2026-08-03, same
		// NPC, same session:
		//
		//   npc, mouth demonstrably working ....... mean 0.000  (164 samples)
		//   player, opened facing the camera ...... mean 0.068  (mouth works)
		//   player, opened facing away ............ mean 0.145  (mouth frozen)
		//
		// The midpoint between the two player cases is ~0.105, and that is the only
		// justification for the threshold below. It is calibrated on two
		// conversations with one NPC and should be treated as a smoke alarm, not an
		// instrument — the numbers are printed alongside it so the verdict can
		// always be second-guessed from the raw values.
		struct Tally
		{
			std::uint32_t samples{ 0 };
			float         sum{ 0.0f };
			float         peak{ 0.0f };
		};

		Tally playerPhonemes{};

		constexpr float kAppliedBelow = 0.105f;

		// What one keyframe channel currently holds.
		//
		// count is read from the engine's own struct and used as a loop bound, so
		// it is clamped. A BSFaceGenAnimationData that is mid-construction, or a
		// pointer that is not really one, would otherwise walk arbitrary memory —
		// and this runs on actors the mod has already been told not to touch.
		// Phonemes number 16 in Skyrim; 256 is slack, not a guess at the real size.
		//
		// `readable` is the field that matters and it used to be thrown away.
		//
		// Sample returns peak = 0.0f for FOUR different reasons: the channel is
		// genuinely flat, `values` is null, `count` is zero, or `count` is absurd.
		// ReportFace logged only the peak, so all four printed as `ph=0.000` and
		// were indistinguishable. An entire conclusion was built on that ambiguity
		// — "NPCs lipsync perfectly with the phoneme channel flat at zero", which
		// retired phenomeKeyFrame as a lead and sent the investigation off toward
		// BGShkPhonemeController. The NPC reading it rests on is equally consistent
		// with the channel simply not being readable from here. Log the reason.
		//
		// `argmax` because "peak" is a maximum across the channel's slots at one
		// instant, not a maximum over time, and which slot won is the whole
		// question when the value is being called a phoneme or a jaw modifier.
		// Skyrim's modifier set is eyes and brows; the jaw sits on the phoneme
		// side. A bare max cannot tell a blink from a mouth.
		struct Channel
		{
			std::uint32_t count{ 0 };
			float         peak{ 0.0f };
			std::int32_t  argmax{ -1 };
			bool          updated{ false };
			bool          readable{ false };
		};

		[[nodiscard]] Channel Sample(const RE::BSFaceGenKeyframeMultiple& a_keyframe)
		{
			Channel channel{};
			channel.count = a_keyframe.count;
			channel.updated = a_keyframe.isUpdated;

			if (!a_keyframe.values || a_keyframe.count == 0 || a_keyframe.count > 256) {
				return channel;
			}

			channel.readable = true;
			for (std::uint32_t i = 0; i < a_keyframe.count; ++i) {
				if (a_keyframe.values[i] > channel.peak || channel.argmax < 0) {
					channel.peak = a_keyframe.values[i];
					channel.argmax = static_cast<std::int32_t>(i);
				}
			}
			return channel;
		}

		// One channel, rendered so a zero can be argued with.
		//
		// `rd=0` means the peak is meaningless. `n` is the slot count, `i` the slot
		// that won, `up` the engine's own isUpdated flag — which is set by SetValue
		// and cleared by whoever consumes the keyframe, so watching it flip is how
		// you find the consumer without hooking anything.
		[[nodiscard]] std::string Describe(const Channel& a_channel)
		{
			return fmt::format("{:.3f}[rd={} n={} i={} up={}]"sv,
				a_channel.peak,
				a_channel.readable ? 1 : 0,
				a_channel.count,
				a_channel.argmax,
				a_channel.updated ? 1 : 0);
		}

		[[nodiscard]] std::string_view PostureOf(RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!state) {
				return "unknown"sv;
			}
			switch (state->GetSitSleepState()) {
			case RE::SIT_SLEEP_STATE::kNormal:            return "standing"sv;
			case RE::SIT_SLEEP_STATE::kIsSitting:         return "seated"sv;
			case RE::SIT_SLEEP_STATE::kIsSleeping:        return "asleep"sv;
			case RE::SIT_SLEEP_STATE::kWantToSit:         return "about-to-sit"sv;
			case RE::SIT_SLEEP_STATE::kWaitingForSitAnim: return "sitting-down"sv;
			case RE::SIT_SLEEP_STATE::kWantToStand:       return "standing-up"sv;
			default:                                      return "other"sv;
			}
		}

		// The "two facegen objects per actor" question, settled from the headers.
		//
		// It is not two objects, and this no longer needs measuring. vfunc 62
		// GetFaceNode() is not overridden by Actor, Character or PlayerCharacter;
		// TESObjectREFR::GetFaceNode delegates straight to GetFaceNodeSkinned
		// (vfunc 61), which Character does override. So GetFaceNode() and
		// GetFaceNodeSkinned() return the SAME node for every actor, and an
		// earlier comment here claiming they "return different pointers on every
		// sample" was wrong — the probe never logged either pointer, so there was
		// nothing behind the claim.
		//
		// What is still worth logging is the third path: vfunc 63
		// GetFaceGenAnimationData() hangs off the actor's process data, and
		// nothing in the headers forces it to be the same instance the node
		// carries. That comparison is `same=` below. The old log line reported it
		// as `SAME=`, LIPSYNC.md §3 cites `SAME=yes` as measured — and no such
		// token exists in this build. It went out with a rewrite and the citation
		// was never updated, so that row was resting on a deleted instrument.
		void ReportFace(std::string_view a_who, RE::Actor* a_actor)
		{
			const auto posture = PostureOf(a_actor);

			auto* fromProcess = FaceOf(a_actor);
			auto* node = a_actor ? a_actor->GetFaceNode() : nullptr;
			auto* skinned = a_actor ? a_actor->GetFaceNodeSkinned() : nullptr;
			auto* fromNode = node ? node->GetRuntimeData().animationData.get() : nullptr;

			if (!fromProcess && !fromNode) {
				Log::Info(Log::Category::kStaging,
					"Face probe | {} | {} | NO facegen data on either path (node={})."sv,
					a_who, posture, static_cast<const void*>(node));
				return;
			}

			const auto describe = [](RE::BSFaceGenAnimationData* a_data, Channel& a_phoneme,
									  Channel& a_expression, Channel& a_modifier) {
				if (!a_data) {
					return;
				}
				a_phoneme = Sample(a_data->phenomeKeyFrame);
				a_expression = Sample(a_data->expressionKeyFrame);
				a_modifier = Sample(a_data->modifierKeyFrame);
			};

			Channel procPhoneme{}, procExpression{}, procModifier{};
			Channel nodePhoneme{}, nodeExpression{}, nodeModifier{};
			describe(fromProcess, procPhoneme, procExpression, procModifier);
			describe(fromNode, nodePhoneme, nodeExpression, nodeModifier);

			// The node's own state, which is where the gate should be.
			//
			// BSFaceGenNiNode is what applies the morphs: UpdateDownwardPass
			// (vfunc 0x2C, overridden) walks animationData onto the head, using
			// lastTime for its delta and flags for whatever it is allowed to do.
			//
			// lastTime reads as the obvious liveness signal and it is not one, at
			// this sampling rate. Measured over 145 paired samples on disk it was
			// identical between PLAYER and npc to two decimal places on every
			// single sample, and advanced at exactly wall-clock rate — it is a
			// global timestamp, so it can never separate one actor from another by
			// construction. It froze once, for three samples, on BOTH actors at
			// once: a global pause, not a per-actor stall.
			//
			// Worse, at 2 Hz "it advanced ~0.5" is satisfied by a node that updated
			// thirty times and by a node that updated once. LIPSYNC.md §3 reads
			// this field as proof the player's face node is being updated normally.
			// It is not capable of showing that. Frame-rate resolution, from inside
			// UpdateDownwardPass itself, is the only thing that would be.
			std::uint16_t nodeFlags = 0;
			float         nodeLastTime = -1.0f;
			if (node) {
				const auto& runtime = node->GetRuntimeData();
				nodeFlags = runtime.flags;
				nodeLastTime = runtime.lastTime;
			}

			// The face node's NiAVObject flags, in full.
			//
			// Only bit 0 (APP_CULLED) was ever read, off the skinned node, and it
			// read 0 on all 290 samples on disk — so "the head was not culled" is
			// the one thing it established, and LIPSYNC.md never recorded even
			// that. The bits that matter for whether a downward pass reaches this
			// node at all are the selective-update ones (0x02/0x04/0x08/0x10), and
			// they were never looked at. Log the whole word.
			//
			// Note these are NOT the flags the write-up calls "the face node's
			// flags". PLAYER 0x003C / NPC 0x001C come from
			// BSFaceGenNiNode::RUNTIME_DATA::flags — a uint16 at runtime+0x38 with
			// no enum anywhere in CommonLibSSE. Two different fields, similar
			// values, and the doc conflates them. `fg=` is the facegen one, `av=`
			// the NiAVObject one.
			const std::uint32_t nodeAvFlags = node ? node->GetFlags().underlying() : 0u;
			const std::uint32_t nodeCulled = node ? ((nodeAvFlags & 1u) ? 1u : 0u) : 2u;

			// Whether the process-side object and the node-side object are one.
			const bool sameObject = fromProcess && fromNode && fromProcess == fromNode;

			// The geometry of the reported behaviour, quantified.
			//
			// 2026-08-03: the lips move when the camera is on the player's face and
			// stop when it is at their back. The screenshots also show the head
			// plainly RENDERED in the failing case, in frame, so this is not
			// culling — `culled` read 0 on every sample ever taken, which agreed
			// and was not weighted properly.
			//
			// `facing` is the dot of the actor's own heading with the direction
			// from them to the camera: +1 is the camera dead in front of their
			// face, -1 is directly behind their head. If the phoneme channel goes
			// quiet as this crosses zero, the correlation is in the log rather than
			// in a screenshot, and the sign tells us which way round.
			//
			// `dist` is there to separate orientation from proximity, since the two
			// move together in an over-the-shoulder shot and nothing so far has
			// told them apart.
			// `body` was the first attempt and it measured the wrong thing.
			//
			// It dotted the actor's GetAngleZ heading against the direction to the
			// camera. During a conversation the player's BODY barely turns — they
			// face the NPC throughout — so it mostly reported where SD had put the
			// camera along the player/NPC axis, and it read positive through two
			// runs the reporter had deliberately set up as opposites. Kept because
			// it is free and because knowing it does NOT separate the cases is
			// itself worth recording.
			//
			// `inView` is the one that answers the question: the camera's own
			// forward vector against the direction from the lens to the HEAD node.
			// +1 is the head dead ahead of the lens, 0 is edge of frame, negative
			// is behind the camera entirely. Unambiguous, and it needs no guess
			// about which way a head node's axes point.
			//
			// `headFwd` is the face-toward-lens reading, taken from the head node
			// rather than the body so headtracking is included. Column 1 of a
			// Skyrim node's rotation is its forward, the same convention ApplyPose
			// relies on for the camera. Treat the sign as unverified until it is
			// seen to move with something known.
			float body = -2.0f;
			float inView = -2.0f;
			float headFwd = -2.0f;
			float dist = -1.0f;
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot && a_actor) {
				const auto& root = camera->cameraRoot->world;
				const auto  eye = root.translate;
				const auto  here = a_actor->GetPosition();

				const float bx = eye.x - here.x;
				const float by = eye.y - here.y;
				const float flat = std::sqrt(bx * bx + by * by);
				if (flat > 1.0f) {
					const float heading = a_actor->GetAngleZ();
					body = (std::sin(heading) * bx + std::cos(heading) * by) / flat;
				}

				const auto head = node ? node->world.translate : here;
				float      hx = head.x - eye.x;
				float      hy = head.y - eye.y;
				float      hz = head.z - eye.z;
				dist = std::sqrt(hx * hx + hy * hy + hz * hz);
				if (dist > 1.0f) {
					hx /= dist;
					hy /= dist;
					hz /= dist;
					inView = root.rotate.entry[0][1] * hx +
						root.rotate.entry[1][1] * hy +
						root.rotate.entry[2][1] * hz;

					if (node) {
						const auto& hr = node->world.rotate;
						headFwd = -(hr.entry[0][1] * hx + hr.entry[1][1] * hy + hr.entry[2][1] * hz);
					}
				}
			}

			// Does the engine think this actor is voicing a line?
			//
			// This is the thing that has never been looked at, and in hindsight it
			// should have been first. Everything so far measured the FACE — the
			// morph pass, the keyframes, the node flags — and found the player's
			// face healthy and simply not moving. So the question is not what the
			// face does with a lipsync track; it is whether the engine ever had one
			// for the player at all.
			//
			// The measured asymmetry that points here: the NPC's phoneme keyframe
			// is rewritten every single frame (isUpdated true on 100% of morph-pass
			// calls) while the player's is stale and untouched. Something drives
			// the NPC's channel continuously and nothing drives the player's.
			//
			// DBVO does not use the dialogue path. It plays lines with the console
			// command `Player.SpeakSound "DBVO/<pack>/<line>.fuz"`, verified from
			// the string table of DBVO_Script_MCM.pex. The .fuz carries an embedded
			// .lip track, so the data exists — the question is whether SpeakSound
			// registers a voice line on the actor the way the dialogue system does,
			// and whether that registration survives whatever SD is doing.
			//
			// voiceState / soundHandles / voiceTimer are the engine's own record of
			// exactly that. If the player's is empty while their audio is audible,
			// the lipsync system was never given anything to play, and no amount of
			// work on the face will matter.
			std::uint32_t voiceState = 0xFFFFFFFFu;
			float         voiceTimer = -1.0f;
			float         voiceElapsed = -1.0f;
			std::uint32_t sound0 = 0;
			std::uint32_t sound0State = 0xFFu;
			std::uint32_t sound1 = 0;
			std::uint32_t sound1State = 0xFFu;
			bool          talkingToPC = false;
			std::string   subtitle{};
			if (auto* high = HighOf(a_actor)) {
				voiceState = high->voiceState.underlying();
				voiceTimer = high->voiceTimer;
				voiceElapsed = high->voiceTimeElapsed;
				sound0 = high->soundHandles[0].soundID;
				sound0State = high->soundHandles[0].state.underlying();
				sound1 = high->soundHandles[1].soundID;
				sound1State = high->soundHandles[1].state.underlying();
				talkingToPC = high->talkingToPC;
				if (high->voiceSubtitle.c_str()) {
					subtitle.assign(high->voiceSubtitle.c_str());
					if (subtitle.size() > 24) {
						subtitle.resize(24);
					}
				}
			}

			// The engine's per-frame morph budget, free to read and never looked at.
			//
			// uiNumActorsAllowedToMorph defaults to 10 and caps at 64, and the
			// candidate set is built by distance. In a 3,632-mod profile with
			// followers in frame it is not obviously generous. If this number moves
			// when the shot changes, that is the mechanism and no hook is needed.
			std::uint32_t morphBudget = 0;
			bool          morphEmotions = false;
			if (auto* faceGen = RE::BSFaceGenManager::GetSingleton()) {
				morphBudget = faceGen->numActorsToMorph;
				morphEmotions = faceGen->emotions;
			}

			if (engaged && a_who == "PLAYER"sv && nodePhoneme.readable) {
				playerPhonemes.samples++;
				playerPhonemes.sum += nodePhoneme.peak;
				playerPhonemes.peak = std::max(playerPhonemes.peak, nodePhoneme.peak);
			}

			Log::Info(Log::Category::kStaging,
				"Face probe | {} | {} | {} | VOICE state={} timer={:.2f} elapsed={:.2f} snd0={}/{} snd1={}/{} toPC={} sub=\"{}\" | ph={} ex={} md={} | same={} skin={} | fg=0x{:04X} t={:.2f} | av=0x{:08X} culled={} | inView={:+.2f} headFwd={:+.2f} dist={:.0f} | morph={}"sv,
				a_who, posture,
				engaged ? "STAGING"sv : "idle   "sv,
				voiceState, voiceTimer, voiceElapsed,
				sound0, sound0State, sound1, sound1State,
				talkingToPC ? 1 : 0, subtitle,
				Describe(nodePhoneme), Describe(nodeExpression), Describe(nodeModifier),
				fromProcess && fromNode ? (sameObject ? "yes"sv : "NO"sv) : "n/a"sv,
				skinned == node ? "same"sv : "DIFFERENT"sv,
				nodeFlags, nodeLastTime,
				nodeAvFlags, nodeCulled,
				inView, headFwd, dist,
				morphBudget);
		}

		// One step of the gaze model.
		//
		// a_holdBias is the fraction of the time this character should be looking
		// at the other: high while listening, lower while speaking.
		void StepGaze(RE::Actor* a_actor, Gaze& a_gaze, float a_delta, float a_holdBias)
		{
			auto* high = HighOf(a_actor);
			if (!high) {
				return;
			}

			a_gaze.remaining -= a_delta;
			if (a_gaze.remaining <= 0.0f) {
				const bool wantAvert = NextUnit() > a_holdBias;

				if (wantAvert) {
					// A glance away, not a turn of the head — small, and more often
					// sideways or down than up, which is where people actually look
					// when they are thinking.
					const float lateral = (NextUnit() - 0.5f) * 90.0f;
					const float vertical = (NextUnit() - 0.75f) * 40.0f;
					a_gaze.offset = { lateral, lateral * 0.4f, vertical };
					a_gaze.averted = true;
					a_gaze.remaining = 0.5f + NextUnit() * 1.3f;
				} else {
					a_gaze.offset = {};
					a_gaze.averted = false;
					a_gaze.remaining = 1.4f + NextUnit() * 2.6f;
				}
			}

			// Ease toward the wanted offset so the eyes travel rather than teleport.
			auto&       live = high->headTrackTargetOffset;
			const float k = std::clamp(a_delta * 6.0f, 0.0f, 1.0f);
			live.x += (a_gaze.offset.x - live.x) * k;
			live.y += (a_gaze.offset.y - live.y) * k;
			live.z += (a_gaze.offset.z - live.z) * k;
		}
	}

	void Performance::Configure(bool a_expressions, bool a_gaze)
	{
		// Configuration I/O stays on the game thread, never in the face morph hook.
		// Missing profile keys use built-in defaults so old installations need no
		// INI replacement. SD_user.ini can override any individual profile field.
		playerUpperFace.profiles = ExpressionProfiles::Defaults();
		for (std::size_t i = 0; i < playerUpperFace.profiles.size(); ++i) {
			auto& profile = playerUpperFace.profiles[i];
			const auto section = ExpressionProfiles::kSections[i];
			for (std::size_t key = 0; key < profile.controls.size(); ++key) {
				const int fallback = static_cast<int>(std::lround(profile.controls[key] * 100));
				profile.controls[key] = static_cast<float>(std::clamp(
					Config::Int(section, ExpressionProfiles::kKeys[key], fallback), key < 2 ? -100 : 0, 100)) / 100.0f;
			}
			profile.region = static_cast<std::uint32_t>(std::clamp(
				Config::Int(section, "iRegionEmotion", static_cast<int>(profile.region)), 0, 7));
			profile.regionWeight = static_cast<float>(std::clamp(
				Config::Int(section, "iRegionStrength", static_cast<int>(std::lround(profile.regionWeight * 100))), 0, 100)) / 100.0f;
		}
		cinematicListening.store(Config::Bool("Performance", "bCinematicListening", true), std::memory_order_relaxed);
		wantExpressions.store(a_expressions, std::memory_order_relaxed);
		wantGaze = a_gaze;
		// Expression profiles do not depend on mouth animation being enabled.
		if (a_expressions) FaceGen::Install();
	}

	bool Performance::ExpressionsEnabled() noexcept
	{
		return wantExpressions.load(std::memory_order_relaxed);
	}

	void Performance::UpdateFace(float a_delta)
	{
		if (!std::isfinite(a_delta) || a_delta <= 0.0f) return;
		const float delta = std::min(a_delta, 0.1f);
		auto npc = npcHandle.get();
		const bool driving = engaged && ExpressionsEnabled();
		const bool npcTalking = driving && Dialogue::Session::GetSingleton().Speaking();
		const bool playerTalking = driving && LipSync::PlayerSpeaking();

		if (driving) {
			const auto serial = LipSync::PlayerLineSerial();
			if (playerTalking && (!playerExprWasTalking || serial != playerLineSerial)) {
				const auto topic = LipSync::PlayerLineText();
				// Clause planning owns text interpretation; no borrowed NPC emotion.
				const Reading reading{ kNeutral, 0 };
				ApplyPlayerReading(reading,
					topic.empty() ? "speaking, no topic text"sv : "speaking, text cues"sv, topic);
				const float duration = LipSync::PlayerLineDuration();
				playerUpperFace.SetDuration(duration);
				Log::Info(Log::Category::kStaging, "Acting clock: {} duration={:.2f}s; clause positions are text-weight estimates."sv,
					playerUpperFace.voiceDuration ? "voice-window (may be fallback)"sv : "text estimate"sv, playerUpperFace.duration);
				if (!npcTalking) SetReading(npcExpression, { kNeutral, 0 }, kReactionScale);
				playerLineSerial = serial;
			}
			// Keep the just-spoken emotion while settling. The next NPC response
			// supplies a new listener reaction; do not resurrect its previous line.
		}
		playerExprWasTalking = playerTalking;
		if (!driving) playerLineEmotion.store(kNeutral, std::memory_order_relaxed);

		const float scale = intensityScale;
		const auto advance = [&](FaceExpression& face, RE::Actor* actor, bool attentive) {
			face.age += delta;
			face.quietFor = attentive ? 0.0f : face.quietFor + delta;
			Shape target{};
			if (driving) {
				// Brief speech-state gaps must not pump the expression in and out.
				const float gain = LineGain(face.age, face.quietFor < 0.25f);
				for (std::size_t i = 0; i < kSlots; ++i) target[i] = face.base[i] * gain;
			}
			const bool any = Step(face.current, target, delta) && scale > 0.0f;
			if (any) {
				if (auto* data = FaceOf(actor)) {
					const auto peak = std::max_element(face.current.begin(), face.current.end());
					const auto slot = static_cast<std::uint32_t>(peak - face.current.begin());
					data->SetExpressionOverride(slot, std::clamp(*peak * scale, 0.0f, 1.0f));
					face.ownsOverride = true;
				}
			} else if (face.ownsOverride) {
				ClearFace(actor);
				face.ownsOverride = false;
			}
			return any;
		};
		// Full-face acting is permitted only in a silent NPC-listening interval.
		// A voice start clears it immediately instead of fading through lip closures.
		const bool listenerActive = playerListener.Step(delta, playerUpperFace.reading,
			playerUpperFace.age, scale, playerUpperFace.listening && npcTalking,
			playerTalking || !playerUpperFace.listening, driving && cinematicListening.load(std::memory_order_relaxed));
		const bool wasMoving = std::any_of(playerUpperFace.motion.current.begin(),
			playerUpperFace.motion.current.end(), [](float v) { return v > 0.0f; });
		const auto previousBeat = playerUpperFace.beatIndex;
		if (playerTalking) playerUpperFace.SynchronizeSpeech(LipSync::PlayerLineElapsed(), delta);
		const bool moving = playerUpperFace.Step(delta, driving, playerTalking || npcTalking, scale, listenerActive);
		if (driving && playerUpperFace.beatIndex != previousBeat) {
			const auto& beat = playerUpperFace.plan.beats[playerUpperFace.beatIndex];
			Log::Info(Log::Category::kStaging, "Acting beat active: {} {} at {:.2f}/{:.2f}s (text-weight timing)."sv,
				playerUpperFace.beatIndex + 1, Acting::Name(beat.action), playerUpperFace.age, playerUpperFace.duration);
		}
		if (driving) playerLineEmotion.store(playerUpperFace.reading.emotion, std::memory_order_relaxed);
		// Publish the zero endpoint too, so the final residual is not parked on the head.
		const bool playerActive = driving || moving || wasMoving;
		const bool npcActive = advance(npcExpression, npc.get(), npcTalking || playerTalking);
		// No engine calls under this lock: the render hook only copies 17 floats.
		{
			const std::lock_guard lock(expressionMutex);
			publishedUpperFace = playerUpperFace.motion.current;
			publishedRegionalFace = playerUpperFace.regionalMotion.current;
			// Native-listener suppression is eased at the target above. Publishing
			// a hard zero here would bypass that smoothing at both handoff edges.
			publishedListener = playerListener.current;
			publishedListenerActive = listenerActive;
			publishedNpc = npcExpression.current;
			publishedPlayerActive = playerActive;
			publishedNpcActive = npcActive;
		}
	}

	bool Performance::PlayerExpressionActive() noexcept
	{
		const std::lock_guard lock(expressionMutex);
		return publishedPlayerActive;
	}

	std::uint32_t Performance::PlayerEmotion() noexcept
	{
		return playerLineEmotion.load(std::memory_order_relaxed);
	}

	bool Performance::SampleExpression(float* a_out, std::uint32_t a_count, bool a_player) noexcept
	{
		if (!a_out || a_count == 0) return false;
		if (const int forced = forcedExpression.load(std::memory_order_relaxed); a_player && forced >= 0) {
			std::fill_n(a_out, a_count, 0.0f);
			if (static_cast<std::uint32_t>(forced) < a_count) a_out[forced] = 1.0f;
			return true;
		}
		if (a_player) return false;
		const std::lock_guard lock(expressionMutex);
		if (!publishedNpcActive) return false;
		const auto& shape = publishedNpc;
		const float scale = intensityScale;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			a_out[i] = i < kSlots ? std::clamp(shape[i] * scale, 0.0f, 1.0f) : 0.0f;
		}
		return true;
	}

	bool Performance::SampleListenerExpression(float* a_out, std::uint32_t a_count) noexcept
	{
		if (!a_out || a_count == 0 || !ExpressionsEnabled() || !cinematicListening.load(std::memory_order_relaxed) ||
			forcedExpression.load(std::memory_order_relaxed) >= 0) return false;
		const std::lock_guard lock(expressionMutex);
		if (!publishedListenerActive) return false;
		for (std::uint32_t i = 0; i < a_count; ++i) a_out[i] = i < kSlots ? publishedListener[i] : 0.0f;
		return true;
	}

	bool Performance::SampleUpperFace(float* a_out, std::uint32_t a_count) noexcept
	{
		if (!a_out || a_count == 0) return false;
		const std::lock_guard lock(expressionMutex);
		if (!publishedPlayerActive) return false;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			a_out[i] = i < publishedUpperFace.size() ? publishedUpperFace[i] : 0.0f;
		}
		return true;
	}

	bool Performance::SampleRegionalFace(std::array<float, 8>& a_out) noexcept
	{
		const std::lock_guard lock(expressionMutex);
		a_out = publishedRegionalFace;
		return publishedPlayerActive && forcedExpression.load(std::memory_order_relaxed) < 0;
	}

	void Performance::SetForcedExpression(int a_slot)
	{
		const int slot = (a_slot >= 0 && a_slot <= 16) ? a_slot : -1;
		if (forcedExpression.exchange(slot, std::memory_order_relaxed) == slot) {
			return;
		}

		if (slot < 0) {
			Log::Info(Log::Category::kStaging,
				"Forced expression off; the player's face is the engine's again."sv);
			return;
		}

		// Loud, and it says what to look at. A diagnostic that needs the log read
		// to know whether it engaged is a diagnostic that gets misread — the same
		// note SetForcedViseme carries, for the same reason.
		Log::Warn(Log::Category::kStaging,
			"FORCED EXPRESSION {}: pinning that slot to 1.0 on the player every frame. "
			"Look at the player's face in third person - it should be visibly stuck in that "
			"expression, with no conversation needed. If it is NOT, the expression channel "
			"does not reach the player's geometry and the fault is the head, not this mod. "
			"Set [Diagnostics] iForceExpression=-1 to stop."sv,
			slot);
	}

	void Performance::SetGaze(float a_listenerHold, float a_speakerHold)
	{
		listenerHold = std::clamp(a_listenerHold, 0.0f, 1.0f);
		speakerHold = std::clamp(a_speakerHold, 0.0f, 1.0f);
	}

	void Performance::Engage(RE::Actor* a_npc)
	{
		if (engaged || !a_npc) {
			return;
		}

		npcHandle = a_npc->GetHandle();
		npcGaze = {};
		playerGaze = {};
		engaged = true;

		// The player's face starts this conversation with no opinion. Carrying the
		// last one over would open every conversation on a reaction to a line from
		// the previous one — and with a rebuilt head, on a stale target as well.
		playerReaction = {};
		playerExprWasTalking = false;
		playerUpperFace.Begin({}, true, {});
		playerListener = {};
		Log::Info(Log::Category::kStaging,
			"Expression profiles v4: fixed 1.50 strength; soft ceiling; continuous brow velocity; eased listener handoff; mouth-safe player speech."sv);
		Log::Info(Log::Category::kStaging, "Cinematic listening: {}; strong v2 (scale 0.72, cap 0.85), player speech priority."sv,
			cinematicListening.load(std::memory_order_relaxed) ? "enabled"sv : "disabled"sv);
		npcExpression = {};
		playerLineSerial = 0;
		playerLineEmotion.store(kNeutral, std::memory_order_relaxed);

		gazeReported.Reset();
		holdReported.Reset();
		playerPhonemes = {};
		Log::Info(Log::Category::kStaging,
			"Performance settings for this conversation: gaze={} expressions={} holdFace={} intensity={:.2f} listenerHold={:.2f} speakerHold={:.2f}."sv,
			wantGaze ? "on"sv : "OFF"sv,
			wantExpressions ? "on"sv : "OFF"sv,
			wantHoldFace ? "on"sv : "OFF"sv,
			intensityScale, listenerHold, speakerHold);

		ApplyFaceHold(RE::PlayerCharacter::GetSingleton());

		// Open the dense sampling window on the event under test.
		probeBurst = kProbeBurstSeconds;
		probeCountdown = 0.0f;
	}

	void Performance::HoldPlayerFace(bool a_hold)
	{
		wantHoldFace = a_hold;

		// Turning it off mid-conversation puts the head back immediately rather
		// than waiting for the conversation to end, so the menu toggle is a live
		// A/B on the thing it controls instead of a setting for next time.
		if (!wantHoldFace) {
			ReleaseHeldNodes();
		} else if (engaged) {
			ApplyFaceHold(RE::PlayerCharacter::GetSingleton());
		}
	}

	void Performance::Release()
	{
		if (!engaged) {
			return;
		}
		engaged = false;

		// Hand the face and the eyes back exactly as they were found. An
		// expression left overridden follows the NPC around for the rest of the
		// session, which is the sort of bug that gets blamed on a body mod.
		// Before anything else, and unconditionally: the head goes back exactly as
		// it was found. A conversation that ends by any route — menu close, combat,
		// a cell change, the subject dying — must not leave draw flags raised on
		// the player for the rest of the session.
		ReleaseHeldNodes();

		// The verdict for this conversation, in one line.
		//
		// Printed whenever the probe gathered anything, so a run can be scored from
		// the log without anyone watching a mouth. See the Tally comment for why a
		// LOW number is the good one.
		if (playerPhonemes.samples > 0) {
			const float mean = playerPhonemes.sum / static_cast<float>(playerPhonemes.samples);
			Log::Info(Log::Category::kStaging,
				"VERDICT | player facegen looks {} | phoneme mean={:.4f} peak={:.3f} over {} samples "
				"(low = drained by the morph = mouth moving; calibration npc 0.000 / working 0.068 / frozen 0.145)."sv,
				mean < kAppliedBelow ? "APPLIED — mouth should be moving"sv
									 : "NOT APPLIED — mouth frozen"sv,
				mean, playerPhonemes.peak, playerPhonemes.samples);
		}
		playerPhonemes = {};

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  npc = npcHandle.get();

		// Let the player's envelope finish fading through UpdateFace. The NPC
		// leaves our morph hook at End(), so release its owned override now.
		if (npcExpression.ownsOverride) ClearFace(npc.get());
		npcExpression = {};
		{
			const std::lock_guard lock(expressionMutex);
			publishedNpc = {};
			publishedNpcActive = false;
			publishedListener = {};
			publishedListenerActive = false;
		}
		if (wantGaze) {
			if (auto* high = HighOf(player)) high->headTrackTargetOffset = {};
			if (auto* high = HighOf(npc.get())) high->headTrackTargetOffset = {};
		}

		npcHandle = {};
	}

	void Performance::ResetForLoad()
	{
		Release();
		FaceGen::ReleaseModifiers();
		const auto configuredProfiles = playerUpperFace.profiles;
		playerUpperFace = {};
		playerUpperFace.profiles = configuredProfiles;
		playerListener = {};
		npcExpression = {};
		playerReaction = {};
		playerExprWasTalking = false;
		playerLineSerial = 0;
		playerLineEmotion.store(kNeutral, std::memory_order_relaxed);
		const std::lock_guard lock(expressionMutex);
		publishedUpperFace = {};
		publishedRegionalFace = {};
		publishedListener = {};
		publishedListenerActive = false;
		publishedNpc = {};
		publishedPlayerActive = false;
		publishedNpcActive = false;
	}

	void Performance::OnLine(RE::Actor* a_speaker, std::uint32_t a_emotion,
		std::uint16_t a_percent, std::string_view a_text)
	{
		if (!engaged || !a_speaker || a_speaker != npcHandle.get().get()) return;
		// Retain readings while disabled, so switching expressions on can react
		// to the current line without waiting for another response.
		LipSync::OnResponse();
		playerExprWasTalking = false;
		// Preserve authored NPC emotion. Neutral NPC records no longer acquire
		// a full-face override merely because their line mentions a loaded word.
		const Reading reading = a_emotion > kNeutral && a_emotion <= kPuzzled && a_percent > 0 ?
			Reading{ a_emotion, std::min<std::uint16_t>(a_percent, 100), false } : Reading{ kNeutral, 0 };
		SetReading(npcExpression, reading, 1.0f);
		playerReaction = Listener::ResponseTo(reading.emotion, reading.percent);
		ApplyPlayerReading(playerReaction, "listening"sv, a_text, true);
		Log::Info(Log::Category::kStaging,
			"NPC expression: slot {} ({} emotion {} @ {}%, authored {} @ {}%) <- {}"sv,
			ExpressionFor(reading.emotion), reading.inferred ? "text-inferred"sv : "record"sv,
			reading.emotion, reading.percent, a_emotion, a_percent, a_text);
	}

	void Performance::ConfigureProbe(bool a_probeFace)
	{
		probeFace = a_probeFace;
		probeCountdown = 0.0f;
	}

	// Every change of the player's voice sound handle, at frame rate.
	//
	// The 2 Hz probe cannot tell a handle that was DROPPED from a line that was
	// SHORT, and that distinction is the entire hypothesis: the failing case shows
	// the engine holding a voice sound on the player for a fraction of the time it
	// does in the working case, but line lengths differ and a sampled fraction
	// cannot separate the two.
	//
	// So this watches for transitions rather than sampling levels. Each one prints
	// the sound ID and how long the previous one was held, continuously, to the
	// millisecond. A line that plays to completion and one that is cut off look
	// nothing alike in that record.
	void WatchVoice(float a_delta)
	{
		static std::uint32_t lastSound = 0xFFFFFFFFu;
		static float         heldFor = 0.0f;

		auto* high = HighOf(RE::PlayerCharacter::GetSingleton());
		if (!high) {
			return;
		}

		const std::uint32_t now = high->soundHandles[0].soundID;
		heldFor += a_delta;

		if (now == lastSound) {
			return;
		}

		constexpr std::uint32_t kNone = 0xFFFFFFFFu;
		if (lastSound != kNone && now == kNone) {
			Log::Info(Log::Category::kStaging,
				"VOICE player sound {} RELEASED after {:.2f}s."sv, lastSound, heldFor);
		} else if (now != kNone) {
			Log::Info(Log::Category::kStaging,
				"VOICE player sound {} STARTED{}."sv, now,
				lastSound != kNone ? fmt::format(" (replacing {} after {:.2f}s)", lastSound, heldFor) : "");
		}

		lastSound = now;
		heldFor = 0.0f;
	}

	void Performance::Probe(RE::Actor* a_npc, float a_delta)
	{
		if (!probeFace) {
			return;
		}

		// Ahead of the countdown: this one is per frame, not twice a second.
		WatchVoice(a_delta);

		probeBurst = std::max(0.0f, probeBurst - a_delta);

		probeCountdown -= a_delta;
		if (probeCountdown > 0.0f) {
			return;
		}
		// Zero rather than a small interval: the next frame's subtraction takes it
		// negative, so the burst samples at whatever rate the game is running.
		probeCountdown = probeBurst > 0.0f ? 0.0f : kProbeIdleInterval;

		// Deliberately does NOT require `engaged`. With [Direction] bEnabled=0 the
		// director never opens, so Engage never runs — and that configuration is
		// precisely the control this probe exists to measure.
		ReportFace("PLAYER"sv, RE::PlayerCharacter::GetSingleton());
		if (a_npc) {
			ReportFace("npc"sv, a_npc);
		}
	}

	void Performance::Update(float a_delta, bool a_npcSpeaking)
	{
		if (!engaged) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  npc = npcHandle.get();
		if (!player || !npc) {
			return;
		}

		// The probe used to run here and no longer does — see Performance::Probe.
		// Driven from Director::Tick instead, so it still reports when SD is
		// staging nothing at all, which is the control case.

		// Ahead of the gaze early-return, deliberately. The head hold is not part
		// of the gaze model and has to survive bGaze=0.
		RefreshFaceHold(player);

		if (!wantGaze) {
			return;
		}

		// THE PLAYER LOOKS AT WHOEVER THEY ARE TALKING TO.
		//
		// a_npcSpeaking is false for the whole of the player's own voiced line —
		// the engine's dialogue state is blank throughout it — so the player was
		// falling to speakerHold and glancing away for roughly half of every line
		// they delivered. Looking away while you talk is a real thing people do,
		// but not when the camera has cut to your face for the delivery, and not on
		// every line.
		//
		// Read from the sound handle here rather than passed in, so this holds with
		// the topic fade off, with synthesized lipsync off, and with any player
		// voice mod: the handle is the one signal that does not depend on some
		// other feature being switched on.
		const bool playerTalking = VoiceHandlePlaying(player);

		if (playerTalking && !playerWasTalking) {
			// Mid-glance when the line starts. Waiting for the current aversion to
			// expire would leave the head turned away for up to 1.8s of it, which
			// is most of a short line.
			playerGaze.offset = {};
			playerGaze.averted = false;
			playerGaze.remaining = 0.0f;
		}
		playerWasTalking = playerTalking;

		StepGaze(npc.get(), npcGaze, a_delta, a_npcSpeaking ? speakerHold : listenerHold);
		StepGaze(player, playerGaze, a_delta,
			playerTalking ? 1.0f : (a_npcSpeaking ? listenerHold : speakerHold));

		if (gazeReported.Take()) {
			Log::Info(Log::Category::kStaging, "Gaze model running for both participants."sv);
		}
	}
}

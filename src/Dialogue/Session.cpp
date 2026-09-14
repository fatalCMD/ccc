#include "SD/Dialogue/Session.h"

#include "SD/Core/Logging.h"

namespace SD::Dialogue
{
	namespace
	{
		using Emotion = RE::EmotionType;

		std::string_view EmotionName(Emotion a_type) noexcept
		{
			switch (a_type) {
			case Emotion::kNeutral:  return "Neutral"sv;
			case Emotion::kAnger:    return "Anger"sv;
			case Emotion::kDisgust:  return "Disgust"sv;
			case Emotion::kFear:     return "Fear"sv;
			case Emotion::kSad:      return "Sad"sv;
			case Emotion::kHappy:    return "Happy"sv;
			case Emotion::kSurprise: return "Surprise"sv;
			case Emotion::kPuzzled:  return "Puzzled"sv;
			default:                 return "Unknown"sv;
			}
		}

		RE::Actor* AsActor(RE::ObjectRefHandle a_handle)
		{
			const auto ref = a_handle.get();
			return ref ? ref->As<RE::Actor>() : nullptr;
		}

		std::string NameOf(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return "<none>"s;
			}
			const char* name = a_actor->GetName();
			return (name && *name) ? std::string{ name } : "<unnamed>"s;
		}

		std::string Condense(const char* a_raw, std::size_t a_max)
		{
			if (!a_raw || !*a_raw) {
				return "<no text>"s;
			}
			std::string text{ a_raw };
			// Response text carries newlines; a log line should stay one line.
			std::replace(text.begin(), text.end(), '\n', ' ');
			std::replace(text.begin(), text.end(), '\r', ' ');
			if (text.size() > a_max) {
				text.resize(a_max);
				text += "..."sv;
			}
			return text;
		}

		// The head position the staging solver will frame against.
		//
		// Falls back to the root when the head node is missing rather than failing:
		// a creature or a headless custom race still has to be framed somehow, and
		// the caller cannot tell the difference from a null.
		std::optional<RE::NiPoint3> HeadOf(RE::Actor* a_actor)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return std::nullopt;
			}
			if (auto* node = root->GetObjectByName("NPC Head [Head]"sv)) {
				return node->world.translate;
			}
			return root->world.translate;
		}

		// Read the response list the manager already built for this topic info.
		//
		// Deliberately does not construct a DialogueItem to fill the gaps. That ctor
		// is a relocated engine call that allocates from the game heap, and guessing
		// at its lifetime during live dialogue is a poor trade for a field the
		// director can survive without. When the match fails the line is reported
		// unresolved and the log says so — which is exactly the evidence needed to
		// decide later whether the fallback is worth the risk.
		void ReadResponses(RE::MenuTopicManager* a_manager, RE::TESTopicInfo* a_info, Line& a_line)
		{
			auto* dialogue = a_manager->lastSelectedDialogue;
			if (!dialogue || dialogue->parentTopicInfo != a_info) {
				return;
			}

			std::size_t                 count = 0;
			const RE::DialogueResponse* first = nullptr;
			for (auto* response : dialogue->responses) {
				if (!response) {
					continue;
				}
				if (!first) {
					first = response;
				}
				++count;
			}

			if (!first) {
				return;
			}

			a_line.responseCount = count;
			a_line.emotion = first->animFaceArchType.get();
			a_line.emotionPercent = first->percent;
			a_line.text = Condense(first->text.c_str(), 90);
			a_line.hasSpeakerIdle = first->speakerIdle != nullptr;
			a_line.hasListenIdle = first->listenIdle != nullptr;
			a_line.resolved = true;
		}
	}

	Session& Session::GetSingleton() noexcept
	{
		static Session instance;
		return instance;
	}

	void Session::OnFrame(float a_delta)
	{
		auto* manager = RE::MenuTopicManager::GetSingleton();
		if (!manager) {
			return;
		}

		// THE TWO HANDLES, READ SEPARATELY. See Session.h for what each one means.
		//
		// speaker goes null the moment the menu closes, but the NPC keeps talking
		// through the farewell line. lastSpeaker is what carries the conversation
		// through that tail, and cutting away on the goodbye would be the most
		// visible possible failure — so the tail still keeps the session alive.
		// What it no longer does is look like live participation.
		auto*      live = AsActor(manager->speaker);
		auto*      tail = AsActor(manager->lastSpeaker);
		auto*      speaker = live ? live : tail;
		const bool inConversation = speaker != nullptr;

		// Published every frame, before any of the transitions below can end the
		// session — see PlayerEngaged.
		engaged = live != nullptr;

		if (inConversation && !active) {
			Enter(speaker);
			hadLive = live != nullptr;
			lostLive = false;
		} else if (!inConversation && active) {
			Exit();
			return;
		} else if (inConversation && active && speaker->GetFormID() != partnerID) {
			// The player walked from one conversation straight into another.
			//
			// The speaker handle never goes null in between, so watching only for
			// "in a conversation or not" left the session running with the previous
			// partner's handle — and anything keyed to the session opening never
			// fired for the new one.
			Log::Info(Log::Category::kDialogue,
				"Partner changed mid-session: [{:08X}] -> {} [{:08X}]; restarting."sv,
				partnerID, NameOf(speaker), speaker->GetFormID());
			Exit();
			Enter(speaker);
			hadLive = live != nullptr;
			lostLive = false;
		} else if (active && live && lostLive) {
			// INTERRUPTING SOMEBODY WHO NEVER STOPPED TALKING.
			//
			// The reported case: an NPC is part way through a line — a farewell the
			// player walked out on, or world chatter that left lastSpeaker set —
			// and the player activates them. The engine starts a genuinely new
			// conversation. Nothing in the old reading of this changed: same
			// actor, same form id, session already active, so no Enter fired, the
			// serial never moved, and Runtime's "one open per partner" rule
			// concluded it had already staged this one. The result was a second
			// conversation with no camera, no bars, and a topic list nothing was
			// driving — which is what "does not initialize correctly" looked like
			// from the outside.
			//
			// Ended and restarted exactly once, on the frame the live speaker comes
			// back, and never per frame: lostLive is cleared by the restart and can
			// only be set again by the speaker handle going null once more.
			Log::Info(Log::Category::kDialogue,
				"Re-entered dialogue with {} [{:08X}] while their previous line was "
				"still running; ending the old session and starting a new one."sv,
				NameOf(live), live->GetFormID());
			Exit();
			Enter(live);
			hadLive = true;
			lostLive = false;
		} else if (active) {
			// The bookkeeping for the branch above, and the only place it is set.
			//
			// A session entered from the tail alone — a forcegreet, an NPC talking
			// at the player with no menu — has never had a live speaker, so it can
			// never arm this. That is deliberate: the menu appearing partway
			// through a forcegreet is the same conversation continuing, not a new
			// one, and restarting it there would cost a cut for nothing.
			if (live) {
				hadLive = true;
				lostLive = false;
			} else if (hadLive) {
				lostLive = true;
			}
		}

		if (!active) {
			return;
		}

		sessionElapsed += a_delta;

		auto* info = manager->currentTopicInfo;
		if (info != lastInfo) {
			if (speaking) {
				EndLine();
			}
			lastInfo = info;
			if (info) {
				BeginLine(info, speaker);
			}
		}

		if (speaking) {
			lineElapsed += a_delta;
		}
	}

	void Session::Enter(RE::Actor* a_speaker)
	{
		active = true;
		speaking = false;
		sessionElapsed = 0.0f;
		lineElapsed = 0.0f;
		lineCount = 0;
		lastInfo = nullptr;
		current = {};
		partnerID = a_speaker ? a_speaker->GetFormID() : 0;
		partner = a_speaker ? a_speaker->GetHandle() : RE::ActorHandle{};

		// The identity everything downstream keys on. Bumped here and nowhere else,
		// so one Enter is one conversation however many times the same person is
		// spoken to. See ConversationSerial.
		++serial;

		auto*      player = RE::PlayerCharacter::GetSingleton();
		const auto speakerHead = HeadOf(a_speaker);
		const auto playerHead = HeadOf(player);

		float separation = -1.0f;
		if (speakerHead && playerHead) {
			separation = speakerHead->GetDistance(*playerHead);
		}

		Log::Info(Log::Category::kDialogue,
			"Conversation {} opened with {} [{:08X}] — separation {:.1f}u, speaker head {}, player head {}."sv,
			serial, NameOf(a_speaker), partnerID, separation,
			speakerHead ? "found"sv : "MISSING"sv,
			playerHead ? "found"sv : "MISSING"sv);
	}

	void Session::Exit()
	{
		if (speaking) {
			EndLine();
		}

		Log::Info(Log::Category::kDialogue,
			"Conversation closed after {:.1f}s and {} line(s)."sv, sessionElapsed, lineCount);

		active = false;
		speaking = false;
		lastInfo = nullptr;
		partnerID = 0;
		partner = {};
		current = {};
		lineElapsed = 0.0f;
		sessionElapsed = 0.0f;

		// Cleared with everything else. Left set, the first frame of the NEXT
		// session would see a live speaker with lostLive still armed and restart a
		// conversation that had only just begun.
		hadLive = false;
		lostLive = false;
		engaged = false;
	}

	void Session::BeginLine(RE::TESTopicInfo* a_info, RE::Actor* a_speaker)
	{
		auto* manager = RE::MenuTopicManager::GetSingleton();

		current = {};
		current.topicInfoID = a_info->GetFormID();
		current.speakerID = a_speaker ? a_speaker->GetFormID() : 0;
		current.speakerName = NameOf(a_speaker);
		current.greeting = manager && manager->isGreetingPlayer;
		// forceGoodbye is the same B2 flag CommonLibSSE used to call isSayingGoodbye;
		// it was renamed upstream, not replaced.
		current.farewell = manager && manager->forceGoodbye;

		if (manager) {
			ReadResponses(manager, a_info, current);
		}

		speaking = true;
		lineElapsed = 0.0f;
		++lineCount;

		if (current.resolved) {
			Log::Info(Log::Category::kDialogue,
				"Line {} start | {} [{:08X}] | {}({}%) | responses {} | idles spk={} lsn={}{}{} | \"{}\""sv,
				lineCount, current.speakerName, current.topicInfoID,
				EmotionName(current.emotion), current.emotionPercent,
				current.responseCount,
				current.hasSpeakerIdle ? "y"sv : "n"sv,
				current.hasListenIdle ? "y"sv : "n"sv,
				current.greeting ? " | GREETING"sv : ""sv,
				current.farewell ? " | FAREWELL"sv : ""sv,
				current.text);
		} else {
			// Expected whenever the NPC speaks something the player did not pick
			// from the menu — forcegreets, scene lines, idle chatter. How often this
			// fires decides whether the director needs a second source of truth.
			Log::Info(Log::Category::kDialogue,
				"Line {} start | {} [{:08X}] | UNRESOLVED (no matching response list){}{}"sv,
				lineCount, current.speakerName, current.topicInfoID,
				current.greeting ? " | GREETING"sv : ""sv,
				current.farewell ? " | FAREWELL"sv : ""sv);
		}
	}

	void Session::EndLine()
	{
		// The single most important number in this build. The cut policy needs to
		// know a line's length to decide whether it can be cut on at all, and
		// whether that length is knowable *before* the line plays or only after it
		// ends decides whether the director can plan a shot or must react to one.
		Log::Info(Log::Category::kDialogue,
			"Line {} end   | {:.2f}s | {} response(s){}"sv,
			lineCount, lineElapsed, current.responseCount,
			current.resolved ? ""sv : " | was unresolved"sv);

		speaking = false;
		lineElapsed = 0.0f;
	}

	void Session::Abandon()
	{
		active = false;
		speaking = false;
		lastInfo = nullptr;
		partnerID = 0;
		partner = {};
		current = {};
		lineElapsed = 0.0f;
		sessionElapsed = 0.0f;
		lineCount = 0;
		hadLive = false;
		lostLive = false;
		engaged = false;

		// The serial is NOT reset. It is an identity, not a count, and Runtime
		// compares it against one it recorded before the load — so restarting it at
		// zero could hand the new world a serial the old one had already staged.
	}
}

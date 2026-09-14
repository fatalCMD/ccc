#pragma once

namespace SD::Dialogue
{
	// One spoken line, resolved at the moment it starts.
	struct Line
	{
		RE::FormID  topicInfoID{ 0 };
		RE::FormID  speakerID{ 0 };
		std::string speakerName;
		std::string text;

		// The direction signal. Every authored response in the game carries an
		// emotion and an intensity, because the engine uses them to drive facegen.
		// Nothing has ever used them to choose a shot.
		RE::EmotionType emotion{ RE::EmotionType::kNeutral };
		std::uint16_t                     emotionPercent{ 0 };

		// A topic info can hold several responses spoken back to back. Vanilla does
		// not expose which one is playing through the manager, so the director sees
		// the group as one line for now; this count is how the log reports when that
		// approximation is about to be wrong.
		std::size_t responseCount{ 0 };

		// Authored staging that no mod has ever read: the animation the speaker and
		// the listener are meant to play on this line.
		bool hasSpeakerIdle{ false };
		bool hasListenIdle{ false };

		bool resolved{ false };  // false when the response list could not be matched
		bool greeting{ false };
		bool farewell{ false };
	};

	// Watches MenuTopicManager and turns it into conversation and line edges.
	//
	// v0 is deliberately observational — nothing here moves a camera. It exists so
	// that every signal the director will be built on can be checked against a
	// running game first. Prisma stalled because a frontend was built on a frame
	// source that turned out not to exist; proving the source before building on
	// it is the cheap version of that lesson.
	class Session
	{
	public:
		[[nodiscard]] static Session& GetSingleton() noexcept;

		void OnFrame(float a_delta);
		void Abandon();  // a load is starting; drop state without touching the world

		[[nodiscard]] bool            Active() const noexcept { return active; }
		[[nodiscard]] RE::ActorHandle Partner() const noexcept { return partner; }

		// WHICH CONVERSATION THIS IS, counted from load. Bumped by Enter().
		//
		// The partner's form id was the only identity a conversation had, and it is
		// not one: two conversations with the same person in a row are the same id,
		// and Runtime — which opens the camera once per partner and deliberately
		// will not reopen while that key matches — could not tell them apart.
		//
		// That is the whole of the reported "activate somebody who is already
		// talking and the conversation does not start properly". Leaving a
		// conversation while the NPC is still speaking keeps this session alive on
		// MenuTopicManager::lastSpeaker, which is correct — the line is still
		// running and cutting away from a farewell is the worst thing this mod can
		// do. Walk back and activate them again inside that window and the engine
		// starts a genuinely new conversation, but nothing here changed: same
		// speaker, same id, session already active. Runtime saw its key still
		// matching and never staged, so the second conversation ran with no camera,
		// no bars and a topic list nothing was driving.
		//
		// A serial makes the identity a conversation rather than a person.
		[[nodiscard]] std::uint32_t ConversationSerial() const noexcept { return serial; }

		// IS THE PLAYER STILL IN THIS CONVERSATION, as opposed to standing outside
		// one that has not finished talking at them?
		//
		// Active() is true for both, and has to be: cutting away from a farewell is
		// the worst thing a dialogue camera can do, so the tail keeps the session
		// alive. This is the narrower question, and the one that decides whether a
		// cinematic suspended for an inventory or a barter window is owed a return.
		// Come back from a trade the player ended by walking out and the answer is
		// no — there is a line still running, and nothing left to frame it for.
		[[nodiscard]] bool PlayerEngaged() const noexcept { return engaged; }
		[[nodiscard]] bool        Speaking() const noexcept { return speaking; }
		[[nodiscard]] const Line& Current() const noexcept { return current; }
		[[nodiscard]] float       LineElapsed() const noexcept { return lineElapsed; }

	private:
		void Enter(RE::Actor* a_speaker);
		void Exit();
		void BeginLine(RE::TESTopicInfo* a_info, RE::Actor* a_speaker);
		void EndLine();

		bool              active{ false };
		bool              speaking{ false };
		Line              current{};
		RE::TESTopicInfo* lastInfo{ nullptr };
		RE::FormID        partnerID{ 0 };
		RE::ActorHandle   partner{};
		std::uint32_t     serial{ 0 };

		// THE TWO HALVES OF "IS THE PLAYER ACTUALLY IN THIS CONVERSATION".
		//
		// MenuTopicManager has two speaker handles and they mean different things.
		// `speaker` is live participation: the player is in the conversation and
		// the engine is holding it open for them. `lastSpeaker` is the tail — the
		// player has left and the NPC is finishing whatever they were part way
		// through saying.
		//
		// Collapsing the two, which is what this used to do, is right for keeping a
		// session alive across a farewell and wrong for everything else, because it
		// makes the end of a conversation and the middle of one indistinguishable.
		//
		//   hadLive   this session has been observed with a live speaker at least
		//             once, so it is a conversation the player entered rather than
		//             a forcegreet observed from its trailing line.
		//   lostLive  ...and has since lost it. A live speaker arriving while this
		//             is set is the player starting a NEW conversation with
		//             somebody who never stopped talking from the last one.
		bool hadLive{ false };
		bool lostLive{ false };

		// This frame's answer to PlayerEngaged. Written every frame the session is
		// alive rather than kept as an edge, so a caller asking on any frame gets
		// the state as it is rather than as it last changed.
		bool engaged{ false };
		float             lineElapsed{ 0.0f };
		float             sessionElapsed{ 0.0f };
		std::uint32_t     lineCount{ 0 };
	};
}

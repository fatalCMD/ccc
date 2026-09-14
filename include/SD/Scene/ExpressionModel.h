#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "SD/Core/Text.h"

namespace SD::Scene::Expressions
{
	enum Emotion : std::uint32_t
	{
		kNeutral, kAnger, kDisgust, kFear, kSad, kHappy, kSurprise, kPuzzled
	};
	enum Expression : std::int32_t
	{
		kDialogueAnger, kDialogueFear, kDialogueHappy, kDialogueSad,
		kDialogueSurprise, kDialoguePuzzled, kDialogueDisgusted, kMoodNeutral
	};
	struct Reading
	{
		std::uint32_t emotion{ kNeutral };
		std::uint16_t percent{ 50 };
		bool inferred{ false };
	};
	inline constexpr std::uint32_t kSlots = 17;
	using Shape = std::array<float, kSlots>;

	[[nodiscard]] inline std::int32_t ExpressionFor(std::uint32_t emotion)
	{
		constexpr std::array slots{ kMoodNeutral, kDialogueAnger, kDialogueDisgusted,
			kDialogueFear, kDialogueSad, kDialogueHappy, kDialogueSurprise, kDialoguePuzzled };
		return emotion < slots.size() ? slots[emotion] : kMoodNeutral;
	}
	[[nodiscard]] inline bool Word(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			static_cast<unsigned char>(c) >= 128;
	}
	[[nodiscard]] inline bool Contains(std::string_view text, std::string_view phrase)
	{
		for (std::size_t pos = text.find(phrase); pos != std::string_view::npos; pos = text.find(phrase, pos + 1)) {
			const auto end = pos + phrase.size();
			if ((pos == 0 || !Word(text[pos - 1])) &&
				(end == text.size() || !Word(text[end]))) {
				return true;
			}
		}
		return false;
	}
	[[nodiscard]] inline unsigned Hits(std::string_view text, std::initializer_list<std::string_view> phrases)
	{
		unsigned hits = 0;
		for (auto phrase : phrases) {
			hits += Contains(text, phrase) ? 1u : 0u;
		}
		return hits;
	}
	// Small, explicit English cues supplement neutral records. This is not an
	// interpretation of voice tone; non-neutral authored emotion takes priority.
	[[nodiscard]] inline Reading EmotionFromText(std::string_view line)
	{
		std::string text;
		text.reserve(line.size());
		for (std::size_t pos = 0; pos < line.size();) {
			const auto begin = pos;
			const auto cp = Text::NextCodepoint(line, pos);
			if (cp == 0x2018 || cp == 0x2019) text.push_back('\'');
			else if (cp < 128) text.push_back(cp == U'_' ? ' ' : Text::AsciiLower(static_cast<char>(cp)));
			else text.append(line.substr(begin, pos - begin));
		}
		bool exclamation = false;
		for (std::size_t pos = 0; pos < line.size();) {
			const auto cp = Text::NextCodepoint(line, pos);
			exclamation |= cp == U'!' || cp == U'\uFF01';
		}
		const bool deniesFear = Hits(text, { "not afraid", "not scared", "don't be afraid", "nothing to fear" }) > 0;
		struct Candidate { std::uint32_t emotion; unsigned hits; };
		const Candidate candidates[]{
			{ kAnger, Hits(text, { "how dare", "shut up", "damn you", "coward", "liar", "traitor",
				"enough of", "get out", "you fool", "kill you", "i'll kill", "betrayed me",
				"i hate", "you'll pay", "leave me alone", "intimidate" }) },
			{ kDisgust, Hits(text, { "disgusting", "revolting", "vile", "filth", "wretched", "sickens me" }) },
			{ kSad, Hits(text, { "i'm sorry", "im sorry", "my condolences", "he died", "she died", "they died",
				"is dead", "passed away", "mourn", "grief", "i regret", "a shame", "i miss",
				"forgive me", "heartbroken", "tragic", "lost my", "miss him", "miss her" }) },
			{ kFear, deniesFear ? 0u : Hits(text, { "afraid", "scared", "frightened", "terrified",
				"please don't", "spare me", "help me", "be careful", "isn't entirely secured", "dangers of",
				"defend yourself", "watch out" }) },
			{ kHappy, Hits(text, { "thank you", "thanks", "i'm glad", "im glad", "wonderful", "excellent",
				"my pleasure", "gladly", "good to see", "delighted", "you're welcome", "i adore", "i love",
				"i appreciate", "you're kind", "well done", "good work", "happy to", "pleased to", "fascinating" }) },
			{ kSurprise, Hits(text, { "really?", "you're joking", "youre joking", "can't believe", "cant believe",
				"impossible", "incredible", "what?!", "truly?", "are you serious", "how in the world", "what in the world" }) },
			{ kPuzzled, Hits(text, { "don't understand", "do not understand", "i wonder", "perhaps", "somehow", "no idea" }) }
		};
		Reading result{};
		unsigned best = 0;
		for (const auto candidate : candidates) {
			if (candidate.hits > best) {
				best = candidate.hits;
				result.emotion = candidate.emotion;
			}
		}
		if (result.emotion != kNeutral) {
			result.percent = static_cast<std::uint16_t>(best > 0 ? (exclamation ? 85 : 70) : 55);
			result.inferred = true;
		}
		return result;
	}
	[[nodiscard]] inline Reading Resolve(std::uint32_t authored, std::uint16_t percent, std::string_view text)
	{
		if (authored > kNeutral && authored <= kPuzzled && percent > 0) {
			return { authored, std::min<std::uint16_t>(percent, 100), false };
		}
		return EmotionFromText(text);
	}
	[[nodiscard]] inline Reading ReactionTo(std::uint32_t emotion, std::uint16_t percent)
	{
		// A listener acknowledges the speaker's state instead of copying a
		// hostile expression back at them. The caller applies a softer strength.
		switch (emotion) {
		case kAnger: return { percent >= 95 ? kFear : kPuzzled, percent };
		case kFear: return { kSad, percent };  // concern, not a confused response to danger
		case kDisgust: return { kPuzzled, percent };
		case kSad: return { kSad, percent };
		case kHappy: return { kHappy, percent };
		case kSurprise: return { kSurprise, percent };
		case kPuzzled: return { kPuzzled, percent };
		default: return { kNeutral, 0 };
		}
	}
	[[nodiscard]] inline float AuthoredStrength(std::int32_t index, std::uint16_t percent)
	{
		const float value = std::clamp(static_cast<float>(percent) / 100.0f, 0.0f, 1.0f);
		return index == kMoodNeutral ? value * 0.20f : value;
	}
	[[nodiscard]] inline Shape MakeShape(std::int32_t index, float strength)
	{
		Shape out{};
		if (index < 0 || index >= static_cast<std::int32_t>(kSlots) || !std::isfinite(strength)) {
			return out;
		}
		out[static_cast<std::size_t>(index)] = std::clamp(strength, 0.0f, 1.0f);
		return out;
	}
	[[nodiscard]] inline float LineGain(float age, bool speaking)
	{
		// A readable initial reaction settles without freezing at its peak.
		return speaking ? 0.88f + 0.12f * std::exp(-std::max(age, 0.0f) / 1.8f) : 0.38f;
	}
	inline bool Step(Shape& current, const Shape& target, float delta)
	{
		if (!std::isfinite(delta) || delta < 0.0f) {
			return std::any_of(current.begin(), current.end(), [](float v) { return v > 0.0f; });
		}
		delta = std::min(delta, 0.1f);
		bool any = false;
		for (std::size_t i = 0; i < current.size(); ++i) {
			const float goal = std::isfinite(target[i]) ? std::clamp(target[i], 0.0f, 1.0f) : 0.0f;
			const float time = goal > current[i] ? 0.12f : 0.38f;
			current[i] += (goal - current[i]) * (1.0f - std::exp(-delta / time));
			if (current[i] < 0.002f) current[i] = 0.0f;
			any |= current[i] > 0.0f;
		}
		return any;
	}
}

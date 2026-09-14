#pragma once

#include "SD/Scene/ExpressionProfiles.h"

namespace SD::Scene::Acting
{
	// Conversational action is independent of emotional tone. These names are
	// authoring directions, not a claim that text reveals a person's feelings.
	enum class Action { Tell, Ask, Confirm, Doubt, Explain, Reassure, Greet };
	enum class Evidence { Neutral, Grammar, Explicit, Authored };
	inline constexpr std::size_t kMaxBeats = 8;
	struct Beat
	{
		Action action{ Action::Tell };
		Expressions::Reading tone{ Expressions::kNeutral, 0 };
		Evidence evidence{ Evidence::Neutral };
		float begin{}, end{ 1 };
		unsigned words{ 1 };
	};
	struct Plan
	{
		std::array<Beat, kMaxBeats> beats{};
		std::size_t count{ 1 };
		unsigned words{};
	};
	[[nodiscard]] inline std::string_view Name(Action value)
	{
		switch (value) {
		case Action::Ask: return "inquiry";
		case Action::Confirm: return "confirmation";
		case Action::Doubt: return "skepticism";
		case Action::Explain: return "explanation";
		case Action::Reassure: return "reassurance";
		case Action::Greet: return "greeting";
		default: return "statement";
		}
	}
	[[nodiscard]] inline std::string_view Name(Evidence value)
	{
		switch (value) {
		case Evidence::Grammar: return "grammar";
		case Evidence::Explicit: return "explicit-text";
		case Evidence::Authored: return "authored";
		default: return "neutral";
		}
	}
	[[nodiscard]] inline std::string Normalize(std::string_view input)
	{
		std::string text;
		// Only planning allocates. Frame sampling is fixed-size arithmetic.
		input = input.substr(0, 4096);
		for (std::size_t i = 0; i < input.size();) {
			const auto cp = Text::NextCodepoint(input, i);
			if (cp == 0x201C || cp == 0x201D) text += '"';
			else if (cp == 0x2018 || cp == 0x2019) text += '\'';
			else if (cp == 0xFF1F || cp == 0x061F) text += '?';
			else if (cp == 0xFF01) text += '!';
			else if (cp == 0x3002) text += '.';
			else if (cp < 128) text += Text::AsciiLower(static_cast<char>(cp));
			else text += ' '; // Unsupported languages get no invented English tone.
		}
		return text;
	}
	[[nodiscard]] inline unsigned Words(std::string_view text)
	{
		unsigned count = 0;
		bool was = false;
		for (char c : text) {
			const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'';
			if (word && !was) ++count;
			was = word;
		}
		return count;
	}
	[[nodiscard]] inline bool Starts(std::string_view text, std::initializer_list<std::string_view> prefixes)
	{
		const auto start = text.find_first_not_of(" \t\r\n\"");
		if (start == std::string_view::npos) return false;
		text.remove_prefix(start);
		for (auto prefix : prefixes) if (text.starts_with(prefix) &&
			(text.size() == prefix.size() || !Expressions::Word(text[prefix.size()]))) return true;
		return false;
	}
	[[nodiscard]] inline Beat Read(std::string_view text, Expressions::Reading authored = {})
	{
		using namespace Expressions;
		Beat beat;
		beat.words = std::max(1u, Words(text));
		// Quoted speech contributes duration but not the speaker's own tone.
		std::string unquoted(text);
		bool quoted = false;
		for (auto& c : unquoted) {
			if (c == '"') { quoted = !quoted; c = ' '; }
			else if (quoted) c = ' ';
		}
		text = unquoted;
		for (auto lead : { "but", "yet", "however", "well", "so", "and" }) {
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first != std::string_view::npos && Starts(text, { lead })) {
				text.remove_prefix(first + std::string_view(lead).size());
				break;
			}
		}
		const bool wh = Starts(text, { "why", "what", "where", "when", "who", "whose", "which", "how" });
		const bool question = text.find('?') != std::string_view::npos || wh;
		if (Hits(text, { "are you sure", "can i trust", "i don't believe", "i do not believe", "doesn't add up", "makes no sense" }))
			beat.action = Action::Doubt;
		else if (Hits(text, { "don't be afraid", "nothing to fear", "you're safe", "i will protect you", "not afraid", "not scared" }))
			beat.action = Action::Reassure;
		else if (question) beat.action = wh ? Action::Ask : Action::Confirm;
		else if (Starts(text, { "tell me", "explain", "i'd like to know" })) beat.action = Action::Ask;
		else if (Starts(text, { "because", "it's simple", "the reason", "that means", "in other words" })) beat.action = Action::Explain;
		else if (Starts(text, { "greetings", "hello", "good morning", "good evening", "well met" })) beat.action = Action::Greet;
		if (beat.action != Action::Tell) beat.evidence = Evidence::Grammar;
		// Explicit first-person/social cues only. Mentioning death, dragons or
		// fighting is not enough to make a speaker sad, afraid or angry.
		if (authored.emotion > kNeutral && authored.emotion <= kPuzzled && !authored.inferred && authored.percent > 0) {
			beat.tone = authored;
			beat.tone.percent = std::min<std::uint16_t>(authored.percent, 100);
			beat.evidence = Evidence::Authored;
		} else {
			const bool negated = Hits(text, { "not glad", "not happy", "not sorry", "not afraid", "not scared",
				"don't hate", "do not hate", "won't kill", "will not kill" }) > 0;
			if (negated) return beat;
			if (Hits(text, { "thank you", "thanks", "good to see you", "i'm glad", "my pleasure", "well done" })) beat.tone = { kHappy, 70, true };
			else if (Hits(text, { "my condolences", "i'm sorry", "i miss you", "i miss him", "i miss her", "forgive me" })) beat.tone = { kSad, 65, true };
			else if (Hits(text, { "how dare you", "i hate you", "you'll pay", "shut up", "i'll kill you", "intimidate" })) beat.tone = { kAnger, 75, true };
			else if (Hits(text, { "i'm afraid", "i'm scared", "i'm terrified", "spare me" }) && beat.action != Action::Reassure) beat.tone = { kFear, 65, true };
			else if (Hits(text, { "that's disgusting", "that is disgusting", "sickens me" })) beat.tone = { kDisgust, 65, true };
			else if (Hits(text, { "i can't believe", "you're joking", "what a surprise" })) beat.tone = { kSurprise, 70, true };
			if (beat.tone.emotion != kNeutral) beat.evidence = Evidence::Explicit;
		}
		return beat;
	}
	[[nodiscard]] inline Plan Build(std::string_view input, Expressions::Reading authored = {})
	{
		const auto text = Normalize(input);
		Plan plan;
		plan.count = 0;
		std::size_t start = 0;
		bool quoted = false;
		for (std::size_t i = 0; i <= text.size(); ++i) {
			if (i < text.size() && text[i] == '"') quoted = !quoted;
			const bool commaClause = i < text.size() && text[i] == ',' && Starts(std::string_view(text).substr(i + 1),
				{ "but", "yet", "however", "why", "what", "how", "where", "when", "who", "i'm sorry", "thank you" });
			const bool boundary = i == text.size() || (!quoted && (text[i] == '.' || text[i] == '?' || text[i] == '!' || text[i] == ';' || text[i] == ':' || commaClause));
			if (!boundary) continue;
			const auto end = i < text.size() ? i + 1 : i;
			const auto clause = std::string_view(text).substr(start, end - start);
			if (Words(clause)) {
				if (plan.count == kMaxBeats) { // Bound planning; extend the last beat, do not index past it.
					plan.beats.back().words += Words(clause);
				} else plan.beats[plan.count++] = Read(clause, authored);
			}
			start = end;
		}
		if (!plan.count) { plan.count = 1; plan.beats[0] = Read({}, authored); }
		float total = 0;
		for (std::size_t i = 0; i < plan.count; ++i) { total += plan.beats[i].words + .6f; plan.words += plan.beats[i].words; }
		float cursor = 0;
		for (std::size_t i = 0; i < plan.count; ++i) {
			auto& beat = plan.beats[i];
			beat.begin = cursor / total;
			cursor += beat.words + .6f;
			beat.end = cursor / total;
		}
		return plan;
	}
	[[nodiscard]] inline std::size_t Index(const Plan& plan, float age, float duration)
	{
		const auto count = std::clamp<std::size_t>(plan.count, 1, kMaxBeats);
		const float position = std::isfinite(age) && std::isfinite(duration) && duration > 0 ? std::clamp(age / duration, 0.0f, 1.0f) : 0;
		for (std::size_t i = 0; i + 1 < count; ++i) if (position < plan.beats[i].end) return i;
		return count - 1;
	}
	using Pose = ExpressionProfiles::Pose;
	[[nodiscard]] inline ExpressionProfiles::ID ProfileFor(const Beat& beat)
	{
		using namespace Expressions;
		using ID = ExpressionProfiles::ID;
		if (beat.tone.percent > 0) {
			switch (beat.tone.emotion) {
			case kAnger: return ID::Anger;
			case kDisgust: return ID::Disgust;
			case kFear: return ID::Fear;
			case kSad: return ID::Sad;
			case kHappy: return ID::Happy;
			case kSurprise: return ID::Surprise;
			case kPuzzled: return ID::Puzzled;
			default: break;
			}
		}
		switch (beat.action) {
		case Action::Ask: return ID::Inquiry;
		case Action::Confirm: return ID::Confirmation;
		case Action::Doubt: return ID::Skepticism;
		case Action::Explain: return ID::Explanation;
		case Action::Reassure: return ID::Reassurance;
		case Action::Greet: return ID::Greeting;
		default: return ID::Neutral;
		}
	}
	[[nodiscard]] inline Pose Recipe(const Beat& beat, bool listener,
		const ExpressionProfiles::Library& profiles = ExpressionProfiles::kDefaults)
	{
		const auto selected = ProfileFor(beat);
		auto out = ExpressionProfiles::Resolve(profiles[static_cast<std::size_t>(selected)]);
		// One complete authored pose: never layer a question brow over anger, etc.
		const bool emotional = selected >= ExpressionProfiles::ID::Anger;
		const float strength = emotional ? std::sqrt(std::clamp(beat.tone.percent / 100.0f, 0.0f, 1.0f)) : 1.0f;
		for (auto& v : out.modifiers) v *= strength * (listener ? .72f : 1.0f);
		for (auto& v : out.regional) v *= strength * (listener ? .75f : 1.0f);
		return out;
	}
	[[nodiscard]] inline float Smooth(float value)
	{
		value = std::clamp(value, 0.0f, 1.0f);
		return value * value * (3 - 2 * value);
	}
	[[nodiscard]] inline Pose Sample(const Plan& plan, float age, float duration, bool listener, float intensity,
		const ExpressionProfiles::Library& profiles = ExpressionProfiles::kDefaults)
	{
		if (!std::isfinite(age) || !std::isfinite(duration) || !std::isfinite(intensity) || intensity <= 0 || duration <= 0) return {};
		const auto& beat = plan.beats[Index(plan, age, duration)];
		const float begin = beat.begin * duration;
		const float span = std::max(.15f, (beat.end - beat.begin) * duration);
		const float local = age - begin - (listener ? .15f : 0.0f);
		// A new clause cannot act before its estimated onset. Questions hold
		// through their own clause, not through the preceding declaration.
		const float attack = Smooth(local / std::min(.24f, span * .25f));
		const bool question = beat.action == Action::Ask || beat.action == Action::Confirm || beat.action == Action::Doubt;
		float settle = question ? .72f : (beat.tone.emotion == Expressions::kSurprise ? .22f : .48f);
		const float decayStart = std::min(.8f, span * .4f);
		const float decay = Smooth((local - decayStart) / std::max(.3f, span * .45f));
		const float endFade = std::exp(-std::max(0.0f, age - duration) / .7f);
		const float gain = attack * (1 - (1 - settle) * decay) * endFade * std::clamp(intensity, 0.0f, 2.0f) * 1.35f;
		auto pose = Recipe(beat, listener, profiles);
		for (auto& v : pose.modifiers) v = ExpressionProfiles::SoftLimit(v * gain);
		for (auto& v : pose.regional) v = ExpressionProfiles::SoftLimit(v * gain);
		// Signed profile lifts already exclude contradictory up/down targets.
		return pose;
	}
}

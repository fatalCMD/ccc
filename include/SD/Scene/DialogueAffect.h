#pragma once

#include "SD/Scene/ExpressionModel.h"

namespace SD::Scene::Affect
{
	enum class Intent { Attention, Curiosity, Concern, Resolve, Doubt };

	[[nodiscard]] inline Intent ReadIntent(std::string_view line)
	{
		std::string text;
		text.reserve(line.size());
		for (char c : line) text.push_back(Text::AsciiLower(c));
		using Expressions::Hits;
		if (Hits(text, { "whose side", "can i trust", "don't believe", "do not believe", "are you sure" }))
			return Intent::Doubt;
		if (Hits(text, { "nothing to fear", "not afraid", "don't be afraid", "no danger" }))
			return Intent::Resolve;
		if (Hits(text, { "protect", "killed", "murder", "attack", "terrors", "danger", "slay",
			"giant", "dragons", "bounty", "threat", "destroy these monsters" }))
			return Intent::Concern;
		if (Hits(text, { "courage", "vigilance", "loyalty", "allegiance", "draw my sword", "the war", "good planning" }))
			return Intent::Resolve;
		if (line.find('?') != std::string_view::npos || Hits(text, { "tell me", "i'd like to know", "explain" }))
			return Intent::Curiosity;
		return Intent::Attention;
	}

	[[nodiscard]] inline Expressions::Reading ContextReading(Expressions::Reading value, Intent intent)
	{
		using namespace Expressions;
		if (value.emotion != kNeutral) return value;
		switch (intent) {
		case Intent::Concern: return { kSad, 60, true };
		case Intent::Doubt: return { kPuzzled, 65, true };
		default: return value;
		}
	}
}

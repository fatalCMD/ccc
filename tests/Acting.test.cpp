#include "SD/Scene/Acting.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
	void Expect(bool value, const char* why) {
		if (!value) { std::cerr << why << '\n'; std::exit(EXIT_FAILURE); }
	}
}
int main()
{
	using namespace SD::Scene;
	using namespace Expressions;
	using Acting::Action;
	const auto late = Acting::Build("I wouldn't have expected to see one of the Legion here. Why Ivarstead?");
	Expect(late.count == 2 && late.beats[0].action == Action::Tell && late.beats[1].action == Action::Ask, "Carius statement then inquiry");
	Expect(late.beats[1].begin > .75f, "late question is not scheduled at the generic opening or 62 percent");
	Expect(Acting::Index(late, 1, 4.04f) == 0 && Acting::Index(late, 3.8f, 4.04f) == 1, "measured duration retimes clause positions");
	const auto early = Acting::Build("Why would you want to learn about them? Killing is simple enough.");
	Expect(early.count == 2 && early.beats[0].action == Action::Ask && early.beats[1].action == Action::Tell, "question then statement");
	Expect(early.beats[1].tone.emotion == kNeutral, "killing as a topic is not inferred anger or sadness");
	for (auto text : { "Why Ivarstead?", "What have you learned in your travels?", "Tell me about your travels.", "Where are you going" })
		Expect(Acting::Build(text).beats[0].action == Action::Ask, "information request has inquiry recipe");
	Expect(Acting::Build("Do you live here?").beats[0].action == Action::Confirm, "confirmation is not a wh-question");
	Expect(Acting::Build("Are you sure?").beats[0].action == Action::Doubt, "skepticism has its own action");
	Expect(Acting::Build("Because I was asked to help.").beats[0].action == Action::Explain, "explanation has its own action");
	Expect(Acting::Build("Greetings.").beats[0].action == Action::Greet, "Carius greeting differs from a plain statement");
	Expect(Acting::Build("You’re safe. There is nothing to fear.").beats[1].action == Action::Reassure, "reassurance is not fear");
	for (auto text : { "They killed a dragon.", "How can you protect the city?", "The war is over.",
		"I'm not afraid.", "I'm not sorry.", "I don't hate you.", "He said \"I'll kill you\".",
		"Where is the temple?", "Why would I be afraid?" })
		Expect(Acting::Build(text).beats[0].tone.emotion == kNeutral, "ambiguous, negated or quoted material cannot invent strong tone");
	Expect(Acting::Build("Thank you.").beats[0].tone.emotion == kHappy, "explicit gratitude has warmth");
	Expect(Acting::Build("I'm sorry.").beats[0].tone.emotion == kSad, "explicit apology has concern");
	Expect(Acting::Build("I'm scared.").beats[0].tone.emotion == kFear, "explicit fear remains available");
	Expect(Acting::Build("How dare you!").beats[0].tone.emotion == kAnger, "explicit anger remains available");
	Expect(Acting::Build("Thank you.", { kSad, 80 }).beats[0].tone.emotion == kSad, "authored tone takes priority over text");
	const auto turn = Acting::Build("Thank you. I'm sorry.");
	Expect(turn.beats[0].tone.emotion == kHappy && turn.beats[1].tone.emotion == kSad, "different clauses retain different tones");
	const auto contrast = Acting::Build("Thank you, but I'm sorry. Well, why Ivarstead?");
	Expect(contrast.count == 4 && contrast.beats[0].tone.emotion == kHappy && contrast.beats[1].tone.emotion == kSad &&
		contrast.beats[3].action == Action::Ask, "contrast clauses preserve emotional changes and question grammar");
	const auto plain = Acting::Recipe(Acting::Build("I live here.").beats[0], false);
	const auto ask = Acting::Recipe(late.beats[1], false);
	const auto confirm = Acting::Recipe(Acting::Build("Do you live here?").beats[0], false);
	const auto doubt = Acting::Recipe(Acting::Build("Are you sure?").beats[0], false);
	Expect(ask.modifiers[2] > plain.modifiers[2] * 5 && ask.modifiers[7] > confirm.modifiers[7], "inquiry has knit and focus, not just raised brows");
	Expect(ask.modifiers[4] < ask.modifiers[5] && doubt.modifiers[0] > ask.modifiers[0], "purposeful asymmetry and stronger skepticism differ");
	Expect(ask.regional[kPuzzled] > 0 && ask.regional[kSurprise] == 0, "inquiry does not use the eye-widening surprise region");
	Expect(Acting::Sample(late, 1, 4.04f, false, 1).regional[kPuzzled] == 0, "future question does not leak into preceding statement");
	Expect(Acting::Sample(turn, .01f, 4, true, 1).modifiers == Brows::Shape{}, "listener onset cannot anticipate information");
	for (auto text : { "", "!!!", "Why here？", "لماذا؟", "One. Two. Three. Four. Five. Six. Seven. Eight. Nine. Ten.", "He said \"Why here?\". Why there?" }) {
		const auto plan = Acting::Build(text);
		Expect(plan.count >= 1 && plan.count <= Acting::kMaxBeats, "bounded nonempty plan");
		for (std::size_t i = 0; i < plan.count; ++i) {
			Expect(plan.beats[i].begin < plan.beats[i].end, "ordered positive spans");
			if (i) Expect(plan.beats[i].begin == plan.beats[i-1].end, "no gaps or overlaps");
		}
		for (float strength : { 0.0f, .98f, 2.0f, 1000.0f }) for (int frame = 0; frame < 1000; ++frame) {
			const auto pose = Acting::Sample(plan, frame / 60.0f, 4, false, strength);
			for (float v : pose.modifiers) Expect(std::isfinite(v) && v >= 0 && v <= .65001f, "safe modifier sample");
			for (float v : pose.regional) Expect(std::isfinite(v) && v >= 0 && v <= .65001f, "safe regional sample");
			for (std::size_t side = 0; side < 2; ++side) Expect(pose.modifiers[side] + pose.modifiers[side+4] <= .65001f, "opposing directions share budget");
		}
	}
	Expect(Acting::Build("Why here？").beats[0].action == Action::Ask, "unicode question mark normalized");
	Expect(Acting::Build(std::string(10000, 'x')).count == 1, "oversize input is bounded");
	for (auto action : { Action::Tell, Action::Ask, Action::Confirm, Action::Doubt, Action::Explain, Action::Reassure, Action::Greet }) {
		for (std::uint32_t emotion = kNeutral; emotion <= kPuzzled; ++emotion) {
			Acting::Plan plan;
			plan.beats[0].action = action;
			plan.beats[0].tone = { emotion, 100 };
			for (float duration : { .15f, 1.0f, 4.0f, 120.0f }) {
				for (int frame = 0; frame < 120; ++frame) {
					const auto pose = Acting::Sample(plan, frame * duration / 120, duration, false, 2);
					for (float v : pose.modifiers) Expect(std::isfinite(v) && v >= 0 && v <= .65001f, "all action/tone combinations bounded");
					for (std::size_t side = 0; side < 2; ++side)
						Expect(pose.modifiers[side] + pose.modifiers[side+4] <= .65001f, "all combinations obey opposing-brow budget");
				}
			}
		}
	}
	const auto quoted = Acting::Build("He said \"Why here?\". Why there?");
	Expect(quoted.count == 2 && quoted.beats[0].action == Action::Tell && quoted.beats[0].words == 4,
		"quoted question contributes timing but does not become the player's question");
	for (float bad : { -1.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
		Expect(Acting::Sample(late, 1, 4, false, bad).modifiers == Brows::Shape{}, "invalid or zero strength is inert");
	std::cout << "Acting semantics, chronology, distinct recipes and safety passed.\n";
}

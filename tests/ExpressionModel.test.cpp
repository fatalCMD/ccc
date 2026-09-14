#include "SD/Scene/ExpressionModel.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "Expressions: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	using namespace SD::Scene::Expressions;
	const auto authored = Resolve(kSad, 80, "Thank you! Wonderful!");
	Expect(authored.emotion == kSad && authored.percent == 80 && !authored.inferred,
		"authored NPC emotion outranks contradictory text cues");
	Expect(Resolve(kHappy, 250, {}).percent == 100, "authored intensity is bounded");
	Expect(Resolve(kHappy, 0, "I'm sorry.").emotion == kSad,
		"zero intensity records can use explicit text cues");
	Expect(Resolve(kNeutral, 50, "Please be careful here. The site isn't entirely secured.").emotion == kFear,
		"Tolfdir's neutral-authored caution supplies a concerned listener reaction");
	Expect(Resolve(kNeutral, 50, "Anything! Anything at all that might be of interest. That's why I adore this location.").emotion == kHappy,
		"enthusiasm in a neutral-authored record is recognized");
	Expect(Resolve(kNeutral, 50, "What in the world was that racket? Is everything all right?").emotion == kSurprise,
		"a startled response does not stay neutral");
	Expect(EmotionFromText("How in the world did that happen?").emotion == kSurprise,
		"surprise takes precedence over generic question punctuation");
	Expect(EmotionFromText("Perhaps the amulet is important somehow.").emotion == kPuzzled,
		"uncertainty has a distinct reading");
	Expect(EmotionFromText("How dare you! You'll pay for this!").emotion == kAnger, "explicit threats read as anger");
	Expect(EmotionFromText("That is revolting.").emotion == kDisgust, "disgust is distinct");
	Expect(EmotionFromText("I'm sorry. He died.").emotion == kSad, "grief reads as sadness");
	Expect(EmotionFromText("What have you got for sale?").emotion == kNeutral, "ordinary questions do not invent confusion");
	Expect(EmotionFromText("Thank_you").emotion == kHappy, "voice filename keys remain useful as fallback text");
	Expect(EmotionFromText("I\xe2\x80\x99m glad.").emotion == kHappy, "curly apostrophes match English cues");
	Expect(EmotionFromText("Well!").emotion == kNeutral, "punctuation alone cannot invent anger");
	Expect(EmotionFromText("You have skill. These are familiar places.").emotion == kNeutral,
		"emotion phrases require word boundaries");
	Expect(EmotionFromText("I'm not afraid.").emotion == kNeutral, "negated fear is not played as fear");
	Expect(EmotionFromText("\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x9f").emotion == kNeutral,
		"unknown-language questions do not invent an emotional reading");
	Expect(Resolve(999, 50, {}).emotion == kNeutral, "unknown metadata is safe");

	constexpr std::array expected{ kMoodNeutral, kDialogueAnger, kDialogueDisgusted, kDialogueFear,
		kDialogueSad, kDialogueHappy, kDialogueSurprise, kDialoguePuzzled };
	for (std::uint32_t emotion = 0; emotion < expected.size(); ++emotion) {
		Expect(ExpressionFor(emotion) == expected[emotion], "record emotion maps to the correct engine morph");
	}
	const auto listener = ReactionTo(kAnger, 80);
	Expect(listener.emotion == kPuzzled, "listener acknowledges anger instead of copying hostility");
	Expect(ReactionTo(kHappy, 80).emotion == kHappy && ReactionTo(kSad, 80).emotion == kSad,
		"listeners share positive emotion and acknowledge grief");
	const auto listening = ReactionTo(kNeutral, 50);
	Expect(listening.emotion == kNeutral && listening.percent == 0,
		"neutral NPC exposition retains relaxed attention without a puzzled expression");
	Expect(ReactionTo(kFear, 70).emotion == kSad, "a warning gets concern instead of confusion");
	Expect(EmotionFromText("Be prepared to defend yourself.").emotion == kFear,
		"the logged warning is recognized as concern");
	Expect(EmotionFromText("I have no idea what connection they'd have to this place.").emotion == kPuzzled,
		"the logged uncertainty receives a matching reading");
	for (std::int32_t slot = kDialogueAnger; slot <= kDialogueDisgusted; ++slot) {
		const auto pose = MakeShape(slot, 0.7f);
		Expect(std::count_if(pose.begin(), pose.end(), [](float value) { return value > 0.0f; }) == 1,
			"a held emotion uses one clear engine pose instead of stacking different emotions");
	}

	const auto happy = MakeShape(kDialogueHappy, 0.8f);
	Expect(happy[kDialogueHappy] == 0.8f && happy[kDialogueDisgusted] == 0.0f && happy[kDialogueAnger] == 0.0f,
		"happy shapes do not include conflicting sneers or anger");
	const auto sad = MakeShape(kDialogueSad, 0.8f);
	Expect(sad[kDialogueHappy] == 0.0f, "sad shapes do not contain a smile");
	Expect(LineGain(0.0f, true) > LineGain(2.0f, true) && LineGain(2.0f, true) > LineGain(2.0f, false),
		"a reaction settles from onset into speech and then rest");

	Shape face{};
	for (int frame = 0; frame < 12; ++frame) Step(face, happy, 1.0f / 60.0f);
	Expect(face[kDialogueHappy] > 0.6f, "the primary expression becomes readable within 200ms");
	const float previousSmile = face[kDialogueHappy];
	Step(face, sad, 1.0f / 60.0f);
	Expect(face[kDialogueHappy] < previousSmile && face[kDialogueHappy] > 0.0f && face[kDialogueSad] > 0.0f,
		"a new line crossfades rather than snapping or retaining only the old emotion");
	for (int frame = 0; frame < 120; ++frame) Step(face, sad, 1.0f / 60.0f);
	Expect(face[kDialogueSad] > 0.79f && face[kDialogueHappy] < 0.005f, "a new emotion replaces the old one");
	for (int frame = 0; frame < 240; ++frame) Step(face, {}, 1.0f / 60.0f);
	Expect(!Step(face, {}, 1.0f / 60.0f), "release reaches exact zero and yields the channel");
	Expect(std::all_of(face.begin(), face.end(), [](float v) { return v == 0.0f; }), "no expression leaks after release");
	Step(face, happy, std::numeric_limits<float>::quiet_NaN());
	Expect(face[kDialogueHappy] == 0.0f, "invalid frame time cannot corrupt the face");
	Step(face, happy, 10.0f);
	Expect(face[kDialogueHappy] < 0.5f, "a paused or stalled frame cannot slam the face to its target");
	const auto invalid = MakeShape(-1, 1.0f);
	Expect(std::all_of(invalid.begin(), invalid.end(), [](float v) { return v == 0.0f; }), "invalid slots yield no morph");
	std::cout << "Expression policy, dialogue cues, morphs and transitions passed.\n";
}

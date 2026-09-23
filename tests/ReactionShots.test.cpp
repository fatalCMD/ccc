#include "SD/Camera/ReactionShots.h"
#include "SD/Camera/ReplyBoundary.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "ReactionShots: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	using SD::Camera::ReactionShots;

	// One ordinary line: eligible, takeable, automatic framing.
	void Line(ReactionShots& a_shots, const ReactionShots::Settings& a_settings, std::uint32_t a_roll)
	{
		a_shots.OnNpcLine(a_settings, true, true, true, a_roll);
	}
}

int main()
{
	const ReactionShots::Settings always{ true, 3, 100 };
	const ReactionShots::Settings never{ true, 3, 0 };
	const ReactionShots::Settings off{ false, 3, 100 };

	// At 100%, three lines on them and the fourth on you, over and over.
	ReactionShots shots;
	for (int cycle = 0; cycle < 4; ++cycle) {
		Line(shots, always, 0);
		Expect(!shots.Active() && !shots.Owed(), "the first line of a cycle is theirs");
		Line(shots, always, 0);
		Expect(!shots.Active() && !shots.Owed(), "the second line of a cycle is theirs");
		Line(shots, always, 0);
		Expect(!shots.Active() && shots.Owed(), "the third line is theirs and owes the next");
		Line(shots, always, 0);
		Expect(shots.Active() && !shots.Owed(), "the fourth line plays on you");
	}
	Line(shots, always, 0);
	Expect(!shots.Active(), "a reaction lasts one line");

	// The roll decides, and a miss waits a whole window rather than rolling again
	// on the next line.
	ReactionShots rolled;
	const ReactionShots::Settings half{ true, 2, 50 };
	Line(rolled, half, 0);
	Line(rolled, half, 50);
	Expect(!rolled.Owed(), "a roll of 50 misses a 50% chance");
	Line(rolled, half, 0);
	Expect(!rolled.Owed() && !rolled.Active(), "a miss does not roll again on the next line");
	Line(rolled, half, 49);
	Expect(rolled.Owed(), "a roll of 49 hits a 50% chance");
	Line(rolled, half, 0);
	Expect(rolled.Active(), "a hit plays the next line on you");

	ReactionShots zero;
	for (int line = 0; line < 60; ++line) {
		Line(zero, never, 0);
		Expect(!zero.Owed() && !zero.Active(), "0% never reacts");
	}

	ReactionShots disabled;
	for (int line = 0; line < 60; ++line) {
		Line(disabled, off, 0);
		Expect(!disabled.Owed() && !disabled.Active(), "switched off never reacts");
	}

	// Short lines neither count nor carry the reaction.
	ReactionShots shortLines;
	Line(shortLines, always, 0);
	Line(shortLines, always, 0);
	shortLines.OnNpcLine(always, false, true, true, 0);
	Expect(!shortLines.Owed(), "a short line does not count toward the window");
	Line(shortLines, always, 0);
	Expect(shortLines.Owed(), "the third eligible line owes the reaction");
	shortLines.OnNpcLine(always, false, true, true, 0);
	Expect(!shortLines.Active() && shortLines.Owed(), "a short line does not take the reaction");
	Line(shortLines, always, 0);
	Expect(shortLines.Active(), "the next eligible line does");
	shortLines.OnNpcLine(always, false, true, true, 0);
	Expect(!shortLines.Active(), "any new line ends the reaction, short or not");

	// A raised line keeps its close-up; the reaction waits for the next one.
	ReactionShots raised;
	Line(raised, always, 0);
	Line(raised, always, 0);
	Line(raised, always, 0);
	raised.OnNpcLine(always, true, false, true, 0);
	Expect(!raised.Active() && raised.Owed(), "a raised line is not the reaction");
	Line(raised, always, 0);
	Expect(raised.Active(), "the ordinary line after it is");

	// The Director calls Reset at each new reply, so counts and rolls never
	// carry over between replies.
	ReactionShots replies;
	Line(replies, always, 0);
	Line(replies, always, 0);
	Line(replies, always, 0);
	Expect(replies.Owed(), "the third line of a reply owes the fourth");
	replies.Reset();
	Line(replies, always, 0);
	Expect(!replies.Active() && !replies.Owed(), "a roll left over from the last reply is not collected");
	Line(replies, always, 0);
	replies.Reset();
	Line(replies, always, 0);
	Line(replies, always, 0);
	Expect(!replies.Owed(), "lines from the last reply do not count toward this one");
	Line(replies, always, 0);
	Line(replies, always, 0);
	Expect(replies.Active(), "a long enough reply still gets its reaction");
	replies.Reset();
	Expect(!replies.Active(), "a reply ending ends a reaction still playing");

	// Manual framing drops a pending reaction.
	ReactionShots manual;
	Line(manual, always, 0);
	Line(manual, always, 0);
	Line(manual, always, 0);
	manual.OnNpcLine(always, true, true, false, 0);
	Expect(!manual.Owed() && !manual.Active(), "manual framing drops an owed reaction");

	// every = 1 at 100% alternates between them and you.
	ReactionShots alternate;
	const ReactionShots::Settings every1{ true, 1, 100 };
	Line(alternate, every1, 0);
	Expect(alternate.Owed(), "every 1 line owes after one");
	Line(alternate, every1, 0);
	Expect(alternate.Active(), "and takes the next");
	Line(alternate, every1, 0);
	Expect(!alternate.Active() && alternate.Owed(), "then theirs again, owing the next");

	ReactionShots clamped;
	const ReactionShots::Settings silly{ true, -4, 250 };
	Line(clamped, silly, 99);
	Expect(clamped.Owed(), "every below 1 acts as 1, and a chance over 100 always hits");

	shots.Reset();
	Expect(!shots.Active() && !shots.Owed(), "reset clears everything");
	Line(shots, always, 0);
	Line(shots, always, 0);
	Expect(!shots.Owed(), "reset restarts the count");

	// ReplyBoundary
	using SD::Camera::ReplyBoundary;
	ReplyBoundary boundary;
	boundary.Reset();
	Expect(boundary.OnLine(false), "their first line after silence starts a reply");
	Expect(!boundary.OnLine(true), "the next line of the same reply does not");
	boundary.Advance(3.0f);
	boundary.OnPick();
	Expect(boundary.OnLine(true), "a topic picked while they talk starts a reply on their next line");
	Expect(!boundary.OnLine(true), "and only once");

	// A click and a voiced line for the same pick are one pick.
	boundary.Advance(3.0f);
	boundary.OnPick();
	boundary.OnPick();
	Expect(boundary.OnLine(true), "a doubled pick starts one reply");
	Expect(!boundary.OnLine(true), "not two");

	// The line hook can run before the tick sees the click it answers.
	boundary.Advance(3.0f);
	Expect(boundary.OnLine(false), "the answer arrives first");
	boundary.Advance(0.1f);
	boundary.OnPick();
	Expect(!boundary.OnLine(true), "a click seen a frame late does not reset the reply it started");
	boundary.Advance(0.6f);
	boundary.OnPick();
	Expect(boundary.OnLine(true), "a later pick is a real one");

	// Silence and a pick together are still one reply.
	boundary.Advance(3.0f);
	boundary.OnPick();
	Expect(boundary.OnLine(false), "a pick after they finished starts a reply");
	Expect(!boundary.OnLine(true), "and the pick is spent by it");
	boundary.Advance(0.0f);
	boundary.Advance(-4.0f);
	boundary.OnPick();
	Expect(!boundary.OnLine(true), "zero and negative frame time do not age the late-edge window");

	boundary.Reset();
	boundary.OnPick();
	Expect(boundary.OnLine(true), "after a reset a pick counts straight away");

	std::cout << "ReactionShots tests passed\n";
}

#include "SD/Camera/PlayerVoiceHandoff.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "PlayerVoiceHandoff: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	SD::Camera::PlayerVoiceHandoff handoff;
	handoff.Reset(10);
	handoff.Update(10, false, false, true);
	Expect(!handoff.Active(), "unvoiced choice must not anticipate a reply");
	handoff.Update(11, true, false, false);
	Expect(!handoff.Active(), "retain the player while their audio plays");
	handoff.Update(11, false, false, false);
	Expect(handoff.Active(), "audio end must anticipate before the NPC speaks");
	for (int frame = 0; frame < 120; ++frame) {
		handoff.Update(11, false, false, false);
		Expect(handoff.Active(), "post-line delay must not return to player");
	}
	handoff.Update(11, false, true, false);
	Expect(handoff.Active(), "reply arrival must preserve handoff and waive player beat");
	handoff.Update(11, false, false, false);
	Expect(!handoff.Active(), "NPC reply ending returns coverage to the player");
	handoff.Update(11, false, false, false);
	Expect(!handoff.Active(), "completed serial must not retrigger");

	handoff.Update(12, true, false, false);
	handoff.Update(13, true, false, false);
	Expect(!handoff.Active(), "superseding a voiced line must not create a false end");
	handoff.Update(13, false, true, false);
	Expect(handoff.Active(), "skip straight into reply must waive extra player beat");
	handoff.Update(14, true, false, false);
	Expect(!handoff.Active(), "new player line clears prior handoff");
	handoff.Update(14, false, false, true);
	Expect(!handoff.Active(), "cancelled or unanswered topic must not strand coverage");

	handoff.Update(15, true, false, false);
	handoff.Update(15, false, false, false);
	handoff.Reset(15);
	handoff.Update(15, true, false, false);
	handoff.Update(15, false, false, false);
	Expect(!handoff.Active(), "close/reopen must not inherit the old voice tail");
	handoff.Update(16, false, false, false);
	Expect(!handoff.Active(), "missed start must not invent a completed player line");
	handoff.Update(17, true, false, false);
	handoff.Update(17, false, false, false);
	Expect(handoff.Active(), "fresh line after reentry should still hand off");
	handoff.Update(17, false, false, true);
	Expect(!handoff.Active(), "topic list returning without an answer releases handoff");
	std::cout << "PlayerVoiceHandoff tests passed\n";
}

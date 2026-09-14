#include "SD/Scene/DialogueDisplayOverride.h"

#include <cstdlib>
#include <iostream>

namespace
{
	struct Clip
	{
		double alpha{ 100.0 };
		bool visible{ true };
		bool readable{ true };
		bool writable{ true };
		int writes{ 0 };
	};
	struct Node
	{
		Clip* clip{};
		bool operator==(const Node&) const = default;
		bool ReadAlpha(double& a_value) const
		{
			if (!clip || !clip->readable) { return false; }
			a_value = clip->alpha;
			return true;
		}
		bool ReadVisible(bool& a_value) const
		{
			if (!clip || !clip->readable) { return false; }
			a_value = clip->visible;
			return true;
		}
		bool WriteAlpha(double a_value)
		{
			if (!clip || !clip->writable) { return false; }
			clip->alpha = a_value;
			++clip->writes;
			return true;
		}
		bool WriteVisible(bool a_value)
		{
			if (!clip || !clip->writable) { return false; }
			clip->visible = a_value;
			++clip->writes;
			return true;
		}
	};
	void Expect(bool a_ok, const char* a_message)
	{
		if (!a_ok) { std::cerr << a_message << '\n'; std::exit(EXIT_FAILURE); }
	}
}

int main()
{
	SD::Scene::DialogueDisplayOverride<Node> fade;
	Clip oldMenu;
	fade.Bind({ &oldMenu });
	Expect(fade.SetAlpha(50) && fade.SetAlpha(0) && fade.SetHidden(true), "fade old menu");
	oldMenu.readable = false;  // close while a restore cannot reach the old clip
	Expect(!fade.Release(), "failed restoration stays pending for the bound clip");
	Clip newMenu{ 0.0, false };
	fade.Bind({ &newMenu });  // same path, different movie / different display object
	Expect(fade.SetAlpha(100) && fade.SetHidden(false) && fade.Release(), "new menu is untouched");
	Expect(newMenu.alpha == 0 && !newMenu.visible && newMenu.writes == 0,
		"old debt cannot reveal replacement placeholder rows");
	newMenu.alpha = 100;
	newMenu.visible = true;
	Expect(fade.SetAlpha(40) && fade.SetHidden(true), "new menu may fade once populated");
	fade.Bind({ &newMenu });  // unchanged movie across barter suspension
	Expect(fade.Release(), "same-object restoration succeeds");
	Expect(newMenu.alpha == 100 && newMenu.visible, "same movie regains its original values");
	const int restoredWrites = newMenu.writes;
	Expect(fade.Release() && newMenu.writes == restoredWrites, "restoration occurs once");

	Clip hiddenByGame{ 0.0, false };
	fade.Bind({ &hiddenByGame });
	Expect(fade.SetAlpha(0) && fade.SetHidden(true) && fade.Release(), "already hidden clip is untouched");
	Expect(hiddenByGame.writes == 0 && !hiddenByGame.visible, "do not claim another writer's hide");

	Clip altered{ 75.0, true };
	fade.Bind({ &altered });
	Expect(fade.SetAlpha(20), "capture non-default original alpha");
	altered.alpha = 33.0;  // engine or another mod changes it while SD is releasing
	Expect(fade.Release() && altered.alpha == 33.0, "release preserves another writer's new value");
	Expect(fade.SetAlpha(0) && fade.SetAlpha(100) && altered.alpha == 33.0,
		"ending a fade restores the actual original rather than forcing 100");

	Clip retry;
	fade.Bind({ &retry });
	Expect(fade.SetAlpha(0) && fade.SetHidden(true), "prepare failed write recovery");
	retry.writable = false;
	Expect(!fade.Release(), "failed writes retain restoration debt");
	retry.writable = true;
	Expect(fade.Release() && retry.alpha == 100 && retry.visible, "retry restores the same clip");

	Expect(fade.SetAlpha(0), "prepare a menu destroyed without observing an empty frame");
	Clip replacement;
	fade.Bind({ &replacement });
	Expect(retry.alpha == 100 && replacement.writes == 0,
		"direct replacement restores only the old clip");
	fade.Reset();
	Expect(fade.Release() && replacement.writes == 0, "load reset cannot resurrect old debt");
	std::cout << "Dialogue display ownership tests passed\n";
}

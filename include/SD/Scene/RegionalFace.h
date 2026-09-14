#pragma once

namespace SD::Scene
{
	class RegionalFace
	{
	public:
		static void SetEnabled(bool value);
		[[nodiscard]] static bool Enabled();
		static void Prepare(RE::Actor* player);
		static void Before(RE::BSFaceGenNiNode* node);
		static void After(RE::BSFaceGenNiNode* node, bool suppress = false);
		static void Reset();
		static void Report();
	};
}

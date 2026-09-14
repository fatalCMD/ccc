#pragma once

#include "SD/Scene/UpperFace.h"

namespace SD::Scene
{
	// Opt-in, one-shot diagnostic. Not an expression setting or camera mode.
	class FaceTest
	{
	public:
		static void Tick(float delta);
		static bool Sample(UpperFace::Shape& values);
		static void Record(RE::BSFaceGenNiNode* node);
		static void Cancel();
	};
}

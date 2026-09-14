#include "SD/Scene/Focus.h"

#include "SD/Core/Logging.h"

namespace SD::Scene
{
	namespace
	{
		// Vanilla, in Skyrim.esm, present in every install. Found by scanning the
		// master for IMAD records rather than guessed: 170 carry an editor ID and
		// exactly one is a depth-of-field modifier.
		constexpr RE::FormID kVatsDof = 0x00035301;

		bool          engaged{ false };
		bool          enabled{ true };
		float         strength{ 0.55f };
		Log::OnceFlag availabilityReported;

		[[nodiscard]] RE::TESImageSpaceModifier* Modifier()
		{
			return RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(kVatsDof);
		}
	}

	bool Focus::Available()
	{
		return Modifier() != nullptr;
	}

	void Focus::Configure(bool a_enabled, float a_strength)
	{
		enabled = a_enabled;
		strength = std::clamp(a_strength, 0.0f, 1.0f);
	}

	void Focus::Engage()
	{
		if (engaged || !enabled) {
			return;
		}

		auto* imod = Modifier();
		if (!imod) {
			if (availabilityReported.Take()) {
				Log::Error(Log::Category::kStaging,
					"VATSImodDOF ({:08X}) not found; depth of field unavailable."sv, kVatsDof);
			}
			return;
		}

		RE::ImageSpaceModifierInstanceForm::Trigger(imod, strength, nullptr);
		engaged = true;

		if (availabilityReported.Take()) {
			Log::Info(Log::Category::kStaging,
				"Depth of field engaged via VATSImodDOF at strength {:.2f}."sv, strength);
		}
	}

	void Focus::Release()
	{
		if (!engaged) {
			return;
		}
		engaged = false;

		if (auto* imod = Modifier()) {
			RE::ImageSpaceModifierInstanceForm::Stop(imod);
		}
	}
}

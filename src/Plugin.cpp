#include "Plugin.h"

#include "SD/Compat/DBReV.h"
#include "SD/Compat/SmoothCam.h"
#include "SD/Core/Logging.h"
#include "SD/Runtime.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Not at plugin load. SKSE loads plugins alphabetically, so
			// SceneDirector.dll is registered before SmoothCam.dll and asking for
			// SmoothCam's listener that early fails outright. kPostLoad is the
			// first point at which every plugin has registered.
			SD::Compat::SmoothCam::Register();

			// Same constraint, same reason, and DBReV's author names it as the
			// first thing that bites integrators: registering before DBReV.dll has
			// loaded returns false and leaves a listener that never fires.
			SD::Compat::DBReV::Register();
			break;

		case SKSE::MessagingInterface::kDataLoaded:
			SD::Runtime::Initialize();
			break;

		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			SD::Runtime::AbandonForLoad();
			break;

		case SKSE::MessagingInterface::kPostLoadGame:
			SD::Runtime::OnGameLoaded();
			break;

		default:
			break;
		}
	}
}

EXTERN_C [[maybe_unused]] __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SD::Log::Setup(Plugin::NAME);
	SD::Log::Info(SD::Log::Category::kCore, "{} {} loading."sv, Plugin::DISPLAY_NAME, Plugin::VERSION.string("."sv));

	// Pass false: CommonLibSSE-NG 7 added an a_log parameter that defaults to TRUE,
	// and it does more than add a banner. log::init() reopens this same file with
	// truncate, wiping the line logged just above, then replaces the default logger
	// and the pattern. Log::Setup above already rotated the previous log and set the
	// format this project reads. Let it own the log.
	SKSE::Init(a_skse, false);

	// SE and AE only. Every vtable index this mod writes — Actor::Update (0xAD),
	// UpdateInDialogue (0x4C), TESCamera::Update (0x02), camera state Update (0x03)
	// — is a flat-Skyrim index. CommonLibSSE-NG says so itself: it declares
	// `Actor::Update` with SKYRIM_REL_VR_VIRTUAL, which expands to nothing in a
	// multi-target build precisely because the VR slot is elsewhere. The VTABLE
	// addresses still resolve under VR, so without this check write_vfunc would
	// happily overwrite the wrong slots and corrupt every actor in the game.
	if (REL::Module::IsVR()) {
		SD::Log::Error(SD::Log::Category::kCore,
			"Skyrim VR detected. Scene Director hooks flat-Skyrim vtable slots and will not install."sv);
		return false;
	}

	if (const auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	} else {
		SD::Log::Error(SD::Log::Category::kCore, "Messaging interface unavailable."sv);
		return false;
	}

	return true;
}

EXTERN_C [[maybe_unused]] __declspec(dllexport) constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData data;
	data.PluginName(Plugin::NAME);
	data.PluginVersion(Plugin::VERSION);
	data.AuthorName("Scene Director");
	data.UsesAddressLibrary();

	// Read SKSE's own words for this flag before changing it — the CommonLib name
	// is misleading. `kVersionIndependentEx_NoStructUse` is documented as "set this
	// if your plugin either doesn't use any game structures **or has put in
	// extraordinary effort to work with pre and post 1.6.629 structure layout**".
	// The second clause is this build: every version-dependent member is reached
	// through CommonLibSSE-NG's `RelocateMemberIfNewer(RUNTIME_SSE_1_6_629, ...)`
	// accessors (`GetActorRuntimeData()` and friends), which pick the offset from
	// the running exe, and the target is compiled with ENABLE_SKYRIM_SE and
	// ENABLE_SKYRIM_AE so both layouts are present in the binary.
	//
	// Leaving this call out is what made SKSE refuse the plugin on 1.6.1170 with
	// "disabled, only compatible with versions earlier than 1.6.629" — the gate in
	// PluginManager.cpp fires on any address-library plugin that claims neither
	// this flag nor StructsPost629. StructsPost629 is the wrong alternative: it
	// asserts the binary is post-629 *only*, which would give up 1.6.317–1.6.353.
	data.UsesNoStructs();
	return data;
}();

EXTERN_C [[maybe_unused]] __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
	a_info->name = SKSEPlugin_Version.pluginName;
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

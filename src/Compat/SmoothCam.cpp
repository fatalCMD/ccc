#include "SD/Compat/SmoothCam.h"

#include "SD/Core/Logging.h"

#define SMOOTHCAM_API_COMMONLIB
#include "SD/Compat/SmoothCamAPI.h"

namespace SD::Compat
{
	namespace
	{
		SmoothCamAPI::IVSmoothCam2* api{ nullptr };
		bool                        callbackRegistered{ false };
		bool                        holding{ false };
		Log::OnceFlag               absenceReported;

		[[nodiscard]] SKSE::PluginHandle Handle()
		{
			return SKSE::GetPluginHandle();
		}
	}

	void SmoothCam::Register()
	{
		if (callbackRegistered) {
			return;
		}

		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			Log::Warn(Log::Category::kCompat, "SKSE messaging unavailable; SmoothCam handoff disabled."sv);
			return;
		}

		callbackRegistered = SmoothCamAPI::RegisterInterfaceLoaderCallback(
			messaging,
			[](void* a_instance, SmoothCamAPI::InterfaceVersion a_version) {
				if (a_version == SmoothCamAPI::InterfaceVersion::V2 ||
					a_version == SmoothCamAPI::InterfaceVersion::V3) {
					api = reinterpret_cast<SmoothCamAPI::IVSmoothCam2*>(a_instance);
					Log::Info(Log::Category::kCompat, "SmoothCam interface obtained (V{})."sv,
						a_version == SmoothCamAPI::InterfaceVersion::V3 ? 3 : 2);
				} else {
					Log::Warn(Log::Category::kCompat, "Unsupported SmoothCam interface version."sv);
				}
			});

		if (!callbackRegistered) {
			Log::Warn(Log::Category::kCompat, "SmoothCam callback registration failed."sv);
		}
	}

	void SmoothCam::Request()
	{
		Register();
		if (api) {
			return;
		}

		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging || !callbackRegistered) {
			return;
		}

		if (!SmoothCamAPI::RequestInterface(messaging, SmoothCamAPI::InterfaceVersion::V2)) {
			Log::Info(Log::Category::kCompat, "SmoothCam interface request not dispatched; assuming absent."sv);
		}
	}

	bool SmoothCam::Present() noexcept
	{
		return api != nullptr;
	}

	bool SmoothCam::Acquire()
	{
		if (holding) {
			return true;
		}

		if (!api) {
			// Not installed. Nothing owns the camera, so nothing has to be asked.
			if (absenceReported.Take()) {
				Log::Info(Log::Category::kCompat, "SmoothCam not present; taking the camera directly."sv);
			}
			holding = true;
			return true;
		}

		const auto result = api->RequestCameraControl(Handle());
		switch (result) {
		case SmoothCamAPI::APIResult::OK:
			holding = true;
			Log::Info(Log::Category::kCompat, "SmoothCam camera control acquired."sv);
			return true;

		case SmoothCamAPI::APIResult::AlreadyGiven:
			holding = true;
			return true;

		case SmoothCamAPI::APIResult::AlreadyTaken:
			// Someone else is directing. Declining is correct: two mods writing one
			// transform produces a camera that visibly fights itself, and the other
			// consumer asked first.
			Log::Warn(Log::Category::kCompat,
				"SmoothCam camera already held by another plugin (owner {}); not staging this conversation."sv,
				static_cast<std::uint32_t>(api->GetCameraOwner()));
			return false;

		default:
			Log::Warn(Log::Category::kCompat, "SmoothCam refused camera control (result {})."sv,
				static_cast<std::uint32_t>(result));
			return false;
		}
	}

	void SmoothCam::Release()
	{
		if (!holding) {
			return;
		}
		holding = false;

		if (!api) {
			return;
		}

		const auto result = api->ReleaseCameraControl(Handle());
		if (result == SmoothCamAPI::APIResult::OK) {
			Log::Info(Log::Category::kCompat, "SmoothCam camera control released."sv);
		} else {
			Log::Warn(Log::Category::kCompat, "SmoothCam release returned {}."sv,
				static_cast<std::uint32_t>(result));
		}
	}

	bool SmoothCam::Holding() noexcept
	{
		return holding;
	}
}

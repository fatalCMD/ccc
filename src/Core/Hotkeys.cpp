#include "SD/Core/Hotkeys.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"

namespace SD::Core
{
	namespace
	{
		using Action = Hotkeys::Action;

		constexpr std::size_t kActions = static_cast<std::size_t>(Action::kCount);

		// DirectX scan codes, which is what the game reports and what every other
		// SKSE plugin's ini stores. Zero is not a key on any keyboard, so it is the
		// unassigned sentinel with nothing to disambiguate.
		std::array<std::atomic<std::uint32_t>, kActions> bindings{};

		// -1 when nothing is waiting for a key.
		std::atomic<int>           armed{ -1 };
		std::atomic<std::uint32_t> caught{ 0 };

		// Which row asked, remembered separately because the sink clears `armed`
		// at the moment it catches the key and the panel reads the result a frame
		// later. Only ever one capture in flight — two rows cannot arm at once.
		std::atomic<int> lastArmed{ -1 };


		[[nodiscard]] const char* IniKey(Action a_action)
		{
			switch (a_action) {
			case Action::kFraming: return "iKeyFraming";
			default:               return "iKeyNextAngle";
			}
		}

		// Escape cancels rather than binding.
		//
		// It is the one key every capture widget in every game reserves, and a
		// player who opens the binding by accident has to be able to get out of it
		// without either committing something or being stuck. Binding Escape is a
		// legitimate thing to want and it is still reachable — clear the binding
		// and it is unassigned, which is what wanting Escape almost always means.
		constexpr std::uint32_t kEscape = 0x01;

		class Sink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static Sink* GetSingleton()
			{
				static Sink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
				RE::BSTEventSource<RE::InputEvent*>*) override
			{
				// Returns kContinue on every path in this function, including the
				// early ones. See the note in Hotkeys.h: this sink observes and
				// never swallows, and that is what keeps it clear of the removed
				// dialogue input handler.
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}

				const bool capturing = armed.load(std::memory_order_relaxed) >= 0;

				// Ordinary presses only matter while a conversation is being
				// directed. Capture has to work anywhere, because the settings
				// panel is most often opened standing in a field.
				if (!capturing && !Camera::Director::Staging()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				for (auto* event = *a_event; event; event = event->next) {
					auto* button = event->AsButtonEvent();
					if (!button || !button->IsDown()) {
						continue;  // IsDown is the press EDGE, not the held state
					}
					if (event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
						continue;
					}

					const auto code = button->GetIDCode();
					if (code == 0) {
						continue;
					}

					if (capturing) {
						// Recorded, not applied. SetBinding writes the ini and the
						// input thread is not where a file write belongs, so the
						// panel picks this up on its next frame.
						static_cast<void>(Hotkeys::OfferKey(code));
						return RE::BSEventNotifyControl::kContinue;
					}

					// Compared against the live atomics rather than a cached copy,
					// so a rebind takes effect on the next press.
					for (std::size_t i = 0; i < kActions; ++i) {
						if (code != bindings[i].load(std::memory_order_relaxed)) {
							continue;
						}
						switch (static_cast<Action>(i)) {
						case Action::kFraming:
							Camera::Director::RequestFraming();
							break;
						default:
							Camera::Director::RequestCut();
							break;
						}
					}
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void ReadBindings()
		{
			for (std::size_t i = 0; i < kActions; ++i) {
				// Clamped to the byte range scan codes occupy, so a garbage value
				// in a hand-edited ini reads as unassigned rather than as a code
				// that can never be pressed and never be diagnosed.
				const int stored = Config::Int("Direction", IniKey(static_cast<Action>(i)), 0);
				bindings[i].store(stored > 0 && stored <= 0xFF ?
						static_cast<std::uint32_t>(stored) :
						0u,
					std::memory_order_relaxed);
			}
		}
	}

	void Hotkeys::Install()
	{
		ReadBindings();

		// REGISTERED WHETHER OR NOT ANYTHING IS BOUND, which the previous version
		// was not. Capture reads the same event stream, and capture has to work
		// when nothing is bound yet — which is always the first time somebody opens
		// the page. The cost when idle is one relaxed load and a staging check.
		auto* manager = RE::BSInputDeviceManager::GetSingleton();
		if (!manager) {
			Log::Warn(Log::Category::kCore,
				"Input manager unavailable; camera hotkeys will not respond."sv);
			return;
		}

		manager->AddEventSink(Sink::GetSingleton());

		if (!AnyBound()) {
			// Said once, plainly, and this is not a formality.
			//
			// The failure this project has hit more often than any other is a
			// feature that is present, unconfigured, and silent about it — the log
			// reads as though everything works and the control appears to do
			// nothing. Both keys ship unassigned because guessing a binding inside
			// somebody else's load order is worse than asking for one, so the
			// absence has to announce itself.
			Log::Info(Log::Category::kCore,
				"No camera hotkeys assigned. Settings -> Controls, or [Direction] iKeyNextAngle / iKeyFraming."sv);
			return;
		}

		Log::Info(Log::Category::kCore, "Camera hotkeys listening: next angle {}, framing {}."sv,
			KeyName(Binding(Action::kNextAngle)), KeyName(Binding(Action::kFraming)));
	}

	void Hotkeys::Refresh()
	{
		ReadBindings();
	}

	std::uint32_t Hotkeys::Binding(Action a_action) noexcept
	{
		const auto index = static_cast<std::size_t>(a_action);
		return index < kActions ? bindings[index].load(std::memory_order_relaxed) : 0u;
	}

	void Hotkeys::SetBinding(Action a_action, std::uint32_t a_code)
	{
		const auto index = static_cast<std::size_t>(a_action);
		if (index >= kActions) {
			return;
		}

		const auto code = a_code <= 0xFF ? a_code : 0u;

		// THE SAME KEY CANNOT DO TWO THINGS. Taking it from the other action rather
		// than refusing the bind: the player has just pressed a key while looking
		// at THIS row, so this row is what they meant, and a refusal reads as the
		// capture having failed.
		if (code != 0) {
			for (std::size_t i = 0; i < kActions; ++i) {
				if (i != index && bindings[i].load(std::memory_order_relaxed) == code) {
					bindings[i].store(0u, std::memory_order_relaxed);
					Config::SetInt("Direction", IniKey(static_cast<Action>(i)), 0);
					Log::Info(Log::Category::kCore, "{} was already on {}; cleared it."sv,
						IniKey(static_cast<Action>(i)), KeyName(code));
				}
			}
		}

		bindings[index].store(code, std::memory_order_relaxed);
		Config::SetInt("Direction", IniKey(a_action), static_cast<int>(code));

		Log::Info(Log::Category::kCore, "{} bound to {}."sv, IniKey(a_action), KeyName(code));
	}

	void Hotkeys::Arm(Action a_action)
	{
		caught.store(0, std::memory_order_relaxed);
		lastArmed.store(static_cast<int>(a_action), std::memory_order_relaxed);
		armed.store(static_cast<int>(a_action), std::memory_order_relaxed);
	}

	bool Hotkeys::OfferKey(std::uint32_t a_code)
	{
		if (a_code == 0 || armed.load(std::memory_order_relaxed) < 0) {
			return false;
		}

		caught.store(a_code, std::memory_order_relaxed);
		armed.store(-1, std::memory_order_relaxed);
		return true;
	}

	void Hotkeys::Cancel()
	{
		armed.store(-1, std::memory_order_relaxed);
		caught.store(0, std::memory_order_relaxed);
	}

	bool Hotkeys::Capturing() noexcept
	{
		return armed.load(std::memory_order_relaxed) >= 0;
	}

	bool Hotkeys::CapturingFor(Action a_action) noexcept
	{
		return armed.load(std::memory_order_relaxed) == static_cast<int>(a_action);
	}

	bool Hotkeys::PollCapture()
	{
		const auto code = caught.exchange(0, std::memory_order_relaxed);
		if (code == 0) {
			return false;
		}

		// The sink cleared `armed` when it caught the key, so the action has to be
		// recovered from what the panel last asked for. Kept here rather than
		// stored alongside the code because there is exactly one capture in flight
		// at a time — two rows cannot be armed at once.
		const int action = lastArmed.exchange(-1, std::memory_order_relaxed);
		if (action < 0 || static_cast<std::size_t>(action) >= kActions) {
			return false;
		}

		if (code == kEscape) {
			// Cancelled, not bound. Leaves whatever was already there alone.
			return true;
		}

		SetBinding(static_cast<Action>(action), code);
		return true;
	}

	bool Hotkeys::AnyBound() noexcept
	{
		for (std::size_t i = 0; i < kActions; ++i) {
			if (bindings[i].load(std::memory_order_relaxed) != 0) {
				return true;
			}
		}
		return false;
	}

	std::string_view Hotkeys::KeyName(std::uint32_t a_code) noexcept
	{
		switch (a_code) {
		case 0x00: return "Not assigned"sv;
		case 0x01: return "Escape"sv;
		case 0x02: return "1"sv;
		case 0x03: return "2"sv;
		case 0x04: return "3"sv;
		case 0x05: return "4"sv;
		case 0x06: return "5"sv;
		case 0x07: return "6"sv;
		case 0x08: return "7"sv;
		case 0x09: return "8"sv;
		case 0x0A: return "9"sv;
		case 0x0B: return "0"sv;
		case 0x0C: return "Minus"sv;
		case 0x0D: return "Equals"sv;
		case 0x0E: return "Backspace"sv;
		case 0x0F: return "Tab"sv;
		case 0x10: return "Q"sv;
		case 0x11: return "W"sv;
		case 0x12: return "E"sv;
		case 0x13: return "R"sv;
		case 0x14: return "T"sv;
		case 0x15: return "Y"sv;
		case 0x16: return "U"sv;
		case 0x17: return "I"sv;
		case 0x18: return "O"sv;
		case 0x19: return "P"sv;
		case 0x1A: return "Left Bracket"sv;
		case 0x1B: return "Right Bracket"sv;
		case 0x1C: return "Enter"sv;
		case 0x1D: return "Left Ctrl"sv;
		case 0x1E: return "A"sv;
		case 0x1F: return "S"sv;
		case 0x20: return "D"sv;
		case 0x21: return "F"sv;
		case 0x22: return "G"sv;
		case 0x23: return "H"sv;
		case 0x24: return "J"sv;
		case 0x25: return "K"sv;
		case 0x26: return "L"sv;
		case 0x27: return "Semicolon"sv;
		case 0x28: return "Apostrophe"sv;
		case 0x29: return "Grave"sv;
		case 0x2A: return "Left Shift"sv;
		case 0x2B: return "Backslash"sv;
		case 0x2C: return "Z"sv;
		case 0x2D: return "X"sv;
		case 0x2E: return "C"sv;
		case 0x2F: return "V"sv;
		case 0x30: return "B"sv;
		case 0x31: return "N"sv;
		case 0x32: return "M"sv;
		case 0x33: return "Comma"sv;
		case 0x34: return "Period"sv;
		case 0x35: return "Slash"sv;
		case 0x36: return "Right Shift"sv;
		case 0x37: return "Numpad *"sv;
		case 0x38: return "Left Alt"sv;
		case 0x39: return "Space"sv;
		case 0x3A: return "Caps Lock"sv;
		case 0x3B: return "F1"sv;
		case 0x3C: return "F2"sv;
		case 0x3D: return "F3"sv;
		case 0x3E: return "F4"sv;
		case 0x3F: return "F5"sv;
		case 0x40: return "F6"sv;
		case 0x41: return "F7"sv;
		case 0x42: return "F8"sv;
		case 0x43: return "F9"sv;
		case 0x44: return "F10"sv;
		case 0x45: return "Num Lock"sv;
		case 0x46: return "Scroll Lock"sv;
		case 0x47: return "Numpad 7"sv;
		case 0x48: return "Numpad 8"sv;
		case 0x49: return "Numpad 9"sv;
		case 0x4A: return "Numpad -"sv;
		case 0x4B: return "Numpad 4"sv;
		case 0x4C: return "Numpad 5"sv;
		case 0x4D: return "Numpad 6"sv;
		case 0x4E: return "Numpad +"sv;
		case 0x4F: return "Numpad 1"sv;
		case 0x50: return "Numpad 2"sv;
		case 0x51: return "Numpad 3"sv;
		case 0x52: return "Numpad 0"sv;
		case 0x53: return "Numpad ."sv;
		case 0x57: return "F11"sv;
		case 0x58: return "F12"sv;
		case 0x9C: return "Numpad Enter"sv;
		case 0x9D: return "Right Ctrl"sv;
		case 0xB5: return "Numpad /"sv;
		case 0xB8: return "Right Alt"sv;
		case 0xC5: return "Pause"sv;
		case 0xC7: return "Home"sv;
		case 0xC8: return "Up Arrow"sv;
		case 0xC9: return "Page Up"sv;
		case 0xCB: return "Left Arrow"sv;
		case 0xCD: return "Right Arrow"sv;
		case 0xCF: return "End"sv;
		case 0xD0: return "Down Arrow"sv;
		case 0xD1: return "Page Down"sv;
		case 0xD2: return "Insert"sv;
		case 0xD3: return "Delete"sv;
		default:   return "Unrecognised key"sv;
		}
	}
}

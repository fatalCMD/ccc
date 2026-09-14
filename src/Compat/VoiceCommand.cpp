#include "SD/Compat/VoiceCommand.h"

#include "SD/Core/Logging.h"

namespace SD::Compat
{
	namespace
	{
		RE::SCRIPT_FUNCTION::Execute_t* original{ nullptr };
		bool                            installed{ false };

		// Console commands do not run on a thread this owns, and the reader is
		// LipSync on the main thread. One short string, one write per line.
		std::mutex  mutex;
		std::string pack;

		// The line SpeakSound was last asked to play on the player, waiting to be
		// drained. Same mutex: it is written from the same call, in the same
		// breath, and read by the same frame tick.
		//
		// No SKSE task marshalling, for the reason DBReV's callback gives — the
		// consumer polls every frame anyway, and AddTask would only add a frame of
		// latency to a signal whose entire value is arriving on time.
		bool               startPending{ false };
		VoiceCommand::Line pendingLine;

		// Latched on the first player line and never cleared. See EverHeard.
		std::atomic_bool everHeard{ false };

		Log::OnceFlag learnedReported;
		Log::OnceFlag firstLineReported;
		Log::OnceFlag otherRefReported;

		// "DBVO/voicebella/Some_line.fuz" -> "voicebella"
		//
		// Taken as "the segment between the first and second separator" rather than
		// by matching a known root, so a DBReV-native path (DBReV/<pack>/<plugin>/...)
		// yields its pack too. Both separators are accepted: the string is built by
		// another mod and ends up in a Windows path, and neither is guaranteed.
		[[nodiscard]] std::string PackFromPath(std::string_view a_path)
		{
			const auto first = a_path.find_first_of("/\\");
			if (first == std::string_view::npos) {
				return {};
			}

			const auto second = a_path.find_first_of("/\\", first + 1);
			if (second == std::string_view::npos || second <= first + 1) {
				return {};
			}

			return std::string{ a_path.substr(first + 1, second - first - 1) };
		}

		// SpeakSound plays any sound at all, and mods use it for things that are
		// not speech. A voice line is a .fuz, so anything else is somebody else's
		// business and must not open a line.
		[[nodiscard]] bool IsVoiceFile(std::string_view a_path)
		{
			constexpr std::string_view kExtension = ".fuz"sv;
			if (a_path.size() <= kExtension.size()) {
				return false;
			}

			const auto tail = a_path.substr(a_path.size() - kExtension.size());
			for (std::size_t i = 0; i < kExtension.size(); ++i) {
				const char raw = tail[i];
				const char lower = static_cast<char>(raw >= 'A' && raw <= 'Z' ? raw + 32 : raw);
				if (lower != kExtension[i]) {
					return false;
				}
			}

			return true;
		}

		bool Execute(const RE::SCRIPT_PARAMETER* a_paramInfo,
			RE::SCRIPT_FUNCTION::ScriptData*    a_scriptData,
			RE::TESObjectREFR*                  a_thisObj,
			RE::TESObjectREFR*                  a_containingObj,
			RE::Script*                         a_scriptObj,
			RE::ScriptLocals*                   a_locals,
			double&                             a_result,
			std::uint32_t&                      a_opcodeOffsetPtr)
		{
			// A COPY of the offset, and the original still gets the untouched one.
			//
			// ParseParameters advances the cursor it is given. Handing it the real
			// one would leave the engine's own parse starting past its arguments,
			// which is a corrupted console command rather than a missing feature.
			std::uint32_t offset = a_opcodeOffsetPtr;
			char          buffer[512]{};

			if (RE::Script::ParseParameters(a_paramInfo, a_scriptData, offset, a_thisObj,
					a_containingObj, a_scriptObj, a_locals, buffer)) {
				buffer[sizeof(buffer) - 1] = '\0';

				if (auto found = PackFromPath(buffer); !found.empty()) {
					bool changed = false;
					{
						const std::scoped_lock lock{ mutex };
						changed = pack != found;
						if (changed) {
							pack = found;
						}
					}

					// Reported on the first line and again if it ever changes, which
					// is a real event: the pack is per-character in both DBVO 2 and
					// DBReV, so switching character switches this mid-session.
					if (changed) {
						if (learnedReported.Take()) {
							Log::Info(Log::Category::kCompat,
								"Voice pack in use: \"{}\", read from the SpeakSound call. Line lengths will be measured from this pack."sv,
								found);
						} else {
							Log::Info(Log::Category::kCompat,
								"Voice pack changed to \"{}\"."sv, found);
						}
					}

					// --- The line itself ---------------------------------------
					//
					// ONLY WHEN IT IS THE PLAYER SPEAKING, and that is the whole
					// difference between this and handle polling. Player.SpeakSound
					// is what both DBVO generations issue; a follower framework
					// voicing an NPC through the same command must not drive the
					// player's mouth, and handle polling had no way to tell.
					//
					// Note the asymmetry with the pack above, which stays ungated on
					// purpose. Pack learning works today and is not worth risking on
					// an assumption about which ref the command arrives on. If that
					// assumption is wrong the worst this new path can do is never
					// fire, and EverHeard then leaves every consumer on its old code.
					const bool onPlayer = a_thisObj && a_thisObj->IsPlayerRef();

					if (onPlayer && IsVoiceFile(buffer)) {
						{
							const std::scoped_lock lock{ mutex };

							// A start that overwrites an unconsumed start is a topic
							// clicked before the previous line finished. The newest
							// line is the one to animate.
							pendingLine.path.assign(buffer);
							pendingLine.pack = found;
							startPending = true;
						}

						everHeard.store(true, std::memory_order_relaxed);

						if (firstLineReported.Take()) {
							Log::Info(Log::Category::kCompat,
								"SpeakSound named the player's line: \"{}\". Line starts and lengths now come from the command rather than from sound handles."sv,
								buffer);
						}
					} else if (!onPlayer && otherRefReported.Take()) {
						// One line, once, and it is a diagnosis rather than a fault.
						// A log that learned the pack but never opened a line is
						// otherwise indistinguishable from one where the wrapper
						// never ran at all.
						Log::Info(Log::Category::kCompat,
							"SpeakSound seen on something other than the player (\"{}\"); noted for the pack only."sv,
							buffer);
					}
				}
			}

			// ALWAYS, whatever happened above. This wrapper exists to observe.
			return original ? original(a_paramInfo, a_scriptData, a_thisObj, a_containingObj,
									a_scriptObj, a_locals, a_result, a_opcodeOffsetPtr) :
							  true;
		}
	}

	void VoiceCommand::Install()
	{
		if (installed) {
			return;
		}
		installed = true;

		auto* command = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SpeakSound"sv);
		if (!command || !command->executeFunction) {
			Log::Warn(Log::Category::kCompat,
				"SpeakSound console command not found; the voice pack cannot be identified and line lengths will fall back to searching every pack."sv);
			return;
		}

		original = command->executeFunction;

		RE::SCRIPT_FUNCTION::Execute_t* replacement = &Execute;
		REL::safe_write(reinterpret_cast<std::uintptr_t>(std::addressof(command->executeFunction)),
			std::addressof(replacement), sizeof(replacement));

		Log::Info(Log::Category::kCompat,
			"Watching SpeakSound to identify the active voice pack and the player's lines."sv);
	}

	std::string VoiceCommand::Pack()
	{
		const std::scoped_lock lock{ mutex };
		return pack;
	}

	bool VoiceCommand::TakeLineStart(Line& a_out)
	{
		const std::scoped_lock lock{ mutex };
		if (!startPending) {
			return false;
		}

		startPending = false;
		a_out = pendingLine;
		return true;
	}

	bool VoiceCommand::EverHeard() noexcept
	{
		return everHeard.load(std::memory_order_relaxed);
	}

	void VoiceCommand::Reset()
	{
		const std::scoped_lock lock{ mutex };
		startPending = false;
		pendingLine = {};

		// `pack` survives deliberately. It is a property of the character being
		// played rather than of the line, and the save being loaded is
		// overwhelmingly the same character — so keeping it means the first line
		// after a load is measured from the right pack instead of searching all
		// fifteen.
	}
}

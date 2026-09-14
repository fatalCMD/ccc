// DBReV_API.h — public SKSE messaging contract for Dragonborn ReVoiced.
//
// This header may be freely used, modified and redistributed, including in
// closed-source projects. It is a declaration only: it contains no DBReV code,
// requires no linking against DBReV.dll, and has no dependency beyond <cstdint>.
//
// DBReV broadcasts these messages whenever the player speaks a voiced dialogue
// line. Communication is strictly one-way: DBReV pushes, you receive.
//
//   SKSE::GetMessagingInterface()->RegisterListener(DBReV::kSender, Callback);
//
// See INTEGRATION.md. The three things that will bite you, in order:
//   1. Register from kPostLoad, not from plugin load (SKSE load order).
//   2. The callback runs on the PAPYRUS thread, not the main thread.
//   3. Every pointer below dies when the callback returns.
//
// ABI: append-only. Fields are never reordered, resized or removed; new fields
// are added at the end and `version` is incremented. Check `version` (and, if
// you like, `structSize`) before reading anything.

#pragma once

#include <cstdint>

namespace DBReV
{
    // The SKSE plugin name to register against. A true return from
    // RegisterListener(kSender, ...) means DBReV is present in this load order;
    // that is the supported way to detect it. Do not test for DBReV.esp.
    inline constexpr const char* kSender = "DBReV";

    enum : std::uint32_t
    {
        // data -> const PlayerLineStart*
        // Sent immediately after the engine has been asked to speak the player's
        // line, i.e. the audio is starting now.
        kPlayerLineStart = 1,

        // data -> const PlayerLineEnd*
        // Sent once per kPlayerLineStart, when the dialogue actually advances.
        kPlayerLineEnd = 2,
    };

    enum : std::uint32_t
    {
        kEndReason_Completed  = 0,  // the line played to its measured end
        kEndReason_Skipped    = 1,  // the player skipped it (Smart Talk, Enter-to-advance)
        kEndReason_Superseded = 2,  // a new topic was clicked before this one finished
    };

    struct PlayerLineStart
    {
        std::uint32_t version;       // currently 1
        std::uint32_t structSize;    // sizeof(PlayerLineStart) as DBReV built it

        // Measured length of the audio, in seconds, parsed from the FUZ/xWMA
        // header before playback. 0.0 means DBReV could not measure this file
        // and fell back to an estimate — in that case only totalSeconds is
        // meaningful.
        float audioSeconds;

        // audioSeconds plus the user's configured post-line delay. This is how
        // long DBReV will hold the dialogue open before advancing, so it is the
        // number to hold a shot for.
        float totalSeconds;

        // Index of the clicked topic, as the engine's TopicClicked delegate
        // reported it. Matches position in MenuTopicManager::dialogueList.
        std::uint32_t topicIndex;

        // Size of lipData in bytes; 0 when the FUZ carried no LIP chunk.
        std::uint32_t lipSize;

        // Path handed to Player.SpeakSound, relative to Data/Sound/. UTF-8.
        const char* voicePath;

        // Active voice pack, e.g. "NG:my_pack" or "DBVO:my_pack". May be empty.
        const char* voicePackId;

        // Sanitized topic text, as used to build the filename. May be empty.
        const char* topicKey;

        // The LIP chunk lifted verbatim out of the FUZ container: the authored
        // phoneme track the engine itself would use for lip sync. nullptr when
        // absent. DBReV does not parse or validate this — it is the raw bytes.
        const void* lipData;
    };
    static_assert(sizeof(PlayerLineStart) == 56, "PlayerLineStart ABI changed");

    struct PlayerLineEnd
    {
        std::uint32_t version;     // currently 1
        std::uint32_t structSize;  // sizeof(PlayerLineEnd) as DBReV built it
        std::uint32_t topicIndex;  // matches the PlayerLineStart it closes
        std::uint32_t reason;      // one of kEndReason_*
    };
    static_assert(sizeof(PlayerLineEnd) == 16, "PlayerLineEnd ABI changed");
}

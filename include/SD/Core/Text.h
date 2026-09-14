#pragma once

// Reading Skyrim's own words, one character at a time.
//
// Everything textual that reaches this mod comes from the GAME — topic rows off
// Scaleform, subtitles off the response record — and all of it is UTF-8. Until
// now every consumer walked it as bytes, which is correct for exactly one
// question ("are these bytes the ASCII substring I am looking for") and wrong
// for every other one.
//
// The two that were wrong, both found on 2026-08-28:
//
//   - Director::WordCount counted runs between spaces. Japanese and Chinese do
//     not put spaces between words, so an entire sentence counted as one, and
//     bHoldOnShortLines then classified every line in the game as too short to
//     cut on.
//   - Performance::EmotionFromText looked for '?' and '!'. A Japanese or
//     Chinese localisation writes those as ？ and ！ — three bytes each, neither
//     containing an ASCII '?' — so the question rule, which that function's own
//     comment calls the one that earns most of its keep, never fired once.
//
// Neither is fixable by looking harder at bytes. They need characters, so this
// is the one place that turns the former into the latter.

namespace SD::Text
{
	// U+FFFD, returned for a byte sequence that is not valid UTF-8.
	//
	// A replacement character rather than a failure, because every caller here is
	// classifying rather than parsing: a corrupt byte is a character that is not a
	// question mark and not a kanji, which is all any of them need to know. It
	// also cannot match any of the ranges they test, so a malformed line degrades
	// to "no punctuation, no words" rather than to a hang or a wrong answer.
	inline constexpr char32_t kReplacement = 0xFFFD;

	// Decode one character, advancing a_pos past it.
	//
	// ALWAYS ADVANCES, and that is load-bearing rather than tidy: every caller is
	// a loop over the whole string, and a decoder that can return without moving
	// turns a malformed byte into an infinite one. A bad lead byte consumes
	// exactly itself and reports kReplacement.
	[[nodiscard]] inline char32_t NextCodepoint(std::string_view a_text, std::size_t& a_pos)
	{
		if (a_pos >= a_text.size()) {
			return 0;
		}

		const auto byte = [&](std::size_t a_index) {
			return static_cast<std::uint8_t>(a_text[a_index]);
		};

		const std::uint8_t lead = byte(a_pos);

		if (lead < 0x80) {
			++a_pos;
			return lead;
		}

		// How many bytes the lead claims, and the bits it contributes. 0 marks a
		// continuation byte or one of the two lengths UTF-8 retired, either of
		// which is a stray rather than a start.
		std::size_t  length = 0;
		std::uint32_t value = 0;
		if ((lead & 0xE0) == 0xC0) {
			length = 2;
			value = lead & 0x1Fu;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			value = lead & 0x0Fu;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			value = lead & 0x07u;
		} else {
			++a_pos;
			return kReplacement;
		}

		if (a_pos + length > a_text.size()) {
			++a_pos;
			return kReplacement;
		}

		for (std::size_t i = 1; i < length; ++i) {
			const std::uint8_t continuation = byte(a_pos + i);
			if ((continuation & 0xC0) != 0x80) {
				++a_pos;  // truncated sequence: resync from the next byte, not past it
				return kReplacement;
			}
			value = (value << 6) | (continuation & 0x3Fu);
		}

		a_pos += length;

		// Overlong forms and surrogates are rejected on the same grounds as a bad
		// lead byte: they are not the character they encode, and treating them as
		// one would let "。" be smuggled in as something that is not it.
		const bool overlong = (length == 2 && value < 0x80) ||
			(length == 3 && value < 0x800) ||
			(length == 4 && value < 0x10000);
		const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
		if (overlong || surrogate || value > 0x10FFFF) {
			return kReplacement;
		}

		return static_cast<char32_t>(value);
	}

	// ASCII-only lowering, deliberately.
	//
	// std::tolower is locale-dependent for anything above 0x7F, and the game is
	// entitled to have called setlocale before this mod ever runs. Every keyword
	// this is used against is ASCII, so folding non-ASCII bytes could only ever
	// change an answer by accident.
	[[nodiscard]] inline constexpr char AsciiLower(char a_char) noexcept
	{
		return (a_char >= 'A' && a_char <= 'Z') ?
			static_cast<char>(a_char - 'A' + 'a') :
			a_char;
	}
}

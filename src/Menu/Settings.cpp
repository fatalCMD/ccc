#include "SD/Menu/Settings.h"

#include "SD/Camera/Director.h"
#include "SD/Camera/Presets.h"
#include "SD/Camera/Shot.h"
#include "SD/Core/Config.h"
#include "SD/Core/Hotkeys.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/Interface.h"
#include "SD/Scene/KeyLight.h"
#include "SD/Scene/LightRig.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/RegionalFace.h"

#include <chrono>

// Third-party, 11,000 lines, and not built to this project's warning level.
// /W4 /WX would reject it outright, so it is quarantined rather than excused —
// nothing else in the tree gets this treatment.
#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace SD::Menu
{
	namespace
	{
		namespace MF = SKSEMenuFramework;
		namespace Im = ImGuiMCP;

		// --- THE PALETTE, AND THE BYTE ORDER THAT KEEPS CATCHING PEOPLE OUT ----
		//
		// The packed ImU32 overload of PushStyleColor takes **0xAABBGGRR** — alpha,
		// then BLUE, then green, then red. Not RGBA. The two colours that were
		// already in this file prove it: 0xFF66DD66 draws the green checkmark and
		// only reads as green with blue at 0x66 and red at 0x66 around a green
		// 0xDD, and 0xFF3BA0F0 draws the warning line, which is amber — R 0xF0,
		// G 0xA0, B 0x3B — and would be a pale blue if the order were the other way.
		//
		// So: write the RGB you want, then reverse the three bytes.
		//
		// The named ImGuiCol_ constants are not reachable at this scope and the
		// ImVec4 overload does not compile here, so the indices are written out.
		// They are checked against the enum in SKSEMenuFramework.h, where Text is
		// the first entry and CheckMark the nineteenth.
		constexpr int kColText = 0;
		constexpr int kColCheck = 18;
		constexpr int kColButton = 21;
		constexpr int kColButtonHovered = 22;
		constexpr int kColButtonActive = 23;
		constexpr int kColHeader = 24;
		constexpr int kColHeaderHovered = 25;
		constexpr int kColHeaderActive = 26;
		constexpr int kColSeparator = 27;
		constexpr int kColTableHeaderBg = 46;

		// kColTableBorderStrong and kColTableBorderLight went with the grid the
		// Shots page used to draw. Nothing in the panel has cell borders now.

		// Blue is the house colour, and 1.4 spends far less of it than 1.3 did.
		// It marks identity — the page you are on, the rule under a heading, the
		// one action on a panel worth pressing — and nothing else. A blue that is
		// everywhere is not an accent, it is a background.
		constexpr std::uint32_t kBlue = 0xFFFF9C40u;      // RGB(64,156,255)  accent
		constexpr std::uint32_t kBlueLine = 0xFFC87A32u;  // RGB(50,122,200)  rules
		constexpr std::uint32_t kBlueDeep = 0xFF5A3C1Eu;  // RGB(30,60,90)    fills

		constexpr std::uint32_t kTextOn = 0xFFFFFFFFu;
		constexpr std::uint32_t kTextOff = 0xFF7A7A7Au;
		constexpr std::uint32_t kMuted = 0xFF8A8A8Au;
		constexpr std::uint32_t kGreen = 0xFF66DD66u;
		constexpr std::uint32_t kAmber = 0xFF3BA0F0u;

		// --- ICONS ------------------------------------------------------------
		//
		// THE ATLAS IS NOT THE ONE THE TEXT IS DRAWN IN, and everything below
		// follows from that.
		//
		// SKSE Menu Framework exposes PushSolid/PushRegular/PushBrands, which means
		// Font Awesome is a SEPARATE font rather than glyphs merged into the
		// default atlas. So an icon and a word cannot share one string: push the
		// icon font over "Save" and the Latin letters have no glyphs and draw as
		// boxes. Every icon here is therefore its own item, drawn between a push
		// and a pop, with SameLine putting the text beside it.
		//
		// The codepoints are deliberately conservative — all of them are in the
		// f000-f2ff block and all have been in Font Awesome since 4, so they are
		// present whichever build the framework happens to ship. Nothing here is
		// load-bearing either way: every icon sits NEXT TO the word it decorates,
		// so a missing glyph costs a blank square and never the meaning of a
		// control.
		constexpr unsigned kIconCamera = 0xf03du;    // video
		constexpr unsigned kIconShots = 0xf008u;     // film
		constexpr unsigned kIconScreen = 0xf26cu;    // tv
		constexpr unsigned kIconFaces = 0xf118u;     // face
		constexpr unsigned kIconPresets = 0xf1deu;   // sliders
		constexpr unsigned kIconKeys = 0xf11cu;      // keyboard
		constexpr unsigned kIconAbout = 0xf05au;     // circle-info
		constexpr unsigned kIconCheck = 0xf00cu;     // check
		constexpr unsigned kIconWarning = 0xf071u;   // triangle-exclamation
		constexpr unsigned kIconReset = 0xf0e2u;     // arrow-rotate-left
		constexpr unsigned kIconSave = 0xf0c7u;      // floppy-disk
		constexpr unsigned kIconRename = 0xf044u;    // pen-to-square
		constexpr unsigned kIconDelete = 0xf1f8u;    // trash
		constexpr unsigned kIconTimer = 0xf017u;     // clock
		constexpr unsigned kIconLine = 0xf075u;      // comment
		constexpr unsigned kIconHold = 0xf04bu;      // play

		// Cached, because UnicodeToUtf8 builds a std::wstring_convert and a
		// std::string every call and this runs per icon per frame.
		[[nodiscard]] const char* Glyph(unsigned a_codepoint)
		{
			static std::unordered_map<unsigned, std::string> cache;
			auto                                            it = cache.find(a_codepoint);
			if (it == cache.end()) {
				it = cache.emplace(a_codepoint, FontAwesome::UnicodeToUtf8(a_codepoint)).first;
			}
			return it->second.c_str();
		}

		// One icon, in one colour, IN A SLOT OF FIXED WIDTH, leaving the cursor on
		// the same line so the caller's text follows it.
		//
		// THE SLOT IS THE WHOLE POINT. Font Awesome glyphs are not monospaced —
		// the film reel is wide, the keyboard is wider, the clock is narrow — so
		// drawing one and calling SameLine() started every heading at a different
		// x. Down a page of group titles that reads as the text being loosely
		// aligned rather than as icons of different shapes, and it was reported
		// that way.
		//
		// So the advance is fixed and the glyph is padded to it, rather than the
		// glyph deciding where the words begin. Measured in em rather than pixels,
		// because the panel's font scale is not a constant — see Compact.
		//
		// Every font push here is balanced in the same statement, so no path can
		// leave the icon font active over ordinary text. CalcTextSize is asked
		// INSIDE the push, or it would measure the glyph in a font that has no
		// glyph there to measure.
		void Icon(unsigned a_codepoint, std::uint32_t a_colour = kBlue)
		{
			const float slot = Im::GetFontSize() * 1.35f;

			FontAwesome::PushSolid();
			const float width = Im::CalcTextSize(Glyph(a_codepoint)).x;
			Im::PushStyleColor(kColText, a_colour);
			Im::TextUnformatted(Glyph(a_codepoint));
			Im::PopStyleColor();
			FontAwesome::Pop();

			// Clamped at zero so a glyph wider than the slot costs the alignment
			// rather than overlapping the word beside it.
			const float gap = Im::GetStyle()->ItemSpacing.x;
			Im::SameLine(0.0f, std::max(slot - width, 0.0f) + gap);
		}

		// --- DENSITY ----------------------------------------------------------
		//
		// The panel drew at the framework's own font size and padding, which is
		// sized for a page with a handful of controls on it. The Shots page has
		// thirty-nine angles on it, and seven rows of them fitted on screen.
		//
		// Scoped rather than global, and undone in a destructor rather than at the
		// end of each function, because every page has early returns in it and a
		// font scale left pushed would follow the player into another mod's
		// section. SetWindowFontScale is per-window and does not survive the reset;
		// the style vars are a stack and are popped.
		//
		// 0.85 was picked by eye against the reported screenshot: about a fifth
		// more rows per screen, and still comfortably readable at the sizes this
		// menu is actually used at.
		constexpr float kFontScale = 0.85f;

		struct Compact
		{
			Compact()
			{
				Im::SetWindowFontScale(kFontScale);

				// Vertical is where the density is. Horizontal is left roughly
				// alone: the name column on the Shots page is the thing under
				// pressure and squeezing it buys nothing.
				Im::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, Im::ImVec2(6.0f, 3.0f));
				Im::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, Im::ImVec2(4.0f, 2.0f));
				Im::PushStyleVar(ImGuiMCP::ImGuiStyleVar_CellPadding, Im::ImVec2(6.0f, 2.0f));
			}

			~Compact()
			{
				Im::PopStyleVar(3);
				Im::SetWindowFontScale(1.0f);
			}

			Compact(const Compact&) = delete;
			Compact(Compact&&) = delete;
			Compact& operator=(const Compact&) = delete;
			Compact& operator=(Compact&&) = delete;
		};

		// WHAT USED TO BE HERE: BoldText, which drew a string twice a pixel apart
		// to fake a weight the one available font does not have.
		//
		// It was reported, correctly, as the text being doubled. At the UI scale
		// this panel actually runs at, one pixel is not a thicker stroke — it is a
		// second copy of the word, offset and legible as such, and it landed on
		// every heading and on the name of every angle that was switched on. The
		// screenshot that came back had them ringed.
		//
		// There is no bold. Emphasis is COLOUR here: blue for a heading, white for
		// something live, grey for something that is not. That is enough, it is
		// what the rest of the panel already used alongside the fake bold, and it
		// cannot draw twice.
		void Text(std::uint32_t a_colour, const char* a_text)
		{
			Im::PushStyleColor(kColText, a_colour);
			Im::TextUnformatted(a_text);
			Im::PopStyleColor();
		}

		// The identity line at the top of a page: icon, name, and one optional
		// grey fact about the page, on the same line.
		//
		// ONE LINE, and the count is part of why. The Shots page opened with a
		// heading, a sentence explaining what angles are, a tally and a note about
		// changes applying immediately — four lines of chrome above the thing
		// somebody came to the page for, three of which say nothing that is not
		// obvious from looking at it.
		void PageHeader(unsigned a_icon, const char* a_title, const char* a_note = nullptr)
		{
			Icon(a_icon, kBlue);
			Text(kBlue, a_title);

			if (a_note && *a_note) {
				Im::SameLine();
				Text(kMuted, a_note);
			}

			Im::PushStyleColor(kColSeparator, kBlueLine);
			Im::Separator();
			Im::PopStyleColor();
			Im::Spacing();
		}

		// A collapsible group heading, with an icon beside it.
		//
		// The id is split off with ### for the same reason the shot rows do it — a
		// heading whose visible text can ever change must not change identity, or
		// ImGui treats it as a header it has never seen and quietly reopens it.
		[[nodiscard]] bool Group(unsigned a_icon, const char* a_title, const char* a_id,
			bool a_open = true)
		{
			Icon(a_icon, kBlue);

			char label[160]{};
			std::snprintf(label, sizeof(label), "%s###%s", a_title, a_id);

			Im::PushStyleColor(kColHeader, kBlueDeep);
			Im::PushStyleColor(kColHeaderHovered, kBlueLine);
			Im::PushStyleColor(kColHeaderActive, kBlue);
			const bool open = Im::CollapsingHeader(label,
				a_open ? ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen : 0);
			Im::PopStyleColor(3);
			return open;
		}

		// --- THE ROW GRID -----------------------------------------------------
		//
		// EVERY CONTROL ON EVERY PAGE IS ONE ROW OF THIS, and that is the whole of
		// the 1.4 layout.
		//
		// WHAT USED TO BE HERE: ImGui's default label-on-the-RIGHT placement, plus
		// SameLine at hand-picked pixel offsets wherever that was not good enough.
		// Two things were wrong with it. The obvious one is that a settings panel
		// whose names are down the right-hand side in a ragged column cannot be
		// scanned. The other is that fixed offsets are only ever right for one
		// string length, one font size and one window width — the Shots page had
		// already been reported with a name drawn straight through the control
		// beside it, at 210px, on somebody else's resolution.
		//
		// A two-column table cannot be overrun by its neighbour, reflows with the
		// panel, and survives UI scaling, because the widths are proportions rather
		// than pixels. The control column takes whatever is left, so a slider is as
		// long as the window allows and no longer.
		[[nodiscard]] bool BeginRows(const char* a_id)
		{
			constexpr auto kFlags = ImGuiMCP::ImGuiTableFlags_SizingStretchProp |
			                        ImGuiMCP::ImGuiTableFlags_NoSavedSettings |
			                        ImGuiMCP::ImGuiTableFlags_PadOuterX;

			if (!Im::BeginTable(a_id, 2, kFlags)) {
				return false;
			}

			// 40/60. Wide enough for the longest name in the mod ("Return to First
			// Person Afterwards") at one line, and not so wide that a slider loses
			// travel it needs.
			Im::TableSetupColumn("name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.40f, 1);
			Im::TableSetupColumn("value", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.60f, 2);
			return true;
		}

		void EndRows()
		{
			Im::EndTable();
			Im::Spacing();
		}

		// Opens a row: the name in the left column, the cursor left in the right
		// one with the item width already set to fill it.
		//
		// AlignTextToFramePadding is what makes a label sit on the centre line of
		// the control beside it rather than on its top edge, and without it every
		// row in the panel reads as very slightly broken.
		void Row(const char* a_label, bool a_stretch = true)
		{
			Im::TableNextRow();
			Im::TableNextColumn();
			Im::AlignTextToFramePadding();
			Im::TextUnformatted(a_label);
			Im::TableNextColumn();
			if (a_stretch) {
				Im::SetNextItemWidth(Im::GetContentRegionAvail().x);
			}
		}

		// The (?) marker, submitted AFTER the control it belongs to has been asked
		// its questions. See SliderRow for why that ordering is load-bearing.
		void Help(const char* a_help)
		{
			if (!a_help || !*a_help) {
				return;
			}
			Im::SameLine();
			Im::TextDisabled("(?)");
			if (Im::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
				Im::BeginTooltip();
				Im::PushTextWrapPos(Im::GetFontSize() * 30.0f);
				Im::TextUnformatted(a_help);
				Im::PopTextWrapPos();
				Im::EndTooltip();
			}
		}

		// A slider row that writes through.
		//
		// Every control here does three things in one place: draw, persist to
		// SD_user.ini, and push the value live. Splitting those apart is how a
		// settings panel ends up with a value that shows one thing, saves another
		// and takes effect on neither.
		//
		// The visible label is drawn by Row() in the other column, so the widget
		// gets a HIDDEN id built from the ini key — which is unique across the
		// whole file by construction (Deploy.ps1 merges by key name and would
		// otherwise carry a value into the wrong section), so it is also a
		// guaranteed-unique ImGui id.
		bool SliderRow(const char* a_label, int& a_value, int a_min, int a_max,
			const char* a_section, const char* a_key, const char* a_format = "%d",
			const char* a_help = nullptr)
		{
			Row(a_label);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			const int before = a_value;
			Im::SliderInt(id, &a_value, a_min, a_max, a_format);

			// Both questions must be asked HERE, before another widget is
			// submitted.
			//
			// Every ImGui IsItem* query refers to the most recently submitted item,
			// and the help marker below submits one. Asking after it drew meant
			// asking whether a piece of disabled text had just finished being
			// edited, which is never true — so the write was unreachable and
			// nothing ever persisted. The symptom was maddening precisely because
			// the live apply still worked: settings held until the first
			// conversation, then Open() re-read the unchanged file and stamped the
			// defaults back over them.
			//
			// Deactivated-after-edit fires on the frame a drag ends, which is the
			// right moment to touch the disk: once per gesture, not once per frame.
			const bool released = Im::IsItemDeactivatedAfterEdit();
			const bool active = Im::IsItemActive();
			const bool moved = a_value != before;

			Help(a_help);

			// Release is the primary trigger. The second clause is the safety net:
			// a value that changed while the widget is NOT being dragged came from
			// a ctrl-click entry or the keyboard, and those paths do not always
			// produce a deactivation edge. Persistence should not rest on one flag.
			// Neither clause can fire per-frame during a drag, which is the only
			// case worth avoiding — a full-range drag crosses hundreds of values.
			if (released || (moved && !active)) {
				Config::SetInt(a_section, a_key, a_value);
			}

			// Live application keys off the frame-by-frame change, so dragging a
			// floor is felt immediately rather than only on release.
			return moved;
		}

		// A time slider whose value reads in SECONDS inside the track.
		//
		// The store stays hundredths — every timing key in this mod is hundredths
		// and changing that would be a migration for nothing — so the widget is a
		// float over seconds and the conversion happens here. A caption saying
		// "8.00s" beside a slider reading 800 was a whole extra row per control and
		// a number nobody could read without it; putting the unit in the track
		// costs nothing and removes both.
		bool SecondsRow(const char* a_label, int& a_hundredths, int a_min, int a_max,
			const char* a_section, const char* a_key, const char* a_help = nullptr)
		{
			Row(a_label);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			float seconds = static_cast<float>(a_hundredths) / 100.0f;
			Im::SliderFloat(id, &seconds, static_cast<float>(a_min) / 100.0f,
				static_cast<float>(a_max) / 100.0f, "%.2f s",
				ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);

			const bool released = Im::IsItemDeactivatedAfterEdit();
			const bool active = Im::IsItemActive();

			// Rounded rather than truncated, so a slider parked on 8.00 stores 800
			// and not 799. Drift of one hundredth is invisible on screen and is
			// exactly the sort of thing preset comparison would report forever.
			const int rounded = std::clamp(
				static_cast<int>(std::lround(static_cast<double>(seconds) * 100.0)), a_min, a_max);
			const bool moved = rounded != a_hundredths;
			a_hundredths = rounded;

			Help(a_help);

			if (released || (moved && !active)) {
				Config::SetInt(a_section, a_key, a_hundredths);
			}
			return moved;
		}

		// A percentage row over a value stored in THOUSANDTHS of screen height.
		//
		// The letterbox is the only setting in the mod whose stored unit is not
		// what anybody would say out loud. "115" means 11.5% of the frame per bar,
		// and the old panel drew the raw number with the percentage as a caption
		// beside it. The slider now speaks percent and the store keeps thousandths,
		// which is the same trade SecondsRow makes.
		//
		// Whole percent, deliberately. Sub-percent bar height is not a thing anyone
		// has an opinion about, and a value carried over from an older config lands
		// on the nearest percent the first time it is touched.
		bool PercentRow(const char* a_label, int& a_thousandths, int a_maxPercent,
			const char* a_section, const char* a_key)
		{
			Row(a_label);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			int percent = (a_thousandths + 5) / 10;
			Im::SliderInt(id, &percent, 0, a_maxPercent, "%d%%");

			const bool released = Im::IsItemDeactivatedAfterEdit();
			const bool active = Im::IsItemActive();
			const int  wanted = percent * 10;
			const bool moved = wanted != a_thousandths;
			a_thousandths = wanted;

			if (released || (moved && !active)) {
				Config::SetInt(a_section, a_key, a_thousandths);
			}
			return moved;
		}

		// A toggle row. The name is in the left column; the box sits at the left of
		// the right column, where every other control on the page begins.
		bool ToggleRow(const char* a_label, bool& a_value, const char* a_section,
			const char* a_key, const char* a_help = nullptr)
		{
			Row(a_label, false);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			bool changed = false;
			Im::PushStyleColor(kColCheck, kGreen);
			if (Im::Checkbox(id, &a_value)) {
				Config::SetBool(a_section, a_key, a_value);
				changed = true;
			}
			Im::PopStyleColor();

			Help(a_help);
			return changed;
		}

		// An amber note, wrapped, with a warning icon. The one thing on any page
		// that is allowed to be a paragraph.
		void Warning(const char* a_text)
		{
			Icon(kIconWarning, kAmber);
			Im::PushStyleColor(kColText, kAmber);
			Im::TextWrapped("%s", a_text);
			Im::PopStyleColor();
		}

		// Grey metadata under a group: what a setting means for the whole section,
		// or why it is currently doing nothing.
		void Note(const char* a_text)
		{
			Im::PushStyleColor(kColText, kMuted);
			Im::TextWrapped("%s", a_text);
			Im::PopStyleColor();
		}

		// ---- Camera ----------------------------------------------------------

		void __stdcall RenderCamera()
		{
			const Compact compact;

			auto dials = Camera::Director::GetTunables();
			bool changed = false;

			PageHeader(kIconCamera, "CAMERA");

			if (BeginRows("cameraTop")) {
				changed |= ToggleRow("Direct the Camera", dials.enabled,
					"Direction", "bEnabled");

				// Read straight off the ini rather than from Tunables: it is
				// consulted once, in Open(), so there is nothing live to mirror.
				bool restoreFirst = Config::Bool("Direction", "bRestoreFirstPerson", true);
				static_cast<void>(ToggleRow("Return to First Person Afterwards", restoreFirst,
					"Direction", "bRestoreFirstPerson",
					"Only if this mod was the thing that took you out of it. A conversation "
					"you started in third person leaves you in third person either way."));

				EndRows();
			}

			Im::BeginDisabled(!dials.enabled);

			// ---- Per Line Angle Change --------------------------------------
			//
			// THE FIRST OF TWO INDEPENDENT CUT MODES. The section is collapsible
			// and its own first row is the switch, so the heading names a feature
			// and the row underneath says whether it is on — rather than the
			// heading being a switch, which is a control nobody finds.
			if (Group(kIconLine, "Per Line Angle Change", "cutLines")) {
				Im::Indent();

				if (BeginRows("cutLinesGrid")) {
					changed |= ToggleRow("Change Angle Per Line", dials.perLineAngleChange,
						"Direction", "bPerLineAngleChange");
					EndRows();
				}

				Im::BeginDisabled(!dials.perLineAngleChange);

				if (BeginRows("cutLinesDials")) {
					changed |= SliderRow("Minimum Lines", dials.cutEveryMin, 1, 20,
						"Direction", "iCutEveryMin", "%d");
					changed |= SliderRow("Maximum Lines", dials.cutEveryMax, 1, 20,
						"Direction", "iCutEveryMax", "%d");
					changed |= ToggleRow("Ignore Short Lines", dials.holdOnShortLines,
						"Direction", "bHoldOnShortLines");
					EndRows();
				}

				Im::BeginDisabled(!dials.holdOnShortLines);
				if (BeginRows("cutLinesShort")) {
					changed |= SliderRow("Short Line Length", dials.shortLineWords, 2, 20,
						"Direction", "iShortLineWords", "%d words");
					EndRows();
				}
				Im::EndDisabled();

				Note("The camera keeps an angle until it has heard this many lines, "
					 "re-rolled between the two so the rhythm is not countable. "
					 "The count starts again with every reply.");

				Im::EndDisabled();
				Im::Unindent();
			}

			// ---- Reaction Shots ---------------------------------------------
			if (Group(kIconFaces, "Reaction Shots", "cutReactions")) {
				Im::Indent();

				if (BeginRows("cutReactionsGrid")) {
					changed |= ToggleRow("Show You Listening", dials.reactionShots,
						"Direction", "bReactionShots",
						"Occasionally shows one of their lines on you, then cuts back. "
						"Skips short lines and full-intensity lines.");
					EndRows();
				}

				Im::BeginDisabled(!dials.reactionShots);
				if (BeginRows("cutReactionsDials")) {
					changed |= SliderRow("Lines Before A Reaction", dials.reactionEvery, 1, 10,
						"Direction", "iReactionEvery", "%d lines");
					changed |= SliderRow("Reaction Chance", dials.reactionChance, 0, 100,
						"Direction", "iReactionChance", "%d%%");
					EndRows();
				}
				Note("After this many of their lines, the chance is rolled. On a hit, "
					 "their next line plays on you. The count and the roll start again "
					 "with every reply.");
				Im::EndDisabled();

				Im::Unindent();
			}

			// ---- Timed Angle Change -----------------------------------------
			//
			// THE SECOND MODE, AND IT IS GENUINELY INDEPENDENT OF THE FIRST. Off on
			// both halves of the exchange by default, which makes every cut in the
			// mod a motivated one.
			const bool anyTimer = dials.timedCutsWhileSpeaking || dials.timedCutsWhileChoosing;

			if (Group(kIconTimer, "Timed Angle Change", "cutTimed")) {
				Im::Indent();

				if (BeginRows("cutTimedToggles")) {
					changed |= ToggleRow("Timer While Talking", dials.timedCutsWhileSpeaking,
						"Direction", "bTimedCutsWhileSpeaking");
					changed |= ToggleRow("Timer While Choosing", dials.timedCutsWhileChoosing,
						"Direction", "bTimedCutsWhileChoosing");
					EndRows();
				}

				// The one control the timers own. Disabled with them, because a
				// duration for a timer that cannot fire is a live-looking control
				// that does nothing.
				Im::BeginDisabled(!anyTimer);
				if (BeginRows("cutTimedDials")) {
					changed |= SecondsRow("Change After", dials.maxShotTime, 100, 2000,
						"Direction", "iMaxShotTime");
					EndRows();
				}
				Im::EndDisabled();

				if (!anyTimer) {
					Note("Both timers are off, so angles change on lines alone.");
				}

				Im::Unindent();
			}

			// ---- Holds -------------------------------------------------------
			//
			// SAFETY FLOORS, NOT A CUT MODE, and that is why they are their own
			// group rather than living under the timers. Every cut in the mod is
			// gated by these during normal pacing, so greying them out with
			// the timers would hide the controls that decide how the line-based
			// mode feels.
			if (Group(kIconHold, "Holds", "cutHolds", false)) {
				Im::Indent();
				if (BeginRows("cutHoldsGrid")) {
					changed |= SecondsRow("Shortest Hold", dials.minShotTime, 30, 900,
						"Direction", "iMinShotTime",
						"Minimum hold for normal cuts. Keep Subject Visible can recover sooner "
						"when the subject is obstructed.");
					changed |= SecondsRow("Shortest On A Turn", dials.minTurnTime, 0, 200,
						"Direction", "iMinTurnTime",
						"The floor when the conversation passes between the two of you, "
						"which is the one moment an angle is allowed to be cut short.");
					changed |= SecondsRow("Stay On You After You Pick", dials.playerBeat, 0, 300,
						"Direction", "iPlayerBeat");
					changed |= SecondsRow("Stay On You After You Speak", dials.playerVoiceHold, 0, 300,
						"Direction", "iPlayerVoiceHold",
						"For voiced player lines: how long the camera stays on you after your "
						"line ends. 0 cuts immediately.");
					EndRows();
				}
				Im::Unindent();
			}

			// ---- Framing -----------------------------------------------------
			if (Group(kIconCamera, "Framing", "cutFraming", false)) {
				Im::Indent();
				if (BeginRows("cutFramingGrid")) {
					changed |= ToggleRow("Cut To You On Your Turn", dials.coverPlayerTurn,
						"Direction", "bCoverPlayerTurn");

					if (ToggleRow("Keep Subject Visible", dials.protectSubject,
						"Direction", "bKeepSubjectVisible",
						"Choose clear enabled shots and check the subject while the shot plays. "
						"Foreground objects matter only when they cover the face or touch the lens. "
						"Always checks actor obstructions. Enabling this turns off Ignore "
						"Obstructions Mid-Shot.")) {
						changed = true;
						if (dials.protectSubject) {
							dials.holdPlacement = false;
							Config::SetBool("Direction", "bHoldPlacement", false);
						}
					}

					Im::BeginDisabled(!dials.protectSubject);
					changed |= ToggleRow("First-Person Fallback", dials.firstPersonFallback,
						"Direction", "bFirstPersonFallback",
						"If no clear enabled shot is available, use first person while the "
						"conversation continues. Return after a cinematic view stays clear. "
						"Off can hold the current enabled shot even if obstructed. If it is "
						"disabled or unavailable, the normal game camera takes over.");
					Im::EndDisabled();

					Im::BeginDisabled(dials.protectSubject);
					changed |= ToggleRow("Avoid Framing Bystanders", dials.avoidCrowds,
						"Direction", "bAvoidCrowds",
						"Angles with somebody standing across the sightline score lower, so "
						"the camera picks around a crowd. Keep Subject Visible always checks "
						"actors and takes over this setting while enabled.");
					Im::EndDisabled();

					// THE 180-DEGREE RULE. It is a real piece of film grammar with
					// a real consequence, and it is the one control here whose name
					// cannot explain itself to somebody who has not met it.
					// True 180 depends on Never Cross, so it turns it on and locks it.
					if (ToggleRow("True 180 Rule", dials.true180,
						"Direction", "bTrue180",
						"Films you over one shoulder and them over the opposite one, so the "
						"camera stays on one side of the conversation. Also turns on Never "
						"Cross The Eyeline.")) {
						changed = true;
						if (dials.true180) {
							dials.enforceLine = true;
							Config::SetBool("Direction", "bEnforceLine", true);
						}
					}

					Im::BeginDisabled(dials.true180);
					changed |= ToggleRow("Never Cross The Eyeline", dials.enforceLine,
						"Direction", "bEnforceLine",
						"Stops the camera swinging an angle across the line between you while "
						"it looks for room. Reverse shots can still land on the far side; "
						"True 180 Rule fixes that.");
					Im::EndDisabled();

					if (ToggleRow("Ignore Obstructions Mid-Shot", dials.holdPlacement,
						"Direction", "bHoldPlacement",
						"Check for walls and bodies when the angle is chosen, then stop. "
						"The camera holds still while a cart or a passer-by crosses frame, "
						"instead of easing in and back out. It also stops getting out of "
						"the way, so a conversation that walks somewhere can clip. Enabling "
						"this turns off Keep Subject Visible.")) {
						changed = true;
						if (dials.holdPlacement) {
							dials.protectSubject = false;
							Config::SetBool("Direction", "bKeepSubjectVisible", false);
						}
					}
					EndRows();
				}
				if (dials.protectSubject) {
					Note("Subject protection includes actor checks. Objects beside the face "
						 "can stay in the foreground.");
				}
				Im::Unindent();
			}

			Im::EndDisabled();

			if (changed) {
				Camera::Director::ApplyTunables(dials);
			}
		}

		// ---- Screen ----------------------------------------------------------

		void __stdcall RenderScreen()
		{
			const Compact compact;

			auto dials = Camera::Director::GetTunables();
			bool changed = false;

			PageHeader(kIconScreen, "SCREEN");

			if (Group(kIconScreen, "Black Bars", "screenBars")) {
				Im::Indent();
				if (BeginRows("screenBarsGrid")) {
					// TWO WAYS TO SAY OFF, AND ONE OF THEM USED TO WIN SILENTLY.
					//
					// Before 1.4 the only letterbox control was the height, so
					// "I don't want bars" was expressed by dragging it to zero.
					// 1.4 added the switch the setting always needed — and left
					// anybody who had done that with a switch that does nothing
					// when they tick it, because bars of zero height are invisible
					// however the switch is set.
					//
					// Turning the feature ON therefore gives it a height to have.
					// Only from zero, and only on the tick: a height somebody
					// deliberately set is never overwritten, and neither is a zero
					// they are looking straight at with the switch already on.
					if (ToggleRow("Black Bars", dials.letterbox,
							"Direction", "bLetterbox")) {
						changed = true;

						if (dials.letterbox && dials.letterboxHeight <= 0) {
							const Camera::Tunables shipped{};
							dials.letterboxHeight = shipped.letterboxHeight;
							Config::SetInt("Direction", "iLetterboxHeight",
								dials.letterboxHeight);
							Log::Info(Log::Category::kCore,
								"Black bars switched on at zero height; set to the shipped "
								"{}%."sv,
								dials.letterboxHeight / 10);
						}
					}

					Im::BeginDisabled(!dials.letterbox);
					changed |= PercentRow("Bar Height", dials.letterboxHeight, 30,
						"Direction", "iLetterboxHeight");
					Im::EndDisabled();

					EndRows();
				}
				Im::Unindent();
			}

			if (Group(kIconLine, "Dialogue", "screenDialogue")) {
				Im::Indent();

				if (BeginRows("screenDialogueGrid")) {
					changed |= ToggleRow("Fade Out Dialogue", dials.fadeTopicList,
						"Direction", "bFadeTopicList");
					changed |= ToggleRow("Hide NPC Name", dials.hideSpeakerName,
						"Direction", "bHideSpeakerName");
					EndRows();
				}

				Im::BeginDisabled(!dials.fadeTopicList);
				if (BeginRows("screenFadeGrid")) {
					changed |= ToggleRow("Fade After PC Line", dials.fadeAfterPlayerLine,
						"Direction", "bFadeAfterPlayerLine",
						"Holds your options on screen for as long as your own voiced line "
						"is playing, then starts the delay. Needs a player-voice mod to do "
						"anything; without one your line is instant and the fade begins at "
						"the click either way.");
					changed |= SecondsRow("Fade Delay", dials.choiceFadeDelay, 0, 600,
						"Direction", "iChoiceFadeDelay");
					changed |= SecondsRow("Fade Time", dials.choiceFadeTime, 5, 200,
						"Direction", "iChoiceFadeTime");
					EndRows();
				}
				Im::EndDisabled();

				if (!dials.fadeTopicList) {
					Note("Your options stay at full opacity for the whole conversation.");
				}

				Im::Unindent();
			}

			if (changed) {
				Camera::Director::ApplyTunables(dials);
			}
		}

		// ---- Faces -----------------------------------------------------------
		// Coordinated expressions and independent mouth fallback.
		void __stdcall RenderFaces()
		{
			const Compact compact;

			PageHeader(kIconFaces, "FACES");

			if (Group(kIconFaces, "Expressions", "faceExpressions")) {
				Im::Indent();
				bool expressions = Scene::Performance::ExpressionsEnabled();
				bool regional = Scene::RegionalFace::Enabled();
				bool touched = false;
				if (BeginRows("faceExpressionRows")) {
					touched |= ToggleRow("Responsive Expressions", expressions,
						"Performance", "bExpressions");
					Im::BeginDisabled(!expressions);
					if (ToggleRow("Eye & Cheek Detail", regional, "Performance", "bRegionalExpressions"))
						Scene::RegionalFace::SetEnabled(regional);
					Im::EndDisabled();
					EndRows();
				}
				if (touched) Scene::Performance::Configure(expressions, false);
				Note("Reactions follow dialogue intent and carry briefly between related lines. "
					"Eye & Cheek Detail adds regional movement on supported heads while preserving mouth lip sync.");
				Im::Unindent();
			}

			if (Group(kIconFaces, "Mouth", "faceVoice")) {
				Im::Indent();

				bool synth = Scene::LipSync::Enabled();
				int  mouth = Scene::LipSync::StrengthPercent();

				bool touched = false;

				if (BeginRows("faceVoiceMouth")) {
					touched |= ToggleRow("Lip Sync Fallback", synth,
						"Performance", "bSynthLipSync");

					Im::BeginDisabled(!synth);
					touched |= SliderRow("Mouth Movement", mouth, 0, 100,
						"Performance", "iLipSyncStrength", "%d%%");
					Im::EndDisabled();
					EndRows();
				}

				// SHOWN ONLY WHILE IT IS ON, because a warning about a feature
				// nobody has enabled is noise on a page they came to for something
				// else.
				if (synth) {
					Warning("For DBVO 1 or DBVO 2 only. Dragonborn ReVoiced is the better "
							"choice and drives your mouth itself \xe2\x80\x94 running both means "
							"two mods writing the same visemes.");
					Im::Spacing();
				}

				if (touched) {
					Scene::LipSync::Configure(synth, mouth);
				}

				Im::Unindent();
			}

			Note("Brow and squint poses are part of each expression profile, not a separate speech effect. "
				"Strength is automatically capped and tuned for cinematic acting. Edit Expression.* sections in SD_user.ini to customize profiles. Mouth lip sync stays independent.");
		}

		// ---- Keys ------------------------------------------------------------

		// ANY KEY, CAPTURED FROM THE GAME'S OWN INPUT.
		//
		// This was a dropdown of about forty hand-picked keys, which is a worse
		// control for one honest reason and one dishonest one. The honest one: no
		// list is ever the key somebody wanted. The dishonest one: the list existed
		// because reading the keyboard through ImGui needs a translation table
		// between its key enum and the DirectX scan codes the game reports, and
		// that table is wrong in exactly the places nobody tests.
		//
		// Capturing from the game's OWN input stream sidesteps the translation
		// entirely — whatever the sink records is by construction the code that
		// will later match. See Hotkeys.h.
		using Action = Core::Hotkeys::Action;

		void KeyRow(const char* a_label, Action a_action)
		{
			const bool waiting = Core::Hotkeys::CapturingFor(a_action);
			const auto code = Core::Hotkeys::Binding(a_action);
			const auto name = Core::Hotkeys::KeyName(code);

			Im::PushID(a_label);
			Row(a_label, false);

			char button[96]{};
			if (waiting) {
				std::snprintf(button, sizeof(button), "Press any key...###bind");
			} else {
				std::snprintf(button, sizeof(button), "%.*s###bind",
					static_cast<int>(name.size()), name.data());
			}

			Im::PushStyleColor(kColButton, kBlueDeep);
			Im::PushStyleColor(kColButtonHovered, kBlueLine);
			Im::PushStyleColor(kColButtonActive, kBlue);
			if (Im::Button(button, ImGuiMCP::ImVec2{ 170.0f, 0.0f })) {
				if (waiting) {
					Core::Hotkeys::Cancel();
				} else {
					Core::Hotkeys::Arm(a_action);
				}
			}
			Im::PopStyleColor(3);

			if (code != 0 && !waiting) {
				Im::SameLine();
				if (Im::SmallButton("Clear")) {
					Core::Hotkeys::SetBinding(a_action, 0);
				}
			}

			if (waiting) {
				Im::SameLine();
				Im::TextDisabled("Esc cancels");
			}

			Im::PopID();
		}

		// THE FRAMEWORK'S OWN INPUT ROUTE, AND WHY THE GAME'S IS NOT ENOUGH.
		//
		// A capture widget lives inside the settings menu, and while that menu is
		// open the framework is intercepting input to drive ImGui. Relying on the
		// game's own event sink to see the binding press is therefore relying on
		// exactly the case least likely to hold — a control that would work
		// everywhere except where it is used.
		//
		// Returns false unconditionally: this observes, it does not claim the
		// press. What "true" means to the framework is not documented here, and
		// guessing at consume semantics is how the removed dialogue handler's whole
		// class of bug started. OfferKey is idempotent, so a key arriving by both
		// routes is taken once.
		bool __stdcall OnMenuInput(RE::InputEvent* a_event)
		{
			if (!a_event || !Core::Hotkeys::Capturing()) {
				return false;
			}

			auto* button = a_event->AsButtonEvent();
			if (!button || !button->IsDown()) {
				return false;
			}
			if (a_event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
				return false;
			}

			static_cast<void>(Core::Hotkeys::OfferKey(button->GetIDCode()));
			return false;
		}

		void __stdcall RenderControls()
		{
			const Compact compact;

			// Polled here rather than in the input sink, because binding writes the
			// ini and the input thread is not where a file write belongs.
			Core::Hotkeys::PollCapture();

			PageHeader(kIconKeys, "KEYS");

			if (BeginRows("keyGrid")) {
				KeyRow("Next Angle", Action::kNextAngle);
				KeyRow("Who To Look At", Action::kFraming);
				EndRows();
			}

			Im::Separator();
			Im::Spacing();

			if (!Camera::Director::Staging()) {
				Note("Not in a conversation.");
			} else {
				const auto label = Camera::FramingLabel(Camera::Director::CurrentFraming());
				Icon(kIconCheck, kGreen);
				Im::Text("Looking at: %.*s", static_cast<int>(label.size()), label.data());
			}
		}

		// ---- Shots -----------------------------------------------------------

		using Camera::ShotType;

		// The three pools, in the order they read rather than the order they are
		// declared. Shoulder shots first in each because they are the staples and
		// the ones anybody looking at this page came to find.
		constexpr std::array kNpcShots{
			ShotType::kOverPlayerShoulder,
			ShotType::kOverPlayerShoulderLow,
			ShotType::kOverPlayerShoulderHigh,
			ShotType::kOverPlayerShoulderWide,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kCloseUp,
			ShotType::kExtremeClose,
			ShotType::kCloseLow,
			ShotType::kCloseHigh,
			ShotType::kCloseProfile,
			ShotType::kCloseWide,
			ShotType::kMediumNpc,
			ShotType::kMediumProfile,
			ShotType::kLowAngle,
			ShotType::kLowProfile,
			ShotType::kLongNpc,
			ShotType::kOverhead,
		};

		constexpr std::array kPlayerShots{
			ShotType::kOverNpcShoulder,
			ShotType::kOverNpcShoulderLow,
			ShotType::kOverNpcShoulderHigh,
			ShotType::kOverNpcShoulderWide,
			ShotType::kDirtyPlayer,
			ShotType::kThreeQuarterPlayer,
			ShotType::kClosePlayer,
			ShotType::kExtremeClosePlayer,
			ShotType::kMediumPlayer,
			ShotType::kPlayerProfile,
			ShotType::kPlayerLow,
			ShotType::kHighAngle,
			ShotType::kLongPlayer,
			ShotType::kPlayerOverhead,
		};

		constexpr std::array kRoomShots{
			ShotType::kTwoShot,
			ShotType::kProfile,
			ShotType::kWide,
			ShotType::kMaster,
			ShotType::kGroundLevel,
			ShotType::kDistant,
			ShotType::kDistantLow,
		};

		// A shot missing from all three lists is a shot the player can never
		// switch off, and nothing at runtime would ever say so. Adding one to the
		// enum without adding it here now fails the build instead.
		static_assert(
			kNpcShots.size() + kPlayerShots.size() + kRoomShots.size() ==
				static_cast<std::size_t>(ShotType::kCount),
			"Every shot must appear on the Shots page, or it cannot be turned off.");

		// THE INERT PAIR A SETUP WITH NO EFFECT CARRIES.
		//
		// Amount and Duration mean nothing on a locked setup — nothing moves, over
		// any duration — so the panel hides them. What it must NOT do is leave
		// whatever they happened to hold, because preset comparison reads both
		// whether or not the move uses them: two setups both showing "None" would
		// then report different drift, and a preset would stop recognising itself
		// for a reason with nothing on screen to explain it.
		//
		// These are the same numbers the built-in looks write for a locked setup.
		constexpr int kInertAmount = 0;
		constexpr int kInertTime = 400;

		// The moves, ordered as the enum is, because that ordering is the ini's
		// storage format — see the note on Move.
		constexpr std::array kMoves{
			Camera::Move::kLocked, Camera::Move::kPushIn, Camera::Move::kPullOut,
			Camera::Move::kCraneUp, Camera::Move::kCraneDown,
			Camera::Move::kTiltUp, Camera::Move::kTiltDown,
			Camera::Move::kDrift,
			Camera::Move::kZoomIn, Camera::Move::kZoomOut,
			Camera::Move::kOrbitLeft, Camera::Move::kOrbitRight,
			Camera::Move::kTruckLeft, Camera::Move::kTruckRight,
		};

		// "None" rather than "Locked Off" for the no-move entry. Everything else
		// keeps the name the shot table gives it.
		[[nodiscard]] const char* EffectLabel(Camera::Move a_move)
		{
			return a_move == Camera::Move::kLocked ? "None" : Camera::MoveLabel(a_move).data();
		}

		void SetEffect(ShotType a_type, Camera::Move a_move)
		{
			Camera::Shot::SetMove(a_type, a_move);
			Config::SetInt("Shots", Camera::MoveKey(a_type), static_cast<int>(a_move));

			// Choosing None settles the two dials it hides. See kInertAmount.
			if (a_move == Camera::Move::kLocked) {
				Camera::Shot::SetMoveAmount(a_type, kInertAmount);
				Camera::Shot::SetMoveTime(a_type, kInertTime);
				Config::SetInt("Shots", Camera::MoveAmountKey(a_type), kInertAmount);
				Config::SetInt("Shots", Camera::MoveTimeKey(a_type), kInertTime);
			}
		}

		// The editor. Everything about one angle, in a window that is not competing
		// with thirty-eight others for height.
		void ShotEditor(ShotType a_type)
		{
			Im::SetNextWindowSize(ImGuiMCP::ImVec2{ 460.0f, 0.0f });
			if (!Im::BeginPopup("###edit")) {
				return;
			}

			// A POPUP IS ITS OWN WINDOW, and the font scale is per-window.
			//
			// The style vars the page pushed are on a global stack and reach in
			// here already; the scale does not, so without this the editor would
			// open at full size over a panel drawn at 0.85 and read as a different
			// mod's dialog.
			//
			// SET RATHER THAN SCOPED, deliberately. Compact resets the scale in its
			// destructor, which here would run after EndPopup — by which point the
			// current window is the PANEL again, so the guard would helpfully undo
			// the page's own scale halfway down the page. A popup window is this
			// mod's alone and nothing else ever draws into it, so there is nothing
			// to hand back.
			Im::SetWindowFontScale(kFontScale);

			const auto name = Camera::Name(a_type);
			Icon(kIconShots, kBlue);
			Im::Text("%.*s", static_cast<int>(name.size()), name.data());
			Im::SameLine();
			Im::TextDisabled("- %.*s", static_cast<int>(Camera::SubjectName(a_type).size()),
				Camera::SubjectName(a_type).data());
			Im::Separator();
			Im::Spacing();

			const int authoredWeight = Camera::AuthoredWeight(a_type);
			const int authoredLens = static_cast<int>(Camera::AuthoredLens(a_type));

			int  weight = Camera::Shot::Weight(a_type);
			int  lens = Camera::Shot::Lens(a_type);
			auto current = Camera::Shot::MoveOf(a_type);

			if (BeginRows("shotEdit")) {
				Row("Effect");
				if (Im::BeginCombo("###effect", EffectLabel(current))) {
					for (const auto move : kMoves) {
						const bool selected = move == current;
						if (Im::Selectable(EffectLabel(move), selected)) {
							SetEffect(a_type, move);
							current = move;
						}
						if (selected) {
							Im::SetItemDefaultFocus();
						}
					}
					Im::EndCombo();
				}

				// HIDDEN RATHER THAN GREYED, and that is a change from 1.3.
				//
				// The old panel disabled them, on the argument that a control which
				// vanishes takes the explanation of why with it. True in general
				// and wrong here: the explanation is one word and it is already on
				// screen, in the row above, reading "None". Two dead rows under it
				// are just clutter on the densest popup in the mod.
				if (current != Camera::Move::kLocked) {
					int amount = Camera::Shot::MoveAmount(a_type);
					if (SliderRow("Amount", amount, 0, 100, "Shots",
							Camera::MoveAmountKey(a_type), "%d%%")) {
						Camera::Shot::SetMoveAmount(a_type, amount);
					}

					int moveTime = Camera::Shot::MoveTime(a_type);
					if (SecondsRow("Duration", moveTime, 30, 900, "Shots",
							Camera::MoveTimeKey(a_type))) {
						Camera::Shot::SetMoveTime(a_type, moveTime);
					}
				}

				// "FOV" rather than "Lens". The unit is degrees of field of view and
				// the 1.4 panel names controls after their unit wherever one exists,
				// so the number in the track can be read without a caption.
				if (SliderRow("FOV", lens, Camera::kMinLens, Camera::kMaxLens, "Shots",
						Camera::LensKey(a_type), "%d\xc2\xb0")) {
					Camera::Shot::SetLens(a_type, lens);
				}

				// "Frequency" rather than "Weight". The number is a weight and
				// behaves like one, but nobody outside the code has to know that:
				// what they need to know is that doubling it doubles how often the
				// angle comes up.
				if (SliderRow("Frequency", weight, 0, 100, "Shots",
						Camera::WeightKey(a_type), weight == 0 ? "never" : "%d")) {
					Camera::Shot::SetWeight(a_type, weight);
				}

				EndRows();
			}

			// Lighting, only where it can do anything. The Light page is gone for
			// 1.4 and these are reachable only by hand-setting [Lighting] bPerShot,
			// which is what keeps the experimental rig available without putting a
			// question to somebody who never asked it.
			if (Config::Bool("Lighting", "bPerShot", false)) {
				Im::Spacing();
				Im::SeparatorText("Light");

				const auto looks = Scene::AllLooks();
				int        look = Camera::Shot::LightOf(a_type);
				if (look < 0 || static_cast<std::size_t>(look) >= looks.size()) {
					look = Scene::FindLook(Camera::AuthoredLight(a_type));
				}
				if (look < 0) {
					look = Scene::DefaultLook();
				}

				if (BeginRows("shotLight")) {
					Row("Look");
					if (Im::BeginCombo("###look", looks[static_cast<std::size_t>(look)].name)) {
						for (std::size_t i = 0; i < looks.size(); ++i) {
							const bool selected = static_cast<int>(i) == look;
							if (Im::Selectable(looks[i].name, selected)) {
								Camera::Shot::SetLight(a_type, static_cast<int>(i));
								Config::SetString("Shots", Camera::LightKey(a_type), looks[i].key);
								look = static_cast<int>(i);
							}
							if (Im::IsItemHovered()) {
								Im::SetTooltip("%s", looks[i].summary);
							}
							if (selected) {
								Im::SetItemDefaultFocus();
							}
						}
						Im::EndCombo();
					}

					int  lx = Camera::Shot::LightOffsetX(a_type);
					int  ly = Camera::Shot::LightOffsetY(a_type);
					int  lz = Camera::Shot::LightOffsetZ(a_type);
					bool nudged = false;

					nudged |= SliderRow("Left / Right", lx, -400, 400, "Shots",
						Camera::LightXKey(a_type));
					nudged |= SliderRow("Near / Far", ly, -400, 400, "Shots",
						Camera::LightYKey(a_type));
					nudged |= SliderRow("Down / Up", lz, -400, 400, "Shots",
						Camera::LightZKey(a_type));

					if (nudged) {
						Camera::Shot::SetLightOffset(a_type, lx, ly, lz);
					}

					EndRows();
				}
			}

			// ONE DEFAULT PER ANGLE, and it appears only when there is something to
			// undo.
			//
			// Deliberately does NOT touch the on/off. Somebody who has switched off
			// thirty-three angles to build a look has made the most expensive
			// decision on this page, and a button labelled Default that undid it on
			// a misclick would be unforgivable. Turning an angle back on is one tick
			// and is nobody's accident.
			const auto authoredMove = Camera::Shot::AuthoredMove(a_type);
			const int  authoredAmount = Camera::Shot::AuthoredMoveAmount(a_type);
			const int  authoredTime = Camera::Shot::AuthoredMoveTime(a_type);

			const bool tuned = weight != authoredWeight || lens != authoredLens ||
				current != authoredMove ||
				Camera::Shot::MoveAmount(a_type) != authoredAmount ||
				Camera::Shot::MoveTime(a_type) != authoredTime;

			Im::Spacing();
			Im::Separator();

			Im::BeginDisabled(!tuned);
			Icon(kIconReset, tuned ? kBlue : kMuted);
			if (Im::Button("Put This Angle Back")) {
				Camera::Shot::SetWeight(a_type, authoredWeight);
				Camera::Shot::SetLens(a_type, authoredLens);
				Camera::Shot::SetMove(a_type, authoredMove);
				Camera::Shot::SetMoveAmount(a_type, authoredAmount);
				Camera::Shot::SetMoveTime(a_type, authoredTime);

				Config::SetInt("Shots", Camera::WeightKey(a_type), authoredWeight);
				Config::SetInt("Shots", Camera::LensKey(a_type), authoredLens);
				Config::SetInt("Shots", Camera::MoveKey(a_type), static_cast<int>(authoredMove));
				Config::SetInt("Shots", Camera::MoveAmountKey(a_type), authoredAmount);
				Config::SetInt("Shots", Camera::MoveTimeKey(a_type), authoredTime);
			}
			Im::EndDisabled();

			Im::SameLine();
			if (Im::Button("Close")) {
				Im::CloseCurrentPopup();
			}

			Im::EndPopup();
		}

		// How many cells one angle occupies. Declared once, because three places
		// have to agree: the setup, the fill, and the blank half of an odd row.
		constexpr int kShotColumns = 4;

		// FOUR COLUMNS, DOWN FROM SIX, AND THE TWO THAT WENT WERE THE TWO NOBODY
		// SCANS FOR.
		//
		// Six columns twice over is twelve, and twelve does not fit: the reported
		// screenshot has the header reading "Wei..." and a value reading
		// "locked of", which is a table telling you it has run out of room. The
		// answer is fewer columns rather than smaller text.
		//
		// FOV went first. It is a number between 20 and 150 that means something
		// only next to the angle's fill, which is not on the row either, so on its
		// own it was decoration. Effect went with it: "Push in" and "Zoom out" are
		// what an angle DOES once it is chosen, which is a tuning question, and
		// tuning happens in the editor one click away.
		//
		// What is left is what somebody actually reads a list of thirty-nine
		// angles for: which ones are on, what they are called, and how often each
		// comes up. Everything else is still one click away and now has room to be
		// laid out properly when it gets there.
		//
		// The user_id argument is not decoration. Column identity is hashed from
		// the label, and "On" appearing twice in one table is two columns claiming
		// one id; giving each its own number keeps them apart.
		// MEASURED, NOT GUESSED, and the guesses are why this needed fixing twice.
		//
		// 26 pixels for the checkbox and 68 for "Frequency" were right at one font
		// size and one cell padding, and both changed underneath them: the reported
		// screenshot has a checkbox clipped by its own column and a header reading
		// "Freque...". A fixed width in pixels is a bet on a font the panel does
		// not promise, and the panel now scales its own font as well.
		//
		// So each column asks for exactly what it holds. A checkbox is square and
		// its side is the frame height; a header is as wide as its text; a button
		// is its label plus its frame. All three move together when the scale does.
		void SetupShotColumns(int a_half)
		{
			constexpr auto kFixed = ImGuiMCP::ImGuiTableColumnFlags_WidthFixed;
			constexpr auto kStretch = ImGuiMCP::ImGuiTableColumnFlags_WidthStretch;
			constexpr auto kNoHeader = ImGuiMCP::ImGuiTableColumnFlags_NoHeaderLabel;

			const auto id = [a_half](int a_index) {
				return static_cast<unsigned>(a_half * kShotColumns + a_index);
			};

			const auto* style = Im::GetStyle();
			const float cell = style->CellPadding.x * 2.0f;
			const float frame = style->FramePadding.x * 2.0f;

			// A pixel or two of slack on top of the exact fit. A checkbox is
			// exactly GetFrameHeight() square and a button is exactly its text plus
			// its padding, so an exact column is one rounding error away from
			// clipping the thing it was measured for — which is precisely how the
			// tick came back cut off.
			constexpr float kSlack = 4.0f;

			const float onWidth = Im::GetFrameHeight() + cell + kSlack;
			const float freqWidth = Im::CalcTextSize("Frequency").x + cell + kSlack;
			const float editWidth = Im::CalcTextSize("Edit").x + frame + cell + kSlack;

			Im::TableSetupColumn("", kFixed | kNoHeader, onWidth, id(1));
			Im::TableSetupColumn("Angle", kStretch, 1.0f, id(2));
			Im::TableSetupColumn("Frequency", kFixed, freqWidth, id(3));
			Im::TableSetupColumn("", kFixed | kNoHeader, editWidth, id(4));
		}

		// One angle, filling the next four cells wherever the cursor happens to be.
		void ShotCells(ShotType a_type)
		{
			Im::PushID(static_cast<int>(a_type));

			bool      on = Camera::Shot::Enabled(a_type);
			const int weight = Camera::Shot::Weight(a_type);

			// An angle that is switched on but weighted to never is not in play, and
			// the header count has always said so. The name says so too.
			const bool live = on && weight > 0;

			Im::TableNextColumn();
			Im::PushStyleColor(kColCheck, kGreen);
			if (Im::Checkbox("###on", &on)) {
				Config::SetBool("Shots", Camera::Key(a_type), on);
				Camera::Shot::SetEnabled(a_type, on);
			}
			Im::PopStyleColor();

			// WHITE IF IT IS IN PLAY, GREY IF IT IS NOT, and nothing else.
			//
			// This was the fake bold, and it was the doubled text in the report.
			// The tinted cell behind it went at the same time: with row striping
			// underneath, a blue wash on some cells and not others made the list
			// read as a spreadsheet somebody had been highlighting in.
			Im::TableNextColumn();
			Im::AlignTextToFramePadding();
			const auto name = Camera::Name(a_type);
			char       label[96]{};
			std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data());
			Text(live ? kTextOn : kTextOff, label);

			// HOW OFTEN, which is the one number worth having on the row.
			Im::TableNextColumn();
			Im::AlignTextToFramePadding();
			if (weight == 0) {
				Im::TextDisabled("never");
			} else {
				Im::TextDisabled("%d", weight);
			}

			Im::TableNextColumn();
			Im::PushStyleColor(kColButton, kBlueDeep);
			Im::PushStyleColor(kColButtonHovered, kBlueLine);
			Im::PushStyleColor(kColButtonActive, kBlue);
			if (Im::SmallButton("Edit")) {
				Im::OpenPopup("###edit");
			}
			Im::PopStyleColor(3);

			// Submitted inside the same PushID, so every angle's popup is its own.
			// A popup is drawn at window level, so being inside a cell costs it
			// nothing.
			ShotEditor(a_type);

			Im::PopID();
		}

		// The cells an absent angle leaves behind, when a group has an odd count
		// and the last row is half empty.
		void EmptyShotCells()
		{
			for (int i = 0; i < kShotColumns; ++i) {
				Im::TableNextColumn();
			}
		}

		[[nodiscard]] int CountOn(std::span<const ShotType> a_shots)
		{
			int on = 0;
			for (const auto type : a_shots) {
				if (Camera::Shot::Enabled(type) && Camera::Shot::Weight(type) > 0) {
					++on;
				}
			}
			return on;
		}

		void SetAll(std::span<const ShotType> a_shots, bool a_on)
		{
			for (const auto type : a_shots) {
				Camera::Shot::SetEnabled(type, a_on);
				Config::SetBool("Shots", Camera::Key(type), a_on);
			}
		}

		void ShotSection(const char* a_title, const char* a_id, std::span<const ShotType> a_shots,
			const char* a_empty)
		{
			const int on = CountOn(a_shots);
			const int total = static_cast<int>(a_shots.size());

			char heading[128]{};
			std::snprintf(heading, sizeof(heading), "%s  -  %d of %d on###%s",
				a_title, on, total, a_id);

			Icon(kIconShots, kBlue);

			Im::PushStyleColor(kColHeader, kBlueDeep);
			Im::PushStyleColor(kColHeaderHovered, kBlueLine);
			Im::PushStyleColor(kColHeaderActive, kBlue);
			const bool open = Im::CollapsingHeader(heading, ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen);
			Im::PopStyleColor(3);

			if (!open) {
				return;
			}

			Im::PushID(a_id);

			// AN EMPTY GROUP IS A REAL FAILURE, not a preference, and nothing said
			// so. The picker draws from these three pools by side of the
			// conversation. Empty one and there is nothing for it to return for
			// that half, so the camera falls through to the emergency single for
			// every line of it.
			if (on == 0) {
				Warning(a_empty);
			}

			Im::PushStyleColor(kColButton, kBlueDeep);
			Im::PushStyleColor(kColButtonHovered, kBlueLine);
			Im::PushStyleColor(kColButtonActive, kBlue);
			if (Im::SmallButton("All on")) {
				SetAll(a_shots, true);
			}
			Im::SameLine();
			if (Im::SmallButton("All off")) {
				SetAll(a_shots, false);
			}
			Im::PopStyleColor(3);

			// STRIPES, NOT A GRID.
			//
			// This drew a full border around every one of twelve cells per row, in
			// blue, over a blue-tinted background on the cells that were live, under
			// blue header text. It read as a spreadsheet somebody had been
			// highlighting in, and it was the other half of "I don't like how the
			// table system looks".
			//
			// Row striping does the whole of the job a grid was doing — it keeps the
			// eye on one row across the width — and draws nothing between columns,
			// which is what makes a list of names read as a list rather than as
			// cells. The one line kept is the rule under the header.
			constexpr auto kTableFlags = ImGuiMCP::ImGuiTableFlags_RowBg |
			                             ImGuiMCP::ImGuiTableFlags_NoSavedSettings |
			                             ImGuiMCP::ImGuiTableFlags_SizingFixedFit |
			                             ImGuiMCP::ImGuiTableFlags_PadOuterX;

			Im::Spacing();

			// Grey rather than blue. The header names four columns; it is not a
			// heading, and the accent belongs to the group title above it.
			Im::PushStyleColor(kColTableHeaderBg, 0x00000000u);

			// The cell padding this table used to push for itself is gone: Compact
			// sets it for the whole page, and a second opinion here is how the
			// checkbox column came to be measured against one padding and drawn
			// under another.

			if (Im::BeginTable("angles", kShotColumns * 2, kTableFlags)) {
				SetupShotColumns(0);
				SetupShotColumns(1);

				Im::PushStyleColor(kColText, kMuted);
				Im::TableHeadersRow();
				Im::PopStyleColor();

				const auto count = a_shots.size();
				for (std::size_t i = 0; i < count; i += 2) {
					Im::TableNextRow();

					ShotCells(a_shots[i]);

					if (i + 1 < count) {
						ShotCells(a_shots[i + 1]);
					} else {
						// An odd group leaves the right half of the last row empty.
						// The cells are still submitted so the stripe runs the full
						// width rather than stopping halfway.
						EmptyShotCells();
					}
				}

				Im::EndTable();
			}

			Im::PopStyleColor();

			Im::PopID();
		}

		void __stdcall RenderShots()
		{
			const Compact compact;

			const int on = CountOn(kNpcShots) + CountOn(kPlayerShots) + CountOn(kRoomShots);
			const int total = static_cast<int>(
				kNpcShots.size() + kPlayerShots.size() + kRoomShots.size());

			// ONE LINE, WHERE THERE WERE FOUR.
			//
			// A heading, a sentence saying angles are things the camera can choose
			// from, a tally, and a note that changes apply immediately — three of
			// which are either obvious from looking at the page or true of every
			// page in the panel. The tally is the only one that says anything, so
			// it rides on the heading.
			char tally[64]{};
			std::snprintf(tally, sizeof(tally), "%d of %d in play", on, total);
			PageHeader(kIconShots, "SHOTS", tally);

			ShotSection("NPC", "npcCols", kNpcShots,
				"Nothing here is on. The camera has no angle to use while they are "
				"speaking, so it will fall back to a plain shot for every line.");
			ShotSection("PC", "playerCols", kPlayerShots,
				"Nothing here is on. The camera has no angle to use on your turn. "
				"Turn at least one on, or switch off 'Cut To You On Your Turn' under "
				"Camera.");
			ShotSection("Room", "roomCols", kRoomShots,
				"Nothing here is on. That is a perfectly normal way to shoot a "
				"conversation - the camera will simply stay on faces.");
		}

		// ---- Presets ---------------------------------------------------------

		// A cached answer to "which preset am I on, and how far from each one".
		//
		// This page was unusable without it. One drift number is fifty-four profile
		// reads — ten dials plus every shot key — and each of those is a file
		// operation that Mod Organizer's virtual filesystem hooks. Working out the
		// active preset means doing that for all of them, and the list then did it
		// AGAIN per row: comfortably over a thousand file syscalls every frame the
		// page was open, at sixty frames a second.
		//
		// Refreshed on a timer rather than once, because SD.ini is editable from
		// outside the game and a stale "Current:" line would be a lie rather than
		// merely old.
		struct PresetState
		{
			const Camera::Preset*                 active{ nullptr };
			int                                   activeSlot{ -1 };
			std::size_t                           count{ 0 };
			std::chrono::steady_clock::time_point taken{};
			bool                                  valid{ false };

			// Cached with everything else, and for the same reason. Reading a slot
			// is two profile reads, and the rows below asked for all three every
			// frame the page was open.
			std::array<Camera::CustomSlot, Camera::kCustomSlots> slots{};
		};

		PresetState presetState;

		void RefreshPresetState(bool a_force)
		{
			const auto now = std::chrono::steady_clock::now();
			if (!a_force && presetState.valid &&
				now - presetState.taken < std::chrono::milliseconds(500)) {
				return;
			}

			const auto presets = Camera::AllPresets();
			presetState.count = presets.size();
			presetState.active = nullptr;

			// Stops at the first exact match. Two presets cannot both be running,
			// and the ones after it would only be asked a question whose answer
			// cannot change the outcome.
			for (std::size_t i = 0; i < presetState.count; ++i) {
				if (Camera::PresetDrift(presets[i]) == 0) {
					presetState.active = &presets[i];
					break;
				}
			}

			for (int i = 0; i < Camera::kCustomSlots; ++i) {
				presetState.slots[static_cast<std::size_t>(i)] = Camera::ReadCustomSlot(i);
			}

			// Asked after the built-ins, and it OUTRANKS them. A saved slot can
			// hold the same settings as a built-in look — most obviously right
			// after somebody applies one and saves it — and both would then answer
			// "this is what is running".
			presetState.activeSlot = Camera::ActiveCustomSlot();
			if (presetState.activeSlot >= 0) {
				presetState.active = nullptr;
			}

			presetState.taken = now;
			presetState.valid = true;
		}

		// Your own saved looks.
		//
		// Kept visibly apart from the built-ins, and that is a claim about what
		// they are rather than decoration. A built-in look is a designed vocabulary
		// - a shot list chosen to go together, with an effect given to each one for
		// a reason. A slot is a photograph of your settings.
		void RenderCustomSlots()
		{
			// The rename buffers live across frames because InputText edits in
			// place. One per slot, or typing in the second would rewrite the first.
			static std::array<std::array<char, 40>, Camera::kCustomSlots> nameBuf{};
			static bool                                                   primed = false;

			Im::Spacing();

			// Gold marks a slot that holds something, against the green that marks
			// the one you are actually on.
			constexpr unsigned kSlotGold = 0xFF4FC4E8u;

			Im::PushStyleColor(kColText, kSlotGold);
			Im::SeparatorText("MY PRESETS");
			Im::PopStyleColor();

			const int activeSlot = presetState.activeSlot;

			for (int i = 0; i < Camera::kCustomSlots; ++i) {
				const auto  idx = static_cast<std::size_t>(i);
				const auto& slot = presetState.slots[idx];
				const bool  live = slot.used && activeSlot == i;

				if (!primed) {
					std::snprintf(nameBuf[idx].data(), nameBuf[idx].size(), "%s",
						slot.used ? slot.name.c_str() : "");
				}

				Im::PushID(1000 + i);

				char header[160]{};
				if (slot.used) {
					std::snprintf(header, sizeof(header), "%s%s###slot",
						slot.name.c_str(), live ? "  (in use)" : "");
				} else {
					std::snprintf(header, sizeof(header), "Slot %d   -   empty###slot", i + 1);
				}

				Icon(live ? kIconCheck : kIconPresets,
					live ? kGreen : (slot.used ? kSlotGold : kMuted));

				Im::PushStyleColor(kColText, live ? kGreen : (slot.used ? kSlotGold : kMuted));
				const bool open = Im::CollapsingHeader(header);
				Im::PopStyleColor();

				if (open) {
					Im::Indent();

					if (slot.used) {
						Icon(kIconCheck, kBlue);
						if (Im::Button("Use")) {
							Camera::ApplyCustomSlot(i);
							RefreshPresetState(true);
						}
						Im::SameLine();
						Icon(kIconSave, kBlue);
						if (Im::Button("Save Over")) {
							Camera::SaveCustomSlot(i, nameBuf[idx].data());
							RefreshPresetState(true);
						}
						Im::SameLine();
						Icon(kIconDelete, kAmber);
						if (Im::Button("Delete")) {
							Camera::DeleteCustomSlot(i);
							nameBuf[idx][0] = '\0';
							RefreshPresetState(true);
						}

						Icon(kIconRename, kMuted);
						Im::SetNextItemWidth(220.0f);
						if (Im::InputText("Name", nameBuf[idx].data(), nameBuf[idx].size())) {
							// Renames on every keystroke rather than behind a
							// button. A Save button next to a text box is a thing
							// people walk away from without pressing, and the cost
							// of being wrong here is one ini write.
							Camera::RenameCustomSlot(i, nameBuf[idx].data());
						}
					} else {
						Icon(kIconRename, kMuted);
						Im::SetNextItemWidth(220.0f);
						Im::InputTextWithHint("Name", "e.g. My tavern look",
							nameBuf[idx].data(), nameBuf[idx].size());

						const bool named = nameBuf[idx][0] != '\0';
						Im::BeginDisabled(!named);
						Icon(kIconSave, named ? kBlue : kMuted);
						if (Im::Button("Save What I Have Now")) {
							Camera::SaveCustomSlot(i, nameBuf[idx].data());
							RefreshPresetState(true);
						}
						Im::EndDisabled();
					}

					Im::Unindent();
				}

				Im::PopID();
			}

			primed = true;
		}

		void __stdcall RenderPresets()
		{
			const Compact compact;

			RefreshPresetState(false);

			PageHeader(kIconPresets, "PRESETS");

			// The ONE line of prose left on this page, and it earns its place: it
			// is the only warning that ticking a box here overwrites forty settings
			// on another one.
			Note("Ticking one rewrites the Shots page. Nothing is locked afterwards.");
			Im::Spacing();

			const auto presets = Camera::AllPresets();

			// ONE CHECKBOX PER LOOK, and ticking it is what applies it.
			//
			// RADIO BEHAVIOUR, WITHOUT A RADIO. Nothing stores which preset is on;
			// it is worked out by comparing live settings, so applying one makes
			// every other one's comparison fail and they untick themselves. That is
			// also what keeps these honest against MY PRESETS below — a saved slot
			// in use nulls the active preset, so the two lists can never both claim
			// to be what is running.
			for (std::size_t i = 0; i < presetState.count && i < presets.size(); ++i) {
				const auto& preset = presets[i];
				const bool  current = presetState.active == &preset;

				Im::PushID(preset.key);

				Icon(current ? kIconCheck : kIconPresets, current ? kGreen : kMuted);

				Im::PushStyleColor(kColText, current ? kGreen : 0xFFCCCCCCu);
				Im::PushStyleColor(kColCheck, kGreen);

				bool ticked = current;
				if (Im::Checkbox(preset.name, &ticked)) {
					if (ticked) {
						Camera::ApplyPreset(preset);
					}

					// Unticking is not an action, because a preset is applied and
					// then stops existing — there is nothing to go back to. The
					// refresh below puts the tick straight back, since the settings
					// still match the look that wrote them.
					RefreshPresetState(true);
				}

				Im::PopStyleColor(2);

				Im::PushStyleColor(kColText, kMuted);
				Im::TextWrapped("      %s", preset.summary);
				Im::PopStyleColor();

				Im::PopID();
			}

			RenderCustomSlots();

			Im::Spacing();
			Im::Separator();
			Note("No menu? SD.ini, [Presets] sApply=close - applies once, then clears.");
		}

		// ---- About -----------------------------------------------------------

		void __stdcall RenderAbout()
		{
			const Compact compact;

			PageHeader(kIconAbout, "CINEMATIC CONVERSATION CAMERA");
			Note("by hashhbbrown");
			Im::Spacing();

			Im::TextWrapped(
				"Skyrim points one camera at a conversation and leaves it there. "
				"Whoever is speaking, whatever they are saying, however long it goes "
				"on - the shot never changes. You are not in the scene, you are "
				"standing next to it.");
			Im::Spacing();

			Im::TextWrapped(
				"Every dialogue camera mod before this one describes the camera as an "
				"offset from the player. An offset can orbit you, raise you, tighten "
				"on you - but it can never stand behind the person you are talking "
				"to. That single limitation is why no Skyrim mod has ever cut to a "
				"reverse shot.");
			Im::Spacing();

			Im::TextWrapped(
				"This one solves for a position in the world instead, using where "
				"both people actually are. Once the camera is free of you it can do "
				"what a film crew does: shoot over your shoulder, cut to theirs, hold "
				"the two of you while you read your options, creep closer when a "
				"line is delivered with weight. A conversation that is cut, rather "
				"than merely watched.");
			Im::Spacing();

			// The camera-write harness (Diagnostics/iPoseMode), the face probe
			// (Diagnostics/bLogFaceAnim) and the whole of [Lighting] are
			// deliberately not surfaced here. All of them read a default when the
			// key is missing, so the code behind them stays in the tree and stays
			// inert until somebody sets a key by hand.

			Im::Separator();

			if (Camera::Director::Staging()) {
				Icon(kIconCheck, kGreen);
				Im::PushStyleColor(kColText, kGreen);
				Im::TextUnformatted("Staging: active");
				Im::PopStyleColor();
			} else {
				Note("Staging: idle");
			}

			Note("Settings are written to SKSE/Plugins/SD_user.ini and applied immediately. "
				 "Updates replace SD.ini but never that file.");
			Note("Sliders persist when released, not while dragging.");

			Im::SeparatorText("Reset");

			Icon(kIconReset, kBlue);
			if (Im::Button("Restore Defaults")) {
				// EVERY KEY THE CAMERA AND SCREEN PAGES CAN WRITE, PUT BACK TO WHAT
				// THE MOD SHIPS.
				//
				// Taken from a default-constructed Tunables rather than written out
				// as literals, so this cannot drift from the shipped values the way
				// a hand-maintained list would. Tunables' own initialisers are the
				// Close preset's numbers; see the note on the struct.
				const Camera::Tunables defaults{};
				Camera::Director::ApplyTunables(defaults);

				Config::SetInt("Direction", "iCutEveryMin", defaults.cutEveryMin);
				Config::SetInt("Direction", "iCutEveryMax", defaults.cutEveryMax);
				Config::SetBool("Direction", "bPerLineAngleChange", defaults.perLineAngleChange);
				Config::SetBool("Direction", "bHoldOnShortLines", defaults.holdOnShortLines);
				Config::SetInt("Direction", "iShortLineWords", defaults.shortLineWords);
				Config::SetBool("Direction", "bTimedCutsWhileSpeaking",
					defaults.timedCutsWhileSpeaking);
				Config::SetBool("Direction", "bTimedCutsWhileChoosing",
					defaults.timedCutsWhileChoosing);
				Config::SetInt("Direction", "iMinShotTime", defaults.minShotTime);
				Config::SetInt("Direction", "iMinTurnTime", defaults.minTurnTime);
				Config::SetInt("Direction", "iMaxShotTime", defaults.maxShotTime);
				Config::SetInt("Direction", "iPlayerBeat", defaults.playerBeat);
				Config::SetInt("Direction", "iPlayerVoiceHold", defaults.playerVoiceHold);
				Config::SetBool("Direction", "bReactionShots", defaults.reactionShots);
				Config::SetInt("Direction", "iReactionEvery", defaults.reactionEvery);
				Config::SetInt("Direction", "iReactionChance", defaults.reactionChance);
				Config::SetBool("Direction", "bKeepSubjectVisible", defaults.protectSubject);
				Config::SetBool("Direction", "bFirstPersonFallback", defaults.firstPersonFallback);
				Config::SetBool("Direction", "bHoldPlacement", defaults.holdPlacement);
				Config::SetBool("Direction", "bTrue180", defaults.true180);
				Config::SetBool("Direction", "bEnforceLine", defaults.enforceLine);
				Config::SetBool("Direction", "bLetterbox", defaults.letterbox);
				Config::SetInt("Direction", "iLetterboxHeight", defaults.letterboxHeight);
				Config::SetBool("Direction", "bFadeTopicList", defaults.fadeTopicList);
				Config::SetBool("Direction", "bHideSpeakerName", defaults.hideSpeakerName);
				Config::SetBool("Direction", "bFadeAfterPlayerLine", defaults.fadeAfterPlayerLine);
				Config::SetInt("Direction", "iChoiceFadeDelay", defaults.choiceFadeDelay);
				Config::SetInt("Direction", "iChoiceFadeTime", defaults.choiceFadeTime);

				Log::Info(Log::Category::kCore, "Direction dials reset to defaults from the menu."sv);
			}
			Note("Does not touch the Shots page. Use a preset for that.");
		}
	}

	void Settings::Register()
	{
		if (!MF::IsInstalled()) {
			Log::Info(Log::Category::kCore,
				"SKSE Menu Framework not present; SD.ini is the only settings interface."sv);
			return;
		}

		MF::SetSection("Cinematic Conversation Camera");

		// Kept alive for the process. The wrapper unregisters in its destructor,
		// so a temporary here would register and immediately unregister — the key
		// capture would then silently only work through the game's own sink, which
		// is the route least likely to fire while this menu is open.
		static auto* menuInput = MF::AddInputEvent(OnMenuInput);
		static_cast<void>(menuInput);

		// Ordered as a new player meets them: pick a look, tune the camera, pick
		// your angles, tidy the screen, then the pages most people never open.
		//
		// LIGHT IS NOT REGISTERED FOR 1.4. The lamps still exist — see
		// Scene::LightRig and [Lighting] in SD.ini — and they are still off by
		// default, but the page asked the player which of several lighting rigs
		// they would like, and the honest answer to that is "I do not know, I
		// wanted the faces to look better". It is dormant rather than deleted:
		// setting [Lighting] bLights=1 by hand still works, and per-angle lighting
		// still appears in the shot editor when [Lighting] bPerShot=1.
		MF::AddSectionItem("Presets", RenderPresets);
		MF::AddSectionItem("Camera", RenderCamera);
		MF::AddSectionItem("Shots", RenderShots);
		MF::AddSectionItem("Screen", RenderScreen);
		MF::AddSectionItem("Faces", RenderFaces);
		MF::AddSectionItem("Keys", RenderControls);
		MF::AddSectionItem("About", RenderAbout);

		Log::Info(Log::Category::kCore, "SKSE Menu Framework detected; settings panel registered."sv);
	}
}

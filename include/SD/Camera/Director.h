#pragma once

namespace SD::Camera
{
	// The absolute-pose camera, and the cut policy that drives it.
	//
	// Every other dialogue camera in Skyrim expresses itself as an offset from the
	// player — vanilla, Alternate Conversation Camera, MOS's wardrobe framing, OPS.
	// An offset anchored to the player can orbit them but can never stand behind
	// the person they are talking to, which is why no Skyrim mod has ever cut to a
	// reverse shot. This writes a world position and a look direction instead.
	//
	// The conversational rhythm is not speaker-to-speaker. Skyrim's player has no
	// voice, so the NPC is always the only speaker and "cut on speaker change"
	// would never fire. The real alternation is the player's menu turn against the
	// NPC's spoken reply, and both edges are already available: the dialogue menu
	// for one, the response cue for the other.
	// The direction dials, in the units the ini stores them in.
	//
	// Numbers are integers on purpose. Times are hundredths of a second and the
	// letterbox is thousandths of screen height, so the numeric dials, the in-game
	// menu and the ini all speak the same units. One conversion, in ApplyTunables,
	// rather than three.
	//
	// The two bools at the bottom are the only non-numeric members, and they live
	// here rather than with the other bEnabled-style switches for a specific
	// reason: everything in SD.ini outside this struct is read once in Open() and
	// therefore only takes effect on the NEXT conversation. These change cut
	// policy, which is the thing a player tunes by talking to somebody and
	// watching, so they have to be live. Being a tunable is what makes them live.
	struct Tunables
	{
		// THE MASTER SWITCH, AND IT IS LIVE NOW.
		//
		// bEnabled was read ONCE in Runtime::Initialize into a file-static, gated
		// the open/close block from there, and had no control in the menu at all —
		// so the only way to turn this mod off was to alt-tab, edit a text file and
		// restart Skyrim. Exactly the shape of the bHideInterface bug, which had a
		// startup copy and a menu copy that disagreed all evening.
		//
		// It is a Tunables field for the same reason those four became one: this is
		// what ReadTuning fills and what the menu pushes through ApplyTunables, so
		// one path gets both hot-reload and live application. Runtime asks for it
		// rather than keeping its own copy.
		//
		// Turning it off mid-conversation hands the camera straight back —
		// Runtime's own close branch does that, so there is no second mechanism.
		bool enabled{ true };

		int minShotTime{ 240 };
		int minTurnTime{ 25 };
		int maxShotTime{ 900 };

		// EVERY DEFAULT IN THIS STRUCT IS THE CLOSE PRESET'S, AND THAT IS A RULE
		// RATHER THAN A COINCIDENCE.
		//
		// A fresh install has no SD_user.ini, so the shipped SD.ini answers every
		// read — and where a key is missing from that file too, these fallbacks do.
		// The Presets page works out which look is running by comparing live state
		// against each preset, so if any of the three disagreed, a clean 1.4
		// installation would show no preset ticked at all and the player's first
		// impression of the page would be that it is broken.
		//
		// So: these initialisers, config/SD.ini, the reset button on the About page
		// and Camera::kCloseStyle all carry the same numbers. Change one and change
		// all four; the Close preset is the authority.

		// WHAT USED TO BE HERE: dollyAmount, dollyWindow and roomSwing.
		//
		// All three were one number applied to every shot at once — how much of
		// each setup's move happens, how long it takes, and how far the room shots
		// may swing off the open direction. Move amount and duration are per setup
		// now, on the shot's own controls, and the room swing is gone with the
		// randomiser it drove: each room shot carries its own angle off the open
		// direction in the table, and an orbit is something the player asks a
		// specific shot for rather than something rolled behind their back.
		// BLACK BARS, AND THEY ARE A LIVE SETTING NOW.
		//
		// bLetterbox was read once in Runtime::Initialize to decide whether to
		// install the Present hook at all, and was therefore marked "(restart)" in
		// the ini and had no control in the menu — the same shape as bEnabled and
		// bHideInterface before them, and the same outcome: a switch nobody could
		// find and could not have used if they had.
		//
		// The hook is installed unconditionally now and this decides whether the
		// bars are ever asked for. Cheap either way: a retracted letterbox costs
		// one atomic read and an early return per present.
		bool letterbox{ true };

		int letterboxHeight{ 120 };
		int cutEveryMin{ 3 };
		int cutEveryMax{ 6 };
		int playerBeat{ 45 };

		// iListenerGaze AND iSpeakerGaze ARE GONE, with the Eye Contact section
		// they were the whole of. 1.4 defers to the game's own gaze and head
		// tracking; see the note on the retired Faces controls in SD.ini. A value
		// left in an old user ini is simply not read.

		// Hundredths of a second the spent topic list is held before it starts
		// fading, once the turn has passed. Carried here rather than read straight
		// off the ini like its sibling iListReturnDelay, because the menu draws it
		// as a slider: Slider() only writes on release, so a widget re-seeded from
		// the file every frame is handed its pre-drag value and snaps back under
		// the cursor. Sliders read live state; toggles can read the file.
		int choiceFadeDelay{ 150 };

		// Hundredths of a second the spent list takes to fade once it starts.
		int choiceFadeTime{ 200 };

		// How short a line has to be before it stops earning a cut. Counted in
		// words, and only consulted when holdOnShortLines is on.
		int shortLineWords{ 4 };

		// PER LINE ANGLE CHANGE — the first of the two independent cut modes.
		//
		// On, the camera counts eligible lines and takes a new angle once it has
		// seen as many as the cadence rolled, between cutEveryMin and cutEveryMax.
		// Off, lines motivate nothing at all and only the timers below can ask for
		// an angle. Neither on is a legitimate configuration: coverage still moves
		// the camera when the wrong person is on screen, and nothing else does.
		bool perLineAngleChange{ true };

		// IGNORE SHORT LINES. Whether a line too brief to say anything is allowed
		// to count toward the cadence above. Explained where it is enforced, in
		// ApplyCue.
		bool holdOnShortLines{ true };

		// bCutOnLineEnd IS GONE, and it is gone rather than defaulted off.
		//
		// It made the END of a line a third reason to cut, on top of the line
		// starting and the turn passing — three edges inside about two seconds on a
		// short exchange, which reads as restlessness rather than direction. A line
		// finishing is not an event; nothing has happened except that somebody
		// stopped talking. The shot the line was framed on is now held through the
		// pause, always, and the next angle lands on the next line or on a timer.
		//
		// A stale bCutOnLineEnd in an old user ini is simply not read.

		// TIMED ANGLE CHANGE — the second cut mode, and the only one in the mod
		// driven purely by a clock. Off on both halves of the exchange by default.
		bool timedCutsWhileSpeaking{ false };
		bool timedCutsWhileChoosing{ false };

		// WHAT USED TO BE HERE: openOnSpeaker and establishTime.
		//
		// THERE IS NO OPENING SHOT ANY MORE. Not defaulted off — removed, along
		// with the establishing two-shot it timed and the window that protected it.
		//
		// The two-shot was a picture of two people standing apart, held over the
		// top of every conversation. Walk up to somebody and they greet you
		// immediately, so what it actually did was play across the start of a line
		// nobody had finished saying, then cut to them, then cut to you — three
		// angles in about two seconds before the mod did the one thing it exists
		// for. Its only honest use was the silent opening, which is rare, and even
		// there a shot of the two of you says less than a shot of the person about
		// to speak.
		//
		// The rule is now the same one coverage uses everywhere else, from the
		// first frame: the camera is on whoever is talking, and on the player when
		// nobody is. Open() draws from kOpeners or kPlayerOpeners accordingly.

		// THE 180-DEGREE LINE, HELD RATHER THAN DECLARED.
		//
		// Subjects::side has always named itself the 180-degree rule and the shot
		// table has always been authored to respect it — every subject-anchored
		// setup carries a positive angle off the eyeline and takes its sign from
		// `side`. The angle SWEEP then added an offset before that multiply, and
		// the sweep runs to minus eighty, so every setup in the table could be
		// searched onto the far side of the line it was declared to obey. A dirty
		// single authored at 15 degrees crossed on four of its nine bearings.
		//
		// On, the sweep is folded back onto the sanctioned side and the two
		// participants keep their sides of the screen for the whole scene. Off is
		// the old behaviour, kept because it is the only way to get back to a
		// config tuned before the rule existed.
		bool enforceLine{ true };

		// Whether somebody standing in the shot costs it points.
		//
		// The Havok probes ignore actors by design — a ray from a head would hit
		// that head — so until now a guard between the camera and the speaker
		// simply did not exist. Answered geometrically rather than with a raycast,
		// so the cost is a short loop rather than a physics call; off skips it
		// entirely and is the cheaper path in a crowded cell.
		bool avoidCrowds{ true };

		// WHETHER A SHOT KEEPS CHECKING FOR OBSTRUCTIONS AFTER IT HAS CUT.
		//
		// Off — the shipped behaviour — the camera re-solves its distance every
		// frame against a fresh set of raycasts. That is what keeps it out of
		// geometry the conversation walks into, and it is also why a cart crossing
		// behind the lens, or a guard passing in front of it, pulls the camera in
		// and then lets it drift back out. The move is small, correct by its own
		// lights, and nothing the player asked for; on a busy street it never
		// stops.
		//
		// On, the checks run in full at the cut and then stop. The camera is placed
		// clear of walls, out of collision, with a line to its subject — and from
		// that point the shot holds, and things may pass through frame without
		// moving it. What is given up is the correction afterwards: a subject who
		// walks somewhere new takes the camera with them at a fixed bearing and
		// distance, through whatever is in the way.
		//
		// A setting rather than a fix, because which behaviour is right depends on
		// the scene. Held is better on a street or in a market and worse in a
		// conversation that moves. The cut is unaffected either way; see
		// Shot::Subjects::holdPlacement for how that is guaranteed.
		bool holdPlacement{ false };

		// Admit shots by subject visibility and recover without ending dialogue.
		// The explicit legacy hold preference wins conflicting INI values.
		bool protectSubject{ false };
		bool firstPersonFallback{ true };

		// The player's turn belongs to the player.
		//
		// While they are reading topics and while their reply is voiced, the camera
		// goes to them rather than to the room. Off, the room shots share that half
		// of the exchange — which is where every wide-heavy set of shots ended up
		// never cutting to the player at all, because a neutral is never the WRONG
		// subject and so was always an acceptable answer.
		bool coverPlayerTurn{ true };

		// FADE OUT DIALOGUE. Fade the topic list out while somebody is speaking.
		//
		// Hiding the list is safe against the KEYBOARD — ClickGuard closes the
		// control-map Accept path, verified in play. It could not be made safe
		// against the MOUSE: _visible on the holder and selectedIndex,
		// disableSelection, disableInput and enabled on the list were all confirmed
		// applied to a list that defines them, and a click still committed the
		// first topic. That press reaches the movie by a route touching neither
		// MenuControls nor those objects.
		//
		// A player-voice framework is the right owner for this — it knows when the
		// player's line starts and ends and hides the tree from inside the code
		// that owns dialogue input. DBVO 2's hide_dialogue_tree does exactly that.
		//
		// INDEPENDENT OF THE HUD HIDE, and that is a fix rather than a tidy-up.
		// The fade's whole driver used to sit inside `if (hideInterface)`, so
		// bHideInterface=0 switched it off silently — bFadeTopicList=1 read
		// correctly, applied correctly, and was then never consulted, because the
		// block that consults it did not run. Diagnosed 2026-08-19 off a log with
		// two complete conversations in it and not one "Menu phase" line. The
		// switch is gone now and the HUD hide is unconditional, so the gate this
		// note is about cannot return; the independence is kept because the fade
		// having its own reason to run is what made it debuggable.
		bool fadeTopicList{ true };

		// THE SCREEN FURNITURE. Both were read ONCE, in Runtime::Initialize, and
		// could not be changed without restarting the game — which the menu admitted
		// with a caption rather than fixing.
		//
		// They live here now for the same reason every other dial does: Tunables is
		// what ReadTuning fills from the ini and what the menu pushes through
		// ApplyTunables, so one path gets both hot-reload at the next conversation
		// and live application to the one already open. Two owners for one flag is
		// how the HUD hide came to have a startup value and a menu value that
		// disagreed all evening.
		//
		// bHideInterface AND bHideHudWholesale ARE GONE, and they are gone rather
		// than defaulted on. The HUD hide is the mod: a conversation staged behind a
		// compass, three meters and a crosshair is not a shot, and a camera mod that
		// ships an option to not be one is offering the player a way to switch off
		// the thing they installed. Wholesale went with it because it was only ever
		// the price of a hide that could not name what it was hiding — see
		// Scene::Interface, which no longer needs to name anything.
		bool hideSpeakerName{ true };

		// FADE AFTER PC LINE, AND THE INVERTED FLAG IT REPLACES.
		//
		// bHideChoicesWhilePlayerSpeaks stored the opposite of what its label said.
		// The checkbox read "Hold until your voice ends" and the branch behind it
		// was `!hideChoicesWhilePlayerSpeaks && VoicePlaying(player)` — so TICKING
		// the box that promised to hold was what stopped it holding, and the
		// shipped default of 1 meant the hold never ran for anybody. A setting
		// whose name and stored meaning disagree cannot be reasoned about from
		// either end, which is why this is a NEW KEY rather than a relabel:
		// bFadeAfterPlayerLine, true meaning hold.
		//
		// On: the spent list stays up for as long as the player's own voiced line
		// is actually playing, and the configured delay and fade only start once
		// that line ENDS. Off: the fade starts at the click, which is what the
		// engine does on its own.
		//
		// A stale bHideChoicesWhilePlayerSpeaks is not read. Migrating it would
		// mean inverting somebody's value on their behalf and being wrong for
		// anyone who had already worked the old meaning out.
		bool fadeAfterPlayerLine{ true };

		// Whether setups that change the lens across a take are allowed to.
		//
		// A zoom is the one move here with no camera equivalent — nothing physical
		// moves, the image just magnifies — so it reads as a deliberate stylistic
		// choice in a way a dolly does not. Off, those setups play locked off.

		// Diagnostic. How ApplyPose writes the camera transform, 0 being the
		// shipped behaviour. A tunable purely so the in-game menu can change it
		// between conversations without a restart — see ApplyPose in Director.cpp
		// for what each mode does and why these four.
		int poseMode{ 0 };
	};

	// WHO THE CAMERA IS ON, when the player has taken the decision off the mod.
	//
	// Session-scoped and deliberately not persisted: this is a thing you reach for
	// during one conversation that is being filmed badly, not a preference. It
	// resets to kAuto when the conversation ends, so the next one behaves the way
	// the settings say it should.
	enum class Framing : std::uint8_t
	{
		kAuto,  // the director decides, from who is speaking. Shipped behaviour.
		kThem,  // the other party, whoever is talking
		kYou,   // the player, whoever is talking
		kRoom   // neutrals only: the space, and both of you in it
	};

	[[nodiscard]] std::string_view FramingLabel(Framing a_framing) noexcept;

	class Director
	{
	public:
		// SetHideInterface AND SetHideChoicesWhilePlayerSpeaks ARE GONE. Both are
		// Tunables fields now, applied through ApplyTunables like everything else.
		//
		// They were a second way to set a flag Tunables also owns, called once from
		// Runtime::Initialize and never again, and having two owners is precisely
		// how bHideInterface ended up unchangeable: the menu wrote the ini, the
		// Director kept its startup copy, and the control did nothing until the game
		// was restarted.

		// Read the dials as they currently stand, and push a new set live.
		//
		// Apply takes effect on the next frame rather than the next conversation,
		// which is the whole point of having a menu: a cut-floor slider you cannot
		// feel move while a conversation is open is a slider nobody can tune.
		[[nodiscard]] static Tunables GetTunables();
		static void                   ApplyTunables(const Tunables& a_tunables);

		// Load the dials from the ini into the live set.
		//
		// Called at startup as well as per conversation. Without the startup call
		// the live set is default-constructed until the player's first
		// conversation, so opening the menu before then showed built-in defaults
		// rather than what is in the file — and the first slider release wrote
		// those defaults over the player's own values.
		static void LoadSettings();

		// a_restaging: this is the SAME conversation coming back after the director
		// released it while the player was still in it — see Runtime's Stranded().
		// It is not a fresh open and it is not a menu resume; the only thing it
		// changes is that the per-conversation settings re-read is skipped, for the
		// same reason a resume skips it. The dials were read when this conversation
		// opened, the ini has not moved since, and those hundreds of synchronous
		// profile reads are an eighth of a second of the engine's own dialogue
		// camera on a conversation that is already on screen.
		static void Open(RE::Actor* a_speaker, bool a_restaging = false);
		static void Close();

		// Runs every frame from the always-on frame source, whatever the camera is
		// doing.
		//
		// Release used to be decided inside the third-person camera hook, which
		// only fires while that camera state is active. Force-exit a conversation
		// in a way that changes state — first person, a mount, furniture, another
		// menu — and the hook stopped, so the release never ran and the player was
		// left walking around with a staged camera and letterbox bars.
		static void Tick(float a_delta);

		// A response has begun. Carries the emotion, so the policy can decide
		// whether this line has earned a closer shot.
		static void OnCue(RE::Actor* a_speaker, RE::DialogueResponse* a_response);

		// The dialogue menu opened or closed. This is the edge that means the
		// player has left — the session's own end waits for the NPC to stop
		// talking, which is far too late to hand the camera back.
		static void OnDialogueMenu(bool a_opening);

		// SOME OTHER MENU HAS TAKEN THE SCREEN.
		//
		// Called from the menu watch, which is the only part of this mod still
		// running once a pausing menu is up. Tick is not: it rides
		// PlayerCharacter::Update, which stops dead the moment numPausesGame goes
		// above zero, and its own "another menu took the screen; releasing" branch
		// has therefore never fired for the menus it was written for. Letterbox
		// already learned this and retracts from the present hook for the same
		// reason; this is the rest of that lesson.
		//
		// A notification, not a state change. Whether a menu still owns the screen
		// is MenuWatch's to answer and nothing here keeps a second copy of it —
		// two owners for one flag is what made the last three settings bugs in this
		// file look unfixable. a_menu is for the log: it names the menu
		// responsible, so the next report of this arrives already diagnosed.
		static void OnScreenTaken(std::string_view a_menu);

		// THE OTHER EDGE, and it has to be delivered rather than noticed.
		//
		// The camera's resting state is not sampled for half a second after a
		// screen-owning menu lets go, because the frames just after one are the
		// worst possible reading — whatever else manages the camera is still
		// settling. That window used to be timed from the last frame that OBSERVED
		// a menu open, which cannot work for a menu that pauses the game: the frame
		// source stops, so the newest observation is the moment the menu OPENED.
		// Spend four seconds in a container and the window is long expired before
		// the first frame back.
		//
		// MenuWatch runs on the engine's own menu events, which keep arriving while
		// the game is stopped, so it is the only thing that can see this edge. It
		// calls this when the LAST screen-owning menu closes.
		static void OnScreenReleased();

		// Is a conversation SUSPENDED behind a menu rather than finished?
		//
		// The distinction the inventory regression turned on. A conversation whose
		// screen was taken by a barter, container or inventory menu is still there
		// underneath it: the session is live, the partner has not changed, and the
		// cinematic is owed back when the menu closes. A conversation that ENDED —
		// the player walked away, the NPC was killed, the topic hung up — is not,
		// and restoring the frame for it would put bars over an empty street.
		//
		// Runtime asks this to tell the two apart on the frame after the menu
		// closes, and calls AbandonSuspension when the session has gone in the
		// meantime.
		[[nodiscard]] static bool Suspended() noexcept;

		// The conversation the suspension was taken for, or 0.
		[[nodiscard]] static RE::FormID SuspendedFor() noexcept;

		// Give up on resuming: the conversation ended while the menu was up.
		// Logged once, at the transition, and never per frame.
		//
		// a_handBackView PAYS THE DEBT THE SUSPENSION DEFERRED. Suspending for a
		// menu deliberately does not put the camera's resting aim and zoom back,
		// because it is coming straight back and that write lands on the game's
		// PERSISTENT third-person zoom — see Close(). If the conversation turns out
		// to have ended instead, somebody still has to do it, and this is where.
		//
		// False only for a load, where the recorded resting state describes a world
		// that is going away and stamping it into the camera would carry the old
		// save's zoom into the new one.
		static void AbandonSuspension(bool a_handBackView = true);

		// Called from the ThirdPersonState::Update hook after the game's own camera
		// work, so this write lands last and survives the frame.
		static void OnThirdPersonUpdate(RE::ThirdPersonState* a_state);

		[[nodiscard]] static bool Staging() noexcept;

		// THE THREE THINGS A KEY CAN ASK FOR MID-CONVERSATION.
		//
		// All three are REQUESTS, not actions, and that is the whole of their
		// thread safety: they are called from the input thread, set an atomic, and
		// are drained on the next Tick. Nothing here touches the camera, the shot
		// state or Scaleform on the caller's thread — which is the failure mode
		// this project has already paid for once, in ApplyTunables reaching for
		// GFx from the settings panel's own draw.
		//
		// Safe to call at any time. Outside a conversation they are dropped on the
		// floor, so a bound key does nothing in the world rather than something
		// surprising.

		// Cut now: a new angle on the same beat, ignoring the minimum-hold floor
		// and the "has anything happened worth cutting for" test. Those two are
		// what a player is overriding when they reach for this.
		static void RequestCut() noexcept;

		// Cycle who the camera is on: auto, them, you, the room.
		static void RequestFraming() noexcept;

		// WHAT USED TO BE HERE: RequestSuspend, a third hotkey that handed the
		// camera back for the rest of one conversation.
		//
		// Turning the mod off is a CHECKBOX now, not a key. A key is the right
		// control for something you do mid-scene and undo a moment later; enabling
		// and disabling a mod is neither of those, and burning one of only three
		// bindings on it made the other two harder to reach. Tunables::enabled is
		// live, so the checkbox hands the camera back on the spot exactly as the
		// key did — and unlike the key, it is still off next time.

		[[nodiscard]] static Framing CurrentFraming() noexcept;

		// WHAT USED TO BE HERE: ChoicesVisible/ChoicesVisibleFor, published from the
		// director for an input guard to read on the input thread. That guard was
		// removed (see Runtime's note) and these outlived it unread, which is how
		// their 95-alpha threshold went on being maintained against a caller that no
		// longer existed.

		// bFadeAfterPlayerLine is Tunables::fadeAfterPlayerLine. Whether the spent
		// topic list is held up for the length of the player's own voiced reply
		// before the fade starts.
		//
		// The engine says nobody is speaking for the whole of the player's turn
		// under a voice mod — the topic is not committed until the spoken line
		// ends — so the end of that line has to be observed rather than inferred.
		// DBReV announces it; the DBVO and vanilla paths watch the sound handle.

		// Is the mod switched on? Tunables::enabled, mirrored out.
		//
		// SetDirecting is GONE. It was the other half of the bug: Runtime read
		// bEnabled once, pushed it in here, and both then kept their own copy of a
		// flag neither could change. The direction now travels one way — the ini
		// or the menu fills Tunables, ApplyTunables mirrors it here, and Runtime
		// asks. One owner, which is the rule this file already learned once.
		[[nodiscard]] static bool Directing() noexcept;

		// The dialogue menu is open and this mod intends to hide the topic list
		// during it. True from the menu-open event, which lands BEFORE Open() —
		// the window in which a first click used to reach Accept unguarded.
		[[nodiscard]] static bool DialogueMenuUp() noexcept;
	};
}

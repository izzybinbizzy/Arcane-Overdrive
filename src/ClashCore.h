// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The part of Crossfire that knows nothing about the game: the shapes a projectile is tested as, finding which ones
// touched during a frame, what a touch does (the reaction table and the strength contest), spell struggles (who may
// lock, how hard each side pushes, the contest over time and where the meeting point is), and the settings files.
// It includes no game header, so tests/ builds and runs it on its own (see tests/run.sh) - everything here is tested
// there, the game-facing files only feed it.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Crossfire::Core
{
	// ------------------------------------------------------------------ vectors (game units; z is up)
	struct Vec3
	{
		float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };

		friend constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		friend constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		friend constexpr Vec3 operator*(Vec3 a, float s) noexcept { return { a.x * s, a.y * s, a.z * s }; }
		friend constexpr Vec3 operator*(float s, Vec3 a) noexcept { return a * s; }
		friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
	};

	[[nodiscard]] constexpr float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
	[[nodiscard]] float           Length(Vec3 a) noexcept;
	[[nodiscard]] bool            Finite(Vec3 a) noexcept;
	[[nodiscard]] Vec3            Lerp(Vec3 a, Vec3 b, float t) noexcept;

	// the direction a reference faces from its rotation (radians): x is pitch, positive looking down; z is heading,
	// clockwise from north (+y). Heading 0 pitch 0 is (0, 1, 0).
	[[nodiscard]] Vec3 DirectionFromAngles(float a_pitch, float a_heading) noexcept;

	// A path as UTF-8 text. path::string() converts to the ANSI code page on Windows and throws for a name that code page
	// cannot show (a Japanese file name on a Western system); this cannot.
	[[nodiscard]] std::string PathText(const std::filesystem::path& a_path);

	// distance from a point to the segment a-b (a == b is a point)
	[[nodiscard]] float DistancePointSegment(Vec3 p, Vec3 a, Vec3 b) noexcept;

	// ------------------------------------------------------------------ auto lock (ClashAim.cpp): a bolt turned at launch toward
	// its target and in flight toward an oncoming enemy spell - the way of turning a projectile is True Directional Movement's
	// (Ershin, GPL-3.0)
	[[nodiscard]] Vec3 UnitOf(Vec3 a) noexcept;  // a zero vector stays zero

	struct Angles
	{
		float pitch{ 0.0f }, heading{ 0.0f };
	};
	[[nodiscard]] Angles AnglesFromDirection(Vec3 a_dir) noexcept;  // DirectionFromAngles' inverse

	// `a_dir` turned toward `a_want` by at most `a_maxTurn` radians; both unit, so is the result
	[[nodiscard]] Vec3 TurnToward(Vec3 a_dir, Vec3 a_want, float a_maxTurn) noexcept;

	// the launch lock: the way from `a_from` to `a_aim` when that lies within `a_cone` radians of `a_dir` and `a_range` units
	[[nodiscard]] std::optional<Vec3> LockDirection(Vec3 a_from, Vec3 a_dir, Vec3 a_aim, float a_cone, float a_range) noexcept;

	// A flying at B and B at A, nearly head on (each within `a_cone` radians of the line between them) and closer than
	// `a_range`: A's new way, turned toward B by at most `a_maxTurn` radians
	[[nodiscard]] std::optional<Vec3> SteerInto(Vec3 a_posA, Vec3 a_dirA, Vec3 a_posB, Vec3 a_dirB, float a_range, float a_cone,
		float a_maxTurn) noexcept;

	// the rotation that points a model's forward axis (+y) along `a_dir` with no roll, as its three columns
	struct Basis
	{
		Vec3 right, forward, up;
	};
	[[nodiscard]] Basis LookBasis(Vec3 a_dir) noexcept;

	// ------------------------------------------------------------------ what a projectile is, for Crossfire
	enum class Element : std::uint8_t
	{
		kFire,
		kFrost,
		kShock,
		kPoison,
		kArcane,    // any other hostile magic
		kPhysical,  // arrows, bolts, thrown ammo without an elemental enchantment
		kForce,     // a shout's cone with no element of its own (Unrelenting Force and its like)
		kTotal
	};
	inline constexpr std::size_t kElements = static_cast<std::size_t>(Element::kTotal);

	[[nodiscard]] std::string_view ElementName(Element a_element) noexcept;
	[[nodiscard]] bool             ParseElement(std::string_view a_text, Element& a_out) noexcept;  // case-insensitive

	// the shape a projectile is tested as. A sphere moves during the frame (from its position last frame to this one);
	// the others are where they are this frame. A cone is a sphere whose radius grows with the distance it has gone.
	enum class Shape : std::uint8_t
	{
		kSphere,   // missiles, arrows, lobbed grenades, each particle of a spray, a cone
		kBeam,     // a lightning bolt: a capsule from its origin to where it ends
		kBarrier,  // a wall spell: an upright rectangle, its base on the ground
	};

	struct Body
	{
		std::uint32_t id{ 0 };       // the projectile's handle; unique among the bodies of a frame
		std::uint32_t shooter{ 0 };  // who fired it (0: nobody - a trap)
		Shape         shape{ Shape::kSphere };
		Element       element{ Element::kArcane };
		bool          immune{ false };    // beams, walls and cones are never destroyed by a clash
		bool          lockable{ false };  // a stream that may lock in a spell struggle
		bool          bolt{ false };      // a one-shot bolt with BoltsMeet on: two of them are paired though both are immune
		float         radius{ 0.0f };
		float         strength{ 0.0f };
		Vec3          from, to;           // sphere: last frame -> this frame; beam: start -> end; barrier: `from` is the base centre
		Vec3          across;             // barrier only: unit, horizontal, along the wall
		float         halfWidth{ 0.0f };  // barrier only
		float         height{ 0.0f };     // barrier only
	};

	[[nodiscard]] bool Valid(const Body& a_body) noexcept;  // finite, sane sizes; anything else is left out of the frame

	struct Box
	{
		Vec3 lo, hi;
	};
	[[nodiscard]] Box Bounds(const Body& a_body) noexcept;

	// When during the frame (0..1) two bodies first touch, and where. False: they did not.
	struct Touch
	{
		float t{ 0.0f };
		Vec3  point;
	};
	[[nodiscard]] bool FirstTouch(const Body& a, const Body& b, Touch& a_out) noexcept;

	// distance from a point to a barrier's rectangle
	[[nodiscard]] float DistancePointBarrier(Vec3 p, const Body& a_barrier) noexcept;

	struct Contact
	{
		std::size_t a{ 0 }, b{ 0 };  // indices into the bodies passed in; a < b
		Touch       touch;
	};

	// Every pair that touched this frame and may interact, earliest first (ties by index, so a frame is reproducible).
	// `a_mayInteract(i, j)` is asked only for pairs whose boxes overlap; two immune bodies are never paired, unless both
	// are lockable (two held beams meeting can start a struggle, though neither can be destroyed). Bodies that are not
	// Valid() are skipped. `a_out` is cleared first and at most `a_max` contacts are kept (the earliest).
	template <class F>
	void FindContacts(std::span<const Body> a_bodies, F&& a_mayInteract, std::size_t a_max, std::vector<Contact>& a_out);

	// ------------------------------------------------------------------ what a touch does
	enum class Action : std::uint8_t
	{
		kPass,        // nothing
		kClash,       // the strength contest (below)
		kAnnihilate,  // both go, whatever their strength
		kWins,        // the first element destroys the second and goes on untouched
		kLoses,       // the other way round
	};
	[[nodiscard]] std::string_view ActionName(Action a_action) noexcept;
	[[nodiscard]] bool             ParseAction(std::string_view a_text, Action& a_out) noexcept;
	[[nodiscard]] constexpr Action Mirror(Action a) noexcept
	{
		return a == Action::kWins ? Action::kLoses : a == Action::kLoses ? Action::kWins :
		                                                                   a;
	}

	using Table = std::array<std::array<Action, kElements>, kElements>;
	[[nodiscard]] Table DefaultTable() noexcept;
	void                SetReaction(Table& a_table, Element a, Element b, Action a_action) noexcept;  // and the mirror

	struct Outcome
	{
		bool  happened{ false };  // anything changed
		bool  killA{ false }, killB{ false };
		float strengthA{ 0.0f }, strengthB{ 0.0f };  // what each has left (unchanged unless weakened)
	};

	// The clash: when one side is at least `a_ratio` times the other it destroys it and goes on, less the loser's
	// strength when `a_weaken`; otherwise both go. An immune side is never destroyed or weakened; it can still destroy.
	[[nodiscard]] Outcome Resolve(const Table& a_table, float a_ratio, bool a_weaken, Element ea, float sa, bool immuneA,
		Element eb, float sb, bool immuneB) noexcept;

	// ------------------------------------------------------------------ strength
	enum class Kind : std::uint8_t
	{
		kSpell,   // a missile or lobbed spell, staff or scroll
		kStream,  // one particle of a spray (Flames, Frostbite, Sparks is a beam)
		kArrow,
		kVoice,  // a shout's own projectile
		kBeam,
		kBarrier,
		kCone,
	};

	struct Tuning
	{
		float streamShare{ 0.15f };     // a spray particle's share of its spell's cost
		float arrowScale{ 2.0f };       // strength per point of an arrow's damage
		float shoutStrength{ 200.0f };  // every shout projectile, whatever it costs (shouts cost no magicka)
		float minSpellStrength{ 5.0f };
	};

	// a_cost: the spell's base magicka cost; a_power: the projectile's power (dual casting raises it); a_damage: an
	// arrow's damage. Never below 0.01 and always finite.
	[[nodiscard]] float BaseStrength(Kind a_kind, float a_cost, float a_power, float a_damage, const Tuning& a_tuning) noexcept;

	// how big a cone is once it has gone `a_travelled` units
	[[nodiscard]] float ConeRadius(float a_initial, float a_travelled, float a_tangent) noexcept;

	// ------------------------------------------------------------------ the per-frame decisions Clash.cpp makes
	struct Config;

	// a projectile's size for Crossfire: its own collision radius scaled, plus the aim assist, within the limits
	[[nodiscard]] float Radius(float a_own, const Config& a_config) noexcept;

	// along a wall spell facing `a_heading` (radians): horizontal, unit, square to the way it faces
	[[nodiscard]] Vec3 BarrierAcross(float a_heading) noexcept;

	// where a projectile seen for the first time was a frame ago: back along its velocity (`a_velocity`, or
	// `a_fallback` when that is nothing), never further back than it has flown (`a_travelled`, ignored if not finite)
	[[nodiscard]] Vec3 Backtrack(Vec3 a_now, Vec3 a_velocity, Vec3 a_fallback, float a_delta, float a_travelled) noexcept;

	// two handles as one key, the same whichever comes first: a pair of projectiles, or of shooters
	[[nodiscard]] constexpr std::uint64_t PairKey(std::uint32_t a, std::uint32_t b) noexcept
	{
		return (std::uint64_t{ std::min(a, b) } << 32) | std::max(a, b);
	}

	// may two projectiles clash at all? `a_hostile` is only asked when both shooters are actors and allies are ignored
	struct Shooters
	{
		std::uint32_t a{ 0 }, b{ 0 };  // handles; 0 is nobody
		bool          playerA{ false }, playerB{ false };
		bool          actorA{ false }, actorB{ false };
	};
	template <class F>
	[[nodiscard]] bool MayInteract(const Shooters& a_s, int a_who, bool a_ignoreAllies, F&& a_hostile)
	{
		if (a_s.a == a_s.b) {
			return false;  // one shooter's own projectiles, or two with nobody behind them (traps)
		}
		if (a_who == 1 && !a_s.playerA && !a_s.playerB) {
			return false;
		}
		if (a_ignoreAllies && a_s.actorA && a_s.actorB) {
			return a_hostile();
		}
		return true;
	}

	// what a weakened projectile's power (and an arrow's damage) is multiplied by
	[[nodiscard]] float DamageScale(float a_before, float a_after) noexcept;

	// ------------------------------------------------------------------ which element a spell is
	struct MagicFacts
	{
		std::span<const std::string_view> keywords;  // the effect's keywords' editor IDs
		std::string_view                  resist;    // the effect's resist value: "ResistFire", "ResistFrost", ...
		bool                              voice{ false };
		bool                              cone{ false };
	};
	// First the keyword lists, element by element in order, then the resist value, then a voice cone is Force;
	// anything else is Arcane.
	[[nodiscard]] Element Classify(const MagicFacts& a_facts, const std::array<std::vector<std::string>, kElements>& a_keywords) noexcept;

	// ------------------------------------------------------------------ limits on explosions
	// At most `a_perFrame` explosions a frame, and none for the same key (a pair of shooters) within `a_cooldown`
	// seconds of the last, so two sprays meeting do not spawn one explosion per particle.
	class Limiter
	{
	public:
		void                      BeginFrame(double a_now, int a_perFrame, float a_cooldown) noexcept;
		[[nodiscard]] bool        Allow(std::uint64_t a_key);
		[[nodiscard]] std::size_t Remembered() const noexcept { return _last.size(); }

	private:
		std::unordered_map<std::uint64_t, double> _last;
		double                                    _now{ 0.0 };
		double                                    _lastPrune{ 0.0 };
		float                                     _cooldown{ 0.0f };
		int                                       _left{ 0 };
	};

	// ------------------------------------------------------------------ spell struggles
	// Two hostile streams (sprays, held beams, breath) that meet lock together, and the stronger caster pushes the
	// meeting point back until it breaks through. A struggle belongs to a pair of shooters, never to projectiles: a spray
	// is hundreds of particles that live a moment each, and a beam may be launched again. What is here decides; Struggle.cpp
	// reads the actors, feeds this, and carries out what a step says.
	struct StruggleConfig;

	// what kind of stream a projectile is, if it is one that can lock
	enum class Stream : std::uint8_t
	{
		kNone,    // missiles, arrows, walls, cones and one-shot bolts: they clash as they always have
		kSpray,   // a particle of a spell's spray (Flames, Frostbite, a staff's)
		kBeam,    // a held beam (Sparks, Lightning Storm)
		kBreath,  // a particle of a shout's spray (Fire Breath, a dragon's)
	};
	// a_concentration: the spell is held, not fired once; a_voice: it is a shout
	[[nodiscard]] Stream StreamOf(Kind a_kind, bool a_concentration, bool a_voice) noexcept;
	[[nodiscard]] bool   StreamOn(Stream a_stream, const StruggleConfig& a_config) noexcept;  // the switch for its kind

	// one side of a meeting, as far as who may lock goes
	struct LockSide
	{
		Stream stream{ Stream::kNone };
		bool   actor{ false };  // a trap is not
		bool   alive{ false };
		bool   player{ false };
		bool   npc{ false };  // a person (ActorTypeNPC); an actor that is neither this, a dragon nor the player is a creature
		bool   dragon{ false };
		bool   busy{ false };  // already in another struggle
	};
	// May these two lock? The same answer either way round. `a_action` is the reaction table's for the pair: a contest
	// locks, and opposites that would cancel lock when `opposites` is on; Pass, Wins and Loses never do.
	[[nodiscard]] bool MayLock(const LockSide& a, const LockSide& b, Action a_action, const StruggleConfig& a_config) noexcept;

	// Is each caster aiming at the other (within about 50 degrees of the line between their muzzles), and are they at
	// least `a_minGap` apart (MinDistance)? Two streams that cross by chance do not lock.
	[[nodiscard]] bool Facing(Vec3 a_muzzleA, Vec3 a_aimA, Vec3 a_muzzleB, Vec3 a_aimB, float a_minGap) noexcept;
	// a lock whose casters come closer than this is called off: a little inside MinDistance, so a lock that starts at it
	// is not called off by a step
	[[nodiscard]] float KeepGap(const StruggleConfig& a_config) noexcept;

	inline constexpr float kLockIn = 0.4f;     // seconds the lock holds where the streams met before either side pushes
	inline constexpr float kGrace = 0.35f;     // a side not seen this long has stopped (a spray has gaps between particles)
	inline constexpr float kOutlast = 0.2f;    // one that stops while pushed back at least this far ran dry or quit losing
	inline constexpr float kDrawBand = 0.02f;  // at the time limit, closer to even than this is a draw
	inline constexpr float kSurgeAt = 0.6f;    // how far back a side is pushed before it surges
	inline constexpr float kSurgeTime = 3.0f;
	inline constexpr float kBreathPace = 2.0f;    // a breath lasts two or three seconds, so a lock with one moves twice as fast
	inline constexpr float kCooldownTime = 2.0f;  // after a struggle the pair pass through each other this long, then may lock again
	inline constexpr float kSparkEvery = 0.5f;    // a burst at the lock while both push
	inline constexpr float kWobble = 0.03f;       // the meeting point trembles this share of the way, for looks only
	inline constexpr float kHandGap = 60.0f;      // the meeting point never comes closer to either caster's hands
	inline constexpr float kKeepShare = 0.8f;     // of MinDistance: casters closer than this call a lock off
	inline constexpr float kFacingCos = 0.643f;   // cos 50 degrees
	inline constexpr float kMinLead = 0.15f;      // the least lead of the more skilled caster when skill always wins
	inline constexpr float kSkillPoint = 0.02f;   // a point of skill over the other is 2% more push (at weight 1)
	inline constexpr float kLevelPoint = 0.02f;   // and a level, 2%

	// One side's pusher, read from the live actor every frame. A field that is not finite counts as its default here.
	struct Caster
	{
		float skill{ 0.0f };        // magic skill, 0..300 (MagicSkill)
		float level{ 1.0f };        // 1..300
		float magicka{ 1.0f };      // what is left of it, 0..1
		float cost{ 1.0f };         // the spell's base cost, never the projectile's power, so dual casting counts once; a breath: BreathStrength
		bool  dual{ false };        // dual cast, a staff, or both hands streaming into the lock
		bool  school{ true };       // the stream is a spell of a school of magic (a breath is not)
		bool  paysMagicka{ true };  // false for breath and staffs: they neither push less when low nor pay for being pushed back
		float mult{ 1.0f };         // PowerMult
	};

	// a_schools: Alteration, Conjuration, Destruction, Illusion, Restoration. a_own: the stream's own school (0..4; -1
	// none). a_source: 0 the own school (the best when there is none), 1 the best, 2 the mean. 0..300; NaN counts as 0.
	[[nodiscard]] float PickSkill(std::span<const float, 5> a_schools, int a_own, int a_source) noexcept;
	// a breath has no school, so it counts the shouter's level (at most 100); a creature counts at least that, since many
	// have nothing in any school
	[[nodiscard]] float MagicSkill(float a_picked, float a_level, bool a_creature, bool a_breath) noexcept;
	// EnemyPower for the side against the player, DragonPower for a dragon, and a breath's words (a_words 1..3; 0 is
	// not a shout)
	[[nodiscard]] float PowerMult(bool a_opposesPlayer, bool a_dragon, int a_words, const StruggleConfig& a_config) noexcept;
	// How hard a side pushes, as a logarithm, so two compare by their difference: skill, level, the spell's cost, the
	// magicka left, dual casting and the multiplier, each by its weight. Always finite; |result| < 100.
	[[nodiscard]] float LogPower(const Caster& a_caster, const StruggleConfig& a_config) noexcept;
	// How much A outpushes B, -1..1: (Pa - Pb) / (Pa + Pb), so a caster twice as strong leads by 1/3. Exactly the negative
	// of B's over A. With SkillAlwaysWins, when both cast from a school and one has at least a point more skill, that
	// one leads by at least kMinLead whatever else there is: the rest only changes how fast. a_surge*: surging now.
	[[nodiscard]] float Advantage(const Caster& a, const Caster& b, const StruggleConfig& a_config, bool a_surgeA = false,
		bool a_surgeB = false) noexcept;
	// the share of the way from the meeting point to a caster's hands the lock moves in a second at a lead of 1
	[[nodiscard]] float PushRate(const StruggleConfig& a_config) noexcept;
	// seconds from the lock to the breakthrough at this lead and pace (kBreathPace with a breath, else 1); 1e6 for never
	[[nodiscard]] float SecondsToWin(float a_lead, float a_pace, const StruggleConfig& a_config) noexcept;

	enum class Phase : std::uint8_t
	{
		kLocked,    // pushing
		kBroken,    // one broke through; the loser reels for BreakTime and any stream it casts fizzles
		kDeclined,  // the lock chance missed: they pass through each other until one stops
		kCooldown,  // over; they pass through each other, and may lock again after kCooldownTime
	};

	enum class StruggleEvent : std::uint8_t
	{
		kNone,
		kOverwhelmed,  // one broke through (aWon says who)
		kGaveWay,      // one stopped while not losing (aWon: B stopped)
		kDraw,         // the time limit, dead even
		kReleased,     // both stopped
		kCalledOff,    // the game says it cannot go on: an actor gone, dead or too far, the casters too close or out of reach
		kGone,         // over: forget it
	};

	struct Struggle
	{
		std::uint32_t a{ 0 }, b{ 0 };  // the shooters' handles, a < b
		Phase         phase{ Phase::kLocked };
		Action        reaction{ Action::kClash };      // A's element against B's
		bool          breath{ false };                 // either side is a breath: the lock moves at kBreathPace
		bool          dragon{ false };                 // either side is a dragon
		bool          aWon{ false };                   // after an ending: A broke through, or B gave way
		float         balance{ 0.0f };                 // -1 A's hands, 0 where the streams met, +1 B's hands
		float         startShare{ 0.5f };              // where the streams met, as a share of the way from A's muzzle to B's
		float         age{ 0.0f };                     // seconds locked
		float         quietA{ 0.0f }, quietB{ 0.0f };  // seconds each side has not been seen
		float         timer{ 0.0f };                   // seconds in kBroken or kCooldown
		float         lead{ 0.0f };                    // the last step's Advantage
		float         surgeA{ 0.0f }, surgeB{ 0.0f };  // seconds of surge left
		float         nextSpark{ kSparkEvery };
		bool          surgedA{ false }, surgedB{ false };  // each side surges once
		std::uint64_t seed{ 0 };
		Vec3          muzzleA, muzzleB;                // where each stream comes from (Place)
		float         reachA{ 0.0f }, reachB{ 0.0f };  // how far each stream can reach
		bool          placed{ false };
	};

	// what the game saw of the two sides this frame
	struct Sight
	{
		bool valid{ true };                   // false: the struggle is called off
		bool seenA{ false }, seenB{ false };  // each side's stream still comes at the other (kDeclined: streams at all)
	};

	// what the game must do after a step
	struct StruggleStep
	{
		StruggleEvent event{ StruggleEvent::kNone };
		bool          aWon{ false };
		float         lead{ 0.0f };
		float         drainA{ 0.0f }, drainB{ 0.0f };    // magicka each side pays this step for being pushed back
		bool          spark{ false };                    // a burst at the lock
		bool          surgeA{ false }, surgeB{ false };  // a surge began
	};

	// A new struggle for two shooters whose streams met at `a_meet`. The sides are stored in handle order (a < b), with
	// their muzzles, reaches and the reaction turned round to match. The lock chance (the dragon one when either side is
	// a dragon) is rolled once from `a_seed`: a miss gives kDeclined.
	[[nodiscard]] Struggle Begin(std::uint32_t a_shooterA, std::uint32_t a_shooterB, Action a_reaction, Vec3 a_meet, Vec3 a_muzzleA,
		Vec3 a_muzzleB, float a_reachA, float a_reachB, bool a_bothBeams, bool a_breath, bool a_dragon, std::uint64_t a_seed,
		const StruggleConfig& a_config) noexcept;
	// where the streams come from now and how far each reaches; nothing usable (not finite, the muzzles less than a unit
	// apart) keeps the last
	void Place(Struggle& a_struggle, Vec3 a_muzzleA, Vec3 a_muzzleB, float a_reachA, float a_reachB) noexcept;
	// One frame of a struggle, with the two sides as they are now. An overwhelm leaves the loser reeling (kBroken, or
	// kCooldown with no BreakTime); any other ending goes to kCooldown. `a_dt` is capped at a quarter of a second.
	[[nodiscard]] StruggleStep Advance(Struggle& a_struggle, const Caster& a, const Caster& b, const Sight& a_sight, float a_dt,
		const StruggleConfig& a_config) noexcept;
	// the magicka a side pays over `a_dt` for being pushed back `a_towardMe` (0..1) of the way, on top of its spell's
	[[nodiscard]] float MagickaDrain(float a_towardMe, float a_cost, float a_dt, const StruggleConfig& a_config) noexcept;

	// a point measured along the line from one point to another, and how far off that line it is
	struct AxisPoint
	{
		float along{ 0.0f }, off{ 0.0f };
	};
	[[nodiscard]] AxisPoint Project(Vec3 p, Vec3 a_from, Vec3 a_to) noexcept;  // from == to: along 0, off the distance
	// Is a projectile of a stream going from one caster to another still on its way there? A spray fans out, so the
	// tube widens with the distance; `a_length` is the distance between the casters.
	[[nodiscard]] bool InCorridor(AxisPoint a_point, float a_length, float a_radius) noexcept;
	// does a beam from `a_from` along `a_dir` come at `a_target`, within the same widening tube?
	[[nodiscard]] bool BeamAims(Vec3 a_from, Vec3 a_dir, Vec3 a_target, float a_radius) noexcept;
	// where on the way from A's muzzle to B's the streams met, 0.15..0.85 (0.5 when the muzzles are one point)
	[[nodiscard]] float StartShare(Vec3 a_point, Vec3 a_muzzleA, Vec3 a_muzzleB) noexcept;
	// how far a stream can reach: its range or how far it flies in its lifetime, whichever is less; 150..6000
	[[nodiscard]] float StreamReach(float a_range, float a_speed, float a_lifetime) noexcept;
	// can the two streams still meet? A lock whose casters step out of reach of each other is called off
	[[nodiscard]] bool  InReach(float a_distance, float a_reachA, float a_reachB) noexcept;
	[[nodiscard]] float Wobble(float a_age) noexcept;  // -1..1, uneven so it never looks like a pendulum

	// The meeting point, as a distance `at` from A's muzzle along the axis to B's. Balance 0 is where the streams met, +1
	// is B's hands and -1 A's, never closer than kHandGap to either and never beyond what either stream can reach.
	struct Front
	{
		Vec3  from, axis, point;  // A's muzzle, the unit axis towards B's, the meeting point itself
		float length{ 0.0f }, at{ 0.0f };
		bool  valid{ false };
	};
	[[nodiscard]] Front FrontOf(const Struggle& a_struggle) noexcept;
	// Has a projectile of one side's stream got past the meeting point? Measured from that side's own muzzle, inside its
	// tube: A's beyond the front, B's short of it. What is past is snuffed, so both streams end at the front.
	[[nodiscard]] bool PastFront(const Front& a_front, bool a_sideA, Vec3 a_point, float a_radius) noexcept;
	// how long a locked beam from `a_origin` along `a_dir` may be to end at the front: to the plane through the meeting
	// point square to the axis, and a little into it, 32..a_full. A beam more than about 78 degrees off the axis is left whole.
	[[nodiscard]] float BeamCut(const Front& a_front, Vec3 a_origin, Vec3 a_dir, float a_full) noexcept;

	[[nodiscard]] std::uint64_t Mix(std::uint64_t a_x) noexcept;                       // SplitMix64
	[[nodiscard]] bool          Roll(std::uint64_t a_seed, float a_percent) noexcept;  // the same seed, the same answer
	// the breakthrough's extra hit: `a_magnitude` is the winning spell's magnitude times its projectile's power; about two
	// seconds of the stream, more for a clearer win, less the loser's resistance (at most 85%)
	[[nodiscard]] float OverwhelmHit(float a_magnitude, float a_lead, float a_resist, const StruggleConfig& a_config) noexcept;
	// the camera shake for something `a_distance` from the player, 0..1; it fades out by 3000 units
	[[nodiscard]] float ShakeStrength(float a_base, float a_distance, const StruggleConfig& a_config) noexcept;
	[[nodiscard]] float StaggerFor(float a_lead, const StruggleConfig& a_config) noexcept;  // the loser's stagger

	// The struggles going on, by pair of shooters. A shooter is in at most one live struggle (kLocked or kBroken); the
	// caller asks Engaged before it begins another.
	class StruggleBook
	{
	public:
		[[nodiscard]] Struggle*   Find(std::uint32_t a, std::uint32_t b) noexcept;  // either order
		[[nodiscard]] bool        Engaged(std::uint32_t a_shooter) const noexcept;
		[[nodiscard]] bool        Loser(std::uint32_t a_shooter) const noexcept;           // reeling: it lost, and the struggle is kBroken
		[[nodiscard]] bool        Holds(std::uint32_t a, std::uint32_t b) const noexcept;  // these two are locked, or one reels from the other
		Struggle&                 Put(const Struggle& a_struggle);                         // by its pair; replaces one already there
		void                      Erase(std::uint64_t a_key);
		void                      Clear() noexcept { _book.clear(); }
		[[nodiscard]] std::size_t Size() const noexcept { return _book.size(); }
		[[nodiscard]] auto        begin() noexcept { return _book.begin(); }
		[[nodiscard]] auto        end() noexcept { return _book.end(); }
		[[nodiscard]] auto        begin() const noexcept { return _book.begin(); }
		[[nodiscard]] auto        end() const noexcept { return _book.end(); }

	private:
		std::unordered_map<std::uint64_t, Struggle> _book;
	};

	// ------------------------------------------------------------------ the settings files
	// A form named in a file: "Skyrim.esm|0x012EB7" (file and the id within it) or an editor ID.
	struct FormRef
	{
		std::string   file;  // empty: `editorID` names it
		std::uint32_t id{ 0 };
		std::string   editorID;
		friend bool   operator==(const FormRef&, const FormRef&) = default;
	};
	[[nodiscard]] bool ParseFormRef(std::string_view a_text, FormRef& a_out);

	// Spell struggles. Its own struct in Config, so what was there keeps its place.
	struct StruggleConfig
	{
		// [Struggle]
		bool  enabled{ true };
		bool  sprays{ true };
		bool  beams{ true };
		bool  breath{ true };
		bool  opposites{ true };      // a pair the reaction table has cancel each other (Fire and Frost) locks too
		bool  beamsStop{ true };      // a locked beam is cut short at the meeting point
		bool  betweenOthers{ true };  // two casters neither of whom is the player
		bool  creatures{ true };
		float minGap{ 50.0f };   // MinDistance: casters' hands closer than this do not start a lock (vanilla combat closes in)
		float chance{ 100.0f };  // percent, rolled once each time two streams meet
		float dragonChance{ 100.0f };
		float pushTime{ 4.0f };  // seconds for a caster twice as strong to push from where they met to the other's hands
		float maxTime{ 10.0f };  // then whoever is ahead breaks through (0: no limit)
		// [Power]
		bool  skillAlwaysWins{ true };
		int   skillSource{ 0 };  // 0 the stream's own school, 1 the best school, 2 the mean of the five
		float skillWeight{ 1.0f };
		float levelWeight{ 0.5f };
		float spellWeight{ 0.5f };
		float magickaWeight{ 0.5f };
		float dualBonus{ 1.5f };
		float breathStrength{ 25.0f };  // what a breath counts as: a spell of this cost
		float wordBonus{ 0.25f };       // each word of a breath past the first
		float enemyPower{ 1.0f };       // whoever struggles against the player
		float dragonPower{ 1.0f };
		float magickaPressure{ 0.5f };  // times the spell's cost, per second, for the side pushed right back
		bool  surge{ false };
		float surgePower{ 1.5f };
		// [Aftermath]
		bool  breakCast{ true };
		float breakTime{ 1.5f };  // seconds the overwhelmed caster reels
		bool  stagger{ true };
		bool  staggerPlayer{ true };
		float staggerStrength{ 0.6f };
		float overwhelmDamage{ 1.0f };
		bool  finishers{ false };
		float finisherForce{ 4.0f };
		bool  intimidate{ false };
		float xp{ 20.0f };
		bool  messages{ true };
		// [Show]
		bool  lockBursts{ true };
		float cameraShake{ 0.4f };
		bool  bar{ true };
		float barHeight{ 82.0f };  // percent of the way down the screen
		float barScale{ 1.0f };
		float barOpacity{ 0.9f };
		bool  barNames{ false };   // the two names over the bar
		bool  barSkills{ false };  // both magic skills under it
	};

	struct Config
	{
		// [General]
		bool  enabled{ true };
		int   who{ 0 };  // 0 everyone's projectiles; 1 only a clash with one of the player's
		bool  ignoreAllies{ true };
		float maxDistance{ 6000.0f };  // from the player; projectiles further away are left alone
		// [Hits]
		float radiusScale{ 1.0f };
		float radiusBonus{ 12.0f };  // added to every projectile's own collision radius: aiming at a fireball is hard
		float minRadius{ 4.0f };
		float maxRadius{ 256.0f };
		float barrierHeight{ 160.0f };
		// [Clash]
		float  overpowerRatio{ 2.0f };
		bool   weakenSurvivor{ true };
		bool   weakenDamage{ true };  // a weakened projectile also does less when it lands
		bool   boltsMeet{ true };     // two one-shot bolts (Lightning Bolt) that cross burst and both go
		float  boltLinger{ 0.3f };    // seconds a fired bolt's line still clashes after its projectile is gone
		Tuning tuning;
		// [Effects]
		bool  explosions{ true };
		bool  safeExplosionsOnly{ true };  // skip an explosion that would do anything but show (damage, enchantment, spawns)
		bool  standInBursts{ true };       // a spell with no burst of its own (Firebolt) bursts as its element's spells do
		float burstScale{ 1.5f };          // how big a clash's burst is
		int   maxExplosionsPerFrame{ 4 };
		float explosionCooldown{ 0.12f };
		int   maxContactsPerFrame{ 32 };
		// [Player]
		float skillXP{ 6.0f };  // skill experience for each enemy projectile one of the player's destroys
		// [AutoLock]
		bool  lockYours{ true };    // the player's bolts lock onto the foe at launch
		bool  lockOthers{ true };   // every other caster's bolts onto its combat target's
		bool  steer{ true };        // a bolt turns into an oncoming enemy spell that nearly misses it
		bool  lockArrows{ false };  // arrows lock and steer too
		float lockCone{ 25.0f };    // degrees: a bolt aimed further off its target keeps its own aim
		float lockRange{ 4000.0f };
		float steerRange{ 600.0f };
		float steerCone{ 20.0f };   // degrees off the line between the two
		float steerTurn{ 120.0f };  // degrees a second
		// [Events]
		bool modEvents{ true };  // "ArcaneOverdrive_Clash" for Papyrus
		// [Debug]
		bool debugLog{ false };
		// [Struggle] [Power] [Aftermath] [Show]
		StruggleConfig struggle;

		// rules (SpellMasteryHeat Clash Rules.ini and SpellMasteryHeat Clash\*.ini)
		std::array<std::vector<std::string>, kElements> keywords;
		Table                                           reactions{ DefaultTable() };
		std::vector<FormRef>                            exclude;

		Config();
	};

	// Reads an ini's text into `a_config` - only what the text sets; a line it cannot use is skipped and said in
	// `a_warnings` (with its line number). Out-of-range numbers are clamped (and said). Never throws on bad text.
	void ParseIni(std::string_view a_text, Config& a_config, std::vector<std::string>& a_warnings);

	// the settings file the menu writes: [General] .. [Debug], every value, nothing from the rules. Every key is unique
	// across the sections, since RangeOf finds a setting by its key alone.
	[[nodiscard]] std::string WriteSettings(const Config& a_config);

	// settings that can be changed from the menu are clamped to these
	struct Range
	{
		float lo, hi;
	};
	[[nodiscard]] Range RangeOf(std::string_view a_key) noexcept;  // "RadiusBonus", ...; {0,0} for an unknown key
}

// ------------------------------------------------------------------ template definitions
#include <numeric>

namespace Crossfire::Core
{
	template <class F>
	void FindContacts(std::span<const Body> a_bodies, F&& a_mayInteract, std::size_t a_max, std::vector<Contact>& a_out)
	{
		a_out.clear();
		if (a_max == 0 || a_bodies.size() < 2) {
			return;
		}
		// sort and sweep along x: only bodies whose boxes overlap in x are looked at any closer
		thread_local std::vector<std::size_t> order;
		thread_local std::vector<Box>         boxes;
		order.clear();
		boxes.resize(a_bodies.size());
		for (std::size_t i = 0; i < a_bodies.size(); ++i) {
			if (Valid(a_bodies[i])) {
				boxes[i] = Bounds(a_bodies[i]);
				order.push_back(i);
			}
		}
		std::ranges::sort(order, [](std::size_t l, std::size_t r) { return boxes[l].lo.x < boxes[r].lo.x || (boxes[l].lo.x == boxes[r].lo.x && l < r); });
		for (std::size_t oi = 0; oi < order.size(); ++oi) {
			const std::size_t i = order[oi];
			const Box&        bi = boxes[i];
			for (std::size_t oj = oi + 1; oj < order.size(); ++oj) {
				const std::size_t j = order[oj];
				const Box&        bj = boxes[j];
				if (bj.lo.x > bi.hi.x) {
					break;
				}
				if (bj.lo.y > bi.hi.y || bi.lo.y > bj.hi.y || bj.lo.z > bi.hi.z || bi.lo.z > bj.hi.z) {
					continue;
				}
				const Body& x = a_bodies[i];
				const Body& y = a_bodies[j];
				if (x.immune && y.immune && !(x.lockable && y.lockable) && !(x.bolt && y.bolt)) {
					continue;
				}
				const std::size_t lo = std::min(i, j), hi = std::max(i, j);
				if (!a_mayInteract(lo, hi)) {
					continue;
				}
				Touch touch;
				if (FirstTouch(a_bodies[lo], a_bodies[hi], touch)) {
					a_out.push_back({ lo, hi, touch });
				}
			}
		}
		std::ranges::sort(a_out, [](const Contact& l, const Contact& r) {
			if (l.touch.t != r.touch.t) {
				return l.touch.t < r.touch.t;
			}
			return l.a != r.a ? l.a < r.a : l.b < r.b;
		});
		if (a_out.size() > a_max) {
			a_out.resize(a_max);
		}
	}
}

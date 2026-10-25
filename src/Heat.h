// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The two bars as plain numbers, with nothing of the game in it (tests/test_heat.cpp): the Overdrive bar (the charge), the
// Heat bar (the strain) and how Heat lets Overdrive go, the stages, Overdrive, the cooldown and its penalties, the bonus
// a spell gets, how much the hand lights grow, and which spells count.
//
//   THE OVERDRIVE BAR, the charge, 0 to 100
//     a cast        adds 8 x sqrt(cost / 50), between 4 and 24, times Charge speed; a held spell adds the same per second of
//                   its per-second cost, times Held spell gain (60%: they hold for a long time)
//     same school   as the last cast: the gain is multiplied by 1 + Same school bonus (+50%)
//     another school  the charge drops by School switch cooling (25) before the gain is added
//     after a cast  Grace period (3 s), then it drains by Drain speed (5 a second)
//     full          Overdrive is ready; it goes when the Heat gate lets it (at once with the gate off, or on the key)
//   THE HEAT BAR, the strain, 0 to 100 - it shows how FAST you cast
//     a cast        adds that cast gain x Heat per cast (1.5) x Heat climbs; the same element as the last x 1.25, a dual
//                   cast x 1.5; a held spell adds Heat held (3) x its gain a second
//     always        cools toward 0 by Cooling (a quarter of what it holds a second) x Heat cools
//     stages        Warm from 25, Hot from 50, Overheating from 75: the bonus and the hand lights; the red zone from the
//                   red line (85)
//   THE GATE (Heat gate on)
//     sweet spot    the default: Overdrive goes only with Heat in the band, from its bottom (50) up to the red line; too
//                   cold, nothing; too hot, a misfire - Heat sheds 25 (and with Backlash the game side staggers you and
//                   bites your magicka). On automatic it simply waits for the band
//     vent          Overdrive goes whenever Heat is below the red line; in the red a press waits until it has cooled
//     focus         Heat at the band's bottom or hotter, and a focus held for Focus time (1 s)
//   OVERDRIVE       Burst length (6 s) at the Overdrive bonus, the Heat spent with it (down to 0 by its end); then the
//                   Cooldown (10 s): both bars locked at 0, with slow magicka (on, to 40%) and weaker spells (off, 15%)

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Heat
{
	enum class School : std::uint8_t
	{
		kAlteration,
		kConjuration,
		kDestruction,
		kIllusion,
		kRestoration,
		kNone  // a spell with no school (none of the five skills): it builds heat, and counts as its own school
	};
	inline constexpr std::size_t kSchools = 6;

	[[nodiscard]] constexpr std::string_view SchoolName(School a_school) noexcept
	{
		constexpr std::string_view kNames[]{ "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "No school" };
		return kNames[std::min<std::size_t>(static_cast<std::size_t>(a_school), kSchools - 1)];
	}

	enum class Stage : std::uint8_t
	{
		kCold,
		kWarm,
		kHot,
		kOverheating,
		kOverdrive
	};

	[[nodiscard]] constexpr std::string_view StageName(Stage a_stage) noexcept
	{
		constexpr std::string_view kNames[]{ "Cold", "Warm", "Hot", "Overheating", "Overdrive" };
		return kNames[std::min<std::size_t>(static_cast<std::size_t>(a_stage), 4)];
	}

	enum class OverdriveMode : int
	{
		kAutomatic = 0,  // it goes by itself when the charge is full and the gate lets it
		kKey = 1,        // it waits, full, for the key
		kOff = 2
	};

	enum class Gate : int
	{
		kSweetSpot = 0,
		kVent = 1,
		kFocus = 2
	};

	// what the gate says now, for the bars
	enum class GateNow : std::uint8_t
	{
		kOpen,
		kCold,  // Heat below the band
		kHot,   // Heat past the red line
		kFocus  // the focus gate: hold the focus
	};

	// what a press of the Overdrive key did
	enum class Release : std::uint8_t
	{
		kStarted,
		kNotReady,  // not on the key, the charge not full, or Overdrive / the cooldown running
		kCold,
		kHot,      // too hot, with misfires off: nothing
		kWaiting,  // the vent gate: it goes once Heat is below the red line
		kMisfire,
		kFocus  // the focus gate: holding is what counts
	};

	enum class Boost : int
	{
		kDamage = 0,  // only effects that damage
		kAll = 1      // every hostile effect's magnitude
	};

	struct Tuning
	{
		// the Overdrive bar
		float gain{ 1.0f };         // Charge speed
		float heldGain{ 0.6f };     // a held spell's gain, against a cast's
		float drain{ 5.0f };        // charge a second, after the grace period
		float grace{ 3.0f };        // seconds
		float sameSchool{ 0.5f };   // extra gain for the same school as the last cast
		float switchLoss{ 25.0f };  // charge lost on casting another school
		bool  perSchool{ false };   // one charge per school, else one shared charge
		// the Heat bar
		float                heatPerCast{ 1.5f };        // x the cast gain
		float                heatHeld{ 3.0f };           // x a held spell's gain, a second
		float                heatSameElement{ 0.25f };   // extra heat for the same element as the last spell
		float                heatDual{ 0.5f };           // extra heat for a dual cast
		float                cool{ 0.25f };              // the part of its heat it loses a second
		float                heatUp{ 1.0f };             // Heat climbs
		float                heatDown{ 1.0f };           // Heat cools
		std::array<float, 3> at{ 25.0f, 50.0f, 75.0f };  // where Warm, Hot and Overheating start
		float                redLine{ 85.0f };           // the red zone from here; the sweet spot's top
		// the gate
		bool  heatGate{ true };
		Gate  gate{ Gate::kSweetSpot };
		float bandLow{ 50.0f };  // the sweet spot's bottom, and the focus gate's least heat
		bool  misfire{ true };   // the sweet spot: a release past the red line misfires
		float misfireDrop{ 25.0f };
		bool  backlash{ false };         // ... and staggers you and takes a bite of your magicka (the game side)
		float backlashMagicka{ 0.15f };  // of your magicka pool
		float focusSeconds{ 1.0f };
		// the bonus
		bool          bonusOn{ true };
		float         bonusPerStage{ 0.04f };  // Warm +4%, Hot +8%, Overheating +12%
		float         overdriveBonus{ 0.25f };
		OverdriveMode mode{ OverdriveMode::kAutomatic };
		float         burst{ 6.0f };      // seconds of Overdrive
		float         cooldown{ 10.0f };  // seconds locked at 0 after it
		bool          slowMagicka{ true };
		float         slowMagickaTo{ 0.4f };  // magicka regenerates at this fraction during the cooldown
		bool          weaker{ false };
		float         weakerBy{ 0.15f };  // spells this much weaker during the cooldown

		bool operator==(const Tuning&) const = default;
	};

	inline constexpr float kFull = 100.0f;

	// what a cast of this cost adds before the multipliers
	[[nodiscard]] inline float CastGain(float a_cost) noexcept
	{
		const float cost = std::isfinite(a_cost) ? std::max(a_cost, 0.0f) : 0.0f;
		return std::clamp(8.0f * std::sqrt(cost / 50.0f), 4.0f, 24.0f);
	}

	[[nodiscard]] inline Stage StageAt(const Tuning& a_t, float a_heat) noexcept
	{
		// the thresholds in order, each below full (the menu cannot set them otherwise; a hand-edited file could)
		const float warm = std::clamp(a_t.at[0], 1.0f, kFull - 3.0f);
		const float hot = std::clamp(a_t.at[1], warm + 1.0f, kFull - 2.0f);
		const float overheating = std::clamp(a_t.at[2], hot + 1.0f, kFull - 1.0f);
		return a_heat >= overheating ? Stage::kOverheating : a_heat >= hot ? Stage::kHot :
		                                                 a_heat >= warm    ? Stage::kWarm :
		                                                                     Stage::kCold;
	}

	// the band's bottom and the red line, in order and inside the bar
	struct Band
	{
		float low{ 50.0f }, red{ 85.0f };
	};
	[[nodiscard]] inline Band BandOf(const Tuning& a_t) noexcept
	{
		const float low = std::isfinite(a_t.bandLow) ? std::clamp(a_t.bandLow, 0.0f, kFull - 5.0f) : 50.0f;
		return { low, std::isfinite(a_t.redLine) ? std::clamp(a_t.redLine, low + 5.0f, kFull) : std::max(85.0f, low + 5.0f) };
	}

	// the spell's magnitude multiplier at a stage (Overdrive: its own bonus)
	[[nodiscard]] inline float StageBonus(const Tuning& a_t, Stage a_stage) noexcept
	{
		if (!a_t.bonusOn) {
			return 1.0f;
		}
		if (a_stage == Stage::kOverdrive) {
			return 1.0f + std::max(a_t.overdriveBonus, 0.0f);
		}
		return 1.0f + std::max(a_t.bonusPerStage, 0.0f) * static_cast<float>(static_cast<int>(a_stage));
	}

	// the hand lights' multiplier at a stage: Light growth per stage, Overdrive counting as a fourth stage
	[[nodiscard]] inline float LightFactor(float a_perStage, Stage a_stage) noexcept
	{
		const float k = std::isfinite(a_perStage) ? std::clamp(a_perStage, 0.0f, 2.0f) : 0.0f;
		return 1.0f + k * static_cast<float>(static_cast<int>(a_stage));
	}

	// ------------------------------------------------------------------ which spells build heat
	struct SpellFacts
	{
		enum class Kind : std::uint8_t
		{
			kSpell,
			kStaff,
			kScroll,
			kPower,  // powers, lesser powers and shouts
			kOther   // potions, poisons, enchantments on weapons, abilities, diseases
		};
		Kind kind{ Kind::kSpell };
		bool healingOrWard{ false };
	};

	struct Counts
	{
		bool healingAndWards{ false };
		bool stavesAndScrolls{ false };
	};

	[[nodiscard]] inline bool BuildsHeat(const SpellFacts& a_spell, const Counts& a_counts) noexcept
	{
		switch (a_spell.kind) {
		case SpellFacts::Kind::kSpell:
			break;
		case SpellFacts::Kind::kStaff:
		case SpellFacts::Kind::kScroll:
			if (!a_counts.stavesAndScrolls) {
				return false;
			}
			break;
		default:
			return false;
		}
		return !a_spell.healingOrWard || a_counts.healingAndWards;
	}

	// ------------------------------------------------------------------ the two bars
	class Meter
	{
	public:
		enum class State : std::uint8_t
		{
			kBuilding,
			kOverdrive,
			kCooldown
		};

		// a fire-and-forget cast of a_school costing a_cost; a_element 0 none, 1 fire, 2 frost, 3 shock (Spells.cpp)
		void Cast(const Tuning& a_t, School a_school, float a_cost, int a_element = 0, bool a_dual = false) noexcept
		{
			if (_state != State::kBuilding) {
				return;
			}
			const float gain = CastGain(a_cost);
			AddHeat(a_t, gain * Clamp(a_t.heatPerCast, 0.0f, 10.0f) * (a_dual ? 1.0f + Clamp(a_t.heatDual, 0.0f, 3.0f) : 1.0f), a_element);
			Add(a_t, a_school, gain * Speed(a_t));
			StartIfLet(a_t);
		}

		// a held spell, this frame: a_costPerSecond for a_delta seconds
		void Hold(const Tuning& a_t, School a_school, float a_costPerSecond, float a_delta, int a_element = 0) noexcept
		{
			const float dt = Delta(a_delta);
			if (dt <= 0.0f || _state != State::kBuilding) {
				return;
			}
			const float gain = CastGain(a_costPerSecond) * dt;
			AddHeat(a_t, gain * Clamp(a_t.heatHeld, 0.0f, 20.0f), a_element);
			Add(a_t, a_school, gain * Speed(a_t) * Clamp(a_t.heldGain, 0.0f, 3.0f));
			StartIfLet(a_t);
		}

		// the focus gate, every frame: a_focusing = the key held, or both hands holding a charged spell
		void Focus(const Tuning& a_t, bool a_focusing, float a_delta) noexcept
		{
			const float dt = Delta(a_delta);
			if (!FocusGate(a_t) || a_t.mode == OverdriveMode::kOff || _state != State::kBuilding || !Full() || _heat < BandOf(a_t).low || !a_focusing) {
				_focus = 0.0f;
				return;
			}
			_focus += dt;
			if (_focus >= std::max(a_t.focusSeconds, 0.0f)) {
				Start(a_t);
			}
		}

		// time passing: the cooling and the drain, Overdrive's and the cooldown's clocks, and Overdrive starting by itself
		void Tick(const Tuning& a_t, float a_delta) noexcept
		{
			const float dt = Delta(a_delta);
			switch (_state) {
			case State::kOverdrive:
				_timer -= dt;
				_heat = _heatAtStart * std::clamp(_timer / std::max(_burst, 0.1f), 0.0f, 1.0f);  // spent as power
				if (_timer <= 0.0f) {
					_state = State::kCooldown;
					_timer = std::max(a_t.cooldown, 0.0f);
					_charge.fill(0.0f);
					_heat = 0.0f;
					if (_timer <= 0.0f) {
						_state = State::kBuilding;
					}
				}
				return;
			case State::kCooldown:
				_charge.fill(0.0f);
				_heat = 0.0f;
				_timer -= dt;
				if (_timer <= 0.0f) {
					_state = State::kBuilding;
					_timer = 0.0f;
				}
				return;
			default:
				break;
			}
			_heat *= std::exp(-Clamp(a_t.cool, 0.0f, 5.0f) * Clamp(a_t.heatDown, 0.1f, 5.0f) * dt);
			if (_heat < 0.01f) {
				_heat = 0.0f;
			}
			_sinceCast += dt;
			if (_sinceCast > std::max(a_t.grace, 0.0f)) {
				const float drop = std::max(a_t.drain, 0.0f) * std::min(dt, _sinceCast - std::max(a_t.grace, 0.0f));
				for (auto& c : _charge) {
					c = std::max(c - drop, 0.0f);
				}
			}
			StartIfLet(a_t);
		}

		// the Overdrive key
		Release PressKey(const Tuning& a_t) noexcept
		{
			if (a_t.mode != OverdriveMode::kKey || _state != State::kBuilding || !Full()) {
				return Release::kNotReady;
			}
			switch (Gated(a_t)) {
			case GateNow::kOpen:
				Start(a_t);
				return Release::kStarted;
			case GateNow::kCold:
				return Release::kCold;
			case GateNow::kFocus:
				return Release::kFocus;
			default:
				break;
			}
			if (a_t.gate == Gate::kVent) {
				_waiting = true;
				return Release::kWaiting;
			}
			if (!a_t.misfire) {
				return Release::kHot;
			}
			_heat = std::max(_heat - std::max(a_t.misfireDrop, 0.0f), 0.0f);
			++_misfires;
			return Release::kMisfire;
		}

		// ---- reading it
		[[nodiscard]] State  Now() const noexcept { return _state; }
		[[nodiscard]] float  Timer() const noexcept { return _timer; }
		[[nodiscard]] School Last() const noexcept { return _last; }
		[[nodiscard]] float  HeatNow() const noexcept { return _heat; }
		[[nodiscard]] float  FocusHeld() const noexcept { return _focus; }
		[[nodiscard]] int    Misfires() const noexcept { return _misfires; }
		[[nodiscard]] bool   Waiting() const noexcept { return _waiting; }
		// the charge a spell of a_school builds (the shared bar, or its school's)
		[[nodiscard]] float ChargeOf(const Tuning& a_t, School a_school) const noexcept { return _charge[Index(a_t, a_school)]; }
		// the charge the bar shows: the shared bar, or the last school's
		[[nodiscard]] float Charge(const Tuning& a_t) const noexcept { return _charge[Index(a_t, _last)]; }
		// full and waiting to go (the gate decides when)
		[[nodiscard]] bool    Ready(const Tuning& a_t) const noexcept { return a_t.mode != OverdriveMode::kOff && _state == State::kBuilding && Full(); }
		[[nodiscard]] bool    RedZone(const Tuning& a_t) const noexcept { return _state == State::kBuilding && _heat >= BandOf(a_t).red; }
		[[nodiscard]] GateNow Gated(const Tuning& a_t) const noexcept
		{
			if (!a_t.heatGate) {
				return GateNow::kOpen;
			}
			const auto band = BandOf(a_t);
			if (a_t.gate == Gate::kVent) {
				return _heat >= band.red ? GateNow::kHot : GateNow::kOpen;
			}
			if (_heat < band.low) {
				return GateNow::kCold;
			}
			if (a_t.gate == Gate::kFocus) {
				return _focus >= std::max(a_t.focusSeconds, 0.0f) ? GateNow::kOpen : GateNow::kFocus;
			}
			return _heat >= band.red ? GateNow::kHot : GateNow::kOpen;
		}
		// the stage now: the Heat's, Overdrive while it lasts, Cold in the cooldown
		[[nodiscard]] Stage StageNow(const Tuning& a_t) const noexcept
		{
			return _state == State::kOverdrive ? Stage::kOverdrive : _state == State::kCooldown ? Stage::kCold :
			                                                                                      StageAt(a_t, _heat);
		}

		// a spell's magnitude multiplier now
		[[nodiscard]] float Bonus(const Tuning& a_t) const noexcept
		{
			if (_state == State::kCooldown) {
				return a_t.weaker ? 1.0f - std::clamp(a_t.weakerBy, 0.0f, 0.9f) : 1.0f;
			}
			return StageBonus(a_t, StageNow(a_t));
		}

		// how fast magicka regenerates now, as a fraction of normal
		[[nodiscard]] float MagickaRegen(const Tuning& a_t) const noexcept
		{
			return _state == State::kCooldown && a_t.slowMagicka ? std::clamp(a_t.slowMagickaTo, 0.0f, 1.0f) : 1.0f;
		}

		// nothing going on: no charge, no heat, no Overdrive, no cooldown
		[[nodiscard]] bool Idle() const noexcept
		{
			return _state == State::kBuilding && _heat <= 0.0f && std::ranges::all_of(_charge, [](float c) { return c <= 0.0f; });
		}

		// ---- the bench: set the charge or the heat, force a stage or Overdrive
		void SetCharge(const Tuning& a_t, float a_charge) noexcept
		{
			Build();
			_charge[Index(a_t, _last)] = Fraction(a_charge);
			_sinceCast = 0.0f;
		}
		void SetHeat(float a_heat) noexcept
		{
			Build();
			_heat = Fraction(a_heat);
		}
		void ForceStage(const Tuning& a_t, Stage a_stage) noexcept
		{
			if (a_stage == Stage::kOverdrive) {
				SetCharge(a_t, kFull);
				Start(a_t);
				return;
			}
			SetHeat(a_stage == Stage::kCold ? 0.0f : StageThreshold(a_t, a_stage));
		}
		void Reset() noexcept { *this = Meter{}; }

	private:
		[[nodiscard]] static float       Clamp(float a_v, float a_lo, float a_hi) noexcept { return std::isfinite(a_v) ? std::clamp(a_v, a_lo, a_hi) : a_lo; }
		[[nodiscard]] static float       Delta(float a_delta) noexcept { return std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, 0.25f) : 0.0f; }
		[[nodiscard]] static float       Fraction(float a_v) noexcept { return std::isfinite(a_v) ? std::clamp(a_v, 0.0f, kFull) : 0.0f; }
		[[nodiscard]] static float       Speed(const Tuning& a_t) noexcept { return std::isfinite(a_t.gain) ? std::clamp(a_t.gain, 0.0f, 5.0f) : 1.0f; }
		[[nodiscard]] static bool        FocusGate(const Tuning& a_t) noexcept { return a_t.heatGate && a_t.gate == Gate::kFocus; }
		[[nodiscard]] static std::size_t Index(const Tuning& a_t, School a_school) noexcept
		{
			return a_t.perSchool ? std::min<std::size_t>(static_cast<std::size_t>(a_school), kSchools - 1) : 0;
		}
		[[nodiscard]] static float StageThreshold(const Tuning& a_t, Stage a_stage) noexcept
		{
			for (float h = 0.0f; h <= kFull; h += 0.5f) {
				if (StageAt(a_t, h) == a_stage) {
					return h;
				}
			}
			return kFull;
		}
		[[nodiscard]] bool Full() const noexcept { return *std::ranges::max_element(_charge) >= kFull; }

		void Build() noexcept
		{
			if (_state != State::kBuilding) {
				_state = State::kBuilding;
				_timer = 0.0f;
			}
		}

		// Overdrive by itself: automatic, or a vent-gate press waiting for the heat to fall below the red line
		void StartIfLet(const Tuning& a_t) noexcept
		{
			if (_state != State::kBuilding || !Full()) {
				_waiting = false;
				return;
			}
			const bool wants = a_t.mode == OverdriveMode::kAutomatic || (_waiting && a_t.mode == OverdriveMode::kKey);
			if (wants && !FocusGate(a_t) && Gated(a_t) == GateNow::kOpen) {
				Start(a_t);
			}
		}

		void Start(const Tuning& a_t) noexcept
		{
			_state = State::kOverdrive;
			_burst = std::max(a_t.burst, 0.1f);
			_timer = _burst;
			_heatAtStart = _heat;
			_focus = 0.0f;
			_waiting = false;
			_charge.fill(0.0f);
			_charge[0] = kFull;  // shown full while it lasts (the shared bar, or the last school's)
			_charge[Index(a_t, _last)] = kFull;
		}

		void AddHeat(const Tuning& a_t, float a_heat, int a_element) noexcept
		{
			const bool  same = a_element != 0 && a_element == _element;
			const float add = a_heat * Clamp(a_t.heatUp, 0.0f, 5.0f) * (same ? 1.0f + Clamp(a_t.heatSameElement, 0.0f, 3.0f) : 1.0f);
			if (std::isfinite(add) && add > 0.0f) {
				_heat = std::min(_heat + add, kFull);
			}
			if (a_element != 0) {
				_element = a_element;
			}
		}

		void Add(const Tuning& a_t, School a_school, float a_gain) noexcept
		{
			if (!std::isfinite(a_gain) || a_gain <= 0.0f) {
				return;
			}
			auto& c = _charge[Index(a_t, a_school)];
			if (_started && a_school != _last) {
				// another school: the bar it was building loses charge, once, before this cast adds any
				auto& before = _charge[Index(a_t, _last)];
				before = std::max(before - std::max(a_t.switchLoss, 0.0f), 0.0f);
			}
			const bool same = _started && a_school == _last;
			c = std::min(c + a_gain * (same ? 1.0f + std::max(a_t.sameSchool, 0.0f) : 1.0f), kFull);
			_last = a_school;
			_started = true;
			_sinceCast = 0.0f;
		}

		std::array<float, kSchools> _charge{};
		float                       _heat{ 0.0f };
		int                         _element{ 0 };
		School                      _last{ School::kNone };
		bool                        _started{ false };
		float                       _sinceCast{ 0.0f };
		State                       _state{ State::kBuilding };
		float                       _timer{ 0.0f };
		float                       _burst{ 6.0f };
		float                       _heatAtStart{ 0.0f };
		float                       _focus{ 0.0f };
		bool                        _waiting{ false };
		int                         _misfires{ 0 };
	};

	// ------------------------------------------------------------------ slow magicka, without changing anything that lasts
	// During the cooldown, part of each frame's natural regeneration is taken back: a rise no bigger than regeneration
	// could give in that time (a_cap) is regeneration; a bigger one (a potion, a spell) is left alone. Returns how much to
	// take back. Nothing on the actor is changed for longer than a frame, so a crash or a load leaves nothing behind.
	[[nodiscard]] inline float TakeBack(float a_before, float a_now, float a_cap, float a_regen) noexcept
	{
		if (!std::isfinite(a_before) || !std::isfinite(a_now) || !std::isfinite(a_cap) || !std::isfinite(a_regen)) {
			return 0.0f;
		}
		const float rise = a_now - a_before;
		if (rise <= 0.0f || rise > a_cap || a_regen >= 1.0f) {
			return 0.0f;
		}
		return rise * (1.0f - std::clamp(a_regen, 0.0f, 1.0f));
	}
}

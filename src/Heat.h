// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The whole meter as plain numbers, with nothing of the game in it (tests/test_heat.cpp): what a cast adds, the stages,
// the drain after the grace period, the loss on a change of school, Overdrive (automatic, on a key, or off), the
// cooldown and its two penalties, the bonus a spell gets, how much the hand lights grow, and which spells count.
//
//   heat 0 to 100     Warm from 25, Hot from 50, Overheating from 75, Overdrive at 100 (thresholds adjustable)
//   a cast            adds 8 x sqrt(cost / 50), between 4 and 24, times Heat gain speed; a held spell adds the same
//                     per second of its per-second cost, times Held spell gain (60%: they hold for a long time)
//   same school       as the last cast: the gain is multiplied by 1 + Same school bonus (+50%)
//   another school    the heat drops by School switch cooling (25 points: one stage) before the gain is added
//   after a cast      Grace period (2 s), then the heat drains by Drain speed (10 a second)
//   Overdrive         Burst length (6 s) at the Overdrive bonus; then the Cooldown (10 s): the meter is locked at 0,
//                     with slow magicka (on, to 40%) and weaker spells (off, 15% weaker) as the two optional penalties

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
		kAutomatic = 0,  // it starts by itself at 100
		kKey = 1,        // it waits at Overheating until the key is pressed
		kOff = 2
	};

	enum class Boost : int
	{
		kDamage = 0,  // only effects that damage
		kAll = 1      // every hostile effect's magnitude
	};

	struct Tuning
	{
		float                gain{ 1.0f };               // Heat gain speed
		float                heldGain{ 0.6f };           // a held spell's gain, against a cast's
		float                drain{ 10.0f };             // heat a second, after the grace period
		float                grace{ 2.0f };              // seconds
		float                sameSchool{ 0.5f };         // extra gain for the same school as the last cast
		float                switchLoss{ 25.0f };        // heat lost on casting another school
		bool                 perSchool{ false };         // one meter per school, else one shared meter
		std::array<float, 3> at{ 25.0f, 50.0f, 75.0f };  // where Warm, Hot and Overheating start
		float                bonusPerStage{ 0.04f };     // Warm +4%, Hot +8%, Overheating +12%
		float                overdriveBonus{ 0.25f };
		OverdriveMode        mode{ OverdriveMode::kAutomatic };
		float                burst{ 6.0f };      // seconds of Overdrive
		float                cooldown{ 10.0f };  // seconds locked at 0 after it
		bool                 slowMagicka{ true };
		float                slowMagickaTo{ 0.4f };  // magicka regenerates at this fraction during the cooldown
		bool                 weaker{ false };
		float                weakerBy{ 0.15f };  // spells this much weaker during the cooldown

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

	// the spell's magnitude multiplier at a stage (Overdrive: its own bonus)
	[[nodiscard]] inline float StageBonus(const Tuning& a_t, Stage a_stage) noexcept
	{
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

	// ------------------------------------------------------------------ the meter
	class Meter
	{
	public:
		enum class State : std::uint8_t
		{
			kBuilding,
			kOverdrive,
			kCooldown
		};

		// a fire-and-forget cast of a_school costing a_cost
		void Cast(const Tuning& a_t, School a_school, float a_cost) noexcept { Add(a_t, a_school, CastGain(a_cost) * Speed(a_t)); }

		// a held spell, this frame: a_costPerSecond for a_delta seconds
		void Hold(const Tuning& a_t, School a_school, float a_costPerSecond, float a_delta) noexcept
		{
			const float dt = Delta(a_delta);
			if (dt > 0.0f) {
				Add(a_t, a_school, CastGain(a_costPerSecond) * Speed(a_t) * std::clamp(a_t.heldGain, 0.0f, 3.0f) * dt);
			}
		}

		// time passing: the drain, and Overdrive's and the cooldown's clocks
		void Tick(const Tuning& a_t, float a_delta) noexcept
		{
			const float dt = Delta(a_delta);
			switch (_state) {
			case State::kOverdrive:
				_timer -= dt;
				if (_timer <= 0.0f) {
					_state = State::kCooldown;
					_timer = std::max(a_t.cooldown, 0.0f);
					_heat.fill(0.0f);
					if (_timer <= 0.0f) {
						_state = State::kBuilding;
					}
				}
				return;
			case State::kCooldown:
				_heat.fill(0.0f);
				_timer -= dt;
				if (_timer <= 0.0f) {
					_state = State::kBuilding;
					_timer = 0.0f;
				}
				return;
			default:
				break;
			}
			_sinceCast += dt;
			if (_sinceCast > std::max(a_t.grace, 0.0f)) {
				const float drop = std::max(a_t.drain, 0.0f) * std::min(dt, _sinceCast - std::max(a_t.grace, 0.0f));
				for (auto& h : _heat) {
					h = std::max(h - drop, 0.0f);
				}
			}
		}

		// the Overdrive key: starts it when the meter is full; false when it cannot
		bool PressKey(const Tuning& a_t) noexcept
		{
			if (a_t.mode != OverdriveMode::kKey || _state != State::kBuilding || Peak() < kFull) {
				return false;
			}
			Start(a_t);
			return true;
		}

		// ---- reading it
		[[nodiscard]] State  Now() const noexcept { return _state; }
		[[nodiscard]] float  Timer() const noexcept { return _timer; }
		[[nodiscard]] School Last() const noexcept { return _last; }
		[[nodiscard]] bool   Ready(const Tuning& a_t) const noexcept
		{
			return a_t.mode == OverdriveMode::kKey && _state == State::kBuilding && Peak() >= kFull;
		}
		// the heat a spell of a_school is judged by (the shared meter, or its school's)
		[[nodiscard]] float HeatOf(const Tuning& a_t, School a_school) const noexcept { return _heat[Index(a_t, a_school)]; }
		// the heat the bar shows: the shared meter, or the last school's
		[[nodiscard]] float Shown(const Tuning& a_t) const noexcept { return _heat[Index(a_t, _last)]; }
		[[nodiscard]] Stage StageOf(const Tuning& a_t, School a_school) const noexcept
		{
			return _state == State::kOverdrive ? Stage::kOverdrive : _state == State::kCooldown ? Stage::kCold :
			                                                                                      StageAt(a_t, HeatOf(a_t, a_school));
		}
		[[nodiscard]] Stage ShownStage(const Tuning& a_t) const noexcept { return StageOf(a_t, _last); }

		// a spell's magnitude multiplier now
		[[nodiscard]] float Bonus(const Tuning& a_t, School a_school) const noexcept
		{
			if (_state == State::kCooldown) {
				return a_t.weaker ? 1.0f - std::clamp(a_t.weakerBy, 0.0f, 0.9f) : 1.0f;
			}
			return StageBonus(a_t, StageOf(a_t, a_school));
		}

		// how fast magicka regenerates now, as a fraction of normal
		[[nodiscard]] float MagickaRegen(const Tuning& a_t) const noexcept
		{
			return _state == State::kCooldown && a_t.slowMagicka ? std::clamp(a_t.slowMagickaTo, 0.0f, 1.0f) : 1.0f;
		}

		// nothing going on: no heat, no Overdrive, no cooldown
		[[nodiscard]] bool Idle() const noexcept
		{
			return _state == State::kBuilding && std::ranges::all_of(_heat, [](float h) { return h <= 0.0f; });
		}

		// ---- the bench: set the heat, force a stage or Overdrive
		void SetHeat(const Tuning& a_t, float a_heat) noexcept
		{
			if (_state != State::kBuilding) {
				_state = State::kBuilding;
				_timer = 0.0f;
			}
			_heat[Index(a_t, _last)] = std::isfinite(a_heat) ? std::clamp(a_heat, 0.0f, kFull) : 0.0f;
			_sinceCast = 0.0f;
		}
		void ForceStage(const Tuning& a_t, Stage a_stage) noexcept
		{
			if (a_stage == Stage::kOverdrive) {
				SetHeat(a_t, kFull);
				Start(a_t);
				return;
			}
			const float at = a_stage == Stage::kCold ? 0.0f : StageThreshold(a_t, a_stage);
			SetHeat(a_t, at);
		}
		void Reset() noexcept { *this = Meter{}; }

	private:
		[[nodiscard]] static float       Delta(float a_delta) noexcept { return std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, 0.25f) : 0.0f; }
		[[nodiscard]] static float       Speed(const Tuning& a_t) noexcept { return std::isfinite(a_t.gain) ? std::clamp(a_t.gain, 0.0f, 5.0f) : 1.0f; }
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
		[[nodiscard]] float Peak() const noexcept { return *std::ranges::max_element(_heat); }

		void Start(const Tuning& a_t) noexcept
		{
			_state = State::kOverdrive;
			_timer = std::max(a_t.burst, 0.1f);
			_heat.fill(0.0f);
			_heat[0] = kFull;  // shown full while it lasts (the shared meter, or the last school's)
			_heat[Index(a_t, _last)] = kFull;
		}

		void Add(const Tuning& a_t, School a_school, float a_gain) noexcept
		{
			if (_state != State::kBuilding || !std::isfinite(a_gain) || a_gain <= 0.0f) {
				return;
			}
			auto& h = _heat[Index(a_t, a_school)];
			if (_started && a_school != _last) {
				// another school: the meter it was building loses heat, once, before this cast adds any
				auto& before = _heat[Index(a_t, _last)];
				before = std::max(before - std::max(a_t.switchLoss, 0.0f), 0.0f);
			}
			const bool same = _started && a_school == _last;
			h = std::min(h + a_gain * (same ? 1.0f + std::max(a_t.sameSchool, 0.0f) : 1.0f), kFull);
			_last = a_school;
			_started = true;
			_sinceCast = 0.0f;
			if (h >= kFull && a_t.mode == OverdriveMode::kAutomatic) {
				Start(a_t);
			}
		}

		std::array<float, kSchools> _heat{};
		School                      _last{ School::kNone };
		bool                        _started{ false };
		float                       _sinceCast{ 0.0f };
		State                       _state{ State::kBuilding };
		float                       _timer{ 0.0f };
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

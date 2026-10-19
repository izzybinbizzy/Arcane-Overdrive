// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The settings, and the settings file's keys, with nothing of the game in it (tests/test_heat.cpp reads and writes them
// off the game; IniText.h does the reading and writing). Settings.cpp only opens the file and keeps the one shared copy.
//
//   [Settings]
//   Enabled=1
//   GainSpeed=100          percent, 25 to 300
//   HeldGain=60            percent, 10 to 200 - a held spell's gain against a cast's
//   DrainSpeed=10          heat a second, 1 to 50
//   GraceSeconds=2         0 to 10
//   PerSchool=0            1: one meter per school
//   SameSchoolBonus=50     percent extra gain, 0 to 200
//   SwitchLoss=25          heat lost on casting another school, 0 to 100
//   WarmAt=25  HotAt=50  OverheatingAt=75
//   BonusPerStage=4        percent, 0 to 25 - Warm gets this, Hot twice, Overheating three times
//   OverdriveBonus=25      percent, 0 to 100
//   Boost=0                0 damage only, 1 all hostile effects
//   Overdrive=0            0 automatic, 1 key press, 2 off
//   OverdriveKey=34        keyboard: the DirectInput scan code (34 is G); mouse 256 and up; gamepad 266 to 281 (SKSE's)
//   BurstSeconds=6         1 to 30
//   CooldownSeconds=10     0 to 60
//   SlowMagicka=1  SlowMagickaTo=40   percent of normal regeneration
//   WeakerSpells=0  WeakerBy=15       percent weaker
//   LightGrowth=1  LightPerStage=15   percent brighter and wider per stage
//   HandGlow=1             a light of its own on a hand no other mod lights
//   Bar=1  BarX=17  BarY=95  BarWidth=100   the bar's centre in percent of the screen, and its size in percent
//   BarStyle=2             0 flames (his order 2026-10-11: "like a fx hud ... like flames"), 1 the simple bar,
//                          2 spell bars (his order 2026-10-15: every look a bar, live, from the spell's own art - HudBars.cpp)
//   SpellGrowth=1  GrowthMax=100   Overdrive grows the spells themselves, up to this much bigger (Overload.cpp)
//   BurnoutFlicker=1       the spells in hand and in the air sputter out through the cooldown
//   ArcHeat=1              the heat meter as a Reticle Arcs bar (Arcs.cpp); while it shows, the bar above steps aside
//   Marks=1  MarkHurt=1    Overheating fire / frost spells leave scorch / frost marks, which hurt who stands in them (Marks.cpp)
//   MarkMax=6  MarkSeconds=8  MarkHurtPerSecond=5   1-20 alive, 2-30 s, 0-50 health a second (x1.5 in Overdrive)
//   HealingAndWards=0  StavesAndScrolls=0  Enemies=0
//   DebugLog=0

#pragma once

#include "Heat.h"
#include "IniText.h"

#include <array>
#include <cstdint>
#include <span>

namespace Plugin
{
	struct Settings
	{
		bool         enabled{ true };
		Heat::Tuning tuning{};
		Heat::Boost  boost{ Heat::Boost::kDamage };
		int          overdriveKey{ 34 };
		bool         lightGrowth{ true };
		float        lightPerStage{ 0.15f };
		bool         handGlow{ true };
		bool         bar{ true };
		float        barX{ 0.17f }, barY{ 0.95f }, barWidth{ 1.0f };
		int          barStyle{ 2 };              // 0 flames, 1 the simple bar, 2 spell bars (the default since 2026-10-15)
		bool         spellGrowth{ true };        // Overdrive grows the spells (the art in hand, the projectiles)
		float        growthMax{ 1.0f };          // how much bigger at the start of Overdrive: 1 = double
		bool         burnoutFlicker{ true };     // the cooldown makes the spells sputter out
		bool         arcHeat{ true };            // the heat meter offered to Reticle Arcs (his answer 2026-10-18, checklist 28 (d))
		bool         marks{ true };              // Overheating fire / frost spells leave scorch / frost marks (Marks.cpp)
		bool         markHurt{ true };           // ... and they hurt who stands in them (his answer 2026-10-18, checklist 28 (b))
		int          markMax{ 6 };               // marks alive at once (ini only)
		float        markSeconds{ 8.0f };        // how long a mark lasts (ini only)
		float        markHurtPerSecond{ 5.0f };  // health a second at Overheating, x1.5 in Overdrive (ini only)
		Heat::Counts counts{};
		bool         enemies{ false };
		bool         debugLog{ false };

		bool operator==(const Settings& a_o) const
		{
			return enabled == a_o.enabled && tuning == a_o.tuning && boost == a_o.boost && overdriveKey == a_o.overdriveKey &&
			       lightGrowth == a_o.lightGrowth && lightPerStage == a_o.lightPerStage && handGlow == a_o.handGlow && bar == a_o.bar &&
			       barX == a_o.barX && barY == a_o.barY && barWidth == a_o.barWidth && barStyle == a_o.barStyle && spellGrowth == a_o.spellGrowth && growthMax == a_o.growthMax &&
			       burnoutFlicker == a_o.burnoutFlicker && arcHeat == a_o.arcHeat && marks == a_o.marks && markHurt == a_o.markHurt &&
			       markMax == a_o.markMax && markSeconds == a_o.markSeconds && markHurtPerSecond == a_o.markHurtPerSecond && counts.healingAndWards == a_o.counts.healingAndWards &&
			       counts.stavesAndScrolls == a_o.counts.stavesAndScrolls && enemies == a_o.enemies && debugLog == a_o.debugLog;
		}
	};

	namespace SettingsText
	{
		using IniText::Pct;
		using IniText::ReadInt;
		using IniText::ToPct;
		using Key = IniText::Key<Settings>;

		[[nodiscard]] inline int   Whole(float a_v) { return std::isfinite(a_v) ? static_cast<int>(std::lround(a_v)) : 0; }
		[[nodiscard]] inline float Within(int a_v, int a_lo, int a_hi) { return static_cast<float>(std::clamp(a_v, a_lo, a_hi)); }
		inline constexpr int       kNoKey = 0;
		inline constexpr int       kLastKey = 511;

		inline constexpr Key kKeys[]{
			{ "Enabled", [](const Settings& s) { return s.enabled ? 1 : 0; }, [](Settings& s, int v) { s.enabled = v != 0; } },
			{ "GainSpeed", [](const Settings& s) { return ToPct(s.tuning.gain); }, [](Settings& s, int v) { s.tuning.gain = Pct(v, 25, 300); } },
			{ "HeldGain", [](const Settings& s) { return ToPct(s.tuning.heldGain); }, [](Settings& s, int v) { s.tuning.heldGain = Pct(v, 10, 200); } },
			{ "DrainSpeed", [](const Settings& s) { return Whole(s.tuning.drain); }, [](Settings& s, int v) { s.tuning.drain = Within(v, 1, 50); } },
			{ "GraceSeconds", [](const Settings& s) { return Whole(s.tuning.grace); }, [](Settings& s, int v) { s.tuning.grace = Within(v, 0, 10); } },
			{ "PerSchool", [](const Settings& s) { return s.tuning.perSchool ? 1 : 0; }, [](Settings& s, int v) { s.tuning.perSchool = v != 0; } },
			{ "SameSchoolBonus", [](const Settings& s) { return ToPct(s.tuning.sameSchool); },
				[](Settings& s, int v) { s.tuning.sameSchool = Pct(v, 0, 200); } },
			{ "SwitchLoss", [](const Settings& s) { return Whole(s.tuning.switchLoss); }, [](Settings& s, int v) { s.tuning.switchLoss = Within(v, 0, 100); } },
			{ "WarmAt", [](const Settings& s) { return Whole(s.tuning.at[0]); }, [](Settings& s, int v) { s.tuning.at[0] = Within(v, 5, 90); } },
			{ "HotAt", [](const Settings& s) { return Whole(s.tuning.at[1]); }, [](Settings& s, int v) { s.tuning.at[1] = Within(v, 10, 95); } },
			{ "OverheatingAt", [](const Settings& s) { return Whole(s.tuning.at[2]); }, [](Settings& s, int v) { s.tuning.at[2] = Within(v, 15, 99); } },
			{ "BonusPerStage", [](const Settings& s) { return ToPct(s.tuning.bonusPerStage); },
				[](Settings& s, int v) { s.tuning.bonusPerStage = Pct(v, 0, 25); } },
			{ "OverdriveBonus", [](const Settings& s) { return ToPct(s.tuning.overdriveBonus); },
				[](Settings& s, int v) { s.tuning.overdriveBonus = Pct(v, 0, 100); } },
			{ "Boost", [](const Settings& s) { return static_cast<int>(s.boost); },
				[](Settings& s, int v) { s.boost = static_cast<Heat::Boost>(std::clamp(v, 0, 1)); } },
			{ "Overdrive", [](const Settings& s) { return static_cast<int>(s.tuning.mode); },
				[](Settings& s, int v) { s.tuning.mode = static_cast<Heat::OverdriveMode>(std::clamp(v, 0, 2)); } },
			{ "OverdriveKey", [](const Settings& s) { return s.overdriveKey; }, [](Settings& s, int v) { s.overdriveKey = std::clamp(v, kNoKey, kLastKey); } },
			{ "BurstSeconds", [](const Settings& s) { return Whole(s.tuning.burst); }, [](Settings& s, int v) { s.tuning.burst = Within(v, 1, 30); } },
			{ "CooldownSeconds", [](const Settings& s) { return Whole(s.tuning.cooldown); }, [](Settings& s, int v) { s.tuning.cooldown = Within(v, 0, 60); } },
			{ "SlowMagicka", [](const Settings& s) { return s.tuning.slowMagicka ? 1 : 0; }, [](Settings& s, int v) { s.tuning.slowMagicka = v != 0; } },
			{ "SlowMagickaTo", [](const Settings& s) { return ToPct(s.tuning.slowMagickaTo); },
				[](Settings& s, int v) { s.tuning.slowMagickaTo = Pct(v, 0, 100); } },
			{ "WeakerSpells", [](const Settings& s) { return s.tuning.weaker ? 1 : 0; }, [](Settings& s, int v) { s.tuning.weaker = v != 0; } },
			{ "WeakerBy", [](const Settings& s) { return ToPct(s.tuning.weakerBy); }, [](Settings& s, int v) { s.tuning.weakerBy = Pct(v, 0, 90); } },
			{ "LightGrowth", [](const Settings& s) { return s.lightGrowth ? 1 : 0; }, [](Settings& s, int v) { s.lightGrowth = v != 0; } },
			{ "LightPerStage", [](const Settings& s) { return ToPct(s.lightPerStage); }, [](Settings& s, int v) { s.lightPerStage = Pct(v, 0, 100); } },
			{ "HandGlow", [](const Settings& s) { return s.handGlow ? 1 : 0; }, [](Settings& s, int v) { s.handGlow = v != 0; } },
			{ "Bar", [](const Settings& s) { return s.bar ? 1 : 0; }, [](Settings& s, int v) { s.bar = v != 0; } },
			{ "BarX", [](const Settings& s) { return ToPct(s.barX); }, [](Settings& s, int v) { s.barX = Pct(v, 0, 100); } },
			{ "BarY", [](const Settings& s) { return ToPct(s.barY); }, [](Settings& s, int v) { s.barY = Pct(v, 0, 100); } },
			{ "BarWidth", [](const Settings& s) { return ToPct(s.barWidth); }, [](Settings& s, int v) { s.barWidth = Pct(v, 50, 300); } },
			{ "BarStyle", [](const Settings& s) { return s.barStyle; }, [](Settings& s, int v) { s.barStyle = std::clamp(v, 0, 2); } },
			{ "SpellGrowth", [](const Settings& s) { return s.spellGrowth ? 1 : 0; }, [](Settings& s, int v) { s.spellGrowth = v != 0; } },
			{ "GrowthMax", [](const Settings& s) { return ToPct(s.growthMax); }, [](Settings& s, int v) { s.growthMax = Pct(v, 0, 200); } },
			{ "BurnoutFlicker", [](const Settings& s) { return s.burnoutFlicker ? 1 : 0; }, [](Settings& s, int v) { s.burnoutFlicker = v != 0; } },
			{ "ArcHeat", [](const Settings& s) { return s.arcHeat ? 1 : 0; }, [](Settings& s, int v) { s.arcHeat = v != 0; } },
			{ "Marks", [](const Settings& s) { return s.marks ? 1 : 0; }, [](Settings& s, int v) { s.marks = v != 0; } },
			{ "MarkHurt", [](const Settings& s) { return s.markHurt ? 1 : 0; }, [](Settings& s, int v) { s.markHurt = v != 0; } },
			{ "MarkMax", [](const Settings& s) { return s.markMax; }, [](Settings& s, int v) { s.markMax = std::clamp(v, 1, 20); } },
			{ "MarkSeconds", [](const Settings& s) { return Whole(s.markSeconds); }, [](Settings& s, int v) { s.markSeconds = Within(v, 2, 30); } },
			{ "MarkHurtPerSecond", [](const Settings& s) { return Whole(s.markHurtPerSecond); },
				[](Settings& s, int v) { s.markHurtPerSecond = Within(v, 0, 50); } },
			{ "HealingAndWards", [](const Settings& s) { return s.counts.healingAndWards ? 1 : 0; },
				[](Settings& s, int v) { s.counts.healingAndWards = v != 0; } },
			{ "StavesAndScrolls", [](const Settings& s) { return s.counts.stavesAndScrolls ? 1 : 0; },
				[](Settings& s, int v) { s.counts.stavesAndScrolls = v != 0; } },
			{ "Enemies", [](const Settings& s) { return s.enemies ? 1 : 0; }, [](Settings& s, int v) { s.enemies = v != 0; } },
			{ "DebugLog", [](const Settings& s) { return s.debugLog ? 1 : 0; }, [](Settings& s, int v) { s.debugLog = v != 0; } },
		};

		// keys an older file may still hold: read, never written (his rename 2026-10-18 - Blazing is Overheating now)
		inline constexpr Key kOldKeys[]{
			{ "BlazingAt", [](const Settings& s) { return Whole(s.tuning.at[2]); }, [](Settings& s, int v) { s.tuning.at[2] = Within(v, 15, 99); } },
		};
		inline constexpr auto kReadKeys = [] {
			std::array<Key, std::size(kKeys) + std::size(kOldKeys)> all{};
			std::ranges::copy(kKeys, all.begin());
			std::ranges::copy(kOldKeys, all.begin() + std::size(kKeys));
			return all;
		}();

		[[nodiscard]] inline std::span<const Key> Keys() noexcept { return kKeys; }

		inline bool Apply(Settings& a_s, std::string_view a_key, int a_value) { return IniText::Apply(std::span<const Key>(kReadKeys), a_s, a_key, a_value); }

		inline IniText::ReadResult Read(std::istream& a_in, Settings& a_s) { return IniText::Read(a_in, a_s, std::span<const Key>(kReadKeys)); }

		inline void Write(std::ostream& a_out, const Settings& a_s) { IniText::Write(a_out, a_s, Keys(), "Arcane Overdrive"); }

		// a gamepad button's code in the numbering SKSE plugins share (SKSE's InputMap::GamepadMaskToKeycode): the game hands
		// a button its XInput mask - the d-pad, Start, Back, the sticks and the shoulders 266-275, A B X Y 276-279 - and a
		// trigger an id of its own, 9 and 10 (280 / 281). -1: not a button SKSE numbers
		[[nodiscard]] constexpr int GamepadCode(std::uint32_t a_id) noexcept
		{
			constexpr std::array<std::uint32_t, 14> kMasks{ 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200,
				0x1000, 0x2000, 0x4000, 0x8000 };
			for (std::size_t i = 0; i < kMasks.size(); ++i) {
				if (kMasks[i] == a_id) {
					return 266 + static_cast<int>(i);
				}
			}
			return a_id == 0x0009 ? 280 : a_id == 0x000A ? 281 :
			                                               -1;
		}

		// a key's name for the menu: keyboard scan codes, then mouse buttons, then gamepad buttons
		[[nodiscard]] inline std::string KeyName(int a_code)
		{
			struct Named
			{
				int         code;
				const char* name;
			};
			static constexpr Named kNames[]{ { 1, "Esc" }, { 14, "Backspace" }, { 15, "Tab" }, { 16, "Q" }, { 17, "W" }, { 18, "E" }, { 19, "R" },
				{ 20, "T" }, { 21, "Y" }, { 22, "U" }, { 23, "I" }, { 24, "O" }, { 25, "P" }, { 28, "Enter" }, { 29, "Left Ctrl" }, { 30, "A" },
				{ 31, "S" }, { 32, "D" }, { 33, "F" }, { 34, "G" }, { 35, "H" }, { 36, "J" }, { 37, "K" }, { 38, "L" }, { 41, "~" },
				{ 42, "Left Shift" }, { 44, "Z" }, { 45, "X" }, { 46, "C" }, { 47, "V" }, { 48, "B" }, { 49, "N" }, { 50, "M" },
				{ 54, "Right Shift" }, { 56, "Left Alt" }, { 57, "Space" }, { 58, "Caps Lock" }, { 59, "F1" }, { 60, "F2" }, { 61, "F3" },
				{ 62, "F4" }, { 63, "F5" }, { 64, "F6" }, { 65, "F7" }, { 66, "F8" }, { 67, "F9" }, { 68, "F10" }, { 87, "F11" }, { 88, "F12" },
				{ 157, "Right Ctrl" }, { 184, "Right Alt" }, { 199, "Home" }, { 200, "Up" }, { 201, "Page Up" }, { 203, "Left" },
				{ 205, "Right" }, { 207, "End" }, { 208, "Down" }, { 209, "Page Down" }, { 210, "Insert" }, { 211, "Delete" } };
			if (a_code <= kNoKey) {
				return "none";
			}
			for (const auto& n : kNames) {
				if (n.code == a_code) {
					return n.name;
				}
			}
			if (a_code >= 2 && a_code <= 11) {
				return std::to_string(a_code == 11 ? 0 : a_code - 1);
			}
			if (a_code >= 266) {
				return "Gamepad button " + std::to_string(a_code - 266 + 1);
			}
			if (a_code >= 256) {
				return "Mouse button " + std::to_string(a_code - 256 + 1);
			}
			return "Key " + std::to_string(a_code);
		}
	}
}

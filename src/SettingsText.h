// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The settings, the settings files' keys and the three presets, with nothing of the game in it (tests/test_heat.cpp reads
// and writes them off the game; IniText.h does the reading and writing). Settings.cpp only opens the files and keeps the
// one shared copy. His answer to the update plan: "functional absolutes will be put in skse menu while everything else
// will be placed in advanced config" - so two files:
//
//   SpellMasteryHeat.ini            [Settings]: what the menu sets - every part on or off, the preset's choices, the bars
//   SpellMasteryHeat Advanced.ini   every number, in sections, each line explained (kAdvancedKeys' notes)
//
// A key is read from either file and from any section (an older one-file ini still reads); each is written only to its own.
// The presets (his names, easy to hard): Milk Drinker, Dragonborn (the defaults), Aedra. A preset is a set of key values;
// the menu shows the one every value matches, else Custom.

#pragma once

#include "Heat.h"
#include "IniText.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

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
		bool         spellGrowth{ true };        // the spells grow with the Overdrive bar (the art in hand, the projectiles)
		float        growthMax{ 1.0f };          // how much bigger at the start of Overdrive: 1 = double
		bool         burnoutFlicker{ true };     // the cooldown makes the spells sputter out
		bool         arcHeat{ true };            // the bars offered to Reticle Arcs (Arcs.cpp)
		bool         marks{ true };              // Overheating fire / frost spells leave scorch / frost marks (Marks.cpp)
		bool         markHurt{ true };           // ... and they hurt who stands in them
		int          markMax{ 6 };               // marks alive at once
		float        markSeconds{ 8.0f };        // how long a mark lasts
		float        markHurtPerSecond{ 5.0f };  // health a second at Overheating, x1.5 in Overdrive
		Heat::Counts counts{};
		bool         enemies{ false };
		bool         debugLog{ false };

		bool operator==(const Settings& a_o) const
		{
			return enabled == a_o.enabled && tuning == a_o.tuning && boost == a_o.boost && overdriveKey == a_o.overdriveKey &&
			       lightGrowth == a_o.lightGrowth && lightPerStage == a_o.lightPerStage && handGlow == a_o.handGlow && bar == a_o.bar &&
			       barX == a_o.barX && barY == a_o.barY && barWidth == a_o.barWidth && spellGrowth == a_o.spellGrowth &&
			       growthMax == a_o.growthMax && burnoutFlicker == a_o.burnoutFlicker && arcHeat == a_o.arcHeat && marks == a_o.marks &&
			       markHurt == a_o.markHurt && markMax == a_o.markMax && markSeconds == a_o.markSeconds && markHurtPerSecond == a_o.markHurtPerSecond &&
			       counts.healingAndWards == a_o.counts.healingAndWards && counts.stavesAndScrolls == a_o.counts.stavesAndScrolls && enemies == a_o.enemies &&
			       debugLog == a_o.debugLog;
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
		[[nodiscard]] inline int   On(bool a_v) { return a_v ? 1 : 0; }
		inline constexpr int       kNoKey = 0;
		inline constexpr int       kLastKey = 511;

		inline constexpr const char* kMenuFile = "Data/SKSE/Plugins/SpellMasteryHeat.ini";
		inline constexpr const char* kAdvancedFile = "Data/SKSE/Plugins/SpellMasteryHeat Advanced.ini";

		// the menu's own file: what is switched on, and the choices
		inline constexpr Key kMenuKeys[]{
			{ "Enabled", [](const Settings& s) { return On(s.enabled); }, [](Settings& s, int v) { s.enabled = v != 0; } },
			{ "Overdrive", [](const Settings& s) { return static_cast<int>(s.tuning.mode); },
				[](Settings& s, int v) { s.tuning.mode = static_cast<Heat::OverdriveMode>(std::clamp(v, 0, 2)); } },
			{ "OverdriveKey", [](const Settings& s) { return s.overdriveKey; }, [](Settings& s, int v) { s.overdriveKey = std::clamp(v, kNoKey, kLastKey); } },
			{ "HeatGate", [](const Settings& s) { return On(s.tuning.heatGate); }, [](Settings& s, int v) { s.tuning.heatGate = v != 0; } },
			{ "Misfire", [](const Settings& s) { return On(s.tuning.misfire); }, [](Settings& s, int v) { s.tuning.misfire = v != 0; } },
			{ "Backlash", [](const Settings& s) { return On(s.tuning.backlash); }, [](Settings& s, int v) { s.tuning.backlash = v != 0; } },
			{ "DamageBonus", [](const Settings& s) { return On(s.tuning.bonusOn); }, [](Settings& s, int v) { s.tuning.bonusOn = v != 0; } },
			{ "Boost", [](const Settings& s) { return static_cast<int>(s.boost); }, [](Settings& s, int v) { s.boost = static_cast<Heat::Boost>(std::clamp(v, 0, 1)); } },
			{ "PerSchool", [](const Settings& s) { return On(s.tuning.perSchool); }, [](Settings& s, int v) { s.tuning.perSchool = v != 0; } },
			{ "SlowMagicka", [](const Settings& s) { return On(s.tuning.slowMagicka); }, [](Settings& s, int v) { s.tuning.slowMagicka = v != 0; } },
			{ "WeakerSpells", [](const Settings& s) { return On(s.tuning.weaker); }, [](Settings& s, int v) { s.tuning.weaker = v != 0; } },
			{ "LightGrowth", [](const Settings& s) { return On(s.lightGrowth); }, [](Settings& s, int v) { s.lightGrowth = v != 0; } },
			{ "HandGlow", [](const Settings& s) { return On(s.handGlow); }, [](Settings& s, int v) { s.handGlow = v != 0; } },
			{ "SpellGrowth", [](const Settings& s) { return On(s.spellGrowth); }, [](Settings& s, int v) { s.spellGrowth = v != 0; } },
			{ "BurnoutFlicker", [](const Settings& s) { return On(s.burnoutFlicker); }, [](Settings& s, int v) { s.burnoutFlicker = v != 0; } },
			{ "Marks", [](const Settings& s) { return On(s.marks); }, [](Settings& s, int v) { s.marks = v != 0; } },
			{ "MarkHurt", [](const Settings& s) { return On(s.markHurt); }, [](Settings& s, int v) { s.markHurt = v != 0; } },
			{ "Bar", [](const Settings& s) { return On(s.bar); }, [](Settings& s, int v) { s.bar = v != 0; } },
			{ "BarX", [](const Settings& s) { return ToPct(s.barX); }, [](Settings& s, int v) { s.barX = Pct(v, 0, 100); } },
			{ "BarY", [](const Settings& s) { return ToPct(s.barY); }, [](Settings& s, int v) { s.barY = Pct(v, 0, 100); } },
			{ "BarWidth", [](const Settings& s) { return ToPct(s.barWidth); }, [](Settings& s, int v) { s.barWidth = Pct(v, 50, 300); } },
			{ "ArcHeat", [](const Settings& s) { return On(s.arcHeat); }, [](Settings& s, int v) { s.arcHeat = v != 0; } },
			{ "HealingAndWards", [](const Settings& s) { return On(s.counts.healingAndWards); }, [](Settings& s, int v) { s.counts.healingAndWards = v != 0; } },
			{ "StavesAndScrolls", [](const Settings& s) { return On(s.counts.stavesAndScrolls); }, [](Settings& s, int v) { s.counts.stavesAndScrolls = v != 0; } },
			{ "Enemies", [](const Settings& s) { return On(s.enemies); }, [](Settings& s, int v) { s.enemies = v != 0; } },
		};

		// the advanced file: every number, in sections, each explained
		inline constexpr Key kAdvancedKeys[]{
			{ "ChargeSpeed", [](const Settings& s) { return ToPct(s.tuning.gain); }, [](Settings& s, int v) { s.tuning.gain = Pct(v, 25, 300); }, "Overdrive",
				"how fast casting fills the Overdrive bar, percent (25 to 300)" },
			{ "HeldGain", [](const Settings& s) { return ToPct(s.tuning.heldGain); }, [](Settings& s, int v) { s.tuning.heldGain = Pct(v, 10, 200); }, "Overdrive",
				"a held spell (Flames, Sparks) against a cast one, percent (10 to 200)" },
			{ "DrainSpeed", [](const Settings& s) { return Whole(s.tuning.drain); }, [](Settings& s, int v) { s.tuning.drain = Within(v, 1, 50); }, "Overdrive",
				"how much of the bar drains a second once the grace period is over (1 to 50; the bar runs 0 to 100)" },
			{ "GraceSeconds", [](const Settings& s) { return Whole(s.tuning.grace); }, [](Settings& s, int v) { s.tuning.grace = Within(v, 0, 10); }, "Overdrive",
				"seconds after your last cast before the bar drains (0 to 10)" },
			{ "SameSchoolBonus", [](const Settings& s) { return ToPct(s.tuning.sameSchool); }, [](Settings& s, int v) { s.tuning.sameSchool = Pct(v, 0, 200); },
				"Overdrive", "how much faster the bar fills when you cast the same school as your last spell, percent (0 to 200)" },
			{ "SwitchLoss", [](const Settings& s) { return Whole(s.tuning.switchLoss); }, [](Settings& s, int v) { s.tuning.switchLoss = Within(v, 0, 100); },
				"Overdrive", "how much of the bar is lost when you cast a different school (0 to 100)" },
			{ "BurstSeconds", [](const Settings& s) { return Whole(s.tuning.burst); }, [](Settings& s, int v) { s.tuning.burst = Within(v, 1, 30); }, "Overdrive",
				"how long Overdrive lasts, seconds (1 to 30)" },
			{ "CooldownSeconds", [](const Settings& s) { return Whole(s.tuning.cooldown); }, [](Settings& s, int v) { s.tuning.cooldown = Within(v, 0, 60); },
				"Overdrive", "the burnout after it, both bars locked at zero, seconds (0 to 60)" },
			{ "SlowMagickaTo", [](const Settings& s) { return ToPct(s.tuning.slowMagickaTo); }, [](Settings& s, int v) { s.tuning.slowMagickaTo = Pct(v, 0, 100); },
				"Overdrive", "with slow magicka on: magicka comes back at this percent of its speed in the burnout (0 to 100)" },
			{ "WeakerBy", [](const Settings& s) { return ToPct(s.tuning.weakerBy); }, [](Settings& s, int v) { s.tuning.weakerBy = Pct(v, 0, 90); }, "Overdrive",
				"with weaker spells on: how much weaker your spells are in the burnout, percent (0 to 90)" },

			{ "Gate", [](const Settings& s) { return static_cast<int>(s.tuning.gate); },
				[](Settings& s, int v) { s.tuning.gate = static_cast<Heat::Gate>(std::clamp(v, 0, 2)); }, "Heat",
				"how Heat lets Overdrive go: 0 the sweet spot (Heat between BandLow and RedLine), 1 vent (Heat below RedLine), 2 focus (Heat at BandLow or "
				"hotter and FocusMs of holding the key, or both hands' spells charged)" },
			{ "BandLow", [](const Settings& s) { return Whole(s.tuning.bandLow); }, [](Settings& s, int v) { s.tuning.bandLow = Within(v, 0, 95); }, "Heat",
				"the sweet spot's bottom (0 to 95)" },
			{ "RedLine", [](const Settings& s) { return Whole(s.tuning.redLine); }, [](Settings& s, int v) { s.tuning.redLine = Within(v, 10, 100); }, "Heat",
				"the red zone starts here: the sweet spot's top, where your spells turn wild (10 to 100)" },
			{ "HeatUp", [](const Settings& s) { return ToPct(s.tuning.heatUp); }, [](Settings& s, int v) { s.tuning.heatUp = Pct(v, 25, 300); }, "Heat",
				"how fast Heat climbs, percent (25 to 300)" },
			{ "HeatDown", [](const Settings& s) { return ToPct(s.tuning.heatDown); }, [](Settings& s, int v) { s.tuning.heatDown = Pct(v, 25, 300); }, "Heat",
				"how fast Heat cools, percent (25 to 300)" },
			{ "HeatPerCast", [](const Settings& s) { return ToPct(s.tuning.heatPerCast); }, [](Settings& s, int v) { s.tuning.heatPerCast = Pct(v, 0, 1000); },
				"Heat", "Heat a cast adds, percent of what it adds to the Overdrive bar (0 to 1000)" },
			{ "HeatHeld", [](const Settings& s) { return ToPct(s.tuning.heatHeld); }, [](Settings& s, int v) { s.tuning.heatHeld = Pct(v, 0, 2000); }, "Heat",
				"Heat a held spell adds a second, percent of a cast of its cost (0 to 2000)" },
			{ "HeatSameElement", [](const Settings& s) { return ToPct(s.tuning.heatSameElement); },
				[](Settings& s, int v) { s.tuning.heatSameElement = Pct(v, 0, 300); }, "Heat", "extra Heat for the same element as your last spell, percent (0 to 300)" },
			{ "HeatDual", [](const Settings& s) { return ToPct(s.tuning.heatDual); }, [](Settings& s, int v) { s.tuning.heatDual = Pct(v, 0, 300); }, "Heat",
				"extra Heat for a dual cast, percent (0 to 300)" },
			{ "Cooling", [](const Settings& s) { return ToPct(s.tuning.cool); }, [](Settings& s, int v) { s.tuning.cool = Pct(v, 5, 100); }, "Heat",
				"the part of its Heat the bar loses a second, percent (5 to 100)" },
			{ "WarmAt", [](const Settings& s) { return Whole(s.tuning.at[0]); }, [](Settings& s, int v) { s.tuning.at[0] = Within(v, 5, 90); }, "Heat",
				"the Heat at which you turn Warm (5 to 90)" },
			{ "HotAt", [](const Settings& s) { return Whole(s.tuning.at[1]); }, [](Settings& s, int v) { s.tuning.at[1] = Within(v, 10, 95); }, "Heat",
				"... Hot (10 to 95)" },
			{ "OverheatingAt", [](const Settings& s) { return Whole(s.tuning.at[2]); }, [](Settings& s, int v) { s.tuning.at[2] = Within(v, 15, 99); }, "Heat",
				"... Overheating (15 to 99)" },
			{ "MisfireDrop", [](const Settings& s) { return Whole(s.tuning.misfireDrop); }, [](Settings& s, int v) { s.tuning.misfireDrop = Within(v, 0, 100); },
				"Heat", "the Heat a misfire sheds (0 to 100)" },
			{ "BacklashMagicka", [](const Settings& s) { return ToPct(s.tuning.backlashMagicka); },
				[](Settings& s, int v) { s.tuning.backlashMagicka = Pct(v, 0, 50); }, "Heat", "with backlash on: the bite a misfire takes out of your magicka, percent of the pool (0 to 50)" },
			{ "FocusMs", [](const Settings& s) { return static_cast<int>(std::lround(s.tuning.focusSeconds * 1000.0f)); },
				[](Settings& s, int v) { s.tuning.focusSeconds = static_cast<float>(std::clamp(v, 0, 5000)) / 1000.0f; }, "Heat",
				"the focus gate's hold, milliseconds (0 to 5000)" },

			{ "BonusPerStage", [](const Settings& s) { return ToPct(s.tuning.bonusPerStage); }, [](Settings& s, int v) { s.tuning.bonusPerStage = Pct(v, 0, 25); },
				"Bonus", "with the damage bonus on: Warm adds this to your spells, Hot twice, Overheating three times, percent (0 to 25)" },
			{ "OverdriveBonus", [](const Settings& s) { return ToPct(s.tuning.overdriveBonus); }, [](Settings& s, int v) { s.tuning.overdriveBonus = Pct(v, 0, 100); },
				"Bonus", "the bonus while Overdrive lasts, percent (0 to 100)" },

			{ "LightPerStage", [](const Settings& s) { return ToPct(s.lightPerStage); }, [](Settings& s, int v) { s.lightPerStage = Pct(v, 0, 100); }, "Display",
				"with light growth on: how much brighter and wider the hand lights grow a stage, percent (0 to 100)" },

			{ "GrowthMax", [](const Settings& s) { return ToPct(s.growthMax); }, [](Settings& s, int v) { s.growthMax = Pct(v, 0, 200); }, "Growth",
				"with spells growing on: how much bigger they are at the start of Overdrive, percent (100 = double, 0 to 200)" },

			{ "MarkMax", [](const Settings& s) { return s.markMax; }, [](Settings& s, int v) { s.markMax = std::clamp(v, 1, 20); }, "Marks",
				"scorch and frost marks alive at once (1 to 20)" },
			{ "MarkSeconds", [](const Settings& s) { return Whole(s.markSeconds); }, [](Settings& s, int v) { s.markSeconds = Within(v, 2, 30); }, "Marks",
				"how long a mark lasts, seconds (2 to 30)" },
			{ "MarkHurtPerSecond", [](const Settings& s) { return Whole(s.markHurtPerSecond); }, [](Settings& s, int v) { s.markHurtPerSecond = Within(v, 0, 50); },
				"Marks", "with marks hurting on: health a second to who stands in one, x1.5 in Overdrive (0 to 50)" },

			{ "DebugLog", [](const Settings& s) { return On(s.debugLog); }, [](Settings& s, int v) { s.debugLog = v != 0; }, "Debug",
				"1: each cast, the bars after it and each release of Overdrive go into SpellMasteryHeat.log" },
		};

		// keys an older file may still hold: read, never written - Blazing became Overheating, and the one meter's gain speed
		// is the Overdrive bar's
		inline constexpr Key kOldKeys[]{
			{ "BlazingAt", [](const Settings& s) { return Whole(s.tuning.at[2]); }, [](Settings& s, int v) { s.tuning.at[2] = Within(v, 15, 99); } },
			{ "GainSpeed", [](const Settings& s) { return ToPct(s.tuning.gain); }, [](Settings& s, int v) { s.tuning.gain = Pct(v, 25, 300); } },
		};

		template <std::size_t... N>
		[[nodiscard]] constexpr auto Joined(const Key (&... a_tables)[N])
		{
			std::array<Key, (N + ...)> all{};
			auto                       at = all.begin();
			((at = std::ranges::copy(a_tables, at).out), ...);
			return all;
		}
		inline constexpr auto kKeys = Joined(kMenuKeys, kAdvancedKeys);
		inline constexpr auto kReadKeys = Joined(kMenuKeys, kAdvancedKeys, kOldKeys);

		[[nodiscard]] inline std::span<const Key> Keys() noexcept { return kKeys; }

		inline bool Apply(Settings& a_s, std::string_view a_key, int a_value) { return IniText::Apply(std::span<const Key>(kReadKeys), a_s, a_key, a_value); }

		// keys of what is gone, skipped quietly: the bar's look (only the high-res bars are left)
		inline constexpr std::string_view kRetired[]{ "BarStyle" };

		inline IniText::ReadResult Read(std::istream& a_in, Settings& a_s)
		{
			return IniText::Read(a_in, a_s, std::span<const Key>(kReadKeys), std::span<const std::string_view>(kRetired));
		}

		inline void WriteMenu(std::ostream& a_out, const Settings& a_s)
		{
			IniText::Write(a_out, a_s, std::span<const Key>(kMenuKeys),
				"; Arcane Overdrive - written by its menu (SKSE Menu Framework). Every number is in SpellMasteryHeat Advanced.ini.\n");
		}

		inline void WriteAdvanced(std::ostream& a_out, const Settings& a_s)
		{
			IniText::Write(a_out, a_s, std::span<const Key>(kAdvancedKeys),
				"; Arcane Overdrive - the advanced settings: every number. Whole numbers only. Edit with the game closed, or press\n"
				"; \"Read the advanced file again\" in the menu. Picking a preset in the menu rewrites the values it sets.\n");
		}

		// ------------------------------------------------------------------ the presets
		struct Preset
		{
			const char*                                 name;
			std::array<std::pair<const char*, int>, 16> values;
		};

		// the same keys in all three, so picking one always lands on it; the damage bonuses are equal in all of them (the
		// presets change how hard the bars are to ride, not the reward)
		inline constexpr Preset kPresets[]{
			{ "Milk Drinker",
				{ { { "HeatGate", 1 }, { "Gate", 1 }, { "BandLow", 50 }, { "RedLine", 85 }, { "Misfire", 0 }, { "Backlash", 0 }, { "HeatUp", 70 },
					{ "HeatDown", 130 }, { "ChargeSpeed", 130 }, { "BurstSeconds", 8 }, { "CooldownSeconds", 6 }, { "SlowMagicka", 0 }, { "SlowMagickaTo", 40 },
					{ "WeakerSpells", 0 }, { "WeakerBy", 15 }, { "Enemies", 0 } } } },
			{ "Dragonborn",
				{ { { "HeatGate", 1 }, { "Gate", 0 }, { "BandLow", 50 }, { "RedLine", 85 }, { "Misfire", 1 }, { "Backlash", 0 }, { "HeatUp", 100 },
					{ "HeatDown", 100 }, { "ChargeSpeed", 100 }, { "BurstSeconds", 6 }, { "CooldownSeconds", 10 }, { "SlowMagicka", 1 }, { "SlowMagickaTo", 40 },
					{ "WeakerSpells", 0 }, { "WeakerBy", 15 }, { "Enemies", 0 } } } },
			{ "Aedra",
				{ { { "HeatGate", 1 }, { "Gate", 0 }, { "BandLow", 60 }, { "RedLine", 80 }, { "Misfire", 1 }, { "Backlash", 1 }, { "HeatUp", 130 },
					{ "HeatDown", 80 }, { "ChargeSpeed", 80 }, { "BurstSeconds", 5 }, { "CooldownSeconds", 14 }, { "SlowMagicka", 1 }, { "SlowMagickaTo", 25 },
					{ "WeakerSpells", 1 }, { "WeakerBy", 15 }, { "Enemies", 1 } } } },
		};
		inline constexpr int kDefaultPreset = 1;

		inline void ApplyPreset(Settings& a_s, std::size_t a_preset)
		{
			if (a_preset < std::size(kPresets)) {
				for (const auto& [key, value] : kPresets[a_preset].values) {
					Apply(a_s, key, value);
				}
			}
		}

		// the preset every value matches; none: Custom
		[[nodiscard]] inline std::optional<std::size_t> PresetOf(const Settings& a_s)
		{
			for (std::size_t i = 0; i < std::size(kPresets); ++i) {
				const bool all = std::ranges::all_of(kPresets[i].values, [&a_s](const auto& kv) {
					const auto* k = IniText::Find(std::span<const Key>(kKeys), kv.first);
					return k && k->get(a_s) == kv.second;
				});
				if (all) {
					return i;
				}
			}
			return std::nullopt;
		}

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

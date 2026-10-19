// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which LOOK the HUD shows for a spell, read from the spell's own data - never from its name - with nothing of the game in
// it (tests/test_heat.cpp). Looks.cpp turns a game spell into the Effect list below.
// HIS LIST, 2026-10-13 ("Cool on arcane design"): fire = flames; frost = a frostbite bar; absorb health = a liquid glass tube
// filling / draining; absorb magicka / stamina = the same tube with blue / green liquid; shock = lightning along a track;
// healing = warm golden light; illusion = violet mist; conjuration = a purple portal ring; bound weapons = a spectral blade
// filling in; alteration = a stone / crystal rune circle; sun / holy = a gold sunburst; poison / disease = a green bubbling
// vial; anything else = the generic arcane ribbon.
//
// How a spell is judged, effect by effect (the costliest effect first, the game's own "main effect"; a cloak's effect is
// replaced by the spell the cloak applies, Looks.cpp):
//   STRONG (the effect itself says what it is), first match wins:
//     bound weapon archetype                       -> bound
//     absorb archetype on Health / Magicka / Stamina -> that absorb tube
//     the keywords MagicDamageFire / Frost / Shock -> fire / frost / shock (keywords first, as Crossfire reads an element)
//     what resists it: fire / frost / shock        -> fire / frost / shock;   poison or disease -> poison
//     turn undead, or a harmful Restoration effect that lowers Health (Sun Fire, Stendarr's Aura, Vampire's Bane: none of
//     them carries a keyword or a resistance - read off Dawnguard.esm 2026-10-13) -> sun
//     a helpful effect that raises Health, MagicRestoreHealth, or a cure (disease, paralysis, poison) -> healing
//     a ward (WardPower, MagicWard)                -> generic (wards are not healing)
//     summon, raise dead, command, soul trap, banish -> conjuration
//     calm, fear, frenzy, invisibility, rally      -> illusion
//     paralysis, telekinesis, detect life, candlelight, lock / open -> alteration
//   WEAK (only its school), used when no effect of the spell is strong, from the costliest effect:
//     Alteration -> alteration, Conjuration -> conjuration, Illusion -> illusion, Restoration -> healing, else generic

#pragma once

#include "Heat.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace SpellLook
{
	enum class Look : std::uint8_t
	{
		kGeneric,
		kFire,
		kFrost,
		kShock,
		kAbsorbHealth,
		kAbsorbMagicka,
		kAbsorbStamina,
		kHealing,
		kIllusion,
		kConjuration,
		kBound,
		kAlteration,
		kSun,
		kPoison
	};
	inline constexpr std::size_t kLooks = 14;

	// the look's name: also the name of its HUD sheet (LookSheets.h)
	[[nodiscard]] constexpr std::string_view Name(Look a_look) noexcept
	{
		constexpr std::string_view kNames[]{ "generic", "fire", "frost", "shock", "absorbhealth", "absorbmagicka", "absorbstamina",
			"healing", "illusion", "conjuration", "bound", "alteration", "sun", "poison" };
		const auto                 i = static_cast<std::size_t>(a_look);
		return i < kLooks ? kNames[i] : kNames[0];
	}

	// a magic effect's archetype, numbered exactly as the game numbers it (CommonLib's RE::EffectArchetypes::ArchetypeID);
	// only the ones the judging below names are listed - any other number is simply "some other archetype"
	enum class Archetype : std::int32_t
	{
		kValueModifier = 0,
		kCureDisease = 3,
		kAbsorb = 4,
		kDualValueModifier = 5,
		kCalm = 6,
		kDemoralize = 7,
		kFrenzy = 8,
		kCommandSummoned = 10,
		kInvisibility = 11,
		kLight = 12,
		kLock = 15,
		kOpen = 16,
		kBoundWeapon = 17,
		kSummonCreature = 18,
		kDetectLife = 19,
		kTelekinesis = 20,
		kParalysis = 21,
		kReanimate = 22,
		kSoulTrap = 23,
		kTurnUndead = 24,
		kCureParalysis = 27,
		kCurePoison = 29,
		kPeakValueModifier = 34,
		kCloak = 35,
		kRally = 38,
		kBanish = 42
	};

	enum class Value : std::uint8_t  // the actor value an effect works on
	{
		kOther,
		kHealth,
		kMagicka,
		kStamina,
		kWard
	};

	enum class Resist : std::uint8_t  // what resists the effect
	{
		kNone,
		kFire,
		kFrost,
		kShock,
		kPoison,
		kDisease,
		kOther
	};

	struct Keywords
	{
		bool fire{ false }, frost{ false }, shock{ false };  // MagicDamageFire / Frost / Shock
		bool restoreHealth{ false };                         // MagicRestoreHealth
		bool ward{ false };                                  // MagicWard
	};

	struct Effect
	{
		Archetype    archetype{ Archetype::kValueModifier };
		Value        primary{ Value::kOther };
		Value        secondary{ Value::kOther };
		Resist       resist{ Resist::kNone };
		Heat::School school{ Heat::School::kNone };
		bool         harmful{ false };  // hostile or detrimental
		Keywords     keys{};
	};

	struct Verdict
	{
		Look look{ Look::kGeneric };
		bool strong{ false };  // the effect itself said it; false = only its school did
	};

	[[nodiscard]] constexpr bool ChangesValue(const Effect& a_e) noexcept
	{
		switch (a_e.archetype) {
		case Archetype::kValueModifier:
		case Archetype::kPeakValueModifier:
		case Archetype::kDualValueModifier:
		case Archetype::kAbsorb:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] constexpr bool OnHealth(const Effect& a_e) noexcept
	{
		return a_e.primary == Value::kHealth || (a_e.archetype == Archetype::kDualValueModifier && a_e.secondary == Value::kHealth);
	}

	[[nodiscard]] constexpr Verdict Judge(const Effect& a_e) noexcept
	{
		using A = Archetype;
		if (a_e.archetype == A::kBoundWeapon) {
			return { Look::kBound, true };
		}
		if (a_e.archetype == A::kAbsorb) {
			switch (a_e.primary) {
			case Value::kHealth:
				return { Look::kAbsorbHealth, true };
			case Value::kMagicka:
				return { Look::kAbsorbMagicka, true };
			case Value::kStamina:
				return { Look::kAbsorbStamina, true };
			default:
				break;  // an absorb of something else: judged by what follows
			}
		}
		if (a_e.keys.fire) {
			return { Look::kFire, true };
		}
		if (a_e.keys.frost) {
			return { Look::kFrost, true };
		}
		if (a_e.keys.shock) {
			return { Look::kShock, true };
		}
		switch (a_e.resist) {
		case Resist::kFire:
			return { Look::kFire, true };
		case Resist::kFrost:
			return { Look::kFrost, true };
		case Resist::kShock:
			return { Look::kShock, true };
		case Resist::kPoison:
		case Resist::kDisease:
			return { Look::kPoison, true };
		default:
			break;
		}
		if (a_e.archetype == A::kTurnUndead ||
			(a_e.school == Heat::School::kRestoration && a_e.harmful && ChangesValue(a_e) && OnHealth(a_e))) {
			return { Look::kSun, true };
		}
		if (!a_e.harmful && ((ChangesValue(a_e) && OnHealth(a_e)) || a_e.keys.restoreHealth || a_e.archetype == A::kCureDisease ||
								a_e.archetype == A::kCureParalysis || a_e.archetype == A::kCurePoison)) {
			return { Look::kHealing, true };
		}
		if (a_e.primary == Value::kWard || a_e.keys.ward) {
			return { Look::kGeneric, true };
		}
		switch (a_e.archetype) {
		case A::kSummonCreature:
		case A::kReanimate:
		case A::kCommandSummoned:
		case A::kSoulTrap:
		case A::kBanish:
			return { Look::kConjuration, true };
		case A::kCalm:
		case A::kDemoralize:
		case A::kFrenzy:
		case A::kInvisibility:
		case A::kRally:
			return { Look::kIllusion, true };
		case A::kParalysis:
		case A::kTelekinesis:
		case A::kDetectLife:
		case A::kLight:
		case A::kLock:
		case A::kOpen:
			return { Look::kAlteration, true };
		default:
			break;
		}
		switch (a_e.school) {
		case Heat::School::kAlteration:
			return { Look::kAlteration, false };
		case Heat::School::kConjuration:
			return { Look::kConjuration, false };
		case Heat::School::kIllusion:
			return { Look::kIllusion, false };
		case Heat::School::kRestoration:
			return { Look::kHealing, false };
		default:
			return { Look::kGeneric, false };
		}
	}

	// a whole spell: a_effects with the costliest first. The first effect that says what it is decides; when none does,
	// the costliest effect's school
	[[nodiscard]] constexpr Look Classify(std::span<const Effect> a_effects) noexcept
	{
		for (const auto& e : a_effects) {
			if (const auto v = Judge(e); v.strong) {
				return v.look;
			}
		}
		return a_effects.empty() ? Look::kGeneric : Judge(a_effects.front()).look;
	}
}

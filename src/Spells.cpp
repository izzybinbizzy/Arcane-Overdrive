// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// What a spell is to the meter, read from its costliest effect - the game's own rule for "which school is this spell":
//   school     the effect's magic skill (Alteration ... Restoration; anything else is "no school")
//   kind       a spell; a staff; a scroll; a power or shout (never builds heat); anything else (never)
//   healing or ward   a beneficial effect on Health (or with MagicRestoreHealth), or a ward (WardPower, MagicWard)
//   held       a concentration spell (Flames, Sparks): it builds heat per second, from the per-frame pass
//   element    for the hand glow's color: the effect's keywords first, then what resists it, as Crossfire reads it
// And which effects the bonus touches: damage only (a hostile or harmful effect that lowers Health), or every hostile
// or harmful effect.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		const RE::EffectSetting* MainEffect(const RE::MagicItem* a_item)
		{
			if (!a_item) {
				return nullptr;
			}
			if (const auto* e = a_item->GetCostliestEffectItem(); e && e->baseEffect) {
				return e->baseEffect;
			}
			for (const auto* e : a_item->effects) {
				if (e && e->baseEffect) {
					return e->baseEffect;
				}
			}
			return nullptr;
		}

		Heat::School SchoolOf(const RE::EffectSetting* a_effect)
		{
			switch (a_effect ? a_effect->GetMagickSkill() : RE::ActorValue::kNone) {
			case RE::ActorValue::kAlteration:
				return Heat::School::kAlteration;
			case RE::ActorValue::kConjuration:
				return Heat::School::kConjuration;
			case RE::ActorValue::kDestruction:
				return Heat::School::kDestruction;
			case RE::ActorValue::kIllusion:
				return Heat::School::kIllusion;
			case RE::ActorValue::kRestoration:
				return Heat::School::kRestoration;
			default:
				return Heat::School::kNone;
			}
		}

		bool Has(const RE::EffectSetting* a_effect, std::string_view a_keyword)
		{
			return a_effect && a_effect->HasKeywordString(a_keyword);
		}

		bool ChangesValue(const RE::EffectSetting* a_effect)
		{
			using A = RE::EffectSetting::Archetype;
			switch (a_effect->GetArchetype()) {
			case A::kValueModifier:
			case A::kPeakValueModifier:
			case A::kDualValueModifier:
			case A::kAbsorb:
				return true;
			default:
				return false;
			}
		}

		bool OnHealth(const RE::EffectSetting* a_effect)
		{
			return a_effect->data.primaryAV == RE::ActorValue::kHealth ||
			       (a_effect->GetArchetype() == RE::EffectSetting::Archetype::kDualValueModifier && a_effect->data.secondaryAV == RE::ActorValue::kHealth);
		}

		int ElementOf(const RE::EffectSetting* a_effect)
		{
			if (!a_effect) {
				return 0;
			}
			if (Has(a_effect, "MagicDamageFire")) {
				return 1;
			}
			if (Has(a_effect, "MagicDamageFrost")) {
				return 2;
			}
			if (Has(a_effect, "MagicDamageShock")) {
				return 3;
			}
			switch (a_effect->data.resistVariable) {
			case RE::ActorValue::kResistFire:
				return 1;
			case RE::ActorValue::kResistFrost:
				return 2;
			case RE::ActorValue::kResistShock:
				return 3;
			default:
				return 0;
			}
		}

		SpellInfo ReadNow(const RE::MagicItem* a_item)
		{
			SpellInfo   info;
			const auto* effect = MainEffect(a_item);
			if (!a_item || !effect) {
				info.facts.kind = Heat::SpellFacts::Kind::kOther;
				return info;
			}
			using T = RE::MagicSystem::SpellType;
			using K = Heat::SpellFacts::Kind;
			switch (a_item->GetSpellType()) {
			case T::kSpell:
				info.facts.kind = K::kSpell;
				break;
			case T::kStaffEnchantment:
				info.facts.kind = K::kStaff;
				break;
			case T::kScroll:
				info.facts.kind = K::kScroll;
				break;
			case T::kPower:
			case T::kLesserPower:
			case T::kVoicePower:
				info.facts.kind = K::kPower;
				break;
			default:
				info.facts.kind = K::kOther;
				break;
			}
			const bool harmful = effect->IsHostile() || effect->IsDetrimental();
			const bool healing = !harmful && ((ChangesValue(effect) && OnHealth(effect)) || Has(effect, "MagicRestoreHealth"));
			const bool ward = effect->data.primaryAV == RE::ActorValue::kWardPower || Has(effect, "MagicWard");
			info.facts.healingOrWard = healing || ward;
			info.school = SchoolOf(effect);
			info.held = a_item->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
			info.element = ElementOf(effect);
			return info;
		}
	}

	namespace
	{
		std::atomic<std::uint32_t> gReadFrame{ 0 };
	}

	void NewReadFrame() { gReadFrame.fetch_add(1, std::memory_order_relaxed); }

	std::uint32_t ReadFrameNow() { return gReadFrame.load(std::memory_order_relaxed); }

	SpellInfo Read(const RE::MagicItem* a_item)
	{
		// the same few spells are read many times a frame (both hands, the held spell, the hand lights): the last four read on
		// this thread are kept for the frame they were read in, so a spell another mod changes is read afresh the next frame
		struct Kept
		{
			const RE::MagicItem* item{ nullptr };
			RE::FormID           id{ 0 };
			std::uint32_t        frame{ 0 };
			SpellInfo            info;
		};
		thread_local std::array<Kept, 4> kept{};
		thread_local std::size_t         next = 0;
		const RE::FormID                 id = a_item ? a_item->GetFormID() : 0;
		const std::uint32_t              frame = gReadFrame.load(std::memory_order_relaxed);
		for (const auto& k : kept) {
			if (k.item && k.item == a_item && k.id == id && k.frame == frame) {
				return k.info;
			}
		}
		const auto info = ReadNow(a_item);
		if (a_item) {
			kept[next] = { a_item, id, frame, info };
			next = (next + 1) % kept.size();
		}
		return info;
	}

	bool Boosted(const RE::EffectSetting* a_effect, Heat::Boost a_boost)
	{
		if (!a_effect || !(a_effect->IsHostile() || a_effect->IsDetrimental())) {
			return false;
		}
		return a_boost == Heat::Boost::kAll || (ChangesValue(a_effect) && OnHealth(a_effect));
	}
}

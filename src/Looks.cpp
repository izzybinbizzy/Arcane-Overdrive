// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Which HUD look a spell gets (SpellLook.h has the rules and his list): the spell's magic effects, read off the game's own
// records - archetype, the actor values it works on, what resists it, its school, hostile / detrimental, five keywords - the
// costliest effect first. A cloak (Flame Cloak, Stendarr's Aura) is judged by the spell it applies, one level deep. Never the
// spell's name, so a modded spell sorts itself.
// Everything here runs on the main thread (the player's update).

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		using SpellLook::Resist;
		using SpellLook::Value;

		Value ValueOf(RE::ActorValue a_av)
		{
			switch (a_av) {
			case RE::ActorValue::kHealth:
				return Value::kHealth;
			case RE::ActorValue::kMagicka:
				return Value::kMagicka;
			case RE::ActorValue::kStamina:
				return Value::kStamina;
			case RE::ActorValue::kWardPower:
				return Value::kWard;
			default:
				return Value::kOther;
			}
		}

		Resist ResistOf(RE::ActorValue a_av)
		{
			switch (a_av) {
			case RE::ActorValue::kNone:
				return Resist::kNone;
			case RE::ActorValue::kResistFire:
				return Resist::kFire;
			case RE::ActorValue::kResistFrost:
				return Resist::kFrost;
			case RE::ActorValue::kResistShock:
				return Resist::kShock;
			case RE::ActorValue::kPoisonResist:
				return Resist::kPoison;
			case RE::ActorValue::kResistDisease:
				return Resist::kDisease;
			default:
				return Resist::kOther;
			}
		}

		Heat::School SchoolOf(RE::ActorValue a_skill)
		{
			switch (a_skill) {
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

		SpellLook::Effect FactsOf(const RE::EffectSetting& a_mgef)
		{
			SpellLook::Effect e;
			e.archetype = static_cast<SpellLook::Archetype>(std::to_underlying(a_mgef.GetArchetype()));
			e.primary = ValueOf(a_mgef.data.primaryAV);
			e.secondary = ValueOf(a_mgef.data.secondaryAV);
			e.resist = ResistOf(a_mgef.data.resistVariable);
			e.school = SchoolOf(a_mgef.GetMagickSkill());
			e.harmful = a_mgef.IsHostile() || a_mgef.IsDetrimental();
			e.keys.fire = a_mgef.HasKeywordString("MagicDamageFire");
			e.keys.frost = a_mgef.HasKeywordString("MagicDamageFrost");
			e.keys.shock = a_mgef.HasKeywordString("MagicDamageShock");
			e.keys.restoreHealth = a_mgef.HasKeywordString("MagicRestoreHealth");
			e.keys.ward = a_mgef.HasKeywordString("MagicWard");
			return e;
		}

		// a spell's effects, as plain facts; a fixed list (no allocation: this runs every frame for the spells in hand)
		class Effects
		{
		public:
			void Add(const SpellLook::Effect& a_e) noexcept
			{
				if (_count < _list.size()) {
					_list[_count++] = a_e;
				}
			}
			[[nodiscard]] std::span<const SpellLook::Effect> All() const noexcept { return { _list.data(), _count }; }

		private:
			std::array<SpellLook::Effect, 24> _list{};
			std::size_t                       _count{ 0 };
		};

		void AddSpell(Effects& a_out, const RE::MagicItem& a_item, bool a_followCloak);

		void AddEffect(Effects& a_out, const RE::EffectSetting& a_mgef, bool a_followCloak)
		{
			if (a_followCloak && a_mgef.GetArchetype() == RE::EffectSetting::Archetype::kCloak && a_mgef.data.associatedForm) {
				if (const auto* applied = a_mgef.data.associatedForm->As<RE::MagicItem>()) {
					AddSpell(a_out, *applied, false);  // one level: a cloak that applies a cloak stops here
					return;
				}
			}
			a_out.Add(FactsOf(a_mgef));
		}

		void AddSpell(Effects& a_out, const RE::MagicItem& a_item, bool a_followCloak)
		{
			const auto* main = a_item.GetCostliestEffectItem();
			if (main && main->baseEffect) {
				AddEffect(a_out, *main->baseEffect, a_followCloak);
			}
			for (const auto* e : a_item.effects) {
				if (e && e != main && e->baseEffect) {
					AddEffect(a_out, *e->baseEffect, a_followCloak);
				}
			}
		}
	}

	SpellLook::Look LookOf(const RE::MagicItem* a_item)
	{
		if (!a_item) {
			return SpellLook::Look::kGeneric;
		}
		// the spell in hand is looked at every frame: the last four answers on this thread are kept for about a second, and
		// only while the spell's effects are the ones they were read from (a mod that rewrites them is seen at once)
		struct Kept
		{
			const RE::MagicItem*     item{ nullptr };
			RE::FormID               id{ 0 };
			std::uint32_t            frame{ 0 };
			std::size_t              effects{ 0 };
			const RE::EffectSetting* main{ nullptr };
			SpellLook::Look          look{ SpellLook::Look::kGeneric };
		};
		constexpr std::uint32_t          kKeepFrames = 60;
		thread_local std::array<Kept, 4> kept{};
		thread_local std::size_t         next = 0;
		const RE::FormID                 id = a_item->GetFormID();
		const std::uint32_t              frame = ReadFrameNow();
		const auto*                      costliest = a_item->GetCostliestEffectItem();
		const RE::EffectSetting*         main = costliest ? costliest->baseEffect : nullptr;
		const std::size_t                count = a_item->effects.size();
		for (const auto& k : kept) {
			if (k.item == a_item && k.id == id && k.effects == count && k.main == main && frame - k.frame <= kKeepFrames) {
				return k.look;
			}
		}
		Effects effects;
		AddSpell(effects, *a_item, true);
		const auto look = SpellLook::Classify(effects.All());
		kept[next] = { a_item, id, frame, count, main, look };
		next = (next + 1) % kept.size();
		return look;
	}

	const RE::MagicItem* HeldSpell(RE::Actor* a_actor, const Settings& a_s)
	{
		if (!a_actor) {
			return nullptr;
		}
		for (const std::size_t hand : { std::size_t{ 1 }, std::size_t{ 0 } }) {  // the right hand first, as the flames' tint
			auto* caster = a_actor->GetActorRuntimeData().magicCasters[hand];
			if (!caster || !caster->currentSpell || caster->state.get() != RE::MagicCaster::State::kCasting) {
				continue;
			}
			const auto info = Read(caster->currentSpell);
			if (info.held && Heat::BuildsHeat(info.facts, a_s.counts)) {
				return caster->currentSpell;
			}
		}
		return nullptr;
	}

	const RE::MagicItem* EquippedSpell(RE::PlayerCharacter* a_player)
	{
		if (!a_player) {
			return nullptr;
		}
		for (const bool leftHand : { false, true }) {
			const auto* item = a_player->GetEquippedObject(leftHand);
			if (const auto* spell = item ? item->As<RE::MagicItem>() : nullptr) {
				return spell;
			}
		}
		return nullptr;
	}
}

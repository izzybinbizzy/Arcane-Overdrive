// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// What a projectile is to Crossfire, worked out once for each combination of projectile record, spell, effect, ammo
// and enchantment, and remembered.
//
// Only hostile projectiles clash: an arrow, a bolt, or a spell with a hostile or detrimental effect. A spell that
// harms nobody (Magelight, a mod's invisible scripted marker) is never touched, so nothing that relies on its
// projectile arriving can break. Anything named in [Exclude] is never touched either.
//
// The element comes from the effect's keywords (the [Elements] lists), then its resist value, then "a shout's cone
// with no element is Force"; an arrow is Physical unless its enchantment has an element (Core::Classify).

#include "ClashShared.h"

namespace Crossfire
{
	namespace
	{
		struct Key
		{
			const void* base;
			const void* spell;
			const void* effect;
			const void* ammo;
			const void* weapon;  // the bow or crossbow: [Exclude] can name one, so two bows sharing an arrow may differ
			const void* enchantment;
			bool        operator==(const Key&) const = default;
		};

		struct KeyHash
		{
			[[nodiscard]] std::size_t operator()(const Key& a_key) const noexcept
			{
				std::size_t h = 0;
				for (const void* p : { a_key.base, a_key.spell, a_key.effect, a_key.ammo, a_key.weapon, a_key.enchantment }) {
					h ^= std::hash<const void*>{}(p) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
				}
				return h;
			}
		};

		std::unordered_map<Key, Info, KeyHash> gCache;

		[[nodiscard]] std::string_view ResistName(RE::ActorValue a_av) noexcept
		{
			switch (a_av) {
			case RE::ActorValue::kResistFire:
				return "ResistFire";
			case RE::ActorValue::kResistFrost:
				return "ResistFrost";
			case RE::ActorValue::kResistShock:
				return "ResistShock";
			case RE::ActorValue::kPoisonResist:
				return "PoisonResist";
			default:
				return {};
			}
		}

		[[nodiscard]] Core::Element ElementOf(const RE::EffectSetting* a_effect, bool a_voice, bool a_cone)
		{
			std::array<std::string_view, 32> names{};
			std::size_t                      count = 0;
			std::string_view                 resist;
			if (a_effect) {
				for (std::uint32_t i = 0; a_effect->keywords && i < a_effect->numKeywords && count < names.size(); ++i) {
					if (const auto* kw = a_effect->keywords[i]) {
						if (const char* id = kw->GetFormEditorID(); id && *id) {
							names[count++] = id;
						}
					}
				}
				resist = ResistName(a_effect->data.resistVariable);
			}
			const Core::MagicFacts facts{ std::span<const std::string_view>(names.data(), count), resist, a_voice, a_cone };
			return Core::Classify(facts, Live().keywords);
		}

		[[nodiscard]] std::optional<Core::Rgb> LightOf(const RE::EffectSetting* a_effect)
		{
			const auto* light = a_effect ? a_effect->data.light : nullptr;
			if (!light) {
				return std::nullopt;
			}
			const auto& c = light->data.color;
			return Core::Rgb{ c.red, c.green, c.blue };
		}

		// the clash design: from the effect's tags, then words in the effect's, spell's and projectile's names and editor
		// IDs (where the game keeps them) and the projectile's model file, then its element, then its light's hue
		[[nodiscard]] Core::Look LookFor(const RE::EffectSetting* a_effect, const RE::MagicItem* a_spell, const RE::BGSProjectile* a_base,
			const Info& a_info)
		{
			std::array<std::string_view, 32> tags{};
			std::size_t                      count = 0;
			for (std::uint32_t i = 0; a_effect && a_effect->keywords && i < a_effect->numKeywords && count < tags.size(); ++i) {
				if (const auto* kw = a_effect->keywords[i]) {
					if (const char* id = kw->GetFormEditorID(); id && *id) {
						tags[count++] = id;
					}
				}
			}
			std::string words;
			const auto  add = [&](std::string_view a_text) {
				if (!a_text.empty()) {
					words.append(a_text).push_back(' ');
				}
			};
			const auto text = [](const char* a_text) { return a_text ? std::string_view(a_text) : std::string_view(); };
			if (a_effect) {
				add(text(a_effect->GetFullName()));
				add(text(a_effect->GetFormEditorID()));
			}
			if (a_spell) {
				add(text(a_spell->GetFullName()));
				add(text(a_spell->GetFormEditorID()));
			}
			if (a_base) {
				add(text(a_base->GetFormEditorID()));
				add(Core::FileOf(text(a_base->GetModel())));  // the file only: nearly every spell's folder is "Magic"
			}
			const Core::LookFacts facts{ a_info.element, std::span<const std::string_view>(tags.data(), count), words, false, a_info.absorb,
				a_info.resist == RE::ActorValue::kResistMagic, a_info.light };
			return Core::LookOf(facts);
		}

		[[nodiscard]] const RE::EffectSetting* MainEffect(const RE::MagicItem* a_item)
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

		[[nodiscard]] bool Hostile(const RE::MagicItem* a_item, const RE::EffectSetting* a_effect)
		{
			if (a_effect && (a_effect->IsHostile() || a_effect->IsDetrimental())) {
				return true;
			}
			if (a_item) {
				for (const auto* e : a_item->effects) {
					if (e && e->baseEffect && (e->baseEffect->IsHostile() || e->baseEffect->IsDetrimental())) {
						return true;
					}
				}
			}
			return false;
		}

		// the spell's base cost: its override if it has one, otherwise what its effects cost
		[[nodiscard]] float CostOf(const RE::MagicItem* a_item)
		{
			if (!a_item) {
				return 0.0f;
			}
			if (const auto* data = a_item->GetData(); data && (data->flags & 1u) != 0 && data->costOverride > 0) {
				return static_cast<float>(data->costOverride);
			}
			float cost = 0.0f;
			for (const auto* e : a_item->effects) {
				if (e && std::isfinite(e->cost) && e->cost > 0.0f) {
					cost += e->cost;
				}
			}
			return cost;
		}

		[[nodiscard]] Info Work(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base, const Key& a_key)
		{
			Info        info;
			auto&       rd = a_projectile->GetProjectileRuntimeData();
			const auto* spell = static_cast<const RE::MagicItem*>(a_key.spell);
			const auto* effect = static_cast<const RE::EffectSetting*>(a_key.effect);
			const auto* enchantment = static_cast<const RE::EnchantmentItem*>(a_key.enchantment);
			const auto  type = a_projectile->GetFormType();

			if (Excluded(a_base) || Excluded(spell) || Excluded(effect) || Excluded(rd.ammoSource) || Excluded(rd.weaponSource) || Excluded(enchantment)) {
				return info;
			}
			const bool arrow = type == RE::FormType::ProjectileArrow && !spell;
			const bool voice = spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower;
			info.cone = type == RE::FormType::ProjectileCone;

			if (arrow) {
				info.eligible = true;
				info.kind = Core::Kind::kArrow;
				info.skill = RE::ActorValue::kArchery;
				info.element = Core::Element::kPhysical;
				info.look = Core::Look::kPhysical;
				if (const auto* enchantEffect = MainEffect(enchantment)) {
					if (const auto e = ElementOf(enchantEffect, false, false); e != Core::Element::kArcane) {
						info.element = e;
					}
				}
				return info;
			}
			if (!spell || !Hostile(spell, effect)) {
				return info;  // not an arrow and harms nobody: never ours to touch
			}
			info.eligible = true;
			info.element = ElementOf(effect, voice, info.cone);
			info.cost = CostOf(spell);
			info.skill = voice ? RE::ActorValue::kNone : spell->GetAssociatedSkill();
			const bool concentration = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
			info.staff = spell->GetSpellType() == RE::MagicSystem::SpellType::kStaffEnchantment;
			info.absorb = effect && effect->data.archetype == RE::EffectArchetypes::ArchetypeID::kAbsorb;
			info.resist = effect ? effect->data.resistVariable : RE::ActorValue::kNone;
			if (const auto* top = spell->GetCostliestEffectItem(); top && std::isfinite(top->effectItem.magnitude)) {
				info.magnitude = top->effectItem.magnitude;
			}
			info.spell = spell;
			info.light = LightOf(effect);
			info.look = LookFor(effect, spell, a_base, info);
			switch (type) {
			case RE::FormType::ProjectileBeam:
				info.kind = Core::Kind::kBeam;
				info.shape = Core::Shape::kBeam;
				info.immune = true;
				break;
			case RE::FormType::ProjectileBarrier:
				info.kind = Core::Kind::kBarrier;
				info.shape = Core::Shape::kBarrier;
				info.immune = true;
				break;
			case RE::FormType::ProjectileCone:
				info.kind = voice ? Core::Kind::kVoice : Core::Kind::kCone;
				info.immune = true;
				break;
			case RE::FormType::ProjectileFlame:
				info.kind = Core::Kind::kStream;
				if (voice) {
					info.cost = Live().tuning.shoutStrength;  // a breath shout's spray: each particle a share of a shout
				}
				break;
			default:
				info.kind = voice ? Core::Kind::kVoice : Core::Kind::kSpell;
				break;
			}
			info.stream = Core::StreamOf(info.kind, concentration, voice);  // the switches are read live, not cached here
			(void)a_base;
			return info;
		}
	}

	const Info& InfoOf(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base)
	{
		auto&                      rd = a_projectile->GetProjectileRuntimeData();
		const RE::MagicItem*       spell = rd.spell;
		const RE::EffectSetting*   effect = rd.avEffect ? rd.avEffect : MainEffect(spell);
		const RE::EnchantmentItem* enchantment = nullptr;
		if (a_projectile->GetFormType() == RE::FormType::ProjectileArrow) {
			enchantment = static_cast<RE::ArrowProjectile*>(a_projectile)->GetArrowRuntimeData().enchantItem;
		}
		const Key key{ a_base, spell, effect, rd.ammoSource, rd.weaponSource, enchantment };
		if (const auto it = gCache.find(key); it != gCache.end()) {
			return it->second;
		}
		if (gCache.size() > 4096) {
			gCache.clear();  // cannot happen in a real game; a bound all the same
		}
		return gCache.emplace(key, Work(a_projectile, a_base, key)).first->second;
	}

	void ClearInfo() { gCache.clear(); }

	RE::BGSExplosion* ExplosionOf(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base)
	{
		if (auto* e = a_projectile->GetProjectileRuntimeData().explosion) {
			return e;
		}
		if (a_base && a_base->data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kExplosion)) {
			return a_base->data.explosionType;
		}
		return nullptr;
	}

	// an explosion that only shows: no damage of its own, no enchantment, nothing it spawns or places. The ones spells
	// use are like this (a spell's own effect is what hurts, and Crossfire does not deliver it); a mod's may not be.
	bool SafeExplosion(const RE::BGSExplosion* a_explosion)
	{
		return a_explosion && !a_explosion->formEnchanting && !(a_explosion->data.damage > 0.0f) && !a_explosion->data.spawnProjectile &&
		       !a_explosion->data.impactPlacedObject;
	}

	namespace
	{
		RE::SpellItem*                                 gFear = nullptr;
		std::array<RE::BGSExplosion*, Core::kElements> gStandIn{};
		std::array<std::string, Core::kLooks>          gClashArt{};  // meshes\ArcaneOverdrive\Clash<Look>.nif, when there is one
		std::unordered_map<std::string, std::string>   gPairArt;     // a pair's name -> its mesh, when there is one
	}

	// For each element, the burst a clash shows for a spell that has none of its own (Firebolt, Ice Spike, Lightning
	// Bolt): the explosion of the costliest aimed spell of that element. Skyrim.esm's own spells first, so the look is the
	// game's; any plugin's when the game has none. Most spell explosions carry the spell's harm (82 of 102 in his order),
	// so one that does is shown by its MODEL only (StandInModel) - a visual, never the explosion itself.
	// ⛔ PAID FOR 2026-10-02: a runtime copy of the explosion with the harm taken out (IFormFactory::Create) crashed the
	// game a few seconds after every load (0xc0000409 in ucrtbase, no crash log) - no forms are made at run time.
	void FindStandInBursts()
	{
		gStandIn = {};
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return;
		}
		std::array<float, Core::kElements> best{};
		std::array<bool, Core::kElements>  vanilla{};
		for (auto* s : data->GetFormArray<RE::SpellItem>()) {
			if (!s || s->GetSpellType() != RE::MagicSystem::SpellType::kSpell || s->GetDelivery() != RE::MagicSystem::Delivery::kAimed) {
				continue;
			}
			const auto* effect = MainEffect(s);
			const auto* proj = effect ? effect->data.projectileBase : nullptr;
			if (!proj || !proj->data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kExplosion) || !proj->data.explosionType ||
				!Hostile(s, effect)) {
				continue;
			}
			const auto e = static_cast<std::size_t>(ElementOf(effect, false, false));
			if (e >= Core::kElements) {
				continue;
			}
			const bool  fromGame = (s->GetFormID() >> 24) == 0;
			const float cost = CostOf(s);
			if ((fromGame && !vanilla[e]) || (fromGame == vanilla[e] && cost > best[e])) {
				gStandIn[e] = proj->data.explosionType;
				best[e] = cost;
				vanilla[e] = fromGame;
			}
		}
		for (std::size_t e = 0; e < Core::kElements; ++e) {
			const auto* src = gStandIn[e];
			SKSE::log::info("stand-in burst for {}: {}", Core::ElementName(static_cast<Core::Element>(e)),
				src ? std::format("{:08X} ({}){}", src->GetFormID(), src->GetModel() ? src->GetModel() : "",
						  SafeExplosion(src) ? "" : ", its model only (the explosion carries harm)") :
					  std::string("none"));
		}
	}

	namespace
	{
		[[nodiscard]] std::string ArtIfThere(std::string_view a_file)
		{
			const auto                   name = std::format("ArcaneOverdrive\\{}.nif", a_file);
			RE::BSResourceNiBinaryStream stream("meshes\\" + name);
			return stream.good() ? name : std::string();
		}
	}

	// Clash art: `meshes\ArcaneOverdrive\Clash<Look>.nif` (loose or in an archive) plays where two projectiles meet, on top of
	// the burst, and `ClashPair<Pair>.nif` where a special pair meets; a mesh that is not there is simply not shown
	void FindClashArt()
	{
		for (std::size_t l = 0; l < Core::kLooks; ++l) {
			const auto look = Core::LookName(static_cast<Core::Look>(l));
			gClashArt[l] = ArtIfThere(std::format("Clash{}", look));
			if (!gClashArt[l].empty()) {
				SKSE::log::info("clash art for {}: meshes\\{}", look, gClashArt[l]);
			}
		}
		gPairArt.clear();
		for (const std::string_view pair : { "Steam", "Plasma", "ShatterStorm", "Eclipse", "Resonance" }) {
			if (auto art = ArtIfThere(std::format("ClashPair{}", pair)); !art.empty()) {
				SKSE::log::info("pair art for {}: meshes\\{}", pair, art);
				gPairArt.emplace(std::string(pair), std::move(art));
			}
		}
	}

	const char* ClashArt(Core::Look a_look)
	{
		const auto l = static_cast<std::size_t>(a_look);
		return l < gClashArt.size() && !gClashArt[l].empty() ? gClashArt[l].c_str() : nullptr;
	}

	const char* PairArt(std::string_view a_pair)
	{
		const auto it = a_pair.empty() ? gPairArt.end() : gPairArt.find(std::string(a_pair));
		return it != gPairArt.end() ? it->second.c_str() : nullptr;
	}

	RE::BGSExplosion* StandInBurst(Core::Element a_element)
	{
		const auto e = static_cast<std::size_t>(a_element);
		return e < gStandIn.size() && SafeExplosion(gStandIn[e]) ? gStandIn[e] : nullptr;
	}

	const char* StandInModel(Core::Element a_element)
	{
		const auto e = static_cast<std::size_t>(a_element);
		if (e >= gStandIn.size() || !gStandIn[e] || SafeExplosion(gStandIn[e])) {
			return nullptr;
		}
		const char* m = gStandIn[e]->GetModel();
		return m && *m ? m : nullptr;
	}

	// the cheapest of Skyrim.esm's own aimed Illusion fear spells (Fear itself), for "Winning scares weaker enemies"
	void FindFearSpell()
	{
		gFear = nullptr;
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return;
		}
		float best = 0.0f;
		for (auto* s : data->GetFormArray<RE::SpellItem>()) {
			if (!s || s->GetSpellType() != RE::MagicSystem::SpellType::kSpell ||
				s->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget || s->GetDelivery() != RE::MagicSystem::Delivery::kAimed ||
				s->GetAssociatedSkill() != RE::ActorValue::kIllusion) {
				continue;
			}
			const auto* file = s->GetFile(0);
			if (!file || _stricmp(std::string(file->GetFilename()).c_str(), "Skyrim.esm") != 0) {
				continue;
			}
			const auto* top = s->GetCostliestEffectItem();
			if (!top || !top->baseEffect || top->baseEffect->data.archetype != RE::EffectArchetypes::ArchetypeID::kDemoralize) {
				continue;
			}
			const float cost = CostOf(s);
			if (!gFear || cost < best) {
				gFear = s;
				best = cost;
			}
		}
		if (gFear) {
			SKSE::log::info("struggles: the fear spell for Intimidate is {:08X} {}", gFear->GetFormID(), gFear->GetName());
		} else {
			SKSE::log::info("struggles: no fear spell found in Skyrim.esm, so Intimidate is unavailable");
		}
	}

	RE::SpellItem* FearSpell() { return gFear; }

	void Survey()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return;
		}
		std::size_t total = 0, withExplosion = 0, safe = 0;
		std::size_t byType[7]{};
		for (const auto* p : data->GetFormArray<RE::BGSProjectile>()) {
			if (!p) {
				continue;
			}
			++total;
			for (std::size_t bit = 0; bit < 7; ++bit) {
				if ((p->data.types.underlying() & (1u << bit)) != 0) {
					++byType[bit];
				}
			}
			if (p->data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kExplosion) && p->data.explosionType) {
				++withExplosion;
				safe += SafeExplosion(p->data.explosionType) ? 1 : 0;
			}
		}
		SKSE::log::info("{} projectile records: {} missile, {} lobber, {} beam, {} spray, {} cone, {} wall, {} arrow; {} explode, {} of those only show",
			total, byType[0], byType[1], byType[2], byType[3], byType[4], byType[5], byType[6], withExplosion, safe);
	}
}

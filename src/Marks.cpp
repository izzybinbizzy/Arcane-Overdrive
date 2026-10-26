// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The scorch and frost marks (Marks.h has his words and the numbers). Once a frame, on the main thread (Meters.cpp's Frame):
//   - WHERE: while the player's meter is Overheating or in Overdrive, every impact of the player's own fire or frost spell
//     projectile leaves a mark at its point - read off each projectile's own impact list (no hook): an impact not seen
//     before is a new one. A mark near a live one of the same element refreshes it; the pool is capped, the oldest ends first.
//   - THE LOOK: the game's own temporary effect at the point, for the mark's lifetime, with a vanilla hazard's model - the
//     small burning ground of the fire hazards (Effects\FXFire02New.nif), the ice of the frost hazard (Magic\IceHazard01.nif).
//     Nothing new is made - no form, no plugin file.
//   - THE HURT (his answer 2026-10-18, "Marks hurt", on by default): every tick each mark hurts every actor in its reach who
//     is not the player, not on the player's side (Meters.cpp's rule), not the player's summon and not dead - its fire or frost
//     resistance taken off.

#include "Marks.h"
#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char* kModels[3]{ "", "Effects\\FXFire02New.nif", "Magic\\IceHazard01.nif" };

		Marks::Pool                                                        gPool;
		std::unordered_map<std::uint32_t, RE::NiPointer<RE::BSTempEffect>> gLooks;  // a mark's id -> its temporary effect
		// the impacts listed on the player's projectiles LAST frame: one not among them is new. Rebuilt every frame from the
		// live lists, so a freed impact's address used again later is new again
		std::unordered_set<const void*> gSeen;
		std::atomic<std::uint32_t>      gMade{ 0 }, gHurt{ 0 }, gAlive{ 0 };

		Marks::Rules RulesOf(const Settings& a_s)
		{
			Marks::Rules r;
			r.max = static_cast<std::size_t>(std::clamp(a_s.markMax, 1, 20));
			r.life = std::clamp(a_s.markSeconds, 2.0f, 30.0f);
			return r;
		}

		Marks::Point PointOf(const RE::NiPoint3& a_p) { return { a_p.x, a_p.y, a_p.z }; }

		// where a reference is, as one number: its interior cell, or the worldspace of its exterior (walking from one exterior
		// cell into the next keeps a mark; a load door or fast travel does not). 0: not placed anywhere yet
		std::uint64_t PlaceOf(const RE::TESObjectREFR* a_ref)
		{
			const auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
			if (!cell) {
				return 0;
			}
			if (cell->IsInteriorCell()) {
				return (1ull << 32) | cell->GetFormID();
			}
			const auto* world = a_ref->GetWorldspace();
			return world ? (2ull << 32) | world->GetFormID() : (1ull << 32) | cell->GetFormID();
		}

		void EndLook(std::uint32_t a_id)
		{
			if (const auto it = gLooks.find(a_id); it != gLooks.end()) {
				if (it->second) {
					it->second->lifetime = it->second->age;  // ends on the game's next update of it
				}
				gLooks.erase(it);
			}
		}

		void Show(const Marks::Added& a_added, const RE::NiPoint3& a_at, int a_element, float a_life, RE::TESObjectCELL* a_cell)
		{
			if (a_added.ended) {
				EndLook(a_added.ended);
			}
			if (!a_added.fresh) {  // a live mark refreshed: its look lasts a whole life again
				if (const auto it = gLooks.find(a_added.id); it != gLooks.end() && it->second) {
					it->second->lifetime = it->second->age + a_life;
				}
				return;
			}
			++gMade;
			if (!a_cell) {
				return;
			}
			const RE::NiPoint3 upright{ 0.0f, 0.0f, 0.0f };
			if (auto* look = RE::BSTempEffectParticle::Spawn(a_cell, a_life, kModels[a_element], upright, a_at, 1.0f, 0, nullptr)) {
				gLooks[a_added.id] = RE::NiPointer<RE::BSTempEffect>(look);
			}
		}

		// the impacts of the player's own fire / frost spell projectiles, not seen before
		void Capture(RE::PlayerCharacter* a_player, const Marks::Rules& a_r)
		{
			auto* mgr = RE::Projectile::Manager::GetSingleton();
			if (!mgr) {
				return;
			}
			std::vector<RE::ProjectileHandle> handles;
			{
				RE::BSSpinLockGuard guard(mgr->projectileLock);
				for (const auto* list : { &mgr->unlimited, &mgr->limited, &mgr->pending }) {
					for (const auto& h : *list) {
						handles.push_back(h);
					}
				}
			}
			std::unordered_set<const void*> now;
			for (auto& h : handles) {
				auto proj = h.get();
				if (!proj) {
					continue;
				}
				auto&      rd = proj->GetProjectileRuntimeData();
				const auto shooter = rd.shooter.get();
				if (shooter.get() != a_player || !rd.spell) {
					continue;
				}
				const int element = Read(rd.spell).element;
				if (element != 1 && element != 2) {
					continue;
				}
				for (const auto* impact : rd.impacts) {
					if (!impact || !now.insert(impact).second || gSeen.contains(impact)) {
						continue;
					}
					// a spell that struck someone hit them at chest height: the mark goes on the ground at their feet, where it
					// burns and where its reach finds them
					auto at = impact->desiredTargetLoc;
					if (const auto hit = impact->collidee.get(); hit && hit->As<RE::Actor>()) {
						at.z = hit->GetPositionZ();
					}
					const auto added = gPool.Add(PointOf(at), element, a_r, PlaceOf(proj.get()));
					if (added.id) {
						Show(added, at, element, a_r.life, proj->GetParentCell() ? proj->GetParentCell() : a_player->GetParentCell());
					}
				}
			}
			gSeen = std::move(now);
		}

		// an actor a mark may hurt: not the player, not on the player's side, not the player's summon, alive
		bool Hurtable(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (!a_actor || a_actor == a_player || a_actor->IsDead() || a_actor->IsPlayerTeammate()) {
				return false;
			}
			if (a_actor->IsCommandedActor()) {
				const auto boss = a_actor->GetCommandingActor();
				if (boss.get() == a_player) {
					return false;
				}
			}
			return true;
		}

		void HurtAround(const std::vector<std::uint32_t>& a_ticked, const Marks::Rules& a_r, const Settings& a_s, bool a_overdrive,
			RE::PlayerCharacter* a_player)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists || a_ticked.empty()) {
				return;
			}
			std::vector<RE::NiPointer<RE::Actor>> actors;
			for (auto& h : lists->highActorHandles) {
				if (auto a = h.get(); a && Hurtable(a.get(), a_player)) {
					actors.push_back(std::move(a));
				}
			}
			for (const auto id : a_ticked) {
				const auto* mark = gPool.Find(id);
				if (!mark) {
					continue;
				}
				const auto resistAv = mark->element == 1 ? RE::ActorValue::kResistFire : RE::ActorValue::kResistFrost;
				for (auto& a : actors) {
					if (!Marks::Inside(mark->at, PointOf(a->GetPosition()), a_r.radius)) {
						continue;
					}
					auto*       av = a->AsActorValueOwner();
					const float hurt = Marks::Hurt(a_s.markHurtPerSecond, a_r.tick, a_overdrive, av->GetActorValue(resistAv));
					if (hurt > 0.0f) {
						av->DamageActorValue(RE::ActorValue::kHealth, hurt);
						++gHurt;
					}
				}
			}
		}
	}

	void UpdateMarks(const MeterView& a_v, const Settings& a_s, float a_dt)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !a_s.enabled || !a_s.marks) {
			ReleaseMarks();
			return;
		}
		const auto r = RulesOf(a_s);
		const bool overdrive = a_v.state == Heat::Meter::State::kOverdrive;
		if (overdrive || (a_v.state == Heat::Meter::State::kBuilding && a_v.stage >= Heat::Stage::kOverheating)) {
			Capture(player, r);
		} else {
			gSeen.clear();  // below Overheating nothing is captured; an impact still listed when it climbs back counts as new
		}
		std::vector<std::uint32_t> ended, ticked;
		// a load door or fast travel is no game load: the marks left in another place end here, before any of them can hurt
		// an actor of this place standing at the same coordinates
		if (const auto here = PlaceOf(player)) {
			std::vector<std::uint32_t> elsewhere;
			gPool.DropElsewhere(here, elsewhere);
			for (const auto id : elsewhere) {
				EndLook(id);
			}
		}
		gPool.Advance(a_dt, r, ended, ticked);
		for (const auto id : ended) {
			gLooks.erase(id);  // its look ends on its own (same lifetime)
		}
		if (a_s.markHurt) {
			HurtAround(ticked, r, a_s, overdrive, player);
		}
		gAlive = static_cast<std::uint32_t>(gPool.All().size());
	}

	void ReleaseMarks()
	{
		for (const auto& m : gPool.All()) {
			EndLook(m.id);
		}
		gLooks.clear();
		gPool.Clear();
		gSeen.clear();
		gAlive = 0;
	}

	MarksView MarksNow() { return { gAlive.load(), gMade.load(), gHurt.load() }; }
}

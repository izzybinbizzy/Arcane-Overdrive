// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The meters. The player has one; with "Enemies build heat" on (off by default), every other actor that casts a spell
// gets one too, and its spells get the same bonus and penalties (no bar or hand lights for them).
//
//   a cast         the game's spell cast event (main.cpp), queued; counted on the next frame. Held spells are not
//                  counted there: the per-frame pass adds their heat for each frame a hand is casting one.
//   every frame    held spells, the Overdrive key, the drain and the clocks (Heat::Meter::Tick), the slow magicka, the
//                  hand lights (Hands.cpp), and what the bar and the Debug page show.
//   the bonus      Multiplier, asked by the effect hook in main.cpp as each effect lands on a target, on whatever thread
//                  the game applies it: everything here is behind one lock.
//
// Slow magicka during the cooldown: part of each frame's natural regeneration is taken back (Heat::TakeBack). Nothing on
// the actor is changed for longer than that frame - no modifier on its magicka rate - so a crash in the middle of a
// cooldown leaves nothing behind, and a load or a restart has nothing to undo; the meter itself is cleared on load.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr float kForgetEnemy = 30.0f;  // seconds an enemy's meter may sit idle before it is forgotten

		struct Track
		{
			Heat::Meter     meter;
			float           lastMagicka{ -1.0f };
			float           idle{ 0.0f };
			RE::ActorHandle actor;  // an enemy's (the player's is not needed)
		};

		struct Cast
		{
			RE::ActorHandle      actor;
			const RE::MagicItem* item{ nullptr };
		};

		std::mutex                               gLock;  // everything below, up to the view
		Track                                    gPlayer;
		std::unordered_map<std::uint32_t, Track> gEnemies;  // by the actor's handle
		std::vector<Cast>                        gCasts;
		std::optional<SpellLook::Look>           gLastLook;              // the HUD look of the player's last spell that built heat
		const RE::MagicItem*                     gLastSpell{ nullptr };  // and that spell (its own art feeds the spell bar)

		std::atomic<bool>          gKey{ false };
		std::atomic<int>           gBenchStage{ -1 };
		std::atomic<float>         gBenchHeat{ -1.0f };
		std::atomic<std::uint64_t> gBoosted{ 0 };

		std::mutex gViewLock;
		MeterView  gView;

		bool Counts(const SpellInfo& a_info, const Settings& a_s) { return Heat::BuildsHeat(a_info.facts, a_s.counts); }

		// an actor whose spells build heat: the player, and with Enemies on, anyone else who is not on the player's side
		bool Heats(RE::Actor* a_actor, const Settings& a_s)
		{
			if (!a_actor) {
				return false;
			}
			if (a_actor->IsPlayerRef()) {
				return true;
			}
			return a_s.enemies && !a_actor->IsPlayerTeammate() && !a_actor->IsDead();
		}

		Track* TrackOf(RE::Actor* a_actor, const Settings& a_s, bool a_make)
		{
			if (!a_actor) {
				return nullptr;
			}
			if (a_actor->IsPlayerRef()) {
				return &gPlayer;
			}
			if (!a_s.enemies) {
				return nullptr;
			}
			const auto key = a_actor->GetHandle().native_handle();
			if (const auto it = gEnemies.find(key); it != gEnemies.end()) {
				return &it->second;
			}
			if (!a_make || !Heats(a_actor, a_s)) {
				return nullptr;
			}
			auto& track = gEnemies[key];
			track.actor = a_actor->GetHandle();
			return &track;
		}

		float CostOf(const RE::MagicItem* a_item, RE::Actor* a_actor)
		{
			const float c = a_item->CalculateMagickaCost(a_actor);
			return std::isfinite(c) ? std::max(c, 0.0f) : 0.0f;
		}

		// the hands' held spells this frame
		void Held(RE::Actor* a_actor, Track& a_track, const Settings& a_s, float a_delta)
		{
			for (std::size_t i = 0; i < 2; ++i) {  // the left hand, the right hand
				auto* caster = a_actor->GetActorRuntimeData().magicCasters[i];
				if (!caster || !caster->currentSpell || caster->state.get() != RE::MagicCaster::State::kCasting) {
					continue;
				}
				const auto info = Read(caster->currentSpell);
				if (info.held && Counts(info, a_s)) {
					a_track.meter.Hold(a_s.tuning, info.school, CostOf(caster->currentSpell, a_actor), a_delta);
				}
			}
		}

		// slow magicka: take back part of this frame's regeneration
		void Regen(RE::Actor* a_actor, Track& a_track, const Settings& a_s, float a_delta)
		{
			auto*       owner = a_actor->AsActorValueOwner();
			const float now = owner->GetActorValue(RE::ActorValue::kMagicka);
			const float regen = a_track.meter.MagickaRegen(a_s.tuning);
			if (regen < 1.0f && a_track.lastMagicka >= 0.0f && a_delta > 0.0f) {
				// what natural regeneration could give in this frame, with room to spare (a quarter of the pool a second);
				// a bigger rise is a potion or a spell, and is left alone
				const float pool = std::max(owner->GetPermanentActorValue(RE::ActorValue::kMagicka), 1.0f);
				const float back = Heat::TakeBack(a_track.lastMagicka, now, pool * 0.25f * a_delta + 0.5f, regen);
				if (back > 0.0f) {
					owner->DamageActorValue(RE::ActorValue::kMagicka, back);
					a_track.lastMagicka = now - back;
					return;
				}
			}
			a_track.lastMagicka = now;
		}

		// the stage a hand's spell is judged by: its school's (a meter per school), else the shared meter's
		Heat::Stage HandStage(RE::PlayerCharacter* a_player, bool a_left, const Settings& a_s)
		{
			const auto* item = a_player->GetEquippedObject(a_left);
			const auto* spell = item ? item->As<RE::MagicItem>() : nullptr;
			const auto  school = spell ? Read(spell).school : gPlayer.meter.Last();
			return gPlayer.meter.StageOf(a_s.tuning, school);
		}
	}

	void OnCast(RE::Actor* a_caster, const RE::MagicItem* a_item)
	{
		if (!a_caster || !a_item) {
			return;
		}
		std::lock_guard lock(gLock);
		if (gCasts.size() < 256) {
			gCasts.push_back({ a_caster->GetHandle(), a_item });
		}
	}

	void PressOverdriveKey() { gKey = true; }
	void BenchSetHeat(float a_heat) { gBenchHeat = std::isfinite(a_heat) ? std::clamp(a_heat, 0.0f, Heat::kFull) : 0.0f; }
	void BenchForceStage(Heat::Stage a_stage) { gBenchStage = static_cast<int>(a_stage); }

	void Frame(float a_delta)
	{
		const auto  s = Config();
		const float dt = std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, 0.25f) : 0.0f;
		auto*       player = RE::PlayerCharacter::GetSingleton();
		if (!s.enabled || !player) {
			ResetAll();
			ReleaseHands();
			ReleaseOverload();
			ReleaseMarks();
			std::lock_guard lock(gViewLock);
			gView = {};
			return;
		}
		Heat::Stage left = Heat::Stage::kCold, right = Heat::Stage::kCold;
		MeterView   view;
		bool        lookKnown = false;
		{
			std::lock_guard   lock(gLock);
			std::vector<Cast> casts;
			casts.swap(gCasts);
			for (const auto& c : casts) {
				const auto actorPtr = c.actor.get();
				auto*      actor = actorPtr.get();
				const auto info = Read(c.item);
				if (info.held || !Counts(info, s)) {
					continue;
				}
				if (auto* track = TrackOf(actor, s, true)) {
					track->meter.Cast(s.tuning, info.school, CostOf(c.item, actor));
					if (track == &gPlayer) {
						gLastLook = LookOf(c.item);
						gLastSpell = c.item;
					}
					if (s.debugLog && track == &gPlayer) {
						SKSE::log::info("cast {} ({}): heat {:.0f}, {}", c.item->GetName(), Heat::SchoolName(info.school),
							gPlayer.meter.Shown(s.tuning), Heat::StageName(gPlayer.meter.ShownStage(s.tuning)));
					}
				}
			}
			if (gKey.exchange(false)) {
				auto* ui = RE::UI::GetSingleton();
				if (ui && !ui->GameIsPaused() && gPlayer.meter.PressKey(s.tuning) && s.debugLog) {
					SKSE::log::info("Overdrive: the key");
				}
			}
			if (const int stage = gBenchStage.exchange(-1); stage >= 0) {
				gPlayer.meter.ForceStage(s.tuning, static_cast<Heat::Stage>(std::clamp(stage, 0, 4)));
			}
			if (const float heat = gBenchHeat.exchange(-1.0f); heat >= 0.0f) {
				gPlayer.meter.SetHeat(s.tuning, heat);
			}

			Held(player, gPlayer, s, dt);
			if (const auto* held = HeldSpell(player, s)) {
				gLastLook = LookOf(held);
				gLastSpell = held;
			}
			gPlayer.meter.Tick(s.tuning, dt);
			Regen(player, gPlayer, s, dt);
			for (auto it = gEnemies.begin(); it != gEnemies.end();) {
				const auto actorPtr = it->second.actor.get();
				auto*      actor = actorPtr.get();
				auto&      track = it->second;
				if (actor) {
					Held(actor, track, s, dt);
					track.meter.Tick(s.tuning, dt);
					Regen(actor, track, s, dt);
				}
				track.idle = track.meter.Idle() ? track.idle + dt : 0.0f;
				if (!actor || !s.enemies || actor->IsDead() || track.idle > kForgetEnemy) {
					it = gEnemies.erase(it);
				} else {
					++it;
				}
			}

			left = HandStage(player, true, s);
			right = HandStage(player, false, s);
			const auto& m = gPlayer.meter;
			view.heat = m.Shown(s.tuning);
			view.stage = m.ShownStage(s.tuning);
			view.state = m.Now();
			view.timer = m.Timer();
			view.timerOf = m.Now() == Heat::Meter::State::kOverdrive ? s.tuning.burst : std::max(s.tuning.cooldown, 0.1f);
			view.ready = m.Ready(s.tuning);
			view.last = m.Last();
			view.bonus = m.Bonus(s.tuning, m.Last());
			view.regen = m.MagickaRegen(s.tuning);
			view.enemies = gEnemies.size();
			view.look = gLastLook.value_or(SpellLook::Look::kGeneric);
			view.spell = gLastSpell;
			lookKnown = gLastLook.has_value();
		}
		if (!lookKnown) {  // nothing has built heat yet: the spell in hand (read outside the lock, as the tint below)
			view.spell = EquippedSpell(player);
			view.look = LookOf(view.spell);
		}
		view.boosted = gBoosted.load();
		// the flames' tint: the element of the spell in the right hand, else the left (none: the heat's own colors)
		for (const bool leftHand : { false, true }) {
			const auto* item = player->GetEquippedObject(leftHand);
			if (const auto* spell = item ? item->As<RE::MagicItem>() : nullptr; spell && Read(spell).element != 0) {
				view.element = Read(spell).element;
				break;
			}
		}
		NoteSpellArt(view.spell);     // the spell's own textures and color for the spell bar (read once per spell)
		UpdateOverload(view, s, dt);  // the spells grow in Overdrive and sputter in the cooldown - before the hand lights read it
		UpdateHands(left, right, s);
		UpdateMarks(view, s, dt);  // Overheating fire / frost spells leave scorch / frost marks that hurt (his (b), 2026-10-18)
		std::lock_guard lock(gViewLock);
		gView = view;
	}

	float Multiplier(RE::Actor* a_caster, const RE::MagicItem* a_item, const RE::EffectSetting* a_effect)
	{
		if (!a_caster || !a_item || !a_effect) {
			return 1.0f;
		}
		const auto s = Config();
		if (!s.enabled || !Heats(a_caster, s) || !Boosted(a_effect, s.boost)) {
			return 1.0f;
		}
		const auto info = Read(a_item);
		using K = Heat::SpellFacts::Kind;
		const bool spell = info.facts.kind == K::kSpell || ((info.facts.kind == K::kStaff || info.facts.kind == K::kScroll) && s.counts.stavesAndScrolls);
		if (!spell) {
			return 1.0f;
		}
		std::lock_guard lock(gLock);
		const auto*     track = TrackOf(a_caster, s, false);
		const float     k = track ? track->meter.Bonus(s.tuning, info.school) : 1.0f;
		if (k != 1.0f) {
			++gBoosted;
		}
		return k;
	}

	void ResetAll()
	{
		std::lock_guard lock(gLock);
		gPlayer = {};
		gEnemies.clear();
		gCasts.clear();
		gLastLook.reset();
		gLastSpell = nullptr;
		gKey = false;
	}

	MeterView PlayerView()
	{
		std::lock_guard lock(gViewLock);
		return gView;
	}
}

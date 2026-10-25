// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The bars (Heat.h). The player has a pair; with "Enemies build heat" on (off by default), every other actor that casts a
// spell gets a pair too, and its spells get the same bonus and penalties (no bars or hand lights for them; their
// Overdrive goes by itself when the gate lets it - an enemy has no key).
//
//   a cast         the game's spell cast event (main.cpp), queued with its element and whether it was dual cast; counted
//                  on the next frame. Held spells are not counted there: the per-frame pass adds their charge and heat for
//                  each frame a hand is casting one.
//   every frame    held spells, the Overdrive key (a press, and holding it for the focus gate), the cooling, the drain and
//                  the clocks (Heat::Meter::Tick), the slow magicka, a misfire's backlash, the hand lights (Hands.cpp), and
//                  what the bars and the Debug page show.
//   the bonus      Multiplier, asked by the effect hook in main.cpp as each effect lands on a target, on whatever thread
//                  the game applies it: everything here is behind one lock.
//
// Slow magicka during the cooldown: part of each frame's natural regeneration is taken back (Heat::TakeBack). Nothing on
// the actor is changed for longer than that frame - no modifier on its magicka rate - so a crash in the middle of a
// cooldown leaves nothing behind, and a load or a restart has nothing to undo; the bars themselves are cleared on load.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr float kForgetEnemy = 30.0f;  // seconds an enemy's bars may sit idle before they are forgotten
		constexpr float kMisfireFlash = 0.6f;  // seconds a misfire's sputter lasts
		constexpr float kStagger = 0.4f;       // a backlash's stagger, the game's own stagger graph value

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
			bool                 dual{ false };
		};

		std::mutex                               gLock;  // everything below, up to the view
		Track                                    gPlayer;
		std::unordered_map<std::uint32_t, Track> gEnemies;  // by the actor's handle
		std::vector<Cast>                        gCasts;
		float                                    gMisfire{ 0.0f };  // a misfire's flash left, seconds

		std::atomic<bool>          gKey{ false };
		std::atomic<bool>          gKeyHeld{ false };
		std::atomic<int>           gBenchStage{ -1 };
		std::atomic<float>         gBenchHeat{ -1.0f };
		std::atomic<float>         gBenchCharge{ -1.0f };
		std::atomic<std::uint64_t> gBoosted{ 0 };

		std::mutex gViewLock;
		MeterView  gView;

		// each frame's charges for TrueHUD, which asks on its own thread for every bar it draws: read without the meters' lock
		std::atomic<float>                       gPlayerCharge{ 0.0f };
		std::mutex                               gChargeLock;
		std::unordered_map<std::uint32_t, float> gEnemyCharges;  // by the actor's handle

		void PublishCharges(float a_player, std::unordered_map<std::uint32_t, float>&& a_enemies)
		{
			gPlayerCharge = a_player;
			std::lock_guard lock(gChargeLock);
			gEnemyCharges = std::move(a_enemies);
		}

		bool Counts(const SpellInfo& a_info, const Settings& a_s) { return Heat::BuildsHeat(a_info.facts, a_s.counts); }

		// an enemy's bars: no key, so Overdrive goes by itself when its gate lets it (the focus gate, which needs a key or
		// two charged hands, is the sweet spot for them)
		Heat::Tuning EnemyTuning(Heat::Tuning a_t)
		{
			if (a_t.mode == Heat::OverdriveMode::kKey) {
				a_t.mode = Heat::OverdriveMode::kAutomatic;
			}
			if (a_t.gate == Heat::Gate::kFocus) {
				a_t.gate = Heat::Gate::kSweetSpot;
			}
			return a_t;
		}

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

		RE::MagicCaster* HandCaster(RE::Actor* a_actor, std::size_t a_hand) { return a_actor->GetActorRuntimeData().magicCasters[a_hand]; }

		// the hands' held spells this frame
		void Held(RE::Actor* a_actor, Track& a_track, const Heat::Tuning& a_t, const Settings& a_s, float a_delta)
		{
			for (std::size_t i = 0; i < 2; ++i) {  // the left hand, the right hand
				auto* caster = HandCaster(a_actor, i);
				if (!caster || !caster->currentSpell || caster->state.get() != RE::MagicCaster::State::kCasting) {
					continue;
				}
				const auto info = Read(caster->currentSpell);
				if (info.held && Counts(info, a_s)) {
					a_track.meter.Hold(a_t, info.school, CostOf(caster->currentSpell, a_actor), a_delta, info.element);
				}
			}
		}

		// both hands holding a charged spell: the focus gate's other way in
		bool HandsCharged(RE::Actor* a_actor)
		{
			for (std::size_t i = 0; i < 2; ++i) {
				const auto* caster = HandCaster(a_actor, i);
				const auto  state = caster ? caster->state.get() : RE::MagicCaster::State::kNone;
				if (!caster || !caster->currentSpell || (state != RE::MagicCaster::State::kReady && state != RE::MagicCaster::State::kCharging)) {
					return false;
				}
			}
			return true;
		}

		// slow magicka: take back part of this frame's regeneration
		void Regen(RE::Actor* a_actor, Track& a_track, const Heat::Tuning& a_t, float a_delta)
		{
			auto*       owner = a_actor->AsActorValueOwner();
			const float now = owner->GetActorValue(RE::ActorValue::kMagicka);
			const float regen = a_track.meter.MagickaRegen(a_t);
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

		// a misfire with Backlash on: a stagger, and a bite out of the magicka pool
		void Backlash(RE::PlayerCharacter* a_player, const Heat::Tuning& a_t, bool a_log)
		{
			auto*       owner = a_player->AsActorValueOwner();
			const float pool = std::max(owner->GetPermanentActorValue(RE::ActorValue::kMagicka), 0.0f);
			const float bite = std::min(pool * std::clamp(a_t.backlashMagicka, 0.0f, 0.5f), owner->GetActorValue(RE::ActorValue::kMagicka));
			if (bite > 0.0f) {
				owner->DamageActorValue(RE::ActorValue::kMagicka, bite);
			}
			a_player->SetGraphVariableFloat("staggerMagnitude", kStagger);
			a_player->NotifyAnimationGraph("staggerStart");
			if (a_log) {
				SKSE::log::info("misfire backlash: a stagger, {:.0f} magicka", bite);
			}
		}
	}

	void OnCast(RE::Actor* a_caster, const RE::MagicItem* a_item)
	{
		if (!a_caster || !a_item) {
			return;
		}
		bool dual = false;
		for (std::size_t i = 0; i < 2; ++i) {
			const auto* caster = HandCaster(a_caster, i);
			dual = dual || (caster && caster->GetIsDualCasting());
		}
		std::lock_guard lock(gLock);
		if (gCasts.size() < 256) {
			gCasts.push_back({ a_caster->GetHandle(), a_item, dual });
		}
	}

	void PressOverdriveKey() { gKey = true; }
	void HoldOverdriveKey(bool a_down) { gKeyHeld = a_down; }
	void BenchSetCharge(float a_charge) { gBenchCharge = std::isfinite(a_charge) ? std::clamp(a_charge, 0.0f, Heat::kFull) : 0.0f; }
	void BenchSetHeat(float a_heat) { gBenchHeat = std::isfinite(a_heat) ? std::clamp(a_heat, 0.0f, Heat::kFull) : 0.0f; }
	void BenchForceStage(Heat::Stage a_stage) { gBenchStage = static_cast<int>(a_stage); }

	namespace
	{
		std::atomic<float> gFrameUsAvg{ 0.0f }, gFrameUsMax{ 0.0f };

		// Frame's own time, taken whichever way it returns
		struct FrameTimer
		{
			std::chrono::steady_clock::time_point start{ std::chrono::steady_clock::now() };
			FrameTimer() = default;
			FrameTimer(const FrameTimer&) = delete;
			FrameTimer& operator=(const FrameTimer&) = delete;
			~FrameTimer()
			{
				const float us = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count();
				const float avg = gFrameUsAvg.load(std::memory_order_relaxed);
				gFrameUsAvg.store(avg <= 0.0f ? us : avg + (us - avg) * 0.05f, std::memory_order_relaxed);  // about the last 20 frames
				gFrameUsMax.store(std::max(gFrameUsMax.load(std::memory_order_relaxed), us), std::memory_order_relaxed);
			}
		};
	}

	FrameCost FrameCostNow() { return { gFrameUsAvg.load(), gFrameUsMax.load() }; }

	void ResetFrameCost()
	{
		gFrameUsAvg = 0.0f;
		gFrameUsMax = 0.0f;
	}

	void Frame(float a_delta)
	{
		const FrameTimer timer;
		NewReadFrame();
		const auto  s = Config();
		const float dt = std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, 0.25f) : 0.0f;
		auto*       player = RE::PlayerCharacter::GetSingleton();
		if (!s.enabled || !player) {
			ResetAll();
			ReleaseHands();
			ReleaseOverload();
			ReleaseMarks();
			UpdateTrueHud({}, s);  // switched off: TrueHUD's special bar is given back
			PublishCharges(0.0f, {});
			std::lock_guard lock(gViewLock);
			gView = {};
			return;
		}
		const auto& t = s.tuning;
		const auto  enemyTuning = EnemyTuning(t);
		Heat::Stage stage = Heat::Stage::kCold;
		MeterView   view;
		bool        backlash = false;
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
					track->meter.Cast(track == &gPlayer ? t : enemyTuning, info.school, CostOf(c.item, actor), info.element, c.dual);
					if (s.debugLog && track == &gPlayer) {
						SKSE::log::info("cast {} ({}{}): charge {:.0f}, heat {:.0f}, {}", c.item->GetName(), Heat::SchoolName(info.school), c.dual ? ", dual" : "",
							gPlayer.meter.Charge(t), gPlayer.meter.HeatNow(), Heat::StageName(gPlayer.meter.StageNow(t)));
					}
				}
			}
			if (gKey.exchange(false)) {
				auto* ui = RE::UI::GetSingleton();
				if (ui && !ui->GameIsPaused()) {
					const auto r = gPlayer.meter.PressKey(t);
					if (r == Heat::Release::kMisfire) {
						gMisfire = kMisfireFlash;
						backlash = t.backlash;
					}
					if (s.debugLog) {
						static constexpr const char* kSaid[]{ "Overdrive", "not ready", "too cold", "too hot", "waiting to cool", "MISFIRE", "hold to focus" };
						SKSE::log::info("the Overdrive key: {} (heat {:.0f})", kSaid[static_cast<int>(r)], gPlayer.meter.HeatNow());
					}
				}
			}
			if (const int st = gBenchStage.exchange(-1); st >= 0) {
				gPlayer.meter.ForceStage(t, static_cast<Heat::Stage>(std::clamp(st, 0, 4)));
			}
			if (const float charge = gBenchCharge.exchange(-1.0f); charge >= 0.0f) {
				gPlayer.meter.SetCharge(t, charge);
			}
			if (const float heat = gBenchHeat.exchange(-1.0f); heat >= 0.0f) {
				gPlayer.meter.SetHeat(heat);
			}

			Held(player, gPlayer, t, s, dt);
			const bool focusing = (t.mode == Heat::OverdriveMode::kKey && gKeyHeld.load()) || HandsCharged(player);
			gPlayer.meter.Focus(t, focusing, dt);
			gPlayer.meter.Tick(t, dt);
			Regen(player, gPlayer, t, dt);
			gMisfire = std::max(gMisfire - dt, 0.0f);
			for (auto it = gEnemies.begin(); it != gEnemies.end();) {
				const auto actorPtr = it->second.actor.get();
				auto*      actor = actorPtr.get();
				auto&      track = it->second;
				if (actor) {
					Held(actor, track, enemyTuning, s, dt);
					track.meter.Tick(enemyTuning, dt);
					Regen(actor, track, enemyTuning, dt);
				}
				track.idle = track.meter.Idle() ? track.idle + dt : 0.0f;
				if (!actor || !s.enemies || actor->IsDead() || track.idle > kForgetEnemy) {
					it = gEnemies.erase(it);
				} else {
					++it;
				}
			}

			const auto& m = gPlayer.meter;
			const auto  band = Heat::BandOf(t);
			stage = m.StageNow(t);
			view.charge = m.Charge(t);
			view.heat = m.HeatNow();
			view.stage = stage;
			view.state = m.Now();
			view.timer = m.Timer();
			view.timerOf = m.Now() == Heat::Meter::State::kOverdrive ? t.burst : std::max(t.cooldown, 0.1f);
			view.ready = m.Ready(t);
			view.gate = m.Gated(t);
			view.bandLow = band.low;
			view.redLine = band.red;
			view.redZone = m.RedZone(t);
			view.focus = std::clamp(m.FocusHeld() / std::max(t.focusSeconds, 0.05f), 0.0f, 1.0f);
			view.misfire = gMisfire / kMisfireFlash;
			view.misfires = m.Misfires();
			view.last = m.Last();
			view.bonus = m.Bonus(t);
			view.regen = m.MagickaRegen(t);
			view.enemies = gEnemies.size();
			std::unordered_map<std::uint32_t, float> enemyCharges;
			for (const auto& [key, track] : gEnemies) {
				enemyCharges.emplace(key, track.meter.Charge(enemyTuning));
			}
			PublishCharges(view.charge, std::move(enemyCharges));
		}
		if (backlash) {
			Backlash(player, t, s.debugLog);
		}
		view.boosted = gBoosted.load();
		UpdateOverload(view, s, dt);  // the spells grow with the charge, sputter in the cooldown and at a misfire - before the hand lights read it
		UpdateHands(stage, stage, s);
		UpdateMarks(view, s, dt);  // Overheating fire / frost spells leave scorch / frost marks that hurt
		UpdateTrueHud(view, s);
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
		const float     k = track ? track->meter.Bonus(track == &gPlayer ? s.tuning : EnemyTuning(s.tuning)) : 1.0f;
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
		gMisfire = 0.0f;
		gKey = false;
		gKeyHeld = false;
	}

	MeterView PlayerView()
	{
		std::lock_guard lock(gViewLock);
		return gView;
	}

	float ChargeOf(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0.0f;
		}
		if (a_actor->IsPlayerRef()) {
			return gPlayerCharge.load();
		}
		std::lock_guard lock(gChargeLock);
		const auto      it = gEnemyCharges.find(a_actor->GetHandle().native_handle());
		return it != gEnemyCharges.end() ? it->second : 0.0f;
	}
}

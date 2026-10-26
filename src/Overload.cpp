// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The overcharge on the spells themselves (Overload.h has the numbers). Once a frame, on the main thread (Meters.cpp's
// Frame, before the hand lights):
//   - GROWTH: the casting art in each of the player's hands and every spell projectile of the player's are scaled by the
//     Overdrive bar's fill (Overload::Growth, eased so it never jumps) and their own animations run faster by the ferocity
//     (every time controller under them: the particle emitters, the flicker, the texture scroll). It is the game's own
//     model, made bigger and fiercer - nothing new is drawn. A beam is not scaled (that would stretch it past its target).
//     The player's explosions and hazards keep the size their spells had when they were made (the hooks in main.cpp note
//     each one), and the hit effects the player's spells leave on targets run as fiercely as the spells.
//   - THE RED ZONE: Heat in the red outside Overdrive makes the spells pulse, stutter and blink out (Overload::WildAt),
//     each hand and each projectile on its own, the hand lights blinking with them.
//   - THE FOCUS: as Overdrive fires the spells draw in and swell back (Overload::FocusPull), then hold steady.
//   - BURNOUT: through the cooldown, with Burnout flicker on, the spells sputter out and back (Overload::Shows) and the
//     hand lights dip with them (HandLightFactor, read by Hands.cpp).
//   - OUR OWN EFFECTS (meshes\ArcaneOverdrive\<Focus|Sparks><Fire|Frost|Shock|Arcane>.nif): as Overdrive fires, motes rush
//     into each casting hand and flare there (Focus flare); in the red zone a hand spits a few sparks when its spell swells
//     or blinks out (Sparks in the red). Each is played once where the hand is, as the game plays an impact - no form is
//     made. The look is the hand's spell's element; a model that is not installed is never asked for.
// Every value is written from a remembered base, as the hand lights are (Hands.cpp): a value someone else has written since
// is taken as the new base, and a node or controller no longer ours is put back as it was found.

#include "Plugin.h"
#include "Overload.h"

namespace Plugin
{
	namespace
	{
		constexpr std::uint32_t kKeepFrames = 2;  // a node or controller not seen for this many frames is put back
		constexpr float         kEaseRate = 6.0f;
		constexpr float         kWildEaseRate = 8.0f;
		constexpr float         kWildOutLight = 0.35f;  // a hand light while the red zone blinks its spell out
		constexpr std::size_t   kMostSpawned = 32;
		constexpr std::uint32_t kWait3D = 120;      // frames an explosion or hazard may take to get its model
		constexpr float         kSparkGap = 0.3f;   // a hand spits sparks at most this often, seconds
		constexpr float         kFlareLife = 1.0f;  // how long the game keeps each effect: past its last particle
		constexpr float         kSparkLife = 0.8f;
		constexpr std::uint32_t kSpawnFlags = 7;  // as the clash bursts are played (ClashPass.cpp ModelHere)

		// our effect models: [effect][look], look = SpellInfo::element (0 every other element, 1 fire, 2 frost, 3 shock)
		enum Effect : std::size_t
		{
			kFlare,
			kSparks
		};
		constexpr std::array<const char*, 2>      kEffectNames{ "Focus", "Sparks" };
		constexpr std::array<const char*, 4>      kLookNames{ "Arcane", "Fire", "Frost", "Shock" };
		std::array<std::array<std::string, 4>, 2> gModels{};  // the model's name under meshes\, empty when it is not installed
		bool                                      gModelsRead = false;

		struct Grown
		{
			RE::NiPointer<RE::NiAVObject> node;
			float                         base{ 1.0f }, written{ -1.0f };
			bool                          culled{ false };  // we hid it (the sputter)
			std::uint32_t                 frame{ 0 };
		};

		struct Sped
		{
			RE::NiPointer<RE::NiTimeController> ctrl;
			float                               base{ 1.0f }, written{ -1.0f };
			std::uint32_t                       frame{ 0 };
		};

		// an explosion or hazard made while the spells were grown
		struct Spawned
		{
			RE::ObjectRefHandle ref;
			float               size{ 1.0f };
			bool                hazard{ false };
			bool                owned{ false };  // read as the player's
			std::uint32_t       waited{ 0 };     // frames without its model
		};

		std::unordered_map<RE::NiAVObject*, Grown>      gNodes;
		std::unordered_map<RE::NiTimeController*, Sped> gControllers;
		std::uint32_t                                   gFrame = 1;
		float                                           gPower = 1.0f;  // eased
		float                                           gWild = 0.0f;   // eased
		float                                           gTime = 0.0f;
		float                                           gFocusFrom = -1.0f;
		Overload::Phase                                 gPhase = Overload::Phase::kBuilding;

		std::mutex                       gNoteLock;
		std::vector<RE::ObjectRefHandle> gNoted;    // from the hooks, any thread
		std::vector<Spawned>             gSpawned;  // main thread

		float                      gSparkAt[2]{ -10.0f, -10.0f };  // when each hand last spat sparks
		std::uint32_t              gSparkTick[2]{ 0, 0 };          // the red zone's step each hand last looked at
		std::atomic<float>         gPowerNow{ 1.0f }, gWildNow{ 0.0f }, gPullNow{ 1.0f };
		std::atomic<float>         gLight[2]{ 1.0f, 1.0f };
		std::atomic<std::size_t>   gArts{ 0 }, gProjectiles{ 0 }, gOut{ 0 }, gImpacts{ 0 }, gHits{ 0 };
		std::atomic<std::uint32_t> gExplosions{ 0 }, gHazards{ 0 }, gFlares{ 0 }, gSparks{ 0 }, gModelsFound{ 0 };

		// once, on the main thread: which of our effect models are installed (loose or in an archive)
		void FindModels()
		{
			gModelsRead = true;
			std::uint32_t found = 0;
			for (std::size_t e = 0; e < kEffectNames.size(); ++e) {
				for (std::size_t l = 0; l < kLookNames.size(); ++l) {
					const auto                   name = std::format("ArcaneOverdrive\\{}{}.nif", kEffectNames[e], kLookNames[l]);
					RE::BSResourceNiBinaryStream stream("meshes\\" + name);
					gModels[e][l] = stream.good() ? name : std::string();
					found += stream.good() ? 1 : 0;
				}
			}
			gModelsFound = found;
			SKSE::log::info("effect models found: {} of {}", found, kEffectNames.size() * kLookNames.size());
		}

		// the look of a hand's spell: the one it is casting, else the one it holds
		[[nodiscard]] std::size_t LookOf(RE::PlayerCharacter* a_player, std::size_t a_hand, const RE::MagicCaster* a_caster)
		{
			const RE::MagicItem* spell = a_caster ? a_caster->currentSpell : nullptr;
			if (!spell && a_hand < 2) {
				spell = a_player->GetActorRuntimeData().selectedSpells[a_hand];
			}
			const int e = spell ? Read(spell).element : 0;
			return e >= 1 && e <= 3 ? static_cast<std::size_t>(e) : 0;
		}

		// one of our effects played once where a hand is, at a_scale, as the game plays an impact
		bool PlayAt(RE::PlayerCharacter* a_player, const RE::NiAVObject* a_hand, Effect a_effect, std::size_t a_look, float a_scale, float a_life)
		{
			const auto& model = gModels[a_effect][a_look];
			auto*       cell = a_player->GetParentCell();
			const auto& at = a_hand->world.translate;
			if (model.empty() || !cell || !std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
				return false;
			}
			return RE::BSTempEffectParticle::Spawn(cell, a_life, model.c_str(), RE::NiPoint3{}, at, a_scale, kSpawnFlags, nullptr) != nullptr;
		}

		void Grow(RE::NiAVObject* a_node, float a_scale, bool a_shows)
		{
			if (!a_node) {
				return;
			}
			auto [it, added] = gNodes.try_emplace(a_node);
			auto& g = it->second;
			if (added) {
				g.node.reset(a_node);
				g.base = a_node->local.scale;
			} else if (a_node->local.scale != g.written) {
				g.base = a_node->local.scale;  // someone else wrote it since (the game re-made the art): that is its size now
			}
			g.frame = gFrame;
			const float want = std::isfinite(g.base) && g.base > 0.0f ? g.base * a_scale : a_node->local.scale;
			a_node->local.scale = want;
			g.written = want;
			if (!a_shows && !a_node->GetAppCulled()) {
				a_node->SetAppCulled(true);
				g.culled = true;
			} else if (a_shows && g.culled) {
				a_node->SetAppCulled(false);
				g.culled = false;
			}
		}

		void Speed(RE::NiTimeController* a_ctrl, float a_ferocity)
		{
			auto [it, added] = gControllers.try_emplace(a_ctrl);
			auto& s = it->second;
			if (added) {
				s.ctrl.reset(a_ctrl);
				s.base = a_ctrl->frequency;
			} else if (a_ctrl->frequency != s.written) {
				s.base = a_ctrl->frequency;
			}
			s.frame = gFrame;
			const float want = std::isfinite(s.base) && s.base > 0.0f ? s.base * a_ferocity : a_ctrl->frequency;
			a_ctrl->frequency = want;
			s.written = want;
		}

		// every time controller under a model: on its nodes, its geometry (a particle system's emitters) and their shaders
		void SpeedAll(RE::NiAVObject* a_root, float a_ferocity)
		{
			if (!a_root || a_ferocity == 1.0f) {
				return;
			}
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_obj) {
				for (auto* c = a_obj->GetControllers(); c; c = c->GetNext()) {
					Speed(c, a_ferocity);
				}
				if (auto* geom = a_obj->AsGeometry()) {
					if (const auto& prop = geom->GetGeometryRuntimeData().shaderProperty) {
						for (auto* c = prop->GetControllers(); c; c = c->GetNext()) {
							Speed(c, a_ferocity);
						}
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void PutBack()
		{
			for (auto it = gNodes.begin(); it != gNodes.end();) {
				auto& g = it->second;
				if (gFrame - g.frame > kKeepFrames) {
					if (g.node) {
						if (g.node->local.scale == g.written) {
							g.node->local.scale = g.base;
						}
						if (g.culled) {
							g.node->SetAppCulled(false);
						}
					}
					it = gNodes.erase(it);
				} else {
					++it;
				}
			}
			for (auto it = gControllers.begin(); it != gControllers.end();) {
				auto& s = it->second;
				if (gFrame - s.frame > kKeepFrames) {
					if (s.ctrl && s.ctrl->frequency == s.written) {
						s.ctrl->frequency = s.base;
					}
					it = gControllers.erase(it);
				} else {
					++it;
				}
			}
		}

		// the player's spell projectiles in the world, from the game's own projectile lists (main thread only: the two lists
		// are kept from frame to frame so a frame allocates nothing)
		std::vector<RE::NiPointer<RE::Projectile>> gSpellProjectiles;  // this frame's only: emptied at the end of every update

		const std::vector<RE::NiPointer<RE::Projectile>>& PlayerSpellProjectiles(RE::PlayerCharacter* a_player)
		{
			auto&                                    out = gSpellProjectiles;
			static std::vector<RE::ProjectileHandle> handles;
			out.clear();
			handles.clear();
			auto* mgr = RE::Projectile::Manager::GetSingleton();
			if (!mgr) {
				return out;
			}
			{
				RE::BSSpinLockGuard guard(mgr->projectileLock);
				for (const auto* list : { &mgr->unlimited, &mgr->limited, &mgr->pending }) {
					for (const auto& h : *list) {
						handles.push_back(h);
					}
				}
			}
			for (auto& h : handles) {
				auto proj = h.get();
				if (!proj || out.size() >= 64) {
					continue;
				}
				auto&      rd = proj->GetProjectileRuntimeData();
				const auto shooter = rd.shooter.get();
				if (shooter.get() == a_player && rd.spell) {
					out.push_back(std::move(proj));
				}
			}
			return out;
		}

		[[nodiscard]] std::uint32_t SeedOf(const void* a_ptr) noexcept { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_ptr) >> 4); }

		// the explosions and hazards made since the last frame take a_size (only while it is over 1); the player's are grown
		// until they are gone, the rest dropped. -> how many were grown this frame
		std::size_t GrowSpawned(RE::PlayerCharacter* a_player, float a_size)
		{
			{
				std::scoped_lock lock(gNoteLock);
				for (const auto& h : gNoted) {
					const bool known = std::ranges::any_of(gSpawned, [&h](const Spawned& a_sp) { return a_sp.ref == h; });
					if (a_size > 1.001f && !known && gSpawned.size() < kMostSpawned) {
						gSpawned.push_back({ h, a_size });
					}
				}
				gNoted.clear();
			}
			std::size_t grown = 0;
			std::erase_if(gSpawned, [&](Spawned& a_sp) {
				const auto ref = a_sp.ref.get();
				if (!ref || ref->IsDeleted() || ref->IsDisabled()) {
					return true;
				}
				if (!a_sp.owned) {
					RE::Actor* owner = nullptr;
					if (auto* x = ref->As<RE::Explosion>()) {
						owner = x->GetExplosionRuntimeData().actorOwner.get().get();
					} else if (auto* z = ref->As<RE::Hazard>()) {
						owner = z->GetHazardRuntimeData().ownerActor.get().get();
						a_sp.hazard = true;
					}
					if (!a_player || owner != a_player) {
						return true;
					}
					a_sp.owned = true;
					++(a_sp.hazard ? gHazards : gExplosions);
				}
				auto* root = ref->Get3D();
				if (!root) {
					return ++a_sp.waited > kWait3D;
				}
				Grow(root, a_sp.size, true);
				if (a_sp.hazard) {
					SpeedAll(root, Overload::Ferocity(a_sp.size));  // a hazard's art loops, so faster reads fiercer; an explosion would only end sooner
				}
				++grown;
				return false;
			});
			return grown;
		}

		// the hit effects the player's spells leave on their targets run as fiercely as the spells. -> how many
		std::size_t SpeedHits(RE::PlayerCharacter* a_player, float a_ferocity)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists || !a_player || a_ferocity == 1.0f) {
				return 0;
			}
			std::size_t sped = 0;
			lists->ForEachModelEffect([&](RE::ModelReferenceEffect* a_effect) {
				auto* ctl = a_effect ? skyrim_cast<RE::ActiveEffectReferenceEffectController*>(a_effect->controller) : nullptr;
				if (ctl && ctl->effect && ctl->effect->caster.get().get() == a_player && a_effect->artObject3D) {
					SpeedAll(a_effect->artObject3D.get(), a_ferocity);
					++sped;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return sped;
		}
	}

	void NoteSpawned(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || gPowerNow.load(std::memory_order_relaxed) <= 1.001f) {
			return;  // the spells are not grown: nothing made now would be either
		}
		const auto       handle = a_ref->GetHandle();
		std::scoped_lock lock(gNoteLock);
		if (gNoted.size() < 256) {
			gNoted.push_back(handle);
		}
	}

	void UpdateOverload(const MeterView& a_v, const Settings& a_s, float a_dt)
	{
		++gFrame;
		gTime += std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, 0.25f) : 0.0f;
		auto*       player = RE::PlayerCharacter::GetSingleton();
		const auto  state = a_v.state;
		const auto  phase = state == Heat::Meter::State::kOverdrive ? Overload::Phase::kOverdrive :
		                    state == Heat::Meter::State::kCooldown  ? Overload::Phase::kBurnout :
		                                                              Overload::Phase::kBuilding;
		const float left = a_v.timerOf > 0.0f ? a_v.timer / a_v.timerOf : 0.0f;
		const bool  growing = a_s.enabled && a_s.spellGrowth;
		const float target = growing ? Overload::Growth(phase, a_v.charge / Heat::kFull, a_s.growthMax) : 1.0f;
		gPower = Overload::Ease(gPower, target, kEaseRate, a_dt);
		if (std::abs(gPower - target) < 0.002f) {
			gPower = target;  // settled: back to exactly 1 when the bar is empty, so nothing is touched any more
		}
		const bool fired = phase == Overload::Phase::kOverdrive && gPhase != Overload::Phase::kOverdrive;
		if (fired) {
			gFocusFrom = gTime;
		}
		if (!gModelsRead && (a_s.focusFlare || a_s.redSparks)) {
			FindModels();
		}
		gPhase = phase;
		const float pull = growing && a_s.overdriveFocus && phase == Overload::Phase::kOverdrive ? Overload::FocusPull(gTime - gFocusFrom) : 1.0f;
		const float wildTarget = a_s.enabled && a_s.redZoneWild ? Overload::RedDepth(a_v.redZone, a_v.heat / Heat::kFull, a_v.redLine / Heat::kFull, a_s.wildness) : 0.0f;
		gWild = phase == Overload::Phase::kOverdrive ? 0.0f : Overload::Ease(gWild, wildTarget, kWildEaseRate, a_dt);  // Overdrive focuses it at once
		if (wildTarget == 0.0f && gWild < 0.01f) {
			gWild = 0.0f;
		}
		const float ferocity = Overload::Ferocity(gPower);
		// a misfire sputters the spells for a moment, burnout flicker or not
		const bool  misfire = a_s.enabled && a_v.misfire > 0.0f;
		const bool  sputter = misfire || (a_s.enabled && a_s.burnoutFlicker && phase == Overload::Phase::kBurnout);
		const float flicker = misfire ? a_v.misfire : left;
		const bool  active = gPower != 1.0f || sputter || gWild > 0.0f || pull != 1.0f;
		// the burnout dips the hand lights and brings them back - a hand-light change, so behind Light growth like the stages'
		// growth (Hands.cpp)
		const float dip = a_s.lightGrowth && phase == Overload::Phase::kBurnout ? Overload::BurnoutDip(left) : 1.0f;
		std::size_t arts = 0, projectiles = 0, out = 0, impacts = 0, hits = 0;
		if (player && a_s.enabled) {
			for (std::size_t hand = 0; hand < 2; ++hand) {
				const auto seed = static_cast<std::uint32_t>(0x51ED + hand * 0x9E37);
				const auto wild = Overload::WildAt(gTime, gWild, seed);
				const bool lit = !sputter || Overload::Shows(gTime, flicker, seed);
				const bool shows = lit && wild.shows;
				gLight[hand] = Overload::LightWhen(lit) * (wild.shows ? 1.0f : kWildOutLight) * dip;
				out += shows ? 0 : 1;
				auto* caster = player->GetActorRuntimeData().magicCasters[hand];
				auto* art = caster ? caster->castingArtData.attachedArt.get() : nullptr;
				if (art && (active || gNodes.contains(art))) {
					Grow(art, gPower * pull * wild.size, shows);
					SpeedAll(art, ferocity * wild.speed);
					++arts;
				}
				if (art && fired && a_s.focusFlare && PlayAt(player, art, kFlare, LookOf(player, hand, caster), std::sqrt(gPower), kFlareLife)) {
					++gFlares;
				}
				// a spit of sparks when the red zone swells the hand's spell or blinks it out, once per step of it
				const auto step = static_cast<std::uint32_t>(static_cast<std::int64_t>(std::floor(gTime * 12.0f)) & 0xFFFFFFFF);
				if (art && a_s.redSparks && gWild > 0.0f && step != gSparkTick[hand]) {
					gSparkTick[hand] = step;
					if ((wild.size > 1.0f || !wild.shows) && gTime - gSparkAt[hand] >= kSparkGap &&
						PlayAt(player, art, kSparks, LookOf(player, hand, caster), 1.0f, kSparkLife)) {
						gSparkAt[hand] = gTime;
						++gSparks;
					}
				}
			}
			if (active || !gNodes.empty()) {
				for (const auto& proj : PlayerSpellProjectiles(player)) {
					auto* root = proj->Get3D();
					if (!root) {
						continue;
					}
					const auto seed = SeedOf(proj.get());
					const auto wild = Overload::WildAt(gTime, gWild, seed);
					const bool beam = proj->GetFormType() == RE::FormType::ProjectileBeam;
					const bool shows = (!sputter || Overload::Shows(gTime, flicker, seed)) && wild.shows;
					Grow(root, beam ? 1.0f : gPower * pull * wild.size, shows);
					SpeedAll(root, ferocity * wild.speed);
					++projectiles;
				}
			}
			if (a_s.impactsGrow) {
				hits = growing ? SpeedHits(player, ferocity) : 0;
			}
		} else {
			gLight[0] = 1.0f;
			gLight[1] = 1.0f;
		}
		const bool impactsOn = player && growing && a_s.impactsGrow;
		if (!impactsOn) {
			gSpawned.clear();  // switched off: what was grown goes back (PutBack, below)
		}
		impacts = GrowSpawned(player, impactsOn ? Overload::ImpactSize(gPower * pull, a_s.impactGrowth) : 1.0f);
		gSpellProjectiles.clear();  // keeps its room, lets the projectiles go
		PutBack();
		gPowerNow = gPower;
		gWildNow = gWild;
		gPullNow = pull;
		gArts = arts;
		gProjectiles = projectiles;
		gOut = out;
		gImpacts = impacts;
		gHits = hits;
	}

	void ReleaseOverload()
	{
		gSpellProjectiles.clear();
		{
			std::scoped_lock lock(gNoteLock);
			gNoted.clear();
		}
		gSpawned.clear();
		gFrame += kKeepFrames + 2;
		PutBack();
		gPower = 1.0f;
		gWild = 0.0f;
		gPhase = Overload::Phase::kBuilding;
		gPowerNow = 1.0f;
		gWildNow = 0.0f;
		gPullNow = 1.0f;
		gLight[0] = 1.0f;
		gLight[1] = 1.0f;
		gArts = 0;
		gProjectiles = 0;
		gOut = 0;
		gImpacts = 0;
		gHits = 0;
		gExplosions = 0;
		gHazards = 0;
		gFlares = 0;
		gSparks = 0;
		gSparkAt[0] = gSparkAt[1] = -10.0f;
	}

	float HandLightFactor(std::size_t a_hand) { return a_hand < 2 ? gLight[a_hand].load() : 1.0f; }

	OverloadView OverloadNow()
	{
		return { gPowerNow.load(), gArts.load(), gProjectiles.load(), gOut.load(), gWildNow.load(), gPullNow.load(), gImpacts.load(), gHits.load(),
			gExplosions.load(), gHazards.load(), gFlares.load(), gSparks.load(), gModelsFound.load() };
	}
}

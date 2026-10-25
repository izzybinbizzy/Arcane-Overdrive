// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The overcharge on the spells themselves (Overload.h has his order and the numbers). Once a frame, on the main thread
// (Meters.cpp's Frame, before the hand lights):
//   - GROWTH ("double in size and ferocity when overload is activated, climbing/falling live"): the casting art in each of
//     the player's hands and every spell projectile the player has in the world - the bolts, the sprays (Flames, Frostbite),
//     the beams - are scaled by the power now (Overload::Power, eased so it never jumps) and their own animations run faster
//     by the ferocity (every time controller under them: the particle emitters, the flicker, the texture scroll). It is
//     the game's own model, made bigger and fiercer - nothing new is drawn. A beam is not scaled (that would stretch it past
//     its target), only made fiercer.
//   - BURNOUT ("the burnout to cause the spells themselves to flicker out when the burn out option is ticked"): through the
//     cooldown, with Burnout flicker on, the art in the hands and the player's spells in the air sputter out and back
//     (Overload::Shows, 16 times a second, out most of the time at its start, steady again by its end), and the hand lights
//     dip with them (HandLightFactor, read by Hands.cpp).
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

		std::unordered_map<RE::NiAVObject*, Grown>      gNodes;
		std::unordered_map<RE::NiTimeController*, Sped> gControllers;
		std::uint32_t                                   gFrame = 1;
		float                                           gPower = 1.0f;  // eased
		float                                           gTime = 0.0f;

		std::atomic<float>       gPowerNow{ 1.0f };
		std::atomic<float>       gLight[2]{ 1.0f, 1.0f };
		std::atomic<std::size_t> gArts{ 0 }, gProjectiles{ 0 }, gOut{ 0 };

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
		const std::vector<RE::NiPointer<RE::Projectile>>& PlayerSpellProjectiles(RE::PlayerCharacter* a_player)
		{
			static std::vector<RE::NiPointer<RE::Projectile>> out;
			static std::vector<RE::ProjectileHandle>          handles;
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
		const float target = a_s.enabled && a_s.spellGrowth ? Overload::Power(phase, a_v.charge / Heat::kFull, left, a_s.growthMax) : 1.0f;
		gPower = Overload::Ease(gPower, target, kEaseRate, a_dt);
		if (std::abs(gPower - target) < 0.002f) {
			gPower = target;  // settled: back to exactly 1 when the meter is cold, so nothing is touched any more
		}
		const float ferocity = Overload::Ferocity(gPower);
		// a misfire sputters the spells for a moment, burnout flicker or not
		const bool  misfire = a_s.enabled && a_v.misfire > 0.0f;
		const bool  sputter = misfire || (a_s.enabled && a_s.burnoutFlicker && phase == Overload::Phase::kBurnout);
		const float flicker = misfire ? a_v.misfire : left;
		// the burnout dips the hand lights and brings them back - a hand-light change, so behind Light growth like the stages'
		// growth (Hands.cpp)
		const float dip = a_s.lightGrowth && phase == Overload::Phase::kBurnout ? Overload::BurnoutDip(left) : 1.0f;
		std::size_t arts = 0, projectiles = 0, out = 0;
		if (player && a_s.enabled) {
			for (std::size_t hand = 0; hand < 2; ++hand) {
				const bool shows = !sputter || Overload::Shows(gTime, flicker, static_cast<std::uint32_t>(0x51ED + hand * 0x9E37));
				gLight[hand] = Overload::LightWhen(shows) * dip;
				out += shows ? 0 : 1;
				auto* caster = player->GetActorRuntimeData().magicCasters[hand];
				auto* art = caster ? caster->castingArtData.attachedArt.get() : nullptr;
				if (art && (gPower != 1.0f || sputter || gNodes.contains(art))) {
					Grow(art, gPower, shows);
					SpeedAll(art, ferocity);
					++arts;
				}
			}
			if (gPower != 1.0f || sputter || !gNodes.empty()) {
				std::uint32_t seed = 0x1234u;
				for (const auto& proj : PlayerSpellProjectiles(player)) {
					auto* root = proj->Get3D();
					if (!root) {
						continue;
					}
					const bool beam = proj->GetFormType() == RE::FormType::ProjectileBeam;
					const bool shows = !sputter || Overload::Shows(gTime, flicker, seed += 0x9E3779B9u);
					Grow(root, beam ? 1.0f : gPower, shows);
					SpeedAll(root, ferocity);
					++projectiles;
				}
			}
		} else {
			gLight[0] = 1.0f;
			gLight[1] = 1.0f;
		}
		PutBack();
		gPowerNow = gPower;
		gArts = arts;
		gProjectiles = projectiles;
		gOut = out;
	}

	void ReleaseOverload()
	{
		gFrame += kKeepFrames + 2;
		PutBack();
		gPower = 1.0f;
		gPowerNow = 1.0f;
		gLight[0] = 1.0f;
		gLight[1] = 1.0f;
		gArts = 0;
		gProjectiles = 0;
		gOut = 0;
	}

	float HandLightFactor(std::size_t a_hand) { return a_hand < 2 ? gLight[a_hand].load() : 1.0f; }

	OverloadView OverloadNow() { return { gPowerNow.load(), gArts.load(), gProjectiles.load(), gOut.load() }; }
}

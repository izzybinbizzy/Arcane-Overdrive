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
//     made - and moved with the hand while it lasts. The look is the hand's spell's element; a model that is not installed
//     is never asked for.
//   - LINGERING, each capped and scaled by the Heat: after heavy
//     casting (Hot or hotter as a hand's cast ends) the hand keeps spitting embers / frost / sparks a while (Hand embers) and a
//     light of the spell's color hangs on it and fades out (Hand afterglow); whoever the player's spell hits (Warm or hotter)
//     gets a light that fades out on them (Hit afterglow) and smoulders a moment (Smoulder). The embers and smoulder are the
//     Sparks models above; the lights are ClashLights.cpp's.
// Every value is written from a remembered base, as the hand lights are (Hands.cpp): a value someone else has written since
// is taken as the new base, and a node or controller no longer ours is put back as it was found.

#include "ClashShared.h"
#include "Overload.h"
#include "Plugin.h"

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
		constexpr std::uint32_t kSpawnFlags = 7;     // as the clash bursts are played (ClashPass.cpp ModelHere)
		constexpr float         kEmberGap = 0.35f;   // a lingering hand or target spits again this often, seconds
		constexpr std::size_t   kMostLingering = 8;  // hands and targets lingering at once
		constexpr float         kHitAgain = 0.6f;    // the same target lingers again at most this often (a stream hits many times a second)

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

		// our effects that ride along with a hand while they last (a temp effect stays where it was played)
		struct Riding
		{
			RE::NiPointer<RE::BSTempEffect> effect;
			std::size_t                     hand{ 0 };
			float                           until{ 0.0f };
			RE::NiPointer<RE::NiAVObject>   follow;  // a hit target's node it rides with instead of a hand
		};
		std::vector<Riding> gRiding;  // main thread

		[[nodiscard]] bool Finite(const RE::NiPoint3& a_p) { return std::isfinite(a_p.x) && std::isfinite(a_p.y) && std::isfinite(a_p.z); }

		// where a hand is now: its casting art, else its magic node
		[[nodiscard]] const RE::NiAVObject* HandNode(RE::PlayerCharacter* a_player, std::size_t a_hand)
		{
			auto* caster = a_hand < 2 ? a_player->GetActorRuntimeData().magicCasters[a_hand] : nullptr;
			if (!caster) {
				return nullptr;
			}
			if (const auto* art = caster->castingArtData.attachedArt.get()) {
				return art;
			}
			return caster->GetMagicNode();
		}

		// one of our effects played once where a hand is, at a_scale, as the game plays an impact, then kept on the hand
		bool PlayAt(RE::PlayerCharacter* a_player, std::size_t a_hand, const RE::NiAVObject* a_at, Effect a_effect, std::size_t a_look, float a_scale,
			float a_life, RE::NiAVObject* a_follow = nullptr)
		{
			const auto& model = gModels[a_effect][a_look];
			auto*       cell = a_player->GetParentCell();
			const auto& at = a_at->world.translate;
			if (model.empty() || !cell || !Finite(at)) {
				return false;
			}
			auto* shown = RE::BSTempEffectParticle::Spawn(cell, a_life, model.c_str(), RE::NiPoint3{}, at, a_scale, kSpawnFlags, nullptr);
			if (shown && gRiding.size() < 32) {
				gRiding.push_back({ RE::NiPointer<RE::BSTempEffect>(shown), a_hand, gTime + a_life, RE::NiPointer<RE::NiAVObject>(a_follow) });
			}
			return shown != nullptr;
		}

		// once a frame: each riding effect moved by as much as its hand moved
		void Ride(RE::PlayerCharacter* a_player)
		{
			std::erase_if(gRiding, [&](Riding& r) {
				auto* shown = static_cast<RE::BSTempEffectParticle*>(r.effect.get());
				if (!shown || !a_player || gTime > r.until) {
					return true;
				}
				const auto* hand = r.follow ? r.follow.get() : HandNode(a_player, r.hand);
				auto*       object = shown->particleObject.get();
				if (hand && object && Finite(hand->world.translate) && Finite(object->world.translate)) {
					object->local.translate += hand->world.translate - object->world.translate;
				}
				return false;
			});
		}

		// ------------------------------------------------------------------ lingering
		struct Lingering
		{
			RE::NiPointer<RE::NiAVObject> on;  // a hit target's node; empty for a hand (looked up afresh each time)
			std::size_t                   hand{ 2 };
			std::size_t                   look{ 0 };
			float                         scale{ 1.0f };
			float                         next{ 0.0f }, until{ 0.0f };
			const RE::NiAVObject*         top{ nullptr };  // a target's: the top of its tree when hit (Crossfire::TopOf), only compared
		};
		struct NotedHit
		{
			RE::ActorHandle target;
			RE::FormID      source{ 0 };
		};
		struct RecentHit
		{
			RE::ActorHandle target;
			float           at{ 0.0f };
		};
		std::vector<Lingering>     gLingering;  // main thread
		std::vector<RecentHit>     gRecentHits;
		const RE::MagicItem*       gCasting[2]{ nullptr, nullptr };  // each hand's spell being cast last frame
		std::mutex                 gHitLock;
		std::vector<NotedHit>      gNotedHits;  // from the hit event, any thread
		std::atomic<std::uint32_t> gEmbers{ 0 }, gAfterglows{ 0 }, gHitGlows{ 0 }, gSmoulders{ 0 };

		// 0 at a_from, 1 at a full Heat bar
		[[nodiscard]] float HeatShare(float a_heat, float a_from) noexcept
		{
			return a_from < Heat::kFull ? std::clamp((a_heat - a_from) / (Heat::kFull - a_from), 0.0f, 1.0f) : 0.0f;
		}

		[[nodiscard]] std::size_t LookOfSpell(const RE::MagicItem* a_spell)
		{
			const int e = a_spell ? Read(a_spell).element : 0;
			return e >= 1 && e <= 3 ? static_cast<std::size_t>(e) : 0;
		}

		// the spell's own light color, else its element's
		[[nodiscard]] Crossfire::Core::Rgb ColorOf(const RE::MagicItem* a_spell, std::size_t a_look)
		{
			const auto* e = a_spell ? a_spell->GetCostliestEffectItem() : nullptr;
			if (const auto* light = e && e->baseEffect ? e->baseEffect->data.light : nullptr) {
				if (const auto& c = light->data.color; c.red || c.green || c.blue) {
					return { c.red, c.green, c.blue };
				}
			}
			constexpr std::array<Crossfire::Core::Rgb, 4> kByLook{ { { 150, 170, 255 }, { 255, 140, 60 }, { 140, 200, 255 }, { 170, 140, 255 } } };
			return kByLook[a_look < kByLook.size() ? a_look : 0];
		}

		// a hand's cast just ended: from Hot up its embers keep falling and its light fades out
		void CastEnded(RE::PlayerCharacter* a_player, std::size_t a_hand, const RE::MagicItem* a_spell, float a_heat, const Settings& a_s)
		{
			const float k = HeatShare(a_heat, a_s.tuning.at[1]);
			if (k <= 0.0f) {
				return;
			}
			const auto look = LookOfSpell(a_spell);
			if (a_s.handEmbers) {
				std::erase_if(gLingering, [a_hand](const Lingering& l) { return l.hand == a_hand; });
				if (gLingering.size() < kMostLingering) {
					gLingering.push_back({ {}, a_hand, look, 0.6f + 0.4f * k, gTime, gTime + 1.5f + 2.5f * k });
				}
			}
			// on the third-person hand in both views: the caster's own node is the first-person one in first person
			if (a_s.handAfterglow) {
				if (auto* node = HandLightNode(a_player, a_hand)) {
					Crossfire::LightAfterglow(node, ColorOf(a_spell, look), 180.0f + 120.0f * k, 0.8f + 0.8f * k, 0.8f + 1.4f * k);
					++gAfterglows;
				}
			}
		}

		// the hits noted since the last frame: from Warm up, a light that fades out on the target and a smoulder on them
		void TakeHits(float a_heat, const Settings& a_s)
		{
			std::vector<NotedHit> hits;
			{
				std::scoped_lock lock(gHitLock);
				hits.swap(gNotedHits);
			}
			std::erase_if(gRecentHits, [](const RecentHit& r) { return gTime - r.at > kHitAgain; });
			const float k = HeatShare(a_heat, a_s.tuning.at[0]);
			if (k <= 0.0f || !(a_s.hitAfterglow || a_s.smoulder)) {
				return;
			}
			static const RE::BSFixedString chest{ "NPC Spine2 [Spn2]" };
			for (const auto& h : hits) {
				if (std::ranges::any_of(gRecentHits, [&h](const RecentHit& r) { return r.target == h.target; })) {
					continue;
				}
				const auto  target = h.target.get();
				const auto* spell = RE::TESForm::LookupByID<RE::MagicItem>(h.source);
				auto*       root = target ? target->Get3D() : nullptr;
				if (!spell || !root || target->IsDead()) {
					continue;
				}
				auto* at = root->GetObjectByName(chest);
				at = at ? at : root;
				gRecentHits.push_back({ h.target, gTime });
				const auto look = LookOfSpell(spell);
				if (auto* node = a_s.hitAfterglow ? at->AsNode() : nullptr) {
					Crossfire::LightAfterglow(node, ColorOf(spell, look), 160.0f + 120.0f * k, 0.7f + 0.8f * k, 1.0f + 1.5f * k);
					++gHitGlows;
				}
				if (a_s.smoulder && gLingering.size() < kMostLingering) {
					gLingering.push_back({ RE::NiPointer<RE::NiAVObject>(at), 2, look, 0.5f + 0.3f * k, gTime, gTime + 1.5f + 2.0f * k, Crossfire::TopOf(at) });
				}
			}
		}

		// once a frame: each lingering hand or target spits its embers again, until its time is up
		void Linger(RE::PlayerCharacter* a_player)
		{
			std::erase_if(gLingering, [a_player](Lingering& l) {
				// a target's node keeps its parent when the actor unloads: the top of its tree tells
				if (gTime > l.until || (l.on && (!l.on->parent || Crossfire::TopOf(l.on.get()) != l.top))) {
					return true;
				}
				if (gTime < l.next) {
					return false;
				}
				l.next = gTime + kEmberGap;
				const bool            hand = !l.on;
				const RE::NiAVObject* at = hand ? HandNode(a_player, l.hand) : l.on.get();
				if (at && PlayAt(a_player, hand ? l.hand : 0, at, kSparks, l.look, l.scale, kSparkLife, hand ? nullptr : l.on.get())) {
					++(hand ? gEmbers : gSmoulders);
				}
				return false;
			});
		}

		void Grow(RE::NiAVObject* a_node, float a_scale, bool a_shows)
		{
			if (!a_node) {
				return;
			}
			// back at its own size and showing: put back and forgotten, so nothing keeps scanning for it every frame
			if (a_scale == 1.0f && a_shows) {
				if (const auto known = gNodes.find(a_node); known != gNodes.end()) {
					auto& g = known->second;
					if (a_node->local.scale == g.written) {
						a_node->local.scale = g.base;
					}
					if (g.culled) {
						a_node->SetAppCulled(false);
					}
					gNodes.erase(known);
				}
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

		// A controller's clock is frequency x time + phase (time since its start for an app-init one, the game's clock
		// otherwise): a new frequency alone jumps the clock by seconds, and a held stream's particle system then threw its
		// flames far off at every red-zone step. The phase moves with it, so the clock runs on from where it stood.
		void Retime(RE::NiTimeController* a_ctrl, float a_frequency)
		{
			const float old = a_ctrl->frequency;
			if (old == a_frequency || !std::isfinite(a_frequency)) {
				return;
			}
			const bool appInit = a_ctrl->flags.any(RE::NiTimeController::Flag::kAnimType_AppInit);
			a_ctrl->phase = Overload::RetimedPhase(a_ctrl->phase, appInit ? a_ctrl->lastTime - a_ctrl->startTime : a_ctrl->lastTime, old, a_frequency);
			a_ctrl->frequency = a_frequency;
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
			Retime(a_ctrl, want);
			s.written = a_ctrl->frequency;
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
			std::erase_if(gNodes, [](auto& a_kv) {
				auto& g = a_kv.second;
				if (gFrame - g.frame <= kKeepFrames) {
					return false;
				}
				if (g.node && g.node->local.scale == g.written) {
					g.node->local.scale = g.base;
				}
				if (g.node && g.culled) {
					g.node->SetAppCulled(false);
				}
				return true;
			});
			std::erase_if(gControllers, [](auto& a_kv) {
				auto& s = a_kv.second;
				if (gFrame - s.frame <= kKeepFrames) {
					return false;
				}
				if (s.ctrl && s.ctrl->frequency == s.written) {
					Retime(s.ctrl.get(), s.base);
				}
				return true;
			});
		}

		// the player's spell projectiles this frame (PlayerSpellsNow); the lists are kept from frame to frame so a frame allocates nothing
		std::vector<RE::NiPointer<RE::Projectile>> gSpellProjectiles;  // let go at the end of every frame (ReleaseSpellsNow)
		std::uint32_t                              gSpellsFrame{ 0 };  // the frame they were gathered in; 0: none
		constexpr std::size_t                      kMostSpells = 128;  // the player's own; more is a modded storm, not a look

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
					// a placed explosion by the game's own type info: As<> knows only the base record's form type, so it never matched
					if (auto* x = skyrim_cast<RE::Explosion*>(ref.get())) {
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
				// the cheap test first: most effects have no art up
				auto* ctl = a_effect && a_effect->artObject3D ? skyrim_cast<RE::ActiveEffectReferenceEffectController*>(a_effect->controller) : nullptr;
				if (ctl && ctl->effect && ctl->effect->caster.get().get() == a_player) {
					SpeedAll(a_effect->artObject3D.get(), a_ferocity);
					++sped;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return sped;
		}
	}

	const std::vector<RE::NiPointer<RE::Projectile>>& PlayerSpellsNow(RE::PlayerCharacter* a_player)
	{
		auto& out = gSpellProjectiles;
		if (gSpellsFrame == gFrame) {
			return out;
		}
		gSpellsFrame = gFrame;
		static std::vector<RE::ProjectileHandle> handles;
		out.clear();
		handles.clear();
		auto* mgr = RE::Projectile::Manager::GetSingleton();
		if (!mgr || !a_player) {
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
			if (out.size() >= kMostSpells) {
				break;
			}
			auto proj = h.get();
			if (!proj) {
				continue;
			}
			auto& rd = proj->GetProjectileRuntimeData();
			if (rd.spell && rd.shooter.get().get() == a_player) {
				out.push_back(std::move(proj));
			}
		}
		return out;
	}

	void ReleaseSpellsNow()
	{
		gSpellProjectiles.clear();  // keeps its room, lets the projectiles go
		gSpellsFrame = 0;
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

	void NoteHit(RE::Actor* a_target, RE::FormID a_source)
	{
		if (!a_target || !a_source) {
			return;
		}
		const auto       handle = a_target->GetHandle();
		std::scoped_lock lock(gHitLock);
		if (gNotedHits.size() < 64) {
			gNotedHits.push_back({ handle, a_source });
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
		if (!gModelsRead && (a_s.focusFlare || a_s.redSparks || a_s.handEmbers || a_s.smoulder)) {
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
				if (art && fired && a_s.focusFlare && PlayAt(player, hand, art, kFlare, LookOf(player, hand, caster), std::sqrt(gPower), kFlareLife)) {
					++gFlares;
				}
				// a spit of sparks when the red zone swells the hand's spell or blinks it out, once per step of it
				const auto step = Overload::Step(gTime, 12.0);
				if (art && a_s.redSparks && gWild > 0.0f && step != gSparkTick[hand]) {
					gSparkTick[hand] = step;
					if ((wild.size > 1.0f || !wild.shows) && gTime - gSparkAt[hand] >= kSparkGap &&
						PlayAt(player, hand, art, kSparks, LookOf(player, hand, caster), 1.0f, kSparkLife)) {
						gSparkAt[hand] = gTime;
						++gSparks;
					}
				}
				const RE::MagicItem* casting = caster ? caster->currentSpell : nullptr;
				if (gCasting[hand] && !casting) {
					CastEnded(player, hand, gCasting[hand], a_v.heat, a_s);
				}
				gCasting[hand] = casting;
			}
			TakeHits(a_v.heat, a_s);
			Linger(player);
			if (active || !gNodes.empty()) {
				for (const auto& proj : PlayerSpellsNow(player)) {
					if (projectiles >= 64) {
						break;
					}
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
			gLingering.clear();
			gCasting[0] = gCasting[1] = nullptr;
			std::scoped_lock lock(gHitLock);
			gNotedHits.clear();  // hits noted while off are not acted on when it comes back on
		}
		Ride(player);
		const bool impactsOn = player && growing && a_s.impactsGrow;
		if (!impactsOn) {
			gSpawned.clear();  // switched off: what was grown goes back (PutBack, below)
		}
		impacts = GrowSpawned(player, impactsOn ? Overload::ImpactSize(gPower * pull, a_s.impactGrowth) : 1.0f);
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
		ReleaseSpellsNow();
		{
			std::scoped_lock lock(gNoteLock);
			gNoted.clear();
		}
		gSpawned.clear();
		gRiding.clear();
		gLingering.clear();
		gRecentHits.clear();
		gCasting[0] = gCasting[1] = nullptr;
		{
			std::scoped_lock lock(gHitLock);
			gNotedHits.clear();
		}
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
		// the clock starts again with every load: a float clock run for days loses its fraction
		gTime = 0.0f;
		gFocusFrom = -1.0f;
	}

	float HandLightFactor(std::size_t a_hand) { return a_hand < 2 ? gLight[a_hand].load() : 1.0f; }

	OverloadView OverloadNow()
	{
		return { gPowerNow.load(), gArts.load(), gProjectiles.load(), gOut.load(), gWildNow.load(), gPullNow.load(), gImpacts.load(), gHits.load(),
			gExplosions.load(), gHazards.load(), gFlares.load(), gSparks.load(), gModelsFound.load(), gEmbers.load(), gAfterglows.load(), gHitGlows.load(),
			gSmoulders.load() };
	}
}

// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The pass, once a frame, on the game's main thread (from the player's update, main.cpp):
//   1. copy the projectile manager's handles (under its lock), and for each live, hostile projectile near the player
//      make a Core::Body: where it was last frame and where it is now, how big, what element, how strong
//   2. Core::FindContacts: which of them touched during the frame, earliest first
//   3. settle each touch with Core::Resolve: the loser is killed (after its own explosion is placed where they met),
//      a winner that pays for it is weakened, the player may earn skill experience, and listeners are told
// Everything the game gives us is checked before it is used; anything odd is simply left out of the frame.

#include "ClashShared.h"

#include "CrossfireAPI.h"

namespace Crossfire
{
	namespace
	{
		using Core::PairKey;
		using Core::Vec3;

		[[nodiscard]] Vec3         V(const RE::NiPoint3& a_p) noexcept { return { a_p.x, a_p.y, a_p.z }; }
		[[nodiscard]] RE::NiPoint3 N(Vec3 a_v) noexcept { return { a_v.x, a_v.y, a_v.z }; }
		// a unit vector along a_v; zero for none (not finite, or too short to have a direction)
		[[nodiscard]] Vec3 Unit(Vec3 a_v) noexcept
		{
			const float l = Core::Length(a_v);
			return std::isfinite(l) && l > 1e-4f ? a_v * (1.0f / l) : Vec3{};
		}

		// what we remember of a projectile between frames, by its handle
		struct Track
		{
			Vec3          last;
			float         strength{ 0.0f };
			std::uint32_t frame{ 0 };
		};

		// one projectile in this frame's pass
		struct Entry
		{
			RE::NiPointer<RE::Projectile>    ref;
			RE::NiPointer<RE::TESObjectREFR> shooterRef;
			const RE::BGSProjectile*         base{ nullptr };
			Info                             info;              // a copy: the cache it came from may change
			Track*                           track{ nullptr };  // a node of gTracks: stable until the prune at the end
			Vec3                             now;
			std::uint32_t                    handle{ 0 };
			std::uint32_t                    shooter{ 0 };  // the shooter's handle, 0 for none
			RE::Actor*                       actor{ nullptr };
			bool                             player{ false };
			bool                             killed{ false };
			bool                             lockable{ false };  // a stream that may lock in a spell struggle
			std::size_t                      stream{ 0 };        // its place in gStreams, when lockable
			Vec3                             dir;                // which way it goes this frame
			float                            radius{ 0.0f };
			float                            length{ 0.0f };  // a beam's, as drawn
			bool                             ghost{ false };  // a bolt's line kept after its projectile went (gLinger):
															  // no ref, never destroyed or changed, only clashes
		};

		// A one-shot bolt (Lightning Bolt) is a beam that exists for a moment, so two of them, or a bolt and a missile,
		// almost never share a frame. Its line is kept for BoltLinger seconds after its projectile is gone and still
		// clashes - as an immune body, the way a live beam does.
		struct Linger
		{
			Core::Body body;
			Entry      entry;  // with no ref
			Track      track;
			double     until{ 0.0 };
			bool       seen{ false };  // its projectile was alive this frame
		};
		std::unordered_map<std::uint32_t, Linger> gLinger;

		[[nodiscard]] bool OneShotBolt(const Info& a_info) { return a_info.kind == Core::Kind::kBeam && a_info.stream == Core::Stream::kNone; }

		Stats gStats;

		std::unordered_map<std::uint32_t, Track>  gTracks;
		std::unordered_map<std::uint32_t, double> gKilled;   // handles we killed, and when: never touched again
		std::unordered_map<std::uint32_t, double> gSteered;  // handles counted in Stats::steered, and when (a Track can be remade)
		std::unordered_map<std::uint64_t, bool>   gHostile;  // this frame's answers, by pair of shooters
		std::unordered_set<std::uint64_t>         gSettled;  // pairs of projectiles already settled: a bolt or a wall that
															 // lasts several frames meets a projectile once, not every frame
		std::vector<RE::ProjectileHandle> gHandles;
		std::vector<Entry>                gEntries;
		std::vector<Core::Body>           gBodies;
		std::vector<Core::Contact>        gContacts;
		std::vector<StreamRef>            gStreams;  // this frame's lockable streams, for Struggle.cpp
		Core::Limiter                     gBursts, gXP, gEvents;
		std::uint32_t                     gFrame = 0;
		double                            gClock = 0.0;  // game seconds since the game started; never goes back

		void Gather()
		{
			gHandles.clear();
			auto* manager = RE::Projectile::Manager::GetSingleton();
			if (!manager) {
				return;
			}
			RE::BSSpinLockGuard guard(manager->projectileLock);
			gHandles.reserve(manager->limited.size() + manager->unlimited.size());
			for (const auto& h : manager->limited) {
				gHandles.push_back(h);
			}
			for (const auto& h : manager->unlimited) {
				gHandles.push_back(h);
			}
		}

		[[nodiscard]] float StrengthOf(const Info& a_info, const RE::Projectile::PROJECTILE_RUNTIME_DATA& a_rd, const Core::Tuning& a_tuning)
		{
			float damage = 0.0f;
			if (a_info.kind == Core::Kind::kArrow) {
				damage = a_rd.weaponDamage;
				if (!(std::isfinite(damage) && damage > 0.0f)) {
					damage = 0.0f;
					if (a_rd.ammoSource) {
						damage += a_rd.ammoSource->GetRuntimeData().data.damage;
					}
					if (a_rd.weaponSource) {
						damage += static_cast<float>(a_rd.weaponSource->GetAttackDamage());
					}
				}
			}
			return Core::BaseStrength(a_info.kind, a_info.cost, a_rd.power, damage, a_tuning);
		}

		// not const: CommonLib's BSSimpleList cannot be walked through a const reference
		[[nodiscard]] float BeamLength(RE::Projectile::PROJECTILE_RUNTIME_DATA& a_rd, const RE::BGSProjectile* a_base, Vec3 a_from)
		{
			float length = a_rd.range > 0.0f ? a_rd.range : a_base->data.range;
			if (!(std::isfinite(length) && length > 0.0f)) {
				length = 3000.0f;
			}
			// the bolt ends where it hit
			for (const auto* impact : a_rd.impacts) {
				if (impact) {
					const float d = Core::Length(V(impact->desiredTargetLoc) - a_from);
					if (std::isfinite(d) && d > 1.0f) {
						length = std::min(length, d);
					}
					break;
				}
			}
			return std::clamp(length, 1.0f, 20000.0f);
		}

		[[nodiscard]] bool Hostile(const Entry& a, const Entry& b)
		{
			const auto key = PairKey(a.shooter, b.shooter);
			if (const auto it = gHostile.find(key); it != gHostile.end()) {
				return it->second;
			}
			const bool hostile = a.actor->IsHostileToActor(b.actor) || b.actor->IsHostileToActor(a.actor);
			gHostile.emplace(key, hostile);
			return hostile;
		}

		// auto lock in flight: each bolt turns a little toward the nearest enemy spell coming at it nearly head on, so the two
		// meet instead of brushing past (Core::SteerInto). It takes effect from the next frame, as the game moves it on
		void Steer(float a_delta, const Core::Config& a_cfg)
		{
			const float cone = a_cfg.steerCone * 0.017453292f;
			const float turn = a_cfg.steerTurn * 0.017453292f * a_delta;
			for (std::size_t i = 0; i < gEntries.size(); ++i) {
				auto& a = gEntries[i];
				auto* pa = a.ghost || a.killed ? nullptr : a.ref.get();
				if (!pa || a.info.immune || !(pa->Is(RE::FormType::ProjectileMissile) || (a_cfg.lockArrows && pa->Is(RE::FormType::ProjectileArrow)))) {
					continue;
				}
				std::optional<Vec3> best;
				float               bestDistance = a_cfg.steerRange;
				for (std::size_t j = 0; j < gEntries.size(); ++j) {
					const auto& b = gEntries[j];
					// other bolts only: a ghost line has no spell left, and spray particles / held beams are not steered into
					if (j == i || b.killed || b.ghost || b.info.stream != Core::Stream::kNone || b.shooter == a.shooter) {
						continue;
					}
					const Core::Shooters s{ a.shooter, b.shooter, a.player, b.player, a.actor != nullptr, b.actor != nullptr };
					if (!Core::MayInteract(s, a_cfg.who, a_cfg.ignoreAllies, [&]() { return Hostile(a, b); })) {
						continue;
					}
					const float d = Core::Length(b.now - a.now);
					if (!(d < bestDistance)) {
						continue;
					}
					if (const auto way = Core::SteerInto(a.now, a.dir, b.now, b.dir, a_cfg.steerRange, cone, turn)) {
						best = way;
						bestDistance = d;
					}
				}
				if (!best) {
					continue;
				}
				TurnProjectile(pa, *best);
				a.dir = *best;
				if (gSteered.try_emplace(a.handle, gClock).second) {
					gStats.steered.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		// the burst a destroyed projectile shows: its own explosion when it has one that may be shown, else (StandInBursts)
		// the one its element's spells use - a Firebolt has none and used to vanish without a trace
		RE::BGSExplosion* BurstOf(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base, Core::Element a_element, const Core::Config& a_cfg)
		{
			auto* own = a_projectile ? ExplosionOf(a_projectile, a_base) : nullptr;  // two bolts' lines: no projectile left
			if (own && (!a_cfg.safeExplosionsOnly || SafeExplosion(own))) {
				return own;
			}
			return a_cfg.standInBursts ? StandInBurst(a_element) : nullptr;
		}

		// the burst, placed where the two met - before the projectile is killed, while its cell is sure - at BurstScale.
		// With no projectile (two bolts' lines), in the player's cell
		void BurstHere(RE::BGSExplosion* a_explosion, Vec3 a_at, float a_scale, RE::Projectile* a_projectile)
		{
			auto*              data = RE::TESDataHandler::GetSingleton();
			RE::TESObjectREFR* place = a_projectile;
			if (!place) {
				place = RE::PlayerCharacter::GetSingleton();
			}
			auto* cell = place ? place->GetParentCell() : nullptr;
			if (!data || !cell || !Core::Finite(a_at)) {
				return;
			}
			const auto handle = data->CreateReferenceAtLocation(a_explosion, N(a_at), a_projectile ? a_projectile->GetAngle() : RE::NiPoint3{},
				cell, place->GetWorldspace(), nullptr, nullptr, RE::ObjectRefHandle(), false, true);
			if (auto ref = handle.get()) {
				const float k = std::isfinite(a_scale) ? std::clamp(a_scale, 0.5f, 3.0f) : 1.0f;
				ref->GetReferenceRuntimeData().refScale = static_cast<std::uint16_t>(std::lround(k * 100.0f));
				if (auto* x = skyrim_cast<RE::Explosion*>(ref.get())) {  // As<> knows only the base record's form type
					auto& xd = x->GetExplosionRuntimeData();
					if (std::isfinite(xd.radius)) {
						xd.radius *= k;
					}
				}
				gStats.explosions.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// a model played where the two met, for two seconds (the clash art, or a stand-in burst's model)
		RE::BSTempEffectParticle* ModelHere(const char* a_model, Vec3 a_at, float a_scale, RE::Projectile* a_projectile)
		{
			const char*        model = a_model;
			auto*              player = RE::PlayerCharacter::GetSingleton();
			RE::TESObjectREFR* place = a_projectile ? static_cast<RE::TESObjectREFR*>(a_projectile) : player;
			auto*              cell = place ? place->GetParentCell() : nullptr;
			if (!cell && player) {
				cell = player->GetParentCell();  // a projectile just killed may have left its cell already
			}
			if (!model || !cell || !Core::Finite(a_at)) {
				return nullptr;
			}
			const float k = std::isfinite(a_scale) ? std::clamp(a_scale, 0.5f, 3.0f) : 1.0f;
			auto*       shown = RE::BSTempEffectParticle::Spawn(cell, 2.0f, model, RE::NiPoint3{}, N(a_at), k, 7, nullptr);
			if (shown) {
				gStats.models.fetch_add(1, std::memory_order_relaxed);
			}
			return shown;
		}

		// A spawned design's mesh is cloned by a task of the game's, so its blocks are tinted once they are there; the
		// material is copied first, as every copy of a mesh shares the loaded one.
		struct PendingTint
		{
			RE::NiPointer<RE::BSTempEffect> effect;
			Core::Rgb                       light;
			int                             frames{ 0 };
		};
		std::vector<PendingTint> gTints;

		[[nodiscard]] bool Tint(RE::NiAVObject* a_root, Core::Rgb a_light)
		{
			bool any = false;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
				auto* raw = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto* prop = raw ? netimmerse_cast<RE::BSEffectShaderProperty*>(raw) : nullptr;
				auto* mat = prop ? static_cast<RE::BSEffectShaderMaterial*>(prop->GetMaterial()) : nullptr;
				if (!mat) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				const char* name = a_geometry->name.c_str();
				const auto  mode = Core::TintModeOf(name ? name : "", mat->baseColor.red, mat->baseColor.green, mat->baseColor.blue);
				if (mode == Core::TintMode::kKeep) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				auto* fresh = static_cast<RE::BSEffectShaderMaterial*>(mat->Create());
				if (!fresh) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				fresh->CopyMembers(mat);
				const auto c = Core::Tinted(a_light, mode);
				fresh->baseColor.red = c[0];
				fresh->baseColor.green = c[1];
				fresh->baseColor.blue = c[2];
				prop->SetMaterial(fresh, true);
				if (prop->GetMaterial() != fresh) {
					fresh->~BSEffectShaderMaterial();
					RE::free(fresh);
				}
				any = true;
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return any;
		}

		void TintLater(RE::BSTempEffectParticle* a_shown, const Info* a_from)
		{
			if (a_shown && a_from && a_from->light && gTints.size() < 64) {
				gTints.push_back({ RE::NiPointer<RE::BSTempEffect>(a_shown), *a_from->light });
			}
		}

		// once a frame: the designs whose meshes have arrived take their spell's color; one still empty after a second is let go
		void TintArrived()
		{
			std::erase_if(gTints, [](PendingTint& t) {
				auto* shown = static_cast<RE::BSTempEffectParticle*>(t.effect.get());
				if (shown && shown->particleObject) {
					if (Tint(shown->particleObject.get(), t.light)) {
						gStats.tinted.fetch_add(1, std::memory_order_relaxed);
					}
					return true;
				}
				return !shown || ++t.frames > 60;
			});
		}

		// what a clash shows where a projectile went: its own (or its element's) explosion when it only shows, else its
		// element's stand-in model, and its look's design in its own color (and `a_also`'s, when it looks different).
		// False when there was nothing to show, or the limiter held it back.
		bool ShowBurst(RE::Projectile* a_p, const RE::BGSProjectile* a_base, const Info& a_info, Vec3 a_at, std::uint64_t a_pair,
			const Core::Config& a_cfg, const Info* a_also = nullptr)
		{
			auto*       explosion = BurstOf(a_p, a_base, a_info.element, a_cfg);
			const char* model = !explosion && a_cfg.standInBursts ? StandInModel(a_info.element) : nullptr;
			const char* art = ClashArt(a_info.look);
			const char* also = a_also && a_also->look != a_info.look ? ClashArt(a_also->look) : nullptr;
			if ((!explosion && !model && !art && !also) || !gBursts.Allow(a_pair)) {
				return false;
			}
			if (explosion) {
				BurstHere(explosion, a_at, a_cfg.burstScale, a_p);
			} else if (model) {
				ModelHere(model, a_at, a_cfg.burstScale, a_p);
			}
			TintLater(ModelHere(art, a_at, a_cfg.burstScale, a_p), &a_info);
			TintLater(ModelHere(also, a_at, a_cfg.burstScale, a_p), a_also);
			return true;
		}

		// a special pair's own design on top of both bursts (fire and frost: steam; the same spell: resonance) - only where
		// a burst was allowed, so it obeys the same limits
		void ShowPair(const Info& a, const Info& b, Vec3 a_at, RE::Projectile* a_p, const Core::Config& a_cfg)
		{
			const bool same = a.spell && a.spell == b.spell;
			if (const char* art = PairArt(Core::PairOf(a.look, b.look, same))) {
				if (auto* shown = ModelHere(art, a_at, a_cfg.burstScale, a_p)) {
					gStats.pairs.fetch_add(1, std::memory_order_relaxed);
					TintLater(shown, same ? &a : nullptr);
				}
			}
		}

		// true when it burst where it went
		bool Settle(Entry& a_e, bool a_kill, float a_before, float a_after, Vec3 a_at, std::uint64_t a_pair, const Core::Config& a_cfg)
		{
			auto* p = a_e.ref.get();
			if (a_e.ghost || !p) {
				return false;  // a bolt's line kept after its projectile went: immune, nothing to change
			}
			if (a_kill) {
				const bool burst = a_cfg.explosions && ShowBurst(p, a_e.base, a_e.info, a_at, a_pair, a_cfg);
				p->Kill();
				a_e.killed = true;
				gKilled.insert_or_assign(a_e.handle, gClock);
				gStats.destroyed.fetch_add(1, std::memory_order_relaxed);
				return burst;
			}
			if (a_after < a_before) {
				a_e.track->strength = a_after;
				gStats.weakened.fetch_add(1, std::memory_order_relaxed);
				if (a_cfg.weakenDamage && a_before > 0.0f) {
					// what it does when it lands: a spell's scales with its power, an arrow's with its damage. Never an
					// arrow's power: with no spell, power is its speed (Projectile::GetPowerSpeedMult).
					const float k = Core::DamageScale(a_before, a_after);
					auto&       rd = p->GetProjectileRuntimeData();
					if (a_e.info.kind == Core::Kind::kArrow) {
						if (std::isfinite(rd.weaponDamage)) {
							rd.weaponDamage *= k;
						}
					} else if (rd.spell && std::isfinite(rd.power)) {
						rd.power *= k;
					}
				}
			}
			return false;
		}

		void Reward(const Entry& a_mine, const Entry& a_theirs, bool a_theirsKilled, const Core::Config& a_cfg)
		{
			if (!a_mine.player || a_theirs.player || !a_theirsKilled) {
				return;
			}
			gStats.yours.fetch_add(1, std::memory_order_relaxed);
			if (a_cfg.skillXP > 0.0f && a_mine.info.skill != RE::ActorValue::kNone && gXP.Allow(0)) {
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					player->AddSkillExperience(a_mine.info.skill, a_cfg.skillXP);
				}
			}
		}

		void Tell(const Entry& a, const Entry& b, const Core::Outcome& a_o, Vec3 a_at, std::uint64_t a_pair, const Core::Config& a_cfg)
		{
			CrossfireAPI::Clash msg;
			msg.x = a_at.x;
			msg.y = a_at.y;
			msg.z = a_at.z;
			msg.elementA = static_cast<std::uint8_t>(a.info.element);
			msg.elementB = static_cast<std::uint8_t>(b.info.element);
			msg.flags = static_cast<std::uint8_t>((a_o.killA ? CrossfireAPI::kDestroyedA : 0) | (a_o.killB ? CrossfireAPI::kDestroyedB : 0) |
												  (!a_o.killA && a_o.strengthA < a.track->strength ? CrossfireAPI::kWeakenedA : 0) |
												  (!a_o.killB && a_o.strengthB < b.track->strength ? CrossfireAPI::kWeakenedB : 0));
			msg.shooterA = a.shooterRef ? a.shooterRef->GetFormID() : 0;
			msg.shooterB = b.shooterRef ? b.shooterRef->GetFormID() : 0;
			msg.projectileA = a.base->GetFormID();
			msg.projectileB = b.base->GetFormID();
			if (auto* messaging = SKSE::GetMessagingInterface()) {
				messaging->Dispatch(CrossfireAPI::kClash, &msg, sizeof(msg), nullptr);
			}
			if (a_cfg.modEvents && gEvents.Allow(a_pair)) {
				if (auto* source = SKSE::GetModCallbackEventSource()) {
					// strArg "Fire,Frost"; numArg: 1 the first was destroyed, 2 the second, 3 both; sender: the first's shooter
					SKSE::ModCallbackEvent event{ "ArcaneOverdrive_Clash",
						std::format("{},{}", Core::ElementName(a.info.element), Core::ElementName(b.info.element)),
						static_cast<float>((a_o.killA ? 1 : 0) + (a_o.killB ? 2 : 0)), a.shooterRef.get() };
					source->SendEvent(&event);
				}
			}
		}
	}

	Stats& Counters() { return gStats; }

	void Reset()
	{
		gTracks.clear();
		gKilled.clear();
		gSteered.clear();
		gHostile.clear();
		gSettled.clear();
		gLinger.clear();
		gHandles.clear();
		gEntries.clear();
		gBodies.clear();
		gContacts.clear();
		gStreams.clear();
		gTints.clear();
		ClearInfo();              // keyed by form addresses: a spell or enchantment made in play is freed with its game
		Struggles::Reset(false);  // the handles it holds belong to the world that went away
		gBursts = {};
		gXP = {};
		gEvents = {};
		gStats.tracked.store(0, std::memory_order_relaxed);
	}

	void Update(float a_delta)
	{
		const auto& cfg = Live();
		if (!cfg.enabled || !std::isfinite(a_delta) || a_delta <= 0.0f) {
			if (!gTracks.empty() || Struggles::Active()) {
				Struggles::Reset(true);  // held beams get their range back first, while their handles still mean something
				Reset();                 // off, or time stood still: whatever we remembered is stale when it moves again
			}
			return;
		}
		a_delta = std::min(a_delta, 0.25f);
		++gFrame;
		gClock += static_cast<double>(a_delta);
		TintArrived();

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* myCell = player ? player->GetParentCell() : nullptr;
		if (!myCell) {
			return;
		}
		const bool  interior = myCell->IsInteriorCell();
		const auto* myWorld = player->GetWorldspace();
		const Vec3  me = V(player->GetPosition());
		const float maxDistance2 = cfg.maxDistance * cfg.maxDistance;
		const auto  playerHandle = player->GetHandle().native_handle();
		const auto  radius = [&](float a_own) { return Core::Radius(a_own, cfg); };

		Gather();
		gEntries.clear();
		gBodies.clear();
		gHostile.clear();
		std::erase_if(gKilled, [](const auto& a_kv) { return gClock - a_kv.second > 2.0; });
		std::erase_if(gSteered, [](const auto& a_kv) { return gClock - a_kv.second > 30.0; });  // longer than a bolt flies

		for (const auto& h : gHandles) {
			const std::uint32_t native = h.native_handle();
			if (!native || gKilled.contains(native)) {
				continue;
			}
			auto  ref = h.get();
			auto* p = ref.get();
			if (!p || p->IsDeleted() || p->IsDisabled()) {
				continue;
			}
			auto& rd = p->GetProjectileRuntimeData();
			if (rd.flags.any(RE::Projectile::Flags::kDestroyed, RE::Projectile::Flags::kIsTracer)) {
				continue;
			}
			auto* cell = p->GetParentCell();
			if (!cell || (interior ? cell != myCell : p->GetWorldspace() != myWorld)) {
				continue;
			}
			const Vec3 now = V(p->GetPosition());
			const Vec3 off = now - me;
			if (!Core::Finite(now) || Core::Dot(off, off) > maxDistance2) {
				continue;
			}
			const auto* base = p->GetProjectileBase();
			if (!base) {
				continue;
			}
			const Info& info = InfoOf(p, base);
			if (!info.eligible) {
				continue;
			}

			auto       shooterRef = rd.shooter.get();
			auto*      actor = shooterRef ? shooterRef->As<RE::Actor>() : nullptr;
			const bool lockable = cfg.struggle.enabled && actor && Core::StreamOn(info.stream, cfg.struggle);
			if (lockable && Struggles::Reeling(rd.shooter.native_handle())) {
				// it was overwhelmed a moment ago: whatever it streams now fizzles, and the winner's pours over it
				p->Kill();
				gKilled.insert_or_assign(native, gClock);
				gStats.cut.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			auto [it, fresh] = gTracks.try_emplace(native);
			Track&     track = it->second;
			const bool continuous = !fresh && track.frame + 1 == gFrame;
			if (fresh) {
				track.strength = StrengthOf(info, rd, cfg.tuning);
				if (cfg.debugLog) {
					const char* who = shooterRef ? shooterRef->GetDisplayFullName() : nullptr;
					const char* spell = rd.spell ? rd.spell->GetName() : nullptr;
					SKSE::log::info("launched: {:08X} {} by {:08X} {} with {:08X} {}", base->GetFormID(), Core::ElementName(info.element),
						shooterRef ? shooterRef->GetFormID() : 0, who ? who : "", rd.spell ? rd.spell->GetFormID() : 0, spell ? spell : "");
				}
				LockAtLaunch(p, info, actor, cfg);  // auto lock (ClashAim.cpp): before the way it came is worked out below
			}
			const Vec3 from = continuous ? track.last : Core::Backtrack(now, V(rd.linearVelocity), V(rd.velocity), a_delta, rd.distanceMoved);
			track.last = now;
			track.frame = gFrame;

			Core::Body b;
			b.id = native;
			b.shape = info.shape;
			b.element = info.element;
			b.immune = info.immune;
			b.strength = track.strength;
			b.lockable = lockable;
			b.bolt = cfg.boltsMeet && OneShotBolt(info);  // without it FindContacts never offers two bolts (both immune)
			Vec3 dir;
			switch (info.shape) {
			case Core::Shape::kSphere:
				{
					if (info.kind == Core::Kind::kStream) {
						// a held spray (Flames, Frostbite, a breath) is ONE long-lived projectile that sits at the caster's hand
						// and turns with the aim (measured: one Flames projectile lived 19 s and moved only as its caster
						// did). As a ball at the hand it met nothing: two casters' streams never touched, so no
						// struggle ever began, and "has hit something" or "is not moving" (the tests below, right for a missile)
						// dropped it exactly while it was burning its target. Its body is the stream itself: from the hand
						// along its aim for its reach, ending where it hits, like a held beam.
						const auto angle = p->GetAngle();
						b.shape = Core::Shape::kBeam;
						b.immune = true;  // an arrow crossing it does not put the spell out
						b.from = now;
						b.to = now + Core::DirectionFromAngles(angle.x, angle.z) * BeamLength(rd, base, now);
						b.radius = radius(base->data.collisionRadius);
						dir = Unit(b.to - b.from);
						break;
					}
					// one that has already hit something, or lies still (an arrow stuck in a wall, a lobbed spell resting
					// on the ground), is done flying. Not moving is the test that cannot be wrong; the impact list is only
					// a quicker answer for one that hit this frame. (A missile's very first frame, before it has moved,
					// is skipped too: it is looked at from the next.)
					if (!info.cone && (!rd.impacts.empty() || Core::Length(now - from) < 0.5f)) {
						continue;
					}
					b.from = from;
					b.to = now;
					dir = Core::Length(now - from) > 1e-3f ? Unit(now - from) : Unit(V(rd.linearVelocity));
					if (!Core::Finite(dir) || Core::Length(dir) < 0.5f) {
						dir = Unit(V(rd.velocity));
					}
					if (info.cone) {
						const auto& cone = static_cast<RE::ConeProjectile*>(p)->GetConeRuntimeData();
						b.radius = radius(Core::ConeRadius(cone.initialCollisionSphereRadius, Core::Length(now - V(cone.origin)), cone.coneAngleTangent));
					} else {
						b.radius = radius(base->data.collisionRadius);
						if (info.kind != Core::Kind::kArrow || cfg.lockArrows) {
							b.reach = Core::kHeadOnReach * std::clamp(cfg.headOnCatch, 0.0f, 300.0f) / 100.0f;
						}
					}
					break;
				}
			case Core::Shape::kBeam:
				{
					const auto angle = p->GetAngle();
					b.from = now;
					b.to = now + Core::DirectionFromAngles(angle.x, angle.z) * BeamLength(rd, base, now);
					b.radius = radius(base->data.collisionRadius);
					dir = Unit(b.to - b.from);
					break;
				}
			case Core::Shape::kBarrier:
				{
					const float width = static_cast<RE::BarrierProjectile*>(p)->GetBarrierRuntimeData().width;
					const float heading = p->GetAngleZ();
					if (!(std::isfinite(width) && width > 1.0f) || !std::isfinite(heading)) {
						continue;
					}
					b.from = b.to = now;
					b.across = Core::BarrierAcross(heading);
					b.halfWidth = 0.5f * width;
					b.height = cfg.barrierHeight;
					b.radius = std::min(radius(base->data.collisionRadius), 64.0f);
					break;
				}
			}
			if (!Core::Valid(b)) {
				continue;
			}

			Entry e;
			e.shooterRef = std::move(shooterRef);
			e.shooter = rd.shooter.native_handle();
			e.actor = actor;
			e.lockable = lockable;
			e.dir = Core::Finite(dir) ? dir : Vec3{};
			e.radius = b.radius;
			e.length = Core::Length(b.to - b.from);
			e.player = e.shooter != 0 && e.shooter == playerHandle;
			e.ref = std::move(ref);
			e.base = base;
			e.info = info;
			e.info.immune = b.immune;  // a held stream is immune here, whatever its record's kind says
			e.track = &track;
			e.now = now;
			e.handle = native;
			b.shooter = e.shooter;
			gEntries.push_back(std::move(e));
			gBodies.push_back(b);
		}
		// the bolts: remember each live one's line; a line whose projectile has gone clashes on until BoltLinger runs out
		for (auto& [h, l] : gLinger) {
			l.seen = false;
		}
		if (cfg.boltLinger > 0.0f) {
			for (std::size_t i = 0; i < gEntries.size(); ++i) {
				const auto& e = gEntries[i];
				if (!OneShotBolt(e.info)) {
					continue;
				}
				auto& l = gLinger[e.handle];
				l.body = gBodies[i];
				l.entry = e;
				l.entry.ref.reset();
				l.entry.ghost = true;
				l.track = *e.track;
				l.until = gClock + static_cast<double>(cfg.boltLinger);
				l.seen = true;
			}
		}
		std::erase_if(gLinger, [](const auto& a_kv) { return !a_kv.second.seen && a_kv.second.until < gClock; });
		for (auto& [h, l] : gLinger) {
			if (l.seen || gKilled.contains(h)) {
				continue;
			}
			Entry e = l.entry;
			e.track = &l.track;
			gEntries.push_back(std::move(e));
			gBodies.push_back(l.body);
		}
		gStats.tracked.store(static_cast<std::uint32_t>(gEntries.size()), std::memory_order_relaxed);
		if (cfg.steer) {
			Steer(a_delta, cfg);
		}

		gBursts.BeginFrame(gClock, cfg.maxExplosionsPerFrame, cfg.explosionCooldown);
		gXP.BeginFrame(gClock, 1, 0.5f);
		gEvents.BeginFrame(gClock, 8, 0.25f);

		// the struggles: every lockable stream, then one step of each lock (it may snuff particles past a front)
		gStreams.clear();
		for (std::size_t i = 0; i < gEntries.size(); ++i) {
			auto& e = gEntries[i];
			if (!e.lockable) {
				continue;
			}
			auto*     p = e.ref.get();
			auto&     rd = p->GetProjectileRuntimeData();
			StreamRef s;
			s.entry = i;
			s.projectile = p;
			s.handle = RE::ProjectileHandle(p);
			s.native = e.handle;
			s.shooter = e.shooter;
			s.shooterHandle = rd.shooter;
			s.actor = e.actor;
			s.player = e.player;
			s.info = &e.info;
			s.base = e.base;
			s.now = e.now;
			s.dir = e.dir;
			s.radius = e.radius;
			s.length = e.length;
			s.power = std::isfinite(rd.power) && rd.power > 0.0f ? rd.power : 1.0f;
			s.source = rd.castingSource;
			e.stream = gStreams.size();
			gStreams.push_back(s);
		}
		Struggles::Frame(gStreams, cfg, a_delta, gFrame);
		if (cfg.debugLog && gFrame % 300 == 0 && !gEntries.empty()) {
			SKSE::log::info("pass: {} projectile(s) in range, {} of them lockable stream(s), {} handle(s) in the manager", gEntries.size(),
				gStreams.size(), gHandles.size());
			for (const auto& st : gStreams) {
				SKSE::log::info("  stream: {}{:08X} from ({:.0f}, {:.0f}, {:.0f}) aim ({:.2f}, {:.2f}, {:.2f}) length {:.0f} radius {:.0f}",
					st.player ? "yours " : "", st.base ? st.base->GetFormID() : 0, st.now.x, st.now.y, st.now.z, st.dir.x, st.dir.y, st.dir.z, st.length,
					st.radius);
			}
		}

		if (gEntries.size() >= 2) {
			const auto may = [&](std::size_t i, std::size_t j) {
				const Entry& a = gEntries[i];
				const Entry& b = gEntries[j];
				if (a.killed || b.killed) {
					return false;
				}
				if (a.lockable && b.lockable && Struggles::Holds(a.shooter, b.shooter)) {
					return false;  // a locked pair meets at its front
				}
				const Core::Shooters s{ a.shooter, b.shooter, a.player, b.player, a.actor != nullptr, b.actor != nullptr };
				return Core::MayInteract(s, cfg.who, cfg.ignoreAllies, [&]() { return Hostile(a, b); });
			};
			const float headOnCos = std::cos(std::clamp(cfg.headOnCone, 0.0f, 60.0f) * std::numbers::pi_v<float> / 180.0f);
			Core::FindContacts(gBodies, may, static_cast<std::size_t>(cfg.maxContactsPerFrame), gContacts, headOnCos);
			if (cfg.debugLog && !gContacts.empty() && gFrame % 30 == 0) {
				SKSE::log::info("pass: {} touch(es) this frame among {} projectile(s)", gContacts.size(), gEntries.size());
			}
		} else {
			gContacts.clear();
		}

		for (const auto& c : gContacts) {
			Entry& a = gEntries[c.a];
			Entry& b = gEntries[c.b];
			if (a.killed || b.killed) {
				continue;  // it went earlier in this frame
			}
			const auto meeting = PairKey(a.handle, b.handle);
			if (gSettled.contains(meeting)) {
				continue;
			}
			if (a.lockable && b.lockable && a.shooter != b.shooter &&
				Struggles::TryBegin(gStreams[a.stream], gStreams[b.stream], c.touch.point, cfg)) {
				continue;  // they locked (or already are): the struggle settles it, not a clash
			}
			if (cfg.boltsMeet && OneShotBolt(a.info) && OneShotBolt(b.info)) {
				// two bolts crossing: both are immune beams, which never clash by the table - they burst between them and
				// both go (a line kept after its projectile went only bursts)
				gStats.clashes.fetch_add(1, std::memory_order_relaxed);
				gSettled.insert(meeting);
				const auto    pair = PairKey(a.shooter, b.shooter);
				Core::Outcome o{ true, !a.ghost, !b.ghost, a.track->strength, b.track->strength };
				if (cfg.debugLog) {
					SKSE::log::info("bolts meet: {:08X}{}{} vs {:08X}{}{} at ({:.0f}, {:.0f}, {:.0f})", a.base->GetFormID(), a.player ? " (yours)" : "",
						a.ghost ? " (line)" : "", b.base->GetFormID(), b.player ? " (yours)" : "", b.ghost ? " (line)" : "", c.touch.point.x,
						c.touch.point.y, c.touch.point.z);
				}
				Tell(a, b, o, c.touch.point, pair, cfg);
				if (cfg.explosions) {
					Entry* live = !a.ghost && a.ref ? &a : (!b.ghost && b.ref ? &b : nullptr);
					if (ShowBurst(live ? live->ref.get() : nullptr, live ? live->base : a.base, a.info, c.touch.point, pair, cfg, &b.info)) {
						ShowPair(a.info, b.info, c.touch.point, live ? live->ref.get() : nullptr, cfg);
					}
				}
				for (Entry* x : { &a, &b }) {
					if (!x->ghost && x->ref) {
						x->ref->Kill();
						x->killed = true;
						gKilled.insert_or_assign(x->handle, gClock);
						gStats.destroyed.fetch_add(1, std::memory_order_relaxed);
					}
				}
				continue;
			}
			const float sa = a.track->strength, sb = b.track->strength;
			const auto  o = Core::Resolve(cfg.reactions, cfg.overpowerRatio, cfg.weakenSurvivor, a.info.element, sa, a.info.immune,
				b.info.element, sb, b.info.immune);
			if (!o.happened) {
				continue;
			}
			gStats.clashes.fetch_add(1, std::memory_order_relaxed);
			gSettled.insert(meeting);
			ClashEvent(Core::ElementName(a.info.element), Core::ElementName(b.info.element), o.killA, o.killB, a.player || b.player);
			const auto pair = PairKey(a.shooter, b.shooter);
			if (cfg.debugLog) {
				SKSE::log::info("clash: {:08X} {} {:.0f} by {:08X}{} vs {:08X} {} {:.0f} by {:08X}{} at ({:.0f}, {:.0f}, {:.0f}), t {:.2f}: {} / {}",
					a.base->GetFormID(), Core::ElementName(a.info.element), sa, a.shooterRef ? a.shooterRef->GetFormID() : 0,
					a.player ? " (yours)" : "", b.base->GetFormID(), Core::ElementName(b.info.element), sb,
					b.shooterRef ? b.shooterRef->GetFormID() : 0, b.player ? " (yours)" : "", c.touch.point.x, c.touch.point.y, c.touch.point.z,
					c.touch.t, o.killA ? "destroyed" : (o.strengthA < sa ? std::format("left {:.0f}", o.strengthA) : "untouched"),
					o.killB ? "destroyed" : (o.strengthB < sb ? std::format("left {:.0f}", o.strengthB) : "untouched"));
			}
			Tell(a, b, o, c.touch.point, pair, cfg);  // before Settle: it reads the strengths as they were
			RE::Projectile* at = !a.ghost && a.ref ? a.ref.get() : (!b.ghost && b.ref ? b.ref.get() : nullptr);
			const bool      burstA = Settle(a, o.killA, sa, o.strengthA, c.touch.point, pair, cfg);
			const bool      burstB = Settle(b, o.killB, sb, o.strengthB, c.touch.point, pair, cfg);
			if (burstA || burstB) {
				ShowPair(a.info, b.info, c.touch.point, at, cfg);
			}
			Reward(a, b, o.killB, cfg);
			Reward(b, a, o.killA, cfg);
		}

		// forget what was not seen this frame, and let go of every projectile
		std::erase_if(gTracks, [](const auto& a_kv) { return a_kv.second.frame != gFrame; });
		for (const auto& e : gEntries) {
			if (e.killed) {
				gTracks.erase(e.handle);
			}
		}
		std::erase_if(gSettled, [](std::uint64_t a_pair) {
			const auto known = [](std::uint32_t h) { return gTracks.contains(h) || gLinger.contains(h); };
			return !known(static_cast<std::uint32_t>(a_pair >> 32)) || !known(static_cast<std::uint32_t>(a_pair));
		});
		gEntries.clear();
		gStreams.clear();
		gHandles.clear();
	}

	void Snuff(std::size_t a_entry)
	{
		if (a_entry >= gEntries.size()) {
			return;
		}
		auto& e = gEntries[a_entry];
		if (e.killed || e.ghost || !e.ref) {
			return;
		}
		e.ref->Kill();
		e.killed = true;
		gKilled.insert_or_assign(e.handle, gClock);
		gStats.cut.fetch_add(1, std::memory_order_relaxed);
	}

	bool BurstAt(std::size_t a_entry, Core::Vec3 a_at, std::uint64_t a_pair, const Core::Config& a_config)
	{
		if (a_entry >= gEntries.size() || !a_config.explosions) {
			return false;
		}
		auto& e = gEntries[a_entry];
		auto* p = e.ref.get();
		if (!p) {
			return false;
		}
		return ShowBurst(p, e.base, e.info, a_at, a_pair, a_config);
	}
}

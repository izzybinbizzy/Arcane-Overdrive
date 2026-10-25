// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Spell struggles, on the game's main thread, inside the pass (Clash.cpp). Two hostile streams - sprays, held beams,
// breath - that meet lock together, and the stronger caster pushes the meeting point back until it breaks through.
// Core decides everything that can be decided without the game (who may lock, how hard each side pushes, the contest
// over time, where the meeting point is); this file reads the actors, feeds Core, and carries out what each step says:
// particles past the meeting point are snuffed, a locked beam is cut short there, the loser pays magicka for being
// pushed back, and a breakthrough breaks the loser's cast, staggers and hurts them.

#include "ClashShared.h"
#include "Translation.h"

#include "CrossfireAPI.h"

#include <cstdio>
#include <limits>

namespace Crossfire
{
	namespace
	{
		using Core::Vec3;

		[[nodiscard]] Vec3         V(const RE::NiPoint3& a_p) noexcept { return { a_p.x, a_p.y, a_p.z }; }
		[[nodiscard]] RE::NiPoint3 N(Vec3 a_v) noexcept { return { a_v.x, a_v.y, a_v.z }; }

		// one side of a struggle, as it was when the lock began (the class of actor, its name) and as it last streamed
		struct SideRec
		{
			RE::ObjectRefHandle            ref;
			bool                           player{ false }, npc{ false }, dragon{ false }, creature{ false };
			bool                           breath{ false }, beam{ false };
			Info                           leading;  // its costliest stream seen, kept when it is not seen for a frame
			float                          power{ 1.0f };
			RE::MagicSystem::CastingSource source{ RE::MagicSystem::CastingSource::kOther };
			std::string                    name;
		};

		struct Rec
		{
			SideRec a, b;
			double  nextRumble{ 0.0 };
		};

		// a locked beam cut short: its own range, to give back when it is no longer locked
		struct BeamHold
		{
			RE::ProjectileHandle handle;
			float                range0{ 0.0f };
			bool                 held{ false };  // this frame
		};

		Core::StruggleBook                          gBook;
		std::unordered_map<std::uint64_t, Rec>      gRecs;
		std::unordered_map<std::uint32_t, BeamHold> gBeams;
		// a locked spray's reach (units from the projectile) - read by the FlameProjectile hit hook, which may run off the
		// main thread
		std::mutex                                                  gFlameCutLock;
		std::unordered_map<std::uint32_t, float>                    gFlameCut;
		std::unordered_set<std::uint64_t>                           gTried;  // pairs that may not lock this frame
		std::unordered_map<std::uint32_t, std::vector<std::size_t>> gBy;     // shooter -> its streams this frame
		std::span<const StreamRef>                                  gStreams;
		std::uint64_t                                               gEngagements = 0;
		std::uint32_t                                               gFrame = 0;
		double                                                      gClock = 0.0;
		std::mutex                                                  gViewLock;
		StruggleView                                                gView;

		[[nodiscard]] RE::NiPointer<RE::Actor> ActorOf(const RE::ObjectRefHandle& a_handle)
		{
			auto  ref = a_handle.get();
			auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
			return RE::NiPointer<RE::Actor>(actor);
		}

		[[nodiscard]] bool Alive(RE::Actor* a_actor)
		{
			return a_actor && !a_actor->IsDead() && !a_actor->IsDisabled() && a_actor->Is3DLoaded();
		}

		[[nodiscard]] float AV(RE::Actor* a_actor, RE::ActorValue a_av)
		{
			auto*       owner = a_actor ? a_actor->AsActorValueOwner() : nullptr;
			const float v = owner ? owner->GetActorValue(a_av) : 0.0f;
			return std::isfinite(v) ? v : 0.0f;
		}

		// Alteration, Conjuration, Destruction, Illusion, Restoration as 0..4; -1 for anything else
		[[nodiscard]] int SchoolIndex(RE::ActorValue a_av) noexcept
		{
			const auto i = static_cast<int>(a_av) - static_cast<int>(RE::ActorValue::kAlteration);
			return i >= 0 && i < 5 ? i : -1;
		}

		[[nodiscard]] const char* SchoolName(int a_i) noexcept
		{
			constexpr const char* kNames[]{ "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration" };
			return a_i >= 0 && a_i < 5 ? kNames[a_i] : "Level";
		}

		[[nodiscard]] Vec3 ActorPoint(RE::Actor* a_actor) { return V(a_actor->GetPosition()) + Vec3{ 0.0f, 0.0f, 100.0f }; }

		// where a side's stream comes from, first match wins: a beam's own start; the hand's magic node (read through the
		// caster field - never GetMagicCaster, which may make one); the side's particle nearest its actor; the actor + 100 z
		[[nodiscard]] Vec3 MuzzleOf(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_source, const std::vector<std::size_t>* a_streams)
		{
			const Vec3 at = V(a_actor->GetPosition());
			if (a_streams) {
				for (const auto i : *a_streams) {
					const auto& s = gStreams[i];
					if (s.info->stream == Core::Stream::kBeam && Core::Finite(s.now)) {
						return s.now;
					}
				}
			}
			if (const auto i = std::to_underlying(a_source); i < 4) {
				if (auto* mc = a_actor->GetActorRuntimeData().magicCasters[i]; mc && mc->magicNode) {
					const Vec3 p = V(mc->magicNode->world.translate);
					if (Core::Finite(p) && Core::Length(p - at) < 1000.0f) {
						return p;
					}
				}
			}
			if (a_streams) {
				float best = std::numeric_limits<float>::max();
				Vec3  bestP{};
				for (const auto i : *a_streams) {
					const auto& s = gStreams[i];
					const float d = Core::Length(s.now - at);
					if (std::isfinite(d) && d < best) {
						best = d;
						bestP = s.now;
					}
				}
				if (best < std::numeric_limits<float>::max()) {
					return bestP;
				}
			}
			return ActorPoint(a_actor);
		}

		[[nodiscard]] float ReachOf(const StreamRef& a_s)
		{
			if (a_s.info->stream == Core::Stream::kBeam) {
				const float r = a_s.projectile->GetProjectileRuntimeData().range;
				return Core::StreamReach(std::isfinite(r) && r > 0.0f ? r : a_s.base->data.range, 0.0f, 0.0f);
			}
			return Core::StreamReach(a_s.base->data.range, a_s.base->data.speed, a_s.base->data.lifetime);
		}

		// the words of the shout this breath came from: 1..3, 1 if not found
		[[nodiscard]] int WordsOf(RE::Actor* a_actor, const RE::MagicItem* a_spell)
		{
			if (auto* shout = a_actor ? a_actor->GetCurrentShout() : nullptr) {
				for (int i = 0; i < 3; ++i) {
					if (shout->variations[i].spell && shout->variations[i].spell == a_spell) {
						return i + 1;
					}
				}
			}
			return 1;
		}

		[[nodiscard]] bool DualCasting(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_source)
		{
			if (const auto i = std::to_underlying(a_source); i < 4) {
				if (auto* mc = a_actor->GetActorRuntimeData().magicCasters[i]) {
					return mc->GetIsDualCasting();
				}
			}
			return false;
		}

		// one side's push, from the live actor
		[[nodiscard]] Core::Caster CasterOf(RE::Actor* a_actor, const SideRec& a_side, bool a_twoHands, bool a_opposesPlayer,
			const Core::Config& a_cfg)
		{
			const auto&          sc = a_cfg.struggle;
			Core::Caster         c;
			const float          level = static_cast<float>(a_actor->GetLevel());
			const int            own = SchoolIndex(a_side.leading.skill);
			std::array<float, 5> schools{};
			if (sc.skillSource == 0 && own >= 0) {
				schools[static_cast<std::size_t>(own)] = AV(a_actor, a_side.leading.skill);
			} else {
				for (int i = 0; i < 5; ++i) {
					schools[static_cast<std::size_t>(i)] = AV(a_actor, static_cast<RE::ActorValue>(static_cast<int>(RE::ActorValue::kAlteration) + i));
				}
			}
			const float picked = Core::PickSkill(std::span<const float, 5>(schools), own, sc.skillSource);
			c.skill = Core::MagicSkill(picked, level, a_side.creature, a_side.breath);
			c.level = level;
			c.cost = a_side.breath ? sc.breathStrength : std::max(a_side.leading.cost, a_cfg.tuning.minSpellStrength);
			c.school = !a_side.breath && own >= 0;
			c.paysMagicka = !a_side.breath && !a_side.leading.staff;
			if (c.paysMagicka) {
				const float max = std::max(1.0f, a_actor->GetActorValueMax(RE::ActorValue::kMagicka));
				c.magicka = std::clamp(AV(a_actor, RE::ActorValue::kMagicka) / max, 0.0f, 1.0f);
			}
			c.dual = a_twoHands || a_side.leading.staff || DualCasting(a_actor, a_side.source);
			c.mult = Core::PowerMult(a_opposesPlayer, a_side.dragon, a_side.breath ? WordsOf(a_actor, a_side.leading.spell) : 0, sc);
			return c;
		}

		// the side's streams this frame that come at the other side: which are seen, the costliest, two hands at once
		struct Seen
		{
			bool             seen{ false };
			bool             streaming{ false };  // any stream at all (for kDeclined)
			bool             twoHands{ false };
			const StreamRef* leading{ nullptr };
		};

		[[nodiscard]] Seen Look(std::uint32_t a_shooter, Vec3 a_from, Vec3 a_to)
		{
			Seen       out;
			const auto it = gBy.find(a_shooter);
			if (it == gBy.end()) {
				return out;
			}
			out.streaming = !it->second.empty();
			const float D = Core::Length(a_to - a_from);
			bool        sources[4]{};
			for (const auto i : it->second) {
				const auto& s = gStreams[i];
				bool        ok = false;
				if (s.info->stream == Core::Stream::kBeam) {
					ok = Core::BeamAims(s.now, s.dir, a_to, s.radius);
				} else {
					ok = Core::InCorridor(Core::Project(s.now, a_from, a_to), D, s.radius);
				}
				if (!ok) {
					continue;
				}
				out.seen = true;
				if (const auto k = std::to_underlying(s.source); k < 4) {
					sources[k] = true;
				}
				if (!out.leading || s.info->cost > out.leading->info->cost) {
					out.leading = &s;
				}
			}
			out.twoHands = (sources[0] ? 1 : 0) + (sources[1] ? 1 : 0) + (sources[2] ? 1 : 0) >= 2;
			return out;
		}

		void Take(SideRec& a_side, const StreamRef& a_s)
		{
			a_side.leading = *a_s.info;
			a_side.power = a_s.power;
			a_side.source = a_s.source;
			a_side.beam = a_s.info->stream == Core::Stream::kBeam;
			a_side.breath = a_s.info->stream == Core::Stream::kBreath;
		}

		// a stream entry of this shooter to take an explosion from (its costliest one)
		[[nodiscard]] const StreamRef* AnyStream(std::uint32_t a_shooter)
		{
			const auto it = gBy.find(a_shooter);
			if (it == gBy.end() || it->second.empty()) {
				return nullptr;
			}
			const StreamRef* best = nullptr;
			for (const auto i : it->second) {
				if (!best || gStreams[i].info->cost > best->info->cost) {
					best = &gStreams[i];
				}
			}
			return best;
		}

		void BurstFor(std::uint32_t a_first, std::uint32_t a_second, Vec3 a_at, std::uint64_t a_key, const Core::Config& a_cfg)
		{
			if (!a_cfg.struggle.lockBursts) {
				return;
			}
			// BOTH spells burst where they meet (his ask 2026-10-07: "a visual overload of fx depending on what the spells
			// are") - fire against frost shows the fire burst AND the frost burst; each side keeps its own rate limit
			std::uint64_t key = a_key;
			for (const auto who : { a_first, a_second }) {
				if (const auto* s = AnyStream(who)) {
					BurstAt(s->entry, a_at, key, a_cfg);
				}
				key ^= 0x9E3779B97F4A7C15ull;
			}
		}

		void Shake(float a_base, Vec3 a_at, float a_seconds, const Core::Config& a_cfg)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !Core::Finite(a_at)) {
				return;
			}
			const float strength = Core::ShakeStrength(a_base, Core::Length(a_at - V(player->GetPosition())), a_cfg.struggle);
			if (strength > 0.0f) {
				RE::ShakeCamera(strength, N(a_at), a_seconds);
			}
		}

		void Message(const std::string& a_text, const Core::Config& a_cfg)
		{
			if (a_cfg.struggle.messages && !a_text.empty()) {
				RE::SendHUDMessage::ShowHUDMessage(a_text.c_str());
			}
		}

		// "began", "overwhelmed" ... to other plugins (CFST) and, with ModEvents on, to Papyrus
		void Tell(std::uint8_t a_event, const Core::Struggle& a_s, const Rec& a_r, Vec3 a_at, std::uint8_t a_winner, const Core::Config& a_cfg)
		{
			CrossfireAPI::Struggle msg;
			msg.event = a_event;
			msg.elementA = static_cast<std::uint8_t>(a_r.a.leading.element);
			msg.elementB = static_cast<std::uint8_t>(a_r.b.leading.element);
			msg.winner = a_winner;
			msg.x = a_at.x;
			msg.y = a_at.y;
			msg.z = a_at.z;
			msg.balance = a_s.balance;
			auto ra = a_r.a.ref.get();
			auto rb = a_r.b.ref.get();
			msg.shooterA = ra ? ra->GetFormID() : 0;
			msg.shooterB = rb ? rb->GetFormID() : 0;
			if (auto* messaging = SKSE::GetMessagingInterface()) {
				messaging->Dispatch(CrossfireAPI::kStruggle, &msg, sizeof(msg), nullptr);
			}
			if (!a_cfg.modEvents) {
				return;
			}
			if (auto* source = SKSE::GetModCallbackEventSource()) {
				constexpr const char* kNames[]{ "began", "overwhelmed", "gave way", "draw", "ended" };
				float                 num = 0.0f;
				if (a_winner == 1) {
					num = a_r.a.player ? 1.0f : (a_r.b.player ? -1.0f : 0.0f);
				} else if (a_winner == 2) {
					num = a_r.b.player ? 1.0f : (a_r.a.player ? -1.0f : 0.0f);
				}
				auto*                  sender = a_winner == 2 ? rb.get() : ra.get();
				SKSE::ModCallbackEvent event{ "ArcaneOverdrive_Struggle",
					std::format("{},{},{}", kNames[std::min<std::size_t>(a_event, 4)], Core::ElementName(a_r.a.leading.element),
						Core::ElementName(a_r.b.leading.element)),
					num, sender };
				source->SendEvent(&event);
			}
		}

		[[nodiscard]] bool CanStagger(RE::Actor* a_actor, bool a_player, const Core::Config& a_cfg)
		{
			const auto& sc = a_cfg.struggle;
			if (!sc.stagger || (a_player && !sc.staggerPlayer) || !a_actor) {
				return false;
			}
			return !a_actor->IsDead() && !a_actor->IsInKillMove() && !a_actor->IsOnMount() && !a_actor->IsInRagdollState() && !a_actor->IsDragon();
		}

		void Stagger(RE::Actor* a_actor, float a_magnitude)
		{
			a_actor->SetGraphVariableFloat("staggerMagnitude", a_magnitude);
			a_actor->NotifyAnimationGraph("staggerStart");
		}

		void BreakCast(RE::Actor* a_actor, const SideRec& a_side, const Core::Config& a_cfg)
		{
			if (!a_cfg.struggle.breakCast || a_side.breath || a_side.dragon || !a_side.leading.spell) {
				return;
			}
			for (auto* mc : a_actor->GetActorRuntimeData().magicCasters) {
				if (mc && mc->currentSpell == a_side.leading.spell) {
					mc->InterruptCast(false);  // only the hand that lost
				}
			}
		}

		// the game's own Fear on up to 8 enemies of the player near the victim and below its level
		void Intimidate(RE::Actor* a_victim)
		{
			auto* fear = FearSpell();
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* lists = RE::ProcessLists::GetSingleton();
			auto* caster = player ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
			if (!fear || !player || !lists || !caster) {
				return;
			}
			const auto level = a_victim->GetLevel();
			const Vec3 at = V(a_victim->GetPosition());
			int        n = 0;
			lists->ForEachHighActor([&](RE::Actor* a) {
				if (n >= 8) {
					return RE::BSContainer::ForEachResult::kStop;
				}
				if (a && a != a_victim && Alive(a) && a->GetLevel() < level && a->IsHostileToActor(player) &&
					Core::Length(V(a->GetPosition()) - at) <= 1750.0f) {
					caster->CastSpellImmediate(fear, false, a, 1.0f, false, 1000.0f, player);
					++n;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		void Overwhelmed(Core::Struggle& a_s, Rec& a_r, bool a_aWon, RE::Actor* a_actorA, RE::Actor* a_actorB, const Core::Front& a_front,
			const Core::Config& a_cfg)
		{
			const auto& sc = a_cfg.struggle;
			auto&       gs = Counters();
			SideRec&    W = a_aWon ? a_r.a : a_r.b;
			SideRec&    L = a_aWon ? a_r.b : a_r.a;
			RE::Actor*  winner = a_aWon ? a_actorA : a_actorB;
			RE::Actor*  loser = a_aWon ? a_actorB : a_actorA;
			const auto  lShooter = a_aWon ? a_s.b : a_s.a;
			const float lead = std::abs(a_s.lead);
			gs.overwhelms.fetch_add(1, std::memory_order_relaxed);

			// 1. every stream of the loser goes now (it keeps fizzling while it reels: Reeling)
			if (const auto it = gBy.find(lShooter); it != gBy.end()) {
				for (const auto i : it->second) {
					Snuff(gStreams[i].entry);
				}
			}
			bool killed = false;
			if (loser) {
				BreakCast(loser, L, a_cfg);  // 2
				if (CanStagger(loser, L.player, a_cfg)) {
					Stagger(loser, Core::StaggerFor(a_s.lead, sc));  // 3
				}
				if (sc.overwhelmDamage > 0.0f && winner) {  // 4
					const float resist = W.leading.resist == RE::ActorValue::kNone ? 0.0f : AV(loser, W.leading.resist);
					const float hit = Core::OverwhelmHit(W.leading.magnitude * W.power, a_s.lead, resist, sc);
					if (hit > 0.0f) {
						loser->DoDamage(hit, winner, false);
						if (W.leading.absorb) {
							const float missing = -winner->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth);
							if (std::isfinite(missing) && missing > 0.0f) {
								winner->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kHealth, std::min(hit, missing));
							}
						}
					}
				}
				killed = loser->IsDead() || AV(loser, RE::ActorValue::kHealth) <= 0.0f;        // 5
				if (killed && sc.finishers && !L.player && !loser->IsEssential() && winner) {  // 6
					if (auto* proc = loser->GetActorRuntimeData().currentProcess) {
						const Vec3 from = a_aWon ? a_s.muzzleA : a_s.muzzleB;
						proc->KnockExplosion(loser, N(from), sc.finisherForce * (0.5f + lead));
					}
				}
				if (killed && sc.intimidate && W.player) {  // 7
					Intimidate(loser);
				}
			}
			if (W.player) {  // 8
				gs.won.fetch_add(1, std::memory_order_relaxed);
				if (!W.breath && sc.xp > 0.0f && W.leading.skill != RE::ActorValue::kNone) {
					if (auto* player = RE::PlayerCharacter::GetSingleton()) {
						player->AddSkillExperience(W.leading.skill, killed ? 2.0f * sc.xp : sc.xp);
					}
				}
			}
			if (L.player) {
				gs.lost.fetch_add(1, std::memory_order_relaxed);
			}
			if (W.player) {  // 9
				Message(Translation::Fill(Translation::T("You overwhelm {}."), L.name), a_cfg);
			} else if (L.player) {
				Message(Translation::Fill(Translation::T("{} overwhelms you."), W.name), a_cfg);
			}
			const Vec3 at = a_front.valid ? a_front.point : Core::Lerp(a_s.muzzleA, a_s.muzzleB, 0.5f);  // 10
			Shake(1.0f, at, 0.6f, a_cfg);
			BurstFor(a_aWon ? a_s.a : a_s.b, lShooter, at, Core::PairKey(a_s.a, a_s.b), a_cfg);
			Tell(1, a_s, a_r, at, a_aWon ? 1 : 2, a_cfg);
			if (a_cfg.debugLog) {
				SKSE::log::info("struggle: {} overwhelms {} (lead {:.2f}, {:.1f} s){}", W.name, L.name, a_s.lead, a_s.age, killed ? ", killed" : "");
			}
		}

		void Ended(Core::StruggleEvent a_event, Core::Struggle& a_s, Rec& a_r, bool a_aWon, RE::Actor* a_actorA, RE::Actor* a_actorB,
			const Core::Front& a_front, const Core::Config& a_cfg)
		{
			auto&      gs = Counters();
			const Vec3 at = a_front.valid ? a_front.point : Core::Lerp(a_s.muzzleA, a_s.muzzleB, 0.5f);
			const auto key = Core::PairKey(a_s.a, a_s.b);
			switch (a_event) {
			case Core::StruggleEvent::kOverwhelmed:
				Overwhelmed(a_s, a_r, a_aWon, a_actorA, a_actorB, a_front, a_cfg);
				break;
			case Core::StruggleEvent::kGaveWay:
				{
					gs.gaveWay.fetch_add(1, std::memory_order_relaxed);
					// aWon: B stopped
					const SideRec& stopped = a_aWon ? a_r.b : a_r.a;
					const SideRec& other = a_aWon ? a_r.a : a_r.b;
					if (other.player && !stopped.player) {
						Message(Translation::Fill(Translation::T("{} gives way."), stopped.name), a_cfg);
					}
					BurstFor(a_s.a, a_s.b, at, key, a_cfg);
					Shake(0.3f, at, 0.3f, a_cfg);
					Tell(2, a_s, a_r, at, 0, a_cfg);
					break;
				}
			case Core::StruggleEvent::kDraw:
				{
					gs.draws.fetch_add(1, std::memory_order_relaxed);
					BurstFor(a_s.a, a_s.b, at, key, a_cfg);
					const float half = 0.5f * Core::StaggerFor(0.0f, a_cfg.struggle);
					for (auto [actor, side] : { std::pair<RE::Actor*, SideRec*>{ a_actorA, &a_r.a }, std::pair<RE::Actor*, SideRec*>{ a_actorB, &a_r.b } }) {
						if (!actor) {
							continue;
						}
						if (CanStagger(actor, side->player, a_cfg)) {
							Stagger(actor, half);
						}
						BreakCast(actor, *side, a_cfg);
					}
					if (a_r.a.player || a_r.b.player) {
						Message(Translation::T("The spells burst between you."), a_cfg);
					}
					Tell(3, a_s, a_r, at, 0, a_cfg);
					break;
				}
			case Core::StruggleEvent::kReleased:
			case Core::StruggleEvent::kCalledOff:
				Tell(4, a_s, a_r, at, 0, a_cfg);
				break;
			default:
				break;
			}
			constexpr const char* kNames[]{ "none", "overwhelmed", "gave way", "draw", "released", "called off", "gone" };
			StruggleEvent(kNames[std::min<std::size_t>(std::to_underlying(a_event), 6)], a_r.a.name, a_r.b.name, a_s.balance, a_s.age);
			if (a_cfg.debugLog && a_event != Core::StruggleEvent::kOverwhelmed) {
				SKSE::log::info("struggle: {} vs {} ended: {} (balance {:.2f}, {:.1f} s)", a_r.a.name, a_r.b.name,
					kNames[std::min<std::size_t>(std::to_underlying(a_event), 6)], a_s.balance, a_s.age);
			}
		}

		// snuff each side's particles past the front, and cut its beams there
		void Enforce(const Core::Struggle& a_s, const Core::Front& a_front, const Core::Config& a_cfg)
		{
			if (!a_front.valid) {
				return;
			}
			for (const bool sideA : { true, false }) {
				const auto it = gBy.find(sideA ? a_s.a : a_s.b);
				if (it == gBy.end()) {
					continue;
				}
				for (const auto i : it->second) {
					const auto& s = gStreams[i];
					// a spray (Flames, Frostbite, a breath) is ONE long-lived FlameProjectile at the hand that reaches out to
					// its range (measured 2026-10-01), so the particle test below never found it past the front and the
					// foe's spray still burned him through a lock (his report 2026-10-07). It is cut at the front like a beam.
					if (s.info->stream == Core::Stream::kBeam || s.projectile->GetFormType() == RE::FormType::ProjectileFlame) {
						if (!a_cfg.struggle.beamsStop) {
							continue;
						}
						auto& rd = s.projectile->GetProjectileRuntimeData();
						auto [h, fresh] = gBeams.try_emplace(s.native);
						if (fresh) {
							h->second.handle = s.handle;
							h->second.range0 = std::isfinite(rd.range) && rd.range > 0.0f ? rd.range : s.base->data.range;
						}
						const float cut = Core::BeamCut(a_front, s.now, s.dir, h->second.range0);
						if (std::isfinite(cut) && cut > 0.0f) {
							rd.range = cut;
							// MEASURED 2026-10-07 20:49 (two AI mages, Flames vs Frostbite, locked 9.9 s): both kept losing
							// health through the whole lock - a FlameProjectile does not honour a range written after launch.
							// Its hits are dropped past the cut instead (FlameAddImpact, main.cpp's hook)
							if (s.projectile->GetFormType() == RE::FormType::ProjectileFlame) {
								std::scoped_lock lock(gFlameCutLock);
								gFlameCut.insert_or_assign(s.native, cut);
							}
						}
						h->second.held = true;
						continue;
					}
					// a particle measured from its own side's muzzle: A's past the front, B's short of it
					const Core::Front& f = a_front;
					if (Core::PastFront(f, sideA, s.now, s.radius)) {
						Snuff(s.entry);
					}
				}
			}
		}

		void RestoreBeams(bool a_all)
		{
			for (auto it = gBeams.begin(); it != gBeams.end();) {
				if (!a_all && it->second.held) {
					it->second.held = false;
					++it;
					continue;
				}
				if (auto ref = it->second.handle.get()) {
					ref->GetProjectileRuntimeData().range = it->second.range0;
				}
				{
					std::scoped_lock lock(gFlameCutLock);
					gFlameCut.erase(it->first);
				}
				it = gBeams.erase(it);
			}
		}

		void Publish(const Core::Config& a_cfg)
		{
			StruggleView v;
			const auto&  sc = a_cfg.struggle;
			v.bar = sc.bar;
			v.barHeight = sc.barHeight;
			v.barScale = sc.barScale;
			v.barOpacity = sc.barOpacity;
			v.barNames = sc.barNames;
			v.barSkills = sc.barSkills;
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const auto me = player ? player->GetHandle().native_handle() : 0;
			for (const auto& [key, s] : gBook) {
				if (s.phase != Core::Phase::kLocked || (s.a != me && s.b != me)) {
					continue;
				}
				const auto rit = gRecs.find(key);
				if (rit == gRecs.end()) {
					continue;
				}
				const bool     meA = s.a == me;
				const SideRec& mine = meA ? rit->second.a : rit->second.b;
				const SideRec& theirs = meA ? rit->second.b : rit->second.a;
				v.active = true;
				v.balance = meA ? s.balance : -s.balance;  // + : pushing toward them (A pushes toward +1)
				v.lead = meA ? s.lead : -s.lead;
				v.seconds = s.age;
				v.mine = static_cast<std::uint8_t>(mine.leading.element);
				v.theirs = static_cast<std::uint8_t>(theirs.leading.element);
				v.myBreath = mine.breath;
				v.theirBreath = theirs.breath;
				auto       actorMe = ActorOf(mine.ref);
				auto       actorThem = ActorOf(theirs.ref);
				const auto skillOf = [](RE::Actor* a, const SideRec& side) {
					if (!a) {
						return 0;
					}
					if (side.breath) {
						return static_cast<int>(std::min<std::uint16_t>(a->GetLevel(), 100));
					}
					const int own = SchoolIndex(side.leading.skill);
					return static_cast<int>(own >= 0 ? AV(a, side.leading.skill) : static_cast<float>(a->GetLevel()));
				};
				v.mySkill = skillOf(actorMe.get(), mine);
				v.theirSkill = skillOf(actorThem.get(), theirs);
				std::snprintf(v.school, sizeof(v.school), "%s", mine.breath ? "Thu'um" : SchoolName(SchoolIndex(mine.leading.skill)));
				std::snprintf(v.theirSchool, sizeof(v.theirSchool), "%s", theirs.breath ? "Thu'um" : SchoolName(SchoolIndex(theirs.leading.skill)));
				std::snprintf(v.foe, sizeof(v.foe), "%s", theirs.name.c_str());
				break;
			}
			std::scoped_lock lock(gViewLock);
			gView = v;
		}
	}

	namespace Struggles
	{
		bool Reeling(std::uint32_t a_shooter) { return a_shooter != 0 && gBook.Loser(a_shooter); }

		bool Holds(std::uint32_t a, std::uint32_t b) { return gBook.Holds(a, b); }

		bool Active() { return gBook.Size() != 0 || !gBeams.empty(); }

		bool FlameLocked(RE::Projectile* a_flame)
		{
			if (!a_flame) {
				return false;
			}
			const auto       native = RE::ProjectileHandle(a_flame).native_handle();
			std::scoped_lock lock(gFlameCutLock);
			return gFlameCut.contains(native);
		}

		bool FlameHitPastFront(RE::Projectile* a_flame, const RE::NiPoint3& a_at)
		{
			if (!a_flame) {
				return false;
			}
			const auto native = RE::ProjectileHandle(a_flame).native_handle();
			float      cut = 0.0f;
			{
				std::scoped_lock lock(gFlameCutLock);
				const auto       it = gFlameCut.find(native);
				if (it == gFlameCut.end()) {
					return false;
				}
				cut = it->second;
			}
			return a_flame->GetPosition().GetDistance(a_at) > cut;
		}

		void Reset(bool a_restoreBeams)
		{
			if (a_restoreBeams) {
				RestoreBeams(true);
			}
			gBeams.clear();
			{
				std::scoped_lock lock(gFlameCutLock);
				gFlameCut.clear();
			}
			gBook.Clear();
			gRecs.clear();
			gTried.clear();
			gBy.clear();
			gStreams = {};
			std::scoped_lock lock(gViewLock);
			gView = {};
		}

		void Frame(std::span<const StreamRef> a_streams, const Core::Config& a_cfg, float a_delta, std::uint32_t a_frame)
		{
			const auto& sc = a_cfg.struggle;
			if (!sc.enabled) {
				if (Active()) {
					Reset(true);
				}
				return;
			}
			gStreams = a_streams;
			gFrame = a_frame;
			gClock += static_cast<double>(a_delta);
			for (auto& [who, list] : gBy) {
				list.clear();
			}
			gTried.clear();
			for (std::size_t i = 0; i < a_streams.size(); ++i) {
				gBy[a_streams[i].shooter].push_back(i);
			}

			auto*                      player = RE::PlayerCharacter::GetSingleton();
			const Vec3                 me = player ? V(player->GetPosition()) : Vec3{};
			const float                maxDistance = a_cfg.maxDistance;
			std::vector<std::uint64_t> gone;
			for (auto& [key, s] : gBook) {
				auto rit = gRecs.find(key);
				if (rit == gRecs.end()) {
					s.phase = Core::Phase::kCooldown;  // cannot happen; let it run out
					continue;
				}
				Rec&         r = rit->second;
				auto         actorA = ActorOf(r.a.ref);
				auto         actorB = ActorOf(r.b.ref);
				Core::Sight  sight;
				Core::Caster ca, cb;
				Core::Front  front;
				if (s.phase == Core::Phase::kLocked) {
					// a. may it go on?
					bool valid = Alive(actorA.get()) && Alive(actorB.get());
					if (valid) {
						const float da = Core::Length(V(actorA->GetPosition()) - me), db = Core::Length(V(actorB->GetPosition()) - me);
						valid = da <= maxDistance && db <= maxDistance;
					}
					if (valid && a_cfg.ignoreAllies) {
						valid = actorA->IsHostileToActor(actorB.get()) || actorB->IsHostileToActor(actorA.get());
					}
					if (valid) {
						// b. the muzzles, and how far each stream reaches
						const auto ia = gBy.find(s.a), ib = gBy.find(s.b);
						const Vec3 ma = MuzzleOf(actorA.get(), r.a.source, ia != gBy.end() ? &ia->second : nullptr);
						const Vec3 mb = MuzzleOf(actorB.get(), r.b.source, ib != gBy.end() ? &ib->second : nullptr);
						float      reachA = s.reachA, reachB = s.reachB;
						if (const auto* la = AnyStream(s.a)) {
							reachA = ReachOf(*la);
						}
						if (const auto* lb = AnyStream(s.b)) {
							reachB = ReachOf(*lb);
						}
						Core::Place(s, ma, mb, reachA, reachB);
						const float D = Core::Length(s.muzzleB - s.muzzleA);
						valid = D >= Core::KeepGap(sc) && Core::InReach(D, s.reachA, s.reachB);
					}
					sight.valid = valid;
					if (valid) {
						// c. seen
						const Seen sa = Look(s.a, s.muzzleA, s.muzzleB);
						const Seen sb = Look(s.b, s.muzzleB, s.muzzleA);
						sight.seenA = sa.seen;
						sight.seenB = sb.seen;
						if (sa.leading) {
							Take(r.a, *sa.leading);
						}
						if (sb.leading) {
							Take(r.b, *sb.leading);
						}
						// d. the casters
						ca = CasterOf(actorA.get(), r.a, sa.twoHands, r.b.player, a_cfg);
						cb = CasterOf(actorB.get(), r.b, sb.twoHands, r.a.player, a_cfg);
					}
				} else if (s.phase == Core::Phase::kDeclined) {
					const auto ia = gBy.find(s.a), ib = gBy.find(s.b);
					sight.seenA = ia != gBy.end() && !ia->second.empty();
					sight.seenB = ib != gBy.end() && !ib->second.empty();
				}

				// e. one step
				const auto step = Core::Advance(s, ca, cb, sight, a_delta, sc);
				if (s.phase == Core::Phase::kLocked || step.event != Core::StruggleEvent::kNone) {
					front = Core::FrontOf(s);
				}
				// f. carry it out
				if (step.drainA > 0.0f && actorA) {
					const float cur = AV(actorA.get(), RE::ActorValue::kMagicka);
					if (cur > 0.0f) {
						actorA->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kMagicka, std::min(step.drainA, cur));
					}
				}
				if (step.drainB > 0.0f && actorB) {
					const float cur = AV(actorB.get(), RE::ActorValue::kMagicka);
					if (cur > 0.0f) {
						actorB->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kMagicka, std::min(step.drainB, cur));
					}
				}
				if (step.spark && front.valid) {
					BurstFor(s.a, s.b, front.point, key, a_cfg);
				}
				if (s.phase == Core::Phase::kLocked && (r.a.player || r.b.player) && gClock >= r.nextRumble && front.valid) {
					r.nextRumble = gClock + 0.5;
					const float towardMe = std::max(0.0f, r.a.player ? -s.balance : s.balance);
					Shake(0.15f * (0.5f + towardMe), me, 0.3f, a_cfg);
				}
				if (step.event != Core::StruggleEvent::kNone && step.event != Core::StruggleEvent::kGone) {
					Ended(step.event, s, r, step.aWon, actorA.get(), actorB.get(), front, a_cfg);
				}

				// fronts: each side's stream ends where they meet; a loser that reels has nothing left
				if (s.phase == Core::Phase::kLocked) {
					Enforce(s, front, a_cfg);
				} else if (s.phase == Core::Phase::kBroken) {
					const auto loser = s.aWon ? s.b : s.a;
					if (const auto it = gBy.find(loser); it != gBy.end()) {
						for (const auto i : it->second) {
							Snuff(a_streams[i].entry);
						}
					}
				}
				if (step.event == Core::StruggleEvent::kGone) {
					gone.push_back(key);
				}
			}
			RestoreBeams(false);
			for (const auto k : gone) {
				gRecs.erase(k);
				gBook.Erase(k);
			}
			Publish(a_cfg);
		}

		bool TryBegin(const StreamRef& a, const StreamRef& b, Core::Vec3 a_point, const Core::Config& a_cfg)
		{
			const auto& sc = a_cfg.struggle;
			const auto  key = Core::PairKey(a.shooter, b.shooter);
			if (auto* s = gBook.Find(a.shooter, b.shooter)) {
				return s->phase == Core::Phase::kLocked || s->phase == Core::Phase::kBroken;
			}
			if (gTried.contains(key) || !a.actor || !b.actor || !a.shooter || !b.shooter) {
				return false;
			}
			if (gBook.Engaged(a.shooter) || gBook.Engaged(b.shooter)) {
				gTried.insert(key);
				return false;
			}
			const auto side = [](const StreamRef& s) {
				Core::LockSide l;
				l.stream = s.info->stream;
				l.actor = true;
				l.alive = Alive(s.actor);
				l.player = s.player;
				l.npc = s.actor->IsHumanoid();
				l.dragon = s.actor->IsDragon();
				return l;
			};
			const auto la = side(a), lb = side(b);
			const auto action = a_cfg.reactions[static_cast<std::size_t>(a.info->element)][static_cast<std::size_t>(b.info->element)];
			if (!Core::MayLock(la, lb, action, sc)) {
				gTried.insert(key);
				return false;
			}
			const auto ia = gBy.find(a.shooter), ib = gBy.find(b.shooter);
			const Vec3 ma = MuzzleOf(a.actor, a.source, ia != gBy.end() ? &ia->second : nullptr);
			const Vec3 mb = MuzzleOf(b.actor, b.source, ib != gBy.end() ? &ib->second : nullptr);
			if (!Core::Facing(ma, a.dir, mb, b.dir, sc.minGap)) {
				gTried.insert(key);
				return false;
			}
			const bool bothBeams = la.stream == Core::Stream::kBeam && lb.stream == Core::Stream::kBeam;
			const bool breath = la.stream == Core::Stream::kBreath || lb.stream == Core::Stream::kBreath;
			const bool dragon = la.dragon || lb.dragon;
			const auto seed = Core::Mix(key ^ (++gEngagements << 17) ^ gFrame);
			const auto s = Core::Begin(a.shooter, b.shooter, action, a_point, ma, mb, ReachOf(a), ReachOf(b), bothBeams, breath, dragon, seed, sc);

			// the record, in the struggle's own order (a < b)
			const bool       swapped = s.a != a.shooter;
			const StreamRef& first = swapped ? b : a;
			const StreamRef& second = swapped ? a : b;
			Rec              r;
			for (auto [rec, sref] : { std::pair<SideRec*, const StreamRef*>{ &r.a, &first }, std::pair<SideRec*, const StreamRef*>{ &r.b, &second } }) {
				rec->ref = sref->shooterHandle;
				rec->player = sref->player;
				rec->npc = sref->actor->IsHumanoid();
				rec->dragon = sref->actor->IsDragon();
				rec->creature = !rec->player && !rec->npc && !rec->dragon;
				const char* name = sref->actor->GetDisplayFullName();
				rec->name = name && *name ? name : "Someone";
				Take(*rec, *sref);
			}
			auto& put = gBook.Put(s);
			gRecs.insert_or_assign(key, r);
			if (put.phase != Core::Phase::kLocked) {
				return false;  // the chance missed: they pass through each other until one stops
			}
			auto& gs = Counters();
			gs.struggles.fetch_add(1, std::memory_order_relaxed);
			BurstFor(put.a, put.b, a_point, key, a_cfg);
			if (r.a.player || r.b.player) {
				Shake(sc.cameraShake > 0.0f ? 0.5f : 0.0f, a_point, 0.4f, a_cfg);
			}
			Tell(0, put, r, a_point, 0, a_cfg);
			if (a_cfg.debugLog) {
				auto       aa = ActorOf(r.a.ref), ab = ActorOf(r.b.ref);
				const bool na = r.b.player, nb = r.a.player;
				const auto pa = aa ? CasterOf(aa.get(), r.a, false, na, a_cfg) : Core::Caster{};
				const auto pb = ab ? CasterOf(ab.get(), r.b, false, nb, a_cfg) : Core::Caster{};
				SKSE::log::info(
					"struggle: {} ({} {:.0f}, level {:.0f}, magicka {:.2f}, log power {:.2f}) locks with {} ({} {:.0f}, level {:.0f}, "
					"magicka {:.2f}, log power {:.2f}); lead {:.2f}, meet at {:.2f}",
					r.a.name, SchoolName(SchoolIndex(r.a.leading.skill)), pa.skill, pa.level, pa.magicka, Core::LogPower(pa, sc), r.b.name,
					SchoolName(SchoolIndex(r.b.leading.skill)), pb.skill, pb.level, pb.magicka, Core::LogPower(pb, sc),
					Core::Advantage(pa, pb, sc), put.startShare);
			}
			StruggleEvent("locked", r.a.name, r.b.name, 0.0f, 0.0f);  // balance 0: the streams have just met
			return true;
		}
	}

	StruggleView StruggleNow()
	{
		std::scoped_lock lock(gViewLock);
		return gView;
	}
}

// Arcane Overdrive - SKSE plugin: auto lock - bolts aimed at the foe, and steered into oncoming spells
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Two casters' bolts that fly at each other travel nearly the same line instead of passing beside each other (they still meet
// only when both are in the air at once). A bolt is turned once, as the pass first sees it (LockAtLaunch); a lightning bolt as it picks
// its target (LockBeam, from main.cpp's hook); a bolt in flight a little each frame toward an oncoming enemy spell that would
// nearly miss it (ClashPass.cpp, Core::SteerInto). The way a projectile is turned - its velocity, its angles and its model's
// rotation together - is learned from True Directional Movement by Ershin (GPL-3.0; github.com/ersh1/TrueDirectionalMovement,
// ProjectileHook::ProjectileAimSupport). With TDM installed and a target locked, the player's bolts are TDM's to aim.

#include "ClashShared.h"

#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
// TDM's published interface, copied unchanged as its header asks; it calls GetModuleHandle with a narrow name
#pragma warning(push, 0)
#if defined(__clang__)
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Weverything"
#endif
#pragma push_macro("GetModuleHandle")
#undef GetModuleHandle
#define GetModuleHandle GetModuleHandleA
#include "TrueDirectionalMovementAPI.h"
#pragma pop_macro("GetModuleHandle")
#if defined(__clang__)
#	pragma clang diagnostic pop
#endif
#pragma warning(pop)

namespace Crossfire
{
	namespace
	{
		using Core::Vec3;

		Vec3 V(const RE::NiPoint3& a) { return { a.x, a.y, a.z }; }

		constexpr float kRadians = 0.017453292f;
		constexpr float kLaunchReach = 300.0f;     // units a bolt may have flown and still count as just launched
		constexpr float kAimedAlready = 0.99996f;  // cos 0.5 degrees: a beam already pointing this close is not turned again

		TDM_API::IVTDM1* gTdm{ nullptr };
		std::thread::id  gMainThread;  // the settings and the projectile cache are the main thread's
		std::atomic_bool gToldOffThread{ false };

		// the target's chest, else a point at chest height - not its casting hand: a bolt aimed at a hand held out in front slips
		// past the body, and two bolts fired chest to chest pass close enough to meet
		[[nodiscard]] std::optional<Vec3> AimPointOf(RE::Actor* a_target)
		{
			auto* root = a_target ? a_target->Get3D() : nullptr;
			if (!root) {
				return std::nullopt;
			}
			if (auto* chest = root->GetObjectByName("NPC Spine2 [Spn2]")) {
				return V(chest->world.translate);
			}
			return V(a_target->GetPosition()) + Vec3{ 0.0f, 0.0f, 0.7f * a_target->GetHeight() };
		}

		// TDM holds a target (its lock state is only the setting): then TDM aims the player's bolts itself
		[[nodiscard]] bool TdmLocked()
		{
			return gTdm != nullptr && static_cast<bool>(gTdm->GetCurrentTarget());
		}

		// the player's target: the hostile actor nearest the bolt's way, inside the lock cone and range
		[[nodiscard]] RE::Actor* PlayerTarget(RE::PlayerCharacter* a_player, Vec3 a_from, Vec3 a_dir, const Core::Config& a_cfg)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return nullptr;
			}
			RE::Actor* best = nullptr;
			float      bestDot = std::cos(a_cfg.lockCone * kRadians);
			for (const auto& h : lists->highActorHandles) {
				auto  ref = h.get();
				auto* a = ref.get();
				if (!a || a == a_player || a->IsDead() || a->IsDisabled() || !a->IsHostileToActor(a_player)) {
					continue;
				}
				// chest height off the position: the skeleton is searched only for the one picked (LockAtLaunch)
				const Vec3  to = V(a->GetPosition()) + Vec3{ 0.0f, 0.0f, 0.7f * a->GetHeight() } - a_from;
				const float d = Core::Length(to);
				if (!(d > 1.0f) || d > a_cfg.lockRange) {
					continue;
				}
				const float dot = Core::Dot(Core::UnitOf(a_dir), to * (1.0f / d));
				if (dot > bestDot) {
					bestDot = dot;
					best = a;
				}
			}
			return best;
		}

		// whom this shooter's bolts lock onto: nullptr = nobody (the switch is off, or no target)
		[[nodiscard]] RE::Actor* TargetFor(RE::Actor* a_shooter, Vec3 a_from, Vec3 a_dir, const Core::Config& a_cfg)
		{
			if (!a_shooter) {
				return nullptr;
			}
			// As<RE::PlayerCharacter>() never matches: the form type table knows only Character for actors
			if (a_shooter->IsPlayerRef()) {
				auto* player = static_cast<RE::PlayerCharacter*>(a_shooter);
				if (!a_cfg.lockYours || TdmLocked()) {
					return nullptr;  // off, or TDM has a target locked and aims the player's bolts itself
				}
				return PlayerTarget(player, a_from, a_dir, a_cfg);
			}
			if (!a_cfg.lockOthers) {
				return nullptr;
			}
			auto  target = a_shooter->GetActorRuntimeData().currentCombatTarget.get();
			auto* t = target.get();
			return t && !t->IsDead() ? t : nullptr;
		}

		[[nodiscard]] bool Lockable(const Info& a_info, const RE::Projectile* a_p, const Core::Config& a_cfg)
		{
			if (!a_info.eligible || a_info.stream != Core::Stream::kNone || a_info.cone) {
				return false;  // sprays and held beams point at each other already; a cone is a shout
			}
			return a_p->Is(RE::FormType::ProjectileMissile) || (a_cfg.lockArrows && a_p->Is(RE::FormType::ProjectileArrow));
		}
	}

	void FindTdm()
	{
		gMainThread = std::this_thread::get_id();  // called at kPostLoad, on the main thread
		gTdm = static_cast<TDM_API::IVTDM1*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V1));
		SKSE::log::info("auto lock: True Directional Movement {}", gTdm ? "found - its locked target aims your bolts" : "not loaded");
	}

	void TurnProjectile(RE::Projectile* a_p, Vec3 a_dir)
	{
		const Vec3 d = Core::UnitOf(a_dir);
		if (!a_p || !Core::Finite(d) || Core::Length(d) < 0.5f) {
			return;
		}
		auto&       rd = a_p->GetProjectileRuntimeData();
		const float speed = rd.linearVelocity.Length();
		if (std::isfinite(speed) && speed > 0.0f) {
			rd.linearVelocity = RE::NiPoint3{ d.x, d.y, d.z } * speed;
		}
		const auto angles = Core::AnglesFromDirection(d);
		a_p->data.angle.x = angles.pitch;
		a_p->data.angle.z = angles.heading;
		if (auto* node = a_p->Get3D2()) {
			const auto b = Core::LookBasis(d);
			auto&      m = node->local.rotate;
			const Vec3 cols[3]{ b.right, b.forward, b.up };
			for (int c = 0; c < 3; ++c) {
				m.entry[0][c] = cols[c].x;
				m.entry[1][c] = cols[c].y;
				m.entry[2][c] = cols[c].z;
			}
		}
	}

	bool LockAtLaunch(RE::Projectile* a_p, const Info& a_info, RE::Actor* a_shooter, const Core::Config& a_cfg)
	{
		if (!a_p || !Lockable(a_info, a_p, a_cfg)) {
			return false;
		}
		const auto& rd = a_p->GetProjectileRuntimeData();
		// at launch only: a bolt first seen further out (it flew into range, or was in flight at a load) keeps its way
		if (!(rd.distanceMoved < kLaunchReach)) {
			return false;
		}
		const Vec3 from = V(a_p->GetPosition());
		const Vec3 dir = Core::UnitOf(V(rd.linearVelocity));
		auto*      target = TargetFor(a_shooter, from, dir, a_cfg);
		const auto aim = AimPointOf(target);
		if (!aim) {
			return false;
		}
		const auto way = Core::LockDirection(from, dir, *aim, a_cfg.lockCone * kRadians, a_cfg.lockRange);
		if (!way) {
			return false;
		}
		TurnProjectile(a_p, *way);
		Counters().locked.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void LockBeam(RE::Projectile* a_p)
	{
		if (std::this_thread::get_id() != gMainThread) {
			if (!gToldOffThread.exchange(true)) {
				SKSE::log::info("auto lock: a lightning bolt picked its target off the main thread - such bolts are not aimed");
			}
			return;
		}
		const auto& cfg = Live();
		if (!a_p || !cfg.enabled) {
			return;
		}
		const auto* base = a_p->GetProjectileBase();
		if (!base) {
			return;
		}
		const Info& info = InfoOf(a_p, base);
		if (!info.eligible || info.kind != Core::Kind::kBeam || info.stream != Core::Stream::kNone) {
			return;  // a one-shot bolt (Lightning Bolt) only: a held beam points at its foe already
		}
		auto       shooterRef = a_p->GetProjectileRuntimeData().shooter.get();
		auto*      shooter = shooterRef ? shooterRef->As<RE::Actor>() : nullptr;
		const Vec3 from = V(a_p->GetPosition());
		const Vec3 dir = Core::DirectionFromAngles(a_p->data.angle.x, a_p->data.angle.z);
		const auto aim = AimPointOf(TargetFor(shooter, from, dir, cfg));
		const auto way = aim ? Core::LockDirection(from, dir, *aim, cfg.lockCone * kRadians, cfg.lockRange) : std::nullopt;
		if (!way || Core::Dot(Core::UnitOf(dir), *way) > kAimedAlready) {
			return;  // nothing to lock onto, or already aimed by an earlier pick of the same bolt
		}
		const auto angles = Core::AnglesFromDirection(*way);
		a_p->data.angle.x = angles.pitch;
		a_p->data.angle.z = angles.heading;
		Counters().locked.fetch_add(1, std::memory_order_relaxed);
	}
}

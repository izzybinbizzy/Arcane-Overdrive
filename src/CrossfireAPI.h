// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// For other SKSE plugins: Arcane Overdrive tells every plugin listening to it (SKSE's messaging interface,
// RegisterListener("SpellMasteryHeat", ...)) about each clash, the moment it is settled, on the game's main thread.
// Copy this header; it depends on nothing.
//
//   SKSE::GetMessagingInterface()->RegisterListener("SpellMasteryHeat", [](SKSE::MessagingInterface::Message* a_msg) {
//       if (a_msg && a_msg->type == CrossfireAPI::kClash && a_msg->dataLen >= sizeof(CrossfireAPI::Clash)) {
//           const auto* clash = static_cast<const CrossfireAPI::Clash*>(a_msg->data);
//           ...
//       }
//   });
//
// Papyrus gets the mod event "ArcaneOverdrive_Clash" (see README.md).

#pragma once

#include <cstdint>

namespace CrossfireAPI
{
	inline constexpr std::uint32_t kClash = 'CFCL';

	// elements, as numbers: 0 Fire, 1 Frost, 2 Shock, 3 Poison, 4 Arcane, 5 Physical, 6 Force
	struct Clash
	{
		std::uint32_t version{ 1 };
		float         x{ 0 }, y{ 0 }, z{ 0 };  // where they met, in world units
		std::uint8_t  elementA{ 0 }, elementB{ 0 };
		std::uint8_t  flags{ 0 };  // kDestroyedA | kDestroyedB | kWeakenedA | kWeakenedB
		std::uint8_t  pad{ 0 };
		std::uint32_t shooterA{ 0 }, shooterB{ 0 };        // form IDs of who fired each (0: nobody - a trap)
		std::uint32_t projectileA{ 0 }, projectileB{ 0 };  // form IDs of the projectile records (BGSProjectile)
	};

	// a spell struggle (two streams locked together) began or ended; Papyrus gets "ArcaneOverdrive_Struggle" (see README.md)
	inline constexpr std::uint32_t kStruggle = 'CFST';

	struct Struggle
	{
		std::uint32_t version{ 1 };
		std::uint8_t  event{ 0 };  // 0 began, 1 overwhelmed, 2 gave way, 3 draw, 4 ended quietly
		std::uint8_t  elementA{ 0 }, elementB{ 0 };
		std::uint8_t  winner{ 0 };                   // 0 none, 1 A, 2 B
		float         x{ 0 }, y{ 0 }, z{ 0 };        // the meeting point
		float         balance{ 0 };                  // -1 A's hands .. +1 B's hands
		std::uint32_t shooterA{ 0 }, shooterB{ 0 };  // form IDs
	};

	inline constexpr std::uint8_t kDestroyedA = 1 << 0;
	inline constexpr std::uint8_t kDestroyedB = 1 << 1;
	inline constexpr std::uint8_t kWeakenedA = 1 << 2;
	inline constexpr std::uint8_t kWeakenedB = 1 << 3;
}

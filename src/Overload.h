// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The overcharge on the spells themselves, as plain numbers. HIS ORDER 2026-10-15: "i want the spells to visually burn
// out/overload. i want the burnout to cause the spells themselves to flicker out when the burn out option is ticked ... and
// to double in size and ferocity when overload is activated, climbing/falling live throughout the depletion/overcharge rate."
// Overload.cpp puts these numbers on the casting art in the player's hands and on the player's spell projectiles; the HUD
// bars read the same power, so the bar and the spell grow together. Tested in tests/test_heat.cpp.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Overload
{
	enum class Phase : std::uint8_t
	{
		kBuilding,
		kOverdrive,
		kBurnout
	};

	// how big the spells are now, 1 = as the game made them. While heat builds it climbs to a quarter of the growth at full
	// heat; Overdrive starts at the whole growth (double size at the default 100 %) and falls with the burst's time left,
	// never below where full heat had it; the burnout is 1 (it sputters instead)
	[[nodiscard]] inline float Power(Phase a_phase, float a_heat01, float a_left01, float a_growth) noexcept
	{
		const float g = std::isfinite(a_growth) ? std::clamp(a_growth, 0.0f, 2.0f) : 0.0f;
		const float heat = std::isfinite(a_heat01) ? std::clamp(a_heat01, 0.0f, 1.0f) : 0.0f;
		const float left = std::isfinite(a_left01) ? std::clamp(a_left01, 0.0f, 1.0f) : 0.0f;
		switch (a_phase) {
		case Phase::kOverdrive:
			return 1.0f + g * std::max(0.25f, left);
		case Phase::kBurnout:
			return 1.0f;
		default:
			return 1.0f + g * 0.25f * heat * heat;
		}
	}

	// how fierce: the art's own animation runs this much faster (flames lick faster, frost streams harder)
	[[nodiscard]] inline float Ferocity(float a_power) noexcept
	{
		return std::isfinite(a_power) ? 1.0f + 0.6f * (std::clamp(a_power, 1.0f, 3.0f) - 1.0f) : 1.0f;
	}

	// a value eased toward its target at a_rate a second: the growth follows the meter without a jump
	[[nodiscard]] inline float Ease(float a_now, float a_target, float a_rate, float a_dt) noexcept
	{
		if (!std::isfinite(a_now)) {
			return a_target;
		}
		const float dt = std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, 0.25f) : 0.0f;
		const float k = 1.0f - std::exp(-std::max(a_rate, 0.0f) * dt);
		return a_now + (a_target - a_now) * k;
	}

	[[nodiscard]] constexpr std::uint32_t Hash(std::uint32_t a_x) noexcept
	{
		a_x ^= a_x >> 16;
		a_x *= 0x7FEB352Du;
		a_x ^= a_x >> 15;
		a_x *= 0x846CA68Bu;
		a_x ^= a_x >> 16;
		return a_x;
	}

	// the burnout's sputter: whether a spell shows at this instant, 16 times a second. a_left01 = the cooldown left (1 at its
	// start): at the start a spell is out four times in five, flashing back; it steadies as the cooldown ends and is never out
	// in its last twentieth. a_seed keeps the two hands (and each projectile) from sputtering in step.
	[[nodiscard]] inline bool Shows(float a_time, float a_left01, std::uint32_t a_seed) noexcept
	{
		const float left = std::isfinite(a_left01) ? std::clamp(a_left01, 0.0f, 1.0f) : 0.0f;
		if (left <= 0.05f) {
			return true;
		}
		const double t = std::isfinite(a_time) ? static_cast<double>(a_time) : 0.0;
		const auto   tick = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(t * 16.0))) & 0xFFFFFFFFu);
		const float  roll = static_cast<float>(Hash(tick * 2654435761u ^ a_seed) & 0xFFFFu) / 65535.0f;
		return roll >= 0.8f * left;
	}

	// a light on a sputtering spell: a dim ember while the spell is out
	[[nodiscard]] constexpr float LightWhen(bool a_shows) noexcept { return a_shows ? 1.0f : 0.12f; }

	// HIS ANSWER 2026-10-18 (checklist 28 (c)): "at burnout the hand light dips, then comes back" - the hand lights drop to
	// kDipFloor as the burnout starts and ease back to whole as the cooldown runs out (a_left01 = the cooldown left, 1 at its
	// start; smoothstep, so the return has no corner). With the sputter on, the two multiply.
	inline constexpr float     kDipFloor = 0.35f;
	[[nodiscard]] inline float BurnoutDip(float a_left01) noexcept
	{
		const float back = 1.0f - (std::isfinite(a_left01) ? std::clamp(a_left01, 0.0f, 1.0f) : 0.0f);  // 0 at the start, 1 at the end
		return kDipFloor + (1.0f - kDipFloor) * back * back * (3.0f - 2.0f * back);
	}
}

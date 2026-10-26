// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The overcharge on the spells themselves, as plain numbers: their growth with the Overdrive bar, the red zone's wild look,
// Overdrive's focus, the burnout's sputter and dip. Overload.cpp puts them on the casting art in the player's hands, the
// player's projectiles, explosions, hazards and hit effects. Tested in tests/test_heat.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace Overload
{
	enum class Phase : std::uint8_t
	{
		kBuilding,
		kOverdrive,
		kBurnout
	};

	// the spells' own size, synced to the Overdrive bar: 1 empty, climbing slowly at first to 1 + growth when it is full
	// (double at the default) and held there through Overdrive; 1 in the burnout, which sputters instead
	[[nodiscard]] inline float Growth(Phase a_phase, float a_fill01, float a_growth) noexcept
	{
		const float g = std::isfinite(a_growth) ? std::clamp(a_growth, 0.0f, 2.0f) : 0.0f;
		const float f = std::isfinite(a_fill01) ? std::clamp(a_fill01, 0.0f, 1.0f) : 0.0f;
		switch (a_phase) {
		case Phase::kOverdrive:
			return 1.0f + g;
		case Phase::kBurnout:
			return 1.0f;
		default:
			return 1.0f + g * f * f * (3.0f - 2.0f * f);
		}
	}

	// an explosion, hazard or impact keeps the size it was made at: this share of the spells' growth then
	[[nodiscard]] inline float ImpactSize(float a_power, float a_share) noexcept
	{
		const float p = std::isfinite(a_power) ? std::max(a_power, 1.0f) : 1.0f;
		return 1.0f + (p - 1.0f) * (std::isfinite(a_share) ? std::clamp(a_share, 0.0f, 1.0f) : 0.0f);
	}

	// how deep in the red zone the Heat is: 0 out of it, 0.35 at the red line rising to 1 at a full bar, times the
	// Wildness setting (0 to 2). a_red01 = the red line, 0 to 1 of the bar
	[[nodiscard]] inline float RedDepth(bool a_inRed, float a_heat01, float a_red01, float a_wildness) noexcept
	{
		if (!a_inRed) {
			return 0.0f;
		}
		const float heat = std::isfinite(a_heat01) ? std::clamp(a_heat01, 0.0f, 1.0f) : 0.0f;
		const float red = std::isfinite(a_red01) ? std::clamp(a_red01, 0.0f, 0.99f) : 0.85f;
		const float deep = std::clamp((heat - red) / std::max(1.0f - red, 0.01f), 0.0f, 1.0f);
		return (0.35f + 0.65f * deep) * (std::isfinite(a_wildness) ? std::clamp(a_wildness, 0.0f, 2.0f) : 0.0f);
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

	struct Wild
	{
		float size{ 1.0f };   // times the size it has
		float speed{ 1.0f };  // times its animation's speed
		bool  shows{ true };
	};

	// the red zone's look at an instant, 12 steps a second: uneven size pulses (mostly small dips, now and then a swell),
	// the art's animation racing or stalling, and a step out now and then (one in five at most - a lighter burnout sputter).
	// a_wild = RedDepth; a_seed keeps the hands and each spell apart
	[[nodiscard]] inline Wild WildAt(float a_time, float a_wild, std::uint32_t a_seed) noexcept
	{
		const float w = std::isfinite(a_wild) ? std::clamp(a_wild, 0.0f, 2.0f) : 0.0f;
		if (w <= 0.0f) {
			return {};
		}
		const double t = std::isfinite(a_time) ? static_cast<double>(a_time) : 0.0;
		const auto   tick = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(t * 12.0))) & 0xFFFFFFFFu);
		const auto   roll = [&](std::uint32_t a_salt) { return static_cast<float>(Hash(tick * 2654435761u ^ a_seed ^ a_salt) & 0xFFFFu) / 65535.0f; };
		const float  pick = roll(0x1F3Du), amount = roll(0x7A11u), pace = roll(0xC0DEu), out = roll(0x5EEDu);
		Wild         wl;
		wl.size = pick < 0.3f ? 1.0f + 0.3f * w * amount : 1.0f - 0.12f * w * amount;
		wl.speed = pace < 0.5f ? 1.0f + 1.2f * w * amount : std::max(0.15f, 1.0f - 0.6f * w * amount);
		wl.shows = out >= 0.1f * w;
		return wl;
	}

	// Overdrive focuses the wild look: as it fires the spells draw in (0.6 of their size at 0.2 s), swell back past it (1.15
	// at 0.4 s) and are steady from kFocusSeconds. a_since = seconds since Overdrive began; 1 outside that
	inline constexpr float     kFocusSeconds = 0.6f;
	[[nodiscard]] inline float FocusPull(float a_since) noexcept
	{
		if (!std::isfinite(a_since) || a_since <= 0.0f || a_since >= kFocusSeconds) {
			return 1.0f;
		}
		constexpr std::array<std::pair<float, float>, 4> kKeys{ { { 0.0f, 1.0f }, { 0.2f, 0.6f }, { 0.4f, 1.15f }, { kFocusSeconds, 1.0f } } };
		for (std::size_t i = 1; i < kKeys.size(); ++i) {
			if (a_since <= kKeys[i].first) {
				const float u = (a_since - kKeys[i - 1].first) / (kKeys[i].first - kKeys[i - 1].first);
				return kKeys[i - 1].second + (kKeys[i].second - kKeys[i - 1].second) * u * u * (3.0f - 2.0f * u);
			}
		}
		return 1.0f;
	}

	// a controller's clock = frequency x a_at + phase: the phase that keeps that clock where it is when the frequency changes
	[[nodiscard]] inline float RetimedPhase(float a_phase, float a_at, float a_old, float a_new) noexcept
	{
		return std::isfinite(a_at) && std::isfinite(a_old) && std::isfinite(a_new) ? a_phase + a_at * (a_old - a_new) : a_phase;
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

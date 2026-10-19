// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Glow::Scaled, copied unchanged from the fading module Illuminated and RELight - Spell Addon share (FadeGlow.h, the same
// author's): scaling a light's fade and radius from a remembered base value, with its guard against two plugins scaling
// one light. Tested in
// tests/test_heat.cpp. Nothing of the game in it.

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace Glow
{
	// A scaler that multiplies a value some other code sets (Light Placer rewrites a flickering light's fade every
	// frame, and leaves a steady one alone). Whatever the value holds that we did not write is the new base; we write
	// base * factor. Calling it twice in one frame writes the same number: it never compounds with itself.
	//
	// It CAN compound with another plugin that scales the same value the same way: each sees the other's write as a new
	// base, so the base shrinks (or grows) by the same ratio every frame, toward black or white. Over a few dozen frames a
	// light's own smooth animation (a fade-in, a slow pulse) looks just like that, so a look at the numbers alone cannot
	// tell them apart. Cause and effect can: while a loop is suspected, what we write carries a tiny random dither
	// (0.5%, far below what the eye sees). In a loop, what we read back next frame is our own write times the other
	// plugin's factor, so it follows our write exactly; a value the owner wrote pays no attention to it. So:
	//   - a base that moves by one steady ratio for 30 frames starts a probe, which needs 24 frames where the read-back
	//     followed our dither (a vote comes only on a frame where the dither changed sign: about 48 frames, under 1 s at
	//     60 fps);
	//   - the probe finds the loop: the base is frozen at its value before the drift, and the dither goes on, checking
	//     every frame; the moment the read-back stops following our writes (the loop ended, or it was never one), the
	//     freeze ends and the value read is the base again;
	//   - the probe finds the owner's own animation: nothing is frozen, and no probe runs again for 3 seconds.
	// Values may be negative (a darkness light's fade).
	struct Scaled
	{
		enum class Watch : std::uint8_t
		{
			kFollow,  // the usual case: the value read is the base
			kProbe,   // a loop is suspected: dithering, still following
			kFrozen   // a loop: the base is held, and dithering still checks it
		};

		float base{ 0.0f };
		float written{ 0.0f };  // what we wrote last (meaningful once `touched`)
		bool  touched{ false };
		Watch watch{ Watch::kFollow };
		bool  frozen{ false };  // watch == kFrozen, kept as a plain flag for the menu and the tests

		static constexpr int   kDriftFrames = 30;
		static constexpr int   kProbeFrames = 24;
		static constexpr int   kCooldownFrames = 180;
		static constexpr float kDither = 0.005f;  // half a percent, at frame rate: far below what the eye sees

		[[nodiscard]] float Apply(float a_current, float a_factor) noexcept
		{
			if (!touched) {
				base = a_current;
			} else if (a_current != written) {
				NewRead(a_current);
			}
			const float dither = watch == Watch::kFollow ? 1.0f : 1.0f + kDither * sign;
			const float v = base * a_factor * dither;
			written = v;
			touched = true;
			return v;
		}

		[[nodiscard]] bool Written() const noexcept { return touched; }

		// what to put back when we let go: our base if the value still holds our write, else leave it
		[[nodiscard]] float Restore(float a_current) const noexcept { return touched && a_current == written ? base : a_current; }

	private:
		// someone else wrote the value since our last write (usually once a frame; twice if its owner writes it in two passes, which only makes the probe decide sooner)
		void NewRead(float a_current) noexcept
		{
			if (watch != Watch::kFollow) {
				Vote(a_current);
				prevSign = sign;
				sign = NextSign();
			}
			switch (watch) {
			case Watch::kFollow:
				{
					cooldown = cooldown > 0 ? cooldown - 1 : 0;
					const float ratio = base != 0.0f ? a_current / base : 1.0f;
					const bool  moving = std::abs(ratio - 1.0f) > 0.005f;
					// a loop's ratio is the same to the last digit frame after frame; a smooth animation's creeps
					const bool steady = moving && std::abs(ratio - lastRatio) < 0.002f * std::abs(lastRatio);
					if (!steady || drift == 0) {
						driftFrom = base;
					}
					drift = steady ? drift + 1 : (moving ? 1 : 0);
					lastRatio = ratio;
					base = a_current;
					if (drift >= kDriftFrames && cooldown == 0) {
						Enter(Watch::kProbe);
					}
					break;
				}
			case Watch::kProbe:
				base = a_current;
				if (OwnerSeen()) {
					Leave(a_current);  // the owner's own animation
				} else if (votes >= kProbeFrames) {
					Enter(Watch::kFrozen);
					base = driftFrom;  // another scaler feeds our output back to us: keep the base from before the loop
				}
				break;
			case Watch::kFrozen:
				if (OwnerSeen()) {
					Leave(a_current);  // the read-back stopped following our writes: the value read is the owner's
				}
				break;
			}
		}

		// One frame's vote. x = log(what came back / what we wrote). If the owner wrote it, x carries our dither inverted,
		// so its change from last frame is about -dither * (sign - previous sign); if another scaler multiplied our write,
		// x does not follow our signs at all (it is steady, or carries that scaler's own random dither). A vote counts only
		// on a frame our sign flipped.
		void Vote(float a_current) noexcept
		{
			if (written == 0.0f || a_current == 0.0f || (a_current > 0.0f) != (written > 0.0f)) {
				haveX = false;
				return;
			}
			const float x = std::log(a_current / written);
			if (haveX && sign != prevSign) {
				const float want = -kDither * (sign - prevSign);  // what the owner's value would show: +-2 dither
				const float d = x - lastX;
				const bool  echo = std::abs(d - want) < 0.5f * std::abs(want);
				ownerVotes = (ownerVotes << 1) | (echo ? 1u : 0u);
				++votes;
			}
			lastX = x;
			haveX = true;
		}

		// most of the last 8 votes saw our dither come back inverted: the value is the owner's, not a loop's
		[[nodiscard]] bool OwnerSeen() const noexcept { return votes >= 4 && std::popcount(ownerVotes & 0xFFu) >= (votes >= 8 ? 6 : 4); }

		void Enter(Watch a_watch) noexcept
		{
			if (a_watch == Watch::kProbe) {
				votes = 0;
				ownerVotes = 0;
				haveX = false;
				if (!seeded) {
					seeded = true;
					rng = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this) * 2654435761u) | 1u;  // each its own signs
				}
			}
			watch = a_watch;
			frozen = a_watch == Watch::kFrozen;
		}

		void Leave(float a_current) noexcept
		{
			watch = Watch::kFollow;
			frozen = false;
			base = a_current;
			drift = 0;
			lastRatio = 1.0f;
			cooldown = kCooldownFrames;
		}

		float NextSign() noexcept
		{
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			return (rng & 1u) ? 1.0f : -1.0f;
		}

		float         driftFrom{ 0.0f };  // the base when the current run of steady drift began
		float         lastRatio{ 1.0f };
		int           drift{ 0 };  // consecutive frames the base moved by about the same ratio
		int           cooldown{ 0 }, votes{ 0 };
		std::uint32_t ownerVotes{ 0 };                 // the latest votes, a bit each, newest lowest: 1 = the owner's value
		float         sign{ 1.0f }, prevSign{ 1.0f };  // this frame's dither sign, and last frame's
		float         lastX{ 0.0f };
		bool          haveX{ false };
		std::uint32_t rng{ 1u };
		bool          seeded{ false };
	};
}

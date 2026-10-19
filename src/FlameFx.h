// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The flame heat bar's moving parts, as plain numbers with nothing of the game or of ImGui in it (tests/test_heat.cpp):
// HIS ORDER 2026-10-11: "Make the hud for arcane look cooler its too simple right now maybe like a fx hud if possible
// like flames". Menu.cpp draws from these:
//   Noise         smooth value noise (one and two dimensions): the flicker of every tongue and the flow of the magma fill
//   HeatColor     the heat ramp: deep red -> orange -> amber -> white-hot, by a 0..1 temperature
//   Look          what a stage looks like: how tall the tongues stand, how fast they flicker, how many embers a second,
//                 how hot the colors run (Cold nothing; Warm a low glow; Hot steady tongues; Overheating tall uneven flames;
//                 Overdrive white-hot with sparks; the cooldown - burnout - smoke and dying embers)
//   Particles     a fixed pool of embers / sparks / smoke puffs: spawned along the fill, they rise, sway and fade; the
//                 oldest gives way when the pool is full, so it can never grow

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace FlameFx
{
	// ------------------------------------------------------------------ noise
	[[nodiscard]] inline float Hash(std::int32_t a_x, std::int32_t a_y = 0) noexcept
	{
		auto h = static_cast<std::uint32_t>(a_x) * 374761393u + static_cast<std::uint32_t>(a_y) * 668265263u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;
		return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);  // 0..1
	}

	[[nodiscard]] inline float Smooth(float a_t) noexcept { return a_t * a_t * (3.0f - 2.0f * a_t); }

	// 0..1, continuous in x
	[[nodiscard]] inline float Noise1(float a_x) noexcept
	{
		if (!std::isfinite(a_x)) {
			return 0.5f;
		}
		const float f = std::floor(a_x);
		const auto  i = static_cast<std::int32_t>(std::fmod(f, 1048576.0f));
		const float t = Smooth(a_x - f);
		return Hash(i) + (Hash(i + 1) - Hash(i)) * t;
	}

	// 0..1, continuous in x and y
	[[nodiscard]] inline float Noise2(float a_x, float a_y) noexcept
	{
		if (!std::isfinite(a_x) || !std::isfinite(a_y)) {
			return 0.5f;
		}
		const float fx = std::floor(a_x), fy = std::floor(a_y);
		const auto  ix = static_cast<std::int32_t>(std::fmod(fx, 1048576.0f));
		const auto  iy = static_cast<std::int32_t>(std::fmod(fy, 1048576.0f));
		const float tx = Smooth(a_x - fx), ty = Smooth(a_y - fy);
		const float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
		const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
		return top + (bottom - top) * ty;
	}

	// two octaves: a flame's flicker is a slow sway with a faster shiver on it
	[[nodiscard]] inline float Flicker(float a_x, float a_time) noexcept
	{
		return std::clamp(0.65f * Noise2(a_x, a_time) + 0.35f * Noise2(a_x * 2.3f + 17.0f, a_time * 2.7f), 0.0f, 1.0f);
	}

	// ------------------------------------------------------------------ the heat ramp
	struct Rgb
	{
		float r{ 0.0f }, g{ 0.0f }, b{ 0.0f };
	};

	// 0 deep ember red, 0.35 orange, 0.65 amber, 0.85 yellow, 1 white-hot
	[[nodiscard]] inline Rgb HeatColor(float a_t) noexcept
	{
		constexpr std::array<Rgb, 5>   kStops{ Rgb{ 0.55f, 0.06f, 0.03f }, Rgb{ 1.0f, 0.36f, 0.08f }, Rgb{ 1.0f, 0.62f, 0.18f },
			Rgb{ 1.0f, 0.86f, 0.45f }, Rgb{ 1.0f, 0.98f, 0.92f } };
		constexpr std::array<float, 5> kAt{ 0.0f, 0.35f, 0.65f, 0.85f, 1.0f };
		const float                    t = std::isfinite(a_t) ? std::clamp(a_t, 0.0f, 1.0f) : 0.0f;
		for (std::size_t i = 1; i < kStops.size(); ++i) {
			if (t <= kAt[i]) {
				const float k = (t - kAt[i - 1]) / (kAt[i] - kAt[i - 1]);
				return { kStops[i - 1].r + (kStops[i].r - kStops[i - 1].r) * k, kStops[i - 1].g + (kStops[i].g - kStops[i - 1].g) * k,
					kStops[i - 1].b + (kStops[i].b - kStops[i - 1].b) * k };
			}
		}
		return kStops.back();
	}

	// ------------------------------------------------------------------ what a stage looks like
	enum class Mood : std::uint8_t
	{
		kCold,
		kWarm,
		kHot,
		kOverheating,
		kOverdrive,
		kBurnout  // the cooldown after Overdrive
	};

	struct Look
	{
		float tongue{ 0.0f };       // how tall the flame tongues stand, in bar heights
		float flicker{ 0.0f };      // how fast they flicker (noise per second)
		float unsteady{ 0.0f };     // 0 a steady flame .. 1 tongues leaping and dropping
		float embers{ 0.0f };       // embers a second, along the whole fill
		float temperature{ 0.0f };  // where the fill's head sits on the heat ramp
		float sparks{ 0.0f };       // bright sparks a second (Overdrive)
		float smoke{ 0.0f };        // smoke puffs a second (burnout)
	};

	[[nodiscard]] constexpr Look LookOf(Mood a_mood) noexcept
	{
		switch (a_mood) {
		case Mood::kWarm:
			return { 1.3f, 1.4f, 0.15f, 4.0f, 0.55f, 0.0f, 0.0f };
		case Mood::kHot:
			return { 2.6f, 2.2f, 0.30f, 9.0f, 0.72f, 0.0f, 0.0f };
		case Mood::kOverheating:
			return { 4.2f, 3.4f, 0.60f, 18.0f, 0.86f, 3.0f, 0.0f };
		case Mood::kOverdrive:
			return { 5.6f, 4.6f, 0.45f, 28.0f, 1.0f, 22.0f, 0.0f };
		case Mood::kBurnout:
			return { 0.0f, 0.8f, 0.0f, 1.5f, 0.2f, 0.0f, 5.0f };
		default:
			return {};
		}
	}

	// ------------------------------------------------------------------ embers, sparks and smoke
	struct Particle
	{
		enum class Kind : std::uint8_t
		{
			kEmber,
			kSpark,
			kSmoke
		};
		float x{ 0.0f }, y{ 0.0f };    // px from the bar's left end and its top (y grows downward, so rising is negative)
		float vx{ 0.0f }, vy{ 0.0f };  // px a second
		float age{ 0.0f }, life{ 1.0f };
		float size{ 1.5f };
		float seed{ 0.0f };  // its own sway phase
		Kind  kind{ Kind::kEmber };
		bool  alive{ false };

		[[nodiscard]] float Left() const noexcept { return std::clamp(1.0f - age / std::max(life, 0.01f), 0.0f, 1.0f); }
	};

	template <std::size_t N>
	class Particles
	{
	public:
		// a small generator of its own, so the bar's look never touches anything else's random numbers
		float Rand() noexcept
		{
			_state = _state * 1664525u + 1013904223u;
			return static_cast<float>((_state >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
		}

		// one new particle somewhere on [0, a_width) of the bar's top; the oldest gives way when the pool is full
		void Spawn(Particle::Kind a_kind, float a_width, float a_barHeight, float a_scale) noexcept
		{
			if (!(a_width > 0.0f) || !std::isfinite(a_width) || !std::isfinite(a_scale)) {
				return;
			}
			auto* slot = &_pool[0];
			for (auto& p : _pool) {
				if (!p.alive) {
					slot = &p;
					break;
				}
				if (p.age / std::max(p.life, 0.01f) > slot->age / std::max(slot->life, 0.01f)) {
					slot = &p;
				}
			}
			const float k = std::clamp(a_scale, 0.25f, 4.0f);
			Particle    p;
			p.kind = a_kind;
			p.alive = true;
			p.x = Rand() * a_width;
			p.y = Rand() * a_barHeight * 0.6f;
			p.seed = Rand() * 6.2831f;
			switch (a_kind) {
			case Particle::Kind::kSpark:
				p.vx = (Rand() - 0.5f) * 140.0f * k;
				p.vy = -(90.0f + Rand() * 120.0f) * k;
				p.life = 0.35f + Rand() * 0.35f;
				p.size = (1.0f + Rand() * 1.0f) * k;
				break;
			case Particle::Kind::kSmoke:
				p.vx = (Rand() - 0.5f) * 10.0f * k;
				p.vy = -(14.0f + Rand() * 16.0f) * k;
				p.life = 1.4f + Rand() * 1.2f;
				p.size = (3.0f + Rand() * 3.0f) * k;
				break;
			default:
				p.vx = (Rand() - 0.5f) * 16.0f * k;
				p.vy = -(28.0f + Rand() * 46.0f) * k;
				p.life = 0.7f + Rand() * 1.1f;
				p.size = (1.0f + Rand() * 1.4f) * k;
				break;
			}
			*slot = p;
		}

		// spawns at a_rate a second (fractions carried over), every kind on its own clock
		void Emit(Particle::Kind a_kind, float a_rate, float a_delta, float a_width, float a_barHeight, float a_scale) noexcept
		{
			auto& carry = _carry[static_cast<std::size_t>(a_kind)];
			if (!(a_rate > 0.0f) || !std::isfinite(a_rate) || !(a_delta > 0.0f) || !std::isfinite(a_delta)) {
				return;
			}
			carry = std::min(carry + a_rate * a_delta, static_cast<float>(N));
			while (carry >= 1.0f) {
				carry -= 1.0f;
				Spawn(a_kind, a_width, a_barHeight, a_scale);
			}
		}

		// time passing: they rise, sway (embers and smoke), sparks fall back a little, smoke grows; dead ones are freed
		void Step(float a_delta, float a_time) noexcept
		{
			const float dt = std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, 0.1f) : 0.0f;
			for (auto& p : _pool) {
				if (!p.alive) {
					continue;
				}
				p.age += dt;
				if (p.age >= p.life) {
					p.alive = false;
					continue;
				}
				const float sway = p.kind == Particle::Kind::kSpark ? 0.0f : std::sin(a_time * 3.1f + p.seed) * 12.0f;
				p.x += (p.vx + sway) * dt;
				p.y += p.vy * dt;
				if (p.kind == Particle::Kind::kSpark) {
					p.vy += 160.0f * dt;  // a spark arcs back down
				} else if (p.kind == Particle::Kind::kSmoke) {
					p.size += 6.0f * dt;
				}
			}
		}

		void Clear() noexcept
		{
			for (auto& p : _pool) {
				p.alive = false;
			}
			_carry.fill(0.0f);
		}

		[[nodiscard]] std::size_t Alive() const noexcept
		{
			return static_cast<std::size_t>(std::ranges::count_if(_pool, [](const Particle& p) { return p.alive; }));
		}
		[[nodiscard]] const std::array<Particle, N>& All() const noexcept { return _pool; }

	private:
		std::array<Particle, N> _pool{};
		std::array<float, 3>    _carry{};
		std::uint32_t           _state{ 0x2545F491u };
	};
}

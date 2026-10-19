// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The live spell bars as plain numbers (HudBars.cpp draws them; tested in tests/test_heat.cpp). HIS ORDER 2026-10-15: "they
// all need to be bar style, none of the potion flask or the circles and the likes. they need to be 360 effects and not static
// but dynamic and fluid ... frost needs to be ice flowing in the wind that actually moves and has particles like a frostbite
// spell in the game and flames the same thing ... high detail but optimized heavily"; "360" = BOTH (his answer the same
// night): the effect flows inside the fill AND spills out round the whole frame.
//
// How: every look is the same bar. Its fill is the spell's own texture flowing along it (HudBars.cpp); round it lives a pool
// of particles born on the filled part's whole outline - top, bottom, the head, the tail and inside - and carried by a flow
// field: the curl of a smooth noise, which turns particles without bunching them (the swirl of a fluid), plus each spell
// kind's own push - fire climbs, frost is blown along the bar, shock arcs leap round the frame and throw sparks, the absorbs
// are drawn IN from all round, bound weapons fly in and settle, poison bubbles up and drips. Each kind's numbers are a
// Profile; "body" particles wear the spell's own texture (its projectile's / casting art's particle texture, SpellArt.cpp),
// "flecks" a built-in sprite (embers, ice shards, snowflakes, sparks).
// Cost: at most kMax particles and kArcs arcs, stepped in one pass; no allocation after the first frame.

#pragma once

#include "SpellLook.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace BarFx
{
	struct Rgb
	{
		float r{ 1.0f }, g{ 1.0f }, b{ 1.0f };
	};

	enum class Motion : std::uint8_t
	{
		kRise,      // fire: tongues and embers climb and curl (Flames, Firebolt)
		kWind,      // frost: blown along the bar, shards spinning, mist streaming (Frostbite's spray)
		kArc,       // shock: arcs leap round the frame and throw sparks (Sparks)
		kInward,    // absorb: motes drawn in from all round (Vampiric Drain's stream, into the caster)
		kFloat,     // healing, sun: soft motes rising slowly
		kSwirl,     // illusion, conjuration: mist circling round the bar
		kAssemble,  // bound weapons: shards fly in and settle along the fill
		kBubble,    // poison: bubbles rising, drops falling from the underside
		kDrift      // generic, alteration: glints drifting in the flow
	};

	// the built-in sprites: the cells of BarSprites.dds, left to right (`bar sprites.py` draws them)
	enum class Sprite : std::uint8_t
	{
		kGlow,
		kWisp,
		kShard,
		kFlake,
		kStreak,
		kBubble,
		kMist,
		kStar
	};
	inline constexpr int kSpriteCells = 8;

	enum Side : std::size_t
	{
		kTop,
		kBottom,
		kHead,
		kTail,
		kInside,
		kSides
	};

	struct Profile
	{
		Motion                    motion;
		Rgb                       core, edge;     // a young and an old particle's color when the spell gives none
		float                     rate;           // particles a second at full fill and heat, Bar size 100 %
		float                     life0, life1;   // seconds, the shortest and the longest
		float                     size0, size1;   // pixels at Bar size 100 %: when born, when it dies
		float                     speed;          // pixels a second: the push a particle is born with, away from the bar
		float                     wind, lift;     // pixels a second the flow carries along the bar (+ to the right) and up
		float                     swirl;          // how hard the flow field turns a particle
		float                     spin;           // radians a second, at most
		float                     spill;          // how far round the bar a particle may go, in bar heights (the 360 part)
		float                     flow;           // the fill's own texture scroll, bar lengths a second (- toward the tail)
		float                     body;           // the share of particles that wear the spell's own texture
		Sprite                    bodySprite;     // the built-in sprite a body particle wears when the spell has no texture
		Sprite                    fleck, fleck2;  // the two fleck sprites (half each)
		std::array<float, kSides> sides;          // where particles are born: top, bottom, head, tail, inside (weights)
	};

	// one Profile per SpellLook::Look, in its order
	inline constexpr std::array<Profile, SpellLook::kLooks> kProfiles{ {
		// generic: an arcane glow - glints drifting in the flow
		{ Motion::kDrift, { 0.80f, 0.62f, 1.00f }, { 0.40f, 0.25f, 0.85f }, 70.0f, 0.7f, 1.5f, 10.0f, 4.0f, 16.0f, 12.0f, 8.0f, 28.0f, 1.0f, 1.8f,
			0.12f, 0.40f, Sprite::kMist, Sprite::kStar, Sprite::kGlow, { 0.30f, 0.30f, 0.12f, 0.06f, 0.22f } },
		// fire: tongues climbing off the top and licking round the ends and the underside, embers and sparks rising
		{ Motion::kRise, { 1.00f, 0.86f, 0.50f }, { 0.90f, 0.22f, 0.04f }, 150.0f, 0.35f, 0.85f, 19.0f, 5.0f, 26.0f, 4.0f, 75.0f, 60.0f, 2.5f, 2.8f,
			0.32f, 0.55f, Sprite::kWisp, Sprite::kGlow, Sprite::kStreak, { 0.40f, 0.14f, 0.18f, 0.04f, 0.24f } },
		// frost: mist and ice blown along the bar in the wind - shards spinning, flakes tumbling, out past the head
		{ Motion::kWind, { 0.85f, 0.95f, 1.00f }, { 0.35f, 0.60f, 0.95f }, 130.0f, 0.6f, 1.4f, 12.0f, 5.0f, 22.0f, 95.0f, 10.0f, 45.0f, 4.0f, 2.4f,
			0.45f, 0.45f, Sprite::kMist, Sprite::kShard, Sprite::kFlake, { 0.28f, 0.28f, 0.10f, 0.14f, 0.20f } },
		// shock: arcs (Bar::Arcs) leap round the frame; sparks thrown from them, fast and short
		{ Motion::kArc, { 0.92f, 0.90f, 1.00f }, { 0.55f, 0.50f, 1.00f }, 80.0f, 0.12f, 0.35f, 6.0f, 2.0f, 160.0f, 0.0f, 0.0f, 10.0f, 0.0f, 2.2f,
			0.90f, 0.20f, Sprite::kGlow, Sprite::kStreak, Sprite::kGlow, { 0.30f, 0.30f, 0.20f, 0.05f, 0.15f } },
		// absorb health / magicka / stamina: life drawn in from all round, the fill flowing back toward the caster's end
		{ Motion::kInward, { 1.00f, 0.38f, 0.38f }, { 0.60f, 0.05f, 0.08f }, 90.0f, 0.5f, 1.0f, 7.0f, 3.0f, 0.0f, 0.0f, 0.0f, 20.0f, 1.0f, 2.6f,
			-0.25f, 0.35f, Sprite::kMist, Sprite::kGlow, Sprite::kStreak, { 0.30f, 0.30f, 0.15f, 0.15f, 0.10f } },
		{ Motion::kInward, { 0.45f, 0.65f, 1.00f }, { 0.10f, 0.25f, 0.85f }, 90.0f, 0.5f, 1.0f, 7.0f, 3.0f, 0.0f, 0.0f, 0.0f, 20.0f, 1.0f, 2.6f,
			-0.25f, 0.35f, Sprite::kMist, Sprite::kGlow, Sprite::kStreak, { 0.30f, 0.30f, 0.15f, 0.15f, 0.10f } },
		{ Motion::kInward, { 0.50f, 1.00f, 0.45f }, { 0.10f, 0.60f, 0.10f }, 90.0f, 0.5f, 1.0f, 7.0f, 3.0f, 0.0f, 0.0f, 0.0f, 20.0f, 1.0f, 2.6f,
			-0.25f, 0.35f, Sprite::kMist, Sprite::kGlow, Sprite::kStreak, { 0.30f, 0.30f, 0.15f, 0.15f, 0.10f } },
		// healing: warm motes rising slowly, swelling as they go
		{ Motion::kFloat, { 1.00f, 0.92f, 0.62f }, { 0.95f, 0.70f, 0.25f }, 60.0f, 0.9f, 1.8f, 8.0f, 12.0f, 10.0f, 0.0f, 22.0f, 18.0f, 0.5f, 2.0f,
			0.12f, 0.35f, Sprite::kMist, Sprite::kGlow, Sprite::kStar, { 0.35f, 0.25f, 0.10f, 0.10f, 0.20f } },
		// illusion: violet mist circling round the bar
		{ Motion::kSwirl, { 0.85f, 0.60f, 1.00f }, { 0.45f, 0.20f, 0.80f }, 70.0f, 0.9f, 1.7f, 14.0f, 20.0f, 12.0f, 8.0f, 6.0f, 70.0f, 0.8f, 2.0f,
			0.20f, 0.55f, Sprite::kMist, Sprite::kStar, Sprite::kGlow, { 0.30f, 0.30f, 0.10f, 0.10f, 0.20f } },
		// conjuration: purple wisps spiralling, the fill turning back like a portal's draw
		{ Motion::kSwirl, { 0.75f, 0.45f, 1.00f }, { 0.35f, 0.10f, 0.65f }, 80.0f, 0.8f, 1.5f, 12.0f, 4.0f, 16.0f, -10.0f, 4.0f, 85.0f, 1.5f, 2.2f,
			-0.18f, 0.45f, Sprite::kMist, Sprite::kGlow, Sprite::kStar, { 0.30f, 0.30f, 0.12f, 0.12f, 0.16f } },
		// bound weapons: spectral shards flying in and settling along the fill
		{ Motion::kAssemble, { 0.70f, 0.85f, 1.00f }, { 0.25f, 0.45f, 1.00f }, 85.0f, 0.5f, 1.0f, 7.0f, 3.0f, 0.0f, 0.0f, 0.0f, 15.0f, 3.0f, 2.2f,
			0.30f, 0.25f, Sprite::kGlow, Sprite::kShard, Sprite::kStreak, { 0.30f, 0.30f, 0.15f, 0.15f, 0.10f } },
		// alteration: stone and crystal glints drifting
		{ Motion::kDrift, { 0.95f, 0.85f, 0.65f }, { 0.60f, 0.50f, 0.35f }, 60.0f, 0.8f, 1.6f, 7.0f, 3.0f, 10.0f, 6.0f, 10.0f, 25.0f, 2.0f, 1.8f,
			0.10f, 0.30f, Sprite::kMist, Sprite::kShard, Sprite::kStar, { 0.30f, 0.30f, 0.12f, 0.08f, 0.20f } },
		// sun: gold sparks rising, a glare along the fill
		{ Motion::kFloat, { 1.00f, 0.95f, 0.70f }, { 1.00f, 0.75f, 0.25f }, 80.0f, 0.6f, 1.3f, 9.0f, 4.0f, 30.0f, 0.0f, 18.0f, 15.0f, 0.3f, 2.2f,
			0.20f, 0.30f, Sprite::kGlow, Sprite::kStar, Sprite::kStreak, { 0.35f, 0.25f, 0.15f, 0.05f, 0.20f } },
		// poison: green bubbles rising, drops falling from the underside
		{ Motion::kBubble, { 0.60f, 1.00f, 0.35f }, { 0.20f, 0.55f, 0.10f }, 70.0f, 0.8f, 1.6f, 6.0f, 9.0f, 8.0f, 0.0f, 26.0f, 14.0f, 0.0f, 2.0f,
			0.15f, 0.30f, Sprite::kMist, Sprite::kBubble, Sprite::kGlow, { 0.30f, 0.30f, 0.10f, 0.05f, 0.25f } },
	} };

	[[nodiscard]] constexpr const Profile& ProfileOf(SpellLook::Look a_look) noexcept
	{
		const auto i = static_cast<std::size_t>(a_look);
		return kProfiles[i < kProfiles.size() ? i : 0];
	}

	// ------------------------------------------------------------------ the flow field
	[[nodiscard]] constexpr std::uint32_t Hash(std::uint32_t a_x) noexcept
	{
		a_x ^= a_x >> 16;
		a_x *= 0x7FEB352Du;
		a_x ^= a_x >> 15;
		a_x *= 0x846CA68Bu;
		a_x ^= a_x >> 16;
		return a_x;
	}

	// unsigned on purpose: a corner one past the largest cell wraps instead of overflowing
	[[nodiscard]] inline float Lattice(std::uint32_t a_x, std::uint32_t a_y) noexcept
	{
		const auto h = Hash(a_x * 73856093u ^ a_y * 19349663u);
		return static_cast<float>(h & 0xFFFFu) / 65535.0f;
	}

	// smooth value noise, 0..1
	[[nodiscard]] inline float Noise(float a_x, float a_y) noexcept
	{
		if (!std::isfinite(a_x) || !std::isfinite(a_y)) {
			return 0.5f;
		}
		// far from the bar the field repeats: a float past the integers' range would make the cast below undefined
		if (std::fabs(a_x) > 1.0e6f) {
			a_x = std::fmod(a_x, 1.0e6f);
		}
		if (std::fabs(a_y) > 1.0e6f) {
			a_y = std::fmod(a_y, 1.0e6f);
		}
		const float fx = std::floor(a_x), fy = std::floor(a_y);
		const auto  ix = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(fx)) & 0xFFFFFFFFu);
		const auto  iy = static_cast<std::uint32_t>(static_cast<std::uint64_t>(static_cast<std::int64_t>(fy)) & 0xFFFFFFFFu);
		const float tx = a_x - fx, ty = a_y - fy;
		const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
		const float a = Lattice(ix, iy), b = Lattice(ix + 1u, iy), c = Lattice(ix, iy + 1u), d = Lattice(ix + 1u, iy + 1u);
		return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
	}

	// the flow: the curl of the noise - it turns without bunching or thinning, like a fluid. About -1..1 a component
	inline void Curl(float a_x, float a_y, float& a_vx, float& a_vy) noexcept
	{
		constexpr float e = 0.35f;
		a_vx = (Noise(a_x, a_y + e) - Noise(a_x, a_y - e)) / (2.0f * e);
		a_vy = -(Noise(a_x + e, a_y) - Noise(a_x - e, a_y)) / (2.0f * e);
	}

	class Rng
	{
	public:
		explicit constexpr Rng(std::uint32_t a_seed) noexcept :
			state(a_seed ? a_seed : 1u) {}
		[[nodiscard]] float Next() noexcept  // 0 <= x < 1
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return static_cast<float>(state >> 8) / 16777216.0f;
		}
		[[nodiscard]] float Range(float a_lo, float a_hi) noexcept { return a_lo + (a_hi - a_lo) * Next(); }

	private:
		std::uint32_t state;
	};

	// ------------------------------------------------------------------ the pool
	// what the meter says this frame
	struct Drive
	{
		float fill{ 0.0f };             // 0..1: how far along the bar the fill reaches
		float heat{ 0.0f };             // 0..1: how hard the effects run
		float power{ 1.0f };            // Overload::Power: Overdrive doubles the particles' number, size and speed
		bool  burnout{ false };         // the cooldown: few particles, cooling
		float w{ 240.0f }, h{ 12.0f };  // the bar, pixels
		float k{ 1.0f };                // Bar size
	};

	struct Particle
	{
		float x{ 0.0f }, y{ 0.0f }, vx{ 0.0f }, vy{ 0.0f };  // pixels from the bar's top-left corner, y down
		float age{ 0.0f }, life{ 1.0f };
		float size0{ 0.0f }, size1{ 0.0f };
		float angle{ 0.0f }, spin{ 0.0f };
		float tx{ 0.0f }, ty{ 0.0f };  // where an absorb's or a bound weapon's particle is drawn to
		bool  alive{ false };
		bool  body{ false };    // wears the spell's own texture (else a fleck)
		bool  second{ false };  // the second fleck sprite
		bool  drop{ false };    // poison: a drop falling from the underside

		[[nodiscard]] float Left() const noexcept { return life > 0.0f ? std::clamp(1.0f - age / life, 0.0f, 1.0f) : 0.0f; }
		[[nodiscard]] float Size() const noexcept { return size0 + (size1 - size0) * (1.0f - Left()); }
	};

	inline constexpr int kArcPoints = 10;
	struct Arc
	{
		std::array<float, 2 * kArcPoints> pts{};  // x, y pairs, pixels from the bar's top-left corner
		float                             age{ 0.0f }, life{ 0.0f };
		float                             width{ 1.0f };
		[[nodiscard]] bool                Alive() const noexcept { return age < life; }
	};

	inline constexpr int kMax = 320;
	inline constexpr int kArcs = 6;
	inline constexpr int kSpawnsPerStep = 48;

	class Bar
	{
	public:
		void Step(const Profile& a_p, const Drive& a_d, float a_dt, float a_time) noexcept
		{
			const float dt = std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, 0.1f) : 0.0f;
			const float time = std::isfinite(a_time) ? a_time : 0.0f;
			const Drive d = Clean(a_d);
			// how many are born: more with fill and heat, doubled with the Overdrive power, a trickle in the cooldown
			const float rate = d.fill <= 0.001f ? 0.0f :
			                                      a_p.rate * (0.25f + 0.75f * d.fill) * (0.35f + 0.65f * d.heat) * d.power *
			                                          (d.burnout ? 0.3f : 1.0f) * std::clamp(d.k, 0.5f, 2.0f);
			owed = std::min(owed + rate * dt, static_cast<float>(kSpawnsPerStep));
			while (owed >= 1.0f) {
				owed -= 1.0f;
				Spawn(a_p, d);
			}
			if (a_p.motion == Motion::kArc && !d.burnout && d.fill > 0.001f) {
				arcOwed = std::min(arcOwed + (2.0f + 6.0f * d.heat) * d.power * dt, 4.0f);
				while (arcOwed >= 1.0f) {
					arcOwed -= 1.0f;
					SpawnArc(a_p, d);
				}
			}
			for (auto& arc : arcs) {
				arc.age += dt;
			}
			const float reach = a_p.spill * d.h;
			const float cell = 1.0f / (30.0f * d.k);  // the flow's eddies: about 30 px across at Bar size 100 %
			for (auto& p : parts) {
				if (!p.alive) {
					continue;
				}
				p.age += dt;
				if (p.age >= p.life) {
					p.alive = false;
					continue;
				}
				Move(a_p, d, p, dt, time, cell);
				if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < -reach || p.x > d.w + reach || p.y < -reach || p.y > d.h + reach) {
					p.alive = false;
				}
			}
		}

		[[nodiscard]] const std::array<Particle, kMax>& All() const noexcept { return parts; }
		[[nodiscard]] const std::array<Arc, kArcs>&     Arcs() const noexcept { return arcs; }
		[[nodiscard]] int                               Alive() const noexcept
		{
			return static_cast<int>(std::count_if(parts.begin(), parts.end(), [](const Particle& a_p) { return a_p.alive; }));
		}
		void Clear() noexcept
		{
			parts = {};
			arcs = {};
			owed = 0.0f;
			arcOwed = 0.0f;
		}

	private:
		static Drive Clean(Drive a_d) noexcept
		{
			const auto ok = [](float a_v, float a_lo, float a_hi, float a_or) { return std::isfinite(a_v) ? std::clamp(a_v, a_lo, a_hi) : a_or; };
			a_d.fill = ok(a_d.fill, 0.0f, 1.0f, 0.0f);
			a_d.heat = ok(a_d.heat, 0.0f, 1.0f, 0.0f);
			a_d.power = ok(a_d.power, 1.0f, 3.0f, 1.0f);
			a_d.k = ok(a_d.k, 0.25f, 4.0f, 1.0f);
			a_d.w = ok(a_d.w, 8.0f, 4000.0f, 240.0f);
			a_d.h = ok(a_d.h, 2.0f, 400.0f, 12.0f);
			return a_d;
		}

		Particle* Free() noexcept
		{
			for (int i = 0; i < kMax; ++i) {
				auto& p = parts[static_cast<std::size_t>((next + i) % kMax)];
				if (!p.alive) {
					next = (next + i + 1) % kMax;
					return &p;
				}
			}
			return nullptr;
		}

		// a point on the filled part's outline, or inside it, and the way out of the bar from there
		void Birthplace(const Profile& a_p, const Drive& a_d, float& a_x, float& a_y, float& a_nx, float& a_ny) noexcept
		{
			const float fw = std::max(a_d.w * a_d.fill, 2.0f);
			float       total = 0.0f;
			for (const float s : a_p.sides) {
				total += std::max(s, 0.0f);
			}
			float       pick = rng.Next() * std::max(total, 1e-6f);
			std::size_t side = kInside;
			for (std::size_t i = 0; i < kSides; ++i) {
				pick -= std::max(a_p.sides[i], 0.0f);
				if (pick <= 0.0f) {
					side = i;
					break;
				}
			}
			a_nx = 0.0f;
			a_ny = 0.0f;
			switch (side) {
			case kTop:
				a_x = rng.Next() * fw;
				a_y = 0.0f;
				a_ny = -1.0f;
				break;
			case kBottom:
				a_x = rng.Next() * fw;
				a_y = a_d.h;
				a_ny = 1.0f;
				break;
			case kHead:
				a_x = fw;
				a_y = rng.Next() * a_d.h;
				a_nx = 1.0f;
				break;
			case kTail:
				a_x = 0.0f;
				a_y = rng.Next() * a_d.h;
				a_nx = -1.0f;
				break;
			default:
				{
					a_x = rng.Next() * fw;
					a_y = rng.Next() * a_d.h;
					const float ang = rng.Next() * 6.2831853f;
					a_nx = std::cos(ang);
					a_ny = std::sin(ang);
					break;
				}
			}
		}

		void Spawn(const Profile& a_p, const Drive& a_d) noexcept
		{
			auto* p = Free();
			if (!p) {
				return;
			}
			*p = {};
			p->alive = true;
			p->body = rng.Next() < a_p.body;
			p->second = rng.Next() < 0.5f;
			const float grow = 0.6f + 0.4f * a_d.power;  // Overdrive: bigger
			const float big = p->body ? 2.0f : 1.0f;
			const float jitter = rng.Range(0.75f, 1.25f);
			p->size0 = a_p.size0 * a_d.k * big * grow * jitter;
			p->size1 = a_p.size1 * a_d.k * big * grow * jitter;
			p->life = rng.Range(a_p.life0, a_p.life1) * (p->body ? 1.2f : 1.0f);
			p->spin = rng.Range(-a_p.spin, a_p.spin);
			p->angle = rng.Next() * 6.2831853f;
			const float fw = std::max(a_d.w * a_d.fill, 2.0f);
			if (a_p.motion == Motion::kInward || a_p.motion == Motion::kAssemble) {
				// born out round the bar, drawn to a point inside the fill
				const float ang = rng.Next() * 6.2831853f;
				const float out = a_d.h * a_p.spill * rng.Range(0.45f, 0.85f);
				const float cx = std::clamp(rng.Next() * fw, 0.0f, fw), cy = a_d.h * 0.5f;
				p->x = cx + std::cos(ang) * (out + a_d.h * 0.5f) * 1.6f;
				p->y = cy + std::sin(ang) * (out + a_d.h * 0.5f);
				p->x = std::clamp(p->x, -a_p.spill * a_d.h * 0.95f, a_d.w + a_p.spill * a_d.h * 0.95f);
				p->tx = cx;
				p->ty = rng.Range(0.2f, 0.8f) * a_d.h;
				return;
			}
			float nx = 0.0f, ny = 0.0f;
			Birthplace(a_p, a_d, p->x, p->y, nx, ny);
			const float push = a_p.speed * a_d.k * a_d.power * rng.Range(0.5f, 1.0f);
			p->vx = nx * push + rng.Range(-0.3f, 0.3f) * push;
			p->vy = ny * push + rng.Range(-0.3f, 0.3f) * push;
			if (a_p.motion == Motion::kBubble && !p->body && ny > 0.5f) {
				p->drop = true;  // poison drips from the underside
			}
		}

		void SpawnArc(const Profile& a_p, const Drive& a_d) noexcept
		{
			Arc* arc = nullptr;
			for (auto& a : arcs) {
				if (!a.Alive()) {
					arc = &a;
					break;
				}
			}
			if (!arc) {
				return;
			}
			const float fw = std::max(a_d.w * a_d.fill, 2.0f);
			// from a point on the filled outline to another, or out into the air round the frame (the 360 part)
			const float x0 = rng.Next() * fw, y0 = rng.Next() < 0.5f ? 0.0f : a_d.h;
			float       x1 = 0.0f, y1 = 0.0f;
			if (rng.Next() < 0.5f) {
				x1 = std::clamp(x0 + rng.Range(-0.25f, 0.25f) * a_d.w, 0.0f, fw);
				y1 = y0 > 0.0f ? 0.0f : a_d.h;
			} else {
				const float ang = rng.Next() * 6.2831853f;
				const float out = a_d.h * a_p.spill * rng.Range(0.3f, 0.75f);
				x1 = x0 + std::cos(ang) * out * 1.5f;
				y1 = (y0 > 0.0f ? a_d.h : 0.0f) + std::sin(ang) * out;
			}
			const float dx = x1 - x0, dy = y1 - y0;
			const float len = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
			const float px = -dy / len, py = dx / len;  // across the arc
			const float amp = std::min(len * 0.22f, a_d.h * 0.9f);
			for (int i = 0; i < kArcPoints; ++i) {
				const float t = static_cast<float>(i) / static_cast<float>(kArcPoints - 1);
				const float taper = std::sin(t * 3.1415927f);  // fixed at both ends
				const float off = rng.Range(-1.0f, 1.0f) * amp * taper;
				arc->pts[static_cast<std::size_t>(2 * i)] = x0 + dx * t + px * off;
				arc->pts[static_cast<std::size_t>(2 * i + 1)] = y0 + dy * t + py * off;
			}
			arc->age = 0.0f;
			arc->life = rng.Range(0.06f, 0.16f);
			arc->width = rng.Range(0.8f, 1.6f) * a_d.k * a_d.power;
			// sparks thrown where it strikes
			for (int i = 0; i < 4; ++i) {
				auto* p = Free();
				if (!p) {
					break;
				}
				*p = {};
				p->alive = true;
				p->x = x1;
				p->y = y1;
				const float ang = rng.Next() * 6.2831853f;
				const float push = a_p.speed * a_d.k * a_d.power * rng.Range(0.4f, 1.0f);
				p->vx = std::cos(ang) * push;
				p->vy = std::sin(ang) * push;
				p->life = rng.Range(a_p.life0, a_p.life1);
				p->size0 = a_p.size0 * a_d.k;
				p->size1 = a_p.size1 * a_d.k;
				p->second = false;
			}
		}

		static void Move(const Profile& a_p, const Drive& a_d, Particle& a_q, float a_dt, float a_time, float a_cell) noexcept
		{
			float cx = 0.0f, cy = 0.0f;
			Curl(a_q.x * a_cell + a_time * 0.15f, a_q.y * a_cell - a_time * 0.4f, cx, cy);
			const float k = a_d.k * a_d.power;
			float       wantX = a_p.wind * k + cx * a_p.swirl * a_d.k;
			float       wantY = -a_p.lift * k + cy * a_p.swirl * a_d.k;
			float       drag = 2.5f;
			switch (a_p.motion) {
			case Motion::kRise:
				wantY -= a_p.lift * k * (1.0f - a_q.Left());  // fire climbs faster as it goes
				drag = 4.0f;
				break;
			case Motion::kArc:
				// thrown sparks: almost no drag, falling
				a_q.vy += 140.0f * a_d.k * a_dt;
				a_q.x += a_q.vx * a_dt;
				a_q.y += a_q.vy * a_dt;
				a_q.vx *= std::max(0.0f, 1.0f - 0.8f * a_dt);
				a_q.angle = std::atan2(a_q.vy, a_q.vx);
				return;
			case Motion::kInward:
			case Motion::kAssemble:
				{
					const float pull = a_p.motion == Motion::kInward ? 2.4f : 3.2f;
					wantX = (a_q.tx - a_q.x) * pull + cx * a_p.swirl * a_d.k * 0.5f;
					wantY = (a_q.ty - a_q.y) * pull + cy * a_p.swirl * a_d.k * 0.5f;
					drag = 5.0f;
					break;
				}
			case Motion::kSwirl:
				{
					// round the bar's middle line, the flow doing the rest
					const float my = a_d.h * 0.5f;
					wantX += (a_q.y < my ? 1.0f : -1.0f) * a_p.swirl * 0.35f * a_d.k;
					break;
				}
			case Motion::kBubble:
				if (a_q.drop) {
					wantX = cx * 4.0f * a_d.k;
					wantY = 90.0f * a_d.k;  // a drop falls
				}
				break;
			default:
				break;
			}
			if (a_d.burnout) {
				wantY -= 12.0f * a_d.k;  // what is left drifts up as smoke
			}
			const float blend = std::min(1.0f, drag * a_dt);
			a_q.vx += (wantX - a_q.vx) * blend;
			a_q.vy += (wantY - a_q.vy) * blend;
			a_q.x += a_q.vx * a_dt;
			a_q.y += a_q.vy * a_dt;
			a_q.angle += a_q.spin * a_dt;
		}

		std::array<Particle, kMax> parts{};
		std::array<Arc, kArcs>     arcs{};
		Rng                        rng{ 0x9E3779B9u };
		float                      owed{ 0.0f }, arcOwed{ 0.0f };
		int                        next{ 0 };
	};

	// the fill's two texture layers: where each starts along the texture (they run at different speeds, one against the
	// other, so the fill flows instead of sliding); a_flow from the Profile
	inline void FlowUv(float a_time, float a_flow, float a_power, float& a_u0, float& a_u1) noexcept
	{
		const float t = std::isfinite(a_time) ? std::fmod(a_time, 1000.0f) : 0.0f;
		const float f = std::isfinite(a_flow) ? a_flow : 0.0f;
		const float p = std::isfinite(a_power) ? std::clamp(a_power, 1.0f, 3.0f) : 1.0f;
		a_u0 = std::fmod(t * f * p, 1.0f);
		a_u1 = std::fmod(-t * f * 0.6f * p + 0.37f, 1.0f);
	}

	// a particle's color now: the spell's (or the profile's core) when young, its edge as it dies; cooled grey-blue in the
	// cooldown
	[[nodiscard]] inline Rgb ColorOf(const Profile& a_p, const Rgb& a_spell, bool a_haveSpell, float a_left, bool a_burnout) noexcept
	{
		if (a_burnout) {
			const float c = 0.35f + 0.25f * a_left;
			return { c * 0.85f, c * 0.92f, c * 1.1f };
		}
		const Rgb   young = a_haveSpell ? a_spell : a_p.core;
		const float t = std::clamp(1.0f - a_left, 0.0f, 1.0f);
		return { young.r + (a_p.edge.r - young.r) * t * 0.7f, young.g + (a_p.edge.g - young.g) * t * 0.7f, young.b + (a_p.edge.b - young.b) * t * 0.7f };
	}
}

// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The bars' pixel shader (HLSL), compiled once at run time by HudBars.cpp: the whole look of the HUD. The bar, its frame and
// the effect are painted by one quad and the effect is cut by the frame's shape, so nothing spills out. Off the game it is
// drawn by the bar bench in Arcane's HUD work folder (bar bench.py; bar cost.py times every look on the GPU).

#pragma once

#include <string_view>

namespace Plugin
{
	// cbuffer Bar below, as the plugin fills it
	struct BarShaderConstants
	{
		float size[4];   // the quad in px; the frame's width; the corner radius
		float drive[4];  // time (s); fill 0..1; heat 0..1; power 1..2
		float state[4];  // the look (BarLook); phase (0 building, 1 Overdrive, 2 burnout); fade; sputter
		float core[4];   // the spell's hottest color; w: the Heat strip's band bottom 0..1
		float mid[4];    // its body color; w: the red line 0..1
		float deep[4];   // its deepest color; w: 1 when the Heat gate is on
		float flow[4];   // x: the flow clock (s) - it runs power times as fast, so a change of power never jumps the motion
	};
	static_assert(sizeof(BarShaderConstants) == 112);

	// the looks the shader draws (state.x); SpellLook's fourteen kinds map onto them in HudBars.cpp
	enum class BarLook : int
	{
		kGeneric = 0,
		kFire = 1,
		kFrost = 2,
		kShock = 3,
		kEssence = 4,  // the absorbs: health, magicka, stamina
		kHealing = 5,
		kIllusion = 6,
		kConjuration = 7,
		kBound = 8,
		kAlteration = 9,
		kSun = 10,
		kPoison = 11,
		kHeatStrip = 20
	};

	inline constexpr std::string_view kBarShader = R"hlsl(
// Arcane Overdrive - the bars drawn by ONE pixel shader each. Written for this mod from scratch: value noise, fractal noise,
// a particle grid, rounded-box distances - the textbook versions, nothing taken from another mod.
//
// One quad covers a whole bar (frame included); ImGui's own vertex shader feeds it (position, color, uv 0..1 across the
// quad). Everything the bar shows is computed here, per pixel, so it can never leave the frame: the effect lives in the
// channel and is cut by the frame's rounded shape. The output is PREMULTIPLIED (blend ONE, INV_SRC_ALPHA): alpha says how
// much of the scene the bar covers (the dark glass, the metal frame), and the effect's light is added on top of that.
//
// Every look is the spell's magic itself, the way the game draws it in the hands: a body of flowing light (flame, mist,
// smoke, a stream of essence) and particles moving through it (embers, snow, sparks, motes, glints). No look is a flat band
// sliding along the bar, a liquid in a glass or a pattern of lines. Budget: a few value-noise lookups and one or two particle
// grids a pixel - under a microsecond of GPU a frame at the default bar size (bar cost.py).

cbuffer Bar : register(b0)
{
	float4 gSize;   // x, y: the quad in pixels; z: the frame's width in pixels; w: the corner radius in pixels
	float4 gDrive;  // x: time (s); y: fill 0..1; z: heat 0..1; w: power 1..2 (Overdrive doubles it)
	float4 gState;  // x: the look; y: phase (0 building, 1 Overdrive, 2 burnout); z: the HUD's fade 0..1; w: the burnout sputter
	float4 gCore;   // the spell's hottest color (rgb); w: the Heat strip's band bottom 0..1
	float4 gMid;    // its body color; w: the red line 0..1
	float4 gDeep;   // its deepest color; w: 1 when the Heat gate is on
	float4 gFlow;   // x: the flow clock (s): every motion runs on it, so it speeds up with the power without ever jumping
};

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float4 col : COLOR0;
	float2 uv : TEXCOORD0;
};

// ---- noise: integer hashes of the lattice cell, exact at any coordinate (a float hash of a large coordinate loses its
// precision and the noise turns blocky after a long session)
uint HashU(uint2 v)
{
	uint h = v.x * 0x27d4eb2du ^ (v.y + 0x9e3779b9u) * 0x85ebca6bu;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	h *= 0x297a2d39u;
	h ^= h >> 15;
	return h;
}

float Hash21(float2 p)
{
	return HashU(asuint(int2(floor(p)))) * (1.0 / 4294967296.0);
}

float2 Hash22(float2 p)
{
	const uint2 i = asuint(int2(floor(p)));
	return float2(HashU(i), HashU(i + uint2(0x68e31da4u, 0xb5297a4du))) * (1.0 / 4294967296.0);
}

float ValueNoise(float2 p)
{
	const float2 i = floor(p);
	const float2 f = frac(p);
	const float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), u.x), lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
}

float Fractal(float2 p, int octaves)
{
	float sum = 0.0, amp = 0.5;
	[loop] for (int i = 0; i < octaves; ++i) {
		sum += amp * ValueNoise(p);
		p = p * 2.03 + float2(17.1, 9.2);
		amp *= 0.5;
	}
	return sum;
}

// a curl for a stream: two noise lookups that push it about, so a body of light bends and folds and never repeats (a warp
// of sines did repeat - the same fold every few bar heights)
float2 Swirl(float2 p, float t)
{
	return (float2(ValueNoise(p * 0.9 + float2(t * 0.35, 1.7)), ValueNoise(p * 0.9 + float2(5.2, -t * 0.3))) - 0.5) * 1.1;
}

// thin bright strands where a fractal crosses its middle: smoke tendrils, threads of magicka
float Wisps(float n, float width)
{
	return saturate(1.0 - abs(n - 0.5) / width);
}

// signed distance to a rounded box centred on 0
float RoundBox(float2 p, float2 halfSize, float r)
{
	const float2 q = abs(p) - halfSize + r;
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// ---- particles: at most one per cell of a grid that moves at `vel` cells a second of `t` (the flow clock); the particle
// wanders a little in its cell. Returns its offset from this pixel in px (xy), its own random number (z), 1 when the cell holds one (w)
float4 Field(float2 p, float2 cell, float2 vel, float chance, float t, float seed)
{
	const float2 g = p / cell - vel * t;
	const float2 id = floor(g);
	const float2 r = Hash22(id + seed);
	const float2 spot = 0.3 + 0.4 * Hash22(id.yx + seed * 3.0 + 101.0) + 0.1 * sin(t * (1.1 + r.y * 1.7) + r * 23.0);
	return float4((frac(g) - spot) * cell, r.y, r.x < chance ? 1.0 : 0.0);
}

// a soft round mote of `radius` px, drawn `stretch` times longer along `dir` (a spark, a flake in the wind)
float Mote(float4 f, float radius, float2 dir, float stretch)
{
	const float along = dot(f.xy, dir), across = dot(f.xy, float2(-dir.y, dir.x));
	return f.w * saturate(1.0 - length(float2(along / stretch, across)) / radius);
}

// a four-pointed twinkle: a bright core and two thin arms that flash
float Twinkle(float4 f, float radius, float t)
{
	const float d = length(f.xy);
	const float flash = 0.5 + 0.5 * sin(t * (3.0 + 4.0 * f.z) + f.z * 40.0);
	const float core = saturate(1.0 - d / radius);
	const float thin = max(radius * 0.35, 0.5);
	const float arms = saturate(1.0 - d / (radius * 2.6)) * (saturate(1.0 - abs(f.x) / thin) + saturate(1.0 - abs(f.y) / thin)) * 0.5;
	return f.w * (core * core + arms * flash) * (0.45 + 0.55 * flash);
}

// three-stop ramp: 0 deep -> 0.45 mid -> 0.8 core -> 1 near white
float3 Ramp(float i)
{
	const float3 deep = gDeep.rgb, mid = gMid.rgb, core = gCore.rgb;
	float3       c = lerp(deep * 0.35, deep, saturate(i / 0.2));
	c = lerp(c, mid, saturate((i - 0.2) / 0.3));
	c = lerp(c, core, saturate((i - 0.5) / 0.3));
	return lerp(c, float3(1.0, 0.97, 0.9), saturate((i - 0.82) / 0.18));
}

// the leading edge of the fill: a soft brightening, never a white blob
float Front(float head, float h)
{
	return exp(-head * head / (h * h * 0.6));
}

// ---- the looks: each returns the light of the effect at q (px, y up from the channel's floor), w x h the channel, head =
// px past the fill's front (negative behind it). px = h / 16: particle sizes follow the bar size. t is the plain clock (for
// flicker); every motion runs on gFlow.x

// Flames: tongues of fire leaning toward the head as if fanned by the cast, a bed of coals, embers rising and drifting
float3 Fire(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float  run = gFlow.x * 1.4;
	const float  top = lerp(0.5, 1.15, heat) * (0.85 + 0.15 * power);
	const float2 warp = float2(ValueNoise(float2(s * 1.1 - t * 0.6, v * 1.3 - run * 0.9)), ValueNoise(float2(s * 1.1 + 4.7, v * 1.3 - run * 0.8))) - 0.5;
	const float  n = Fractal(float2(s * 1.5 - v * 0.6 - t * 0.35, v * 0.9 - run) + warp * float2(1.1, 0.7), 4);
	float        i = saturate(n * 1.8 - v / top * 1.1 + 0.12);
	i = i * i * (3.0 - 2.0 * i);
	const float coal = saturate(1.0 - v * 3.2) * (0.4 + 0.35 * ValueNoise(float2(s * 3.0, t * 0.9)));
	const float hot = power - 1.0;
	float3      light = Ramp(saturate(i * (0.86 + 0.3 * hot))) * pow(i, 1.3 - 0.4 * hot) + Ramp(0.32 + 0.3 * coal + 0.3 * hot) * coal * 0.5;
	light *= (1.0 + 0.3 * hot) * (0.9 + 0.15 * Front(head, h));
	const float4 e = Field(q, float2(h * 0.55, h * 0.6), float2(0.35, 1.5), 0.22 * (0.4 + heat) * power, gFlow.x, 3.0);
	light += Ramp(0.92) * Mote(e, 1.1 * px * (0.9 + 0.3 * power), float2(0.24, 0.97), 1.7) * saturate(1.2 - v) * (0.7 + 0.5 * sin(t * 13.0 + e.z * 50.0)) * 1.3;
	return light;
}

// Frostbite: freezing breath blown along the bar in gusts, snow and ice motes in the wind, rime feathering in from the frame
float3 Frost(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float  wind = gFlow.x * 1.6;
	const float  gust = smoothstep(0.15, 0.8, ValueNoise(float2(s * 0.35 - wind * 0.5, t * 0.2)));
	const float2 curl = Swirl(float2(s * 0.7 - wind * 0.3, v * 1.6), t);
	// the breath: stretched along the wind so it streams, folded by the curl
	const float  n = Fractal(float2(s * 0.75 - wind, v * 2.4) + curl * float2(1.0, 0.5), 4);
	const float  mist = saturate(n * 1.8 - 0.5) * (0.45 + 0.55 * gust);
	float3       light = Ramp(0.18) * 0.22 + Ramp(0.35 + 0.5 * mist) * mist * (0.75 + 0.45 * heat) + Ramp(0.85) * Wisps(n, 0.045) * gust * 0.3 * (0.5 + heat);
	// rime: fine frost on the glass at the lid and the floor, creeping further in as the heat (the casting) climbs
	const float lip = min(v, 1.0 - v);
	const float rime = saturate(1.0 - lip / (0.08 + 0.14 * heat)) * Wisps(ValueNoise(float2(s * 5.0, lip * 12.0)), 0.25);
	light += Ramp(0.75) * rime * 0.4;
	// snow in the wind: a near layer (bigger, faster, streaked) and a far one; a few flakes glint
	const float4 near = Field(q, float2(h * 0.7, h * 0.45), float2(3.6, -0.35), 0.35 + 0.3 * heat, gFlow.x, 1.0);
	const float4 far = Field(q, float2(h * 0.4, h * 0.3), float2(2.2, 0.3), 0.4 + 0.3 * heat, gFlow.x, 7.0);
	light += float3(0.88, 0.95, 1.0) * (Mote(near, 0.95 * px, float2(0.995, -0.1), 2.6) * (0.7 + 0.5 * near.z) + Mote(far, 0.65 * px, float2(0.99, 0.13), 2.0) * 0.55);
	light += float3(0.92, 0.98, 1.0) * Twinkle(near, 0.85 * px, t) * step(0.75, near.z) * (0.6 + 0.8 * (power - 1.0));
	return light * (0.92 + 0.2 * Front(head, h));
}

// Sparks: short arcs that crackle between points and jump every few hundredths of a second, sparks spitting off them
float3 Shock(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float s = q.x / h, v = q.y / h, px = h / 16.0;
	const float span = max((q.x - head) / h, 1.0);  // the filled length, in bar heights
	float3      light = Ramp(0.3) * ValueNoise(float2(s * 2.0 - gFlow.x * 3.0, v * 2.0 + t * 2.0)) * 0.3 * (0.5 + heat);
	const float tick = floor(gFlow.x * 12.0), age = frac(gFlow.x * 12.0);  // about 12 strikes a second, 24 in Overdrive
	[unroll] for (int k = 0; k < 3; ++k) {
		const float  seed = tick * 1.73 + k * 31.7;
		const float2 r = Hash22(float2(seed, k + 0.5));
		if (r.x < 0.22 || (k == 2 && heat < 0.45)) {
			continue;  // this arc skips a strike - the crackle
		}
		const float len = 2.0 + 4.5 * r.y;
		const float x0 = Hash21(float2(seed, 7.7)) * max(span - len * 0.5, 0.5);
		const float u = (s - x0) / len;
		if (u < 0.0 || u > 1.0) {
			continue;
		}
		const float ends = sin(u * 3.1416);  // the arc and its glow thin out to nothing at both ends - never a cut edge
		const float y = lerp(Hash21(float2(seed, 2.1)), Hash21(float2(seed, 3.3)), u) * 0.8 + 0.1 + (Fractal(float2(u * 5.0 + seed, seed), 3) - 0.5) * 0.8 * ends;
		const float d = abs(v - y) * h / px;
		light += (Ramp(1.0) * exp(-d * d / 0.45) * 1.3 * sqrt(ends) + Ramp(0.6) * 0.55 / (1.0 + d * d * 0.3) * ends) * (1.0 - age * 0.55);
	}
	const float4 sp = Field(q, float2(h * 0.5, h * 0.45), float2(2.6, 1.2), 0.12 + 0.18 * heat, gFlow.x, 5.0);
	light += Ramp(0.95) * Mote(sp, 0.8 * px, float2(0.9, 0.42), 2.6) * (0.5 + 0.5 * step(0.5, frac(t * 9.0 + sp.z))) * 1.4;
	return light * (0.9 + 0.2 * Front(head, h));
}

// the absorbs: the target's life (or magicka, or stamina) drawn along in three twisting strands of essence, braided out of
// step, a faint glow round them and motes riding the stream
float3 Essence(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float s = q.x / h, v = q.y / h, px = h / 16.0;
	const float flow = gFlow.x * 1.8;
	const float body = ValueNoise(float2(s * 1.4 - flow * 1.2, v * 2.5));
	float3      light = Ramp(0.25 + 0.3 * body) * (0.2 + 0.3 * body) * (0.6 + 0.4 * heat);
	const float wander = ValueNoise(float2(s * 0.6 - flow * 0.4, 3.7)) * 2.4;
	[unroll] for (int k = 0; k < 3; ++k) {
		const float phase = s * (0.85 + 0.17 * k) - flow * (1.0 + 0.12 * k) + k * 2.09 + wander;
		const float drift = ValueNoise(float2(s * 1.1 - flow * 1.3, k * 7.1)) - 0.5;  // each strand frays and drifts on its own
		const float y = 0.5 + 0.3 * sin(phase) * (0.75 + 0.25 * sin(s * 0.41 + k * 1.3 - flow * 0.2)) + drift * 0.3;
		const float d = abs(v - y) * h / px;
		const float depth = 0.5 + 0.5 * cos(phase);  // a strand passing in front is brighter and thicker
		const float width = 0.7 + 1.3 * depth;
		light += Ramp(0.5 + 0.42 * depth) * exp(-d * d / (width * width)) * (0.35 + 0.65 * depth) * (0.75 + 0.35 * heat);
	}
	const float4 m = Field(q + float2(0.0, sin(t * 0.7) * h * 0.15), float2(h * 0.6, h * 0.5), float2(3.0, 0.0), 0.3 + 0.3 * heat, gFlow.x, 2.0);
	light += Ramp(0.95) * Mote(m, 0.95 * px, float2(1.0, 0.0), 1.8);
	return light * (0.9 + 0.2 * Front(head, h));
}

// Healing: warm golden light welling up from below, gently stirring, golden motes rising with a twinkle - gold, never white
float3 Healing(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float2 curl = Swirl(float2(s * 0.8, v * 1.4 - gFlow.x * 0.5), t * 0.7);
	const float  n = Fractal(float2(s * 1.0 + t * 0.15, v * 1.3 - gFlow.x * 0.55) + curl, 3);
	const float  well = 0.3 + 0.45 * exp(-v * 1.8);
	const float  i = saturate(well * (0.75 + 0.35 * heat) * (0.55 + 0.9 * n) + 0.12 * Front(head, h));
	float3       light = lerp(gDeep.rgb * 0.6, gMid.rgb, saturate(i * 1.25)) * i * 1.15 + gCore.rgb * pow(i, 3.0) * 0.45;
	const float4 m = Field(q, float2(h * 0.85, h * 0.7), float2(0.15, 0.9), 0.3 + 0.25 * heat, gFlow.x, 4.0);
	light += lerp(gMid.rgb, float3(1.0, 0.95, 0.75), 0.6) * Twinkle(m, 1.2 * px, t) * saturate(1.25 - v) * 1.2;
	// Overdrive: a radiant swell rolling along the bar
	const float roll = frac(t * 0.6) * (w / h + 2.0) - 1.0;
	light += gMid.rgb * (power - 1.0) * exp(-(s - roll) * (s - roll) * 3.0) * 0.6;
	return light;
}

// Illusion: violet mist folding on itself, a glitter of motes appearing and fading in it
float3 Illusion(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float  drift = gFlow.x * 0.35;
	const float2 curl = Swirl(float2(s * 0.8 - drift, v * 1.6), drift * 2.0) * 1.6;
	const float  mist = Fractal(float2(s * 0.9 - drift * 0.5, v * 1.6) + curl, 4);
	const float  i = saturate(smoothstep(0.28, 0.85, mist) * (0.85 + 0.45 * heat) + 0.1 * Front(head, h));
	float3       light = Ramp(saturate(0.2 + 0.65 * i)) * i * 1.1;
	const float4 g = Field(q, float2(h * 0.7, h * 0.6), float2(-0.25, 0.2), 0.35 + 0.25 * heat, gFlow.x, 6.0);
	light += Ramp(0.95) * Twinkle(g, 1.0 * px, t * 0.7) * 0.9;
	return light;
}

// Conjuration: dark violet smoke curling round a current toward the head, its edges glowing, soul-lights drawn along it
float3 Conjuration(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float  pull = gFlow.x * 1.1;
	const float2 curl = Swirl(float2(s * 0.7 - pull * 0.4, v * 1.8), t * 0.8);
	const float  smoke = Fractal(float2(s * 0.8 - pull, v * 2.2) + curl * float2(1.0, 0.6), 4);
	const float  body = smoothstep(0.35, 0.8, smoke);
	const float  rim = exp(-pow((smoke - 0.62) / 0.11, 2.0));  // each fold's bright edge, soft
	float3       light = Ramp(0.12) * 0.4 + Ramp(0.4 + 0.25 * body) * body * (0.5 + 0.4 * heat) + Ramp(0.8) * rim * 0.3 * (0.5 + heat);
	const float4 soul = Field(q, float2(h * 1.1, h * 0.55), float2(2.4, 0.0), 0.25 + 0.25 * heat, gFlow.x, 8.0);
	light += Ramp(0.97) * (Mote(soul, 1.1 * px, float2(1.0, 0.0), 1.0) + Mote(soul, 0.7 * px, float2(1.0, 0.0), 6.0) * 0.5);  // a soul-light and its tail
	return light * (0.9 + 0.2 * Front(head, h));
}

// a bound weapon's spectral fire: pale blue ghost flames, thin and bright at their edges, flickering, with ethereal sparkles
float3 Bound(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float s = q.x / h, v = q.y / h, px = h / 16.0;
	const float run = gFlow.x * 1.1;
	const float n = Fractal(float2(s * 1.6 - t * 0.5, v * 1.0 - run) + Swirl(float2(s * 0.8, v * 1.2 - run * 0.5), t), 4);
	const float top = lerp(0.65, 1.2, heat);
	const float body = saturate(n * 1.7 - v / top * 0.9 + 0.05);
	const float rim = Wisps(n - v / top * 0.5, 0.06) * saturate(1.3 - v);
	float3      light = Ramp(0.3 + 0.35 * body) * body * 0.9 + Ramp(0.9) * rim * (0.75 + 0.45 * heat);
	light += Ramp(0.2) * 0.25;  // the ether the blade is drawn from
	const float4 g = Field(q, float2(h * 0.7, h * 0.6), float2(0.6, 0.9), 0.3 + 0.25 * heat, gFlow.x, 9.0);
	light += Ramp(0.97) * Twinkle(g, 0.95 * px, t) * 1.1;
	return light * (0.9 + 0.25 * Front(head, h));
}

// Alteration: a pale gold shimmer, the air itself bending, dust of light turning in slow eddies
float3 Alteration(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float2 curl = Swirl(float2(s * 1.1, v * 1.6), gFlow.x * 0.6) * 1.3;
	const float  haze = Fractal(float2(s * 0.9 - gFlow.x * 0.25, v * 1.5) + curl, 3);
	const float  bend = 0.8 + 0.2 * sin(gFlow.x * 3.0 + haze * 9.0);  // the air bending, flickering through the haze
	float3       light = Ramp(0.38 + 0.45 * haze) * (0.3 + 0.9 * smoothstep(0.25, 0.8, haze)) * (0.75 + 0.4 * heat) * bend;
	// the dust: two layers drifting against each other, turning in a slow eddy (an offset, so it never speeds up)
	const float2 eddy = float2(cos(t * 0.5), sin(t * 0.5)) * h * 0.35;
	const float4 a = Field(q + eddy, float2(h * 0.6, h * 0.5), float2(0.4, 0.0), 0.35 + 0.25 * heat, gFlow.x, 10.0);
	const float4 b = Field(q - eddy, float2(h * 0.4, h * 0.35), float2(-0.3, 0.1), 0.3 + 0.25 * heat, gFlow.x, 11.0);
	light += Ramp(0.95) * (Twinkle(a, 1.0 * px, t) * 1.0 + Mote(b, 0.7 * px, float2(1.0, 0.0), 1.0) * 0.6);
	return light * (0.9 + 0.25 * Front(head, h));
}

// sun magic (Dawnguard's, Stendarr's): blazing gold-white radiance with heat shimmer, flaring sun-motes with long glints
float3 Sun(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float2 curl = Swirl(float2(s * 0.9 - gFlow.x * 0.3, v * 1.4 - gFlow.x * 0.6), t);
	const float  shimmer = Fractal(float2(s * 1.2 - gFlow.x * 0.35, v * 1.5 - gFlow.x * 0.8) + curl, 3);
	const float  core = exp(-pow((v - 0.45) * 2.2, 2.0));
	const float  i = saturate((0.3 + 0.4 * core) * (0.35 + 1.2 * shimmer) * (0.75 + 0.35 * heat) + 0.1 * Front(head, h));
	float3       light = lerp(gDeep.rgb * 0.8, gMid.rgb, saturate(i * 1.4)) * i * 1.05 + gCore.rgb * pow(i, 4.0) * 0.45;
	// sun-motes flaring as they drift up: a soft halo round each and a long four-pointed glint
	const float4 f = Field(q, float2(h * 1.0, h * 0.8), float2(0.45, 0.4), 0.35 + 0.2 * heat, gFlow.x, 12.0);
	light += gMid.rgb * Mote(f, 2.6 * px, float2(1.0, 0.0), 1.0) * 0.35 + float3(1.0, 0.96, 0.82) * Twinkle(f, 1.3 * px, t) * 1.2;
	return light;
}

// poison: thick green fumes rolling slowly, darker where they roil, spores drifting through
float3 Poison(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float  roll = gFlow.x * 0.5;
	const float2 curl = Swirl(float2(s * 0.6 - roll * 0.3, v * 1.5), roll) * 1.4;
	const float  fume = Fractal(float2(s * 1.0 - roll * 0.6, v * 1.6 + roll * 0.3) + curl, 4);
	const float  roil = ValueNoise(float2(s * 0.5 + t * 0.1, v * 0.8 - t * 0.15));
	const float  i = smoothstep(0.25, 0.8, fume) * (0.85 + 0.4 * heat) * (0.5 + 0.65 * roil);
	float3       light = Ramp(saturate(0.2 + 0.65 * i)) * i * 1.1;
	const float4 sp = Field(q, float2(h * 0.6, h * 0.55), float2(0.5, 0.3), 0.3 + 0.25 * heat, gFlow.x, 13.0);
	light += Ramp(0.9) * Mote(sp, 0.9 * px, float2(1.0, 0.0), 1.0) * (0.6 + 0.4 * sin(t * 2.0 + sp.z * 30.0));
	return light * (0.9 + 0.2 * Front(head, h));
}

// anything else: magicka flowing along the bar in threads of light, motes carried with it
float3 Generic(float2 q, float w, float h, float t, float heat, float power, float head)
{
	const float  s = q.x / h, v = q.y / h, px = h / 16.0;
	const float2 curl = Swirl(float2(s * 0.8 - gFlow.x * 0.6, v * 1.8), t);
	// stretched along the bar, so the bright threads run with the flow instead of closing into rings
	const float  n = Fractal(float2(s * 0.65 - gFlow.x * 0.9, v * 2.4) + curl * float2(1.0, 0.5), 4);
	const float  body = smoothstep(0.3, 0.8, n) * (1.0 - abs(v - 0.5) * 1.1);
	const float  threads = exp(-pow((n - 0.6) / 0.06, 2.0));
	float3       light = Ramp(saturate(0.25 + 0.5 * body)) * body * (0.8 + 0.4 * heat) + Ramp(0.88) * threads * 0.35 * (0.6 + 0.5 * heat);
	const float4 m = Field(q, float2(h * 0.6, h * 0.5), float2(2.2, 0.1), 0.3 + 0.25 * heat, gFlow.x, 14.0);
	light += Ramp(0.95) * Mote(m, 1.0 * px, float2(1.0, 0.0), 1.6) * 0.9;
	return light * (0.9 + 0.2 * Front(head, h));
}

// the frame: dark iron, lit from above
float3 Metal(float2 p, float2 size)
{
	const float bevel = saturate(1.0 - p.y / size.y);
	return lerp(float3(0.06, 0.055, 0.055), float3(0.3, 0.27, 0.24), bevel * bevel * bevel);
}

// the Heat bar: a thin strip of molten light, amber -> orange as it climbs, red and pulsing past the red line, the sweet spot
// a faint warm band behind the fill with bright notches at its edges
float4 Strip(float2 p, float2 size, float frame, float radius, float t, float heat, float fade)
{
	const float2 centre = size * 0.5;
	const float  outer = RoundBox(p - centre, size * 0.5 - 1.0, radius);
	const float  inner = RoundBox(p - centre, size * 0.5 - 1.0 - frame, max(radius - frame, 0.5));
	const float  cover = saturate(0.5 - outer);
	const float  inside = saturate(0.5 - inner);
	const float  w = size.x - 2.0 * frame - 2.0, h = max(size.y - 2.0 * frame - 2.0, 1.0);
	const float  x = p.x - frame - 1.0;
	const float  u = x / max(w, 1.0);
	const float  low = gCore.w, red = gMid.w, gate = gDeep.w;
	float3       glass = float3(0.035, 0.03, 0.04);
	if (gate > 0.5 && u >= low && u < red) {
		glass += float3(0.11, 0.075, 0.03);
	}
	const float filled = saturate((heat * w - x) / 1.5 + 0.5);
	const float flow = 0.8 + 0.4 * ValueNoise(float2(x / h * 0.35 - t * 2.5, t * 0.5));
	const float tip = exp(-pow((x - heat * w) / (h * 2.5), 2.0));
	float3      c = lerp(float3(1.0, 0.74, 0.28), float3(1.0, 0.4, 0.08), saturate(u / max(red, 0.05)));
	float       pulse = 1.0;
	if (u >= red) {
		c = float3(1.0, 0.14, 0.05);
		pulse = 0.6 + 0.4 * sin(t * 14.0);
	}
	float3 light = c * flow * pulse * (filled + tip * 0.5) * inside;
	if (gate > 0.5) {
		const float px = 1.0 / max(w, 1.0);
		const float notch = saturate(1.0 - abs(u - low) / (px * 1.3)) + saturate(1.0 - abs(u - red) / (px * 1.3));
		light += float3(1.0, 0.92, 0.75) * notch * cover;
	}
	const float  band = saturate(cover - inside);
	const float3 covered = lerp(glass, Metal(p, size), band);
	const float  shadow = saturate(1.0 - outer / 3.0) * (1.0 - cover) * 0.4;
	return float4((covered * cover * 0.88 + light) * fade, (cover * 0.88 + shadow) * fade);
}

float4 main(PS_INPUT input) : SV_Target
{
	const float2 size = gSize.xy;
	const float  frame = gSize.z, radius = gSize.w;
	const float2 p = input.uv * size;  // px from the quad's top-left
	const float  t = gDrive.x, fill = saturate(gDrive.y), heat = saturate(gDrive.z), power = gDrive.w;
	const int    look = (int)gState.x, phase = (int)gState.y;
	const float  fade = saturate(gState.z), sputter = saturate(gState.w);
	if (look == 20) {
		return Strip(p, size, frame, radius, t, fill, fade);
	}

	// the shape: outer edge of the frame, and the channel inside it
	const float2 centre = size * 0.5;
	const float  outer = RoundBox(p - centre, size * 0.5 - 1.0, radius);
	const float  inner = RoundBox(p - centre, size * 0.5 - 1.0 - frame, max(radius - frame, 1.0));
	const float  cover = saturate(0.5 - outer);
	const float  inside = saturate(0.5 - inner);
	const float  w = size.x - 2.0 * frame - 2.0, h = size.y - 2.0 * frame - 2.0;
	const float2 q = float2(p.x - frame - 1.0, size.y - frame - 1.0 - p.y);  // channel px, y up from the floor

	// the filled part, with a soft head (the effect thins out over ~1 bar height rather than ending on a cut)
	const float fillX = fill * w;
	const float head = q.x - fillX;
	const float filled = saturate(1.0 - head / (h * 0.9));

	float3 light = 0.0;
	[branch] if (filled > 0.0 && inside > 0.0) {
		[branch] switch (look) {
		case 1:
			light = Fire(q, w, h, t, heat, power, head);
			break;
		case 2:
			light = Frost(q, w, h, t, heat, power, head);
			break;
		case 3:
			light = Shock(q, w, h, t, heat, power, head);
			break;
		case 4:
			light = Essence(q, w, h, t, heat, power, head);
			break;
		case 5:
			light = Healing(q, w, h, t, heat, power, head);
			break;
		case 6:
			light = Illusion(q, w, h, t, heat, power, head);
			break;
		case 7:
			light = Conjuration(q, w, h, t, heat, power, head);
			break;
		case 8:
			light = Bound(q, w, h, t, heat, power, head);
			break;
		case 9:
			light = Alteration(q, w, h, t, heat, power, head);
			break;
		case 10:
			light = Sun(q, w, h, t, heat, power, head);
			break;
		case 11:
			light = Poison(q, w, h, t, heat, power, head);
			break;
		default:
			light = Generic(q, w, h, t, heat, power, head);
			break;
		}
		if (phase == 2) {  // burnout: the color drains to a grey-blue smoulder, and the sputter cuts it in and out
			const float lum = dot(light, float3(0.3, 0.5, 0.2));
			const float smoke = ValueNoise(float2(q.x / h * 1.2, q.y / h * 1.5 - t * 0.5));
			light = lerp(light * 0.35, float3(0.42, 0.48, 0.6) * (lum * 0.9 + smoke * 0.25), 0.7) * (0.25 + 0.75 * sputter);
		}
		light *= filled * inside;
	}

	// the glass the effect sits behind: dark, so the light reads on snow and on night alike; in Overdrive the filled glass itself
	// glows with the spell's deepest color, breathing
	float3 glass = float3(0.035, 0.03, 0.04);
	if (phase == 1) {
		glass += gDeep.rgb * filled * inside * (0.35 + 0.15 * sin(t * 7.0));
	}
	// only the frame's inner lip catches the light of what burns beside it (never a colored rim - a bright rim across the effect
	// is what read as "a red line through" the old bar)
	const float  band = saturate(cover - inside);
	const float  lip = saturate(1.0 - abs(inner) / 1.2) * band * filled;
	const float3 metal = Metal(p, size) + gMid.rgb * lip * heat * 0.3 * (phase == 1 ? 1.4 + 0.4 * sin(t * 9.0) : 1.0);
	const float3 covered = lerp(glass, metal, band);

	// a soft shadow just outside the frame, so the bar stands off a bright sky
	const float shadow = saturate(1.0 - outer / 4.0) * (1.0 - cover) * 0.45;

	const float  alpha = (cover * 0.88 + shadow) * fade;
	const float3 rgb = (covered * cover * 0.88 + light) * fade;
	return float4(rgb, alpha);
}
)hlsl";
}

// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Community Shaders' two words on a light of our own - the one detector and the one writer the hand glow (Hands.cpp) and the
// clash and afterglow lights (ClashLights.cpp) share. The same rules as Illuminated's Lighting.cpp and LightKit.h (ours):
// Light Limit Fix reads the first two words of every light's runtime data as its flags and cutoff whatever shader files lie
// in the folder, so the words are written whenever CommunityShaders.dll is loaded (Bottled Shaders keeps the name) and never
// otherwise - its shader files alone are a leftover, and those words on a Vanilla / ENB light land in its ambient color.

#pragma once

namespace LightWords
{
	// our lights' names: the hand scaling leaves both alone
	inline constexpr const char* kHandGlow = "SpellMasteryHeatLight";  // Hands.cpp's own hand glow
	inline constexpr const char* kClash = "ArcaneClashLight";          // ClashLights.cpp's: the clash designs' and the afterglows'

	inline constexpr std::uint32_t kInverseSquare = 1u << 10, kInitialised = 1u << 8;
	inline constexpr float         kK = 3918.88f;   // cutoff = K * fade / (reach^2 + size^2)
	inline constexpr float         kSize = 1.414f;  // the light's size, carried in its radius' z

	[[nodiscard]] inline bool InverseSquare()
	{
		static const bool cs = REX::W32::GetModuleHandleA("CommunityShaders.dll") != nullptr;
		return cs;
	}

	// after SetLightAttenuation, which writes the same words: the flags written whole (a light of our own - Initialised, as
	// Light Limit Fix marks the lights it makes, and InverseSquare), the cutoff for this fade and reach, and the third word
	// (Community Shaders' light-record id) cleared
	inline void Write(RE::NiPointLight* a_light, float a_fade, float a_reach) noexcept
	{
		const float                        cutoff = std::clamp(kK * a_fade / (a_reach * a_reach + kSize * kSize), 0.01f, 0.99f);
		const std::array<std::uint32_t, 2> words{ kInverseSquare | kInitialised, std::bit_cast<std::uint32_t>(cutoff) };
		std::memcpy(reinterpret_cast<std::byte*>(&a_light->GetLightRuntimeData()), words.data(), sizeof(words));
		a_light->GetLightRuntimeData().ambient.blue = 0.0f;
	}
}

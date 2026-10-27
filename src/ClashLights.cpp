// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The light a clash's design casts: one real light in the spell's color, hung on the design's mesh where the two met, flaring
// and then fading with it, so the ground and the design's smoke, stones and blood are lit as they are thrown (scene-lit
// parts show nothing at night without a light).
// CREDIT: how the light is made and registered follows ReLight by Truman (github.com/TrumanGIT/ReLight), GPL-3.0-or-later,
// with his permission: one master light made once and cloned for every light, registered as a dynamic, shadowless light.
// With Community Shaders' inverse-square lighting the light's two words are written as a light of its own is (Illuminated's
// LightKit, ours). Main thread only.

#include "ClashShared.h"

namespace Crossfire
{
	namespace
	{
		constexpr const char*   kName = "ArcaneClashLight";
		constexpr float         kSize = 1.414f;   // the light's size, carried in its radius' z
		constexpr float         kK = 3918.88f;    // inverse square: cutoff = K * fade / (reach^2 + size^2)
		constexpr float         kReach = 320.0f;  // units, at 100 % and a burst scale of 1
		constexpr float         kFade = 2.2f;     // its peak brightness at 100 %
		constexpr float         kLife = 1.25f;    // seconds: gone well before the design's 2 s
		constexpr std::uint32_t kIslFlag = 1u << 10, kIslInitialised = 1u << 8;

		struct Lit
		{
			RE::NiPointer<RE::NiNode>       root;
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			float                           peak{ 0.0f };
			float                           age{ 0.0f };
			bool                            placed{ false };  // its design has been in the world (has had a parent)
		};
		std::vector<Lit> gLit;

		[[nodiscard]] RE::ShadowSceneNode* Scene() { return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]; }

		[[nodiscard]] bool InverseSquare()
		{
			static const bool cs = REX::W32::GetModuleHandleA("CommunityShaders.dll") != nullptr;
			return cs;
		}

		// ReLight's master-and-clone: a light made fresh and attached at once crashes, a clone of one made earlier does not
		[[nodiscard]] RE::NiPointLight* CloneLight()
		{
			static RE::NiPointer<RE::NiPointLight> master;
			if (!master) {
				const RE::NiPointer<RE::NiPointLight> fresh(RE::NiPointLight::Create());
				auto*                                 clone = fresh ? netimmerse_cast<RE::NiPointLight*>(fresh->Clone()) : nullptr;
				if (!clone) {
					return nullptr;
				}
				master.reset(clone);
			}
			return netimmerse_cast<RE::NiPointLight*>(master->Clone());
		}

		[[nodiscard]] RE::BSLight* AddToScene(RE::ShadowSceneNode* a_scene, RE::NiPointLight* a_light)
		{
			RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
			params.dynamic = true;
			params.shadowLight = false;
			params.portalStrict = true;
			params.affectLand = true;
			params.affectWater = true;
			params.neverFades = true;
			params.fov = 90.0f;
			params.falloff = 1.0f;
			params.nearDistance = 5.0f;
			params.depthBias = 1.0f;
			return a_scene && a_light ? a_scene->AddLight(a_light, params) : nullptr;
		}

		// Community Shaders' two words at the head of the light's data: its flags (a light of our own, inverse square) and cutoff
		void SetInverseSquare(RE::NiPointLight* a_light, float a_fade, float a_reach)
		{
			const std::array<std::uint32_t, 2> words{ kIslFlag | kIslInitialised,
				std::bit_cast<std::uint32_t>(std::clamp(kK * a_fade / (a_reach * a_reach + kSize * kSize), 0.01f, 0.99f)) };
			std::memcpy(reinterpret_cast<std::byte*>(&a_light->GetLightRuntimeData()), words.data(), sizeof(words));
			a_light->GetLightRuntimeData().ambient.blue = 0.0f;
		}

		// its brightness over its life, 0..1: up in 0.04 s, held to 0.12 s, then easing out to nothing at kLife
		[[nodiscard]] float Curve(float a_age) noexcept
		{
			if (a_age < 0.04f) {
				return a_age / 0.04f;
			}
			if (a_age < 0.12f) {
				return 1.0f;
			}
			const float u = std::clamp((a_age - 0.12f) / (kLife - 0.12f), 0.0f, 1.0f);
			return std::pow(1.0f - u, 1.6f);
		}

		void LetGo(Lit& a_lit, RE::ShadowSceneNode* a_scene)
		{
			if (a_scene && a_lit.bs) {
				a_scene->RemoveLight(a_lit.bs);
			}
			if (a_lit.light && a_lit.light->parent) {
				a_lit.light->parent->DetachChild(a_lit.light.get());
			}
		}
	}

	void LightDesign(RE::NiAVObject* a_root, Core::Rgb a_color, float a_scale, float a_percent)
	{
		auto* root = a_root ? a_root->AsNode() : nullptr;
		auto* scene = Scene();
		if (!root || !scene || !(a_percent > 0.0f) || gLit.size() >= 24) {
			return;
		}
		auto* light = CloneLight();
		if (!light) {
			return;
		}
		const float       top = static_cast<float>((std::max)({ a_color.r, a_color.g, a_color.b }));
		const RE::NiColor color = top > 0.0f ? RE::NiColor{ a_color.r / top, a_color.g / top, a_color.b / top } : RE::NiColor{ 1.0f, 1.0f, 1.0f };
		const float       scale = std::isfinite(a_scale) && a_scale > 0.0f ? a_scale : 1.0f;
		const float       reach = kReach * scale;
		const float       peak = kFade * a_percent / 100.0f;
		light->name = kName;
		auto& data = light->GetLightRuntimeData();
		data.diffuse = color;
		data.fade = 0.0f;
		data.radius = { reach, reach, kSize };
		light->SetLightAttenuation(reach);
		if (InverseSquare()) {
			SetInverseSquare(light, peak, reach);
		} else {
			data.ambient = { color.red * 0.1f, color.green * 0.1f, color.blue * 0.1f };  // RE::Light's rule: a tenth of the diffuse
		}
		// the design's mesh may be scaled and the light hangs inside it: its own scale undoes that
		light->local.translate = {};
		light->local.scale = 1.0f / scale;
		root->AttachChild(light, true);
		RE::NiUpdateData update{};
		light->Update(update);
		auto* bs = AddToScene(scene, light);
		if (!bs) {
			root->DetachChild(light);
			return;
		}
		gLit.push_back({ RE::NiPointer<RE::NiNode>(root), RE::NiPointer<RE::NiPointLight>(light), RE::NiPointer<RE::BSLight>(bs), peak, 0.0f, root->parent != nullptr });
	}

	void UpdateLights(float a_delta)
	{
		if (gLit.empty()) {
			return;
		}
		auto*       scene = Scene();
		const float dt = std::isfinite(a_delta) ? std::clamp(a_delta, 0.0f, 0.25f) : 0.0f;
		std::erase_if(gLit, [&](Lit& a_lit) {
			a_lit.age += dt;
			const bool inWorld = a_lit.root && a_lit.root->parent;
			a_lit.placed = a_lit.placed || inWorld;
			// done, or its design was taken out of the world before its time (a load, a cell change)
			if (a_lit.age >= kLife || !a_lit.root || !a_lit.light || (a_lit.placed && !inWorld)) {
				LetGo(a_lit, scene);
				return true;
			}
			a_lit.light->GetLightRuntimeData().fade = a_lit.peak * Curve(a_lit.age);
			return false;
		});
	}

	void ClearLights()
	{
		auto* scene = Scene();
		for (auto& lit : gLit) {
			LetGo(lit, scene);
		}
		gLit.clear();
	}

	std::size_t LightsLit() { return gLit.size(); }
}

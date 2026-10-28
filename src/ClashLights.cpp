// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The light a clash's design casts: one real light in the spell's color, hung on the design's mesh where the two met, flaring
// and then fading with it, so the ground and the design's smoke, stones and blood are lit as they are thrown (scene-lit
// parts show nothing at night without a light).
// CREDIT: how the light is made and registered follows ReLight by Truman (github.com/TrumanGIT/ReLight), GPL-3.0-or-later,
// with his permission: one master light made once and cloned for every light, registered as a dynamic, shadowless light.
// With Community Shaders' inverse-square lighting the light's two words are written as a light of its own is (LightWords.h).
// Main thread only.

#include "ClashShared.h"
#include "LightWords.h"

namespace Crossfire
{
	namespace
	{
		constexpr float       kReach = 320.0f;   // units, at 100 % and a burst scale of 1
		constexpr float       kFade = 2.2f;      // its peak brightness at 100 %
		constexpr float       kLife = 1.25f;     // seconds: gone well before the design's 2 s
		constexpr std::size_t kMostLights = 24;  // all of ours at once
		constexpr std::size_t kMostGlows = 16;   // of them afterglows: the rest kept for clashes

		struct Lit
		{
			RE::NiPointer<RE::NiNode>       root;
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      bs;
			float                           peak{ 0.0f };
			float                           age{ 0.0f };
			bool                            placed{ false };  // its design has been in the world (has had a parent)
			float                           life{ kLife };
			bool                            glow{ false };   // an afterglow: it starts at its peak and only fades
			const RE::NiAVObject*           top{ nullptr };  // an afterglow: the top of its node's tree when hung, only compared
		};
		std::vector<Lit>         gLit;
		std::atomic<std::size_t> gLitCount{ 0 };  // gLit's size, for DevBench's thread

		[[nodiscard]] RE::ShadowSceneNode* Scene() { return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]; }

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

		[[nodiscard]] float Glow(float a_age, float a_life) noexcept { return std::pow(1.0f - std::clamp(a_age / a_life, 0.0f, 1.0f), 1.6f); }

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

	namespace
	{
		// one light hung on a node: a_reach units, a_peak at its brightest, the node's own scale a_nodeScale undone. An afterglow on a
		// node that already has one starts that one again instead: one per hand and per target
		void Hang(RE::NiNode* a_root, Core::Rgb a_color, float a_reach, float a_peak, float a_nodeScale, float a_life, bool a_glow)
		{
			auto* scene = Scene();
			if (!a_root || !scene || !(a_peak > 0.0f) || !(a_life > 0.0f)) {
				return;
			}
			const float       top = static_cast<float>((std::max)({ a_color.r, a_color.g, a_color.b }));
			const RE::NiColor color = top > 0.0f ? RE::NiColor{ a_color.r / top, a_color.g / top, a_color.b / top } : RE::NiColor{ 1.0f, 1.0f, 1.0f };
			if (a_glow) {
				const auto same = std::ranges::find_if(gLit, [a_root](const Lit& a_lit) { return a_lit.glow && a_lit.root.get() == a_root; });
				if (same != gLit.end() && same->light) {
					same->peak = std::max(a_peak, same->light->GetLightRuntimeData().fade);
					same->age = 0.0f;
					same->life = a_life;
					auto&       was = same->light->GetLightRuntimeData();
					const float reach = std::max(a_reach, was.radius.x);  // the hotter of the two casts
					was.diffuse = color;
					was.radius = { reach, reach, LightWords::kSize };
					same->light->SetLightAttenuation(reach);
					if (LightWords::InverseSquare()) {
						LightWords::Write(same->light.get(), same->peak, was.radius.x);
					} else {
						was.ambient = { color.red * 0.1f, color.green * 0.1f, color.blue * 0.1f };
					}
					return;
				}
				if (std::ranges::count_if(gLit, [](const Lit& a_lit) { return a_lit.glow; }) >= static_cast<std::ptrdiff_t>(kMostGlows)) {
					return;
				}
			}
			if (gLit.size() >= kMostLights) {
				return;
			}
			auto* light = CloneLight();
			if (!light) {
				return;
			}
			light->name = LightWords::kClash;
			auto& data = light->GetLightRuntimeData();
			data.diffuse = color;
			data.fade = 0.0f;
			data.radius = { a_reach, a_reach, LightWords::kSize };
			light->SetLightAttenuation(a_reach);
			if (LightWords::InverseSquare()) {
				LightWords::Write(light, a_peak, a_reach);
			} else {
				data.ambient = { color.red * 0.1f, color.green * 0.1f, color.blue * 0.1f };  // RE::Light's rule: a tenth of the diffuse
			}
			// the node (a design's mesh, a hand, a body) may be scaled and the light hangs inside it: its own scale undoes that
			light->local.translate = {};
			light->local.scale = 1.0f / a_nodeScale;
			a_root->AttachChild(light, true);
			RE::NiUpdateData update{};
			light->Update(update);
			auto* bs = AddToScene(scene, light);
			if (!bs) {
				a_root->DetachChild(light);
				return;
			}
			if (a_glow) {
				light->GetLightRuntimeData().fade = a_peak;
			}
			gLit.push_back({ RE::NiPointer<RE::NiNode>(a_root), RE::NiPointer<RE::NiPointLight>(light), RE::NiPointer<RE::BSLight>(bs), a_peak, 0.0f,
				a_root->parent != nullptr, a_life, a_glow, a_glow ? TopOf(a_root) : nullptr });
			gLitCount = gLit.size();
		}
	}

	void LightDesign(RE::NiAVObject* a_root, Core::Rgb a_color, float a_scale, float a_percent)
	{
		const float scale = std::isfinite(a_scale) && a_scale > 0.0f ? a_scale : 1.0f;
		Hang(a_root ? a_root->AsNode() : nullptr, a_color, kReach * scale, kFade * a_percent / 100.0f, scale, kLife, false);
	}

	void LightAfterglow(RE::NiNode* a_node, Core::Rgb a_color, float a_reach, float a_fade, float a_seconds)
	{
		const float nodeScale = a_node && std::isfinite(a_node->world.scale) && a_node->world.scale > 0.0f ? a_node->world.scale : 1.0f;
		Hang(a_node, a_color, a_reach, a_fade, nodeScale, std::clamp(a_seconds, 0.1f, 10.0f), true);
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
			// an afterglow's node keeps its parent when its actor unloads: the top of its tree tells
			const bool inWorld = a_lit.root && a_lit.root->parent && (!a_lit.top || TopOf(a_lit.root.get()) == a_lit.top);
			a_lit.placed = a_lit.placed || inWorld;
			// done, or its design or actor was taken out of the world before its time (a load, a cell change)
			if (a_lit.age >= a_lit.life || !a_lit.root || !a_lit.light || (a_lit.placed && !inWorld)) {
				LetGo(a_lit, scene);
				return true;
			}
			auto&       data = a_lit.light->GetLightRuntimeData();
			const float fade = a_lit.peak * (a_lit.glow ? Glow(a_lit.age, a_lit.life) : Curve(a_lit.age));
			data.fade = fade;
			if (LightWords::InverseSquare()) {
				LightWords::Write(a_lit.light.get(), fade, data.radius.x);  // its cutoff follows its fade, as the hand glow's does
			}
			return false;
		});
		gLitCount = gLit.size();
	}

	void ClearLights()
	{
		auto* scene = Scene();
		for (auto& lit : gLit) {
			LetGo(lit, scene);
		}
		gLit.clear();
		gLitCount = 0;
	}

	std::size_t LightsLit() { return gLitCount.load(); }
}

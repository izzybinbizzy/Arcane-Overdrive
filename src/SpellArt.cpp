// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The spell's own color, for the bars (his order: "the effects need to mirror or at the very least be influenced by their in
// game spells and not be some random interpretation"). For the spell the bars show, the color the game itself draws it with,
// read from the player's own load order - a VFX replacer's art included: the effect's casting light, else its projectile's
// light, else the base color of the first effect-shader block in its models (the projectile, the casting art, the hit art)
// that is not colored through a palette. The models come through the game's model cache (BSModelDB::Demand - the spell in
// hand has them loaded already). Done once per spell, on the main thread (Meters.cpp's Frame), and handed to the render
// thread under a lock (SpellArtNow).

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		struct Art
		{
			RE::NiColor color{ 1.0f, 1.0f, 1.0f };
			bool        colored{ false };
		};

		std::unordered_map<RE::FormID, Art> gCache;  // main thread
		const RE::MagicItem*                gNow{ nullptr };

		std::mutex                 gViewLock;
		SpellArtView               gView;
		std::atomic<std::uint32_t> gVersion{ 0 };  // counts every new view, so the HUD copies one only when it changed

		RE::NiColor FromColor(const RE::Color& a_c)
		{
			return { static_cast<float>(a_c.red) / 255.0f, static_cast<float>(a_c.green) / 255.0f, static_cast<float>(a_c.blue) / 255.0f };
		}

		// the base color of a model's first effect block that shows its own color (a palette-colored block shows its color
		// only through the palette)
		void ReadModel(const char* a_model, Art& a_art)
		{
			if (a_art.colored || !a_model || !*a_model) {
				return;
			}
			RE::NiPointer<RE::NiNode>         root;
			RE::BSModelDB::DBTraits::ArgsType args{};
			if (RE::BSModelDB::Demand(a_model, root, args) != RE::BSResource::ErrorCode::kNone || !root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* a_geometry) {
				auto* raw = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto* prop = raw ? netimmerse_cast<RE::BSEffectShaderProperty*>(raw) : nullptr;
				auto* mat = prop ? static_cast<RE::BSEffectShaderMaterial*>(prop->GetMaterial()) : nullptr;
				if (!mat) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				const bool  palette = mat->greyscaleTexturePath.c_str() && *mat->greyscaleTexturePath.c_str() &&
				                      prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor);
				const float s = std::clamp(mat->baseColorScale, 0.0f, 4.0f);
				const auto  c = RE::NiColor{ mat->baseColor.red * s, mat->baseColor.green * s, mat->baseColor.blue * s };
				if (palette || c.red + c.green + c.blue <= 0.15f) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				a_art.color = { std::min(c.red, 1.0f), std::min(c.green, 1.0f), std::min(c.blue, 1.0f) };
				a_art.colored = true;
				return RE::BSVisit::BSVisitControl::kStop;
			});
		}

		Art Gather(const RE::MagicItem* a_item)
		{
			Art         art;
			const auto* main = a_item ? a_item->GetCostliestEffectItem() : nullptr;
			const auto* effect = main ? main->baseEffect : nullptr;
			if (!effect) {
				return art;
			}
			const auto& d = effect->data;
			// what the spell lights the world with is what the player sees it as
			if (d.light) {
				art.color = FromColor(d.light->data.color);
				art.colored = true;
			} else if (d.projectileBase && d.projectileBase->data.light) {
				art.color = FromColor(d.projectileBase->data.light->data.color);
				art.colored = true;
			}
			if (d.projectileBase) {
				ReadModel(d.projectileBase->GetModel(), art);
			}
			if (d.castingArt) {
				ReadModel(d.castingArt->GetModel(), art);
			}
			if (d.hitEffectArt) {
				ReadModel(d.hitEffectArt->GetModel(), art);
			}
			return art;
		}

		void Publish(const RE::MagicItem* a_item, const Art* a_art)
		{
			SpellArtView v;
			if (a_item && a_art) {
				v.form = a_item->GetFormID();
				v.colored = a_art->colored;
				v.r = a_art->color.red;
				v.g = a_art->color.green;
				v.b = a_art->color.blue;
			}
			std::lock_guard lock(gViewLock);
			gView = v;
			++gVersion;
		}
	}

	void NoteSpellArt(const RE::MagicItem* a_item)
	{
		if (a_item == gNow) {
			return;
		}
		gNow = a_item;
		const Art* art = nullptr;
		if (a_item) {
			const auto [it, fresh] = gCache.try_emplace(a_item->GetFormID());
			if (fresh) {
				it->second = Gather(a_item);
				SKSE::log::info("bar color for {}: {:.2f} {:.2f} {:.2f}{}", a_item->GetName(), it->second.color.red, it->second.color.green, it->second.color.blue,
					it->second.colored ? "" : " (the look's own)");
			}
			art = &it->second;
		}
		Publish(a_item, art);
	}

	// a game is loading: nothing shown until the next spell is noted
	void ForgetSpellArt()
	{
		gNow = nullptr;
		Publish(nullptr, nullptr);
	}

	std::uint32_t SpellArtVersion() { return gVersion.load(std::memory_order_acquire); }

	SpellArtView SpellArtNow()
	{
		std::lock_guard lock(gViewLock);
		return gView;
	}
}

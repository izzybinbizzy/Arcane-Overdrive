// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The spell's own art, for the HUD bars (his order 2026-10-15: "the effects need to mirror or at the very least be influenced
// by their in game spells and not be some random interpretation"). For the spell the bar shows, the textures and the color
// the game itself draws it with are read from the player's own load order - a VFX replacer's art included:
//   - the models of its costliest effect: the projectile (what flies - Frostbite's spray, Flames' stream), the casting art
//     (what the hand holds), the hit art; in each, the effect-shader blocks: a particle system's texture becomes the bar's
//     "body" (the particles that wear the spell's own look), another effect texture its fill;
//   - when the models give none: the effect shader's particle and fill textures;
//   - the color: the effect's casting light, else its projectile's light, else the art's own base color.
// The models come through the game's model cache (BSModelDB::Demand - the spell in hand has them loaded already) and the
// textures through the game's texture cache (BSShaderManager::GetTexture), so nothing is read twice and they live as long as
// we hold them. Done once per spell, on the main thread (Meters.cpp's Frame), and handed to the render thread as plain
// texture views under a lock (SpellArtNow). Dynamic Wards' Colors.cpp reads its ward art the same way.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		struct Art
		{
			RE::NiPointer<RE::NiSourceTexture> body, fill;
			std::string                        bodyPath, fillPath;
			std::vector<std::array<float, 4>>  frames;  // the body texture's flipbook cells, read off its particle system
			RE::NiColor                        color{ 1.0f, 1.0f, 1.0f };
			bool                               colored{ false };
			int                                tries{ 0 };  // a texture still loading is looked at again for a while
		};

		std::unordered_map<RE::FormID, Art> gCache;  // main thread
		const RE::MagicItem*                gNow{ nullptr };

		std::mutex                 gViewLock;
		SpellArtView               gView;
		std::atomic<std::uint32_t> gVersion{ 0 };  // counts every new view, so the HUD copies one only when it changed

		std::string Lower(std::string a_s)
		{
			std::ranges::transform(a_s, a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		// a texture path as the game's texture cache wants it: "textures\..."
		std::string TexturePath(std::string_view a_path)
		{
			std::string p(a_path);
			std::ranges::replace(p, '/', '\\');
			if (!Lower(p).starts_with("textures\\")) {
				p = "textures\\" + p;
			}
			return p;
		}

		RE::NiColor FromColor(const RE::Color& a_c)
		{
			return { static_cast<float>(a_c.red) / 255.0f, static_cast<float>(a_c.green) / 255.0f, static_cast<float>(a_c.blue) / 255.0f };
		}

		void ReadFrames(RE::NiParticleSystem& a_system, std::vector<std::array<float, 4>>& a_out);

		// the effect-shader textures of one model: a particle system's first, then any other effect block's
		void ReadModel(const char* a_model, Art& a_art)
		{
			if (!a_model || !*a_model || (!a_art.bodyPath.empty() && !a_art.fillPath.empty())) {
				return;
			}
			RE::NiPointer<RE::NiNode>         root;
			RE::BSModelDB::DBTraits::ArgsType args{};
			if (RE::BSModelDB::Demand(a_model, root, args) != RE::BSResource::ErrorCode::kNone || !root) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* a_geometry) {
				auto*       raw = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto*       prop = raw ? netimmerse_cast<RE::BSEffectShaderProperty*>(raw) : nullptr;
				auto*       mat = prop ? static_cast<RE::BSEffectShaderMaterial*>(prop->GetMaterial()) : nullptr;
				const char* src = mat ? mat->sourceTexturePath.c_str() : nullptr;
				if (!src || !*src) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				auto*      system = netimmerse_cast<RE::NiParticleSystem*>(a_geometry);
				const bool particles = system != nullptr;
				auto&      slot = particles ? a_art.bodyPath : a_art.fillPath;
				if (slot.empty()) {
					slot = src;
					if (particles) {
						ReadFrames(*system, a_art.frames);
					}
					// a block colored by a palette shows its color only through the palette: the light's color stands for it
					const bool palette = mat->greyscaleTexturePath.c_str() && *mat->greyscaleTexturePath.c_str() &&
					                     prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor);
					if (!a_art.colored && !palette) {
						const float s = std::clamp(mat->baseColorScale, 0.0f, 4.0f);
						const auto  c = RE::NiColor{ mat->baseColor.red * s, mat->baseColor.green * s, mat->baseColor.blue * s };
						if (c.red + c.green + c.blue > 0.15f) {
							a_art.color = { std::min(c.red, 1.0f), std::min(c.green, 1.0f), std::min(c.blue, 1.0f) };
							a_art.colored = true;
						}
					}
				}
				return a_art.bodyPath.empty() || a_art.fillPath.empty() ? RE::BSVisit::BSVisitControl::kContinue : RE::BSVisit::BSVisitControl::kStop;
			});
		}

		// a particle texture that is a flipbook (Skyrim's fire and frost often are): the cells its particle data names, each as
		// u0 v0 u1 v1 (a cell written as a corner and a size is turned into two corners)
		void ReadFrames(RE::NiParticleSystem& a_system, std::vector<std::array<float, 4>>& a_out)
		{
			a_out.clear();
			const auto& data = a_system.GetParticlesRuntimeData().particleData;
			if (!data) {
				return;
			}
			const auto& d = data->GetParticlesRuntimeData();
			const auto  n = d.subtextureOffsets ? std::min<std::uint32_t>(d.subTextureOffsetsCount, 64) : 0;
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto& c = d.subtextureOffsets[i];
				float       u0 = c.red, v0 = c.green, u1 = c.blue, v1 = c.alpha;
				if (u1 <= u0 || v1 <= v0) {
					u1 = u0 + c.blue;
					v1 = v0 + c.alpha;
				}
				if (std::isfinite(u0) && std::isfinite(v0) && std::isfinite(u1) && std::isfinite(v1) && u1 > u0 && v1 > v0 && u0 >= -0.01f && v0 >= -0.01f &&
					u1 <= 1.01f && v1 <= 1.01f) {
					a_out.push_back({ u0, v0, u1, v1 });
				}
			}
			if (a_out.size() < 2) {
				a_out.clear();  // one cell is the whole texture
			}
		}

		RE::NiPointer<RE::NiSourceTexture> Load(const std::string& a_path)
		{
			if (a_path.empty()) {
				return {};
			}
			RE::NiPointer<RE::NiTexture> t;
			RE::BSShaderManager::GetTexture(TexturePath(a_path).c_str(), true, t, false);
			return RE::NiPointer<RE::NiSourceTexture>(t ? netimmerse_cast<RE::NiSourceTexture*>(t.get()) : nullptr);
		}

		void* ViewOf(const RE::NiPointer<RE::NiSourceTexture>& a_t)
		{
			return a_t && a_t->rendererTexture ? static_cast<void*>(a_t->rendererTexture->resourceView) : nullptr;
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
			// the color first from the lights: what the spell lights the world with is what the player sees it as
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
			if (const auto* shader = d.effectShader) {
				if (art.bodyPath.empty() && shader->particleShaderTexture.textureName.c_str() && *shader->particleShaderTexture.textureName.c_str()) {
					art.bodyPath = shader->particleShaderTexture.textureName.c_str();
				}
				if (art.fillPath.empty() && shader->fillTexture.textureName.c_str() && *shader->fillTexture.textureName.c_str()) {
					art.fillPath = shader->fillTexture.textureName.c_str();
				}
			}
			art.body = Load(art.bodyPath);
			art.fill = Load(art.fillPath);
			if (art.bodyPath.empty()) {
				art.frames.clear();
			}
			return art;
		}

		void Publish(const RE::MagicItem* a_item, const Art* a_art)
		{
			SpellArtView v;
			if (a_item && a_art) {
				v.form = a_item->GetFormID();
				v.body = ViewOf(a_art->body);
				v.fill = ViewOf(a_art->fill);
				v.colored = a_art->colored;
				v.r = a_art->color.red;
				v.g = a_art->color.green;
				v.b = a_art->color.blue;
				v.bodyPath = a_art->bodyPath;
				v.fillPath = a_art->fillPath;
				v.frames = a_art->frames;
			}
			std::lock_guard lock(gViewLock);
			gView = std::move(v);
			++gVersion;
		}
	}

	void NoteSpellArt(const RE::MagicItem* a_item)
	{
		auto* art = a_item ? &gCache[a_item->GetFormID()] : nullptr;
		if (art && art->tries == 0) {
			*art = Gather(a_item);
			art->tries = 1;
			SKSE::log::info("spell bar art for {}: body \"{}\"{}, fill \"{}\"{}, color {:.2f} {:.2f} {:.2f}{}", a_item->GetName(), art->bodyPath,
				ViewOf(art->body) ? "" : " (not loaded)", art->fillPath, ViewOf(art->fill) ? "" : " (not loaded)", art->color.red, art->color.green,
				art->color.blue, art->colored ? "" : " (the look's own)");
		} else if (art && art->tries < 120 && ((!art->bodyPath.empty() && !ViewOf(art->body)) || (!art->fillPath.empty() && !ViewOf(art->fill)))) {
			// a texture the cache was still loading: looked at again each frame for about two seconds
			++art->tries;
			if (!ViewOf(art->body)) {
				art->body = Load(art->bodyPath);
			}
			if (!ViewOf(art->fill)) {
				art->fill = Load(art->fillPath);
			}
			if (a_item == gNow) {
				Publish(a_item, art);
			}
		}
		if (a_item != gNow) {
			gNow = a_item;
			Publish(a_item, art);
		}
	}

	// a game is loading: nothing shown until the next spell is noted; the textures read so far stay held (a texture the HUD may
	// still be drawing this frame is never let go)
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

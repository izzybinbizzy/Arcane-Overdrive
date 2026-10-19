// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The live spell bars on the HUD (Bar style "Spell bars", his order 2026-10-15 - BarFx.h has his words and the why of the
// motion). Every look is the same bar:
//   - the slot: a dark rounded bed with a thin rim in the spell's color;
//   - the fill: the spell's OWN effect texture (SpellArt.cpp) tiled along the filled part in two layers flowing at different
//     speeds against each other, fading out over a soft head; a built-in flowing noise strip when the spell has none;
//   - round it, the particles of BarFx::Bar - born on the whole filled outline and carried by the flow field (the 360
//     part): built-in sprites tinted in the spell's own color (a game particle sheet drew as boxy cards on a HUD quad -
//     the note at the particle loop); shock adds arcs leaping round the frame;
//   - a flare at the head; the stage's name over the right end.
// Glowing layers are drawn ADDITIVELY, as the game draws its magic effects: an ImGui draw callback sets an additive blend
// state (and a sampler that reads every mip, so a big game texture drawn small does not shimmer) for those draw commands,
// and ImGui's own reset callback puts the framework's state back after them. Overdrive's power (Overload::Power) doubles
// the particles' number, size and speed and the fill's flow; the cooldown cools everything to grey-blue and sputters.
// Cost: one pass over at most BarFx::kMax particles, one textured quad each, through the framework's draw calls looked up
// once; measured every frame (the Debug page, DevBench `look.drawUsAvg`).
// Everything here runs on the render thread (the HUD element), except LookStatsNow and BenchForceLook.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include "Plugin.h"

// SKSE Menu Framework's own header (theirs, MIT): its warnings are not ours, and ours are errors (xmake.lua) - under
// MSVC and under clang-cl, which checks every file too
#pragma warning(push, 0)
#if defined(__clang__)
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Weverything"
#endif
#include "SKSEMenuFramework.h"
#if defined(__clang__)
#	pragma clang diagnostic pop
#endif
#pragma warning(pop)
#include "BarFx.h"
#include "HudBars.h"
#include "Overload.h"

namespace Plugin
{
	namespace
	{
		using Steady = std::chrono::steady_clock;
		using ImGuiMCP::ImDrawCallback;
		using ImGuiMCP::ImDrawCmd;
		using ImGuiMCP::ImDrawList;
		using ImGuiMCP::ImTextureID;
		using ImGuiMCP::ImU32;
		using ImGuiMCP::ImVec2;

		constexpr std::string_view kFolder = "Data/SKSE/Plugins/SpellMasteryHeat/";
		constexpr std::string_view kSheetFile = "BarSprites.dds";  // BarFx::Sprite's cells, 64 texels each, in one row
		constexpr std::string_view kFillFile = "BarFill.dds";      // a flowing noise strip that tiles left to right
		constexpr float            kHalfTexel = 0.5f / 512.0f;

		// the framework's draw calls, looked up ONCE (its header looks each one up on every call)
		using AddQuadFn = void (*)(ImDrawList*, ImTextureID, ImVec2, ImVec2, ImVec2, ImVec2, ImVec2, ImVec2, ImVec2, ImVec2, ImU32);
		using AddImageFn = void (*)(ImDrawList*, ImTextureID, ImVec2, ImVec2, ImVec2, ImVec2, ImU32);
		using AddCallbackFn = void (*)(ImDrawList*, ImDrawCallback, void*);
		using AddPolylineFn = void (*)(ImDrawList*, const ImVec2*, int, ImU32, ImGuiMCP::ImDrawFlags, float);
		struct Calls
		{
			AddQuadFn     quad{ nullptr };
			AddImageFn    image{ nullptr };
			AddCallbackFn callback{ nullptr };
			AddPolylineFn polyline{ nullptr };
		};
		const Calls& Fn()
		{
			static const Calls calls{ ::GetMenuFrameworkFunction<AddQuadFn>("ImDrawList_AddImageQuad"),
				::GetMenuFrameworkFunction<AddImageFn>("ImDrawList_AddImage"), ::GetMenuFrameworkFunction<AddCallbackFn>("ImDrawList_AddCallback"),
				::GetMenuFrameworkFunction<AddPolylineFn>("ImDrawList_AddPolyline") };
			return calls;
		}

		// ---- the additive state, made once on the device the HUD is drawn with
		REX::W32::ID3D11BlendState*   gAdd{ nullptr };
		REX::W32::ID3D11SamplerState* gSampler{ nullptr };
		bool                          gStatesTried{ false };

		// The head of ImGui's own DX11 backend data (ImGui_ImplDX11_Data in ImGui 1.90.8, the version SKSE Menu Framework 3 is
		// built on): the device and the context every HUD draw is issued on. The blend state is made on and bound to THESE - the
		// 2026-10-15 quick look on a frame-generation copy drew every "added" layer blended while the renderer's context took
		// the state, so the renderer's pair is only the fallback.
		struct BackendHead
		{
			REX::W32::ID3D11Device*        device;
			REX::W32::ID3D11DeviceContext* context;
		};
		const BackendHead* Backend()
		{
			auto*       io = ImGuiMCP::GetIO();
			const auto* head = io ? static_cast<const BackendHead*>(io->BackendRendererUserData) : nullptr;
			return head && head->device && head->context ? head : nullptr;
		}
		REX::W32::ID3D11DeviceContext* RendererContext()
		{
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			return renderer ? renderer->GetRuntimeData().context : nullptr;
		}

		void MakeStates()
		{
			if (gStatesTried) {
				return;
			}
			gStatesTried = true;
			const auto* imgui = Backend();
			auto*       rendererDevice = RE::BSGraphics::Renderer::GetDevice();
			auto*       rendererContext = RendererContext();
			const char* verdict = "no HUD backend data - the renderer's are used";
			if (imgui) {
				verdict = imgui->device == rendererDevice && imgui->context == rendererContext ? "the same" : "DIFFERENT - the HUD's are used";
			}
			SKSE::log::info("spell bars: HUD device {} context {}, renderer device {} context {} ({})", imgui ? static_cast<const void*>(imgui->device) : nullptr,
				imgui ? static_cast<const void*>(imgui->context) : nullptr, static_cast<const void*>(rendererDevice), static_cast<const void*>(rendererContext), verdict);
			auto* device = imgui ? imgui->device : rendererDevice;
			if (!device) {
				SKSE::log::warn("spell bars: no graphics device - the glow is drawn blended instead of added");
				return;
			}
			REX::W32::D3D11_BLEND_DESC bd{};
			auto&                      rt = bd.renderTarget[0];
			rt.blendEnable = 1;
			rt.srcBlend = REX::W32::D3D11_BLEND_SRC_ALPHA;
			rt.destBlend = REX::W32::D3D11_BLEND_ONE;
			rt.blendOp = REX::W32::D3D11_BLEND_OP_ADD;
			rt.srcBlendAlpha = REX::W32::D3D11_BLEND_ZERO;
			rt.destBlendAlpha = REX::W32::D3D11_BLEND_ONE;
			rt.blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD;
			rt.renderTargetWriteMask = static_cast<std::uint8_t>(REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL);
			if (device->CreateBlendState(&bd, &gAdd) < 0) {
				gAdd = nullptr;
			}
			REX::W32::D3D11_SAMPLER_DESC sd{};
			sd.filter = REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.addressU = REX::W32::D3D11_TEXTURE_ADDRESS_WRAP;
			sd.addressV = REX::W32::D3D11_TEXTURE_ADDRESS_WRAP;
			sd.addressW = REX::W32::D3D11_TEXTURE_ADDRESS_WRAP;
			sd.comparisonFunc = REX::W32::D3D11_COMPARISON_NEVER;
			sd.minLOD = 0.0f;
			sd.maxLOD = 3.402823466e+38f;
			if (device->CreateSamplerState(&sd, &gSampler) < 0) {
				gSampler = nullptr;
			}
			SKSE::log::info("spell bars: additive glow {}, mip sampler {}", gAdd ? "made" : "NOT made", gSampler ? "made" : "NOT made");
		}

		// ---- the blend measurement (2026-10-16 night): the "added" layers keep coming out blended on his Bottled Shaders copy
		// (pixels round the bars lose a channel - added light never can). These count whether the callbacks run at all, whether
		// our blend state is still bound when the glow is done, and which render target the HUD is drawn into (its size,
		// format, and whether it is the swap chain's back buffer or a separate UI buffer - frame generation's). DevBench
		// `look.blend`.
		std::atomic<std::uint32_t> gAddCalls{ 0 }, gProbeCalls{ 0 }, gKeptAdd{ 0 };
		std::atomic<std::uint32_t> gRtWidth{ 0 }, gRtHeight{ 0 }, gRtFormat{ 0 };
		std::atomic<int>           gRtIsBack{ -1 };  // 1 the back buffer, 0 another target, -1 not read, -2 no target bound

		void ReadTarget(REX::W32::ID3D11DeviceContext* a_ctx)
		{
			REX::W32::ID3D11RenderTargetView* rtv{ nullptr };
			a_ctx->OMGetRenderTargets(1, &rtv, nullptr);
			if (!rtv) {
				gRtIsBack = -2;
				return;
			}
			REX::W32::ID3D11Resource* res{ nullptr };
			rtv->GetResource(&res);
			if (res) {
				REX::W32::D3D11_RESOURCE_DIMENSION dim{};
				res->GetType(&dim);
				if (dim == REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					REX::W32::D3D11_TEXTURE2D_DESC desc{};
					static_cast<REX::W32::ID3D11Texture2D*>(res)->GetDesc(&desc);
					gRtWidth = desc.width;
					gRtHeight = desc.height;
					gRtFormat = static_cast<std::uint32_t>(desc.format);
				}
				int   isBack = 0;
				auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto* view = renderer ? renderer->GetRuntimeData().renderWindows[0].renderView : nullptr;
				if (view) {
					REX::W32::ID3D11Resource* back{ nullptr };
					view->GetResource(&back);
					isBack = back == res ? 1 : 0;
					if (back) {
						back->Release();
					}
				}
				gRtIsBack = isBack;
				res->Release();
			}
			rtv->Release();
		}

		void Additive(const ImDrawList*, const ImDrawCmd*)
		{
			const auto* imgui = Backend();
			auto*       ctx = imgui ? imgui->context : RendererContext();
			gAddCalls.fetch_add(1, std::memory_order_relaxed);
			if (!ctx) {
				return;
			}
			if (gAdd) {
				constexpr float kFactor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
				ctx->OMSetBlendState(gAdd, kFactor, 0xFFFFFFFFu);
			}
			if (gSampler) {
				ctx->PSSetSamplers(0, 1, &gSampler);
			}
			ReadTarget(ctx);
		}

		// placed after the last added layer, before ImGui's reset: is our blend state still the bound one?
		void Probe(const ImDrawList*, const ImDrawCmd*)
		{
			gProbeCalls.fetch_add(1, std::memory_order_relaxed);
			const auto* imgui = Backend();
			auto*       ctx = imgui ? imgui->context : RendererContext();
			if (!ctx) {
				return;
			}
			REX::W32::ID3D11BlendState* bound{ nullptr };
			float                       factor[4]{};
			std::uint32_t               mask{ 0 };
			ctx->OMGetBlendState(&bound, factor, &mask);
			if (bound && bound == gAdd) {
				gKeptAdd.fetch_add(1, std::memory_order_relaxed);
			}
			if (bound) {
				bound->Release();
			}
		}

		// ImGui's own "put the renderer's state back" mark (ImDrawCallback_ResetRenderState, -8 in ImGui 1.90.8, the version
		// SKSE Menu Framework 3 is built on - its DX11 backend checks for it before calling a callback)
		ImDrawCallback ResetState() { return reinterpret_cast<ImDrawCallback>(static_cast<std::intptr_t>(-8)); }

		// ---- the built-in textures, loaded once
		struct Builtin
		{
			ImTextureID sheet{ nullptr }, fill{ nullptr };
			bool        tried{ false };
		};
		Builtin& Textures()
		{
			static Builtin b;
			if (!b.tried) {
				b.tried = true;
				for (const auto [file, slot] : { std::pair{ kSheetFile, &b.sheet }, std::pair{ kFillFile, &b.fill } }) {
					const auto path = std::string(kFolder) + std::string(file);
					const auto t0 = Steady::now();
					*slot = SKSEMenuFramework::LoadTexture(path);
					const float ms = std::chrono::duration<float, std::milli>(Steady::now() - t0).count();
					SKSE::log::info("spell bars: {} {} ({:.1f} ms)", path, *slot ? "loaded" : "could NOT be loaded", ms);
				}
			}
			return b;
		}

		struct Uv
		{
			float u0, v0, u1, v1;
		};
		Uv Cell(BarFx::Sprite a_s)
		{
			const float i = static_cast<float>(static_cast<int>(a_s));
			const float n = static_cast<float>(BarFx::kSpriteCells);
			return { i / n + kHalfTexel, 0.004f, (i + 1.0f) / n - kHalfTexel, 0.996f };
		}

		ImU32 Col(float a_r, float a_g, float a_b, float a_a)
		{
			const auto c = [](float a_v) { return static_cast<ImU32>(std::lround(std::clamp(std::isfinite(a_v) ? a_v : 0.0f, 0.0f, 1.0f) * 255.0f)); };
			return c(a_r) | (c(a_g) << 8) | (c(a_b) << 16) | (c(a_a) << 24);
		}

		// ---- the bench and the numbers
		std::atomic<int>   gForced{ -1 }, gShown{ -1 }, gParticles{ 0 }, gArcs{ 0 };
		std::atomic<bool>  gBody{ false }, gFillOwn{ false }, gSprites{ false };
		std::atomic<float> gDrawUsAvg{ 0.0f }, gDrawUsMax{ 0.0f };

		void Measure(Steady::time_point a_start)
		{
			const float us = std::chrono::duration<float, std::micro>(Steady::now() - a_start).count();
			const float avg = gDrawUsAvg.load();
			gDrawUsAvg = avg <= 0.0f ? us : avg + (us - avg) * 0.05f;  // about the last 20 frames
			gDrawUsMax = std::max(gDrawUsMax.load(), us);
		}

		// the render thread's copy of the spell's art, taken only when SpellArt.cpp published a new one
		const SpellArtView& ArtNow()
		{
			static SpellArtView  art;
			static std::uint32_t version = ~0u;
			if (const auto v = SpellArtVersion(); v != version) {
				art = SpellArtNow();
				version = v;
			}
			return art;
		}

		// a quad turned by a_angle about (a_cx, a_cy), a_w by a_h
		void Quad(const Calls& a_fn, ImDrawList* a_dl, ImTextureID a_tex, float a_cx, float a_cy, float a_w, float a_h, float a_angle, const Uv& a_uv, ImU32 a_col)
		{
			const float c = std::cos(a_angle), s = std::sin(a_angle);
			const float hx = a_w * 0.5f, hy = a_h * 0.5f;
			const auto  at = [&](float a_x, float a_y) { return ImVec2{ a_cx + a_x * c - a_y * s, a_cy + a_x * s + a_y * c }; };
			a_fn.quad(a_dl, a_tex, at(-hx, -hy), at(hx, -hy), at(hx, hy), at(-hx, hy), ImVec2{ a_uv.u0, a_uv.v0 }, ImVec2{ a_uv.u1, a_uv.v0 },
				ImVec2{ a_uv.u1, a_uv.v1 }, ImVec2{ a_uv.u0, a_uv.v1 }, a_col);
		}
	}

	bool DrawBars(ImDrawList* a_dl, const Settings& a_s, const MeterView& a_v, float a_alpha, float a_dt)
	{
		const auto  start = Steady::now();
		const auto& fn = Fn();
		auto*       io = ImGuiMCP::GetIO();
		if (!a_dl || !io || !fn.quad || !fn.image || !fn.callback) {
			return false;
		}
		const auto& tex = Textures();
		MakeStates();
		gSprites = tex.sheet && tex.fill;

		// which look, and whose art: the spell's own unless the bench forced another kind
		const int   forced = gForced.load(std::memory_order_relaxed);
		const auto  look = forced >= 0 && forced < static_cast<int>(SpellLook::kLooks) ? static_cast<SpellLook::Look>(forced) : a_v.look;
		const auto& prof = BarFx::ProfileOf(look);
		const auto& art = ArtNow();
		const bool  own = look == a_v.look && a_v.spell && art.form == a_v.spell->GetFormID();
		const auto  body = own ? static_cast<ImTextureID>(art.body) : nullptr;
		const auto  fillOwn = own ? static_cast<ImTextureID>(art.fill) : nullptr;
		const auto  fillTex = fillOwn ? fillOwn : (body && art.frames.empty() ? body : tex.fill);
		gBody = body != nullptr;
		gFillOwn = fillOwn != nullptr || (body != nullptr && art.frames.empty());
		const BarFx::Rgb spell{ art.r, art.g, art.b };
		const bool       haveSpell = own && art.colored;
		const BarFx::Rgb main = haveSpell ? spell : prof.core;

		// where, and what the meter says
		const float  k = std::clamp(a_s.barWidth, 0.5f, 3.0f);
		const float  w = 240.0f * k, h = 10.0f * k;
		const float  cx = io->DisplaySize.x * std::clamp(a_s.barX, 0.0f, 1.0f), y0 = io->DisplaySize.y * std::clamp(a_s.barY, 0.0f, 1.0f);
		const float  x0 = cx - w * 0.5f;
		const bool   overdrive = a_v.state == Heat::Meter::State::kOverdrive, burnout = a_v.state == Heat::Meter::State::kCooldown;
		const float  left = a_v.timerOf > 0.0f ? std::clamp(a_v.timer / a_v.timerOf, 0.0f, 1.0f) : 0.0f;
		const float  heat = std::clamp(a_v.heat / Heat::kFull, 0.0f, 1.0f);
		const float  fill = overdrive || burnout ? left : heat;
		const auto   phase = overdrive ? Overload::Phase::kOverdrive : burnout ? Overload::Phase::kBurnout :
		                                                                         Overload::Phase::kBuilding;
		const float  power = Overload::Power(phase, heat, left, 1.0f);  // the bar itself always doubles in Overdrive
		const float  t = static_cast<float>(ImGuiMCP::GetTime());
		BarFx::Drive drive;
		drive.fill = fill;
		drive.heat = overdrive ? 1.0f : burnout ? 0.25f :
		                                          heat;
		drive.power = power;
		drive.burnout = burnout;
		drive.w = w;
		drive.h = h;
		drive.k = k;
		static BarFx::Bar      bar;
		static SpellLook::Look lastLook = SpellLook::Look::kGeneric;
		if (look != lastLook) {
			bar.Clear();  // a new kind: its own particles, never the last kind's left drifting
			lastLook = look;
		}
		bar.Step(prof, drive, a_dt, t);
		const float alpha = std::clamp(a_alpha, 0.0f, 1.0f);
		const float sputter = burnout && a_s.burnoutFlicker && !Overload::Shows(t, left, 0xBA5Eu) ? 0.15f : 1.0f;
		const float fw = w * fill;

		// 1 · the slot (blended): a dark bed, a thin rim in the spell's color
		ImGuiMCP::ImDrawListManager::AddRectFilled(a_dl, ImVec2{ x0 - 3.0f * k, y0 - 3.0f * k }, ImVec2{ x0 + w + 3.0f * k, y0 + h + 3.0f * k },
			Col(0.025f, 0.022f, 0.03f, 0.55f * alpha), 4.0f * k, 0);
		ImGuiMCP::ImDrawListManager::AddRect(a_dl, ImVec2{ x0 - 3.0f * k, y0 - 3.0f * k }, ImVec2{ x0 + w + 3.0f * k, y0 + h + 3.0f * k },
			burnout ? Col(0.45f, 0.5f, 0.6f, 0.45f * alpha) : Col(prof.edge.r * 0.8f + 0.1f, prof.edge.g * 0.8f + 0.1f, prof.edge.b * 0.8f + 0.1f, 0.5f * alpha),
			4.0f * k, 0, std::max(1.0f, k));

		const auto mainCol = [&](float a_mul, float a_a) {
			if (burnout) {
				return Col(0.42f * a_mul, 0.48f * a_mul, 0.6f * a_mul, a_a);
			}
			return Col(main.r * a_mul, main.g * a_mul, main.b * a_mul, a_a);
		};
		// one flowing texture layer along the filled part, fading out over a soft head
		float ua = 0.0f, ub = 0.0f;
		BarFx::FlowUv(t, prof.flow, power, ua, ub);
		const auto fillLayer = [&](ImTextureID a_tex, float a_u, bool a_flip, float a_mul, float a_a) {
			constexpr int kSoft = 8;
			const float   span = h * 2.0f;  // the texture repeats every two bar heights
			const float   soft = std::min(fw, h * 2.0f);
			const float   body0 = fw - soft;
			if (body0 > 0.5f) {
				fn.image(a_dl, a_tex, ImVec2{ x0, y0 }, ImVec2{ x0 + body0, y0 + h }, ImVec2{ a_u, a_flip ? 1.0f : 0.0f },
					ImVec2{ a_u + body0 / span, a_flip ? 0.0f : 1.0f }, mainCol(a_mul, a_a * alpha * sputter));
			}
			for (int i = 0; i < kSoft; ++i) {
				const float s0 = body0 + soft * static_cast<float>(i) / kSoft, s1 = body0 + soft * static_cast<float>(i + 1) / kSoft;
				const float fade = 1.0f - (static_cast<float>(i) + 0.5f) / kSoft;
				fn.image(a_dl, a_tex, ImVec2{ x0 + s0, y0 }, ImVec2{ x0 + s1, y0 + h }, ImVec2{ a_u + s0 / span, a_flip ? 1.0f : 0.0f },
					ImVec2{ a_u + s1 / span, a_flip ? 0.0f : 1.0f }, mainCol(a_mul, a_a * fade * alpha * sputter));
			}
		};

		// 2 · the base fill (blended): the flowing strip in the spell's color, so how full the bar is reads on any background -
		// added light alone washes out on snow (the in-game test 2026-10-15)
		if (fw > 1.0f && tex.fill) {
			fillLayer(tex.fill, ua * 0.5f, false, 0.85f, 0.7f);
		}

		// 3 · the glow and the spell's own texture flowing over it (added from here on)
		fn.callback(a_dl, Additive, nullptr);
		if (fw > 1.0f && tex.sheet) {
			const float breath = overdrive ? 0.85f + 0.15f * std::sin(t * 10.0f) : 1.0f;
			fn.image(a_dl, tex.sheet, ImVec2{ x0 - h * 1.5f, y0 - h * 1.6f }, ImVec2{ x0 + fw + h * 1.5f, y0 + h * 2.6f }, ImVec2{ Cell(BarFx::Sprite::kGlow).u0, 0.0f },
				ImVec2{ Cell(BarFx::Sprite::kGlow).u1, 1.0f }, mainCol(1.0f, (0.18f + 0.22f * drive.heat) * breath * alpha * sputter));
		}
		if (fw > 1.0f && fillTex) {
			fillLayer(fillTex, ua, false, 0.95f, 0.85f);
			fillLayer(fillTex, ub, true, 0.7f, 0.55f);
		}
		// 4 · the particles, all round the bar
		int alive = 0;
		for (std::size_t i = 0; i < bar.All().size(); ++i) {
			const auto& p = bar.All()[i];
			if (!p.alive) {
				continue;
			}
			++alive;
			const float l = p.Left();
			const float in = std::clamp(p.age * 8.0f, 0.0f, 1.0f);
			const auto  c = BarFx::ColorOf(prof, spell, haveSpell, l, burnout);
			const float a = in * std::pow(l, 0.8f) * alpha * sputter * (p.body ? 0.65f : 0.95f);
			const float size = std::max(p.Size(), 0.5f);
			const float px = x0 + p.x, py = y0 + p.y;
			// Every particle - "body" ones included - wears one of OUR sprite cells, tinted in the spell's own color (BarFx::ColorOf).
			// The spell's particle texture is NOT drawn here: measured 2026-10-15 on his Bottled Shaders order, a game particle sheet
			// is a grey flipbook the game colors through a palette and adds as light, and the cells its model names need not match
			// the winning texture's grid (Flames: 18 cells named, the winning sheet an 8 x 4 grid of 32) - on a HUD quad it drew as
			// tilted tan cards (his "those flames look super boxy"). The spell's own art stays in the fill and the colors.
			if (!tex.sheet) {
				continue;
			}
			if (p.body) {  // a soft glow behind the body, so it reads on a bright background too
				Quad(fn, a_dl, tex.sheet, px, py, size * 1.5f, size * 1.5f, 0.0f, Cell(BarFx::Sprite::kGlow), Col(c.r, c.g, c.b, a * 0.35f));
			}
			const auto sprite = p.body ? prof.bodySprite : (p.second ? prof.fleck2 : prof.fleck);
			if (sprite == BarFx::Sprite::kStreak) {  // a streak is drawn along its flight
				const float speed = std::sqrt(p.vx * p.vx + p.vy * p.vy);
				Quad(fn, a_dl, tex.sheet, px, py, size * (1.5f + std::min(speed / (60.0f * k), 3.0f)), size * 0.45f, std::atan2(p.vy, p.vx), Cell(sprite),
					Col(c.r, c.g, c.b, a));
			} else {
				Quad(fn, a_dl, tex.sheet, px, py, size, size, p.angle, Cell(sprite), Col(c.r, c.g, c.b, a));
			}
		}

		// 5 · shock's arcs: a wide faint glow and a bright core
		int arcs = 0;
		if (fn.polyline) {
			for (const auto& arc : bar.Arcs()) {
				if (!arc.Alive()) {
					continue;
				}
				++arcs;
				std::array<ImVec2, BarFx::kArcPoints> pts{};
				for (int i = 0; i < BarFx::kArcPoints; ++i) {
					pts[static_cast<std::size_t>(i)] = ImVec2{ x0 + arc.pts[static_cast<std::size_t>(2 * i)], y0 + arc.pts[static_cast<std::size_t>(2 * i + 1)] };
				}
				const float fade = 1.0f - arc.age / std::max(arc.life, 0.001f);
				fn.polyline(a_dl, pts.data(), BarFx::kArcPoints, Col(main.r, main.g, main.b, 0.35f * fade * alpha), 0, arc.width * 4.0f);
				fn.polyline(a_dl, pts.data(), BarFx::kArcPoints, Col(0.95f, 0.95f, 1.0f, 0.95f * fade * alpha), 0, arc.width);
			}
		}

		// 6 · the head: a flare where the fill ends, bigger and breathing in Overdrive
		if (fw > 1.0f && tex.sheet) {
			const float breath = 0.8f + 0.2f * std::sin(t * (burnout ? 2.0f : 7.0f));
			const float r = h * 2.2f * breath * (0.75f + 0.25f * power);
			Quad(fn, a_dl, tex.sheet, x0 + fw, y0 + h * 0.5f, r * 2.0f, r * 2.0f, 0.0f, Cell(BarFx::Sprite::kGlow), mainCol(1.2f, 0.7f * alpha * sputter));
			Quad(fn, a_dl, tex.sheet, x0 + fw, y0 + h * 0.5f, r * 0.7f, r * 0.7f, t * 0.7f, Cell(BarFx::Sprite::kStar), Col(1.0f, 1.0f, 1.0f, 0.6f * alpha * sputter));
		}
		fn.callback(a_dl, Probe, nullptr);
		fn.callback(a_dl, ResetState(), nullptr);

		// 7 · the stage's name over the right end ("READY" while the key waits)
		static constexpr const char* kNames[]{ "", "WARM", "HOT", "OVERHEATING", "OVERDRIVE", "BURNOUT" };
		const auto                   mood = burnout ? 5 : overdrive ? 4 :
		                                                              std::clamp(static_cast<int>(a_v.stage), 0, 3);
		const char*                  name = a_v.ready ? "READY" : kNames[static_cast<std::size_t>(mood)];
		if (*name) {
			const float  glint = a_v.ready ? 0.6f + 0.4f * std::sin(t * 6.0f) : 1.0f;
			const ImVec2 size = ImGuiMCP::CalcTextSize(name);
			const ImVec2 at{ x0 + w - size.x, y0 - size.y - 5.0f * k };
			ImGuiMCP::ImDrawListManager::AddText(a_dl, ImVec2{ at.x + 1.0f, at.y + 1.0f }, Col(0.0f, 0.0f, 0.0f, 0.8f * alpha), name);
			ImGuiMCP::ImDrawListManager::AddText(a_dl, at,
				burnout ? Col(0.62f, 0.7f, 0.82f, glint * alpha) : Col(main.r * 0.5f + 0.5f, main.g * 0.5f + 0.5f, main.b * 0.5f + 0.5f, glint * alpha), name);
		}

		gShown = static_cast<int>(look);
		gParticles = alive;
		gArcs = arcs;
		Measure(start);
		return true;
	}

	LookStats LookStatsNow()
	{
		LookStats s;
		s.shown = gShown.load();
		s.forced = gForced.load();
		s.spellBody = gBody.load();
		s.spellFill = gFillOwn.load();
		s.sprites = gSprites.load();
		s.particles = gParticles.load();
		s.arcs = gArcs.load();
		s.drawUsAvg = gDrawUsAvg.load();
		s.drawUsMax = gDrawUsMax.load();
		s.addCalls = gAddCalls.load();
		s.probeCalls = gProbeCalls.load();
		s.keptAdd = gKeptAdd.load();
		s.additiveMade = gAdd != nullptr;
		s.rtWidth = gRtWidth.load();
		s.rtHeight = gRtHeight.load();
		s.rtFormat = gRtFormat.load();
		s.rtIsBack = gRtIsBack.load();
		return s;
	}

	void BenchForceLook(int a_look)
	{
		gForced = a_look >= 0 && a_look < static_cast<int>(SpellLook::kLooks) ? a_look : -1;
	}
}

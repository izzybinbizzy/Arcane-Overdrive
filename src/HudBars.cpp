// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The bars on the HUD, the high-res ones only (his answer: "remove the shitty bars there will only be high res bars"): the
// Overdrive bar and, right above it, the thin Heat bar, each ONE quad painted by BarShader.h - dark glass in a dark iron
// frame, the spell's own kind of effect inside, nothing can leave the frame and it stays sharp at any size. The shader is
// compiled once on a worker thread (a first compile can stall a frame) and bound for each quad by an ImGui draw callback that
// carries that quad's own numbers. Until it is ready - or on a PC without the compiler - a plain bar stands in, so the HUD
// never shows nothing. The colors are the spell's own (SpellArt.cpp: its light, else its art) when they have a hue, else
// the look's. Everything here runs on the render thread (the HUD element), except LookStatsNow and BenchForceLook.

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
#include "BarShader.h"
#include "HudBars.h"
#include "Overload.h"

#include <REX/W32/D3DCOMPILER.h>

#include <cstring>
#include <thread>

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

		// the framework's calls, looked up ONCE (its header looks each one up by name on every call - a frame's worth of
		// GetProcAddress for nothing)
		using AddImageFn = void (*)(ImDrawList*, ImTextureID, ImVec2, ImVec2, ImVec2, ImVec2, ImU32);
		using AddCallbackFn = void (*)(ImDrawList*, ImDrawCallback, void*);
		using AddTextFn = void (*)(ImDrawList*, const ImVec2, ImU32, const char*, const char*);
		using AddRectFn = void (*)(ImDrawList*, const ImVec2, const ImVec2, ImU32, float, ImGuiMCP::ImDrawFlags);
		using GetIOFn = ImGuiMCP::ImGuiIO* (*)();
		using GetTimeFn = double (*)();
		using GetListFn = ImDrawList* (*)();
		struct Calls
		{
			AddImageFn    image{ nullptr };
			AddCallbackFn callback{ nullptr };
			AddTextFn     text{ nullptr };
			AddRectFn     rect{ nullptr };
			GetIOFn       io{ nullptr };
			GetTimeFn     time{ nullptr };
			GetListFn     list{ nullptr };
		};
		const Calls& Fn()
		{
			static const Calls calls{ ::GetMenuFrameworkFunction<AddImageFn>("ImDrawList_AddImage"),
				::GetMenuFrameworkFunction<AddCallbackFn>("ImDrawList_AddCallback"), ::GetMenuFrameworkFunction<AddTextFn>("ImDrawList_AddText_Vec2"),
				::GetMenuFrameworkFunction<AddRectFn>("ImDrawList_AddRectFilled"), ::GetMenuFrameworkFunction<GetIOFn>("igGetIO"),
				::GetMenuFrameworkFunction<GetTimeFn>("igGetTime"), ::GetMenuFrameworkFunction<GetListFn>("igGetForegroundDrawList_Nil") };
			return calls;
		}

		// The head of ImGui's own DX11 backend data (ImGui_ImplDX11_Data in ImGui 1.90.8, the version SKSE Menu Framework 3 is
		// built on): the device and the context every HUD draw is issued on; the renderer's pair is only the fallback.
		struct BackendHead
		{
			REX::W32::ID3D11Device*        device;
			REX::W32::ID3D11DeviceContext* context;
		};
		const BackendHead* Backend()
		{
			auto*       io = Fn().io ? Fn().io() : nullptr;
			const auto* head = io ? static_cast<const BackendHead*>(io->BackendRendererUserData) : nullptr;
			return head && head->device && head->context ? head : nullptr;
		}
		REX::W32::ID3D11DeviceContext* Context()
		{
			if (const auto* imgui = Backend()) {
				return imgui->context;
			}
			auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
			return renderer ? renderer->GetRuntimeData().context : nullptr;
		}

		// ImGui's own "put the renderer's state back" mark (ImDrawCallback_ResetRenderState, -8 in ImGui 1.90.8 - its DX11
		// backend checks for it before calling a callback)
		ImDrawCallback ResetState() { return reinterpret_cast<ImDrawCallback>(static_cast<std::intptr_t>(-8)); }

		using CompileFn = std::int32_t (*)(const void*, std::size_t, const char*, const void*, void*, const char*, const char*, std::uint32_t, std::uint32_t,
			REX::W32::ID3DBlob**, REX::W32::ID3DBlob**);

		REX::W32::ID3D11PixelShader* gPs{ nullptr };
		REX::W32::ID3D11Buffer*      gCb{ nullptr };
		REX::W32::ID3D11BlendState*  gPremul{ nullptr };
		REX::W32::ID3D11Buffer*      gSavedCb{ nullptr };  // ImGui's backend does not restore pixel shader constant buffers
		std::atomic<int>             gShaderState{ 0 };    // 0 not asked, 1 compiling, 2 ready, 3 failed
		std::atomic<float>           gShaderMs{ 0.0f };
		std::atomic<std::uint32_t>   gShaderCalls{ 0 };
		// each quad's numbers, read by its own bind callback when ImGui renders the frame (after DrawBars has returned)
		BarShaderConstants gBar{}, gStrip{};

		void CompileShader(REX::W32::ID3D11Device* a_device)
		{
			const auto t0 = Steady::now();
			const auto fail = [](std::string_view a_why) {
				SKSE::log::warn("bars: {} - a plain bar stands in", a_why);
				gShaderState.store(3, std::memory_order_release);
			};
			// loaded by hand, so a PC without the compiler only loses the look instead of the whole plugin failing to load
			auto*      lib = REX::W32::LoadLibraryW(L"d3dcompiler_47.dll");
			const auto compile = lib ? reinterpret_cast<CompileFn>(REX::W32::GetProcAddress(lib, "D3DCompile")) : nullptr;
			if (!compile) {
				fail("d3dcompiler_47.dll could not be loaded");
				return;
			}
			REX::W32::ID3DBlob* code{ nullptr };
			REX::W32::ID3DBlob* errors{ nullptr };
			const auto          hr = compile(kBarShader.data(), kBarShader.size(), "ArcaneOverdriveBar", nullptr, nullptr, "main", "ps_5_0",
				static_cast<std::uint32_t>(REX::W32::D3DCOMPILE_OPTIMIZATION_LEVEL3), 0, &code, &errors);
			if (errors) {
				SKSE::log::info("bars, the shader compiler: {}", std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
				errors->Release();
			}
			if (hr < 0 || !code) {
				if (code) {
					code->Release();
				}
				fail(std::format("the shader did not compile ({:#x})", static_cast<std::uint32_t>(hr)));
				return;
			}
			const bool shader = a_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gPs) >= 0;
			code->Release();
			REX::W32::D3D11_BUFFER_DESC bd{};
			bd.byteWidth = sizeof(BarShaderConstants);
			bd.usage = REX::W32::D3D11_USAGE_DYNAMIC;
			bd.bindFlags = static_cast<std::uint32_t>(REX::W32::D3D11_BIND_CONSTANT_BUFFER);
			bd.cpuAccessFlags = static_cast<std::uint32_t>(REX::W32::D3D11_CPU_ACCESS_WRITE);
			const bool                 buffer = a_device->CreateBuffer(&bd, nullptr, &gCb) >= 0;
			REX::W32::D3D11_BLEND_DESC bl{};
			auto&                      rt = bl.renderTarget[0];
			rt.blendEnable = 1;
			rt.srcBlend = REX::W32::D3D11_BLEND_ONE;
			rt.destBlend = REX::W32::D3D11_BLEND_INV_SRC_ALPHA;
			rt.blendOp = REX::W32::D3D11_BLEND_OP_ADD;
			rt.srcBlendAlpha = REX::W32::D3D11_BLEND_ONE;
			rt.destBlendAlpha = REX::W32::D3D11_BLEND_INV_SRC_ALPHA;
			rt.blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD;
			rt.renderTargetWriteMask = static_cast<std::uint8_t>(REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL);
			const bool blend = a_device->CreateBlendState(&bl, &gPremul) >= 0;
			if (!shader || !buffer || !blend) {
				fail(std::format("the device refused it (shader {}, constants {}, blend {})", shader, buffer, blend));
				return;
			}
			gShaderMs = std::chrono::duration<float, std::milli>(Steady::now() - t0).count();
			SKSE::log::info("bars: the shader compiled and made in {:.0f} ms", gShaderMs.load());
			gShaderState.store(2, std::memory_order_release);
		}

		void Bind(const ImDrawList*, const ImDrawCmd* a_cmd)
		{
			auto*       ctx = Context();
			const auto* numbers = a_cmd ? static_cast<const BarShaderConstants*>(a_cmd->UserCallbackData) : nullptr;
			if (!ctx || !numbers || gShaderState.load(std::memory_order_acquire) != 2) {
				return;
			}
			REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
			if (ctx->Map(gCb, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped) >= 0 && mapped.data) {
				std::memcpy(mapped.data, numbers, sizeof(BarShaderConstants));
				ctx->Unmap(gCb, 0);
			}
			ctx->PSGetConstantBuffers(0, 1, &gSavedCb);
			ctx->PSSetShader(gPs, nullptr, 0);
			ctx->PSSetConstantBuffers(0, 1, &gCb);
			constexpr float kFactor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
			ctx->OMSetBlendState(gPremul, kFactor, 0xFFFFFFFFu);
			gShaderCalls.fetch_add(1, std::memory_order_relaxed);
		}

		void Unbind(const ImDrawList*, const ImDrawCmd*)
		{
			if (auto* ctx = Context()) {
				ctx->PSSetConstantBuffers(0, 1, &gSavedCb);
			}
			if (gSavedCb) {
				gSavedCb->Release();
				gSavedCb = nullptr;
			}
		}

		// The bars' own GPU time, only while DevBench asks for it (set=gpu value=1): two timestamps round our quads, read back
		// frames later without waiting (four clocks in turn, so the GPU is never stalled for them).
		struct GpuClock
		{
			REX::W32::ID3D11Query* disjoint{ nullptr };
			REX::W32::ID3D11Query* begin{ nullptr };
			REX::W32::ID3D11Query* end{ nullptr };
			bool                   pending{ false };
		};
		std::atomic<bool>       gGpuWanted{ false };
		std::atomic<float>      gGpuUs{ -1.0f };
		std::array<GpuClock, 4> gClocks{};
		std::size_t             gClockNext = 0;
		GpuClock*               gClockRunning = nullptr;

		bool MakeClock(GpuClock& a_c)
		{
			const auto* imgui = Backend();
			auto*       device = imgui ? imgui->device : RE::BSGraphics::Renderer::GetDevice();
			if (!device) {
				return false;
			}
			REX::W32::D3D11_QUERY_DESC qd{ REX::W32::D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			if (!a_c.disjoint && device->CreateQuery(&qd, &a_c.disjoint) < 0) {
				return false;
			}
			qd.query = REX::W32::D3D11_QUERY_TIMESTAMP;
			return (a_c.begin || device->CreateQuery(&qd, &a_c.begin) >= 0) && (a_c.end || device->CreateQuery(&qd, &a_c.end) >= 0);
		}

		void ClockStart(const ImDrawList*, const ImDrawCmd*)
		{
			auto* ctx = Context();
			auto& c = gClocks[gClockNext];
			if (!ctx || c.pending || !MakeClock(c)) {
				return;
			}
			ctx->Begin(c.disjoint);
			ctx->End(c.begin);
			gClockRunning = &c;
			gClockNext = (gClockNext + 1) % gClocks.size();
		}

		void ClockStop(const ImDrawList*, const ImDrawCmd*)
		{
			auto* ctx = Context();
			if (!ctx || !gClockRunning) {
				return;
			}
			ctx->End(gClockRunning->end);
			ctx->End(gClockRunning->disjoint);
			gClockRunning->pending = true;
			gClockRunning = nullptr;
		}

		// a finished clock's reading, never waiting (D3D11_ASYNC_GETDATA_DONOTFLUSH)
		void ReadClocks()
		{
			auto* ctx = Context();
			if (!ctx) {
				return;
			}
			constexpr auto kNoFlush = static_cast<std::uint32_t>(REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH);
			for (auto& c : gClocks) {
				REX::W32::D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				std::uint64_t                                 a = 0, b = 0;
				if (!c.pending || ctx->GetData(c.disjoint, &dj, sizeof(dj), kNoFlush) != 0 || ctx->GetData(c.begin, &a, sizeof(a), kNoFlush) != 0 ||
					ctx->GetData(c.end, &b, sizeof(b), kNoFlush) != 0) {
					continue;
				}
				c.pending = false;
				if (!dj.disjoint && dj.frequency && b >= a) {
					const float us = static_cast<float>(static_cast<double>(b - a) / static_cast<double>(dj.frequency) * 1e6);
					const float avg = gGpuUs.load();
					gGpuUs = avg < 0.0f ? us : avg + (us - avg) * 0.05f;
				}
			}
		}

		struct Rgb
		{
			float r, g, b;
		};
		struct Ramp
		{
			Rgb core, mid, deep;
		};

		// the look's own colors - the spell's replace them when they have a hue; healing and sun keep their gold (his "make the
		// healing more gold and bright")
		Ramp LookRamp(SpellLook::Look a_look)
		{
			using L = SpellLook::Look;
			switch (a_look) {
			case L::kFire:
				return { { 1.0f, 0.86f, 0.48f }, { 1.0f, 0.45f, 0.08f }, { 0.6f, 0.07f, 0.02f } };
			case L::kFrost:
				return { { 0.86f, 0.96f, 1.0f }, { 0.38f, 0.72f, 1.0f }, { 0.05f, 0.16f, 0.48f } };
			case L::kShock:
				return { { 0.93f, 0.9f, 1.0f }, { 0.58f, 0.48f, 1.0f }, { 0.22f, 0.08f, 0.5f } };
			case L::kAbsorbHealth:
				return { { 1.0f, 0.78f, 0.75f }, { 0.9f, 0.12f, 0.12f }, { 0.35f, 0.02f, 0.04f } };
			case L::kAbsorbMagicka:
				return { { 0.8f, 0.9f, 1.0f }, { 0.2f, 0.45f, 1.0f }, { 0.04f, 0.08f, 0.4f } };
			case L::kAbsorbStamina:
				return { { 0.85f, 1.0f, 0.8f }, { 0.25f, 0.85f, 0.25f }, { 0.04f, 0.3f, 0.06f } };
			case L::kHealing:
				return { { 1.0f, 0.96f, 0.78f }, { 1.0f, 0.75f, 0.27f }, { 0.7f, 0.43f, 0.08f } };
			case L::kIllusion:
				return { { 0.95f, 0.85f, 1.0f }, { 0.7f, 0.35f, 1.0f }, { 0.25f, 0.05f, 0.4f } };
			case L::kConjuration:
				return { { 0.9f, 0.75f, 1.0f }, { 0.55f, 0.2f, 0.9f }, { 0.12f, 0.02f, 0.22f } };
			case L::kBound:
				return { { 0.85f, 0.95f, 1.0f }, { 0.35f, 0.65f, 1.0f }, { 0.05f, 0.12f, 0.4f } };
			case L::kAlteration:
				return { { 1.0f, 0.97f, 0.85f }, { 0.95f, 0.8f, 0.5f }, { 0.35f, 0.25f, 0.12f } };
			case L::kSun:
				return { { 1.0f, 1.0f, 0.92f }, { 1.0f, 0.85f, 0.45f }, { 0.6f, 0.38f, 0.08f } };
			case L::kPoison:
				return { { 0.85f, 1.0f, 0.6f }, { 0.35f, 0.8f, 0.15f }, { 0.06f, 0.25f, 0.03f } };
			default:
				return { { 0.85f, 0.8f, 1.0f }, { 0.55f, 0.45f, 1.0f }, { 0.18f, 0.1f, 0.45f } };
			}
		}

		Ramp RampOf(SpellLook::Look a_look, const SpellArtView* a_spell)
		{
			using L = SpellLook::Look;
			if (a_spell && a_look != L::kHealing && a_look != L::kSun) {
				const float hi = std::max({ a_spell->r, a_spell->g, a_spell->b });
				const float lo = std::min({ a_spell->r, a_spell->g, a_spell->b });
				if (hi > 0.05f && (hi - lo) / hi >= 0.25f) {  // a grey smoke texture has no hue to build on
					const Rgb c{ a_spell->r, a_spell->g, a_spell->b };
					return { { c.r + (1.0f - c.r) * 0.55f, c.g + (1.0f - c.g) * 0.55f, c.b + (1.0f - c.b) * 0.55f }, c, { c.r * 0.35f, c.g * 0.35f, c.b * 0.35f } };
				}
			}
			return LookRamp(a_look);
		}

		BarLook ShaderLook(SpellLook::Look a_look)
		{
			using L = SpellLook::Look;
			switch (a_look) {
			case L::kFire:
				return BarLook::kFire;
			case L::kFrost:
				return BarLook::kFrost;
			case L::kShock:
				return BarLook::kShock;
			case L::kAbsorbHealth:
			case L::kAbsorbMagicka:
			case L::kAbsorbStamina:
				return BarLook::kEssence;
			case L::kHealing:
				return BarLook::kHealing;
			case L::kIllusion:
				return BarLook::kIllusion;
			case L::kConjuration:
				return BarLook::kConjuration;
			case L::kBound:
				return BarLook::kBound;
			case L::kAlteration:
				return BarLook::kAlteration;
			case L::kSun:
				return BarLook::kSun;
			case L::kPoison:
				return BarLook::kPoison;
			default:
				return BarLook::kGeneric;
			}
		}

		ImU32 Col(float a_r, float a_g, float a_b, float a_a)
		{
			const auto c = [](float a_v) { return static_cast<ImU32>(std::lround(std::clamp(std::isfinite(a_v) ? a_v : 0.0f, 0.0f, 1.0f) * 255.0f)); };
			return c(a_r) | (c(a_g) << 8) | (c(a_b) << 16) | (c(a_a) << 24);
		}

		// ---- the bench and the numbers
		std::atomic<int>   gForced{ -1 }, gShown{ -1 };
		std::atomic<float> gDrawUsAvg{ 0.0f }, gDrawUsMax{ 0.0f };

		void Measure(Steady::time_point a_start)
		{
			const float us = std::chrono::duration<float, std::micro>(Steady::now() - a_start).count();
			const float avg = gDrawUsAvg.load();
			gDrawUsAvg = avg <= 0.0f ? us : avg + (us - avg) * 0.05f;  // about the last 20 frames
			gDrawUsMax = std::max(gDrawUsMax.load(), us);
		}

		// the render thread's copy of the spell's colors, taken only when SpellArt.cpp published a new one
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

		void Fill(BarShaderConstants& a_c, float a_qw, float a_qh, float a_frame, float a_radius, float a_t, float a_fill, float a_heat, float a_power,
			BarLook a_look, float a_phase, float a_fade, float a_sputter, const Ramp& a_ramp)
		{
			a_c = { { a_qw, a_qh, a_frame, a_radius }, { a_t, a_fill, a_heat, a_power }, { static_cast<float>(static_cast<int>(a_look)), a_phase, a_fade, a_sputter },
				{ a_ramp.core.r, a_ramp.core.g, a_ramp.core.b, 0.0f }, { a_ramp.mid.r, a_ramp.mid.g, a_ramp.mid.b, 0.0f },
				{ a_ramp.deep.r, a_ramp.deep.g, a_ramp.deep.b, 0.0f } };
		}
	}

	bool DrawBars(ImDrawList* a_dl, const Settings& a_s, const MeterView& a_v, float a_alpha, float)
	{
		const auto  start = Steady::now();
		const auto& fn = Fn();
		auto*       io = fn.io ? fn.io() : nullptr;
		if (!a_dl || !io || !fn.text || !fn.rect || !fn.time) {
			return false;
		}
		if (gGpuWanted.load(std::memory_order_relaxed)) {
			ReadClocks();
		}
		if (gShaderState.load(std::memory_order_acquire) == 0 && fn.image && fn.callback) {
			const auto* imgui = Backend();
			if (auto* device = imgui ? imgui->device : RE::BSGraphics::Renderer::GetDevice()) {
				gShaderState.store(1, std::memory_order_release);
				std::thread(CompileShader, device).detach();
			}
		}

		// which look, and whose colors: the spell's own unless the bench forced another kind
		const int   forced = gForced.load(std::memory_order_relaxed);
		const auto  look = forced >= 0 && forced < static_cast<int>(SpellLook::kLooks) ? static_cast<SpellLook::Look>(forced) : a_v.look;
		const auto& art = ArtNow();
		const bool  own = look == a_v.look && a_v.spell && art.form == a_v.spell->GetFormID() && art.colored;
		const auto  ramp = RampOf(look, own ? &art : nullptr);

		// where, and what the bars say
		const float k = std::clamp(a_s.barWidth, 0.5f, 3.0f);
		const float w = 240.0f * k, h = 10.0f * k;
		const float cx = io->DisplaySize.x * std::clamp(a_s.barX, 0.0f, 1.0f), y0 = io->DisplaySize.y * std::clamp(a_s.barY, 0.0f, 1.0f);
		const float x0 = cx - w * 0.5f;
		const bool  overdrive = a_v.state == Heat::Meter::State::kOverdrive, burnout = a_v.state == Heat::Meter::State::kCooldown;
		const float left = a_v.timerOf > 0.0f ? std::clamp(a_v.timer / a_v.timerOf, 0.0f, 1.0f) : 0.0f;
		const float heat = std::clamp(a_v.heat / Heat::kFull, 0.0f, 1.0f);
		const float charge = std::clamp(a_v.charge / Heat::kFull, 0.0f, 1.0f);
		const float fill = overdrive || burnout ? left : charge;  // the Overdrive bar; the heat drives how fierce it looks
		const auto  phase = overdrive ? Overload::Phase::kOverdrive : burnout ? Overload::Phase::kBurnout :
		                                                                        Overload::Phase::kBuilding;
		const float power = Overload::Power(phase, charge, left, 1.0f);  // the bar itself always doubles in Overdrive
		const float drive = overdrive ? 1.0f : burnout ? 0.25f :
		                                                 heat;
		const float t = static_cast<float>(fn.time());
		const float alpha = std::clamp(a_alpha, 0.0f, 1.0f);
		const float sputter = burnout && a_s.burnoutFlicker && !Overload::Shows(t, left, 0xBA5Eu) ? 0.15f : 1.0f;
		const bool  strip = !burnout && a_v.heat > 0.5f;
		const float stripTop = y0 - kHeatStripAbove * k;

		// the stage's name over the right end, over the Heat bar: READY / COLD / TOO HOT / FOCUS while a full bar waits
		const auto label = [&] {
			static constexpr const char* kNames[]{ "", "WARM", "HOT", "OVERHEATING", "OVERDRIVE", "BURNOUT" };
			static constexpr const char* kGate[]{ "READY", "COLD", "TOO HOT", "FOCUS" };
			const auto                   mood = burnout ? 5 : overdrive ? 4 :
			                                                              std::clamp(static_cast<int>(a_v.stage), 0, 3);
			const char*                  name = a_v.ready ? kGate[static_cast<std::size_t>(a_v.gate)] : kNames[static_cast<std::size_t>(mood)];
			if (*name) {
				const float  glint = a_v.ready ? 0.6f + 0.4f * std::sin(t * 6.0f) : 1.0f;
				const ImVec2 size = ImGuiMCP::CalcTextSize(name);
				const ImVec2 at{ x0 + w - size.x, stripTop - size.y - 3.0f * k };
				const auto&  c = ramp.mid;
				fn.text(a_dl, ImVec2{ at.x + 1.0f, at.y + 1.0f }, Col(0.0f, 0.0f, 0.0f, 0.8f * alpha), name, nullptr);
				fn.text(a_dl, at, burnout ? Col(0.62f, 0.7f, 0.82f, glint * alpha) : Col(c.r * 0.5f + 0.5f, c.g * 0.5f + 0.5f, c.b * 0.5f + 0.5f, glint * alpha), name,
					nullptr);
			}
			gShown = static_cast<int>(look);
			Measure(start);
			return true;
		};

		// the shader: one quad for the Overdrive bar (3 px of frame round a 240 x 16 channel, standing on its floor) and one for the
		// Heat strip (2 px round 240 x 4); any texture carries the quad - the shader reads none, so the font atlas does
		const ImTextureID carrier = io->Fonts ? io->Fonts->TexID : nullptr;
		if (gShaderState.load(std::memory_order_acquire) == 2 && carrier) {
			const float qw = w + 6.0f * k, qh = 22.0f * k;
			const float qx = x0 - 3.0f * k, qy = y0 + h + k - qh;
			const float phaseNow = overdrive ? 1.0f : burnout ? 2.0f :
			                                                    0.0f;
			Fill(gBar, qw, qh, 3.0f * k, 6.0f * k, t, fill, drive, power, ShaderLook(look), phaseNow, alpha, sputter, ramp);
			const bool clock = gGpuWanted.load(std::memory_order_relaxed);
			if (clock) {
				fn.callback(a_dl, ClockStart, nullptr);
			}
			fn.callback(a_dl, Bind, &gBar);
			fn.image(a_dl, carrier, ImVec2{ qx, qy }, ImVec2{ qx + qw, qy + qh }, ImVec2{ 0.0f, 0.0f }, ImVec2{ 1.0f, 1.0f }, Col(1.0f, 1.0f, 1.0f, 1.0f));
			fn.callback(a_dl, Unbind, nullptr);
			if (strip) {
				const float sh = 10.0f * k;
				Fill(gStrip, qw, sh, 2.0f * k, 3.0f * k, t, heat, heat, 1.0f, BarLook::kHeatStrip, phaseNow, alpha, 1.0f, ramp);
				gStrip.core[3] = a_v.bandLow / Heat::kFull;
				gStrip.mid[3] = a_v.redLine / Heat::kFull;
				gStrip.deep[3] = a_s.tuning.heatGate ? 1.0f : 0.0f;
				fn.callback(a_dl, Bind, &gStrip);
				fn.image(a_dl, carrier, ImVec2{ qx, stripTop }, ImVec2{ qx + qw, stripTop + sh }, ImVec2{ 0.0f, 0.0f }, ImVec2{ 1.0f, 1.0f }, Col(1.0f, 1.0f, 1.0f, 1.0f));
				fn.callback(a_dl, Unbind, nullptr);
			}
			if (clock) {
				fn.callback(a_dl, ClockStop, nullptr);
			}
			fn.callback(a_dl, ResetState(), nullptr);
			return label();
		}

		// the stand-in while the shader compiles (or when it cannot): a plain bar in the same place, the spell's color
		const auto& c = burnout ? Rgb{ 0.42f, 0.48f, 0.6f } : ramp.mid;
		fn.rect(a_dl, ImVec2{ x0 - 3.0f * k, y0 - 3.0f * k }, ImVec2{ x0 + w + 3.0f * k, y0 + h + 3.0f * k }, Col(0.04f, 0.035f, 0.04f, 0.85f * alpha), 4.0f * k, 0);
		fn.rect(a_dl, ImVec2{ x0, y0 }, ImVec2{ x0 + w * fill, y0 + h }, Col(c.r, c.g, c.b, 0.9f * alpha * sputter), 2.0f * k, 0);
		if (strip) {
			fn.rect(a_dl, ImVec2{ x0, stripTop }, ImVec2{ x0 + w * heat, stripTop + 4.0f * k }, a_v.redZone ? Col(1.0f, 0.14f, 0.05f, alpha) : Col(1.0f, 0.55f, 0.15f, 0.9f * alpha),
				0.0f, 0);
		}
		return label();
	}

	ImGuiMCP::ImGuiIO* HudIO() { return Fn().io ? Fn().io() : nullptr; }

	ImGuiMCP::ImDrawList* HudList() { return Fn().list ? Fn().list() : nullptr; }

	void BenchGpuClock(bool a_on)
	{
		gGpuWanted = a_on;
		gGpuUs = -1.0f;
		gDrawUsAvg = 0.0f;
		gDrawUsMax = 0.0f;
		ResetFrameCost();
	}

	LookStats LookStatsNow()
	{
		LookStats s;
		s.shown = gShown.load();
		s.forced = gForced.load();
		s.drawUsAvg = gDrawUsAvg.load();
		s.drawUsMax = gDrawUsMax.load();
		s.shaderState = gShaderState.load();
		s.shaderMs = gShaderMs.load();
		s.shaderCalls = gShaderCalls.load();
		s.gpuUs = gGpuUs.load();
		return s;
	}

	void BenchForceLook(int a_look)
	{
		gForced = a_look >= 0 && a_look < static_cast<int>(SpellLook::kLooks) ? a_look : -1;
	}
}

// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The Overdrive bar as TrueHUD's special bar (TrueHUD by Ersh, GPL-3.0; TrueHUDAPI.h copied unchanged): the extra bar under
// the player's health, magicka and stamina, and on the floating bars of enemies whose spells build heat. Filled with the
// charge, colored by the Heat's stage, flashed when Overdrive is ready and at a misfire. TrueHUD lets one mod own its
// special bar at a time: when another holds it, Arcane says so in its log and on the Debug page, and leaves it be.

#include "Plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
// TrueHUD's published interface, copied unchanged; it calls GetModuleHandle with a narrow name, and its warnings are not ours
#pragma warning(push, 0)
#if defined(__clang__)
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Weverything"
#endif
#pragma push_macro("GetModuleHandle")
#undef GetModuleHandle
#define GetModuleHandle GetModuleHandleA
#include "TrueHUDAPI.h"
#pragma pop_macro("GetModuleHandle")
#if defined(__clang__)
#	pragma clang diagnostic pop
#endif
#pragma warning(pop)

namespace Plugin
{
	namespace
	{
		using TRUEHUD_API::APIResult;

		constexpr auto kAskAgain = std::chrono::seconds(10);  // another mod may give the special bar up later

		TRUEHUD_API::IVTrueHUD2*              gApi = nullptr;
		bool                                  gOwned = false, gWasReady = false;
		std::chrono::steady_clock::time_point gNextAsk{};
		bool                                  gTakenLogged = false;  // said once, not every 10 s
		std::uint32_t                         gColor = 0;
		int                                   gMisfires = 0;
		std::atomic<int>                      gState{ static_cast<int>(TrueHudState::kMissing) };

		// the bar's color for the Heat's stage, 0xRRGGBB: tan cold, amber, orange, flame, red in the red zone, white-hot in
		// Overdrive, ash in the burnout
		std::uint32_t ColorOf(const MeterView& a_v)
		{
			using S = Heat::Meter::State;
			if (a_v.state == S::kOverdrive) {
				return 0xFFF0C8;
			}
			if (a_v.state == S::kCooldown) {
				return 0x7A8A9E;
			}
			if (a_v.redZone) {
				return 0xD81E10;
			}
			switch (a_v.stage) {
			case Heat::Stage::kWarm:
				return 0xE0A040;
			case Heat::Stage::kHot:
				return 0xF07820;
			case Heat::Stage::kOverheating:
				return 0xFF4A1A;
			default:
				return 0xB09A7C;
			}
		}

		// TrueHUD asks these on its own thread for each actor whose bar it draws
		float Current(RE::Actor* a_actor) { return ChargeOf(a_actor); }
		float Most(RE::Actor*) { return Heat::kFull; }

		void Take()
		{
			const auto handle = SKSE::GetPluginHandle();
			const auto r = gApi->RequestSpecialResourceBarsControl(handle);
			if (r == APIResult::OK || r == APIResult::AlreadyGiven) {
				gOwned = gApi->RegisterSpecialResourceFunctions(handle, TRUEHUD_API::SpecialResourceCallback(Current), TRUEHUD_API::SpecialResourceCallback(Most), true) ==
				         APIResult::OK;
			}
			if (gOwned || r != APIResult::AlreadyTaken || !gTakenLogged) {
				SKSE::log::info("TrueHUD's special bar: {}", gOwned ? "the Overdrive bar" : "held by another mod - left to it, asked again every 10 s");
				gTakenLogged = !gOwned;
			}
		}

		void Let()
		{
			const auto handle = SKSE::GetPluginHandle();
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				gApi->RevertSpecialBarColor(player->GetHandle(), TRUEHUD_API::BarColorType::BarColor);
			}
			gApi->ReleaseSpecialResourceBarControl(handle);
			gOwned = false;
			gColor = 0;
			gNextAsk = {};  // switched on again: asked at once
			SKSE::log::info("TrueHUD's special bar: given back (switched off)");
		}
	}

	void FindTrueHud()
	{
		gApi = static_cast<TRUEHUD_API::IVTrueHUD2*>(TRUEHUD_API::RequestPluginAPI(TRUEHUD_API::InterfaceVersion::V2));
		SKSE::log::info("TrueHUD: {}", gApi ? "found" : "not installed - the bars show only as Reticle Arcs arcs, when that is on");
	}

	void UpdateTrueHud(const MeterView& a_v, const Settings& a_s)
	{
		if (!gApi) {
			gState = static_cast<int>(TrueHudState::kMissing);
			return;
		}
		const bool want = a_s.enabled && a_s.bar;
		const auto now = std::chrono::steady_clock::now();
		if (want && !gOwned && now >= gNextAsk) {
			gNextAsk = now + kAskAgain;
			Take();
		} else if (!want && gOwned) {
			Let();
		}
		if (!gOwned) {
			gState = static_cast<int>(want ? TrueHudState::kTaken : TrueHudState::kOff);
			return;
		}
		gState = static_cast<int>(TrueHudState::kOurs);
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		const auto me = player->GetHandle();
		if (const auto color = ColorOf(a_v); color != gColor) {
			gApi->OverrideSpecialBarColor(me, TRUEHUD_API::BarColorType::BarColor, color);
			gColor = color;
		}
		const auto handle = SKSE::GetPluginHandle();
		if (a_v.ready && !gWasReady) {
			gApi->FlashActorSpecialBar(handle, me, false);
		}
		if (a_v.misfires > gMisfires) {
			gApi->FlashActorSpecialBar(handle, me, true);
		}
		gWasReady = a_v.ready;
		gMisfires = a_v.misfires;
	}

	void ForgetTrueHud()
	{
		gColor = 0;  // the player's bar is made again with a new game: its color is sent again
		gWasReady = false;
		gMisfires = 0;
	}

	TrueHudState TrueHudNow() { return static_cast<TrueHudState>(gState.load()); }
}

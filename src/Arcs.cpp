// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The heat meter as an add-on bar for Reticle Arcs (his answer 2026-10-18, checklist 28 (d): "use reticle arcs"; the author's
// yes: "build around his mod using his api but don't add or change any code. just utilize the resources he provides. color
// will be added when he releases that part."). Only Reticle Arcs' published interface is used - ReticleArcsAPI.h, Reticle
// Arcs by Chiktabba (https://codeberg.org/chiktabba/ReticleArcs), MIT, copied unchanged; Reticle Arcs draws the bar in the
// player's own look. The meter: heat 0-100 in four pieces (the stages), a soft flow along it at Warm and Hot, a pulse at
// Overheating, the warning colour and an alarm while Overdrive counts down, then the burnout draining. No color is sent (his
// word above). While Reticle Arcs shows it, our own bar steps aside (Menu.cpp DrawBar); without Reticle Arcs nothing changes.

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include "Plugin.h"

// Reticle Arcs' own header (theirs, MIT) needs GetModuleHandleW / GetProcAddress; its warnings are not ours
#include <Windows.h>
#pragma warning(push, 0)
#if defined(__clang__)
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Weverything"
#endif
#include "ReticleArcsAPI.h"
#if defined(__clang__)
#	pragma clang diagnostic pop
#endif
#pragma warning(pop)

namespace Plugin
{
	namespace
	{
		using ReticleArcsAPI::BarState;
		using ReticleArcsAPI::Motion;
		using State = Heat::Meter::State;

		std::atomic<bool> gTaken{ false };

		[[nodiscard]] float Part(float a_left, float a_of) { return std::clamp(a_left / std::max(a_of, 0.1f), 0.0f, 1.0f); }

		void HeatBar(float, BarState& a_state)
		{
			const auto s = Config();
			if (!s.enabled || !s.arcHeat) {
				return;
			}
			const auto v = PlayerView();
			a_state.segments = 4;  // Warm, Hot, Overheating, Overdrive
			switch (v.state) {
			case State::kOverdrive:  // the burst counting down
				a_state.active = true;
				a_state.drain = true;
				a_state.percent = Part(v.timer, v.timerOf);
				a_state.alarm = true;
				a_state.motion = Motion::kPulse;
				return;
			case State::kCooldown:  // the burnout draining
				a_state.active = true;
				a_state.drain = true;
				a_state.percent = Part(v.timer, v.timerOf);
				a_state.low = true;
				return;
			default:
				if (v.heat <= 0.5f && !v.ready) {
					return;
				}
				a_state.active = true;
				a_state.percent = std::clamp(v.heat / Heat::kFull, 0.0f, 1.0f);
				a_state.motion = v.stage == Heat::Stage::kOverheating                          ? Motion::kPulse :
				                 v.stage == Heat::Stage::kWarm || v.stage == Heat::Stage::kHot ? Motion::kFlow :
				                                                                                 Motion::kNone;
				a_state.rim = v.ready;  // full and waiting for the Overdrive key
			}
		}

		// Reticle Arcs' settings page: heat climbing through the stages, Overdrive counting down, the burnout draining
		void Preview(float a_time, BarState& a_state)
		{
			const float t = std::fmod(std::max(a_time, 0.0f) / 6.0f, 1.0f);
			a_state.active = true;
			a_state.segments = 4;
			if (t < 0.5f) {
				a_state.percent = t / 0.5f;
				a_state.motion = a_state.percent >= 0.75f ? Motion::kPulse : a_state.percent >= 0.25f ? Motion::kFlow :
				                                                                                        Motion::kNone;
			} else if (t < 0.75f) {
				a_state.drain = true;
				a_state.percent = 1.0f - (t - 0.5f) / 0.25f;
				a_state.alarm = true;
				a_state.motion = Motion::kPulse;
			} else {
				a_state.drain = true;
				a_state.percent = 1.0f - (t - 0.75f) / 0.25f;
				a_state.low = true;
			}
		}
	}

	void RegisterArcBar()
	{
		if (!::GetModuleHandleW(L"ReticleArcs.dll")) {
			return;  // no Reticle Arcs: our own bar, as always
		}
		auto* arcs = ReticleArcsAPI::Request();
		if (!arcs) {
			SKSE::log::info("Reticle Arcs is too old for add-on bars; the heat meter stays our own bar");
			return;
		}
		ReticleArcsAPI::BarDefinition bar;
		bar.id = "ArcaneOverdriveHeat";
		bar.name = "Arcane Overdrive heat";
		bar.tooltip = "Arcane Overdrive's heat: Warm, Hot, Overheating, then Overdrive counting down and the burnout draining.";
		bar.placement = "top";
		bar.update = HeatBar;
		bar.preview = Preview;
		gTaken = arcs->RegisterBar(bar);
		SKSE::log::info("Reticle Arcs (interface {}) {} the heat meter", arcs->GetVersion(), gTaken ? "took" : "refused (ReticleArcs.log says why)");
	}

	bool ArcBarTaken() noexcept { return gTaken.load(std::memory_order_relaxed); }
}

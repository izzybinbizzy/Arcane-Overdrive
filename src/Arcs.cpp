// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The two bars as add-on bars for Reticle Arcs, as its author allows: an add-on through its API, none of its code changed
// or copied in; per-bar color waits for the author's release of it. Only Reticle Arcs' published interface is used - ReticleArcsAPI.h, Reticle Arcs by Chiktabba
// (https://codeberg.org/chiktabba/ReticleArcs), MIT, copied unchanged; Reticle Arcs draws the bars in the player's own look.
//   the Overdrive bar   the charge; a rim when it is full and the gate is open, the warning colour while the gate holds a
//                       full bar back; then Overdrive counting down with an alarm, and the burnout draining
//   the Heat bar        the heat in four pieces (Warm, Hot, Overheating at the default stages), a soft flow at Warm and Hot, a
//                       pulse at Overheating, the warning colour and an alarm in the red zone; it drains with Overdrive
// No color is sent (his word above). A menu choice beside TrueHUD's special bar (TrueHud.cpp); off by default.

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

		void ChargeBar(float, BarState& a_state)
		{
			const auto s = Config();
			if (!s.enabled || !s.arcHeat) {
				return;
			}
			const auto v = PlayerView();
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
				if (v.charge <= 0.5f) {
					return;
				}
				a_state.active = true;
				a_state.percent = std::clamp(v.charge / Heat::kFull, 0.0f, 1.0f);
				a_state.rim = v.ready && v.gate == Heat::GateNow::kOpen;  // full, and the key would let it go
				a_state.low = v.ready && v.gate != Heat::GateNow::kOpen;  // full, held back by the heat
			}
		}

		void HeatBar(float, BarState& a_state)
		{
			const auto s = Config();
			if (!s.enabled || !s.arcHeat) {
				return;
			}
			const auto v = PlayerView();
			if (v.state == State::kCooldown || v.heat <= 0.5f) {
				return;
			}
			a_state.active = true;
			a_state.segments = 4;
			a_state.percent = std::clamp(v.heat / Heat::kFull, 0.0f, 1.0f);
			a_state.drain = v.state == State::kOverdrive;  // spent as power
			a_state.low = v.redZone;
			a_state.alarm = v.redZone;
			a_state.motion = v.stage == Heat::Stage::kOverheating                          ? Motion::kPulse :
			                 v.stage == Heat::Stage::kWarm || v.stage == Heat::Stage::kHot ? Motion::kFlow :
			                                                                                 Motion::kNone;
		}

		// Reticle Arcs' settings page: the charge filling, Overdrive counting down, the burnout draining
		void ChargePreview(float a_time, BarState& a_state)
		{
			const float t = std::fmod(std::max(a_time, 0.0f) / 6.0f, 1.0f);
			a_state.active = true;
			if (t < 0.5f) {
				a_state.percent = t / 0.5f;
				a_state.rim = a_state.percent >= 0.99f;
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

		// ... and the heat climbing through the stages into the red, then cooling
		void HeatPreview(float a_time, BarState& a_state)
		{
			const float t = std::fmod(std::max(a_time, 0.0f) / 6.0f, 1.0f);
			a_state.active = true;
			a_state.segments = 4;
			a_state.percent = t < 0.6f ? t / 0.6f : 1.0f - (t - 0.6f) / 0.4f;
			a_state.low = a_state.alarm = a_state.percent >= 0.85f;
			a_state.motion = a_state.percent >= 0.75f ? Motion::kPulse : a_state.percent >= 0.25f ? Motion::kFlow :
			                                                                                        Motion::kNone;
		}
	}

	void RegisterArcBar()
	{
		if (!::GetModuleHandleW(L"ReticleArcs.dll")) {
			return;  // no Reticle Arcs: TrueHUD's bar only
		}
		auto* arcs = ReticleArcsAPI::Request();
		if (!arcs) {
			SKSE::log::info("Reticle Arcs is too old for add-on bars; TrueHUD's bar only");
			return;
		}
		ReticleArcsAPI::BarDefinition charge;
		charge.id = "ArcaneOverdriveCharge";
		charge.name = "Arcane Overdrive";
		charge.tooltip = "Arcane Overdrive's Overdrive bar: it fills as you cast, then Overdrive counting down and the burnout draining.";
		charge.placement = "top";
		charge.update = ChargeBar;
		charge.preview = ChargePreview;
		ReticleArcsAPI::BarDefinition heat;
		heat.id = "ArcaneOverdriveHeat";
		heat.name = "Arcane Overdrive heat";
		heat.tooltip = "Arcane Overdrive's Heat: how fast you cast - Warm, Hot, Overheating, and the red zone.";
		heat.placement = "top";
		heat.update = HeatBar;
		heat.preview = HeatPreview;
		const bool tookCharge = arcs->RegisterBar(charge);
		const bool tookHeat = arcs->RegisterBar(heat);
		gTaken = tookCharge && tookHeat;
		SKSE::log::info("Reticle Arcs (interface {}): the Overdrive bar {}, the Heat bar {}", arcs->GetVersion(), tookCharge ? "taken" : "refused",
			tookHeat ? "taken" : "refused (ReticleArcs.log says why)");
	}

	bool ArcBarTaken() noexcept { return gTaken.load(std::memory_order_relaxed); }
}

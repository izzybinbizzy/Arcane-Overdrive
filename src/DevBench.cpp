// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order: `inspect kind=spellmasteryheat` returns the Debug page as JSON - the player's
// two bars, the stage, what the gate says, Overdrive or cooldown and its seconds left, the misfires, the last school, the
// bonus and magicka regeneration now, the hand lights being scaled, the key, the preset and the settings - and `menu
// action=invoke name=spellmasteryheat` sets the Overdrive bar (set=charge value=0..100) or the Heat bar (set=heat), forces a
// stage (set=stage value=0 Cold .. 3 Overheating, 4 Overdrive), starts Overdrive (set=overdrive), presses or holds the
// Overdrive key (set=key, set=hold value=0|1), picks a preset (set=preset value=0..2), reads the advanced file again
// (set=reload), or sets any setting (set=setting key=<ini key> value=<n>). DevBench never saves; a later change made in
// the menu saves the settings as they are then. Nothing here runs unless DevBench asks.

#include "Plugin.h"

#include "DevBenchAPI.h"

#include <nlohmann/json.hpp>

namespace Plugin
{
	namespace
	{
		constexpr const char* kKey = "spellmasteryheat";
		constexpr unsigned    kNeedsBuild = 10500;  // DevBench 1.5.0: RegisterToolExtension

		constexpr const char* kInspect =
			R"({"description":"Arcane Overdrive - the player's Overdrive bar (charge 0-100) and Heat bar (heat 0-100), stage, the gate (open, cold, hot, focus) with the sweet spot's edges, red zone, focus held, misfires, state (building, Overdrive, cooldown) and seconds left, ready, last school, spell power and magicka regeneration multipliers now, effects boosted, enemies with heat, hand lights being scaled (and held steady), the Overdrive key, the cost (the main thread's work a frame, microseconds), the spells' overcharge (size, red-zone wildness, focus pull, explosions / hazards / hit effects grown), TrueHUD's special bar (missing, off, taken by another mod, ours), and the settings. Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})";

		constexpr const char* kMenu =
			R"({"description":"Arcane Overdrive - set=charge value=0..100 sets the player's Overdrive bar; set=heat value=0..100 sets the Heat bar; set=stage value=0..4 forces Cold, Warm, Hot, Overheating (the heat) or Overdrive; set=overdrive starts Overdrive, gate or not; set=key presses the Overdrive key; set=hold value=0|1 holds it down or lets it go (the focus gate); set=preset value=0..2 picks Milk Drinker, Dragonborn or Aedra; set=reload reads the advanced file again; set=setting key=<key of either settings file> value=<whole number> sets any setting. Not saved from here.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})";

		using json = nlohmann::json;

		std::string Dump(const json& a_j) { return a_j.dump(-1, ' ', false, json::error_handler_t::replace); }

		void InspectNow(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			if (!a_write) {
				return;
			}
			const auto v = PlayerView();
			const auto h = HandsNow();
			json       out;
			out["charge"] = v.charge;
			out["heat"] = v.heat;
			out["stage"] = std::string(Heat::StageName(v.stage));
			out["state"] = v.state == Heat::Meter::State::kOverdrive ? "overdrive" : v.state == Heat::Meter::State::kCooldown ? "cooldown" :
			                                                                                                                    "building";
			out["secondsLeft"] = v.timer;
			out["ready"] = v.ready;
			static constexpr const char* kGate[]{ "open", "cold", "hot", "focus" };
			out["gate"] = kGate[static_cast<int>(v.gate)];
			out["bandLow"] = v.bandLow;
			out["redLine"] = v.redLine;
			out["redZone"] = v.redZone;
			out["focus"] = v.focus;
			out["misfire"] = v.misfire;
			out["misfires"] = v.misfires;
			const auto preset = SettingsText::PresetOf(Config());
			out["preset"] = preset ? SettingsText::kPresets[*preset].name : "Custom";
			out["lastSchool"] = std::string(Heat::SchoolName(v.last));
			out["spellPower"] = v.bonus;
			out["magickaRegen"] = v.regen;
			out["boosted"] = v.boosted;
			out["enemies"] = v.enemies;
			out["handLights"] = { { "scaled", h.scaled }, { "heldSteady", h.frozen }, { "own", h.own }, { "left", h.factor[0] }, { "right", h.factor[1] } };
			out["overdriveKey"] = SettingsText::KeyName(Config().overdriveKey);
			const auto ov = OverloadNow();
			const auto mk = MarksNow();
			out["marks"] = { { "alive", mk.alive }, { "made", mk.made }, { "hurt", mk.hurt } };
			const auto fc = FrameCostNow();
			out["cost"] = { { "mainUsAvg", fc.usAvg }, { "mainUsMax", fc.usMax } };
			static constexpr const char* kTrueHud[]{ "missing", "off", "taken", "ours" };
			out["trueHud"] = kTrueHud[static_cast<int>(TrueHudNow())];
			out["overcharge"] = { { "power", ov.power }, { "arts", ov.arts }, { "projectiles", ov.projectiles }, { "handsOut", ov.out }, { "wild", ov.wild },
				{ "pull", ov.pull }, { "impacts", ov.impacts }, { "hits", ov.hits }, { "explosions", ov.explosions }, { "hazards", ov.hazards } };
			json       settings;
			const auto s = Config();
			for (const auto& k : SettingsText::kKeys) {
				settings[k.name] = k.get(s);
			}
			out["settings"] = std::move(settings);
			a_write(a_sink, Dump(out).c_str());
		}

		// one field of DevBench's arguments, as text ("" when it is missing or not a string or number)
		std::string Field(const json& a_args, const char* a_key)
		{
			if (!a_args.is_object() || !a_args.contains(a_key)) {
				return {};
			}
			const auto& v = a_args[a_key];
			if (v.is_number_float()) {
				const double d = v.get<double>();
				if (std::isfinite(d) && d == std::trunc(d) && std::abs(d) < 1e9) {
					return std::to_string(static_cast<long long>(d));
				}
			}
			return v.is_string() ? v.get<std::string>() : v.is_number() ? v.dump() :
			                                                              std::string();
		}

		void MenuNow(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			const auto args = json::parse(a_args ? a_args : "", nullptr, false);
			const auto set = Field(args, "set");
			bool       ok = true;
			float      number = 0.0f;
			const auto text = Field(args, "value");
			const bool haveNumber = !text.empty() && std::from_chars(text.data(), text.data() + text.size(), number).ec == std::errc{} && std::isfinite(number);
			if (set == "charge" && haveNumber) {
				BenchSetCharge(number);
			} else if (set == "heat" && haveNumber) {
				BenchSetHeat(number);
			} else if (set == "stage" && haveNumber && number >= 0.0f && number <= 4.0f) {
				BenchForceStage(static_cast<Heat::Stage>(static_cast<int>(number)));
			} else if (set == "overdrive") {
				BenchForceStage(Heat::Stage::kOverdrive);
			} else if (set == "key") {
				PressOverdriveKey();
			} else if (set == "hold" && haveNumber) {
				HoldOverdriveKey(number != 0.0f);
			} else if (set == "preset" && haveNumber && number >= 0.0f && number < static_cast<float>(std::size(SettingsText::kPresets))) {
				ApplyPreset(static_cast<std::size_t>(number));
			} else if (set == "reload") {
				ok = ReloadAdvanced();
			} else if (set == "setting") {
				int value = 0;
				ok = SettingsText::ReadInt(text, value) && ApplySetting(Field(args, "key"), value);
			} else {
				ok = false;
			}
			if (a_write) {
				a_write(a_sink, ok ? R"({"queued":true})" :
									 R"j({"queued":false,"error":"set is charge or heat (value), stage (value 0-4), overdrive, key, hold (value 0 or 1), preset (value 0-2), reload or setting (key, value)"})j");
			}
		}

		// DevBench calls these across the DLL boundary: nothing may be thrown back through it
		void Inspect(void* a_ctx, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				InspectNow(a_ctx, a_args, a_sink, a_write);
			} catch (...) {
				if (a_write) {
					a_write(a_sink, R"({"error":"Arcane Overdrive could not build its report"})");
				}
			}
		}

		void Menu(void* a_ctx, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				MenuNow(a_ctx, a_args, a_sink, a_write);
			} catch (...) {
				if (a_write) {
					a_write(a_sink, R"({"queued":false,"error":"Arcane Overdrive could not handle that"})");
				}
			}
		}
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench || devbench->GetBuildNumber() < kNeedsBuild) {
			return;  // DevBench is not in this load order, or too old; nothing depends on it
		}
		devbench->RegisterToolExtension("inspect", kKey, kInspect, Inspect, nullptr);
		devbench->RegisterMenuHandler(kKey, kMenu, Menu, nullptr);
		SKSE::log::info("DevBench: inspect kind={} and menu invoke name={} registered", kKey, kKey);
	}
}

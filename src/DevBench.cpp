// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order: `inspect kind=spellmasteryheat` returns the Debug page as JSON - the player's
// heat, stage, Overdrive or cooldown and its seconds left, the last school, the bonus and magicka regeneration now, the
// hand lights being scaled, the key, and the settings - and `menu action=invoke name=spellmasteryheat` sets the heat
// (set=heat value=0..100), forces a stage (set=stage value=0 Cold .. 3 Overheating, 4 Overdrive), starts Overdrive
// (set=overdrive), presses the Overdrive key (set=key), or sets any setting (set=setting key=<ini key> value=<n>).
// DevBench never saves; a later change made in the menu saves the settings as they are then.
// Nothing here runs unless DevBench asks.

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
			R"({"description":"Arcane Overdrive - the player's heat (0-100), stage, state (building, Overdrive, cooldown) and seconds left, ready for the key, last school, spell power and magicka regeneration multipliers now, effects boosted, enemies with heat, hand lights being scaled (and held steady), the Overdrive key, the HUD look (the spell's, the one drawn, sheets loaded, draw and load times), and the settings. Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})";

		constexpr const char* kMenu =
			R"({"description":"Arcane Overdrive - set=heat value=0..100 sets the player's heat; set=stage value=0..4 forces Cold, Warm, Hot, Overheating or Overdrive; set=overdrive starts Overdrive; set=key presses the Overdrive key; set=look value=-1..13 shows that HUD look whatever the spell (-1 the spell's own; 0 generic, 1 fire, 2 frost, 3 shock, 4-6 absorb health / magicka / stamina, 7 healing, 8 illusion, 9 conjuration, 10 bound, 11 alteration, 12 sun, 13 poison); set=setting key=<SpellMasteryHeat.ini key> value=<whole number> sets any setting. Not saved from here.","inputSchema":{"type":"object","properties":{"set":{"type":"string"},"key":{"type":"string"},"value":{"type":"number"}}}})";

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
			out["heat"] = v.heat;
			out["stage"] = std::string(Heat::StageName(v.stage));
			out["state"] = v.state == Heat::Meter::State::kOverdrive ? "overdrive" : v.state == Heat::Meter::State::kCooldown ? "cooldown" :
			                                                                                                                    "building";
			out["secondsLeft"] = v.timer;
			out["ready"] = v.ready;
			out["lastSchool"] = std::string(Heat::SchoolName(v.last));
			out["spellPower"] = v.bonus;
			out["magickaRegen"] = v.regen;
			out["boosted"] = v.boosted;
			out["enemies"] = v.enemies;
			out["handLights"] = { { "scaled", h.scaled }, { "heldSteady", h.frozen }, { "own", h.own }, { "left", h.factor[0] }, { "right", h.factor[1] } };
			out["overdriveKey"] = SettingsText::KeyName(Config().overdriveKey);
			const auto ls = LookStatsNow();
			const auto art = SpellArtNow();
			const auto ov = OverloadNow();
			const auto mk = MarksNow();
			out["marks"] = { { "alive", mk.alive }, { "made", mk.made }, { "hurt", mk.hurt } };
			out["look"] = { { "spell", std::string(SpellLook::Name(v.look)) },
				{ "drawn", ls.shown >= 0 ? std::string(SpellLook::Name(static_cast<SpellLook::Look>(ls.shown))) : std::string() },
				{ "forced", ls.forced >= 0 ? std::string(SpellLook::Name(static_cast<SpellLook::Look>(ls.forced))) : std::string() },
				{ "spellBody", ls.spellBody }, { "spellFill", ls.spellFill }, { "sprites", ls.sprites }, { "particles", ls.particles },
				{ "arcs", ls.arcs }, { "drawUsAvg", ls.drawUsAvg }, { "drawUsMax", ls.drawUsMax },
				{ "blend", { { "additiveMade", ls.additiveMade }, { "addCalls", ls.addCalls }, { "probeCalls", ls.probeCalls },
							   { "keptAdd", ls.keptAdd }, { "target", { { "width", ls.rtWidth }, { "height", ls.rtHeight }, { "format", ls.rtFormat }, { "isBackBuffer", ls.rtIsBack } } } } } };
			out["spellArt"] = { { "form", art.form }, { "body", art.bodyPath }, { "bodyLoaded", art.body != nullptr }, { "fill", art.fillPath },
				{ "fillLoaded", art.fill != nullptr }, { "frames", art.frames.size() }, { "colored", art.colored }, { "color", { art.r, art.g, art.b } } };
			out["overcharge"] = { { "power", ov.power }, { "arts", ov.arts }, { "projectiles", ov.projectiles }, { "handsOut", ov.out } };
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
			if (set == "heat" && haveNumber) {
				BenchSetHeat(number);
			} else if (set == "stage" && haveNumber && number >= 0.0f && number <= 4.0f) {
				BenchForceStage(static_cast<Heat::Stage>(static_cast<int>(number)));
			} else if (set == "overdrive") {
				BenchForceStage(Heat::Stage::kOverdrive);
			} else if (set == "key") {
				PressOverdriveKey();
			} else if (set == "look" && haveNumber && number >= -1.0f && number < static_cast<float>(SpellLook::kLooks)) {
				BenchForceLook(static_cast<int>(number));
			} else if (set == "setting") {
				int value = 0;
				ok = SettingsText::ReadInt(text, value) && ApplySetting(Field(args, "key"), value);
			} else {
				ok = false;
			}
			if (a_write) {
				a_write(a_sink, ok ? R"({"queued":true})" :
									 R"j({"queued":false,"error":"set is heat (value), stage (value 0-4), overdrive, key, look (value -1 the spell's own, 0-13) or setting (key, value)"})j");
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

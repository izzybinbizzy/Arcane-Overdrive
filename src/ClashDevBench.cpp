// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// DevBench, when it is in the load order (it is optional; nothing here runs without it):
//   inspect kind=arcaneclash   this session's counters and the struggle the player is in now.
//   arcaneclash.control        action = counters (the same counters as inspect, for a quick before/after) | reload (the
//                              three files read again, as the menu's button) | settings (the switches in force now) | show
//                              (a look's or a pair's design played in front of the player, to film every design).
//   menu invoke name=arcaneclash set=counters|reload|settings - the same.
//   events arcaneclash.clash {a, b, destroyedA, destroyedB, yours} for every clash, and arcaneclash.struggle {state, a, b,
//   balance, seconds} when a struggle locks, when the player's Overdrive shoves it, and when it ends (state: locked,
//   breakpoint, breakpoint on the beat, overwhelmed, gave way, draw, released, called off, gone).

#include "ClashShared.h"

#include "DevBenchGlue.h"

namespace Crossfire
{
	namespace
	{
		using DevBenchGlue::json;

		constexpr const char* kKey = "arcaneclash";

		constexpr const char* kInspect =
			R"json({"description":"Arcane Overdrive's clashes - this session's clash and struggle counters, and the struggle the player is locked in now. Read only.","inputSchema":{"type":"object","properties":{}},"readOnly":true})json";
		constexpr const char* kTool =
			R"json({"description":"Arcane Overdrive's clashes (projectiles that meet in the air clash; two held streams lock in a spell struggle): action=counters returns this session's clash and struggle counters; action=reload reads SpellMasteryHeat Clash Rules.ini, the SpellMasteryHeat Clash folder and SpellMasteryHeat Clashes.ini again (as the menu's button); action=settings returns the switches in force; action=show plays a design as a clash would - look=<Fire, Frost, Shock, Poison, Water, Blood, Sun, Astral, Shadow, Necrotic, Wind, Earth, Arcane, Absorb, Physical, Force>, optional pair=<Steam, Plasma, ShatterStorm, Eclipse, Resonance>, optional color=\"r,g,b\" (0-255, the spell's light it is tinted with), ahead=<units in front of the player, 165> and up=<units above her feet, 110>; action=bar shows the struggle bar's 6-second preview. Every clash is also sent as the event arcaneclash.clash and every struggle start, end and Overdrive Breakpoint shove as arcaneclash.struggle.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["counters","reload","settings","show","bar"]},"look":{"type":"string"},"pair":{"type":"string"},"color":{"type":"string"},"ahead":{"type":"number"},"up":{"type":"number"}},"required":["action"]}})json";
		constexpr const char* kMenu =
			R"json({"description":"Arcane Overdrive's clashes - set=counters | reload | settings, as the arcaneclash.control tool.","inputSchema":{"type":"object","properties":{"set":{"type":"string"}}}})json";

		json CountersNow()
		{
			auto&      s = Counters();
			const auto v = StruggleNow();
			return json{ { "tracked", s.tracked.load() }, { "clashes", s.clashes.load() }, { "destroyed", s.destroyed.load() },
				{ "weakened", s.weakened.load() }, { "bursts", s.explosions.load() }, { "models", s.models.load() }, { "yours", s.yours.load() }, { "struggles", s.struggles.load() },
				{ "overwhelms", s.overwhelms.load() }, { "won", s.won.load() }, { "lost", s.lost.load() }, { "gaveWay", s.gaveWay.load() },
				{ "draws", s.draws.load() }, { "cut", s.cut.load() }, { "autoLocked", s.locked.load() }, { "steered", s.steered.load() }, { "pairs", s.pairs.load() }, { "tinted", s.tinted.load() }, { "locked", v.active }, { "foe", std::string(v.foe) }, { "balance", v.balance },
				{ "seconds", v.seconds }, { "mySkill", v.mySkill }, { "theirSkill", v.theirSkill } };
		}

		json Act(const json& a_args)
		{
			auto action = DevBenchGlue::Text(a_args, "action");
			if (action.empty() || action == "invoke") {  // `menu invoke` hands over its own args: action=invoke, the verb in set
				action = DevBenchGlue::Text(a_args, "set");
			}
			if (action == "counters") {
				auto out = CountersNow();
				out["ok"] = true;
				return out;
			}
			if (action == "reload") {
				ReloadSoon();
				return json{ { "ok", true }, { "note", "the files are read again on the main thread; read action=settings back" } };
			}
			if (action == "settings") {
				const auto  c = MenuCopy();
				const auto& s = c.struggle;
				json        all = json::object();
				for (const auto& [key, value] : Core::SettingValues(c)) {
					all[std::string(key)] = value;
				}
				return json{ { "ok", true }, { "all", std::move(all) }, { "enabled", c.enabled }, { "who", c.who }, { "ignoreAllies", c.ignoreAllies },
					{ "radiusBonus", c.radiusBonus }, { "overpowerRatio", c.overpowerRatio }, { "weakenSurvivor", c.weakenSurvivor },
					{ "boltsMeet", c.boltsMeet }, { "explosions", c.explosions }, { "safeExplosionsOnly", c.safeExplosionsOnly },
					{ "standInBursts", c.standInBursts }, { "modEvents", c.modEvents }, { "log", c.debugLog }, { "struggles", s.enabled },
					{ "sprays", s.sprays }, { "beams", s.beams }, { "breath", s.breath }, { "opposites", s.opposites },
					{ "betweenOthers", s.betweenOthers }, { "skillAlwaysWins", s.skillAlwaysWins }, { "surge", s.surge },
					{ "breakpoint", s.breakpoint }, { "struggleBar", s.bar },
					{ "breakCast", s.breakCast }, { "stagger", s.stagger }, { "messages", s.messages }, { "lockYours", c.lockYours },
					{ "lockOthers", c.lockOthers }, { "steer", c.steer }, { "lockArrows", c.lockArrows }, { "lockCone", c.lockCone },
					{ "steerRange", c.steerRange }, { "steerTurn", c.steerTurn }, { "headOnCatch", c.headOnCatch }, { "headOnCone", c.headOnCone },
					{ "clashLight", c.clashLight }, { "particleLights", c.particleLights }, { "lit", LightsLit() },
					{ "notes", LoadNotes() } };
			}
			if (action == "show") {
				std::optional<Core::Rgb> color;
				if (const auto text = DevBenchGlue::Text(a_args, "color"); !text.empty()) {
					std::array<int, 3> c{ -1, -1, -1 };
					const char*        at = text.data();
					const char*        end = at + text.size();
					bool               good = true;
					for (std::size_t i = 0; i < c.size() && good; ++i) {
						const auto [next, err] = std::from_chars(at, end, c[i]);
						good = err == std::errc{} && (i + 1 < c.size() ? next < end && *next == ',' : next == end);
						at = next + (i + 1 < c.size() ? 1 : 0);
					}
					if (!good || std::ranges::any_of(c, [](int v) { return v < 0 || v > 255; })) {
						return DevBenchGlue::Refusal("color is three whole numbers 0-255: \"r,g,b\"");
					}
					color = Core::Rgb{ static_cast<std::uint8_t>(c[0]), static_cast<std::uint8_t>(c[1]), static_cast<std::uint8_t>(c[2]) };
				}
				const auto look = DevBenchGlue::Text(a_args, "look");
				const auto pair = DevBenchGlue::Text(a_args, "pair");
				const auto ahead = static_cast<float>(DevBenchGlue::Number(a_args, "ahead").value_or(165.0));
				const auto up = static_cast<float>(DevBenchGlue::Number(a_args, "up").value_or(110.0));
				if (!ShowDesign(look, pair, color, ahead, up)) {
					return DevBenchGlue::Refusal("no such look '" + look + "' or pair '" + pair + "'");
				}
				return json{ { "ok", true }, { "look", look }, { "pair", pair }, { "note", "played on the main thread; models / tinted / pairs count it" } };
			}
			if (action == "bar") {
				RequestBarPreview();
				return json{ { "ok", true }, { "note", "the struggle bar's preview shows for 6 s (a lock, your Overdrive filling, released on the pulse)" } };
			}
			return DevBenchGlue::Refusal("unknown action '" + action + "'", json::array({ "counters", "reload", "settings", "show", "bar" }));
		}

		void Inspect(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, CountersNow());
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("Arcane Overdrive's clashes could not build their report"));
			}
		}

		void Control(void*, const char* a_args, void* a_sink, DevBenchAPI::WriteFn a_write) noexcept
		{
			try {
				DevBenchGlue::Reply(a_sink, a_write, Act(DevBenchGlue::Args(a_args)));
			} catch (...) {
				DevBenchGlue::Reply(a_sink, a_write, DevBenchGlue::Refusal("Arcane Overdrive's clashes could not handle that"));
			}
		}
	}

	void ClashEvent(std::string_view a_elementA, std::string_view a_elementB, bool a_destroyedA, bool a_destroyedB, bool a_yours)
	{
		if (!DevBenchAPI::GetDevBenchInterface001()) {
			return;  // no DevBench: no event text built every clash
		}
		DevBenchGlue::Emit("arcaneclash.clash", json{ { "a", a_elementA }, { "b", a_elementB }, { "destroyedA", a_destroyedA },
													{ "destroyedB", a_destroyedB }, { "yours", a_yours } });
	}

	void StruggleEvent(std::string_view a_state, std::string_view a_nameA, std::string_view a_nameB, float a_balance, float a_seconds)
	{
		if (!DevBenchAPI::GetDevBenchInterface001()) {
			return;
		}
		DevBenchGlue::Emit("arcaneclash.struggle",
			json{ { "state", a_state }, { "a", a_nameA }, { "b", a_nameB }, { "balance", a_balance }, { "seconds", a_seconds } });
	}

	void OfferToDevBench()
	{
		auto* devbench = DevBenchAPI::GetDevBenchInterface001();
		if (!devbench || devbench->GetBuildNumber() < DevBenchGlue::kNeedsBuild) {
			return;  // DevBench is not in this load order, or too old; nothing depends on it
		}
		devbench->RegisterToolExtension("inspect", kKey, kInspect, Inspect, nullptr);
		devbench->RegisterTool("arcaneclash.control", kTool, Control, nullptr);
		devbench->RegisterMenuHandler(kKey, kMenu, Control, nullptr);
		SKSE::log::info("DevBench: inspect kind={}, the tool arcaneclash.control and menu invoke name={} registered", kKey, kKey);
	}
}

// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Two pages in SKSE Menu Framework's Mod Control Panel, under their own section - Settings (every switch and the preset,
// saved at once and live on the next frame; every number is in the advanced file) and Debug (both bars, the gate, the last
// school, and buttons to set the bars or force a stage). The bars on screen are TrueHUD's and Reticle Arcs' (TrueHud.cpp,
// Arcs.cpp). The look is the shared MenuStyle.h, in the meter's forge orange.

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
#include "MenuStyle.h"

namespace Plugin
{
	namespace
	{
		constexpr ImGuiMCP::ImVec4 kGold{ 1.0f, 0.86f, 0.55f, 1.0f };
		constexpr ImGuiMCP::ImVec4 kEmber{ 0.98f, 0.62f, 0.32f, 0.9f };

		class HeatStyle
		{
		public:
			HeatStyle()
			{
				using namespace ImGuiMCP;
				PushStyleColor(ImGuiCol_FrameBg, ImVec4{ 0.13f, 0.09f, 0.07f, 0.75f });
				PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4{ 0.32f, 0.17f, 0.08f, 0.75f });
				PushStyleColor(ImGuiCol_Separator, ImVec4{ 1.0f, 0.62f, 0.32f, 0.22f });
			}
			~HeatStyle()
			{
				ImGuiMCP::PopStyleColor(3);
			}
			HeatStyle(const HeatStyle&) = delete;
			HeatStyle& operator=(const HeatStyle&) = delete;

		private:
			MenuStyle::Page page;  // the shared accent, rounding and hovers
		};
		namespace Icon = MenuStyle::Icon;

		void Heading(unsigned a_icon, const char* a_text)
		{
			using namespace ImGuiMCP;
			Spacing();
			auto*        dl = GetWindowDrawList();
			const ImVec2 at = GetCursorScreenPos();
			const float  w = GetContentRegionAvail().x;
			const float  h = GetTextLineHeight() + 6.0f;
			ImDrawListManager::AddRectFilledMultiColor(dl, at, ImVec2{ at.x + w, at.y + h }, IM_COL32(255, 140, 60, 46),
				IM_COL32(255, 140, 60, 0), IM_COL32(255, 140, 60, 0), IM_COL32(255, 140, 60, 46));
			ImDrawListManager::AddLine(dl, ImVec2{ at.x, at.y + h }, ImVec2{ at.x + w * 0.55f, at.y + h }, IM_COL32(255, 170, 90, 110), 1.0f);
			Dummy(ImVec2{ 0.0f, 3.0f });
			Text(" ");
			SameLine(0.0f, 0.0f);
			FontAwesome::PushSolid();
			TextColored(kGold, "%s", FontAwesome::UnicodeToUtf8(a_icon).c_str());
			FontAwesome::Pop();
			SameLine();
			TextColored(kGold, "%s", a_text);
			Dummy(ImVec2{ 0.0f, 4.0f });
		}

		void Tip(const char* a_text) { ImGuiMCP::SetItemTooltip("%s", a_text); }

		void Toggle(const char* a_label, bool& a_value, const char* a_tip, bool& a_save)
		{
			a_save |= ImGuiMCP::Checkbox(a_label, &a_value);
			Tip(a_tip);
		}

		template <class E>
		void Choice(const char* a_label, E& a_value, const char* const* a_items, int a_count, const char* a_tip, bool& a_save)
		{
			int v = static_cast<int>(a_value);
			if (ImGuiMCP::Combo(a_label, &v, a_items, a_count)) {
				a_value = static_cast<E>(std::clamp(v, 0, a_count - 1));
				a_save = true;
			}
			Tip(a_tip);
		}

		void __stdcall RenderSettings()
		{
			const HeatStyle style;
			const Settings  before = Config();
			Settings        s = before;
			auto&           t = s.tuning;
			bool            save = false;
			const float     column = ImGuiMCP::GetContentRegionAvail().x * 0.5f;
			ImGuiMCP::PushItemWidth(column * 0.9f);

			Toggle("Enabled", s.enabled, "Off: no bar builds, no spell is boosted, and the hand lights are put back as the other mods set them.", save);
			ImGuiMCP::BeginDisabled(!s.enabled);

			// the presets: picking one sets its values in both files; any value changed after it reads Custom
			Heading(Icon::kStar, "Difficulty");
			{
				const auto               now = SettingsText::PresetOf(s);
				static const char* const kNames[] = { "Milk Drinker (easy)", "Dragonborn (the default)", "Aedra (hard)", "Custom" };
				int                      pick = now ? static_cast<int>(*now) : 3;
				if (ImGuiMCP::Combo("Preset", &pick, kNames, 4) && pick < 3) {
					SettingsText::ApplyPreset(s, static_cast<std::size_t>(pick));
					save = true;
				}
				Tip("Milk Drinker: Overdrive goes whenever your Heat is below the red line, never misfires, a longer burst and a short "
					"burnout with no penalty. Dragonborn: Overdrive only in the sweet spot, a misfire when too hot. Aedra: a narrow sweet spot, "
					"a misfire staggers you and bites your magicka, a long burnout with weaker spells, and enemy mages build heat too. "
					"The damage bonus is the same in all three. Custom: you changed a value here or in the advanced file.");
			}

			Heading(Icon::kBolt, "Overdrive");
			{
				static const char* const kModes[] = { "Automatic", "Key press", "Off" };
				Choice("Overdrive", t.mode, kModes, 3,
					"Automatic: it goes by itself when the Overdrive bar is full and your Heat lets it. Key press: the full bar waits for your "
					"key. Off: no Overdrive.",
					save);
			}
			ImGuiMCP::BeginDisabled(t.mode != Heat::OverdriveMode::kKey);
			{
				const std::string name = CapturingKey() ? "press a key... (Esc cancels)" : SettingsText::KeyName(s.overdriveKey);
				ImGuiMCP::Text("Overdrive key: %s", name.c_str());
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Set the key")) {
					CaptureKey(true);
				}
				Tip("Click, then tap the key you want. Any key, mouse button or gamepad button.");
				// the fallback, should the menu keep the tapped key from reaching the game: a short list
				static const int         kCodes[] = { 34, 47, 19, 33, 35, 44, 45, 46, 48, 58, 256 + 2, 256 + 3 };
				static const char* const kNames[] = { "G", "V", "R", "F", "H", "Z", "X", "C", "B", "Caps Lock", "Mouse button 3", "Mouse button 4" };
				int                      pick = -1;
				for (int i = 0; i < 12; ++i) {
					pick = kCodes[i] == s.overdriveKey ? i : pick;
				}
				ImGuiMCP::SetNextItemWidth(column * 0.5f);
				if (ImGuiMCP::Combo("or pick one", &pick, kNames, 12) && pick >= 0) {
					s.overdriveKey = kCodes[pick];
					save = true;
				}
				Tip("If tapping a key does not set it, pick one from this list instead.");
			}
			ImGuiMCP::EndDisabled();
			ImGuiMCP::BeginDisabled(t.mode == Heat::OverdriveMode::kOff);
			Toggle("Heat decides when it goes", t.heatGate,
				"On: a full Overdrive bar goes only when your Heat lets it (the preset decides how: the sweet spot, or below the red line). "
				"Off: it goes as soon as the bar is full.",
				save);
			ImGuiMCP::BeginDisabled(!t.heatGate);
			Toggle("Misfires", t.misfire, "Releasing Overdrive past the red line misfires: your spells sputter and your Heat drops a stage.", save);
			ImGuiMCP::SameLine(column);
			ImGuiMCP::BeginDisabled(!t.misfire);
			Toggle("Backlash", t.backlash, "A misfire also staggers you and takes a bite out of your magicka.", save);
			ImGuiMCP::EndDisabled();
			ImGuiMCP::EndDisabled();
			Toggle("Burnout: slow magicka", t.slowMagicka, "During the burnout after Overdrive your magicka comes back slower.", save);
			ImGuiMCP::SameLine(column);
			Toggle("Burnout: weaker spells", t.weaker, "During the burnout your spells are weaker.", save);
			ImGuiMCP::EndDisabled();
			{
				static const char* const kBars[] = { "One shared Overdrive bar", "One per school" };
				int                      v = t.perSchool ? 1 : 0;
				if (ImGuiMCP::Combo("Overdrive bar", &v, kBars, 2)) {
					t.perSchool = v == 1;
					save = true;
				}
				Tip("One shared bar: every school fills the same Overdrive bar. One per school: each school fills its own. Heat is always shared.");
			}

			Heading(Icon::kTemp, "Bonus");
			Toggle("Damage bonus", t.bonusOn, "Your spells grow stronger as your Heat climbs (Warm, Hot, Overheating) and strongest in Overdrive.", save);
			ImGuiMCP::BeginDisabled(!t.bonusOn);
			{
				static const char* const kBoost[] = { "Damage only", "All harmful spell effects" };
				Choice("What is boosted", s.boost, kBoost, 2,
					"Damage only: spells that hurt. All harmful spell effects: every harmful effect's strength (fear, paralysis, "
					"slows and so on).",
					save);
			}
			ImGuiMCP::EndDisabled();
			Toggle("Healing and ward spells count", s.counts.healingAndWards, "Off by default: healing spells and wards fill no bar.", save);
			ImGuiMCP::SameLine(column);
			Toggle("Staves and scrolls count", s.counts.stavesAndScrolls, "Staves and scrolls fill the bars and get the bonus.", save);
			Toggle("Enemies build heat", s.enemies,
				"Off by default. On: enemies' spells fill bars of their own and get the same bonus - a harder game. With TrueHUD their Overdrive "
				"bar shows on their floating health bars.",
				save);

			Heading(Icon::kDisplay, "Display");
			Toggle("Light growth", s.lightGrowth,
				"The lights on your casting hands (from Illuminated or another spell light mod) grow brighter and wider with your Heat, "
				"dip when the burnout starts and come back as it ends.",
				save);
			ImGuiMCP::SameLine(column);
			Toggle("Hand glow", s.handGlow,
				"A soft light of this mod's own on a casting hand when it is Warm or hotter - only on a hand no other mod lights.", save);
			Toggle("Spells grow", s.spellGrowth,
				"The spell in your hands and the spells you cast grow and burn fiercer as the Overdrive bar fills: normal when it is "
				"empty, up to double size when it is full, and that big through Overdrive.",
				save);
			ImGuiMCP::SameLine(column);
			Toggle("Spells flicker out in burnout", s.burnoutFlicker,
				"During the burnout after Overdrive the spell in your hands and your spells in the air sputter out and back, steadier "
				"as it ends. The hand lights dip with them.",
				save);
			ImGuiMCP::BeginDisabled(!s.spellGrowth);
			Toggle("Impacts grow", s.impactsGrow,
				"Your explosions, your hazards (walls, runes, clouds) and the fire or frost your spells leave on a target grow with your "
				"spells, keeping the size they were made at. Big explosions cost frames.",
				save);
			ImGuiMCP::SameLine(column);
			Toggle("Overdrive focus", s.overdriveFocus, "As Overdrive fires your spells draw in and swell back, then burn steady.", save);
			ImGuiMCP::EndDisabled();
			Toggle("Wild in the red", s.redZoneWild,
				"While your Heat is in the red outside Overdrive, your spells pulse, stutter and blink out, and the hand lights and the "
				"bar stutter with them. Releasing Overdrive steadies them.",
				save);
			Toggle("Scorch and frost marks", s.marks,
				"From Overheating on, your fire and frost spells leave a burning or frozen patch where they land for a few seconds.", save);
			ImGuiMCP::SameLine(column);
			ImGuiMCP::BeginDisabled(!s.marks);
			Toggle("Marks hurt", s.markHurt,
				"The patches hurt anyone who stands in them - never you, your followers or your summons. Fire and frost resistance "
				"protect. Stronger in Overdrive.",
				save);
			ImGuiMCP::EndDisabled();
			const bool trueHud = TrueHudNow() != TrueHudState::kMissing;
			ImGuiMCP::BeginDisabled(!trueHud);
			Toggle("Bar in TrueHUD", s.bar,
				trueHud ? "The Overdrive bar as TrueHUD's extra bar under your health, magicka and stamina, colored by your Heat (amber, "
						  "orange, red in the red zone, white-hot in Overdrive) and flashing when Overdrive is ready or misfires. TrueHUD's "
						  "own menu sets when it shows." :
						  "TrueHUD draws this bar, and it is not installed.",
				save);
			ImGuiMCP::EndDisabled();
			ImGuiMCP::SameLine(column);
			ImGuiMCP::BeginDisabled(!ArcBarTaken());
			Toggle("Bars in Reticle Arcs", s.arcHeat,
				ArcBarTaken()   ? "The Overdrive and Heat bars as arcs round the crosshair, in Reticle Arcs' own look (set where they sit in its menu)." :
				ArcsInstalled() ? "Reticle Arcs is installed but did not take these bars (too old for add-on bars - its log says why)." :
								  "Reticle Arcs draws these bars, and it is not installed.",
				save);
			ImGuiMCP::EndDisabled();

			Heading(Icon::kWand, "Every number");
			ImGuiMCP::TextWrapped(
				"How fast the bars fill and cool, the sweet spot's edges, the timings, the bonuses, the marks and the rest "
				"are in Data\\SKSE\\Plugins\\SpellMasteryHeat Advanced.ini, each line explained.");
			if (ImGuiMCP::Button("Read the advanced file again")) {
				ReloadAdvanced();
			}
			Tip("After editing the advanced file while the game runs.");

			ImGuiMCP::EndDisabled();
			ImGuiMCP::PopItemWidth();

			// only what changed on this page goes back, key by key: a change DevBench or the key capture made meanwhile is kept
			for (const auto& k : SettingsText::kKeys) {
				if (k.get(s) != k.get(before)) {
					ApplySetting(k.name, k.get(s));
				}
			}
			if (save) {
				SaveSettings();
			}
		}

		void __stdcall RenderDebug()
		{
			const HeatStyle style;
			{
				Settings s = Config();
				bool     save = false;
				Toggle("Debug log", s.debugLog, "Writes each of your casts and the heat after it to SpellMasteryHeat.log.", save);
				if (save) {
					ApplySetting("DebugLog", s.debugLog ? 1 : 0);
					SaveSettings();
				}
			}
			const auto v = PlayerView();
			Heading(Icon::kGauge, "Now");
			ImGuiMCP::TextColored(kEmber, "Overdrive bar %.0f", v.charge);
			ImGuiMCP::ProgressBar(std::clamp(v.charge / Heat::kFull, 0.0f, 1.0f), ImGuiMCP::ImVec2{ -1.0f, 0.0f }, "");
			ImGuiMCP::TextColored(kEmber, "Heat %.0f - %s%s (sweet spot %.0f to %.0f)", v.heat, std::string(Heat::StageName(v.stage)).c_str(),
				v.redZone ? ", RED ZONE" : "", v.bandLow, v.redLine);
			ImGuiMCP::ProgressBar(std::clamp(v.heat / Heat::kFull, 0.0f, 1.0f), ImGuiMCP::ImVec2{ -1.0f, 0.0f }, "");
			static constexpr const char* kGate[]{ "open", "too cold", "too hot", "hold to focus" };
			if (v.state == Heat::Meter::State::kOverdrive) {
				ImGuiMCP::Text("Overdrive: %.1f s left", v.timer);
			} else if (v.state == Heat::Meter::State::kCooldown) {
				ImGuiMCP::Text("burnout: %.1f s left", v.timer);
			} else if (v.ready) {
				ImGuiMCP::TextColored(kGold, "full - the gate: %s", kGate[static_cast<int>(v.gate)]);
			}
			ImGuiMCP::Text("misfires: %d   focus held: %.0f%%   preset: %s", v.misfires, v.focus * 100.0f,
				[] {
					const auto p = SettingsText::PresetOf(Config());
					return p ? SettingsText::kPresets[*p].name : "Custom";
				}());
			ImGuiMCP::Text("last school: %s", std::string(Heat::SchoolName(v.last)).c_str());
			ImGuiMCP::Text("spell power x%.2f   magicka regeneration x%.2f", v.bonus, v.regen);
			ImGuiMCP::TextDisabled("effects boosted so far: %llu   enemies with heat: %zu", static_cast<unsigned long long>(v.boosted), v.enemies);
			const auto h = HandsNow();
			ImGuiMCP::Text("hand lights: %zu being scaled (x%.2f left, x%.2f right), %zu of this mod's own", h.scaled, h.factor[0], h.factor[1], h.own);
			if (h.frozen) {
				ImGuiMCP::TextColored(kGold, "%zu hand light(s) held steady: another mod scales them the same way", h.frozen);
			}

			Heading(Icon::kDisplay, "Spells and bars");
			{
				const auto                   ov = OverloadNow();
				static constexpr const char* kTrueHud[]{ "not installed", "switched off", "held by another mod", "the Overdrive bar" };
				ImGuiMCP::Text("TrueHUD's special bar: %s   Reticle Arcs: %s", kTrueHud[static_cast<int>(TrueHudNow())], ArcBarTaken() ? "took the bars" : ArcsInstalled() ? "installed, refused the bars" :
																																											 "not installed");
				// in milliseconds, the unit a frame is measured in (a 60 fps frame is 16.7 ms)
				const auto fc = FrameCostNow();
				ImGuiMCP::TextDisabled("this mod a frame: %.3f ms (most %.3f)", fc.usAvg / 1000.0f, fc.usMax / 1000.0f);
				ImGuiMCP::Text("spells now x%.2f size; grown: %zu casting art(s), %zu projectile(s); sputtered out: %zu hand(s)", ov.power, ov.arts, ov.projectiles, ov.out);
				ImGuiMCP::Text("red zone x%.2f wild, focus pull x%.2f; now: %zu explosion(s) / hazard(s) grown, %zu hit effect(s) sped; since loading: %u explosions, %u hazards",
					ov.wild, ov.pull, ov.impacts, ov.hits, ov.explosions, ov.hazards);
			}

			Heading(Icon::kWand, "Try it");
			static int charge = 100, heat = 60;
			ImGuiMCP::SetNextItemWidth(200.0f);
			ImGuiMCP::SliderInt("##charge", &charge, 0, 100, "Overdrive bar %d");
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Set the Overdrive bar")) {
				BenchSetCharge(static_cast<float>(charge));
			}
			ImGuiMCP::SetNextItemWidth(200.0f);
			ImGuiMCP::SliderInt("##heat", &heat, 0, 100, "Heat %d");
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Set the Heat")) {
				BenchSetHeat(static_cast<float>(heat));
			}
			Tip("Sets your bars now, to see them, the gate, the bonus and the hand lights. The Heat cools again at once.");
			for (int st = 0; st <= 4; ++st) {
				if (st > 0) {
					ImGuiMCP::SameLine();
				}
				if (ImGuiMCP::Button(std::string(Heat::StageName(static_cast<Heat::Stage>(st))).c_str())) {
					BenchForceStage(static_cast<Heat::Stage>(st));
				}
			}
			Tip("Puts your Heat at the start of this stage now (Overdrive starts it, gate or not).");
		}
	}

	void RegisterMenu()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::warn("SKSE Menu Framework is not installed, so there are no settings pages; the settings files still apply");
			return;
		}
		MenuStyle::gTheme = MenuStyle::MakeTheme(0xFF8C3A);  // forge orange
		SKSEMenuFramework::SetSection("Arcane Overdrive");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddSectionItem("Debug", RenderDebug);
		SKSE::log::info("settings pages added to SKSE Menu Framework {}", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}

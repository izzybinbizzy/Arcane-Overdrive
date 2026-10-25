// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Two pages in SKSE Menu Framework's Mod Control Panel, under their own section - Settings (every switch and the preset,
// saved at once and live on the next frame; every number is in the advanced file) and Debug (both bars, the gate, the last
// school, and buttons to set the bars or force a stage) - and the HUD element that draws the bars (HudBars.cpp).
// The look is the shared MenuStyle.h, in the meter's forge orange.

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
#include "HudBars.h"

#include <set>

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

		void Percent(const char* a_label, float& a_value, int a_lo, int a_hi, const char* a_tip, bool& a_save)
		{
			int v = static_cast<int>(std::lround(a_value * 100.0f));
			if (ImGuiMCP::SliderInt(a_label, &v, a_lo, a_hi, "%d%%")) {
				a_value = static_cast<float>(std::clamp(v, a_lo, a_hi)) / 100.0f;
			}
			a_save |= ImGuiMCP::IsItemDeactivatedAfterEdit();
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
				"Off by default. On: enemies' spells fill bars of their own and get the same bonus - a harder game. They have no bar on screen.", save);

			Heading(Icon::kDisplay, "Display");
			Toggle("Light growth", s.lightGrowth,
				"The lights on your casting hands (from Illuminated or another spell light mod) grow brighter and wider with your Heat, "
				"dip when the burnout starts and come back as it ends.",
				save);
			ImGuiMCP::SameLine(column);
			Toggle("Hand glow", s.handGlow,
				"A soft light of this mod's own on a casting hand when it is Warm or hotter - only on a hand no other mod lights.", save);
			Toggle("Spells grow", s.spellGrowth,
				"The spell in your hands and the spells you cast grow as the Overdrive bar fills, then in Overdrive up to double size and "
				"twice as fierce at its start, falling back as Overdrive runs out.",
				save);
			ImGuiMCP::SameLine(column);
			Toggle("Spells flicker out in burnout", s.burnoutFlicker,
				"During the burnout after Overdrive the spell in your hands and your spells in the air sputter out and back, steadier "
				"as it ends. The hand lights dip with them.",
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
			ImGuiMCP::BeginDisabled(!ArcBarTaken());
			Toggle("Bars in Reticle Arcs", s.arcHeat,
				ArcBarTaken() ? "The Overdrive and Heat bars as Reticle Arcs bars round the crosshair, in Reticle Arcs' own look (set where "
								"they sit in its menu). While they show, the bars below step aside." :
								"Reticle Arcs draws these bars, and it is not installed.",
				save);
			ImGuiMCP::EndDisabled();
			Toggle("Bars under the magicka bar", s.bar, "The Overdrive and Heat bars on the screen. They show only while there is something to show.", save);
			ImGuiMCP::BeginDisabled(!s.bar);
			Percent("Bar across", s.barX, 0, 100, "Where the bar's middle is, from the left edge of the screen.", save);
			Percent("Bar down", s.barY, 0, 100, "Where the bar is, from the top of the screen.", save);
			Percent("Bar size", s.barWidth, 50, 300, "How long and thick the bar is.", save);
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

			Heading(Icon::kDisplay, "The bars");
			{
				const auto                   ls = LookStatsNow();
				const auto                   art = SpellArtNow();
				const auto                   ov = OverloadNow();
				const std::string            spell(SpellLook::Name(v.look));
				const std::string            shown = ls.shown >= 0 ? std::string(SpellLook::Name(static_cast<SpellLook::Look>(ls.shown))) : "none yet";
				static constexpr const char* kShader[]{ "not asked yet", "compiling", "drawing", "FAILED - a plain bar stands in" };
				ImGuiMCP::Text("the spell's look: %s   drawn: %s   colors: %s", spell.c_str(), shown.c_str(), art.colored ? "the spell's own" : "the look's");
				ImGuiMCP::TextDisabled("the shader: %s (compiled in %.0f ms, %u draws)", kShader[std::clamp(ls.shaderState, 0, 3)], ls.shaderMs, ls.shaderCalls);
				// in milliseconds, the unit a frame is measured in (a 60 fps frame is 16.7 ms)
				const auto fc = FrameCostNow();
				ImGuiMCP::TextDisabled("this mod a frame: game thread %.3f ms (most %.3f), the bars %.3f ms (most %.3f), the bars on the GPU %s", fc.usAvg / 1000.0f,
					fc.usMax / 1000.0f, ls.drawUsAvg / 1000.0f, ls.drawUsMax / 1000.0f, ls.gpuUs >= 0.0f ? std::format("{:.4f} ms", ls.gpuUs / 1000.0f).c_str() : "(not timed)");
				bool clock = ls.gpuClock;
				if (ImGuiMCP::Checkbox("Time the bars on the GPU", &clock)) {
					BenchGpuClock(clock);
				}
				Tip("Reads the GPU's own clock round the two bars, and starts every reading above afresh. Not saved.");
				ImGuiMCP::Text("spells now x%.2f size; grown: %zu casting art(s), %zu projectile(s); sputtered out: %zu hand(s)", ov.power, ov.arts, ov.projectiles, ov.out);
				static const char* const kShow[] = { "the spell's own", "generic", "fire", "frost", "shock", "absorb health", "absorb magicka",
					"absorb stamina", "healing", "illusion", "conjuration", "bound weapon", "alteration", "sun", "poison" };
				static_assert(std::size(kShow) == SpellLook::kLooks + 1);
				int pick = ls.forced + 1;
				ImGuiMCP::SetNextItemWidth(220.0f);
				if (ImGuiMCP::Combo("Show this look", &pick, kShow, static_cast<int>(std::size(kShow)))) {
					BenchForceLook(pick - 1);
				}
				Tip("To see every look without casting each kind of spell: set the bars below and pick a look (in the look's own colors). Not saved.");
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

		// a game menu is up: the bar steps aside as the game's own HUD does. Kept by a menu open / close watcher - the per-frame
		// pass cannot do it, since the game stops updating the player while a menu pauses it (chain7 2026-10-11: the bar stayed
		// over the open console). Illuminated's MenuWatch, the same list.
		class MenuWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static MenuWatch& Get()
			{
				static MenuWatch watch;
				return watch;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string_view name = a_event->menuName.c_str();
				if (std::ranges::find(kHiding, name) != std::end(kHiding)) {
					std::lock_guard lock(gate);
					if (a_event->opening) {
						open.insert(std::string(name));
					} else {
						open.erase(std::string(name));
					}
					hide.store(!open.empty(), std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
			std::atomic<bool> hide{ false };

		private:
			static constexpr std::string_view kHiding[]{ "BarterMenu", "Book Menu", "Console", "Console Native UI Menu", "ContainerMenu",
				"Crafting Menu", "Creation Club Menu", "Creations Menu", "Dialogue Menu", "FavoritesMenu", "GiftMenu", "InventoryMenu",
				"Journal Menu", "Kinect Menu", "LevelUp Menu", "Loading Menu", "Lockpicking Menu", "MagicMenu", "Main Menu", "MapMenu",
				"MessageBoxMenu", "Mist Menu", "Mod Manager Menu", "Quantity Menu", "RaceSex Menu", "Sleep/Wait Menu", "StatsMenu",
				"Training Menu", "Tutorial Menu", "TweenMenu" };
			std::mutex                        gate;
			std::set<std::string>             open;
		};

		bool HideHud()
		{
			if (MenuWatch::Get().hide.load(std::memory_order_relaxed)) {
				return true;
			}
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->GameIsPaused();
		}

		void __stdcall DrawBar()
		{
			using namespace ImGuiMCP;
			static float alpha = 0.0f;
			const auto   s = Config();
			const auto   v = PlayerView();
			auto*        io = HudIO();
			if (!io) {
				return;
			}
			// Reticle Arcs shows the bars instead while it took them and its switch is on (Arcs.cpp)
			const bool  show = s.enabled && s.bar && !(s.arcHeat && ArcBarTaken()) && !HideHud() &&
			                   (v.charge > 0.5f || v.heat > 0.5f || v.state != Heat::Meter::State::kBuilding);
			const float dt = std::isfinite(io->DeltaTime) ? std::clamp(io->DeltaTime, 0.0f, 0.25f) : 0.0f;
			alpha = std::clamp(alpha + (show ? dt : -dt) * 3.0f, 0.0f, 1.0f);
			if (alpha <= 0.0f) {
				return;
			}
			auto* dl = HudList();
			if (dl) {
				DrawBars(dl, s, v, alpha, dt);
			}
		}
	}

	void RegisterMenu()
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&MenuWatch::Get());  // the bar hides over the game's menus
		}
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::warn("SKSE Menu Framework is not installed, so there is no settings page and no bar; the settings file still applies");
			return;
		}
		MenuStyle::gTheme = MenuStyle::MakeTheme(0xFF8C3A);  // forge orange
		SKSEMenuFramework::SetSection("Arcane Overdrive");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddSectionItem("Debug", RenderDebug);
		SKSEMenuFramework::AddHudElement(DrawBar);
		SKSE::log::info("settings pages and the bar added to SKSE Menu Framework {}", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}

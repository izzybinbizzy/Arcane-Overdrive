// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Two pages in SKSE Menu Framework's Mod Control Panel, under their own section - Settings (every setting, saved at once
// and live on the next frame) and Debug (the heat, the stage, the last school, and buttons to set the heat or force a
// stage) - and the bar under the magicka bar, drawn on the HUD the way Crossfire draws its struggle bar.
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
#include "FlameFx.h"
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

		void Whole(const char* a_label, float& a_value, int a_lo, int a_hi, const char* a_format, const char* a_tip, bool& a_save)
		{
			int v = static_cast<int>(std::lround(a_value));
			if (ImGuiMCP::SliderInt(a_label, &v, a_lo, a_hi, a_format)) {
				a_value = static_cast<float>(std::clamp(v, a_lo, a_hi));
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

			Toggle("Enabled", s.enabled, "Off: no heat builds, no spell is boosted, and the hand lights are put back as the other mods set them.", save);
			ImGuiMCP::BeginDisabled(!s.enabled);

			Heading(Icon::kTemp, "Heat");
			Percent("Heat gain speed", t.gain, 25, 300, "How fast casting builds heat. 100% is the normal speed.", save);
			Whole("Drain speed", t.drain, 1, 50, "%d a second", "How fast heat falls once the grace period is over. The meter runs 0 to 100.", save);
			Whole("Grace period", t.grace, 0, 10, "%d s", "How long after your last cast before heat starts to drain.", save);
			{
				static const char* const kMeters[] = { "One shared meter", "One per school" };
				int                      v = t.perSchool ? 1 : 0;
				if (ImGuiMCP::Combo("Meter", &v, kMeters, 2)) {
					t.perSchool = v == 1;
					save = true;
				}
				Tip("One shared meter: every school builds the same heat. One per school: each school builds its own, and a "
					"spell gets the bonus of its own school's heat.");
			}
			Percent("Same school bonus", t.sameSchool, 0, 200, "How much faster heat builds when you cast the same school as your last spell.", save);
			Whole("School switch cooling", t.switchLoss, 0, 100, "%d heat", "How much heat is lost when you cast a different school. 25 is one stage.", save);
			Percent("Held spells' gain", t.heldGain, 10, 200, "How fast a held spell (Flames, Sparks) builds heat, against a cast spell. 60% is the normal speed.", save);
			Whole("Warm at", t.at[0], 5, 90, "%d heat", "The heat at which the meter turns Warm.", save);
			Whole("Hot at", t.at[1], 10, 95, "%d heat", "The heat at which the meter turns Hot.", save);
			Whole("Overheating at", t.at[2], 15, 99, "%d heat", "The heat at which the meter turns Overheating. Overdrive is always at 100.", save);
			Toggle("Healing and ward spells build heat", s.counts.healingAndWards, "Off by default: healing spells and wards do not build heat.", save);
			ImGuiMCP::SameLine(column);
			Toggle("Staves and scrolls count", s.counts.stavesAndScrolls, "Staves and scrolls build heat and get the bonus.", save);
			Toggle("Enemies build heat", s.enemies,
				"Off by default. On: enemies' spells build heat of their own and get the same bonus - a harder game. They have no bar.", save);

			Heading(Icon::kStar, "Bonus");
			Percent("Bonus size per stage", t.bonusPerStage, 0, 25, "Warm adds this to your spells' power, Hot twice this, Overheating three times.", save);
			Percent("Overdrive bonus", t.overdriveBonus, 0, 100, "The bonus while Overdrive lasts.", save);
			{
				static const char* const kBoost[] = { "Damage only", "All harmful spell effects" };
				Choice("What is boosted", s.boost, kBoost, 2,
					"Damage only: spells that hurt. All harmful spell effects: every harmful effect's strength (fear, paralysis, "
					"slows and so on).",
					save);
			}

			Heading(Icon::kBolt, "Overdrive");
			{
				static const char* const kModes[] = { "Automatic", "Key press", "Off" };
				Choice("Overdrive", t.mode, kModes, 3,
					"Automatic: it starts by itself when the meter is full. Key press: the meter waits at Overheating until you press "
					"your key. Off: the meter stops at Overheating.",
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
			Whole("Burst length", t.burst, 1, 30, "%d s", "How long Overdrive lasts.", save);
			Whole("Cooldown length", t.cooldown, 0, 60, "%d s", "After Overdrive the meter is locked at zero for this long.", save);
			Toggle("Cooldown: slow magicka", t.slowMagicka, "During the cooldown your magicka comes back slower.", save);
			ImGuiMCP::BeginDisabled(!t.slowMagicka);
			Percent("Magicka comes back at", t.slowMagickaTo, 0, 100, "40% means magicka regenerates at 40% of its normal speed during the cooldown.", save);
			ImGuiMCP::EndDisabled();
			Toggle("Cooldown: weaker spells", t.weaker, "During the cooldown your spells are weaker.", save);
			ImGuiMCP::BeginDisabled(!t.weaker);
			Percent("Weaker by", t.weakerBy, 0, 90, "How much weaker your spells are during the cooldown.", save);
			ImGuiMCP::EndDisabled();
			ImGuiMCP::EndDisabled();

			Heading(Icon::kDisplay, "Display");
			Toggle("Light growth", s.lightGrowth,
				"The lights on your casting hands (from Illuminated or another spell light mod) grow brighter and wider with each stage, "
				"dip when the burnout starts and come back as it ends.",
				save);
			ImGuiMCP::BeginDisabled(!s.lightGrowth);
			Percent("Growth per stage", s.lightPerStage, 0, 100, "How much the hand lights grow at each stage.", save);
			ImGuiMCP::EndDisabled();
			Toggle("Hand glow", s.handGlow,
				"A soft light of this mod's own on a casting hand when it is Warm or hotter - only on a hand no other mod lights.", save);
			Toggle("Spells grow in Overdrive", s.spellGrowth,
				"The spell in your hands and the spells you cast grow as heat builds, then in Overdrive up to double size and twice as "
				"fierce at its start, falling back as Overdrive runs out.",
				save);
			ImGuiMCP::BeginDisabled(!s.spellGrowth);
			Percent("Growth at the start of Overdrive", s.growthMax, 0, 200, "100% means double size.", save);
			ImGuiMCP::EndDisabled();
			Toggle("Spells flicker out in burnout", s.burnoutFlicker,
				"During the cooldown after Overdrive the spell in your hands and your spells in the air sputter out and back, steadier "
				"as the cooldown ends. The hand lights dip with them.",
				save);
			// his 2026-10-07 lingering design + his answer 2026-10-18 (checklist 28 (b)): the marks, and that they hurt (Marks.cpp)
			Toggle("Scorch and frost marks", s.marks,
				"From Overheating on, your fire and frost spells leave a burning or frozen patch where they land for a few seconds.", save);
			ImGuiMCP::BeginDisabled(!s.marks);
			Toggle("Marks hurt", s.markHurt,
				"The patches hurt anyone who stands in them - never you, your followers or your summons. Fire and frost resistance "
				"protect. Stronger in Overdrive.",
				save);
			ImGuiMCP::EndDisabled();
			// his answer 2026-10-18 (checklist 28 (d)): the meter through Reticle Arcs (Arcs.cpp)
			ImGuiMCP::BeginDisabled(!ArcBarTaken());
			Toggle("Heat bar in Reticle Arcs", s.arcHeat,
				ArcBarTaken() ? "The heat as a Reticle Arcs bar round the crosshair, in Reticle Arcs' own look (set where it sits in its menu). "
								"While it shows, the bar below steps aside." :
								"Reticle Arcs draws this bar, and it is not installed.",
				save);
			ImGuiMCP::EndDisabled();
			Toggle("Bar under the magicka bar", s.bar, "A bar on the screen that shows the heat. It shows only while there is heat.", save);
			ImGuiMCP::BeginDisabled(!s.bar);
			{
				static const char* const kStyles[] = { "Flames", "Simple bar", "Spell bars" };
				Choice("Bar style", s.barStyle, kStyles, 3,
					"Flames: the bar burns - flame tongues that grow with each stage, rising embers, white-hot sparks in Overdrive and "
					"smoke in the cooldown, tinted by the element in your hand. Simple bar: a thin plain bar. Spell bars: a bar made of your "
					"spell's own art, moving - fire climbs off it, frost streams along it in the wind, lightning arcs round it, absorb "
					"spells draw their light in - spilling out all round the bar, and twice as big and fierce in Overdrive.",
					save);
			}
			Percent("Bar across", s.barX, 0, 100, "Where the bar's middle is, from the left edge of the screen.", save);
			Percent("Bar down", s.barY, 0, 100, "Where the bar is, from the top of the screen.", save);
			Percent("Bar size", s.barWidth, 50, 300, "How long and thick the bar is.", save);
			ImGuiMCP::EndDisabled();

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
			ImGuiMCP::TextColored(kEmber, "heat %.0f - %s", v.heat, std::string(Heat::StageName(v.stage)).c_str());
			ImGuiMCP::ProgressBar(std::clamp(v.heat / Heat::kFull, 0.0f, 1.0f), ImGuiMCP::ImVec2{ -1.0f, 0.0f }, "");
			if (v.state == Heat::Meter::State::kOverdrive) {
				ImGuiMCP::Text("Overdrive: %.1f s left", v.timer);
			} else if (v.state == Heat::Meter::State::kCooldown) {
				ImGuiMCP::Text("cooldown: %.1f s left", v.timer);
			} else if (v.ready) {
				ImGuiMCP::TextColored(kGold, "%s", "ready: press the Overdrive key");
			}
			ImGuiMCP::Text("last school: %s", std::string(Heat::SchoolName(v.last)).c_str());
			ImGuiMCP::Text("spell power x%.2f   magicka regeneration x%.2f", v.bonus, v.regen);
			ImGuiMCP::TextDisabled("effects boosted so far: %llu   enemies with heat: %zu", static_cast<unsigned long long>(v.boosted), v.enemies);
			const auto h = HandsNow();
			ImGuiMCP::Text("hand lights: %zu being scaled (x%.2f left, x%.2f right), %zu of this mod's own", h.scaled, h.factor[0], h.factor[1], h.own);
			if (h.frozen) {
				ImGuiMCP::TextColored(kGold, "%zu hand light(s) held steady: another mod scales them the same way", h.frozen);
			}

			Heading(Icon::kDisplay, "Spell bars (Bar style)");
			{
				const auto        ls = LookStatsNow();
				const auto        art = SpellArtNow();
				const auto        ov = OverloadNow();
				const std::string spell(SpellLook::Name(v.look));
				const std::string shown = ls.shown >= 0 ? std::string(SpellLook::Name(static_cast<SpellLook::Look>(ls.shown))) : "none yet";
				ImGuiMCP::Text("the spell's look: %s   drawn: %s", spell.c_str(), shown.c_str());
				ImGuiMCP::TextDisabled("its own art: particles %s%s, fill %s%s, %zu flipbook cell(s), color %s", art.bodyPath.empty() ? "none" : art.bodyPath.c_str(),
					art.body ? "" : " (not loaded)", art.fillPath.empty() ? "none" : art.fillPath.c_str(), art.fill ? "" : " (not loaded)", art.frames.size(),
					art.colored ? "its own" : "the look's");
				ImGuiMCP::TextDisabled("particles %d, arcs %d, built-in sprites %s; drawing %.0f us a frame (most %.0f us)", ls.particles, ls.arcs,
					ls.sprites ? "loaded" : "MISSING", ls.drawUsAvg, ls.drawUsMax);
				ImGuiMCP::Text("spells now x%.2f size; grown: %zu casting art(s), %zu projectile(s); sputtered out: %zu hand(s)", ov.power, ov.arts, ov.projectiles, ov.out);
				static const char* const kShow[] = { "the spell's own", "generic", "fire", "frost", "shock", "absorb health", "absorb magicka",
					"absorb stamina", "healing", "illusion", "conjuration", "bound weapon", "alteration", "sun", "poison" };
				static_assert(std::size(kShow) == SpellLook::kLooks + 1);
				int pick = ls.forced + 1;
				ImGuiMCP::SetNextItemWidth(220.0f);
				if (ImGuiMCP::Combo("Show this look", &pick, kShow, static_cast<int>(std::size(kShow)))) {
					BenchForceLook(pick - 1);
				}
				Tip("To see every look without casting each kind of spell: set Bar style to Spell bars, set the heat below, and pick a look (a forced look uses its built-in art). "
					"Not saved.");
			}

			Heading(Icon::kWand, "Try it");
			static int heat = 50;
			ImGuiMCP::SetNextItemWidth(200.0f);
			ImGuiMCP::SliderInt("##heat", &heat, 0, 100, "heat %d");
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Set the heat")) {
				BenchSetHeat(static_cast<float>(heat));
			}
			Tip("Sets your heat now, to see the bar, the bonus and the hand lights.");
			for (int st = 0; st <= 4; ++st) {
				if (st > 0) {
					ImGuiMCP::SameLine();
				}
				if (ImGuiMCP::Button(std::string(Heat::StageName(static_cast<Heat::Stage>(st))).c_str())) {
					BenchForceStage(static_cast<Heat::Stage>(st));
				}
			}
			Tip("Puts the meter at the start of this stage now (Overdrive starts it).");
		}

		// ------------------------------------------------------------------ the bar, on the render thread
		ImGuiMCP::ImVec4 StageColor(Heat::Stage a_stage, float a_alpha)
		{
			switch (a_stage) {
			case Heat::Stage::kWarm:
				return { 1.0f, 0.82f, 0.35f, a_alpha };
			case Heat::Stage::kHot:
				return { 1.0f, 0.55f, 0.18f, a_alpha };
			case Heat::Stage::kOverheating:
				return { 1.0f, 0.28f, 0.12f, a_alpha };
			case Heat::Stage::kOverdrive:
				return { 1.0f, 0.95f, 0.85f, a_alpha };
			default:
				return { 0.85f, 0.72f, 0.55f, a_alpha };
			}
		}

		// ------------------------------------------------------------------ the flame bar (his order 2026-10-11: "make the hud
		// for arcane look cooler its too simple right now maybe like a fx hud if possible like flames"). Drawn from FlameFx.h:
		// a dark bronze-framed slot; the fill is flowing magma on the heat ramp; flame tongues stand on the fill, taller and
		// more unsteady stage by stage; embers rise off it (and sparks in Overdrive); the head of the fill flares; the cooldown
		// (burnout) is a cooling grey-blue bar with smoke and the last embers. The element of the spell in hand tints it.
		using Flames = FlameFx::Particles<128>;

		FlameFx::Mood MoodOf(const MeterView& a_v)
		{
			if (a_v.state == Heat::Meter::State::kCooldown) {
				return FlameFx::Mood::kBurnout;
			}
			if (a_v.state == Heat::Meter::State::kOverdrive) {
				return FlameFx::Mood::kOverdrive;
			}
			return static_cast<FlameFx::Mood>(std::clamp(static_cast<int>(a_v.stage), 0, 3));
		}

		// the heat ramp, leaned toward the element in hand: frost runs to ice blue, shock to violet-white, fire stays fire
		FlameFx::Rgb Tinted(float a_t, int a_element)
		{
			auto        c = FlameFx::HeatColor(a_t);
			const float k = 0.55f;
			switch (a_element) {
			case 2:  // frost
				c = { c.r * (1 - k) + (0.45f + 0.5f * a_t) * k, c.g * (1 - k) + (0.75f + 0.25f * a_t) * k, c.b * (1 - k) + 1.0f * k };
				break;
			case 3:  // shock
				c = { c.r * (1 - k) + (0.65f + 0.35f * a_t) * k, c.g * (1 - k) + (0.55f + 0.45f * a_t) * k, c.b * (1 - k) + 1.0f * k };
				break;
			default:
				break;
			}
			return c;
		}

		ImGuiMCP::ImU32 Col(const FlameFx::Rgb& a_c, float a_alpha)
		{
			return ImGuiMCP::ColorConvertFloat4ToU32(ImGuiMCP::ImVec4{ std::clamp(a_c.r, 0.0f, 1.0f), std::clamp(a_c.g, 0.0f, 1.0f),
				std::clamp(a_c.b, 0.0f, 1.0f), std::clamp(a_alpha, 0.0f, 1.0f) });
		}

		// one tongue of flame standing on (a_x, a_base): a teardrop leaning by a_lean, a_w wide and a_h tall
		void Tongue(ImGuiMCP::ImDrawList* a_dl, float a_x, float a_base, float a_w, float a_h, float a_lean, ImGuiMCP::ImU32 a_col)
		{
			using ImGuiMCP::ImVec2;
			if (a_h < 0.5f || a_w < 0.5f) {
				return;
			}
			const ImVec2 pts[]{ { a_x - a_w * 0.5f, a_base }, { a_x - a_w * 0.55f, a_base - a_h * 0.28f },
				{ a_x - a_w * 0.32f + a_lean * 0.35f, a_base - a_h * 0.62f }, { a_x + a_lean, a_base - a_h },
				{ a_x + a_w * 0.32f + a_lean * 0.35f, a_base - a_h * 0.62f }, { a_x + a_w * 0.55f, a_base - a_h * 0.28f },
				{ a_x + a_w * 0.5f, a_base } };
			ImGuiMCP::ImDrawListManager::AddConvexPolyFilled(a_dl, pts, 7, a_col);
		}

		void DrawFlames(ImGuiMCP::ImDrawList* a_dl, const Settings& a_s, const MeterView& a_v, float a_alpha, float a_dt)
		{
			using namespace ImGuiMCP;
			static Flames particles;
			auto*         io = GetIO();
			const float   t = static_cast<float>(GetTime());
			const float   k = std::clamp(a_s.barWidth, 0.5f, 3.0f);
			const float   w = 240.0f * k, h = 9.0f * k;
			const float   cx = io->DisplaySize.x * std::clamp(a_s.barX, 0.0f, 1.0f), y = io->DisplaySize.y * std::clamp(a_s.barY, 0.0f, 1.0f);
			const float   x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
			const auto    mood = MoodOf(a_v);
			const auto    look = FlameFx::LookOf(mood);
			const bool    burnout = mood == FlameFx::Mood::kBurnout, overdrive = mood == FlameFx::Mood::kOverdrive;
			float         fill = std::clamp(a_v.heat / Heat::kFull, 0.0f, 1.0f);
			if (overdrive || burnout) {
				fill = std::clamp(a_v.timer / std::max(a_v.timerOf, 0.1f), 0.0f, 1.0f);
			}
			const float fw = w * fill;
			const float pulse = overdrive ? 0.82f + 0.18f * std::sin(t * 10.0f) : 1.0f;

			// the glow under the fill, three soft layers
			if (!burnout && fw > 1.0f) {
				const auto g = Tinted(look.temperature * 0.8f, a_v.element);
				for (int i = 3; i >= 1; --i) {
					const float p = 4.0f * k * static_cast<float>(i);
					ImDrawListManager::AddRectFilled(a_dl, ImVec2{ x0 - p, y - p - h * look.tongue * 0.3f }, ImVec2{ x0 + fw + p, y + h + p },
						Col(g, (0.05f + 0.04f * look.temperature) * a_alpha * pulse), p + 2.0f * k, 0);
				}
			}
			// the slot: dark, with a bronze frame and an inner shadow
			ImDrawListManager::AddRectFilled(a_dl, ImVec2{ x0 - 2.0f * k, y - 2.0f * k }, ImVec2{ x1 + 2.0f * k, y + h + 2.0f * k },
				Col({ 0.035f, 0.025f, 0.02f }, 0.82f * a_alpha), 3.0f * k, 0);
			ImDrawListManager::AddRect(a_dl, ImVec2{ x0 - 2.0f * k, y - 2.0f * k }, ImVec2{ x1 + 2.0f * k, y + h + 2.0f * k },
				Col({ 0.62f, 0.43f, 0.24f }, 0.65f * a_alpha), 3.0f * k, 0, std::max(1.0f, k));
			// the fill: flowing magma - narrow slices on the heat ramp, hotter toward the head, moving with the noise
			const float slice = 3.0f * k;
			for (float x = 0.0f; x < fw; x += slice) {
				const float  xr = std::min(x + slice, fw);
				const float  along = fw > 0.0f ? x / fw : 0.0f;
				const float  flow = FlameFx::Noise2(x * 0.045f / k - t * 1.3f, t * 0.55f);
				FlameFx::Rgb c;
				if (burnout) {
					const float cool = 0.35f + 0.25f * flow;
					c = { 0.30f + 0.15f * cool, 0.34f + 0.16f * cool, 0.42f + 0.18f * cool };
				} else {
					c = Tinted(std::clamp(look.temperature * (0.5f + 0.5f * along) * (0.8f + 0.35f * flow), 0.0f, 1.0f), a_v.element);
				}
				ImDrawListManager::AddRectFilled(a_dl, ImVec2{ x0 + x, y }, ImVec2{ x0 + xr, y + h }, Col(c, 0.95f * a_alpha * pulse), 0.0f, 0);
			}
			// a hot seam along the top of the fill
			if (fw > 1.0f && !burnout) {
				ImDrawListManager::AddLine(a_dl, ImVec2{ x0, y + 0.5f }, ImVec2{ x0 + fw, y + 0.5f },
					Col(Tinted(std::min(1.0f, look.temperature + 0.2f), a_v.element), 0.7f * a_alpha), std::max(1.0f, k));
			}
			// the tongues, three layers each: an outer red veil, the body, a white-hot core
			if (fw > 2.0f && look.tongue > 0.0f) {
				const float step = 9.0f * k;
				const int   n = std::max(1, static_cast<int>(fw / step));
				for (int layer = 0; layer < 3; ++layer) {
					const float scale = layer == 0 ? 1.0f : layer == 1 ? 0.7f :
					                                                     0.38f;
					const float shift = layer == 0 ? -0.38f : layer == 1 ? -0.12f :
					                                                       0.18f;
					const float opacity = layer == 0 ? 0.42f : layer == 1 ? 0.62f :
					                                                        0.85f;
					for (int i = 0; i <= n; ++i) {
						const float fi = static_cast<float>(i);
						const float x = x0 + std::min(fw, (fi + 0.5f) * fw / static_cast<float>(n + 1));
						const float flick = FlameFx::Flicker(fi * 0.37f, t * look.flicker);
						const float jump = 1.0f + look.unsteady * (FlameFx::Noise1(fi * 1.9f + t * 6.0f) - 0.5f) * 1.6f;
						const float head = 0.65f + 0.35f * (x - x0) / std::max(fw, 1.0f);  // taller toward the head
						const float th = h * look.tongue * (0.45f + 0.55f * flick) * jump * head * scale * pulse;
						const float tw = step * 1.35f * (0.75f + 0.35f * flick) * (0.6f + 0.4f * scale);
						const float lean = (FlameFx::Noise1(fi * 1.7f + t * 1.4f) - 0.5f) * tw * 1.1f;
						const auto  c = Tinted(std::clamp(look.temperature + shift, 0.0f, 1.0f), a_v.element);
						Tongue(a_dl, x, y + h * 0.35f, tw, th + h * 0.35f, lean, Col(c, opacity * a_alpha));
					}
				}
			}
			// the head: a flare at the fill's edge, breathing
			if (fw > 1.0f) {
				const float hx = x0 + fw, hy = y + h * 0.5f;
				const float breath = 0.8f + 0.2f * std::sin(t * (burnout ? 2.0f : 7.0f));
				const auto  hc = burnout ? FlameFx::Rgb{ 0.6f, 0.68f, 0.8f } : Tinted(std::min(1.0f, look.temperature + 0.25f), a_v.element);
				for (int g = 0; g < 6; ++g) {
					const float r = (16.0f - static_cast<float>(g) * 2.4f) * k * breath * (overdrive ? 1.5f : 1.0f);
					ImDrawListManager::AddCircleFilled(a_dl, ImVec2{ hx, hy }, r, Col(hc, (0.035f + 0.02f * static_cast<float>(g)) * a_alpha), 20);
				}
				const float r = 4.0f * k;
				ImDrawListManager::AddQuadFilled(a_dl, ImVec2{ hx, hy - r * 1.4f }, ImVec2{ hx + r, hy }, ImVec2{ hx, hy + r * 1.4f }, ImVec2{ hx - r, hy },
					Col({ 1.0f, 0.97f, 0.88f }, a_alpha));
			}
			// the stage marks: small bronze diamonds on the frame
			for (const float at : a_s.tuning.at) {
				const float mx = x0 + w * std::clamp(at / Heat::kFull, 0.0f, 1.0f), my = y + h + 2.0f * k;
				const float r = 2.6f * k;
				ImDrawListManager::AddQuadFilled(a_dl, ImVec2{ mx, my - r }, ImVec2{ mx + r, my }, ImVec2{ mx, my + r }, ImVec2{ mx - r, my },
					Col({ 0.78f, 0.58f, 0.32f }, 0.85f * a_alpha));
			}
			// the ends: bronze chevrons
			for (const auto [ex, d] : { std::pair{ x0 - 2.0f * k, -1.0f }, std::pair{ x1 + 2.0f * k, 1.0f } }) {
				const float my = y + h * 0.5f;
				ImDrawListManager::AddTriangleFilled(a_dl, ImVec2{ ex + d * 9.0f * k, my }, ImVec2{ ex, my - 5.0f * k }, ImVec2{ ex, my + 5.0f * k },
					Col({ 0.70f, 0.50f, 0.28f }, 0.85f * a_alpha));
			}

			// embers, sparks, smoke
			particles.Emit(FlameFx::Particle::Kind::kEmber, look.embers * std::max(fill, 0.15f), a_dt, std::max(fw, 4.0f), h, k);
			particles.Emit(FlameFx::Particle::Kind::kSpark, look.sparks, a_dt, std::max(fw, 4.0f), h, k);
			particles.Emit(FlameFx::Particle::Kind::kSmoke, look.smoke, a_dt, w, h, k);
			particles.Step(a_dt, t);
			for (const auto& p : particles.All()) {
				if (!p.alive) {
					continue;
				}
				const float  left = p.Left();
				const ImVec2 at{ x0 + p.x, y + p.y };
				switch (p.kind) {
				case FlameFx::Particle::Kind::kSmoke:
					ImDrawListManager::AddCircleFilled(a_dl, at, p.size, Col({ 0.42f, 0.42f, 0.46f }, 0.22f * left * a_alpha), 16);
					break;
				case FlameFx::Particle::Kind::kSpark:
					ImDrawListManager::AddLine(a_dl, at, ImVec2{ at.x - p.vx * 0.03f, at.y - p.vy * 0.03f }, Col({ 1.0f, 0.95f, 0.8f }, left * a_alpha),
						p.size);
					break;
				default:
					{
						const auto c = burnout ? FlameFx::Rgb{ 0.9f, 0.4f, 0.15f } : Tinted(0.55f + 0.4f * left, a_v.element);
						ImDrawListManager::AddCircleFilled(a_dl, at, p.size * 2.6f, Col(c, 0.12f * left * a_alpha), 12);
						ImDrawListManager::AddCircleFilled(a_dl, at, p.size, Col(c, 0.9f * left * a_alpha), 8);
						break;
					}
				}
			}

			// the stage's name over the right end, small capitals
			static constexpr const char* kNames[]{ "", "WARM", "HOT", "OVERHEATING", "OVERDRIVE", "BURNOUT" };
			const char*                  name = a_v.ready ? "READY" : kNames[static_cast<int>(mood)];
			if (*name) {
				const auto   nc = burnout ? FlameFx::Rgb{ 0.62f, 0.7f, 0.82f } : Tinted(std::min(1.0f, look.temperature + 0.15f), a_v.element);
				const float  glint = a_v.ready ? 0.6f + 0.4f * std::sin(t * 6.0f) : 1.0f;
				const ImVec2 tp{ x1 - CalcTextSize(name).x, y - h * 0.5f - 16.0f * k - h * look.tongue * 0.5f };
				ImDrawListManager::AddText(a_dl, ImVec2{ tp.x + 1.0f, tp.y + 1.0f }, Col({ 0.0f, 0.0f, 0.0f }, 0.8f * a_alpha), name);
				ImDrawListManager::AddText(a_dl, tp, Col(nc, glint * a_alpha), name);
			}
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
			auto*        io = GetIO();
			if (!io) {
				return;
			}
			// Reticle Arcs shows the meter instead while it took the bar and its switch is on (Arcs.cpp)
			const bool  show = s.enabled && s.bar && !(s.arcHeat && ArcBarTaken()) && !HideHud() &&
			                   (v.heat > 0.5f || v.state != Heat::Meter::State::kBuilding);
			const float dt = std::isfinite(io->DeltaTime) ? std::clamp(io->DeltaTime, 0.0f, 0.25f) : 0.0f;
			alpha = std::clamp(alpha + (show ? dt : -dt) * 3.0f, 0.0f, 1.0f);
			if (alpha <= 0.0f) {
				return;
			}
			auto* dl = GetForegroundDrawList();
			if (!dl) {
				return;
			}
			if (s.barStyle == 2 && DrawBars(dl, s, v, alpha, dt)) {
				return;
			}
			if (s.barStyle != 1) {  // the flames - also when the spell bars cannot draw
				DrawFlames(dl, s, v, alpha, dt);
				return;
			}
			const float k = std::clamp(s.barWidth, 0.5f, 3.0f);
			const float w = 200.0f * k, h = 5.0f * k;
			const float cx = io->DisplaySize.x * std::clamp(s.barX, 0.0f, 1.0f), y = io->DisplaySize.y * std::clamp(s.barY, 0.0f, 1.0f);
			const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
			ImDrawListManager::AddRectFilled(dl, ImVec2{ x0 - 2.0f, y - 2.0f }, ImVec2{ x1 + 2.0f, y + h + 2.0f },
				ColorConvertFloat4ToU32(ImVec4{ 0.05f, 0.04f, 0.03f, 0.75f * alpha }), 2.0f, 0);
			float fill = std::clamp(v.heat / Heat::kFull, 0.0f, 1.0f);
			auto  color = StageColor(v.stage, 0.95f * alpha);
			if (v.state == Heat::Meter::State::kOverdrive) {
				const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(GetTime()) * 10.0f);
				color = { 1.0f, 0.95f * pulse, 0.85f * pulse, alpha };
				fill = std::clamp(v.timer / std::max(v.timerOf, 0.1f), 0.0f, 1.0f);
			} else if (v.state == Heat::Meter::State::kCooldown) {
				color = { 0.45f, 0.5f, 0.6f, 0.8f * alpha };  // grey-blue: what is left of the cooldown
				fill = std::clamp(v.timer / std::max(v.timerOf, 0.1f), 0.0f, 1.0f);
			}
			ImDrawListManager::AddRectFilled(dl, ImVec2{ x0, y }, ImVec2{ x0 + w * fill, y + h }, ColorConvertFloat4ToU32(color), 2.0f, 0);
			// the stage marks
			for (const float at : s.tuning.at) {
				const float x = x0 + w * std::clamp(at / Heat::kFull, 0.0f, 1.0f);
				ImDrawListManager::AddLine(dl, ImVec2{ x, y }, ImVec2{ x, y + h }, ColorConvertFloat4ToU32(ImVec4{ 0.0f, 0.0f, 0.0f, 0.6f * alpha }), 1.0f);
			}
			if (v.ready) {
				const ImU32 text = ColorConvertFloat4ToU32(ImVec4{ 1.0f, 0.9f, 0.7f, alpha });
				ImDrawListManager::AddText(dl, ImVec2{ x1 + 6.0f, y - 5.0f * k }, text, "Ready");
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

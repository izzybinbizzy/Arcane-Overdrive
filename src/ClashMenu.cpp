// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Two pages in Arcane Overdrive's section of SKSE Menu Framework: Clashes and Spell struggles - switches, and a slider where
// a player tunes by feel; every other number lives in SpellMasteryHeat Clashes.ini, which the menu writes whole. The menu
// draws off the game's main thread, so it works on a copy of the settings (MenuCopy) and hands each change over as a task on
// the main thread (Submit). The reaction table is shown, not edited: it lives in SpellMasteryHeat Clash Rules.ini, which a
// patch can add to.

#include "ClashShared.h"
#include "Guarded.h"
#include "Plugin.h"

// SKSE Menu Framework's own header (theirs, MIT): its warnings are not ours, and ours are errors - under MSVC and clang-cl
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
#include "Translation.h"

namespace Crossfire
{
	namespace
	{
		constexpr ImGuiMCP::ImVec4 kDim{ 0.75f, 0.72f, 0.66f, 1.0f };
		using Translation::T;

		// T() for a name that is not a string literal: the English is kept here, so the pointer T() hands back (the English
		// itself when there is no translation) outlives the call (render thread only)
		const char* TS(std::string_view a_english)
		{
			static std::unordered_set<std::string> kept;
			return T(kept.emplace(a_english).first->c_str());
		}

		namespace Icon = MenuStyle::Icon;

		void Heading(unsigned a_icon, const char* a_text) { MenuStyle::Header(a_icon, T(a_text)); }

		// switches take half the page, so a paired switch can sit beside its partner at the same column
		class Layout
		{
		public:
			Layout() :
				column(ImGuiMCP::GetContentRegionAvail().x * 0.5f)
			{
				ImGuiMCP::PushItemWidth(column * 0.9f);
				ImGuiMCP::PushTextWrapPos(0.0f);  // long lines wrap at the panel's edge instead of running past it
			}
			~Layout()
			{
				ImGuiMCP::PopTextWrapPos();
				ImGuiMCP::PopItemWidth();
			}
			Layout(const Layout&) = delete;
			Layout& operator=(const Layout&) = delete;

			void Beside() const { ImGuiMCP::SameLine(column); }

			const float column;
		};

		// A change goes to the game at once and is written to the file; a slider's only when it is let go
		struct Edit
		{
			std::uint64_t gen{ 0 };
			Core::Config  cfg{ MenuCopy(gen) };
			bool          changed{ false };
			bool          save{ false };

			void After(bool a_changed)
			{
				changed |= a_changed;
				save |= a_changed;
			}
			// after a slider: live while it is dragged, written once when it is let go
			void Dragged(bool a_changed)
			{
				const bool done = ImGuiMCP::IsItemDeactivatedAfterEdit();
				changed |= a_changed || done;
				save |= done;
			}
			Edit() = default;
			Edit(const Edit&) = delete;
			Edit& operator=(const Edit&) = delete;
			~Edit()
			{
				if (changed) {
					(void)SubmitIf(cfg, save, gen);  // stale: this frame's click is dropped, the next frame draws the newer copy
				}
			}
		};

		void Check(Edit& a_e, const char* a_label, bool& a_value, const char* a_tip)
		{
			a_e.After(ImGuiMCP::Checkbox(T(a_label), &a_value));
			ImGuiMCP::SetItemTooltip("%s", T(a_tip));
		}

		// a percent setting 0 - 300, saved when the slider is let go
		void Percent(Edit& a_e, const char* a_label, float& a_value, const char* a_tip)
		{
			int        pct = static_cast<int>(std::lround(std::clamp(a_value, 0.0f, 300.0f)));
			const bool moved = ImGuiMCP::SliderInt(T(a_label), &pct, 0, 300, "%d%%");
			a_value = static_cast<float>(pct);
			a_e.Dragged(moved);
			ImGuiMCP::SetItemTooltip("%s", T(a_tip));
		}

		// an element's color in the table
		[[nodiscard]] ImGuiMCP::ImVec4 ElementColor(std::size_t a_element)
		{
			switch (static_cast<Core::Element>(a_element)) {
			case Core::Element::kFire:
				return { 1.0f, 0.45f, 0.15f, 1.0f };
			case Core::Element::kFrost:
				return { 0.45f, 0.78f, 1.0f, 1.0f };
			case Core::Element::kShock:
				return { 0.62f, 0.62f, 1.0f, 1.0f };
			case Core::Element::kPoison:
				return { 0.45f, 0.85f, 0.35f, 1.0f };
			case Core::Element::kPhysical:
				return { 0.8f, 0.8f, 0.8f, 1.0f };
			case Core::Element::kForce:
				return { 0.95f, 0.9f, 0.75f, 1.0f };
			default:
				return { 0.8f, 0.5f, 1.0f, 1.0f };
			}
		}

		// a reaction in its color: a win green, a loss red, both going amber, passing by grey
		ImGuiMCP::ImVec4 ActionColor(Core::Action a_action)
		{
			switch (a_action) {
			case Core::Action::kWins:
				return { 0.5f, 0.88f, 0.5f, 1.0f };
			case Core::Action::kLoses:
				return { 1.0f, 0.5f, 0.45f, 1.0f };
			case Core::Action::kAnnihilate:
				return { 1.0f, 0.7f, 0.35f, 1.0f };
			case Core::Action::kPass:
				return MenuStyle::kMuted;
			default:
				return kDim;
			}
		}

		void DrawTable(const Core::Config& a_cfg)
		{
			using namespace ImGuiMCP;
			constexpr int kColumns = static_cast<int>(Core::kElements) + 1;
			if (!BeginTable("reactions", kColumns, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				return;
			}
			PushTextWrapPos(-1.0f);  // one word a cell: never broken inside it (the page's own wrap would split "Annihilate")
			TableSetupColumn(T("vs"));
			for (std::size_t j = 0; j < Core::kElements; ++j) {
				TableSetupColumn(TS(Core::ElementName(static_cast<Core::Element>(j))));
			}
			TableNextRow(ImGuiTableRowFlags_Headers);
			TableNextColumn();
			TextColored(kDim, "%s", T("vs"));
			for (std::size_t j = 0; j < Core::kElements; ++j) {
				TableNextColumn();
				TextColored(ElementColor(j), "%s", TS(Core::ElementName(static_cast<Core::Element>(j))));
			}
			for (std::size_t i = 0; i < Core::kElements; ++i) {
				TableNextRow();
				TableNextColumn();
				TextColored(ElementColor(i), "%s", TS(Core::ElementName(static_cast<Core::Element>(i))));
				for (std::size_t j = 0; j < Core::kElements; ++j) {
					TableNextColumn();
					const auto action = a_cfg.reactions[i][j];
					TextColored(ActionColor(action), "%s", TS(Core::ActionName(action)));
				}
			}
			PopTextWrapPos();
			EndTable();
		}

		void DrawClashes()
		{
			const MenuStyle::Page page;
			const Layout          layout;
			Edit                  e;
			auto&                 c = e.cfg;

			Check(e, "Clashes", c.enabled, "Spells and arrows that meet in the air clash. Off: nothing here happens at all.");
			ImGuiMCP::BeginDisabled(!c.enabled);

			Heading(Icon::kShield, "Who clashes");
			{
				const char* who[]{ T("Everyone's projectiles"), T("Only when yours are involved") };
				e.After(ImGuiMCP::Combo(T("Projectiles"), &c.who, who, 2));
				ImGuiMCP::SetItemTooltip("%s", T("Everyone: two enemy mages' spells can meet too. Yours only: a clash always involves one of your projectiles."));
			}
			Check(e, "Allies pass through", c.ignoreAllies, "Projectiles of people who are not hostile to each other (you and your follower) never clash.");

			Heading(Icon::kBolt, "Clash");
			Check(e, "The survivor is weakened", c.weakenSurvivor, "The one that flies on loses the strength of what it beat.");
			layout.Beside();
			Check(e, "Bolts meet", c.boltsMeet, "Two lightning bolts (any one-shot bolt) that cross burst between the casters and both go.");

			Heading(Icon::kTarget, "Auto lock");
			Check(e, "Auto lock your spells", c.lockYours,
				"Your bolts fly at the enemy you cast them toward, so they meet theirs in the air. Only when you aim near "
				"an enemy; with True Directional Movement's target lock on, TDM aims them.");
			layout.Beside();
			Check(e, "Auto lock enemy spells", c.lockOthers, "Every other caster's bolts fly at whoever it fights.");
			Check(e, "Steer into oncoming spells", c.steer, "A bolt bends toward an enemy spell coming at it that would nearly miss, so the two meet.");
			layout.Beside();
			Check(e, "Arrows too", c.lockArrows, "Arrows lock and steer like bolts.");
			Percent(e, "Head-on catch", c.headOnCatch,
				"How far apart two bolts flying head on at each other still meet. 100% is the normal reach; "
				"lower, they must pass closer; higher, a wider miss still clashes. 0% turns it off.");

			Heading(Icon::kFire, "Effects");
			Check(e, "Bursts", c.explosions, "A destroyed projectile bursts where it was hit, with its own explosion.");
			layout.Beside();
			ImGuiMCP::BeginDisabled(!c.explosions);
			Check(e, "Only harmless bursts", c.safeExplosionsOnly, "Skip a burst that would do more than show: damage, an enchantment, a push, something it spawns.");
			Check(e, "Stand-in bursts", c.standInBursts, "A spell with no explosion of its own (Firebolt, Ice Spike, Lightning Bolt) bursts the way its element's spells do.");
			Check(e, "Clash designs", c.designs, "A clash's own effect, drawn for its kind of spell in the spell's color, on top of its burst.");
			Percent(e, "Clash light", c.clashLight,
				"The light a clash casts, in the spell's color, flaring and fading with it: it lights the ground "
				"and the clash's smoke and pieces. 0% turns it off.");
			Check(e, "Particle lights", c.particleLights,
				"The burst and its flying pieces light their surroundings as they scatter (ENB's or Community Shaders' particle lights; "
				"nothing on Vanilla).");
			ImGuiMCP::EndDisabled();

			ImGuiMCP::EndDisabled();

			Heading(Icon::kList, "What meets what");
			ImGuiMCP::TextDisabled("%s", T("Row against column. Clash: the stronger wins if it is enough stronger, else both go. Edit SpellMasteryHeat Clash Rules.ini to change."));
			DrawTable(c);
			if (ImGuiMCP::Button(T("Reload the files"))) {
				ReloadSoon();
			}
			ImGuiMCP::SetItemTooltip("%s", T("Read SpellMasteryHeat Clash Rules.ini, the SpellMasteryHeat Clash folder and SpellMasteryHeat Clashes.ini again."));

			Heading(Icon::kGauge, "This session");
			Check(e, "Log every clash", c.debugLog, "Write each clash to SpellMasteryHeat.log (Documents\\My Games\\Skyrim Special Edition\\SKSE).");
			auto& s = Counters();
			ImGuiMCP::TextDisabled(T("%u projectile(s) in range now; %llu clash(es), %llu destroyed, %llu weakened, %llu burst(s), %llu by you"), s.tracked.load(),
				static_cast<unsigned long long>(s.clashes.load()), static_cast<unsigned long long>(s.destroyed.load()),
				static_cast<unsigned long long>(s.weakened.load()), static_cast<unsigned long long>(s.explosions.load()),
				static_cast<unsigned long long>(s.yours.load()));
			for (const auto& note : LoadNotes()) {
				ImGuiMCP::TextDisabled("%s", note.c_str());
			}
			ImGuiMCP::TextDisabled("%s", T("Every number (sizes, strengths, burst limits, experience) is in SpellMasteryHeat Clashes.ini."));
		}

		void DrawStruggles()
		{
			const MenuStyle::Page page;
			const Layout          layout;
			Edit                  e;
			auto&                 s = e.cfg.struggle;

			Check(e, "Spell struggles", s.enabled,
				"When sprays, held beams or breath meet head on, they lock together and push. Whoever has the higher magic skill drives the "
				"meeting point back and breaks through. Off: they pass through each other.");
			ImGuiMCP::BeginDisabled(!s.enabled);

			Heading(Icon::kHand, "What locks");
			Check(e, "Sprays", s.sprays, "Flames, Frostbite and other sprays.");
			ImGuiMCP::SameLine(layout.column * 0.5f);
			Check(e, "Beams", s.beams, "Sparks, Lightning Storm and other held beams. A one-shot bolt never locks.");
			layout.Beside();
			Check(e, "Breath", s.breath, "Fire and Frost Breath, yours or a dragon's.");
			Check(e, "Fire and frost lock too", s.opposites, "Fire and frost lock and push like any other pair. Off: they pass through each other.");
			layout.Beside();
			Check(e, "Between others", s.betweenOthers, "Two casters neither of whom is you can lock too.");

			Heading(Icon::kStar, "Who wins");
			Check(e, "Higher magic skill always wins", s.skillAlwaysWins,
				"When both cast from a school of magic, the higher skill always pushes through; the rest only changes how fast.");
			layout.Beside();
			Check(e, "Surges", s.surge, "A side pushed far back surges once, for a moment.");
			Check(e, "Overdrive Breakpoint", s.breakpoint,
				"Release Overdrive while locked: your stream swells and shoves the meeting point toward them, harder on its pulse, and "
				"pushes twice as hard for the rest of the Overdrive.");

			Heading(Icon::kEye, "The struggle bar");
			Check(e, "Struggle bar", s.bar, "While you are locked, a bar shows who is pushing, with your Overdrive bar as a ring at your end.");
			layout.Beside();
			if (ImGuiMCP::Button(T("Preview the bar"))) {
				RequestBarPreview();
			}
			ImGuiMCP::SetItemTooltip("%s", T("Shows the bar for a few seconds: a lock, your Overdrive filling, then released on the pulse."));
			Check(e, "Names over the bar", s.barNames, "Your name and theirs over the bar.");
			layout.Beside();
			Check(e, "Skills under the bar", s.barSkills, "Both magic skills under the bar.");

			Heading(Icon::kTarget, "Breakthrough");
			Check(e, "Break the loser's cast", s.breakCast, "The overwhelmed caster's spell breaks and they reel for a moment.");
			layout.Beside();
			Check(e, "Stagger the loser", s.stagger, "The overwhelmed caster staggers, with the game's own stagger. Not dragons.");
			Check(e, "Finishers", s.finishers, "A breakthrough that kills throws the body away from the winner. Experimental.");
			layout.Beside();
			Check(e, "Messages", s.messages, "A message when you overwhelm someone, they overwhelm you, they give way, or the spells burst between you.");

			ImGuiMCP::EndDisabled();

			Heading(Icon::kEye, "Now");
			const auto v = StruggleNow();
			MenuStyle::Status(v.active, v.active ? T("Locked") : T("Free"));
			if (v.active) {
				ImGuiMCP::Text(T("Locked with %s: %s %d against %d, %.1f s, the lock %s"), v.foe, T(v.school), v.mySkill, v.theirSkill, v.seconds,
					v.balance > 0.02f ? T("moving toward them") : (v.balance < -0.02f ? T("moving toward you") : T("even")));
			} else {
				ImGuiMCP::TextDisabled("%s", T("Not locked with anyone."));
			}
			auto& n = Counters();
			ImGuiMCP::TextDisabled(T("This session: %llu struggle(s), %llu broken through, %llu won and %llu lost by you, %llu gave way, %llu draw(s)"),
				static_cast<unsigned long long>(n.struggles.load()), static_cast<unsigned long long>(n.overwhelms.load()),
				static_cast<unsigned long long>(n.won.load()), static_cast<unsigned long long>(n.lost.load()),
				static_cast<unsigned long long>(n.gaveWay.load()), static_cast<unsigned long long>(n.draws.load()));
			ImGuiMCP::TextDisabled("%s", T("Every number (weights, magicka, surges, reeling, camera shake) is in SpellMasteryHeat Clashes.ini."));
		}

		// ---------------------------------------------------------------- the struggle bar (Overdrive Breakpoint), on the HUD
		// Crossfire's bar in the game's own meter style: each stream in its spell's color from its end, brightest where they meet
		// at the compass's diamond, which pulses on the lock's beat (Core::kKnotBeat); your end carries your Overdrive bar as a
		// ring in the Heat's color. Your release during the lock flashes there and swells your side. Render thread: it reads
		// only copies (StruggleNow, PlayerView).
		using ImGuiMCP::ImVec2;
		constexpr double  kPreview = 6.0;  // seconds the menu's preview shows
		constexpr float   kPi = std::numbers::pi_v<float>;
		std::atomic<bool> gPreviewAsked{ false };

		struct Shade
		{
			float r{ 1.0f }, g{ 1.0f }, b{ 1.0f };
		};

		// a color at full strength: its brightest part 1, so a dim light color still reads on the bar
		[[nodiscard]] Shade Bright(float a_r, float a_g, float a_b)
		{
			const float top = (std::max)({ a_r, a_g, a_b });
			return top > 0.0f ? Shade{ a_r / top, a_g / top, a_b / top } : Shade{};
		}
		[[nodiscard]] Shade ShadeOf(Core::Rgb a_c) { return Bright(a_c.r, a_c.g, a_c.b); }
		[[nodiscard]] Shade ShadeOf(std::uint32_t a_rgb)
		{
			return { static_cast<float>((a_rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((a_rgb >> 8) & 0xFF) / 255.0f, static_cast<float>(a_rgb & 0xFF) / 255.0f };
		}

		[[nodiscard]] ImGuiMCP::ImU32 Col(Shade a_s, float a_alpha, float a_bright = 1.0f)
		{
			const auto c = [](float v) { return static_cast<ImGuiMCP::ImU32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
			return c(a_s.r * a_bright) | (c(a_s.g * a_bright) << 8) | (c(a_s.b * a_bright) << 16) | (c(a_alpha) << 24);
		}

		[[nodiscard]] float Ease(float a_u) { return 1.0f - std::pow(1.0f - std::clamp(a_u, 0.0f, 1.0f), 3.0f); }

		// a soft glow: rings from wide and faint to small and bright
		void Glow(ImGuiMCP::ImDrawList* a_dl, ImVec2 a_c, float a_r, Shade a_s, float a_alpha)
		{
			for (int i = 0; i < 8; ++i) {
				ImGuiMCP::ImDrawListManager::AddCircleFilled(a_dl, a_c, a_r * (1.0f - static_cast<float>(i) * 0.11f), Col(a_s, a_alpha * (0.05f + static_cast<float>(i) * 0.012f)), 24);
			}
		}

		// a spell's sigil: a ring in its color with its element's mark inside (Crossfire's)
		void Sigil(ImGuiMCP::ImDrawList* a_dl, ImVec2 a_c, float a_r, std::uint8_t a_element, Shade a_s, float a_alpha)
		{
			namespace M = ImGuiMCP::ImDrawListManager;
			const auto  c = Col(a_s, a_alpha);
			const float t = (std::max)(1.0f, a_r * 0.16f);
			const auto  at = [&](float x, float y) { return ImVec2{ a_c.x + x * a_r, a_c.y + y * a_r }; };
			M::AddCircleFilled(a_dl, a_c, a_r, Col({ 0.0f, 0.0f, 0.0f }, 0.62f * a_alpha), 32);
			switch (static_cast<Core::Element>(a_element)) {
			case Core::Element::kFire:  // a flame: a tongue leaning right, a smaller one off its left
				M::AddCircleFilled(a_dl, at(0.0f, 0.24f), a_r * 0.32f, c, 16);
				M::AddTriangleFilled(a_dl, at(-0.32f, 0.2f), at(0.1f, -0.66f), at(0.32f, 0.2f), c);
				M::AddTriangleFilled(a_dl, at(-0.34f, 0.22f), at(-0.36f, -0.28f), at(-0.08f, 0.05f), c);
				break;
			case Core::Element::kFrost:  // a snowflake: three crossed strokes
				for (int i = 0; i < 3; ++i) {
					const float ang = 1.5708f + static_cast<float>(i) * 1.0472f;
					M::AddLine(a_dl, at(std::cos(ang) * 0.6f, -std::sin(ang) * 0.6f), at(-std::cos(ang) * 0.6f, std::sin(ang) * 0.6f), c, t);
				}
				break;
			case Core::Element::kShock:
				{  // a bolt
					const ImVec2 bolt[]{ at(0.15f, -0.62f), at(-0.25f, 0.05f), at(0.08f, 0.05f), at(-0.15f, 0.62f) };
					M::AddPolyline(a_dl, bolt, 4, c, 0, t * 1.2f);
					break;
				}
			case Core::Element::kPoison:  // a drop
				M::AddCircleFilled(a_dl, at(0.0f, 0.2f), a_r * 0.32f, c, 16);
				M::AddTriangleFilled(a_dl, at(-0.3f, 0.12f), at(0.0f, -0.58f), at(0.3f, 0.12f), c);
				break;
			case Core::Element::kForce:  // a ring inside the ring
				M::AddCircle(a_dl, a_c, a_r * 0.45f, c, 24, t);
				M::AddCircleFilled(a_dl, a_c, a_r * 0.15f, c, 12);
				break;
			case Core::Element::kPhysical:  // a blade's point
				M::AddQuadFilled(a_dl, at(0.0f, -0.6f), at(0.22f, 0.0f), at(0.0f, 0.6f), at(-0.22f, 0.0f), c);
				break;
			default:  // anything else: a four-point star
				M::AddQuadFilled(a_dl, at(0.0f, -0.6f), at(0.14f, 0.0f), at(0.0f, 0.6f), at(-0.14f, 0.0f), c);
				M::AddQuadFilled(a_dl, at(-0.6f, 0.0f), at(0.0f, 0.14f), at(0.6f, 0.0f), at(0.0f, -0.14f), c);
				break;
			}
		}

		// a stream from a_from to the knot at a_to: dim at its end, brightest at the knot, streaks riding toward it
		void DrawStream(ImGuiMCP::ImDrawList* a_dl, float a_from, float a_to, float a_y, float a_half, Shade a_s, float a_alpha, double a_time, float a_speed)
		{
			namespace M = ImGuiMCP::ImDrawListManager;
			const bool  left = a_from < a_to;
			const float x0 = (std::min)(a_from, a_to), x1 = (std::max)(a_from, a_to);
			const auto  dim = Col(a_s, a_alpha, 0.45f), full = Col(a_s, a_alpha);
			M::AddRectFilledMultiColor(a_dl, ImVec2{ x0, a_y - a_half }, ImVec2{ x1, a_y + a_half }, left ? dim : full, left ? full : dim, left ? full : dim, left ? dim : full);
			const float len = x1 - x0;
			if (len < 2.0f) {
				return;
			}
			const Shade hot{ (std::min)(a_s.r * 1.4f + 0.2f, 1.0f), (std::min)(a_s.g * 1.4f + 0.2f, 1.0f), (std::min)(a_s.b * 1.4f + 0.2f, 1.0f) };
			for (int i = 0; i < 7; ++i) {
				const float u = static_cast<float>(std::fmod(a_time * a_speed / static_cast<double>(len) + i / 7.0, 1.0));
				const float x = left ? x0 + len * u : x1 - len * u;
				const float w = 16.0f, h = a_half - 1.0f;
				const auto  peak = Col(hot, a_alpha * 0.35f * (0.4f + 0.6f * u)), none = Col(hot, 0.0f);
				M::AddRectFilledMultiColor(a_dl, ImVec2{ (std::max)(x - w, x0), a_y - h }, ImVec2{ x, a_y + h }, none, peak, peak, none);
				M::AddRectFilledMultiColor(a_dl, ImVec2{ x, a_y - h }, ImVec2{ (std::min)(x + w, x1), a_y + h }, peak, none, none, peak);
			}
			// a white-hot core toward the knot
			const auto  core = Col({ 1.0f, 0.97f, 0.9f }, a_alpha * 0.6f), clear = Col({ 1.0f, 0.97f, 0.9f }, 0.0f);
			const float mid = left ? x1 - len * 0.35f : x0 + len * 0.35f;
			M::AddRectFilledMultiColor(a_dl, ImVec2{ (std::min)(mid, a_to), a_y - 1.0f }, ImVec2{ (std::max)(mid, a_to), a_y + 1.0f }, left ? clear : core,
				left ? core : clear, left ? core : clear, left ? clear : core);
		}

		void DrawBar()
		{
			using namespace ImGuiMCP;
			namespace M = ImGuiMCP::ImDrawListManager;
			static float  alpha = 0.0f;
			static double previewFrom = -100.0;
			if (gPreviewAsked.exchange(false)) {
				previewFrom = GetTime();
			}
			auto         v = StruggleNow();
			auto         me = Plugin::PlayerView();
			const double sinceAsked = GetTime() - previewFrom;
			const bool   preview = !v.active && sinceAsked >= 0.0 && sinceAsked < kPreview;
			if (preview) {
				// the menu's sample: a Flames against Frostbite lock, your Overdrive filling, released on the pulse at 2.4 s
				const auto  cfg = MenuCopy().struggle;
				const auto  t = static_cast<float>(sinceAsked);
				const float release = 2.0f * Core::kKnotBeat;
				v.active = true;
				v.bar = true;
				v.barHeight = cfg.barHeight;
				v.barScale = cfg.barScale;
				v.barOpacity = cfg.barOpacity;
				v.barNames = cfg.barNames;
				v.barSkills = cfg.barSkills;
				v.seconds = t;
				v.mine = static_cast<std::uint8_t>(Core::Element::kFire);
				v.theirs = static_cast<std::uint8_t>(Core::Element::kFrost);
				v.myColor = { 255, 133, 36 };
				v.theirColor = { 115, 194, 255 };
				v.balance = 0.04f * std::sin(t * 1.7f) + (t >= release ? 0.4f * Ease((t - release) / 0.35f) : 0.0f);
				v.shoveAge = t >= release ? release : -1.0f;
				v.shoveOnBeat = true;
				v.mySkill = 82;
				v.theirSkill = 64;
				std::snprintf(v.school, sizeof(v.school), "%s", "Destruction");
				std::snprintf(v.theirSchool, sizeof(v.theirSchool), "%s", "Destruction");
				std::snprintf(v.foe, sizeof(v.foe), "%s", T("Bandit Mage"));
				me.charge = t < release ? 25.0f + 75.0f * t / release : (std::max)(100.0f - (t - release) * 100.0f / 6.0f, 0.0f);
				me.state = t < release ? Heat::Meter::State::kBuilding : Heat::Meter::State::kOverdrive;
				me.ready = t >= release - 0.4f && t < release;
				me.stage = Heat::Stage::kHot;
			}
			// the last lock drawn is kept: the bar fades out with it, not with an empty view
			const bool          live = v.active;
			static StruggleView held;
			if (live) {
				held = v;
			} else {
				v = held;
			}
			auto* io = GetIO();
			auto* ui = RE::UI::GetSingleton();
			if (!io) {
				return;
			}
			// out of the way in a pausing menu, unless the menu asked for the preview
			const bool  show = live && v.bar && (preview || !ui || !ui->GameIsPaused());
			const float dt = std::isfinite(io->DeltaTime) ? std::clamp(io->DeltaTime, 0.0f, 0.25f) : 0.0f;
			alpha = std::clamp(alpha + (show ? dt : -dt) * 4.0f, 0.0f, 1.0f);
			auto* dl = alpha > 0.0f ? GetForegroundDrawList() : nullptr;
			if (!dl) {
				return;
			}
			const float  k = std::clamp(v.barScale, 0.5f, 2.0f);
			const float  a = std::clamp(v.barOpacity, 0.1f, 1.0f) * alpha;
			const float  w = 440.0f * k, half = 4.0f * k;
			const float  cx = io->DisplaySize.x * 0.5f, yc = io->DisplaySize.y * std::clamp(v.barHeight, 0.0f, 100.0f) / 100.0f;
			const float  x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
			const float  split = x0 + w * std::clamp((1.0f + v.balance) * 0.5f, 0.0f, 1.0f);
			const Shade  bone{ 0.86f, 0.82f, 0.73f }, cream{ 0.96f, 0.93f, 0.82f }, black{ 0.0f, 0.0f, 0.0f };
			const Shade  mine = ShadeOf(v.myColor), theirs = ShadeOf(v.theirColor);
			const double now = GetTime();
			// your release: how long ago, and your stream swelling while the Overdrive lasts
			const float since = v.shoveAge >= 0.0f ? v.seconds - v.shoveAge : 1e6f;
			const bool  overdrive = me.state == Heat::Meter::State::kOverdrive;
			const float swell = overdrive && since < 1e5f ? Ease(since / 0.25f) : 0.0f;

			// the frame: a dark slot between two thin bone lines that fade out toward the ends, as the compass's frame does
			M::AddRectFilled(dl, ImVec2{ x0 - 2.0f, yc - half - 2.0f }, ImVec2{ x1 + 2.0f, yc + half + 2.0f }, Col(black, 0.62f * a), 2.0f, 0);
			for (const float y : { yc - half - 3.0f * k, yc + half + 3.0f * k }) {
				const auto solid = Col(bone, 0.55f * a), none = Col(bone, 0.0f);
				M::AddRectFilledMultiColor(dl, ImVec2{ x0 - 30.0f * k, y - 0.5f }, ImVec2{ x0 + 40.0f * k, y + 0.5f }, none, solid, solid, none);
				M::AddRectFilled(dl, ImVec2{ x0 + 40.0f * k, y - 0.5f }, ImVec2{ x1 - 40.0f * k, y + 0.5f }, solid, 0.0f, 0);
				M::AddRectFilledMultiColor(dl, ImVec2{ x1 - 40.0f * k, y - 0.5f }, ImVec2{ x1 + 30.0f * k, y + 0.5f }, solid, none, none, solid);
			}
			// the two streams; yours swells in your Overdrive and its magicka runs faster
			DrawStream(dl, x0, split, yc, half + half * 0.9f * swell, mine, a, now, 70.0f * k * (1.0f + 0.6f * swell));
			DrawStream(dl, x1, split, yc, half, theirs, a, now, 70.0f * k);
			// the ends: a tick and a small outward chevron, as on the health and magicka meters
			for (const auto [x, s] : { std::pair{ x0, 1.0f }, std::pair{ x1, -1.0f } }) {
				M::AddLine(dl, ImVec2{ x, yc - half - 6.0f * k }, ImVec2{ x, yc + half + 6.0f * k }, Col(bone, 0.9f * a), (std::max)(1.0f, k));
				M::AddTriangleFilled(dl, ImVec2{ x - s * 11.0f * k, yc }, ImVec2{ x - s * 3.0f * k, yc - 5.0f * k }, ImVec2{ x - s * 3.0f * k, yc + 5.0f * k }, Col(bone, 0.9f * a));
			}
			// the knot: a glow that breathes on the beat, a ring thrown out at each pulse, the compass's diamond
			const float beat = std::fmod((std::max)(v.seconds, 0.0f), Core::kKnotBeat) / Core::kKnotBeat;
			const float pulse = std::exp(-std::pow((std::min)(beat, 1.0f - beat) / 0.08f, 2.0f));
			Glow(dl, ImVec2{ split, yc }, (24.0f + 8.0f * pulse) * k, cream, a * (0.6f + 0.8f * pulse));
			M::AddCircle(dl, ImVec2{ split, yc }, (8.0f + 26.0f * beat) * k, Col(cream, 0.7f * a * std::pow(1.0f - beat, 1.5f)), 32, 2.0f * k);
			const float r = (7.0f + 1.5f * pulse) * k;
			M::AddQuadFilled(dl, ImVec2{ split, yc - r }, ImVec2{ split + r, yc }, ImVec2{ split, yc + r }, ImVec2{ split - r, yc }, Col(cream, a));
			M::AddQuad(dl, ImVec2{ split, yc - r }, ImVec2{ split + r, yc }, ImVec2{ split, yc + r }, ImVec2{ split - r, yc }, Col({ 0.16f, 0.13f, 0.10f }, a), 1.0f);
			// the release: a flash at the knot (bigger on the beat) and a wave along your stream
			if (since < 0.45f) {
				const float f = 1.0f - since / 0.45f;
				Glow(dl, ImVec2{ split, yc }, (30.0f + 40.0f * (1.0f - f)) * k * (v.shoveOnBeat ? 1.3f : 1.0f), { 1.0f, 0.95f, 0.85f }, a * f * (v.shoveOnBeat ? 3.0f : 2.0f));
			}
			if (since < 0.3f) {
				const float wx = x0 + (split - x0) * since / 0.3f;
				const auto  peak = Col({ 1.0f, 0.85f, 0.6f }, 0.9f * a * (1.0f - since / 0.3f)), none = Col({ 1.0f, 0.85f, 0.6f }, 0.0f);
				M::AddRectFilledMultiColor(dl, ImVec2{ wx - 10.0f * k, yc - 10.0f * k }, ImVec2{ wx, yc + 10.0f * k }, none, peak, peak, none);
				M::AddRectFilledMultiColor(dl, ImVec2{ wx, yc - 10.0f * k }, ImVec2{ wx + 10.0f * k, yc + 10.0f * k }, peak, none, none, peak);
			}

			// your end: your spell's sigil inside your Overdrive bar - a ring filling clockwise from the top in the Heat's color
			const ImVec2 ring{ x0 - 34.0f * k, yc };
			Sigil(dl, ring, 12.0f * k, v.mine, mine, a);
			const Shade heat = ShadeOf(Plugin::HeatColor(me));
			const float charge = std::clamp(me.charge / Heat::kFull, 0.0f, 1.0f);
			M::AddCircle(dl, ring, 16.0f * k, Col({ 0.10f, 0.08f, 0.06f }, 0.8f * a), 40, 4.0f * k);
			if (charge > 0.0f) {
				M::PathClear(dl);
				M::PathArcTo(dl, ring, 16.0f * k, -0.5f * kPi, -0.5f * kPi + 2.0f * kPi * charge, 40);
				M::PathStroke(dl, Col(heat, a), 0, 3.0f * k);
			}
			if (charge >= 0.999f || me.ready) {
				// full: the ring glows and embers circle it
				M::AddCircle(dl, ring, 16.0f * k, Col(heat, a * (0.35f + 0.25f * pulse)), 40, 7.0f * k);
				for (int i = 0; i < 3; ++i) {
					const float  ang = static_cast<float>(now) * 2.4f + static_cast<float>(i) * 2.094f;
					const ImVec2 at{ ring.x + std::sin(ang) * 21.0f * k, ring.y - std::cos(ang) * 21.0f * k };
					Glow(dl, at, 3.0f * k, heat, a * 2.0f);
				}
			}
			// their end: their sigil in a plain bone ring
			const ImVec2 foe{ x1 + 34.0f * k, yc };
			Sigil(dl, foe, 12.0f * k, v.theirs, theirs, a);
			M::AddCircle(dl, foe, 16.0f * k, Col(bone, 0.7f * a), 40, 1.5f * k);

			// names over the bar and skills under it, each only when asked for: capitals, with a drop shadow
			auto*       font = GetFont();
			const float fs = GetFontSize();
			const auto  label = [&](std::string a_text, float a_size, float a_y, bool a_right, Shade a_s) {
				for (auto& ch : a_text) {
					ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
				}
				const float tw = CalcTextSize(a_text.c_str()).x * a_size / fs;
				const float x = a_right ? x1 - tw : x0;
				M::AddText(dl, font, a_size, ImVec2{ x + 1.0f, a_y + 1.0f }, Col(black, 0.8f * a), a_text.c_str());
				M::AddText(dl, font, a_size, ImVec2{ x, a_y }, Col(a_s, a), a_text.c_str());
			};
			if (v.barNames) {
				const float size = fs * 1.15f * k;
				label(T("You"), size, yc - half - 9.0f * k - size, false, { 0.91f, 0.88f, 0.80f });
				label(v.foe, size, yc - half - 9.0f * k - size, true, { 0.91f, 0.88f, 0.80f });
			}
			if (v.barSkills) {
				const float size = fs * 0.85f * k;
				label(std::format("{} {}", T(v.school), v.mySkill), size, yc + half + 8.0f * k, false, { 0.75f, 0.71f, 0.63f });
				label(std::format("{} {}", T(v.theirSchool), v.theirSkill), size, yc + half + 8.0f * k, true, { 0.75f, 0.71f, 0.63f });
			}
		}

		void __stdcall Render() { Safe::Guarded("the Clashes page", DrawClashes); }
		void __stdcall RenderStruggles() { Safe::Guarded("the Spell struggles page", DrawStruggles); }
		void __stdcall RenderBar() { Safe::Guarded("the struggle bar", DrawBar); }
	}

	void RequestBarPreview() { gPreviewAsked = true; }

	void RegisterMenu()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			return;
		}
		SKSEMenuFramework::AddSectionItem(T("Clashes"), Render);
		SKSEMenuFramework::AddSectionItem(T("Spell struggles"), RenderStruggles);
		SKSEMenuFramework::AddHudElement(RenderBar);
	}
}

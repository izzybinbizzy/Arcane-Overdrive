// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Two pages in Arcane Overdrive's section of SKSE Menu Framework: Clashes and Spell struggles, each with SWITCHES only -
// every number lives in SpellMasteryHeat Clashes.ini, which the menu writes whole. The menu draws off the game's main
// thread, so it works on a copy of the settings (MenuCopy) and hands each change over as a task on the main thread
// (Submit). The reaction table is shown, not edited: it lives in SpellMasteryHeat Clash Rules.ini, which a patch can add
// to. No struggle bar here: the struggle bar comes with the high-res bars.

#include "ClashShared.h"

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

		// A change goes to the game at once and is written to the file
		struct Edit
		{
			Core::Config cfg{ MenuCopy() };
			bool         changed{ false };

			void After(bool a_changed) { changed |= a_changed; }
			Edit() = default;
			Edit(const Edit&) = delete;
			Edit& operator=(const Edit&) = delete;
			~Edit()
			{
				if (changed) {
					Submit(cfg, true);
				}
			}
		};

		void Check(Edit& a_e, const char* a_label, bool& a_value, const char* a_tip)
		{
			a_e.After(ImGuiMCP::Checkbox(T(a_label), &a_value));
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
			EndTable();
		}

		void __stdcall Render()
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
			{
				int catchPct = static_cast<int>(std::lround(std::clamp(c.headOnCatch, 0.0f, 300.0f)));
				if (ImGuiMCP::SliderInt(T("Head-on catch"), &catchPct, 0, 300, "%d%%")) {
					c.headOnCatch = static_cast<float>(catchPct);
					e.After(true);
				}
				ImGuiMCP::SetItemTooltip("%s", T("How far apart two bolts flying head on at each other still meet. 100% is the normal reach; "
												 "lower, they must pass closer; higher, a wider miss still clashes. 0% turns it off."));
			}

			Heading(Icon::kFire, "Effects");
			Check(e, "Bursts", c.explosions, "A destroyed projectile bursts where it was hit, with its own explosion.");
			layout.Beside();
			ImGuiMCP::BeginDisabled(!c.explosions);
			Check(e, "Only harmless bursts", c.safeExplosionsOnly, "Skip a burst that would do more than show: damage, an enchantment, something it spawns.");
			Check(e, "Stand-in bursts", c.standInBursts, "A spell with no explosion of its own (Firebolt, Ice Spike, Lightning Bolt) bursts the way its element's spells do.");
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

		void __stdcall RenderStruggles()
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
	}

	void RegisterMenu()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			return;
		}
		SKSEMenuFramework::AddSectionItem(T("Clashes"), Render);
		SKSEMenuFramework::AddSectionItem(T("Spell struggles"), RenderStruggles);
	}
}

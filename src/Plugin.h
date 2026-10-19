// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// What the files share. The file map is at the top of main.cpp.

#pragma once

#include "Heat.h"
#include "Scaled.h"
#include "SettingsText.h"
#include "SpellLook.h"

namespace Plugin
{
	// ------------------------------------------------------------------ Settings.cpp: the settings (SettingsText.h)
	// The one shared copy lives in Settings.cpp behind a lock: the menu (render thread), DevBench (its own thread) and the
	// key capture (the input thread) change it while the main thread reads it every frame, so everyone works on a copy.
	[[nodiscard]] Settings Config();
	void                   SetConfig(const Settings& a_settings);
	bool                   ApplySetting(std::string_view a_key, int a_value);
	void                   LoadSettings();
	void                   SaveSettings();

	// ------------------------------------------------------------------ Spells.cpp: what a spell is to the meter
	struct SpellInfo
	{
		Heat::School     school{ Heat::School::kNone };
		Heat::SpellFacts facts{};
		bool             held{ false };  // a concentration spell
		int              element{ 0 };   // 0 none, 1 fire, 2 frost, 3 shock: the hand glow's color
	};
	[[nodiscard]] SpellInfo Read(const RE::MagicItem* a_item);
	// whether this effect's magnitude is boosted, by the Boost setting (damage only, or every hostile effect)
	[[nodiscard]] bool Boosted(const RE::EffectSetting* a_effect, Heat::Boost a_boost);

	// ------------------------------------------------------------------ Meters.cpp: the meters (the player's, and enemies')
	void                OnCast(RE::Actor* a_caster, const RE::MagicItem* a_item);                                         // any thread: the game's spell cast event
	void                Frame(float a_delta);                                                                             // main thread, every frame, from the player's update
	[[nodiscard]] float Multiplier(RE::Actor* a_caster, const RE::MagicItem* a_item, const RE::EffectSetting* a_effect);  // any thread
	void                ResetAll();                                                                                       // a game is loading
	void                PressOverdriveKey();                                                                              // any thread: the key went down (acted on next frame, if the game is not in a menu)
	// the bench (DevBench, the Debug page): the player's meter, on the next frame
	void BenchSetHeat(float a_heat);
	void BenchForceStage(Heat::Stage a_stage);

	struct MeterView
	{
		float                heat{ 0.0f };
		Heat::Stage          stage{ Heat::Stage::kCold };
		Heat::Meter::State   state{ Heat::Meter::State::kBuilding };
		float                timer{ 0.0f }, timerOf{ 1.0f };  // Overdrive's or the cooldown's seconds left, and of how many
		bool                 ready{ false };
		Heat::School         last{ Heat::School::kNone };
		float                bonus{ 1.0f };                      // a spell of the last school's multiplier now
		float                regen{ 1.0f };                      // magicka regeneration now, against normal
		std::size_t          enemies{ 0 };                       // enemies with a meter of their own
		std::uint64_t        boosted{ 0 };                       // effects boosted since the game started
		int                  element{ 0 };                       // the last counted spell's element (0 none, 1 fire, 2 frost, 3 shock): the flames' tint
		SpellLook::Look      look{ SpellLook::Look::kGeneric };  // the HUD look: the last spell that built heat, else the one in hand
		const RE::MagicItem* spell{ nullptr };                   // the spell that look is of (its own art feeds the spell bar - SpellArt.cpp)
	};
	[[nodiscard]] MeterView PlayerView();  // a copy, from any thread

	// ------------------------------------------------------------------ Looks.cpp: which HUD look a spell gets (SpellLook.h)
	[[nodiscard]] SpellLook::Look LookOf(const RE::MagicItem* a_item);  // from the spell's own data, never its name
	// a held spell that builds heat, being cast now (the right hand first); none: nothing held
	[[nodiscard]] const RE::MagicItem* HeldSpell(RE::Actor* a_actor, const Settings& a_settings);
	[[nodiscard]] const RE::MagicItem* EquippedSpell(RE::PlayerCharacter* a_player);  // the spell in the right hand, else the left

	// ------------------------------------------------------------------ SpellArt.cpp: the spell's own art, for the spell bars
	struct SpellArtView
	{
		std::uint32_t                     form{ 0 };         // the spell
		void*                             body{ nullptr };   // its particle texture (the game's own texture view, which the HUD draws with), or none
		void*                             fill{ nullptr };   // its other effect texture, the bar's fill, or none
		std::vector<std::array<float, 4>> frames;            // the body texture's flipbook cells (u0 v0 u1 v1), none: the whole texture
		bool                              colored{ false };  // the color below is the spell's own (its light's, else its art's)
		float                             r{ 1.0f }, g{ 1.0f }, b{ 1.0f };
		std::string                       bodyPath, fillPath;
	};
	void                        NoteSpellArt(const RE::MagicItem* a_item);  // main thread, every frame (read once per spell)
	void                        ForgetSpellArt();                           // a game is loading
	[[nodiscard]] SpellArtView  SpellArtNow();                              // a copy, from any thread
	[[nodiscard]] std::uint32_t SpellArtVersion();                          // changes with every new view (the HUD copies one only then)

	// ------------------------------------------------------------------ HudBars.cpp: the live spell bars (BarFx.h)
	struct LookStats
	{
		int   shown{ -1 };                             // the look last drawn (SpellLook::Look), -1 none yet
		int   forced{ -1 };                            // the bench's choice, -1 the spell's own
		bool  spellBody{ false }, spellFill{ false };  // drawn with the spell's own textures (else the built-in ones)
		bool  sprites{ false };                        // the built-in sprite sheet and fill are loaded
		int   particles{ 0 }, arcs{ 0 };               // alive in the last frame drawn
		float drawUsAvg{ 0.0f }, drawUsMax{ 0.0f };    // our part of a frame on the render thread, microseconds
		// the blend measurement (HudBars.cpp): callbacks run since the game started, our blend state still bound at the end of
		// the added layers, and the render target the HUD is drawn into
		std::uint32_t addCalls{ 0 }, probeCalls{ 0 }, keptAdd{ 0 };
		bool          additiveMade{ false };
		std::uint32_t rtWidth{ 0 }, rtHeight{ 0 }, rtFormat{ 0 };
		int           rtIsBack{ -1 };  // 1 the swap chain's back buffer, 0 another target, -1 not read, -2 none bound
	};
	[[nodiscard]] LookStats LookStatsNow();
	void                    BenchForceLook(int a_look);  // the Debug page and DevBench: show this look whatever the spell (-1: the spell's own)

	// ------------------------------------------------------------------ Overload.cpp: the overcharge on the spells themselves
	void                UpdateOverload(const MeterView& a_v, const Settings& a_s, float a_dt);  // main thread, every frame, before the hands
	void                ReleaseOverload();                                                      // every art and projectile back as it was found
	[[nodiscard]] float HandLightFactor(std::size_t a_hand);                                    // 1, or a dim ember while that hand's spell sputters out
	struct OverloadView
	{
		float       power{ 1.0f };                          // the spells' size now (Overload::Power, eased)
		std::size_t arts{ 0 }, projectiles{ 0 }, out{ 0 };  // casting arts and projectiles ours this frame, hands sputtered out
	};
	[[nodiscard]] OverloadView OverloadNow();

	// ------------------------------------------------------------------ Marks.cpp: the scorch and frost marks (Marks.h)
	void UpdateMarks(const MeterView& a_v, const Settings& a_s, float a_dt);  // main thread, every frame
	void ReleaseMarks();                                                      // every mark and its look ended (switched off, a game loading)
	struct MarksView
	{
		std::uint32_t alive{ 0 }, made{ 0 }, hurt{ 0 };  // marks alive now, made and hurts dealt since the game started
	};
	[[nodiscard]] MarksView MarksNow();

	// ------------------------------------------------------------------ Hands.cpp: the lights on the casting hands
	void UpdateHands(Heat::Stage a_left, Heat::Stage a_right, const Settings& a_settings);  // main thread
	void ReleaseHands();                                                                    // every light back as we found it, our own taken down
	struct HandsView
	{
		std::size_t scaled{ 0 }, frozen{ 0 }, own{ 0 };
		float       factor[2]{ 1.0f, 1.0f };
	};
	[[nodiscard]] HandsView HandsNow();

	// ------------------------------------------------------------------ main.cpp: the Overdrive key, captured from the menu
	void               CaptureKey(bool a_on);
	[[nodiscard]] bool CapturingKey();

	// ------------------------------------------------------------------ Arcs.cpp: the heat meter as a Reticle Arcs bar
	void               RegisterArcBar();        // SKSE's post-load: Reticle Arcs takes add-on bars only until data load
	[[nodiscard]] bool ArcBarTaken() noexcept;  // Reticle Arcs took it (the switch decides whether it shows)

	// ------------------------------------------------------------------ Menu.cpp
	void RegisterMenu();

	// ------------------------------------------------------------------ DevBench.cpp
	void OfferToDevBench();
}

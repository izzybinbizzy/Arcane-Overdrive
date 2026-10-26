// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, either version 3 of the License,
// or (at your option) any later version. See LICENSE.txt.
//
// Two bars. Casting fills the Overdrive bar (faster in one school; switching school costs some; it drains a few seconds
// after your last cast). Casting FAST builds Heat, which cools when you ease off: its stages - Warm, Hot, Overheating -
// each give your spells a little more power and the lights on your casting hands a little more reach. A full Overdrive
// bar goes when Heat lets it (by default only in the sweet spot: too cold does nothing, too hot misfires): a short burst
// at the top bonus, then a cooldown with slower magicka. It needs no plugin (.esp) and no script, and writes nothing
// into the save.
//
// THE FILES, AND WHAT EACH ONE IS FOR
//   main.cpp        this file - the hooks (the player's update; each effect as it lands), the spell cast event, the
//                   Overdrive key, and the order things start in
//   Heat.h          the two bars as plain numbers: the charge, the heat, its stages and gate, misfires, Overdrive, the
//                   cooldown, penalties, which spells count - tests/test_heat.cpp
//   Scaled.h        the light scaler of the fading module Illuminated and RELight - Spell Addon share (FadeGlow.h
//                   Glow::Scaled), copied unchanged - tests/test_heat.cpp
//   IniText.h       a settings file's lines as plain text (the same file in each of these plugins) - tests/test_heat.cpp
//   SettingsText.h  the settings, their keys in the two files, the three presets, and key names - tests/test_heat.cpp
//   Settings.cpp    the settings files, Data\SKSE\Plugins\SpellMasteryHeat.ini (the menu's) and SpellMasteryHeat
//                   Advanced.ini (every number), and the one shared copy
//   Spells.cpp      what a spell is to the meter (school, kind, held, element) and which effects are boosted
//   Meters.cpp      the bars: casts, held spells, the key, the clocks, slow magicka, a misfire's backlash, the bonus
//   Hands.cpp       the lights on the casting hands, scaled (and the optional hand glow of its own)
//   Menu.cpp        the settings and debug pages in SKSE Menu Framework
//   TrueHud.cpp     the Overdrive bar as TrueHUD's special bar, colored by the Heat
//   Arcs.cpp        both bars as Reticle Arcs arcs round the crosshair
//   Overload.h      the overcharge on the spells as plain numbers: growth with the bar, the red zone, the focus, the sputter - tests
//   Overload.cpp    puts them on the casting art in the hands, the player's projectiles, explosions, hazards and hit effects
//   Marks.h         the scorch / frost marks as plain numbers: the cap, the refresh, the lifetime, the hurt ticks - tests
//   Marks.cpp       finds the Overheating impacts, shows each mark with a vanilla hazard's model, hurts who stands in it
//   DevBench.cpp    what DevBench can read and do
//   Clash*          the clashes and spell struggles (Crossfire's code): ClashCore.* the game-free core - tests/test_clash.cpp;
//                   ClashPass.cpp the pass once a frame; ClashStruggle.cpp locked streams; ClashClassify.cpp what a projectile
//                   is; ClashAim.cpp auto lock; ClashSettings.cpp the files; ClashMenu.cpp its two pages; ClashDevBench.cpp
//   Plugin.h        what they share      PCH.h  what they all include
//   MenuStyle.h     the menu look every plugin of ours shares (Dynamic Wards' copy is the master)
//   DevBenchAPI.*, SKSEMenuFramework.h, TrueDirectionalMovementAPI.h, TrueHUDAPI.h, ReticleArcsAPI.h  other authors'
//                   headers, copied unchanged (their licenses beside them; TDM's and TrueHUD's are GPL-3.0, as this plugin)

#include "Plugin.h"
#include "ClashShared.h"
#include "Translation.h"

namespace
{
	// ------------------------------------------------------------------ every frame: the player's update
	struct PlayerUpdate
	{
		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			func(a_this, a_delta);
			Plugin::Frame(a_delta);
			Crossfire::Update(a_delta);  // the clashes and spell struggles (ClashPass.cpp)
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
			func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), thunk);  // Actor::Update: 0xAD SE/AE, 0xAF VR
		}
	};

	// ------------------------------------------------------------------ each new effect, as it takes its perks
	// The damage bonus: ActiveEffect::AdjustForPerks (slot 0x00 of each effect class's table) is where the game sets a new
	// effect's final magnitude from the caster's perks, before the effect starts; the caster's bonus multiplies it right
	// after. MagicTarget::AddTarget's magnitude and power were tried first and measured to change nothing (a Firebolt hit the
	// same with either raised). The value-modifier classes are the ones that damage or drain.
	template <class T>
	struct PerksAdjusted
	{
		static void thunk(T* a_this, RE::Actor* a_caster, RE::MagicTarget* a_target)
		{
			func(a_this, a_caster, a_target);
			if (a_caster && a_this->spell && a_this->effect && a_this->effect->baseEffect && std::isfinite(a_this->magnitude)) {
				const float k = Plugin::Multiplier(a_caster, a_this->spell, a_this->effect->baseEffect);
				if (k != 1.0f) {
					a_this->magnitude *= k;
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
			func = vtbl.write_vfunc(0x00, thunk);
		}
	};

	// ------------------------------------------------------------------ each explosion and hazard as it is made
	// Initialize (SE/AE 0xA2, VR 0xA3) runs once as one is made: it is noted, and Overload.cpp grows the player's to the size
	// their spells had then
	template <class T>
	struct Made
	{
		static void thunk(T* a_this)
		{
			func(a_this);
			Plugin::NoteSpawned(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[0] };
			func = vtbl.write_vfunc(REL::Relocate(0xA2, 0xA2, 0xA3), thunk);
		}
	};

	// ------------------------------------------------------------------ the game's spell cast event
	class CastSink final : public RE::BSTEventSink<RE::TESSpellCastEvent>
	{
	public:
		static CastSink* Get()
		{
			static CastSink sink;
			return &sink;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
		{
			if (a_event && a_event->object) {
				if (auto* actor = a_event->object->As<RE::Actor>()) {
					Plugin::OnCast(actor, RE::TESForm::LookupByID<RE::MagicItem>(a_event->spell));
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ------------------------------------------------------------------ the Overdrive key
	// Any key: keyboard keys by their DirectInput scan code, mouse buttons from 256, gamepad buttons 266-281 (the
	// numbering SKSE plugins share - SettingsText::GamepadCode). While the menu waits for a key ("Set the key"), the next key pressed becomes it
	// (Esc cancels); otherwise the set key starts Overdrive when the meter is ready.
	std::atomic<bool> gCapture{ false };

	int CodeOf(const RE::ButtonEvent& a_button)
	{
		const auto id = a_button.GetIDCode();
		switch (a_button.GetDevice()) {
		case RE::INPUT_DEVICE::kKeyboard:
			return static_cast<int>(id);
		case RE::INPUT_DEVICE::kMouse:
			return 256 + static_cast<int>(id);
		case RE::INPUT_DEVICE::kGamepad:
			return Plugin::SettingsText::GamepadCode(id);
		default:
			return -1;
		}
	}

	class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static InputSink* Get()
		{
			static InputSink sink;
			return &sink;
		}
		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			for (auto* e = a_event ? *a_event : nullptr; e; e = e->next) {
				const auto* button = e->AsButtonEvent();
				if (!button || (!button->IsDown() && !button->IsUp())) {
					continue;
				}
				const int code = CodeOf(*button);
				if (code < 0) {
					continue;
				}
				if (button->IsDown() && gCapture.exchange(false)) {
					if (code != 1) {  // Esc cancels
						Plugin::ApplySetting("OverdriveKey", code);
						Plugin::SaveSettings();
						SKSE::log::info("Overdrive key set: {} ({})", Plugin::SettingsText::KeyName(code), code);
					}
					continue;
				}
				const auto s = Plugin::Config();
				if (!s.enabled || s.tuning.mode != Heat::OverdriveMode::kKey || code != s.overdriveKey) {
					continue;
				}
				Plugin::HoldOverdriveKey(button->IsDown());  // held: the focus gate
				if (button->IsDown()) {
					Plugin::PressOverdriveKey();
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// a spray's hits (FlameProjectile::AddImpact, vfunc 0xBD): while it is locked in a struggle, a hit past the meeting point
	// is dropped - the foe's spray stops where the two meet
	struct FlameAddImpact
	{
		static void thunk(RE::FlameProjectile* a_this, RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_at, const RE::NiPoint3& a_velocity,
			RE::hkpCollidable* a_collidable, std::int32_t a_arg6, std::uint32_t a_arg7)
		{
			if (Crossfire::Struggles::FlameHitPastFront(a_this, a_at)) {
				return;
			}
			func(a_this, a_ref, a_at, a_velocity, a_collidable, a_arg6, a_arg7);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// a concentration spray hurts what its target pick finds (FlameProjectile::RunTargetPick, vfunc 0xB7; measured in
	// Crossfire: its damage does not come through AddImpact). While the spray is locked the only thing past the meeting point
	// is the other caster, so a locked spray picks no target at all
	struct FlameTargetPick
	{
		static bool                                    thunk(RE::FlameProjectile* a_this) { return Crossfire::Struggles::FlameLocked(a_this) ? false : func(a_this); }
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// auto lock: a one-shot bolt (Lightning Bolt) is aimed as it picks its target (BeamProjectile::RunTargetPick, the same slot)
	struct BeamTargetPick
	{
		static bool thunk(RE::BeamProjectile* a_this)
		{
			Crossfire::LockBeam(a_this);
			return func(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void OnDataLoaded()
	{
		Plugin::LoadSettings();
		PlayerUpdate::Install();
		PerksAdjusted<RE::ValueModifierEffect>::Install();
		PerksAdjusted<RE::DualValueModifierEffect>::Install();
		PerksAdjusted<RE::AbsorbEffect>::Install();
		PerksAdjusted<RE::AccumulatingValueModifierEffect>::Install();
		PerksAdjusted<RE::PeakValueModifierEffect>::Install();
		PerksAdjusted<RE::TargetValueModifierEffect>::Install();
		Made<RE::Explosion>::Install();
		Made<RE::Hazard>::Install();
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESSpellCastEvent>(CastSink::Get());
		}
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(InputSink::Get());
		}
		SKSE::log::info("hooked the player update, each effect as it lands and each explosion / hazard as it is made; listening for spell casts and the Overdrive key");
		// the clashes and spell struggles
		Crossfire::LoadSettings();
		Crossfire::Survey();
		Crossfire::FindFearSpell();
		Crossfire::FindStandInBursts();
		Crossfire::FindClashArt();
		REL::Relocation<std::uintptr_t> flame{ RE::FlameProjectile::VTABLE[0] };
		FlameAddImpact::func = flame.write_vfunc(REL::Relocate(0xBD, 0xBD, 0xBE), FlameAddImpact::thunk);    // SE/AE 0xBD, VR 0xBE
		FlameTargetPick::func = flame.write_vfunc(REL::Relocate(0xB7, 0xB7, 0xB8), FlameTargetPick::thunk);  // SE/AE 0xB7, VR 0xB8
		REL::Relocation<std::uintptr_t> beam{ RE::BeamProjectile::VTABLE[0] };
		BeamTargetPick::func = beam.write_vfunc(REL::Relocate(0xB7, 0xB7, 0xB8), BeamTargetPick::thunk);
		SKSE::log::info("clashes: {}", Translation::Load("Data/SKSE/Plugins/SpellMasteryHeat Clash/Translation.json"));
		Plugin::RegisterMenu();
		Crossfire::RegisterMenu();  // its two pages in Arcane Overdrive's section
	}
}

namespace Plugin
{
	void CaptureKey(bool a_on) { gCapture = a_on; }
	bool CapturingKey() { return gCapture.load(); }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SKSE::log::info("Arcane Overdrive {} loading", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string());
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			Plugin::OfferToDevBench();
			Crossfire::OfferToDevBench();
			Crossfire::FindTdm();
			Plugin::FindTrueHud();
			Plugin::RegisterArcBar();  // Reticle Arcs takes add-on bars now or never
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Crossfire::Reset();  // every projectile and caster handle belongs to the world going away
			if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame) {
				break;
			}
			// every meter belongs to the world going away; hand lights go back as they were found
			Plugin::ResetAll();
			Plugin::ReleaseHands();
			Plugin::ReleaseOverload();
			Plugin::ReleaseMarks();
			Plugin::ForgetTrueHud();
			break;
		default:
			break;
		}
	});
	return true;
}

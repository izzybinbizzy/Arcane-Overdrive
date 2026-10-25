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
//   Menu.cpp        the settings and debug pages in SKSE Menu Framework, and the HUD element that draws the bars
//   SpellLook.h     which HUD look a spell gets (his list of looks per spell type) - tests/test_heat.cpp
//   Looks.cpp       a game spell read into SpellLook.h's facts
//   SpellArt.cpp    the spell's own color (its light, else its art), for the bars
//   BarShader.h     the bars' pixel shader: every spell kind's look, the frame, the Heat strip - drawn off the game by a bench
//   HudBars.*       the high-res bars on the HUD: the Overdrive bar and the Heat bar, each one quad of BarShader.h
//   Overload.h      the overcharge on the spells as plain numbers: growth in Overdrive, the burnout's sputter - tests
//   Overload.cpp    puts them on the casting art in the hands and the player's spell projectiles
//   Marks.h         the scorch / frost marks as plain numbers: the cap, the refresh, the lifetime, the hurt ticks - tests
//   Marks.cpp       finds the Overheating impacts, shows each mark with a vanilla hazard's model, hurts who stands in it
//   DevBench.cpp    what DevBench can read and do
//   Plugin.h        what they share      PCH.h  what they all include
//   MenuStyle.h     the menu look every plugin of ours shares (Dynamic Wards' copy is the master)
//   DevBenchAPI.*, SKSEMenuFramework.h  other authors' headers, copied unchanged (their licenses beside them)

#include "Plugin.h"

namespace
{
	// ------------------------------------------------------------------ every frame: the player's update
	struct PlayerUpdate
	{
		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			func(a_this, a_delta);
			Plugin::Frame(a_delta);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
			func = vtbl.write_vfunc(REL::Relocate(0xAD, 0xAD, 0xAF), thunk);  // Actor::Update: 0xAD SE/AE, 0xAF VR
		}
	};

	// ------------------------------------------------------------------ each effect, as it lands on a target
	// The damage bonus, route (a) of the design: MagicTarget::AddTarget (slot 0x01 of an actor's MagicTarget table) is
	// handed each effect of a spell as it is applied to a target, with the magnitude it will be created with
	// (AddTargetData::magnitude). That magnitude is multiplied by the caster's bonus before the game goes on. No plugin
	// file. ASSUMED, to confirm in game: (1) the MagicTarget table is the fifth of Character's and PlayerCharacter's
	// (VTABLE[4]); (2) the effect is created from this magnitude, so changing it changes the damage. Both are checked by
	// TESTING.md's first test (a firebolt's damage with Overdrive forced, against without).
	template <class T>
	struct AddTarget
	{
		static bool thunk(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
		{
			auto* caster = a_data.caster ? a_data.caster->As<RE::Actor>() : nullptr;
			if (caster && a_data.magicItem && a_data.effect && a_data.effect->baseEffect) {
				const float k = Plugin::Multiplier(caster, a_data.magicItem, a_data.effect->baseEffect);
				if (k != 1.0f && std::isfinite(a_data.magnitude)) {
					// the effect is made inside the call from the boosted magnitude; the caller's own number is put back after
					// it, so a caller that hands the same data on (the next target of an area spell) never boosts twice
					const float own = a_data.magnitude;
					a_data.magnitude = own * k;
					const bool added = func(a_this, a_data);
					a_data.magnitude = own;
					return added;
				}
			}
			return func(a_this, a_data);
		}
		static inline REL::Relocation<decltype(thunk)> func;
		static void                                    Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ T::VTABLE[4] };  // MagicTarget
			func = vtbl.write_vfunc(0x01, thunk);
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

	void OnDataLoaded()
	{
		Plugin::LoadSettings();
		PlayerUpdate::Install();
		AddTarget<RE::Character>::Install();
		AddTarget<RE::PlayerCharacter>::Install();
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESSpellCastEvent>(CastSink::Get());
		}
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(InputSink::Get());
		}
		SKSE::log::info("hooked the player update and each effect as it lands; listening for spell casts and the Overdrive key");
		Plugin::RegisterMenu();
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
			Plugin::RegisterArcBar();  // Reticle Arcs takes add-on bars now or never
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// every meter belongs to the world going away; hand lights go back as they were found
			Plugin::ResetAll();
			Plugin::ReleaseHands();
			Plugin::ReleaseOverload();
			Plugin::ReleaseMarks();
			Plugin::ForgetSpellArt();
			break;
		default:
			break;
		}
	});
	return true;
}

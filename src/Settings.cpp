// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The settings file, Data\SKSE\Plugins\SpellMasteryHeat.ini (its lines: SettingsText.h). Read once at data load,
// written whenever the menu changes a setting.
//
// Threads: the menu changes the settings on the render thread and DevBench on its own, while the main thread reads them
// every frame. The one copy here is behind a lock, and everyone else works on a copy (Config / SetConfig).

#include "Plugin.h"

#include <sstream>

namespace Plugin
{
	namespace
	{
		constexpr const char* kPath = "Data/SKSE/Plugins/SpellMasteryHeat.ini";

		std::mutex gLock;
		Settings   gSettings;
	}

	Settings Config()
	{
		std::lock_guard lock(gLock);
		return gSettings;
	}

	void SetConfig(const Settings& a_settings)
	{
		std::lock_guard lock(gLock);
		gSettings = a_settings;
	}

	bool ApplySetting(std::string_view a_key, int a_value)
	{
		std::lock_guard lock(gLock);
		return SettingsText::Apply(gSettings, a_key, a_value);
	}

	void LoadSettings()
	{
		Settings            s;
		IniText::ReadResult read;
		if (std::ifstream in(kPath); in) {
			read = SettingsText::Read(in, s);
		}
		for (const auto& p : read.problems) {
			SKSE::log::warn("settings: {}", p);
		}
		SetConfig(s);
		const auto& t = s.tuning;
		SKSE::log::info(
			"settings: {} line(s) read from {}; enabled {}, gain {}%, drain {}/s, grace {} s, per school {}, overdrive {} (key {}), "
			"burst {} s, cooldown {} s, slow magicka {} ({}%), weaker spells {} ({}%), boost {}, light growth {} ({}%), hand glow {}, bar {}, "
			"healing and wards {}, staves and scrolls {}, enemies {}",
			read.taken, kPath, s.enabled, SettingsText::ToPct(t.gain), t.drain, t.grace, t.perSchool, static_cast<int>(t.mode), s.overdriveKey, t.burst,
			t.cooldown, t.slowMagicka, SettingsText::ToPct(t.slowMagickaTo), t.weaker, SettingsText::ToPct(t.weakerBy), static_cast<int>(s.boost),
			s.lightGrowth, SettingsText::ToPct(s.lightPerStage), s.handGlow, s.bar, s.counts.healingAndWards, s.counts.stavesAndScrolls, s.enemies);
	}

	void SaveSettings()
	{
		std::ostringstream text;
		SettingsText::Write(text, Config());
		// written beside the file, then moved over it: a crash or a full disk mid-write leaves the old settings, never half a file
		const std::string tmp = std::string(kPath) + ".tmp";
		std::ofstream     out(tmp, std::ios::trunc);
		if (!out) {
			SKSE::log::warn("settings: {} could not be written", kPath);
			return;
		}
		out << text.str();
		out.close();
		std::error_code ec;
		if (!out) {
			SKSE::log::warn("settings: {} could not be written", kPath);
		} else if (std::filesystem::rename(tmp, kPath, ec); ec) {
			SKSE::log::warn("settings: {} could not be replaced ({})", kPath, ec.message());
		}
		std::filesystem::remove(tmp, ec);  // nothing left behind when the move failed
	}
}

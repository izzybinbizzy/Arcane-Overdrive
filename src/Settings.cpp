// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The two settings files (their lines: SettingsText.h): Data\SKSE\Plugins\SpellMasteryHeat.ini, written whenever the menu
// changes a setting, and SpellMasteryHeat Advanced.ini, every number. Both are read once at data load. The advanced file
// is written only when one of its own values changed here (a preset picked, DevBench) - never just because the menu
// saved - so a hand edit made while the game runs survives, and "Read the advanced file again" picks it up. It is also
// written at load when it is missing or lacks a key (an older install): the values the old one-file ini held carry over.
//
// Threads: the menu changes the settings on the render thread and DevBench on its own, while the main thread reads them
// every frame. The one copy here is behind a lock, and everyone else works on a copy (Config / SetConfig).

#include "Plugin.h"

#include <sstream>

namespace Plugin
{
	namespace
	{
		std::mutex  gLock;
		Settings    gSettings;
		std::string gAdvancedWritten;  // the advanced file's text as last read or written

		std::string AdvancedText(const Settings& a_s)
		{
			std::ostringstream text;
			SettingsText::WriteAdvanced(text, a_s);
			return text.str();
		}

		// written beside the file, then moved over it: a crash or a full disk mid-write leaves the old settings, never half a file
		bool WriteFile(const char* a_path, const std::string& a_text)
		{
			const std::string tmp = std::string(a_path) + ".tmp";
			std::ofstream     out(tmp, std::ios::trunc);
			if (!out) {
				SKSE::log::warn("settings: {} could not be written", a_path);
				return false;
			}
			out << a_text;
			out.close();
			std::error_code ec;
			bool            ok = true;
			if (!out) {
				SKSE::log::warn("settings: {} could not be written", a_path);
				ok = false;
			} else if (std::filesystem::rename(tmp, a_path, ec); ec) {
				SKSE::log::warn("settings: {} could not be replaced ({})", a_path, ec.message());
				ok = false;
			}
			std::filesystem::remove(tmp, ec);  // nothing left behind when the move failed
			return ok;
		}

		// one file into a_s: its problems logged; how many keys it gave, and whether every advanced key was in it
		int ReadFile(const char* a_path, Settings& a_s, bool& a_allAdvanced)
		{
			std::ifstream in(a_path);
			if (!in) {
				a_allAdvanced = false;
				return -1;
			}
			std::stringstream text;
			text << in.rdbuf();
			const std::string  all = text.str();
			std::istringstream lines(all);
			const auto         read = SettingsText::Read(lines, a_s);
			for (const auto& p : read.problems) {
				SKSE::log::warn("settings: {}: {}", a_path, p);
			}
			// every advanced key the reader took (spaces round "=", any section, any case), never a match in the raw text
			a_allAdvanced = std::ranges::all_of(SettingsText::kAdvancedKeys, [&read](const SettingsText::Key& k) {
				return std::ranges::any_of(read.names, [&k](const char* a_name) { return IniText::SameText(a_name, k.name); });
			});
			return read.taken;
		}
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

	void ApplyPreset(std::size_t a_preset)
	{
		std::lock_guard lock(gLock);
		SettingsText::ApplyPreset(gSettings, a_preset);
	}

	void LoadSettings()
	{
		Settings  s;
		bool      menuHasAll = false, advancedHasAll = false;
		const int fromMenu = ReadFile(SettingsText::kMenuFile, s, menuHasAll);  // an older one-file ini holds the numbers too
		const int fromAdvanced = ReadFile(SettingsText::kAdvancedFile, s, advancedHasAll);
		SetConfig(s);
		const std::string text = AdvancedText(s);
		{
			std::lock_guard lock(gLock);
			gAdvancedWritten = text;
		}
		if (!advancedHasAll && WriteFile(SettingsText::kAdvancedFile, text)) {
			SKSE::log::info("settings: {} {} (every key, its values as read)", SettingsText::kAdvancedFile, fromAdvanced < 0 ? "made" : "completed");
		}
		const auto& t = s.tuning;
		const auto  preset = SettingsText::PresetOf(s);
		SKSE::log::info(
			"settings: {} line(s) from {}, {} from {}; enabled {}, preset {}, overdrive {} (key {}), heat gate {} ({} {}-{}), misfire {}, backlash {}, "
			"charge {}%, heat up {}% down {}%, burst {} s, cooldown {} s, slow magicka {} ({}%), weaker spells {} ({}%), damage bonus {}, boost {}, "
			"light growth {}, hand glow {}, bar {}, healing and wards {}, staves and scrolls {}, enemies {}",
			std::max(fromMenu, 0), SettingsText::kMenuFile, std::max(fromAdvanced, 0), SettingsText::kAdvancedFile, s.enabled,
			preset ? SettingsText::kPresets[*preset].name : "Custom", static_cast<int>(t.mode), s.overdriveKey, t.heatGate, static_cast<int>(t.gate), t.bandLow,
			t.redLine, t.misfire, t.backlash, SettingsText::ToPct(t.gain), SettingsText::ToPct(t.heatUp), SettingsText::ToPct(t.heatDown), t.burst, t.cooldown,
			t.slowMagicka, SettingsText::ToPct(t.slowMagickaTo), t.weaker, SettingsText::ToPct(t.weakerBy), t.bonusOn, static_cast<int>(s.boost), s.lightGrowth,
			s.handGlow, s.bar, s.counts.healingAndWards, s.counts.stavesAndScrolls, s.enemies);
	}

	bool ReloadAdvanced()
	{
		Settings s = Config();
		bool     all = false;
		if (ReadFile(SettingsText::kAdvancedFile, s, all) < 0) {
			SKSE::log::warn("settings: {} could not be read", SettingsText::kAdvancedFile);
			return false;
		}
		SetConfig(s);
		std::lock_guard lock(gLock);
		gAdvancedWritten = AdvancedText(s);
		SKSE::log::info("settings: {} read again", SettingsText::kAdvancedFile);
		return true;
	}

	void SaveSettings()
	{
		const auto         s = Config();
		std::ostringstream menu;
		SettingsText::WriteMenu(menu, s);
		WriteFile(SettingsText::kMenuFile, menu.str());
		const std::string advanced = AdvancedText(s);
		{
			std::lock_guard lock(gLock);
			if (advanced == gAdvancedWritten) {
				return;
			}
			gAdvancedWritten = advanced;
		}
		WriteFile(SettingsText::kAdvancedFile, advanced);
	}
}

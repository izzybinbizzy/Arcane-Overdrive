// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The files, Data\SKSE\Plugins\:
//   SpellMasteryHeat Clash Rules.ini   shipped with the mod: which keywords make which element, what each pair of
//                                      elements does to each other, and what never clashes. Hand-edited; never written.
//   SpellMasteryHeat Clash\*.ini       patches (another mod's spells, say), read in alphabetical order, same sections
//   SpellMasteryHeat Clashes.ini       the settings, written by the menu (Core::WriteSettings); may also hold rules
// Parsing is Core::ParseIni (Core.cpp, tested in tests/); this reads the files, resolves the excluded forms and hands
// the result to the main thread.

#include "ClashShared.h"

namespace Crossfire
{
	namespace
	{
		constexpr const char* kDir = "Data/SKSE/Plugins/";
		constexpr const char* kRules = "Data/SKSE/Plugins/SpellMasteryHeat Clash Rules.ini";
		constexpr const char* kPatches = "Data/SKSE/Plugins/SpellMasteryHeat Clash";
		constexpr const char* kSettings = "Data/SKSE/Plugins/SpellMasteryHeat Clashes.ini";

		Core::Config                   gLive;      // main thread only
		std::unordered_set<RE::FormID> gExcluded;  // main thread only

		std::mutex               gMenuLock;  // guards the two below
		Core::Config             gMenu;
		std::vector<std::string> gNotes;

		[[nodiscard]] bool ReadFile(const std::filesystem::path& a_path, std::string& a_out)
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(a_path, ec)) {
				return false;
			}
			const auto size = std::filesystem::file_size(a_path, ec);
			if (ec || size > 4u * 1024u * 1024u) {  // a settings file is a few kilobytes; anything this big is not one
				return false;
			}
			std::ifstream in(a_path, std::ios::binary);
			if (!in) {
				return false;
			}
			a_out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return true;
		}

		void ParseFile(const std::filesystem::path& a_path, Core::Config& a_config, std::vector<std::string>& a_notes)
		{
			std::string text;
			if (!ReadFile(a_path, text)) {
				return;
			}
			std::vector<std::string> warnings;
			Core::ParseIni(text, a_config, warnings);
			const auto  name = Core::PathText(a_path.filename());
			std::string note = "read " + name;
			if (!warnings.empty()) {
				note += std::format(" ({} warning(s))", warnings.size());
			}
			a_notes.push_back(std::move(note));
			for (const auto& w : warnings) {
				SKSE::log::warn("{}: {}", name, w);
				a_notes.push_back("  " + name + ", " + w);
			}
		}

		// the patches folder, in a stable, case-blind order
		[[nodiscard]] std::vector<std::filesystem::path> Patches()
		{
			std::vector<std::filesystem::path> out;
			std::error_code                    ec;
			if (!std::filesystem::is_directory(kPatches, ec)) {
				return out;
			}
			for (std::filesystem::directory_iterator it(kPatches, ec), end; !ec && it != end; it.increment(ec)) {
				auto ext = Core::PathText(it->path().extension());
				std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext == ".ini" && it->is_regular_file(ec)) {
					out.push_back(it->path());
				}
			}
			std::ranges::sort(out, [](const auto& a, const auto& b) {
				auto x = Core::PathText(a.filename()), y = Core::PathText(b.filename());
				std::ranges::transform(x, x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				std::ranges::transform(y, y.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return x < y;
			});
			return out;
		}

		void ResolveExclusions(const Core::Config& a_config, std::vector<std::string>& a_notes)
		{
			gExcluded.clear();
			auto* data = RE::TESDataHandler::GetSingleton();
			for (const auto& ref : a_config.exclude) {
				RE::TESForm* form = nullptr;
				if (!ref.file.empty()) {
					form = data ? data->LookupForm(ref.id & 0x00FFFFFF, ref.file) : nullptr;
				} else {
					form = RE::TESForm::LookupByEditorID(ref.editorID);
				}
				if (form) {
					gExcluded.insert(form->GetFormID());
				} else {
					// the mod it names is not installed, or the editor ID is not kept in memory (most are not without
					// powerofthree's Tweaks): not an error
					const auto what = ref.file.empty() ? ref.editorID : std::format("{}|0x{:06X}", ref.file, ref.id);
					SKSE::log::info("exclude: {} not found", what);
					a_notes.push_back("  exclude: " + what + " not found");
				}
			}
		}

		// copy what the menu can change, keep the rules
		void TakeSettings(Core::Config& a_to, const Core::Config& a_from)
		{
			auto keywords = std::move(a_to.keywords);
			auto reactions = a_to.reactions;
			auto exclude = std::move(a_to.exclude);
			a_to = a_from;
			a_to.keywords = std::move(keywords);
			a_to.reactions = reactions;
			a_to.exclude = std::move(exclude);
		}

		void Save(const Core::Config& a_config)
		{
			const std::filesystem::path path(kSettings), temp(std::string(kSettings) + ".tmp");
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				if (!out) {
					SKSE::log::warn("settings: {} could not be written", Core::PathText(temp));
					return;
				}
				out << Core::WriteSettings(a_config);
				if (!out.flush()) {
					SKSE::log::warn("settings: {} could not be written", Core::PathText(temp));
					return;
				}
			}
			// written whole, then swapped in: a crash mid-write cannot leave half a file
			std::error_code ec;
			std::filesystem::rename(temp, path, ec);
			if (ec) {
				SKSE::log::warn("settings: {} could not be replaced ({})", kSettings, ec.message());
				std::filesystem::remove(temp, ec);
			}
		}
	}

	void LoadSettings()
	{
		Core::Config             config;
		std::vector<std::string> notes;
		ParseFile(kRules, config, notes);
		for (const auto& patch : Patches()) {
			ParseFile(patch, config, notes);
		}
		ParseFile(kSettings, config, notes);
		if (notes.empty()) {
			notes.push_back(std::string("no settings files found in ") + kDir + "; the defaults apply");
		}
		ResolveExclusions(config, notes);
		gLive = config;
		ClearInfo();
		{
			std::scoped_lock lock(gMenuLock);
			gMenu = config;
			gNotes = notes;
		}
		std::size_t keywords = 0;
		for (const auto& list : config.keywords) {
			keywords += list.size();
		}
		SKSE::log::info(
			"settings: {}, who {}, overpower x{}, radius x{} +{}, explosions {}, {} keyword(s), {} exclusion(s) ({} found), "
			"struggles {}, skill x{}, push {} s",
			config.enabled ? "on" : "off", config.who, config.overpowerRatio, config.radiusScale, config.radiusBonus,
			config.explosions ? (config.safeExplosionsOnly ? "safe only" : "all") : "off", keywords, config.exclude.size(), gExcluded.size(),
			config.struggle.enabled ? "on" : "off", config.struggle.skillWeight, config.struggle.pushTime);
	}

	const Core::Config& Live() { return gLive; }

	Core::Config MenuCopy()
	{
		std::scoped_lock lock(gMenuLock);
		return gMenu;
	}

	std::vector<std::string> LoadNotes()
	{
		std::scoped_lock lock(gMenuLock);
		return gNotes;
	}

	void Submit(const Core::Config& a_config, bool a_save)
	{
		{
			std::scoped_lock lock(gMenuLock);
			TakeSettings(gMenu, a_config);
		}
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		tasks->AddTask([a_config, a_save]() {
			const bool wasOn = gLive.enabled;
			TakeSettings(gLive, a_config);
			ClearInfo();  // a breath shout's spray is priced from ShoutStrength when first seen
			if (wasOn != gLive.enabled) {
				SKSE::log::info("turned {}", gLive.enabled ? "on" : "off");
			}
			if (a_save) {
				Save(gLive);
			}
		});
	}

	void ReloadSoon()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([]() {
				LoadSettings();
				SKSE::log::info("settings reloaded from the menu");
			});
		}
	}

	bool Excluded(const RE::TESForm* a_form) { return a_form && !gExcluded.empty() && gExcluded.contains(a_form->GetFormID()); }
}

// IniText.h - a settings file's lines, read and written as plain text, with nothing of the game in it.
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// Split out of the fading module's settings text: the plugin keeps its settings and key table in SettingsText.h, and this
// does the reading and writing. Each key names its section ([Settings] unless it says otherwise) and may carry a note,
// written above it, so a file with sections can explain every line.
//
// The file is read the way people edit it: a byte order mark (Notepad's), any case in section and key names, spaces,
// Windows line ends, and ; or # comments, on their own line or after a value. Every value is a whole number. A key is
// taken in any section (a line moved to the wrong section still counts, and an older one-section file still reads). A
// line that is not a whole number, or names no setting, keeps the default and is reported; a retired key is skipped
// quietly, and so is anything in a section the table does not use.

#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <istream>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace IniText
{
	[[nodiscard]] inline float Pct(int a_v, int a_lo, int a_hi) { return static_cast<float>(std::clamp(a_v, a_lo, a_hi)) / 100.0f; }
	[[nodiscard]] inline int   ToPct(float a_v) { return std::isfinite(a_v) ? static_cast<int>(std::lround(a_v * 100.0f)) : 0; }

	// one setting: its key in the file, how it reads back as a whole number, and how a whole number sets it (clamped to
	// what the menu allows). One table per plugin, so the file, the menu and DevBench can never disagree.
	template <class S>
	struct Key
	{
		const char* name;
		int (*get)(const S&);
		void (*set)(S&, int);
		const char* section{ "Settings" };
		const char* note{ nullptr };
	};

	[[nodiscard]] inline bool SameText(std::string_view a, std::string_view b) noexcept
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}

	template <class S>
	[[nodiscard]] const Key<S>* Find(std::span<const Key<S>> a_keys, std::string_view a_name) noexcept
	{
		for (const auto& k : a_keys) {
			if (SameText(a_name, k.name)) {
				return &k;
			}
		}
		return nullptr;
	}

	[[nodiscard]] inline std::string_view Trim(std::string_view a_s) noexcept
	{
		while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.front()))) {
			a_s.remove_prefix(1);
		}
		while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.back()))) {
			a_s.remove_suffix(1);
		}
		return a_s;
	}

	// a whole number and nothing else ("15abc" and "1.5" are not)
	[[nodiscard]] inline bool ReadInt(std::string_view a_v, int& a_out) noexcept
	{
		int v = 0;
		const auto [end, ec] = std::from_chars(a_v.data(), a_v.data() + a_v.size(), v);
		if (a_v.empty() || ec != std::errc{} || end != a_v.data() + a_v.size()) {
			return false;
		}
		a_out = v;
		return true;
	}

	// one setting by its key (any case), clamped as the menu clamps it; false for a key it does not know
	template <class S>
	bool Apply(std::span<const Key<S>> a_keys, S& a_s, std::string_view a_key, int a_value)
	{
		if (const auto* k = Find(a_keys, a_key)) {
			k->set(a_s, a_value);
			return true;
		}
		return false;
	}

	struct ReadResult
	{
		int                      taken{ 0 };
		std::vector<std::string> problems;  // "Key=value: why", for the log
		std::vector<const char*> names;     // the table's own name of every key taken
	};

	// reads the lines of the table's sections into a_s; a line that cannot be used keeps the default and is reported
	template <class S>
	ReadResult Read(std::istream& a_in, S& a_s, std::span<const Key<S>> a_keys, std::span<const std::string_view> a_retired = {})
	{
		ReadResult  r;
		std::string line;
		bool        inOurs = false, first = true;
		const auto  ours = [a_keys](std::string_view a_section) {
			return SameText(a_section, "Settings") || std::ranges::any_of(a_keys, [a_section](const Key<S>& k) { return SameText(a_section, k.section); });
		};
		while (std::getline(a_in, line)) {
			std::string_view l = line;
			if (first && l.starts_with("\xEF\xBB\xBF")) {
				l.remove_prefix(3);  // a UTF-8 byte order mark
			}
			first = false;
			if (const auto c = l.find_first_of(";#"); c != std::string_view::npos) {
				l = l.substr(0, c);
			}
			l = Trim(l);
			if (l.empty()) {
				continue;
			}
			if (l.front() == '[' && l.back() == ']') {
				inOurs = ours(Trim(l.substr(1, l.size() - 2)));
				continue;
			}
			const auto eq = l.find('=');
			if (!inOurs || eq == std::string_view::npos) {
				continue;
			}
			const auto key = Trim(l.substr(0, eq));
			const auto val = Trim(l.substr(eq + 1));
			int        v = 0;
			if (std::ranges::any_of(a_retired, [key](std::string_view k) { return SameText(key, k); })) {
				continue;
			}
			const auto* found = Find(a_keys, key);
			if (!found) {
				r.problems.push_back(std::string(key) + ": not a setting");
			} else if (!ReadInt(val, v)) {
				r.problems.push_back(std::string(key) + "=" + std::string(val) + ": not a whole number; the default stays");
			} else {
				Apply(a_keys, a_s, key, v);
				++r.taken;
				r.names.push_back(found->name);
			}
		}
		return r;
	}

	// a_header (its own lines, each starting "; "), then every key under its section, a section's header where it starts and
	// each key's note above it
	template <class S>
	void Write(std::ostream& a_out, const S& a_s, std::span<const Key<S>> a_keys, std::string_view a_header)
	{
		a_out << a_header;
		std::string_view section;
		for (const auto& k : a_keys) {
			if (!SameText(section, k.section)) {
				a_out << (section.empty() ? "" : "\n") << '[' << k.section << "]\n";
				section = k.section;
			}
			if (k.note) {
				a_out << "; " << k.note << '\n';
			}
			a_out << k.name << '=' << k.get(a_s) << '\n';
		}
	}
}

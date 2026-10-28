// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// An exception that leaves a hook, a game event, a menu page or an SKSE task ends the game. Each of those runs its work
// through Guarded: an exception stops that work there, is logged once for that place, and the game goes on (the same helper
// as LightKit.h's in Illuminated and RELight).

#pragma once

namespace Safe
{
	inline void Told(std::atomic<bool>& a_told, const char* a_where, const char* a_what) noexcept
	{
		if (!a_told.exchange(true)) {
			SKSE::log::error("{}: stopped by an error ({}); later ones here are skipped quietly", a_where, a_what);
		}
	}

	template <class F>
	inline void Guarded(const char* a_where, F&& a_work) noexcept
	{
		static std::atomic<bool> told{ false };
		try {
			std::forward<F>(a_work)();
		} catch (const std::exception& e) {
			Told(told, a_where, e.what());
		} catch (...) {
			Told(told, a_where, "unknown");
		}
	}

	// the same, for work with an answer: a_fallback when it was stopped
	template <class R, class F>
	[[nodiscard]] inline R Guarded(const char* a_where, R a_fallback, F&& a_work) noexcept
	{
		static std::atomic<bool> told{ false };
		try {
			return std::forward<F>(a_work)();
		} catch (const std::exception& e) {
			Told(told, a_where, e.what());
		} catch (...) {
			Told(told, a_where, "unknown");
		}
		return a_fallback;
	}
}

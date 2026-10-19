// Spell Mastery Heat - tests for src/Heat.h, src/Scaled.h, src/SettingsText.h and src/IniText.h (no game needed).
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later.
//
// tests/run.sh builds and runs them under g++ and clang++ with the sanitizers; build.bat runs them through xmake.

#include "Heat.h"
#include "Scaled.h"
#include "SettingsText.h"
#include "FlameFx.h"
#include "BarFx.h"
#include "Overload.h"
#include "Marks.h"
#include "SpellLook.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <string>

namespace
{
	int gFailed = 0, gPassed = 0;

	void Check(bool a_ok, const char* a_what, int a_line)
	{
		if (a_ok) {
			++gPassed;
		} else {
			++gFailed;
			if (gFailed <= 20) {
				std::printf("FAIL line %d: %s\n", a_line, a_what);
			}
		}
	}
#define CHECK(x) Check((x), #x, __LINE__)

	bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

	using namespace Heat;
	constexpr float kFrame = 1.0f / 60.0f;

	void Run(Meter& a_m, const Tuning& a_t, float a_seconds)
	{
		for (float s = 0.0f; s < a_seconds; s += kFrame) {
			a_m.Tick(a_t, kFrame);
		}
	}

	void Gains()
	{
		CHECK(Near(CastGain(50.0f), 8.0f));
		CHECK(Near(CastGain(0.0f), 4.0f));   // even a free spell adds a little
		CHECK(Near(CastGain(1e6f), 24.0f));  // capped
		CHECK(Near(CastGain(std::nanf("")), 4.0f));
		CHECK(CastGain(100.0f) > CastGain(50.0f));  // costlier adds more

		Tuning t;
		Meter  m;
		m.Cast(t, School::kDestruction, 50.0f);
		CHECK(Near(m.HeatOf(t, School::kDestruction), 8.0f));
		m.Cast(t, School::kDestruction, 50.0f);  // the same school: +50%
		CHECK(Near(m.HeatOf(t, School::kDestruction), 20.0f));
		CHECK(m.Last() == School::kDestruction);

		// gain speed scales it
		Tuning fast = t;
		fast.gain = 2.0f;
		Meter f;
		f.Cast(fast, School::kDestruction, 50.0f);
		CHECK(Near(f.HeatOf(fast, School::kDestruction), 16.0f));

		// a held spell builds per second, at its own (lower) rate, and never jumps
		Meter h;
		for (int i = 0; i < 60; ++i) {
			h.Hold(t, School::kDestruction, 50.0f, kFrame);
		}
		const float second = h.HeatOf(t, School::kDestruction);
		CHECK(second > 4.0f && second < 8.0f * 0.6f * 1.5f + 0.01f);
		Meter bad;
		bad.Hold(t, School::kDestruction, 50.0f, std::nanf(""));
		bad.Hold(t, School::kDestruction, 50.0f, -1.0f);
		bad.Cast(t, School::kDestruction, -5.0f);  // a nonsense cost still counts as the smallest cast
		CHECK(Near(bad.HeatOf(t, School::kDestruction), 4.0f));
	}

	void Stages()
	{
		Tuning t;
		CHECK(StageAt(t, 0.0f) == Stage::kCold);
		CHECK(StageAt(t, 24.9f) == Stage::kCold);
		CHECK(StageAt(t, 25.0f) == Stage::kWarm);
		CHECK(StageAt(t, 50.0f) == Stage::kHot);
		CHECK(StageAt(t, 75.0f) == Stage::kOverheating);
		CHECK(StageAt(t, 100.0f) == Stage::kOverheating);  // Overdrive is a state, not a heat level
		CHECK(Near(StageBonus(t, Stage::kCold), 1.0f));
		CHECK(Near(StageBonus(t, Stage::kWarm), 1.04f));
		CHECK(Near(StageBonus(t, Stage::kHot), 1.08f));
		CHECK(Near(StageBonus(t, Stage::kOverheating), 1.12f));
		CHECK(Near(StageBonus(t, Stage::kOverdrive), 1.25f));
		// thresholds out of order (a hand-edited file) still give stages in order
		Tuning odd = t;
		odd.at = { 80.0f, 20.0f, 10.0f };
		Stage last = Stage::kCold;
		bool  ordered = true;
		for (float h = 0.0f; h <= 100.0f; h += 0.5f) {
			const auto s = StageAt(odd, h);
			ordered &= s >= last;
			last = s;
		}
		CHECK(ordered);
		CHECK(StageAt(odd, 100.0f) == Stage::kOverheating);
		// light growth
		CHECK(Near(LightFactor(0.15f, Stage::kCold), 1.0f));
		CHECK(Near(LightFactor(0.15f, Stage::kOverheating), 1.45f));
		CHECK(Near(LightFactor(0.15f, Stage::kOverdrive), 1.6f));
		CHECK(Near(LightFactor(std::nanf(""), Stage::kHot), 1.0f));
	}

	void Drain()
	{
		Tuning t;
		Meter  m;
		m.Cast(t, School::kDestruction, 50.0f);
		m.Cast(t, School::kDestruction, 50.0f);  // 20
		Run(m, t, 1.9f);
		CHECK(Near(m.Shown(t), 20.0f));  // within the grace period: nothing drains
		Run(m, t, 1.1f);                 // 1 s of drain at 10 a second
		CHECK(Near(m.Shown(t), 10.0f, 0.4f));
		Run(m, t, 5.0f);
		CHECK(Near(m.Shown(t), 0.0f));  // never below zero
		CHECK(m.Idle());
		// a cast resets the grace period (the last cast's school is still remembered: both get the same-school bonus)
		m.Cast(t, School::kDestruction, 50.0f);
		Run(m, t, 1.5f);
		m.Cast(t, School::kDestruction, 50.0f);
		Run(m, t, 1.5f);
		CHECK(Near(m.Shown(t), 24.0f));
		// a paused game (no time) drains nothing
		for (int i = 0; i < 1000; ++i) {
			m.Tick(t, 0.0f);
		}
		CHECK(Near(m.Shown(t), 24.0f));
	}

	void SchoolSwitch()
	{
		Tuning t;
		Meter  m;
		for (int i = 0; i < 5; ++i) {
			m.Cast(t, School::kDestruction, 50.0f);  // 8 + 12 x 4 = 56
		}
		CHECK(Near(m.Shown(t), 56.0f));
		CHECK(m.ShownStage(t) == Stage::kHot);
		m.Cast(t, School::kConjuration, 50.0f);  // loses 25, then +8 (no same-school bonus)
		CHECK(Near(m.Shown(t), 39.0f));
		CHECK(m.ShownStage(t) == Stage::kWarm);  // one stage down
		// switch cooling 0: nothing lost
		Tuning keep = t;
		keep.switchLoss = 0.0f;
		Meter k;
		k.Cast(keep, School::kDestruction, 50.0f);
		k.Cast(keep, School::kIllusion, 50.0f);
		CHECK(Near(k.Shown(keep), 16.0f));

		// one meter per school: each builds on its own; switching cools the one being left
		Tuning per = t;
		per.perSchool = true;
		Meter p;
		for (int i = 0; i < 5; ++i) {
			p.Cast(per, School::kDestruction, 50.0f);
		}
		p.Cast(per, School::kRestoration, 50.0f);
		CHECK(Near(p.HeatOf(per, School::kDestruction), 31.0f));
		CHECK(Near(p.HeatOf(per, School::kRestoration), 8.0f));
		CHECK(Near(p.Shown(per), 8.0f));  // the bar shows the last school's
		CHECK(p.StageOf(per, School::kDestruction) == Stage::kWarm);
		CHECK(Near(p.Bonus(per, School::kRestoration), 1.0f));
		CHECK(Near(p.Bonus(per, School::kDestruction), 1.04f));
	}

	void Overdrive()
	{
		// automatic: at 100 it starts, lasts the burst, then the cooldown locks the meter at 0
		Tuning t;
		Meter  m;
		int    casts = 0;
		while (m.Now() == Meter::State::kBuilding && casts < 100) {
			m.Cast(t, School::kDestruction, 50.0f);
			++casts;
		}
		CHECK(m.Now() == Meter::State::kOverdrive);
		CHECK(casts == 9);  // 8 + 12 x 8 = 104
		CHECK(m.ShownStage(t) == Stage::kOverdrive);
		CHECK(Near(m.Bonus(t, School::kDestruction), 1.25f));
		CHECK(Near(m.Bonus(t, School::kIllusion), 1.25f));  // every spell, whatever its school
		Run(m, t, 3.0f);
		CHECK(m.Now() == Meter::State::kOverdrive);
		CHECK(Near(m.Shown(t), 100.0f));  // no drain while it lasts
		Run(m, t, 3.1f);
		CHECK(m.Now() == Meter::State::kCooldown);
		CHECK(Near(m.Shown(t), 0.0f));
		CHECK(Near(m.MagickaRegen(t), 0.4f));                 // slow magicka: on by default
		CHECK(Near(m.Bonus(t, School::kDestruction), 1.0f));  // weaker spells: off by default
		m.Cast(t, School::kDestruction, 500.0f);              // heat cannot build
		CHECK(Near(m.Shown(t), 0.0f));
		Run(m, t, 9.5f);
		CHECK(m.Now() == Meter::State::kCooldown);
		Run(m, t, 0.6f);
		CHECK(m.Now() == Meter::State::kBuilding);
		CHECK(Near(m.MagickaRegen(t), 1.0f));
		m.Cast(t, School::kDestruction, 50.0f);
		CHECK(Near(m.Shown(t), 12.0f));  // builds again (the same school as before: +50%)

		// the penalties, each optional
		Tuning harsh = t;
		harsh.weaker = true;
		harsh.slowMagicka = false;
		Meter h;
		h.ForceStage(harsh, Stage::kOverdrive);
		Run(h, harsh, 6.1f);
		CHECK(h.Now() == Meter::State::kCooldown);
		CHECK(Near(h.Bonus(harsh, School::kDestruction), 0.85f));
		CHECK(Near(h.MagickaRegen(harsh), 1.0f));

		// no cooldown at all: straight back to building
		Tuning none = t;
		none.cooldown = 0.0f;
		Meter n;
		n.ForceStage(none, Stage::kOverdrive);
		Run(n, none, 6.1f);
		CHECK(n.Now() == Meter::State::kBuilding);

		// key press: it waits at Overheating, full, until the key; the key does nothing before
		Tuning key = t;
		key.mode = OverdriveMode::kKey;
		Meter k;
		CHECK(!k.PressKey(key));
		for (int i = 0; i < 20; ++i) {
			k.Cast(key, School::kDestruction, 50.0f);
		}
		CHECK(k.Now() == Meter::State::kBuilding);
		CHECK(k.Ready(key));
		CHECK(k.ShownStage(key) == Stage::kOverheating);
		CHECK(Near(k.Bonus(key, School::kDestruction), 1.12f));
		CHECK(k.PressKey(key));
		CHECK(k.Now() == Meter::State::kOverdrive);
		CHECK(!k.PressKey(key));  // not twice
		// left to cool below full, it is no longer ready
		Meter c;
		for (int i = 0; i < 20; ++i) {
			c.Cast(key, School::kDestruction, 50.0f);
		}
		Run(c, key, 2.5f);
		CHECK(!c.Ready(key));
		CHECK(!c.PressKey(key));
		// the key in automatic or off mode does nothing
		Meter a;
		CHECK(!a.PressKey(t));

		// off: the meter tops out at Overheating for ever
		Tuning off = t;
		off.mode = OverdriveMode::kOff;
		Meter o;
		for (int i = 0; i < 50; ++i) {
			o.Cast(off, School::kDestruction, 50.0f);
		}
		CHECK(o.Now() == Meter::State::kBuilding);
		CHECK(o.ShownStage(off) == Stage::kOverheating);
		CHECK(!o.Ready(off) && !o.PressKey(off));

		// held spells take far longer than casts to reach Overdrive (Flames, about 14 magicka a second)
		Meter held;
		float seconds = 0.0f;
		while (held.Now() == Meter::State::kBuilding && seconds < 120.0f) {
			held.Hold(t, School::kDestruction, 14.0f, kFrame);
			held.Tick(t, kFrame);
			seconds += kFrame;
		}
		CHECK(seconds > 15.0f && seconds < 60.0f);
	}

	void Bench()
	{
		Tuning t;
		Meter  m;
		m.SetHeat(t, 60.0f);
		CHECK(m.ShownStage(t) == Stage::kHot);
		m.SetHeat(t, 500.0f);
		CHECK(Near(m.Shown(t), 100.0f));
		m.SetHeat(t, std::nanf(""));
		CHECK(Near(m.Shown(t), 0.0f));
		for (auto s : { Stage::kCold, Stage::kWarm, Stage::kHot, Stage::kOverheating, Stage::kOverdrive }) {
			m.ForceStage(t, s);
			CHECK(m.ShownStage(t) == s);
		}
		m.SetHeat(t, 30.0f);  // ends a forced Overdrive
		CHECK(m.Now() == Meter::State::kBuilding);
		m.Reset();
		CHECK(m.Idle());
	}

	// a long random session: the meter never leaves its range, never goes NaN, and the bonus stays within its settings
	void Random()
	{
		std::mt19937                          rng(11);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		bool                                  ok = true;
		for (int run = 0; run < 40; ++run) {
			Tuning t;
			t.mode = static_cast<OverdriveMode>(run % 3);
			t.perSchool = run % 2 == 0;
			t.weaker = run % 4 < 2;
			Meter m;
			for (int i = 0; i < 20000; ++i) {
				const auto  school = static_cast<School>(static_cast<int>(u(rng) * 6.0f) % 6);
				const float r = u(rng);
				if (r < 0.03f) {
					m.Cast(t, school, u(rng) * 300.0f);
				} else if (r < 0.08f) {
					m.Hold(t, school, u(rng) * 40.0f, kFrame);
				} else if (r < 0.081f) {
					m.PressKey(t);
				}
				m.Tick(t, u(rng) < 0.01f ? 0.3f : kFrame);
				for (int s = 0; s < 6; ++s) {
					const float h = m.HeatOf(t, static_cast<School>(s));
					const float b = m.Bonus(t, static_cast<School>(s));
					ok &= std::isfinite(h) && h >= 0.0f && h <= 100.0f;
					ok &= b >= 0.85f - 1e-6f && b <= 1.25f + 1e-6f;
				}
				ok &= m.MagickaRegen(t) >= 0.4f - 1e-6f && m.MagickaRegen(t) <= 1.0f;
			}
		}
		CHECK(ok);
	}

	void Counting()
	{
		using K = SpellFacts::Kind;
		const Counts defaults{};
		CHECK(BuildsHeat({ K::kSpell, false }, defaults));
		CHECK(!BuildsHeat({ K::kSpell, true }, defaults));  // healing and wards: off by default
		CHECK(BuildsHeat({ K::kSpell, true }, { true, false }));
		CHECK(!BuildsHeat({ K::kStaff, false }, defaults));
		CHECK(!BuildsHeat({ K::kScroll, false }, defaults));
		CHECK(BuildsHeat({ K::kStaff, false }, { false, true }));
		CHECK(BuildsHeat({ K::kScroll, false }, { false, true }));
		CHECK(!BuildsHeat({ K::kStaff, true }, { false, true }));  // a healing staff still needs the healing switch
		for (const Counts c : { Counts{ false, false }, Counts{ true, true } }) {
			CHECK(!BuildsHeat({ K::kPower, false }, c));  // shouts and powers never
			CHECK(!BuildsHeat({ K::kOther, false }, c));
		}
	}

	void SlowMagicka()
	{
		// natural regeneration (a small rise) is slowed; a potion (a rise beyond what regeneration gives) is not
		CHECK(Near(TakeBack(50.0f, 51.0f, 2.0f, 0.4f), 0.6f));
		CHECK(Near(TakeBack(50.0f, 90.0f, 2.0f, 0.4f), 0.0f));
		CHECK(Near(TakeBack(50.0f, 49.0f, 2.0f, 0.4f), 0.0f));  // spent magicka is never touched
		CHECK(Near(TakeBack(50.0f, 51.0f, 2.0f, 1.0f), 0.0f));  // no penalty: nothing taken
		CHECK(Near(TakeBack(50.0f, 51.0f, 2.0f, 0.0f), 1.0f));  // 0%: all of it
		CHECK(Near(TakeBack(std::nanf(""), 51.0f, 2.0f, 0.4f), 0.0f));
		// over a second of regeneration at 3 a second, 40% is what is left
		float magicka = 0.0f, gained = 0.0f;
		for (int i = 0; i < 60; ++i) {
			const float before = magicka;
			magicka += 3.0f * kFrame;  // the game's regeneration
			const float back = TakeBack(before, magicka, 3.0f * kFrame * 2.0f, 0.4f);
			magicka -= back;
			gained = magicka;
		}
		CHECK(Near(gained, 1.2f, 1e-3f));
	}

	// the hand lights: the fading module's scaler, with Heat growing a light the fading module (Illuminated or RELight) also scales
	void HandLights()
	{
		Glow::Scaled s;
		float        v = 1.0f;
		for (int i = 0; i < 100; ++i) {
			v = s.Apply(v, 1.45f);  // a steady light, scaled twice a frame
			v = s.Apply(v, 1.45f);
		}
		CHECK(Near(v, 1.45f));
		CHECK(Near(s.Restore(v), 1.0f));
		CHECK(Near(s.Restore(0.3f), 0.3f));  // someone else wrote since: theirs stays

		// a flickering light (Light Placer rewrites it each frame): ours follows its new value
		Glow::Scaled f;
		bool         follows = true;
		for (int i = 0; i < 200; ++i) {
			const float lp = 1.0f + 0.3f * std::sin(static_cast<float>(i) * 0.7f);
			follows &= Near(f.Apply(lp, 1.3f), lp * 1.3f, 1e-4f);
		}
		CHECK(follows);

		// another plugin scaling the same light the same way: without the guard it would grow to white for ever
		Glow::Scaled ours, theirs;
		float        w = 1.0f;
		for (int i = 0; i < 600; ++i) {
			w = ours.Apply(w, 1.45f);
			w = theirs.Apply(w, 0.9f);
		}
		CHECK(ours.frozen || theirs.frozen);
		CHECK(w < 5.0f && w > 0.2f);  // held, not driven away
	}

	void SettingsFile()
	{
		using namespace Plugin;
		Settings s;
		s.enabled = false;
		s.tuning.gain = 2.0f;
		s.tuning.heldGain = 1.5f;
		s.tuning.drain = 20.0f;
		s.tuning.grace = 5.0f;
		s.tuning.perSchool = true;
		s.tuning.sameSchool = 1.0f;
		s.tuning.switchLoss = 50.0f;
		s.tuning.at = { 20.0f, 40.0f, 60.0f };
		s.tuning.bonusPerStage = 0.1f;
		s.tuning.overdriveBonus = 0.5f;
		s.boost = Boost::kAll;
		s.tuning.mode = OverdriveMode::kKey;
		s.overdriveKey = 257;
		s.tuning.burst = 10.0f;
		s.tuning.cooldown = 20.0f;
		s.tuning.slowMagicka = false;
		s.tuning.slowMagickaTo = 0.6f;
		s.tuning.weaker = true;
		s.tuning.weakerBy = 0.3f;
		s.lightGrowth = false;
		s.lightPerStage = 0.3f;
		s.handGlow = false;
		s.bar = false;
		s.barX = 0.5f;
		s.barY = 0.1f;
		s.barWidth = 2.0f;
		s.counts = { true, true };
		s.enemies = true;
		s.debugLog = true;
		std::stringstream text;
		SettingsText::Write(text, s);
		Settings   back;
		const auto r = SettingsText::Read(text, back);
		CHECK(back == s);
		CHECK(r.problems.empty());
		CHECK(r.taken == static_cast<int>(std::size(SettingsText::kKeys)));

		std::istringstream edited("[Settings]\nGainSpeed=9999\nOverdrive=5\nOverdriveKey=-4\nDrainSpeed=1.5\nNope=3\nBarWidth=10\n");
		Settings           e;
		const auto         re = SettingsText::Read(edited, e);
		CHECK(Near(e.tuning.gain, 3.0f));
		CHECK(e.tuning.mode == OverdriveMode::kOff);
		CHECK(e.overdriveKey == 0);
		CHECK(Near(e.tuning.drain, 10.0f));
		CHECK(Near(e.barWidth, 0.5f));
		CHECK(re.problems.size() == 2);

		// his rename 2026-10-18: the key is OverheatingAt now; an older file's BlazingAt is still read, and never written again
		std::istringstream old("[Settings]\nBlazingAt=80\n");
		Settings           o;
		const auto         ro = SettingsText::Read(old, o);
		CHECK(Near(o.tuning.at[2], 80.0f));
		CHECK(ro.problems.empty() && ro.taken == 1);
		CHECK(SettingsText::Apply(o, "blazingat", 70) && Near(o.tuning.at[2], 70.0f));
		CHECK(text.str().find("OverheatingAt=") != std::string::npos && text.str().find("Blazing") == std::string::npos);
		CHECK(StageName(Stage::kOverheating) == "Overheating");

		CHECK(SettingsText::KeyName(34) == "G");
		CHECK(SettingsText::KeyName(2) == "1");
		CHECK(SettingsText::KeyName(11) == "0");
		CHECK(SettingsText::KeyName(256) == "Mouse button 1");
		CHECK(SettingsText::KeyName(270) == "Gamepad button 5");
		CHECK(SettingsText::KeyName(0) == "none");

		// gamepad buttons in SKSE's numbering: the XInput masks (0x0400 and 0x0800 are not buttons), then the triggers' ids
		CHECK(SettingsText::GamepadCode(0x0001) == 266);  // d-pad up
		CHECK(SettingsText::GamepadCode(0x0010) == 270);  // Start
		CHECK(SettingsText::GamepadCode(0x0200) == 275);  // right shoulder
		CHECK(SettingsText::GamepadCode(0x1000) == 276);  // A
		CHECK(SettingsText::GamepadCode(0x8000) == 279);  // Y
		CHECK(SettingsText::GamepadCode(0x0009) == 280);  // left trigger
		CHECK(SettingsText::GamepadCode(0x000A) == 281);  // right trigger
		CHECK(SettingsText::GamepadCode(0x0400) == -1);
		CHECK(SettingsText::GamepadCode(0) == -1);
		CHECK(SettingsText::GamepadCode(0x0003) == -1);
		static_assert(SettingsText::GamepadCode(0x2000) == 277);  // B, worked out at compile time

		// the settings file that ships reads back to exactly the built-in defaults
		std::string here = __FILE__;
		here = here.substr(0, here.find_last_of("/\\") + 1);
		std::ifstream shipped(here + "../package/SKSE/Plugins/SpellMasteryHeat.ini");
		CHECK(shipped.good());
		Settings fromFile;
		fromFile.enabled = false;
		const auto rs = SettingsText::Read(shipped, fromFile);
		CHECK(fromFile == Settings{});
		CHECK(rs.problems.empty());
		CHECK(rs.taken == static_cast<int>(std::size(SettingsText::kKeys)));
	}

	// the flame bar's moving parts (FlameFx.h, his order 2026-10-11)
	void FlameBar()
	{
		using namespace FlameFx;
		float lo = 1.0f, hi = 0.0f, jump = 0.0f, prev = Noise2(0.0f, 0.0f);
		for (int i = 1; i < 20000; ++i) {
			const float x = static_cast<float>(i) * 0.01f;
			const float n = Noise2(x, x * 0.37f);
			lo = std::min(lo, n);
			hi = std::max(hi, n);
			jump = std::max(jump, std::fabs(n - prev));
			prev = n;
			CHECK(Flicker(x, x * 2.0f) >= 0.0f && Flicker(x, x * 2.0f) <= 1.0f);
		}
		CHECK(lo >= 0.0f && hi <= 1.0f && hi - lo > 0.5f);  // the whole range, never outside it
		CHECK(jump < 0.05f);                                // smooth: no step between neighbours
		CHECK(Near(Noise1(std::nanf("")), 0.5f) && Near(Noise2(std::nanf(""), 1.0f), 0.5f));
		CHECK(Noise1(-12345.6f) >= 0.0f && Noise1(1e9f) <= 1.0f);
		const auto cold = HeatColor(0.0f), white = HeatColor(1.0f), mid = HeatColor(0.5f);
		CHECK(cold.r > cold.g && cold.g < 0.1f);   // deep red
		CHECK(white.g > 0.95f && white.b > 0.9f);  // white-hot
		CHECK(mid.r >= mid.g && mid.g >= mid.b);   // orange in between
		CHECK(Near(HeatColor(-3.0f).r, cold.r) && Near(HeatColor(std::nanf("")).r, cold.r));
		// every stage stands taller and hotter than the one before; the burnout has no tongues but smoke
		CHECK(LookOf(Mood::kCold).tongue == 0.0f && LookOf(Mood::kCold).embers == 0.0f);
		CHECK(LookOf(Mood::kWarm).tongue < LookOf(Mood::kHot).tongue && LookOf(Mood::kHot).tongue < LookOf(Mood::kOverheating).tongue);
		CHECK(LookOf(Mood::kOverheating).tongue < LookOf(Mood::kOverdrive).tongue);
		CHECK(LookOf(Mood::kWarm).temperature < LookOf(Mood::kOverdrive).temperature);
		CHECK(LookOf(Mood::kOverdrive).sparks > 0.0f && LookOf(Mood::kBurnout).tongue == 0.0f && LookOf(Mood::kBurnout).smoke > 0.0f);
		// the pool never grows past its size, frees the dead and spawns at the asked rate
		Particles<16> pool;
		pool.Emit(Particle::Kind::kEmber, 1000.0f, 0.1f, 200.0f, 7.0f, 1.0f);
		CHECK(pool.Alive() == 16);
		for (int i = 0; i < 400; ++i) {
			pool.Step(kFrame, static_cast<float>(i) * kFrame);
		}
		CHECK(pool.Alive() == 0);  // every ember lives under 2 s
		Particles<64> rate;
		for (int i = 0; i < 60; ++i) {
			rate.Emit(Particle::Kind::kSmoke, 10.0f, kFrame, 200.0f, 7.0f, 1.0f);
		}
		CHECK(rate.Alive() >= 9 && rate.Alive() <= 10);  // 10 a second for 1 s (smoke outlives the second)
		for (const auto& p : rate.All()) {
			if (p.alive) {
				CHECK(p.x >= 0.0f && p.x <= 200.0f && p.vy < 0.0f);  // on the bar, rising
			}
		}
		rate.Emit(Particle::Kind::kEmber, std::nanf(""), kFrame, 200.0f, 7.0f, 1.0f);
		rate.Emit(Particle::Kind::kEmber, 10.0f, kFrame, std::nanf(""), 7.0f, 1.0f);
		rate.Step(std::nanf(""), 0.0f);
		rate.Clear();
		CHECK(rate.Alive() == 0);
		// the bar style key round-trips and is clamped
		Plugin::Settings st;
		CHECK(Plugin::SettingsText::Apply(st, "BarStyle", 1) && st.barStyle == 1);
		CHECK(Plugin::SettingsText::Apply(st, "BarStyle", 2) && st.barStyle == 2);  // the spell looks (2026-10-13)
		CHECK(Plugin::SettingsText::Apply(st, "BarStyle", 9) && st.barStyle == 2);
		CHECK(Plugin::SettingsText::Apply(st, "BarStyle", -3) && st.barStyle == 0);
		CHECK(Plugin::Settings{}.barStyle == 2);  // the spell bars by default (his order 2026-10-15)
	}

	// ------------------------------------------------------------------ the spell -> look classifier (SpellLook.h, 2026-10-13)
	namespace Looks
	{
		using namespace SpellLook;
		using A = Archetype;

		// one magic effect as the game's records hold it
		Effect Fx(A a_type, Value a_on, Resist a_resist, School a_school, bool a_harmful, Keywords a_keys = {}, Value a_second = Value::kOther)
		{
			Effect e;
			e.archetype = a_type;
			e.primary = a_on;
			e.secondary = a_second;
			e.resist = a_resist;
			e.school = a_school;
			e.harmful = a_harmful;
			e.keys = a_keys;
			return e;
		}

		Look Of(std::initializer_list<Effect> a_effects) { return Classify(std::span<const Effect>(a_effects.begin(), a_effects.size())); }

		constexpr Keywords kFireKey{ true, false, false, false, false };
		constexpr Keywords kFrostKey{ false, true, false, false, false };
		constexpr Keywords kShockKey{ false, false, true, false, false };
		constexpr Keywords kRestoreKey{ false, false, false, true, false };
		constexpr Keywords kWardKey{ false, false, false, false, true };
		constexpr auto     kDes = School::kDestruction, kRes = School::kRestoration, kCon = School::kConjuration;
		constexpr auto     kAlt = School::kAlteration, kIll = School::kIllusion;
	}

	void Classifier()
	{
		using namespace Looks;
		// the elements: keywords first, then what resists the effect (vanilla Flames / Frostbite / Sparks shapes)
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kFire, kDes, true, kFireKey) }) == Look::kFire);
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kFrost, kDes, true, kFrostKey),
				  Fx(A::kValueModifier, Value::kStamina, Resist::kFrost, kDes, true, kFrostKey) }) == Look::kFrost);
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kShock, kDes, true, kShockKey),
				  Fx(A::kValueModifier, Value::kMagicka, Resist::kShock, kDes, true) }) == Look::kShock);
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kFrost, kDes, true) }) == Look::kFrost);  // no keyword: the resistance
		// Mora's Curse (Dragonborn.esm): the fire keyword AND poison resistance - the keyword wins
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kPoison, kDes, true, kFireKey) }) == Look::kFire);
		// poison and disease: the vial (Poison Rune is Restoration, resisted by poison)
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kPoison, kRes, true) }) == Look::kPoison);
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kDisease, kAlt, true) }) == Look::kPoison);
		// sun / holy: Sun Fire and Stendarr's Aura carry no keyword and no resistance (Dawnguard.esm) - a harmful
		// Restoration effect that lowers Health; Turn Undead by its archetype
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kNone, kRes, true) }) == Look::kSun);
		CHECK(Of({ Fx(A::kTurnUndead, Value::kOther, Resist::kNone, kRes, true) }) == Look::kSun);
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kNone, kDes, true) }) == Look::kGeneric);  // the same in Destruction: no element
		// healing: a helpful raise of Health, the keyword, the cures; a ward is NOT healing
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kNone, kRes, false, kRestoreKey) }) == Look::kHealing);
		CHECK(Of({ Fx(A::kPeakValueModifier, Value::kHealth, Resist::kNone, School::kNone, false) }) == Look::kHealing);
		CHECK(Of({ Fx(A::kCureDisease, Value::kOther, Resist::kNone, kRes, false) }) == Look::kHealing);
		CHECK(Of({ Fx(A::kValueModifier, Value::kWard, Resist::kNone, kRes, false, kWardKey) }) == Look::kGeneric);
		CHECK(Of({ Fx(A::kValueModifier, Value::kOther, Resist::kNone, kRes, false) }) == Look::kHealing);  // a Restoration spell of no kind: its school
		// the absorbs, by what they take
		CHECK(Of({ Fx(A::kAbsorb, Value::kHealth, Resist::kNone, kDes, true) }) == Look::kAbsorbHealth);
		CHECK(Of({ Fx(A::kAbsorb, Value::kMagicka, Resist::kNone, kDes, true) }) == Look::kAbsorbMagicka);
		CHECK(Of({ Fx(A::kAbsorb, Value::kStamina, Resist::kNone, kDes, true) }) == Look::kAbsorbStamina);
		CHECK(Of({ Fx(A::kAbsorb, Value::kOther, Resist::kNone, kDes, true) }) == Look::kGeneric);                  // an absorb of something else
		CHECK(Of({ Fx(A::kAbsorb, Value::kHealth, Resist::kFire, kDes, true, kFireKey) }) == Look::kAbsorbHealth);  // the tube before the element
		// conjuration: bound weapons have their own look, before the school
		CHECK(Of({ Fx(A::kBoundWeapon, Value::kOther, Resist::kNone, kCon, false) }) == Look::kBound);
		CHECK(Of({ Fx(A::kSummonCreature, Value::kOther, Resist::kNone, kCon, false) }) == Look::kConjuration);  // a flame atronach too: a summon
		CHECK(Of({ Fx(A::kReanimate, Value::kOther, Resist::kNone, kCon, true) }) == Look::kConjuration);
		CHECK(Of({ Fx(A::kSoulTrap, Value::kOther, Resist::kNone, kCon, true) }) == Look::kConjuration);
		// illusion and alteration, by archetype, then by school
		CHECK(Of({ Fx(A::kFrenzy, Value::kOther, Resist::kNone, kIll, true) }) == Look::kIllusion);
		CHECK(Of({ Fx(A::kInvisibility, Value::kOther, Resist::kNone, kIll, false) }) == Look::kIllusion);
		CHECK(Of({ Fx(A::kValueModifier, Value::kOther, Resist::kNone, kIll, false) }) == Look::kIllusion);  // Muffle: its school
		CHECK(Of({ Fx(A::kParalysis, Value::kOther, Resist::kNone, kAlt, true) }) == Look::kAlteration);
		CHECK(Of({ Fx(A::kLight, Value::kOther, Resist::kNone, kAlt, false) }) == Look::kAlteration);              // Candlelight
		CHECK(Of({ Fx(A::kPeakValueModifier, Value::kOther, Resist::kNone, kAlt, false) }) == Look::kAlteration);  // Oakflesh: its school
		// the costliest effect's school only when no effect says what it is; a later effect that does wins over it
		CHECK(Of({ Fx(A::kValueModifier, Value::kHealth, Resist::kNone, kDes, true), Fx(A::kValueModifier, Value::kOther, Resist::kFrost, kDes, true) }) ==
			  Look::kFrost);
		CHECK(Of({ Fx(A::kValueModifier, Value::kOther, Resist::kNone, kIll, false), Fx(A::kValueModifier, Value::kOther, Resist::kNone, kAlt, false) }) ==
			  Look::kIllusion);
		CHECK(Of({}) == Look::kGeneric);
		CHECK(Of({ Fx(static_cast<A>(9999), Value::kOther, Resist::kOther, School::kNone, false) }) == Look::kGeneric);  // an unknown archetype

		// every look has a name of its own; out of range is generic
		std::set<std::string> names;
		for (std::size_t i = 0; i < kLooks; ++i) {
			names.insert(std::string(Name(static_cast<Look>(i))));
		}
		CHECK(names.size() == kLooks);
		CHECK(Name(static_cast<Look>(200)) == "generic");
	}

	// ------------------------------------------------------------------ the live spell bars (BarFx.h, his order 2026-10-15)
	struct BarRun
	{
		int   maxAlive{ 0 }, arcs{ 0 };
		bool  finite{ true };
		float meanY{ 0.0f }, meanVx{ 0.0f };
		int   inside{ 0 };
	};

	BarRun RunBar(SpellLook::Look a_look, const BarFx::Drive& a_d, float a_secs)
	{
		BarFx::Bar  bar;
		const auto& p = BarFx::ProfileOf(a_look);
		BarRun      r;
		for (int f = 0; f < static_cast<int>(a_secs * 60.0f); ++f) {
			bar.Step(p, a_d, 1.0f / 60.0f, static_cast<float>(f) / 60.0f);
			r.maxAlive = std::max(r.maxAlive, bar.Alive());
			for (const auto& a : bar.Arcs()) {
				r.arcs += a.Alive() ? 1 : 0;
			}
		}
		int n = 0;
		for (const auto& q : bar.All()) {
			if (!q.alive) {
				continue;
			}
			r.finite &= std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.vx) && std::isfinite(q.vy) && std::isfinite(q.Size());
			r.meanY += q.y;
			r.meanVx += q.vx;
			r.inside += q.x >= 0.0f && q.x <= a_d.w && q.y >= 0.0f && q.y <= a_d.h ? 1 : 0;
			++n;
		}
		if (n) {
			r.meanY /= static_cast<float>(n);
			r.meanVx /= static_cast<float>(n);
		}
		return r;
	}

	void SpellBars()
	{
		using namespace BarFx;
		using SpellLook::Look;
		// every look has a profile: something is born, it lives, it reaches round the bar (the 360 part)
		for (std::size_t i = 0; i < SpellLook::kLooks; ++i) {
			const auto& p = ProfileOf(static_cast<Look>(i));
			float       sides = 0.0f;
			for (const float s : p.sides) {
				CHECK(s >= 0.0f);
				sides += s;
			}
			CHECK(sides > 0.0f);
			CHECK(p.rate > 0.0f && p.life0 > 0.0f && p.life0 <= p.life1 && p.size0 > 0.0f && p.size1 > 0.0f);
			CHECK(p.spill >= 1.5f);
			CHECK(p.body >= 0.0f && p.body <= 1.0f);
		}
		CHECK(&ProfileOf(static_cast<Look>(200)) == &ProfileOf(Look::kGeneric));
		// his examples: fire climbs, frost is blown along the bar, shock arcs; the absorbs draw in, their fill flowing back
		CHECK(ProfileOf(Look::kFire).motion == Motion::kRise && ProfileOf(Look::kFire).lift > 0.0f);
		CHECK(ProfileOf(Look::kFrost).motion == Motion::kWind && ProfileOf(Look::kFrost).wind > 50.0f);
		CHECK(ProfileOf(Look::kShock).motion == Motion::kArc);
		CHECK(ProfileOf(Look::kAbsorbHealth).motion == Motion::kInward && ProfileOf(Look::kAbsorbHealth).flow < 0.0f);

		// the flow field: bounded, finite, the same place the same value, nonsense in -> a calm value out
		for (int i = 0; i < 400; ++i) {
			const float x = static_cast<float>(i) * 0.37f - 60.0f, y = static_cast<float>(i) * 0.11f - 9.0f;
			const float n = Noise(x, y);
			CHECK(n >= 0.0f && n <= 1.0f);
			float vx = 0.0f, vy = 0.0f;
			Curl(x, y, vx, vy);
			CHECK(std::isfinite(vx) && std::isfinite(vy) && std::fabs(vx) < 4.0f && std::fabs(vy) < 4.0f);
		}
		CHECK(Near(Noise(std::nanf(""), 1.0f), 0.5f));
		CHECK(Near(Noise(3.3f, 4.7f), Noise(3.3f, 4.7f)));

		Drive full;
		full.fill = 1.0f;
		full.heat = 1.0f;
		full.w = 240.0f;
		full.h = 10.0f;
		// nothing is born on an empty bar
		Drive empty = full;
		empty.fill = 0.0f;
		for (std::size_t i = 0; i < SpellLook::kLooks; ++i) {
			const auto r = RunBar(static_cast<Look>(i), empty, 2.0f);
			CHECK(r.maxAlive == 0 && r.arcs == 0);
		}
		// a full bar in every look: busy, never past the cap, every number finite
		for (std::size_t i = 0; i < SpellLook::kLooks; ++i) {
			const auto r = RunBar(static_cast<Look>(i), full, 4.0f);
			CHECK(r.maxAlive > 10 && r.maxAlive <= kMax);
			CHECK(r.finite);
		}
		// fire rises: its particles live above the bar's middle; frost streams along +x; shock arcs, fire does not
		CHECK(RunBar(Look::kFire, full, 3.0f).meanY < full.h * 0.5f);
		CHECK(RunBar(Look::kFrost, full, 3.0f).meanVx > 10.0f);
		CHECK(RunBar(Look::kShock, full, 3.0f).arcs > 0 && RunBar(Look::kFire, full, 3.0f).arcs == 0);
		// the absorbs are drawn in: some of their particles are inside the bar
		CHECK(RunBar(Look::kAbsorbHealth, full, 3.0f).inside > 0);
		// Overdrive's power: more of them; the cooldown: fewer, and no arcs
		Drive od = full;
		od.power = 2.0f;
		Drive cool = full;
		cool.burnout = true;
		CHECK(RunBar(Look::kFire, od, 3.0f).maxAlive > RunBar(Look::kFire, full, 3.0f).maxAlive);
		CHECK(RunBar(Look::kFire, cool, 3.0f).maxAlive < RunBar(Look::kFire, full, 3.0f).maxAlive);
		CHECK(RunBar(Look::kShock, cool, 3.0f).arcs == 0);
		// nonsense in: no crash, nothing past the cap, all finite
		Drive junk;
		junk.fill = std::nanf("");
		junk.heat = 50.0f;
		junk.power = 99.0f;
		junk.w = -5.0f;
		junk.h = std::nanf("");
		junk.k = 0.0f;
		const auto rj = RunBar(Look::kFrost, junk, 1.0f);
		CHECK(rj.finite && rj.maxAlive <= kMax);
		Bar still;
		still.Step(ProfileOf(Look::kFire), full, std::nanf(""), 0.0f);
		CHECK(still.Alive() == 0);

		// the fill's two layers flow apart, within one texture's width
		float u0 = 0.0f, u1 = 0.0f;
		FlowUv(12.5f, 0.32f, 1.0f, u0, u1);
		CHECK(std::fabs(u0) <= 1.0f && std::fabs(u1) <= 1.0f && !Near(u0, u1));
		FlowUv(std::nanf(""), std::nanf(""), std::nanf(""), u0, u1);
		CHECK(std::isfinite(u0) && std::isfinite(u1));
		// colors: the spell's own when young, grey-blue in the cooldown
		const auto& fire = ProfileOf(Look::kFire);
		const auto  young = ColorOf(fire, { 0.2f, 0.4f, 1.0f }, true, 1.0f, false);
		CHECK(Near(young.r, 0.2f) && Near(young.b, 1.0f));
		const auto cooled = ColorOf(fire, { 1.0f, 0.4f, 0.1f }, true, 0.5f, true);
		CHECK(cooled.b > cooled.r);
	}

	// ------------------------------------------------------------------ the overcharge on the spells (Overload.h)
	void Overcharge()
	{
		using Overload::Phase;
		// growth: a little while heat builds, double at Overdrive's start, falling as it drains (never below full heat's), 1 in burnout
		CHECK(Near(Overload::Power(Phase::kBuilding, 0.0f, 0.0f, 1.0f), 1.0f));
		CHECK(Near(Overload::Power(Phase::kBuilding, 1.0f, 0.0f, 1.0f), 1.25f));
		CHECK(Overload::Power(Phase::kBuilding, 0.5f, 0.0f, 1.0f) < Overload::Power(Phase::kBuilding, 0.8f, 0.0f, 1.0f));
		CHECK(Near(Overload::Power(Phase::kOverdrive, 1.0f, 1.0f, 1.0f), 2.0f));
		CHECK(Near(Overload::Power(Phase::kOverdrive, 1.0f, 0.5f, 1.0f), 1.5f));
		CHECK(Near(Overload::Power(Phase::kOverdrive, 1.0f, 0.0f, 1.0f), 1.25f));
		CHECK(Near(Overload::Power(Phase::kBurnout, 1.0f, 1.0f, 1.0f), 1.0f));
		CHECK(Near(Overload::Power(Phase::kOverdrive, 1.0f, 1.0f, 0.0f), 1.0f));   // growth off: never bigger
		CHECK(Near(Overload::Power(Phase::kOverdrive, 1.0f, 1.0f, 50.0f), 3.0f));  // at most triple
		// the burnout dip (his answer 2026-10-18, (c)): down at the start, whole again at the end, rising the whole way
		CHECK(Near(Overload::BurnoutDip(1.0f), Overload::kDipFloor));
		CHECK(Near(Overload::BurnoutDip(0.0f), 1.0f));
		CHECK(Near(Overload::BurnoutDip(0.5f), (1.0f + Overload::kDipFloor) / 2.0f));
		for (float l = 1.0f; l > 0.0f; l -= 0.05f) {
			CHECK(Overload::BurnoutDip(l - 0.05f) >= Overload::BurnoutDip(l));
		}
		CHECK(Near(Overload::BurnoutDip(std::numeric_limits<float>::quiet_NaN()), 1.0f));
		CHECK(Near(Overload::BurnoutDip(-3.0f), 1.0f) && Near(Overload::BurnoutDip(9.0f), Overload::kDipFloor));
		CHECK(Near(Overload::Power(Phase::kOverdrive, std::nanf(""), std::nanf(""), std::nanf("")), 1.0f));
		// ferocity follows the size; easing never jumps or overshoots
		CHECK(Near(Overload::Ferocity(1.0f), 1.0f) && Near(Overload::Ferocity(2.0f), 1.6f) && Near(Overload::Ferocity(std::nanf("")), 1.0f));
		float v = 1.0f;
		for (int i = 0; i < 30; ++i) {
			const float next = Overload::Ease(v, 2.0f, 6.0f, 1.0f / 60.0f);
			CHECK(next >= v && next <= 2.0f);
			v = next;
		}
		CHECK(v > 1.9f && v < 2.0f);
		CHECK(Near(Overload::Ease(1.3f, 2.0f, 6.0f, 0.0f), 1.3f) && Near(Overload::Ease(std::nanf(""), 2.0f, 6.0f, 0.1f), 2.0f));
		// the sputter: out about four times in five at the burnout's start, about half at its middle, never at its end
		const auto share = [](float a_left, std::uint32_t a_seed) {
			int shows = 0;
			for (int i = 0; i < 2000; ++i) {
				shows += Overload::Shows(static_cast<float>(i) / 16.0f, a_left, a_seed) ? 1 : 0;
			}
			return static_cast<float>(shows) / 2000.0f;
		};
		CHECK(share(1.0f, 7u) > 0.12f && share(1.0f, 7u) < 0.28f);
		CHECK(share(0.5f, 7u) > 0.5f && share(0.5f, 7u) < 0.7f);
		CHECK(Near(share(0.0f, 7u), 1.0f) && Near(share(0.04f, 7u), 1.0f));
		int differ = 0;  // two hands do not sputter in step
		for (int i = 0; i < 200; ++i) {
			differ += Overload::Shows(static_cast<float>(i) / 16.0f, 1.0f, 1u) != Overload::Shows(static_cast<float>(i) / 16.0f, 1.0f, 2u) ? 1 : 0;
		}
		CHECK(differ > 20);
		CHECK(Overload::Shows(std::nanf(""), std::nanf(""), 3u));
		CHECK(Near(Overload::LightWhen(true), 1.0f) && Overload::LightWhen(false) < 0.2f);
	}

	// ------------------------------------------------------------------ the scorch and frost marks (Marks.h)
	void ScorchMarks()
	{
		Marks::Rules               r;  // 6 alive, 8 s, merge 60, radius 80, tick 0.5
		Marks::Pool                pool;
		std::vector<std::uint32_t> ended, ticked;

		// a new mark, then an impact close by of the same element refreshes it; another element or far away is a new mark
		const auto a = pool.Add({ 0, 0, 0 }, 1, r);
		CHECK(a.fresh && a.id != 0 && a.ended == 0);
		pool.Advance(0.2f, r, ended, ticked);
		const auto again = pool.Add({ 30, 30, 0 }, 1, r);  // 42 away: refreshed
		CHECK(!again.fresh && again.id == a.id && Near(pool.Find(a.id)->age, 0.0f));
		CHECK(pool.Add({ 30, 30, 0 }, 2, r).fresh);  // frost beside fire: its own mark
		CHECK(pool.Add({ 500, 0, 0 }, 1, r).fresh);
		CHECK(pool.All().size() == 3);

		// the cap: full, the OLDEST ends first
		Marks::Pool                capped;
		std::vector<std::uint32_t> ids;
		for (int i = 0; i < 6; ++i) {
			ids.push_back(capped.Add({ static_cast<float>(i) * 200.0f, 0, 0 }, 1, r).id);
			capped.Advance(0.25f, r, ended, ticked);
		}
		const auto seventh = capped.Add({ 5000, 0, 0 }, 1, r);
		CHECK(seventh.fresh && seventh.ended == ids[0] && capped.All().size() == 6 && !capped.Find(ids[0]));
		Marks::Rules one = r;
		one.max = 1;
		Marks::Pool single;
		const auto  first = single.Add({ 0, 0, 0 }, 2, one);
		CHECK(single.Add({ 900, 0, 0 }, 2, one).ended == first.id && single.All().size() == 1);
		Marks::Rules none = r;
		none.max = 0;
		CHECK(!single.Add({ 0, 0, 0 }, 1, none).id);
		CHECK(!single.Add({ std::nanf(""), 0, 0 }, 1, r).id);  // a bad point makes nothing

		// the lifetime: 8 s, in uneven frames; the hurt ticks every 0.5 s, never twice in one half second
		Marks::Pool timed;
		const auto  t = timed.Add({ 0, 0, 0 }, 1, r);
		ended.clear();
		ticked.clear();
		float                                 clock = 0.0f, lastTick = -1.0f;
		int                                   ticks = 0;
		bool                                  close = false;
		std::mt19937                          rng(7);
		std::uniform_real_distribution<float> frame(0.004f, 0.06f);
		while (timed.Find(t.id) && clock < 20.0f) {
			const float dt = frame(rng);
			clock += dt;
			ticked.clear();
			timed.Advance(dt, r, ended, ticked);
			if (!ticked.empty()) {
				close = close || (lastTick >= 0.0f && clock - lastTick < 0.43f);  // a frame's overshoot (<= 0.06 s) carries over
				lastTick = clock;
				++ticks;
			}
		}
		CHECK(!timed.Find(t.id) && clock >= 8.0f && clock < 8.1f);
		CHECK(std::ranges::find(ended, t.id) != ended.end());
		CHECK(ticks >= 15 && ticks <= 16);  // 8 s at one tick each 0.5 s
		CHECK(!close);
		std::vector<std::uint32_t> e2, t2;
		Marks::Pool                longFrame;
		longFrame.Add({ 0, 0, 0 }, 1, r);
		longFrame.Advance(5.0f, r, e2, t2);  // a long frame counts as 0.25 s, one tick at most
		CHECK(t2.size() <= 1 && longFrame.All().size() == 1);
		longFrame.Advance(std::nanf(""), r, e2, t2);
		CHECK(longFrame.All().size() == 1);

		// the place (a load door, fast travel): marks left elsewhere end; marks here stay; the same spot in two places is two marks
		Marks::Pool placed;
		const auto  inside = placed.Add({ 0, 0, 0 }, 1, r, 0x1000000AAull);
		const auto  outside = placed.Add({ 0, 0, 0 }, 1, r, 0x20000003Cull);
		CHECK(inside.fresh && outside.fresh && inside.id != outside.id);
		CHECK(!placed.Add({ 10, 0, 0 }, 1, r, 0x1000000AAull).fresh);  // the same place: refreshed
		std::vector<std::uint32_t> gone;
		placed.DropElsewhere(0x20000003Cull, gone);
		CHECK(gone.size() == 1 && gone[0] == inside.id && placed.Find(outside.id) && !placed.Find(inside.id));
		gone.clear();
		placed.DropElsewhere(0x20000003Cull, gone);
		CHECK(gone.empty() && placed.All().size() == 1);
		placed.DropElsewhere(0x1000000BBull, gone);
		CHECK(gone.size() == 1 && placed.All().empty());

		// the reach: across and up / down
		CHECK(Marks::Inside({ 0, 0, 0 }, { 50, 50, 10 }, 80.0f));
		CHECK(!Marks::Inside({ 0, 0, 0 }, { 70, 70, 0 }, 80.0f));  // 99 across
		CHECK(!Marks::Inside({ 0, 0, 0 }, { 0, 0, 120 }, 80.0f));  // on a ledge above
		CHECK(!Marks::Inside({ 0, 0, 0 }, { std::nanf(""), 0, 0 }, 80.0f));
		CHECK(!Marks::Inside({ 0, 0, 0 }, { 0, 0, 0 }, 0.0f));

		// the hurt: per second x tick, x1.5 in Overdrive, less the resistance (capped at 85 %)
		CHECK(Near(Marks::Hurt(5.0f, 0.5f, false, 0.0f), 2.5f));
		CHECK(Near(Marks::Hurt(5.0f, 0.5f, true, 0.0f), 3.75f));
		CHECK(Near(Marks::Hurt(5.0f, 0.5f, false, 50.0f), 1.25f));
		CHECK(Near(Marks::Hurt(5.0f, 0.5f, false, 100.0f), 2.5f * 0.15f));  // never fully immune
		CHECK(Near(Marks::Hurt(5.0f, 0.5f, false, -40.0f), 2.5f));          // a weakness counts as none
		CHECK(Near(Marks::Hurt(0.0f, 0.5f, true, 0.0f), 0.0f));
		CHECK(Near(Marks::Hurt(std::nanf(""), 0.5f, true, 0.0f), 0.0f) && Near(Marks::Hurt(5.0f, 0.5f, false, std::nanf("")), 2.5f));

		// the settings: on by default, the ini keys clamp
		Plugin::Settings s;
		CHECK(s.marks && s.markHurt && s.markMax == 6 && Near(s.markSeconds, 8.0f) && Near(s.markHurtPerSecond, 5.0f));
		CHECK(Plugin::SettingsText::Apply(s, "MarkMax", 99) && s.markMax == 20);
		CHECK(Plugin::SettingsText::Apply(s, "MarkSeconds", 1) && Near(s.markSeconds, 2.0f));
		CHECK(Plugin::SettingsText::Apply(s, "MarkHurtPerSecond", -3) && Near(s.markHurtPerSecond, 0.0f));
		CHECK(Plugin::SettingsText::Apply(s, "markhurt", 0) && !s.markHurt);
	}
}

int main()
{
	Gains();
	Stages();
	Drain();
	SchoolSwitch();
	Overdrive();
	Bench();
	Random();
	Counting();
	SlowMagicka();
	HandLights();
	SettingsFile();
	FlameBar();
	Classifier();
	SpellBars();
	Overcharge();
	ScorchMarks();
	std::printf("%d passed, %d failed\n", gPassed, gFailed);
	return gFailed == 0 ? 0 : 1;
}

// Spell Mastery Heat - tests for src/Heat.h, src/Scaled.h, src/SettingsText.h and src/IniText.h (no game needed).
// Copyright (C) 2026 izzydoingit. GPL-3.0-or-later.
//
// tests/run.sh builds and runs them under g++ and clang++ with the sanitizers; build.bat runs them through xmake.

#include "Heat.h"
#include "Scaled.h"
#include "SettingsText.h"
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
		CHECK(Near(m.ChargeOf(t, School::kDestruction), 8.0f));
		m.Cast(t, School::kDestruction, 50.0f);  // the same school: +50%
		CHECK(Near(m.ChargeOf(t, School::kDestruction), 20.0f));
		CHECK(m.Last() == School::kDestruction);

		// charge speed scales it
		Tuning fast = t;
		fast.gain = 2.0f;
		Meter f;
		f.Cast(fast, School::kDestruction, 50.0f);
		CHECK(Near(f.ChargeOf(fast, School::kDestruction), 16.0f));

		// a held spell builds per second, at its own (lower) rate, and never jumps
		Meter h;
		for (int i = 0; i < 60; ++i) {
			h.Hold(t, School::kDestruction, 50.0f, kFrame);
		}
		const float second = h.ChargeOf(t, School::kDestruction);
		CHECK(second > 4.0f && second < 8.0f * 0.6f * 1.5f + 0.01f);
		Meter bad;
		bad.Hold(t, School::kDestruction, 50.0f, std::nanf(""));
		bad.Hold(t, School::kDestruction, 50.0f, -1.0f);
		bad.Cast(t, School::kDestruction, -5.0f);  // a nonsense cost still counts as the smallest cast
		CHECK(Near(bad.ChargeOf(t, School::kDestruction), 4.0f));
	}

	// the Heat bar: what a cast and a held spell add, the same element and a dual cast, the cooling - and that it shows how FAST
	// you cast
	void HeatBar()
	{
		Tuning t;
		Meter  m;
		m.Cast(t, School::kDestruction, 50.0f, 1);  // 8 x 1.5
		CHECK(Near(m.HeatNow(), 12.0f));
		m.Cast(t, School::kDestruction, 50.0f, 1);  // the same element: x 1.25
		CHECK(Near(m.HeatNow(), 27.0f));
		m.Cast(t, School::kDestruction, 50.0f, 2);  // another element
		CHECK(Near(m.HeatNow(), 39.0f));
		m.Cast(t, School::kDestruction, 50.0f, 0);  // no element: never "the same"
		m.Cast(t, School::kDestruction, 50.0f, 0);
		CHECK(Near(m.HeatNow(), 63.0f));
		Meter d;
		d.Cast(t, School::kDestruction, 50.0f, 1, true);  // a dual cast: x 1.5
		CHECK(Near(d.HeatNow(), 18.0f));
		Tuning up = t;
		up.heatUp = 2.0f;
		Meter u;
		u.Cast(up, School::kDestruction, 50.0f, 1);
		CHECK(Near(u.HeatNow(), 24.0f));
		Meter capped;
		for (int i = 0; i < 20; ++i) {
			capped.Cast(t, School::kDestruction, 500.0f, 1);
		}
		CHECK(Near(capped.HeatNow(), 100.0f));

		// the cooling: a quarter of what it holds a second (x Heat cools); never below 0, and snapped to 0 near it
		Meter c;
		c.SetHeat(80.0f);
		Run(c, t, 1.0f);
		CHECK(Near(c.HeatNow(), 80.0f * std::exp(-0.25f), 0.6f));
		Tuning down = t;
		down.heatDown = 2.0f;
		Meter c2;
		c2.SetHeat(80.0f);
		Run(c2, down, 1.0f);
		CHECK(Near(c2.HeatNow(), 80.0f * std::exp(-0.5f), 0.6f));
		Run(c, t, 60.0f);
		CHECK(c.HeatNow() == 0.0f);

		// the rate: a 50-magicka fire spell every second settles in the sweet spot; every half second boils into the red; every
		// two seconds stays cool
		const auto settle = [&t](float a_every, float& a_peak, float& a_low) {
			Meter  r;
			Tuning nogo = t;
			nogo.mode = OverdriveMode::kOff;  // the charge must not start Overdrive here
			a_peak = 0.0f;
			a_low = 100.0f;
			for (float s = 0.0f, next = 0.0f; s < 40.0f; s += kFrame) {
				if (s >= next) {
					r.Cast(nogo, School::kDestruction, 50.0f, 1);
					next += a_every;
				}
				r.Tick(nogo, kFrame);
				if (s > 30.0f) {
					a_peak = std::max(a_peak, r.HeatNow());
					a_low = std::min(a_low, r.HeatNow());
				}
			}
		};
		float peak = 0.0f, low = 0.0f;
		settle(1.0f, peak, low);
		CHECK(peak > 60.0f && peak < 75.0f && low > 45.0f && low < 60.0f);
		settle(0.5f, peak, low);
		CHECK(peak >= 99.0f && low > 85.0f);
		settle(2.0f, peak, low);
		CHECK(peak < 45.0f);

		// a held stream (Flames, about 14 magicka a second) sits in the sweet spot; one in each hand boils over
		const auto held = [&t](int a_hands) {
			Meter  r;
			Tuning nogo = t;
			nogo.mode = OverdriveMode::kOff;
			for (float s = 0.0f; s < 30.0f; s += kFrame) {
				for (int i = 0; i < a_hands; ++i) {
					r.Hold(nogo, School::kDestruction, 14.0f, kFrame, 1);
				}
				r.Tick(nogo, kFrame);
			}
			return r.HeatNow();
		};
		CHECK(held(1) > 52.0f && held(1) < 72.0f);
		CHECK(held(2) > 95.0f);

		// the stages and the bonus are the heat's, whatever the school
		Meter s;
		s.SetHeat(60.0f);
		CHECK(s.StageNow(t) == Stage::kHot);
		CHECK(Near(s.Bonus(t), 1.08f));
		Tuning per = t;
		per.perSchool = true;
		s.Cast(per, School::kIllusion, 50.0f);
		CHECK(Near(s.Bonus(per), 1.08f));
		Tuning off = t;
		off.bonusOn = false;  // the damage bonus switched off
		CHECK(Near(s.Bonus(off), 1.0f));
		CHECK(!s.RedZone(t));
		s.SetHeat(90.0f);
		CHECK(s.RedZone(t) && s.StageNow(t) == Stage::kOverheating);
	}

	// how Heat lets a full Overdrive bar go
	void Gates()
	{
		const auto full = [](const Tuning& a_t, float a_heat) {
			Meter m;
			m.SetCharge(a_t, kFull);
			m.SetHeat(a_heat);
			return m;
		};

		// the sweet spot on automatic: cold waits, the band goes, too hot waits until it has cooled into the band - never a misfire
		Tuning t;
		auto   cold = full(t, 30.0f);
		CHECK(cold.Gated(t) == GateNow::kCold && cold.Ready(t));
		Run(cold, t, 0.5f);
		CHECK(cold.Now() == Meter::State::kBuilding);
		auto band = full(t, 60.0f);
		band.Tick(t, kFrame);
		CHECK(band.Now() == Meter::State::kOverdrive);
		Run(band, t, 3.0f);  // the heat is spent with the burst: half of it left at its middle
		CHECK(Near(band.HeatNow(), 30.0f, 1.5f));
		Run(band, t, 3.1f);
		CHECK(band.Now() == Meter::State::kCooldown && band.HeatNow() == 0.0f);
		band.Cast(t, School::kDestruction, 300.0f, 1);  // nothing builds in the burnout
		CHECK(band.HeatNow() == 0.0f && band.Charge(t) == 0.0f);
		auto hot = full(t, 95.0f);
		CHECK(hot.Gated(t) == GateNow::kHot && hot.RedZone(t));
		Run(hot, t, 0.2f);
		CHECK(hot.Now() == Meter::State::kBuilding && hot.Misfires() == 0);
		Run(hot, t, 1.0f);  // 95 cools below 85 in under half a second
		CHECK(hot.Now() == Meter::State::kOverdrive);

		// the sweet spot on the key: cold does nothing, too hot misfires (heat down a stage, still building), the band goes
		Tuning key = t;
		key.mode = OverdriveMode::kKey;
		auto k = full(key, 30.0f);
		CHECK(k.PressKey(key) == Release::kCold && k.Now() == Meter::State::kBuilding);
		k.SetHeat(95.0f);
		CHECK(k.PressKey(key) == Release::kMisfire);
		CHECK(Near(k.HeatNow(), 70.0f) && k.Misfires() == 1 && k.Now() == Meter::State::kBuilding && k.Ready(key));
		CHECK(k.PressKey(key) == Release::kStarted && k.Now() == Meter::State::kOverdrive);
		Tuning calm = key;
		calm.misfire = false;  // misfires off: too hot simply does nothing
		auto nomiss = full(calm, 95.0f);
		CHECK(nomiss.PressKey(calm) == Release::kHot && Near(nomiss.HeatNow(), 95.0f) && nomiss.Misfires() == 0);
		Tuning narrow = key;  // Aedra's band
		narrow.bandLow = 60.0f;
		narrow.redLine = 80.0f;
		auto a = full(narrow, 55.0f);
		CHECK(a.PressKey(narrow) == Release::kCold);
		a.SetHeat(82.0f);
		CHECK(a.PressKey(narrow) == Release::kMisfire);

		// vent: below the red line it goes, cold or not; in the red a press waits for the heat to fall, then goes by itself
		Tuning vent = key;
		vent.gate = Gate::kVent;
		auto v = full(vent, 10.0f);
		CHECK(v.Gated(vent) == GateNow::kOpen && v.PressKey(vent) == Release::kStarted);
		auto vh = full(vent, 95.0f);
		CHECK(vh.PressKey(vent) == Release::kWaiting && vh.Waiting());
		Run(vh, vent, 0.2f);
		CHECK(vh.Now() == Meter::State::kBuilding);
		Run(vh, vent, 1.0f);
		CHECK(vh.Now() == Meter::State::kOverdrive && vh.Misfires() == 0);
		Tuning ventAuto = vent;
		ventAuto.mode = OverdriveMode::kAutomatic;
		auto va = full(ventAuto, 95.0f);
		Run(va, ventAuto, 0.2f);
		CHECK(va.Now() == Meter::State::kBuilding);
		Run(va, ventAuto, 1.0f);
		CHECK(va.Now() == Meter::State::kOverdrive);
		auto wait = full(vent, 95.0f);  // a wait ends if the bar stops being full
		CHECK(wait.PressKey(vent) == Release::kWaiting);
		wait.SetCharge(vent, 50.0f);
		Run(wait, vent, 2.0f);
		CHECK(!wait.Waiting() && wait.Now() == Meter::State::kBuilding);

		// focus: the band's bottom or hotter, then the focus held for its time; letting go starts it over; too cold never fills
		Tuning focus = key;
		focus.gate = Gate::kFocus;
		auto fo = full(focus, 60.0f);
		CHECK(fo.PressKey(focus) == Release::kFocus && fo.Gated(focus) == GateNow::kFocus);
		for (int i = 0; i < 30; ++i) {
			fo.Focus(focus, true, kFrame);
		}
		CHECK(fo.Now() == Meter::State::kBuilding && fo.FocusHeld() > 0.45f);
		fo.Focus(focus, false, kFrame);
		CHECK(fo.FocusHeld() == 0.0f);
		for (int i = 0; i < 70 && fo.Now() == Meter::State::kBuilding; ++i) {
			fo.Focus(focus, true, kFrame);
		}
		CHECK(fo.Now() == Meter::State::kOverdrive);
		auto fc = full(focus, 30.0f);
		for (int i = 0; i < 120; ++i) {
			fc.Focus(focus, true, kFrame);
		}
		CHECK(fc.Now() == Meter::State::kBuilding && fc.FocusHeld() == 0.0f);
		Tuning focusAuto = focus;  // automatic waits for the focus too (two charged hands)
		focusAuto.mode = OverdriveMode::kAutomatic;
		auto fa = full(focusAuto, 60.0f);
		Run(fa, focusAuto, 2.0f);
		CHECK(fa.Now() == Meter::State::kBuilding);

		// the gate off: heat does not matter
		Tuning open = key;
		open.heatGate = false;
		auto o = full(open, 0.0f);
		CHECK(o.Gated(open) == GateNow::kOpen && o.PressKey(open) == Release::kStarted);
		auto o2 = full(open, 99.0f);
		CHECK(o2.PressKey(open) == Release::kStarted && o2.Misfires() == 0);

		// not full, Overdrive off, or in Overdrive: the key does nothing
		Meter empty;
		CHECK(empty.PressKey(key) == Release::kNotReady && !empty.Ready(key));
		Tuning none = key;
		none.mode = OverdriveMode::kOff;
		auto n = full(none, 60.0f);
		CHECK(n.PressKey(none) == Release::kNotReady && !n.Ready(none));
		Run(n, none, 1.0f);
		CHECK(n.Now() == Meter::State::kBuilding);
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
		CHECK(Near(t.drain, 5.0f) && Near(t.grace, 3.0f));  // the Overdrive bar drains slowly by default
		t.drain = 10.0f;                                    // the numbers below: 10 a second after 2 s
		t.grace = 2.0f;
		Meter m;
		m.Cast(t, School::kDestruction, 50.0f);
		m.Cast(t, School::kDestruction, 50.0f);  // 20
		Run(m, t, 1.9f);
		CHECK(Near(m.Charge(t), 20.0f));  // within the grace period: nothing drains
		Run(m, t, 1.1f);                  // 1 s of drain at 10 a second
		CHECK(Near(m.Charge(t), 10.0f, 0.4f));
		Run(m, t, 5.0f);
		CHECK(Near(m.Charge(t), 0.0f));  // never below zero
		Run(m, t, 40.0f);                // the heat of those two casts has cooled away too
		CHECK(m.Idle());
		// a cast resets the grace period (the last cast's school is still remembered: both get the same-school bonus)
		m.Cast(t, School::kDestruction, 50.0f);
		Run(m, t, 1.5f);
		m.Cast(t, School::kDestruction, 50.0f);
		Run(m, t, 1.5f);
		CHECK(Near(m.Charge(t), 24.0f));
		// a paused game (no time) drains nothing and cools nothing
		const float heat = m.HeatNow();
		for (int i = 0; i < 1000; ++i) {
			m.Tick(t, 0.0f);
		}
		CHECK(Near(m.Charge(t), 24.0f) && Near(m.HeatNow(), heat));
	}

	void SchoolSwitch()
	{
		Tuning t;
		Meter  m;
		for (int i = 0; i < 5; ++i) {
			m.Cast(t, School::kDestruction, 50.0f);  // 8 + 12 x 4 = 56
		}
		CHECK(Near(m.Charge(t), 56.0f));
		m.Cast(t, School::kConjuration, 50.0f);  // loses 25, then +8 (no same-school bonus)
		CHECK(Near(m.Charge(t), 39.0f));
		// switch cooling 0: nothing lost
		Tuning keep = t;
		keep.switchLoss = 0.0f;
		Meter k;
		k.Cast(keep, School::kDestruction, 50.0f);
		k.Cast(keep, School::kIllusion, 50.0f);
		CHECK(Near(k.Charge(keep), 16.0f));

		// one Overdrive bar per school: each builds on its own; switching cools the one being left; the heat stays shared
		Tuning per = t;
		per.perSchool = true;
		Meter p;
		for (int i = 0; i < 5; ++i) {
			p.Cast(per, School::kDestruction, 50.0f);
		}
		p.Cast(per, School::kRestoration, 50.0f);
		CHECK(Near(p.ChargeOf(per, School::kDestruction), 31.0f));
		CHECK(Near(p.ChargeOf(per, School::kRestoration), 8.0f));
		CHECK(Near(p.Charge(per), 8.0f));  // the bar shows the last school's
		CHECK(Near(p.HeatNow(), 72.0f));   // six casts' heat, one bar
		CHECK(Near(p.Bonus(per), 1.08f));  // Hot, whatever the school

		// a full school's bar is ready only while it is the one shown: never READY over another school's partly filled bar
		Tuning perKey = per;
		perKey.mode = OverdriveMode::kKey;
		perKey.heatGate = false;
		Meter q;
		q.SetCharge(perKey, 0.0f);
		q.Cast(perKey, School::kDestruction, 50.0f);
		q.SetCharge(perKey, 100.0f);
		CHECK(q.Ready(perKey));
		q.Cast(perKey, School::kAlteration, 50.0f);
		CHECK(!q.Ready(perKey) && q.PressKey(perKey) == Release::kNotReady);
		CHECK(q.Now() == Meter::State::kBuilding);
	}

	void Overdrive()
	{
		// with the Heat gate off, the Overdrive bar alone: automatic, at 100 it starts, lasts the burst, then the cooldown locks it at 0
		Tuning t;
		t.heatGate = false;
		Meter m;
		int   casts = 0;
		while (m.Now() == Meter::State::kBuilding && casts < 100) {
			m.Cast(t, School::kDestruction, 50.0f);
			++casts;
		}
		CHECK(m.Now() == Meter::State::kOverdrive);
		CHECK(casts == 9);  // 8 + 12 x 8 = 104
		CHECK(m.StageNow(t) == Stage::kOverdrive);
		CHECK(Near(m.Bonus(t), 1.25f));  // every spell, whatever its school
		Run(m, t, 3.0f);
		CHECK(m.Now() == Meter::State::kOverdrive);
		CHECK(Near(m.Charge(t), 100.0f));  // no drain while it lasts
		Run(m, t, 3.1f);
		CHECK(m.Now() == Meter::State::kCooldown);
		CHECK(Near(m.Charge(t), 0.0f));
		CHECK(Near(m.MagickaRegen(t), 0.4f));     // slow magicka: on by default
		CHECK(Near(m.Bonus(t), 1.0f));            // weaker spells: off by default
		m.Cast(t, School::kDestruction, 500.0f);  // nothing can build
		CHECK(Near(m.Charge(t), 0.0f));
		Run(m, t, 9.5f);
		CHECK(m.Now() == Meter::State::kCooldown);
		Run(m, t, 0.6f);
		CHECK(m.Now() == Meter::State::kBuilding);
		CHECK(Near(m.MagickaRegen(t), 1.0f));
		m.Cast(t, School::kDestruction, 50.0f);
		CHECK(Near(m.Charge(t), 12.0f));  // builds again (the same school as before: +50%)

		// the penalties, each optional
		Tuning harsh = t;
		harsh.weaker = true;
		harsh.slowMagicka = false;
		Meter h;
		h.ForceStage(harsh, Stage::kOverdrive);
		Run(h, harsh, 6.1f);
		CHECK(h.Now() == Meter::State::kCooldown);
		CHECK(Near(h.Bonus(harsh), 0.85f));
		CHECK(Near(h.MagickaRegen(harsh), 1.0f));

		// no cooldown at all: straight back to building
		Tuning none = t;
		none.cooldown = 0.0f;
		Meter n;
		n.ForceStage(none, Stage::kOverdrive);
		Run(n, none, 6.1f);
		CHECK(n.Now() == Meter::State::kBuilding);

		// key press: it waits, full, until the key; the key does nothing before
		Tuning key = t;
		key.mode = OverdriveMode::kKey;
		key.drain = 10.0f;
		key.grace = 2.0f;
		Meter k;
		CHECK(k.PressKey(key) == Release::kNotReady);
		for (int i = 0; i < 20; ++i) {
			k.Cast(key, School::kDestruction, 50.0f);
		}
		CHECK(k.Now() == Meter::State::kBuilding);
		CHECK(k.Ready(key));
		CHECK(k.StageNow(key) == Stage::kOverheating);  // twenty casts' heat, none of it cooled
		CHECK(Near(k.Bonus(key), 1.12f));
		CHECK(k.PressKey(key) == Release::kStarted);
		CHECK(k.Now() == Meter::State::kOverdrive);
		CHECK(k.PressKey(key) == Release::kNotReady);  // not twice
		// left to drain below full, it is no longer ready
		Meter c;
		for (int i = 0; i < 20; ++i) {
			c.Cast(key, School::kDestruction, 50.0f);
		}
		Run(c, key, 2.5f);
		CHECK(!c.Ready(key));
		CHECK(c.PressKey(key) == Release::kNotReady);
		// the key in automatic or off mode does nothing
		Meter a;
		CHECK(a.PressKey(t) == Release::kNotReady);

		// off: the bar tops out full for ever
		Tuning off = t;
		off.mode = OverdriveMode::kOff;
		Meter o;
		for (int i = 0; i < 50; ++i) {
			o.Cast(off, School::kDestruction, 50.0f);
		}
		CHECK(o.Now() == Meter::State::kBuilding && Near(o.Charge(off), 100.0f));
		CHECK(!o.Ready(off) && o.PressKey(off) == Release::kNotReady);

		// held spells take far longer than casts to fill the bar (Flames, about 14 magicka a second)
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
		m.SetHeat(60.0f);
		CHECK(m.StageNow(t) == Stage::kHot);
		m.SetHeat(500.0f);
		CHECK(Near(m.HeatNow(), 100.0f));
		m.SetHeat(std::nanf(""));
		CHECK(Near(m.HeatNow(), 0.0f));
		m.SetCharge(t, 70.0f);
		CHECK(Near(m.Charge(t), 70.0f));
		m.SetCharge(t, -4.0f);
		CHECK(Near(m.Charge(t), 0.0f));
		for (auto s : { Stage::kCold, Stage::kWarm, Stage::kHot, Stage::kOverheating, Stage::kOverdrive }) {
			m.ForceStage(t, s);  // Overdrive starts whatever the gate says
			CHECK(m.StageNow(t) == s);
		}
		m.SetHeat(30.0f);  // ends a forced Overdrive
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
		int                                   started = 0, misfired = 0;
		for (int run = 0; run < 48; ++run) {
			Tuning t;
			t.mode = static_cast<OverdriveMode>(run % 3);
			t.perSchool = run % 2 == 0;
			t.weaker = run % 4 < 2;
			t.heatGate = run % 8 != 7;
			t.gate = static_cast<Gate>((run / 3) % 3);
			t.misfire = run % 5 != 0;
			Meter  m;
			School school = School::kDestruction;
			int    element = 1;
			for (int i = 0; i < 20000; ++i) {
				if (u(rng) < 0.01f) {  // a mage keeps to a school and an element for a while, so the bar can fill
					school = static_cast<School>(static_cast<int>(u(rng) * 6.0f) % 6);
					element = static_cast<int>(u(rng) * 4.0f) % 4;
				}
				const float r = u(rng);
				if (r < 0.03f) {
					m.Cast(t, school, u(rng) * 300.0f, element, u(rng) < 0.2f);
				} else if (r < 0.08f) {
					m.Hold(t, school, u(rng) * 40.0f, kFrame, element);
				} else if (r < 0.085f) {
					const auto said = m.PressKey(t);
					started += said == Release::kStarted ? 1 : 0;
					misfired += said == Release::kMisfire ? 1 : 0;
				}
				m.Focus(t, u(rng) < 0.5f, kFrame);
				m.Tick(t, u(rng) < 0.01f ? 0.3f : kFrame);
				for (int s = 0; s < 6; ++s) {
					const float c = m.ChargeOf(t, static_cast<School>(s));
					ok &= std::isfinite(c) && c >= 0.0f && c <= 100.0f;
				}
				const float h = m.HeatNow(), b = m.Bonus(t);
				ok &= std::isfinite(h) && h >= 0.0f && h <= 100.0f;
				ok &= b >= 0.85f - 1e-6f && b <= 1.25f + 1e-6f;
				ok &= m.MagickaRegen(t) >= 0.4f - 1e-6f && m.MagickaRegen(t) <= 1.0f;
				ok &= std::isfinite(m.FocusHeld()) && m.FocusHeld() >= 0.0f;
			}
		}
		CHECK(ok);
		CHECK(started > 0 && misfired > 0);  // the random sessions reached both
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
		s.tuning.heatGate = false;
		s.tuning.gate = Gate::kFocus;
		s.tuning.bandLow = 40.0f;
		s.tuning.redLine = 90.0f;
		s.tuning.heatUp = 1.3f;
		s.tuning.heatDown = 0.8f;
		s.tuning.heatPerCast = 2.0f;
		s.tuning.heatHeld = 4.0f;
		s.tuning.heatSameElement = 0.5f;
		s.tuning.heatDual = 1.0f;
		s.tuning.cool = 0.3f;
		s.tuning.misfire = false;
		s.tuning.misfireDrop = 40.0f;
		s.tuning.backlash = true;
		s.tuning.backlashMagicka = 0.2f;
		s.tuning.focusSeconds = 1.5f;
		s.tuning.bonusOn = false;
		// both files, each with only its own keys; read back together they give every setting
		std::stringstream menu, advanced;
		SettingsText::WriteMenu(menu, s);
		SettingsText::WriteAdvanced(advanced, s);
		std::stringstream both(menu.str() + advanced.str());
		Settings          back;
		const auto        r = SettingsText::Read(both, back);
		CHECK(back == s);
		CHECK(r.problems.empty());
		CHECK(r.taken == static_cast<int>(std::size(SettingsText::kKeys)));
		CHECK(menu.str().find("ChargeSpeed") == std::string::npos && menu.str().find("[Settings]\nEnabled=0") != std::string::npos);
		CHECK(advanced.str().find("Enabled") == std::string::npos && advanced.str().find("[Heat]\n") != std::string::npos);
		CHECK(advanced.str().find("; how fast Heat climbs, percent (25 to 300)\nHeatUp=130\n") != std::string::npos);  // each line explained
		Settings           fromAdvanced;                                                                               // the advanced file alone sets none of the menu's keys
		std::istringstream advancedOnly(advanced.str());
		SettingsText::Read(advancedOnly, fromAdvanced);
		CHECK(fromAdvanced.enabled && Near(fromAdvanced.tuning.heatUp, 1.3f));

		std::istringstream edited("[Settings]\nGainSpeed=9999\nOverdrive=5\nOverdriveKey=-4\nDrainSpeed=1.5\nNope=3\nBarWidth=10\n");
		Settings           e;
		const auto         re = SettingsText::Read(edited, e);
		CHECK(Near(e.tuning.gain, 3.0f));
		CHECK(e.tuning.mode == OverdriveMode::kOff);
		CHECK(e.overdriveKey == 0);
		CHECK(Near(e.tuning.drain, 5.0f));
		CHECK(Near(e.barWidth, 0.5f));
		CHECK(re.problems.size() == 2);

		// a key counts in any section; an unknown key in one of ours is reported; another mod's section is left alone quietly
		std::istringstream sections("[Heat]\nRedLine=70\nBarY=20\nNope=1\n[Overdrive]\nBurstSeconds=9\n[Someone Else]\nNope=2\nRedLine=10\n");
		Settings           sec;
		const auto         rsec = SettingsText::Read(sections, sec);
		CHECK(Near(sec.tuning.redLine, 70.0f) && Near(sec.barY, 0.2f) && Near(sec.tuning.burst, 9.0f));
		CHECK(rsec.problems.size() == 1 && rsec.taken == 3);

		// an older one-file ini (his: GainSpeed 300, BlazingAt, BarY 88, BarStyle 0) still reads: Blazing is Overheating, the one
		// meter's gain speed is the Overdrive bar's, neither old name is written again, and the old bar style is skipped quietly
		std::istringstream old("; Spell Mastery Heat\n[Settings]\nGainSpeed=300\nBlazingAt=80\nBarY=88\nBarStyle=0\nDrainSpeed=10\n");
		Settings           o;
		const auto         ro = SettingsText::Read(old, o);
		CHECK(Near(o.tuning.gain, 3.0f) && Near(o.tuning.at[2], 80.0f) && Near(o.barY, 0.88f) && Near(o.tuning.drain, 10.0f));
		CHECK(ro.problems.empty() && ro.taken == 4);
		CHECK(SettingsText::Apply(o, "blazingat", 70) && Near(o.tuning.at[2], 70.0f));
		const std::string all = menu.str() + advanced.str();
		CHECK(all.find("OverheatingAt=") != std::string::npos && all.find("Blazing") == std::string::npos && all.find("GainSpeed") == std::string::npos);
		CHECK(StageName(Stage::kOverheating) == "Overheating");
		CHECK(SettingsText::Apply(o, "FocusMs", 99999) && Near(o.tuning.focusSeconds, 5.0f));
		CHECK(SettingsText::Apply(o, "Gate", 7) && o.tuning.gate == Gate::kFocus);
		CHECK(SettingsText::Apply(o, "BandLow", -10) && Near(o.tuning.bandLow, 0.0f));

		// the presets, easy to hard: the defaults ARE Dragonborn; each lands on itself; one changed value reads Custom
		CHECK(std::size(SettingsText::kPresets) == 3);
		CHECK(std::string(SettingsText::kPresets[0].name) == "Milk Drinker" && std::string(SettingsText::kPresets[1].name) == "Dragonborn" &&
			  std::string(SettingsText::kPresets[2].name) == "Aedra");
		CHECK(SettingsText::PresetOf(Settings{}) == std::optional<std::size_t>{ 1 });
		for (std::size_t i = 0; i < 3; ++i) {
			Settings p;
			p.tuning.burst = 29.0f;  // any start point
			SettingsText::ApplyPreset(p, i);
			CHECK(SettingsText::PresetOf(p) == std::optional<std::size_t>{ i });
			for (const auto& [key, value] : SettingsText::kPresets[i].values) {
				Settings probe;
				CHECK(SettingsText::Apply(probe, key, value));  // every key is a real one
			}
			for (std::size_t j = 0; j < SettingsText::kPresets[i].values.size(); ++j) {  // the same keys, in the same order, in all three
				CHECK(std::string(SettingsText::kPresets[i].values[j].first) == SettingsText::kPresets[0].values[j].first);
			}
		}
		Settings milk;
		SettingsText::ApplyPreset(milk, 0);
		CHECK(milk.tuning.gate == Gate::kVent && !milk.tuning.misfire && Near(milk.tuning.heatUp, 0.7f) && Near(milk.tuning.heatDown, 1.3f) &&
			  Near(milk.tuning.gain, 1.3f) && Near(milk.tuning.burst, 8.0f) && Near(milk.tuning.cooldown, 6.0f) && !milk.tuning.slowMagicka && !milk.tuning.weaker);
		Settings aedra;
		SettingsText::ApplyPreset(aedra, 2);
		CHECK(aedra.tuning.gate == Gate::kSweetSpot && Near(aedra.tuning.bandLow, 60.0f) && Near(aedra.tuning.redLine, 80.0f) && aedra.tuning.backlash &&
			  Near(aedra.tuning.slowMagickaTo, 0.25f) && aedra.tuning.weaker && aedra.enemies && Near(aedra.tuning.burst, 5.0f) && Near(aedra.tuning.cooldown, 14.0f));
		CHECK(Near(milk.tuning.bonusPerStage, aedra.tuning.bonusPerStage) && Near(milk.tuning.overdriveBonus, aedra.tuning.overdriveBonus));  // the same reward
		aedra.tuning.heatUp = 1.2f;
		CHECK(!SettingsText::PresetOf(aedra).has_value());  // Custom
		SettingsText::ApplyPreset(aedra, 99);               // out of range: nothing changes
		CHECK(!SettingsText::PresetOf(aedra).has_value());

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

		// the two settings files that ship are exactly what the plugin writes for the built-in defaults (made with
		// test_heat --write-menu / --write-advanced), and read back to them
		std::string here = __FILE__;
		here = here.substr(0, here.find_last_of("/\\") + 1);
		const auto shippedText = [&here](const char* a_name) {
			std::ifstream     in(here + "../package/SKSE/Plugins/" + a_name, std::ios::binary);
			std::stringstream text;
			text << in.rdbuf();
			return std::pair{ in.good() || in.eof(), text.str() };
		};
		const auto [menuOk, shippedMenu] = shippedText("SpellMasteryHeat.ini");
		const auto [advancedOk, shippedAdvanced] = shippedText("SpellMasteryHeat Advanced.ini");
		std::stringstream defaultMenu, defaultAdvanced;
		SettingsText::WriteMenu(defaultMenu, Settings{});
		SettingsText::WriteAdvanced(defaultAdvanced, Settings{});
		CHECK(menuOk && shippedMenu == defaultMenu.str());
		CHECK(advancedOk && shippedAdvanced == defaultAdvanced.str());
		std::stringstream shippedBoth(shippedMenu + shippedAdvanced);
		Settings          fromFile;
		fromFile.enabled = false;
		const auto rs = SettingsText::Read(shippedBoth, fromFile);
		CHECK(fromFile == Settings{});
		CHECK(rs.problems.empty());
		CHECK(rs.taken == static_cast<int>(std::size(SettingsText::kKeys)));
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
		// the burnout dip: down at the start, whole again at the end, rising the whole way
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

int main(int argc, char** argv)
{
	// the two settings files the package ships, as the plugin writes them for the built-in defaults: test_heat --write-menu
	// <file> / --write-advanced <file> (binary, so the line ends are the plugin's own on every system)
	if (argc > 2 && (std::string_view(argv[1]) == "--write-menu" || std::string_view(argv[1]) == "--write-advanced")) {
		std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
		if (std::string_view(argv[1]) == "--write-menu") {
			Plugin::SettingsText::WriteMenu(out, Plugin::Settings{});
		} else {
			Plugin::SettingsText::WriteAdvanced(out, Plugin::Settings{});
		}
		return out ? 0 : 1;
	}
	Gains();
	HeatBar();
	Gates();
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
	Classifier();
	Overcharge();
	ScorchMarks();
	std::printf("%d passed, %d failed\n", gPassed, gFailed);
	return gFailed == 0 ? 0 : 1;
}

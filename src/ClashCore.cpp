// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The game-free core (see Core.h). No game header here: tests/ builds this file on its own.

#include "ClashCore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace Crossfire::Core
{
	std::string PathText(const std::filesystem::path& a_path)
	{
		const auto u = a_path.generic_u8string();
		return std::string(u.begin(), u.end());
	}

	// ------------------------------------------------------------------ vectors
	float Length(Vec3 a) noexcept { return std::sqrt(Dot(a, a)); }

	bool Finite(Vec3 a) noexcept { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

	Vec3 Lerp(Vec3 a, Vec3 b, float t) noexcept { return a + (b - a) * t; }

	Vec3 DirectionFromAngles(float a_pitch, float a_heading) noexcept
	{
		const float cp = std::cos(a_pitch);
		return { std::sin(a_heading) * cp, std::cos(a_heading) * cp, -std::sin(a_pitch) };
	}

	Vec3 UnitOf(Vec3 a) noexcept
	{
		const float l = Length(a);
		return l > 1e-6f ? a * (1.0f / l) : Vec3{};
	}

	Angles AnglesFromDirection(Vec3 a_dir) noexcept
	{
		return { std::atan2(-a_dir.z, std::hypot(a_dir.x, a_dir.y)), std::atan2(a_dir.x, a_dir.y) };
	}

	Vec3 TurnToward(Vec3 a_dir, Vec3 a_want, float a_maxTurn) noexcept
	{
		const float c = std::clamp(Dot(a_dir, a_want), -1.0f, 1.0f);
		if (std::acos(c) <= a_maxTurn) {
			return a_want;
		}
		const Vec3 side = UnitOf(a_want - a_dir * c);  // the part of the wanted way square to the current one
		if (Length(side) < 0.5f) {
			return a_dir;  // straight behind: no single plane to turn in
		}
		return UnitOf(a_dir * std::cos(a_maxTurn) + side * std::sin(a_maxTurn));
	}

	std::optional<Vec3> LockDirection(Vec3 a_from, Vec3 a_dir, Vec3 a_aim, float a_cone, float a_range) noexcept
	{
		const Vec3  to = a_aim - a_from;
		const float d = Length(to);
		if (!(d > 1.0f) || d > a_range || !Finite(to)) {
			return std::nullopt;
		}
		const Vec3 want = to * (1.0f / d);
		if (Dot(UnitOf(a_dir), want) < std::cos(a_cone)) {
			return std::nullopt;
		}
		return want;
	}

	std::optional<Vec3> SteerInto(Vec3 a_posA, Vec3 a_dirA, Vec3 a_posB, Vec3 a_dirB, float a_range, float a_cone, float a_maxTurn) noexcept
	{
		const Vec3  ab = a_posB - a_posA;
		const float d = Length(ab);
		if (!(d > 1.0f) || d > a_range || !Finite(ab)) {
			return std::nullopt;
		}
		const Vec3  u = ab * (1.0f / d);
		const float c = std::cos(a_cone);
		if (Dot(UnitOf(a_dirA), u) < c || Dot(UnitOf(a_dirB), u * -1.0f) < c) {
			return std::nullopt;
		}
		return TurnToward(UnitOf(a_dirA), u, a_maxTurn);
	}

	Basis LookBasis(Vec3 a_dir) noexcept
	{
		const Vec3  f = UnitOf(a_dir);
		const float h = std::hypot(f.x, f.y);
		if (h < 1e-5f) {
			return { { 1.0f, 0.0f, 0.0f }, f, f.z > 0.0f ? Vec3{ 0.0f, -1.0f, 0.0f } : Vec3{ 0.0f, 1.0f, 0.0f } };
		}
		const Vec3 r{ f.y / h, -f.x / h, 0.0f };
		return { r, f, { r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x } };
	}

	namespace
	{
		// the point on segment a-b nearest p
		[[nodiscard]] Vec3 ClosestOnSegment(Vec3 p, Vec3 a, Vec3 b) noexcept
		{
			const Vec3  ab = b - a;
			const float len2 = Dot(ab, ab);
			if (len2 <= 1e-12f) {
				return a;
			}
			const float t = std::clamp(Dot(p - a, ab) / len2, 0.0f, 1.0f);
			return a + ab * t;
		}

		// the point of a barrier's rectangle nearest p
		[[nodiscard]] Vec3 ClosestOnBarrier(Vec3 p, const Body& a_wall) noexcept
		{
			const Vec3  d = p - a_wall.from;
			const float along = std::clamp(Dot(d, a_wall.across), -a_wall.halfWidth, a_wall.halfWidth);
			// the rectangle stands upright: its second axis is world up, whatever way the wall faces
			const float up = std::clamp(d.z - a_wall.across.z * Dot(d, a_wall.across), 0.0f, a_wall.height);
			return a_wall.from + a_wall.across * along + Vec3{ 0.0f, 0.0f, up };
		}

		// the closest points of segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection, 5.1.9)
		void ClosestSegmentSegment(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3& c1, Vec3& c2) noexcept
		{
			const Vec3      d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
			const float     a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
			constexpr float kEps = 1e-12f;
			float           s = 0.0f, t = 0.0f;
			if (a <= kEps && e <= kEps) {
				c1 = p1;
				c2 = p2;
				return;
			}
			if (a <= kEps) {
				t = std::clamp(f / e, 0.0f, 1.0f);
			} else {
				const float c = Dot(d1, r);
				if (e <= kEps) {
					s = std::clamp(-c / a, 0.0f, 1.0f);
				} else {
					const float b = Dot(d1, d2);
					const float denom = a * e - b * b;
					s = denom > kEps ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
					t = (b * s + f) / e;
					if (t < 0.0f) {
						t = 0.0f;
						s = std::clamp(-c / a, 0.0f, 1.0f);
					} else if (t > 1.0f) {
						t = 1.0f;
						s = std::clamp((b - c) / a, 0.0f, 1.0f);
					}
				}
			}
			c1 = p1 + d1 * s;
			c2 = p2 + d2 * t;
		}

		// the first t in [0, 1] where a convex function of t reaches zero or below. Distance from a point moving on a
		// straight line to a convex shape is convex, so a golden-section search finds the nearest approach and a
		// bisection before it finds the first touch.
		template <class F>
		[[nodiscard]] bool FirstRoot(F&& f, float& a_t) noexcept
		{
			const float f0 = f(0.0f);
			if (!std::isfinite(f0)) {
				return false;
			}
			if (f0 <= 0.0f) {
				a_t = 0.0f;
				return true;
			}
			constexpr float kInvPhi = 0.6180339887f;
			float           lo = 0.0f, hi = 1.0f;
			float           x1 = hi - kInvPhi * (hi - lo), x2 = lo + kInvPhi * (hi - lo);
			float           f1 = f(x1), f2 = f(x2);
			for (int i = 0; i < 48 && f1 > 0.0f && f2 > 0.0f; ++i) {
				if (f1 < f2) {
					hi = x2;
					x2 = x1;
					f2 = f1;
					x1 = hi - kInvPhi * (hi - lo);
					f1 = f(x1);
				} else {
					lo = x1;
					x1 = x2;
					f1 = f2;
					x2 = lo + kInvPhi * (hi - lo);
					f2 = f(x2);
				}
			}
			float best = f1 <= f2 ? x1 : x2;
			float fbest = std::min(f1, f2);
			if (const float f1end = f(1.0f); f1end < fbest) {
				best = 1.0f;
				fbest = f1end;
			}
			if (!(fbest <= 0.0f)) {
				return false;
			}
			lo = 0.0f;
			hi = best;
			for (int i = 0; i < 40; ++i) {
				const float mid = 0.5f * (lo + hi);
				if (f(mid) <= 0.0f) {
					hi = mid;
				} else {
					lo = mid;
				}
			}
			a_t = hi;
			return true;
		}

		[[nodiscard]] Vec3 Between(Vec3 a_centre, float a_ra, Vec3 a_other, float a_rb) noexcept
		{
			const float sum = a_ra + a_rb;
			return sum > 0.0f ? a_centre + (a_other - a_centre) * (a_ra / sum) : Lerp(a_centre, a_other, 0.5f);
		}

		[[nodiscard]] bool SphereSphere(const Body& a, const Body& b, Touch& a_out) noexcept
		{
			const Vec3  s = a.from - b.from;
			const Vec3  v = (a.to - a.from) - (b.to - b.from);
			const float r = a.radius + b.radius;
			const float c = Dot(s, s) - r * r;
			float       t = 0.0f;
			if (c > 0.0f) {
				const float vv = Dot(v, v);
				const float sv = Dot(s, v);
				if (vv <= 1e-12f || sv >= 0.0f) {
					return false;  // not moving relative to each other, or moving apart
				}
				const float disc = sv * sv - vv * c;
				if (disc < 0.0f) {
					return false;
				}
				t = (-sv - std::sqrt(disc)) / vv;
				if (!(t >= 0.0f && t <= 1.0f)) {
					return false;
				}
			}
			const Vec3 pa = Lerp(a.from, a.to, t);
			const Vec3 pb = Lerp(b.from, b.to, t);
			a_out = { t, Between(pa, a.radius, pb, b.radius) };
			return true;
		}

		[[nodiscard]] bool SphereBeam(const Body& a_sphere, const Body& a_beam, Touch& a_out) noexcept
		{
			const float r = a_sphere.radius + a_beam.radius;
			float       t = 0.0f;
			const auto  f = [&](float x) { return DistancePointSegment(Lerp(a_sphere.from, a_sphere.to, x), a_beam.from, a_beam.to) - r; };
			if (!FirstRoot(f, t)) {
				return false;
			}
			const Vec3 p = Lerp(a_sphere.from, a_sphere.to, t);
			a_out = { t, Between(p, a_sphere.radius, ClosestOnSegment(p, a_beam.from, a_beam.to), a_beam.radius) };
			return true;
		}

		[[nodiscard]] bool SphereBarrier(const Body& a_sphere, const Body& a_wall, Touch& a_out) noexcept
		{
			const float r = a_sphere.radius + a_wall.radius;
			float       t = 0.0f;
			const auto  f = [&](float x) { return DistancePointBarrier(Lerp(a_sphere.from, a_sphere.to, x), a_wall) - r; };
			if (!FirstRoot(f, t)) {
				return false;
			}
			const Vec3 p = Lerp(a_sphere.from, a_sphere.to, t);
			a_out = { t, Between(p, a_sphere.radius, ClosestOnBarrier(p, a_wall), a_wall.radius) };
			return true;
		}

		[[nodiscard]] bool BeamBeam(const Body& a, const Body& b, Touch& a_out) noexcept
		{
			Vec3 ca, cb;
			ClosestSegmentSegment(a.from, a.to, b.from, b.to, ca, cb);
			if (Length(ca - cb) > a.radius + b.radius) {
				return false;
			}
			a_out = { 0.0f, Between(ca, a.radius, cb, b.radius) };
			return true;
		}

		[[nodiscard]] char Lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

		[[nodiscard]] bool SameText(std::string_view a, std::string_view b) noexcept
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
		}

		[[nodiscard]] bool Space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

		[[nodiscard]] std::string_view Trim(std::string_view s) noexcept
		{
			while (!s.empty() && Space(s.front())) {
				s.remove_prefix(1);
			}
			while (!s.empty() && Space(s.back())) {
				s.remove_suffix(1);
			}
			return s;
		}

		[[nodiscard]] float Sanitize(float a_value, float a_fallback) noexcept { return std::isfinite(a_value) ? a_value : a_fallback; }
	}

	float DistancePointSegment(Vec3 p, Vec3 a, Vec3 b) noexcept { return Length(p - ClosestOnSegment(p, a, b)); }

	float DistancePointBarrier(Vec3 p, const Body& a_barrier) noexcept { return Length(p - ClosestOnBarrier(p, a_barrier)); }

	bool Valid(const Body& a) noexcept
	{
		constexpr float kHuge = 1.0e7f;  // well beyond any worldspace
		const auto      sane = [](float v, float hi) { return std::isfinite(v) && v >= 0.0f && v <= hi; };
		if (!Finite(a.from) || !Finite(a.to) || !sane(a.radius, 1.0e5f) || !sane(a.reach, 1.0e4f) ||
			!sane(a.strength, std::numeric_limits<float>::max())) {
			return false;
		}
		for (const Vec3 p : { a.from, a.to }) {
			if (std::abs(p.x) > kHuge || std::abs(p.y) > kHuge || std::abs(p.z) > kHuge) {
				return false;
			}
		}
		if (static_cast<std::size_t>(a.element) >= kElements) {
			return false;
		}
		switch (a.shape) {
		case Shape::kSphere:
		case Shape::kBeam:
			return true;
		case Shape::kBarrier:
			return Finite(a.across) && std::abs(Length(a.across) - 1.0f) < 1e-2f && std::abs(a.across.z) < 1e-2f &&
			       sane(a.halfWidth, 1.0e5f) && a.halfWidth > 0.0f && sane(a.height, 1.0e5f) && a.height > 0.0f;
		}
		return false;
	}

	Box Bounds(const Body& a) noexcept
	{
		Box box{ { std::min(a.from.x, a.to.x), std::min(a.from.y, a.to.y), std::min(a.from.z, a.to.z) },
			{ std::max(a.from.x, a.to.x), std::max(a.from.y, a.to.y), std::max(a.from.z, a.to.z) } };
		if (a.shape == Shape::kBarrier) {
			const Vec3 ends[4]{ a.from + a.across * a.halfWidth, a.from - a.across * a.halfWidth,
				a.from + a.across * a.halfWidth + Vec3{ 0, 0, a.height }, a.from - a.across * a.halfWidth + Vec3{ 0, 0, a.height } };
			box.lo = box.hi = ends[0];
			for (const Vec3 e : ends) {
				box.lo = { std::min(box.lo.x, e.x), std::min(box.lo.y, e.y), std::min(box.lo.z, e.z) };
				box.hi = { std::max(box.hi.x, e.x), std::max(box.hi.y, e.y), std::max(box.hi.z, e.z) };
			}
		}
		const float pad = a.radius + a.reach;
		const Vec3  r{ pad, pad, pad };
		return { box.lo - r, box.hi + r };
	}

	bool HeadOn(const Body& a, const Body& b, float a_cos) noexcept
	{
		if (a.shape != Shape::kSphere || b.shape != Shape::kSphere || !(a.reach > 0.0f) || !(b.reach > 0.0f) || !(a_cos <= 1.0f)) {
			return false;
		}
		const Vec3  da = a.to - a.from, db = b.to - b.from;
		const float la = Length(da), lb = Length(db);
		if (!(la > 1e-3f) || !(lb > 1e-3f)) {
			return false;
		}
		return Dot(da, db) / (la * lb) <= -a_cos && Dot(b.from - a.from, da) > 0.0f;
	}

	bool FirstTouch(const Body& a, const Body& b, Touch& a_out, float a_headOnCos) noexcept
	{
		using enum Shape;
		if (a.shape == kSphere && b.shape == kSphere) {
			if (HeadOn(a, b, a_headOnCos)) {
				Body wa = a, wb = b;
				wa.radius += a.reach;
				wb.radius += b.reach;
				return SphereSphere(wa, wb, a_out);
			}
			return SphereSphere(a, b, a_out);
		}
		if (a.shape == kSphere && b.shape == kBeam) {
			return SphereBeam(a, b, a_out);
		}
		if (a.shape == kBeam && b.shape == kSphere) {
			return SphereBeam(b, a, a_out);
		}
		if (a.shape == kSphere && b.shape == kBarrier) {
			return SphereBarrier(a, b, a_out);
		}
		if (a.shape == kBarrier && b.shape == kSphere) {
			return SphereBarrier(b, a, a_out);
		}
		if (a.shape == kBeam && b.shape == kBeam) {
			return BeamBeam(a, b, a_out);
		}
		return false;  // a beam against a wall, or two walls: both are always immune, so nothing would happen
	}

	// ------------------------------------------------------------------ elements and actions
	namespace
	{
		constexpr std::array<std::string_view, kElements> kElementNames{ "Fire", "Frost", "Shock", "Poison", "Arcane", "Physical", "Force" };
	}

	std::string_view ElementName(Element a_element) noexcept
	{
		const auto i = static_cast<std::size_t>(a_element);
		return i < kElements ? kElementNames[i] : std::string_view{ "?" };
	}

	bool ParseElement(std::string_view a_text, Element& a_out) noexcept
	{
		a_text = Trim(a_text);
		for (std::size_t i = 0; i < kElements; ++i) {
			if (SameText(a_text, kElementNames[i])) {
				a_out = static_cast<Element>(i);
				return true;
			}
		}
		return false;
	}

	std::string_view ActionName(Action a_action) noexcept
	{
		switch (a_action) {
		case Action::kPass:
			return "Pass";
		case Action::kClash:
			return "Clash";
		case Action::kAnnihilate:
			return "Annihilate";
		case Action::kWins:
			return "Wins";
		case Action::kLoses:
			return "Loses";
		}
		return "?";
	}

	bool ParseAction(std::string_view a_text, Action& a_out) noexcept
	{
		a_text = Trim(a_text);
		static constexpr std::pair<std::string_view, Action> kNames[]{
			{ "Pass", Action::kPass },
			{ "Ignore", Action::kPass },
			{ "None", Action::kPass },
			{ "Clash", Action::kClash },
			{ "Annihilate", Action::kAnnihilate },
			{ "Cancel", Action::kAnnihilate },
			{ "Wins", Action::kWins },
			{ "Beats", Action::kWins },
			{ "Loses", Action::kLoses },
			{ "LosesTo", Action::kLoses },
		};
		for (const auto& [name, action] : kNames) {
			if (SameText(a_text, name)) {
				a_out = action;
				return true;
			}
		}
		return false;
	}

	void SetReaction(Table& a_table, Element a, Element b, Action a_action) noexcept
	{
		const auto i = static_cast<std::size_t>(a), j = static_cast<std::size_t>(b);
		if (i >= kElements || j >= kElements) {
			return;
		}
		if (i == j && (a_action == Action::kWins || a_action == Action::kLoses)) {
			a_action = Action::kClash;  // an element cannot beat itself; the contest decides
		}
		a_table[i][j] = a_action;
		a_table[j][i] = Mirror(a_action);
	}

	Table DefaultTable() noexcept
	{
		Table t{};
		for (auto& row : t) {
			row.fill(Action::kClash);
		}
		SetReaction(t, Element::kFire, Element::kFrost, Action::kAnnihilate);  // steam: opposites cancel whatever their size
		for (std::size_t i = 0; i < kElements; ++i) {
			SetReaction(t, Element::kForce, static_cast<Element>(i), Action::kWins);  // a shout swats projectiles aside
		}
		SetReaction(t, Element::kForce, Element::kForce, Action::kPass);
		return t;
	}

	Outcome Resolve(const Table& a_table, float a_ratio, bool a_weaken, Element ea, float sa, bool immuneA, Element eb,
		float sb, bool immuneB) noexcept
	{
		Outcome    out{ false, false, false, sa, sb };
		const auto i = static_cast<std::size_t>(ea), j = static_cast<std::size_t>(eb);
		if ((immuneA && immuneB) || i >= kElements || j >= kElements) {
			return out;
		}
		sa = std::isfinite(sa) ? std::max(sa, 0.0f) : 0.0f;
		sb = std::isfinite(sb) ? std::max(sb, 0.0f) : 0.0f;
		out.strengthA = sa;
		out.strengthB = sb;
		a_ratio = std::isfinite(a_ratio) ? std::clamp(a_ratio, 1.0f, 100.0f) : 2.0f;

		bool loseA = false, loseB = false;
		// who beat whom, and what the winner pays (only in a contest)
		float costA = 0.0f, costB = 0.0f;
		switch (a_table[i][j]) {
		case Action::kPass:
			return out;
		case Action::kAnnihilate:
			loseA = loseB = true;
			break;
		case Action::kWins:
			loseB = true;
			break;
		case Action::kLoses:
			loseA = true;
			break;
		case Action::kClash:
			if (sa >= sb * a_ratio && sa > 0.0f) {
				loseB = true;
				costA = sb;
			} else if (sb >= sa * a_ratio && sb > 0.0f) {
				loseA = true;
				costB = sa;
			} else {
				loseA = loseB = true;
			}
			break;
		}
		if (loseA && !immuneA) {
			out.killA = true;
			out.strengthA = 0.0f;
		}
		if (loseB && !immuneB) {
			out.killB = true;
			out.strengthB = 0.0f;
		}
		// the winner of a contest pays for it; one whose strength runs out goes too
		if (a_weaken && !out.killA && !immuneA && costA > 0.0f) {
			out.strengthA = sa - costA;
			if (out.strengthA <= sa * 1e-4f) {
				out.killA = true;
				out.strengthA = 0.0f;
			}
		}
		if (a_weaken && !out.killB && !immuneB && costB > 0.0f) {
			out.strengthB = sb - costB;
			if (out.strengthB <= sb * 1e-4f) {
				out.killB = true;
				out.strengthB = 0.0f;
			}
		}
		out.happened = out.killA || out.killB || out.strengthA != sa || out.strengthB != sb;
		return out;
	}

	// ------------------------------------------------------------------ strength
	float BaseStrength(Kind a_kind, float a_cost, float a_power, float a_damage, const Tuning& a_tuning) noexcept
	{
		const float power = std::clamp(Sanitize(a_power, 1.0f) > 0.0f ? Sanitize(a_power, 1.0f) : 1.0f, 0.1f, 10.0f);
		const float cost = std::max(Sanitize(a_cost, 0.0f), 0.0f);
		const float floorCost = std::max(cost, std::max(Sanitize(a_tuning.minSpellStrength, 0.0f), 0.0f));
		float       s = 0.0f;
		switch (a_kind) {
		case Kind::kSpell:
		case Kind::kBeam:
		case Kind::kBarrier:
		case Kind::kCone:
			s = floorCost * power;
			break;
		case Kind::kStream:
			s = floorCost * std::clamp(Sanitize(a_tuning.streamShare, 0.15f), 0.0f, 10.0f) * power;
			break;
		case Kind::kArrow:
			s = std::max(Sanitize(a_damage, 0.0f), 1.0f) * std::clamp(Sanitize(a_tuning.arrowScale, 2.0f), 0.0f, 1000.0f);
			break;
		case Kind::kVoice:
			s = std::max(Sanitize(a_tuning.shoutStrength, 200.0f), 0.0f);
			break;
		}
		return std::isfinite(s) ? std::clamp(s, 0.01f, 1.0e9f) : 0.01f;
	}

	float ConeRadius(float a_initial, float a_travelled, float a_tangent) noexcept
	{
		const float r = std::max(Sanitize(a_initial, 0.0f), 0.0f) +
		                std::max(Sanitize(a_travelled, 0.0f), 0.0f) * std::clamp(Sanitize(a_tangent, 0.0f), 0.0f, 10.0f);
		return std::min(r, 1.0e4f);
	}

	// ------------------------------------------------------------------ per-frame decisions
	float Radius(float a_own, const Config& a_config) noexcept
	{
		const float own = std::isfinite(a_own) && a_own > 0.0f ? std::min(a_own, 1.0e5f) : 0.0f;
		const float lo = std::max(Sanitize(a_config.minRadius, 0.0f), 0.0f);
		const float hi = std::max(Sanitize(a_config.maxRadius, lo), lo);
		const float r = own * Sanitize(a_config.radiusScale, 1.0f) + Sanitize(a_config.radiusBonus, 0.0f);
		return std::clamp(std::isfinite(r) ? r : lo, lo, hi);
	}

	Vec3 BarrierAcross(float a_heading) noexcept
	{
		if (!std::isfinite(a_heading)) {
			return { 1.0f, 0.0f, 0.0f };
		}
		// facing is (sin h, cos h, 0); a quarter turn clockwise of it is (cos h, -sin h, 0)
		return { std::cos(a_heading), -std::sin(a_heading), 0.0f };
	}

	Vec3 Backtrack(Vec3 a_now, Vec3 a_velocity, Vec3 a_fallback, float a_delta, float a_travelled) noexcept
	{
		Vec3 v = a_velocity;
		if (!Finite(v) || !(Dot(v, v) > 1.0f)) {
			v = a_fallback;
		}
		const float speed = Length(v);
		if (!Finite(a_now) || !std::isfinite(speed) || speed < 1.0f || !std::isfinite(a_delta) || a_delta <= 0.0f) {
			return a_now;
		}
		float back = speed * a_delta;
		if (std::isfinite(a_travelled) && a_travelled >= 0.0f) {
			back = std::min(back, a_travelled);
		}
		return a_now - v * (back / speed);
	}

	float DamageScale(float a_before, float a_after) noexcept
	{
		if (!std::isfinite(a_before) || !std::isfinite(a_after) || a_before <= 0.0f) {
			return 1.0f;
		}
		return std::clamp(a_after / a_before, 0.05f, 1.0f);
	}

	// ------------------------------------------------------------------ elements of a spell
	Element Classify(const MagicFacts& a_facts, const std::array<std::vector<std::string>, kElements>& a_keywords) noexcept
	{
		for (std::size_t e = 0; e < kElements; ++e) {
			for (const auto& wanted : a_keywords[e]) {
				for (const auto have : a_facts.keywords) {
					if (SameText(have, wanted)) {
						return static_cast<Element>(e);
					}
				}
			}
		}
		static constexpr std::pair<std::string_view, Element> kResists[]{
			{ "ResistFire", Element::kFire },
			{ "FireResist", Element::kFire },
			{ "ResistFrost", Element::kFrost },
			{ "FrostResist", Element::kFrost },
			{ "ResistShock", Element::kShock },
			{ "ElectricResist", Element::kShock },
			{ "ShockResist", Element::kShock },
			{ "PoisonResist", Element::kPoison },
			{ "ResistPoison", Element::kPoison },
		};
		for (const auto& [name, element] : kResists) {
			if (SameText(a_facts.resist, name)) {
				return element;
			}
		}
		return a_facts.voice && a_facts.cone ? Element::kForce : Element::kArcane;
	}

	// ------------------------------------------------------------------ looks
	namespace
	{
		constexpr std::array<std::string_view, kLooks> kLookNames{ "Fire", "Frost", "Shock", "Poison", "Water", "Blood", "Sun", "Astral",
			"Shadow", "Necrotic", "Wind", "Earth", "Arcane", "Absorb", "Physical", "Force" };

		[[nodiscard]] bool WordChar(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		}

		[[nodiscard]] bool StartsNoCase(std::string_view a_text, std::size_t a_at, std::string_view a_with) noexcept
		{
			return a_at + a_with.size() <= a_text.size() && SameText(a_text.substr(a_at, a_with.size()), a_with);
		}

		struct Word
		{
			std::string_view                text;
			bool                            start{ false };  // begins a word
			bool                            whole{ false };  // is a whole word
			std::array<std::string_view, 2> notBefore{};     // never when one of these follows it
		};

		[[nodiscard]] bool Has(std::string_view a_text, const Word& a_word) noexcept
		{
			for (std::size_t i = 0; i + a_word.text.size() <= a_text.size(); ++i) {
				if (!StartsNoCase(a_text, i, a_word.text) || ((a_word.start || a_word.whole) && i > 0 && WordChar(a_text[i - 1]))) {
					continue;
				}
				const std::size_t end = i + a_word.text.size();
				if (a_word.whole && end < a_text.size() && WordChar(a_text[end])) {
					continue;
				}
				if (std::ranges::any_of(a_word.notBefore, [&](std::string_view n) { return !n.empty() && StartsNoCase(a_text, end, n); })) {
					continue;
				}
				return true;
			}
			return false;
		}

		constexpr Word W(std::string_view a_text) noexcept { return { a_text }; }
		constexpr Word Start(std::string_view a_text) noexcept { return { a_text, true }; }
		constexpr Word Whole(std::string_view a_text) noexcept { return { a_text, false, true }; }
		constexpr Word Not(std::string_view a_text, std::string_view a, std::string_view b = {}) noexcept { return { a_text, false, false, { a, b } }; }

		struct Family
		{
			Look                  look;
			std::span<const Word> words;
		};

		constexpr Word kPhysical[]{ W("arrow"), W("quiver"), W("bounddagger"), W("boundsword"), W("boundmace"), W("boundpike"), W("boundrapier"),
			W("boundgs"), W("boundbigaxe"), W("boundwarhammer"), W("boundhalberd"), W("boundthrowing"), W("boundcrescent"), W("boundwpn"), W("spike"),
			W("needle"), W("shuriken"), W("dragonsteeth"), W("dragons teeth"), W("dragon's teeth"), W("runeblade"), W("windblade"), W("wind blade") };
		constexpr Word kAbsorb[]{ W("absorb"), W("drain"), W("leech"), W("siphon"), Not("steal", "th"), W("vampiricdrain"), W("vampiric drain"),
			W("vampirictouch"), W("vampiric touch") };
		constexpr Word kBlood[]{ W("blood"), W("vampir"), W("sanguin"), W("hemo"), W("hemalurg"), W("crimson"), Whole("bat"), Whole("bats") };
		constexpr Word kShock[]{ W("shock"), W("lightn"), W("thunder"), W("spark"), W("volt"), W("electr"), W("arclight"), W("tonitrus"),
			W("plasma"), W("static") };
		constexpr Word kNecrotic[]{ W("necro"), W("death"), W("lich"), W("banshee"), W("wraith"), Start("bone"), W("grave"), W("desecrat"), W("geist"),
			W("spectral"), W("ghost"), W("undead"), W("reanimat"), W("souldrain"), W("soul drain"), W("soulrend"), W("soul rend"), W("soultear"),
			W("soul tear") };
		constexpr Word kShadow[]{ W("shadow"), W("dark"), W("void"), Not("abyss", "altide", "al tide"), W("umbra"), W("gloom"), W("dusk"), W("twilight"),
			W("eclipse"), W("night"), W("coldharbour"), W("oblivion") };
		constexpr Word kSun[]{ Start("sun"), W("sunlight"), W("holy"), W("divine"), W("sacred"), W("radian"), W("inquisit"), W("dawn"), W("solar"),
			W("lightspell"), Whole("light"), W("ascension"), W("meridia"), W("stendarr"), W("auriel") };
		constexpr Word kAstral[]{ W("astral"), W("stellar"), Whole("star"), Whole("stars"), W("starfall"), W("luna"), W("moon"), W("celest"), W("cosmic"),
			W("comet"), W("meteor") };
		constexpr Word kPoison[]{ W("poison"), W("toxic"), W("acid"), W("venom"), W("corros"), W("noxious"), W("disease"), W("plague"), W("blight"),
			Start("bile"), W("carrion"), W("virulent"), W("locust"), W("swarm"), Start("bug") };
		constexpr Word kWater[]{ W("water"), W("aqua"), W("tidal"), Start("tide"), W("flood"), Not("torrent", "ial"), W("hydro"), W("ocean"), W("tsunami"),
			W("abyssaltide"), W("abyssal tide") };
		constexpr Word kWind[]{ W("wind"), Whole("air"), W("aero"), W("gale"), W("gust"), W("cyclone"), W("tornado"), W("twister") };
		constexpr Word kEarth[]{ W("natura"), W("nature"), W("earth"), W("stone"), Start("rock"), W("root"), W("thorn"), Start("vine"), W("spore"),
			W("hive"), W("leaf"), W("flora"), W("spriggan"), W("tentacle"), W("briar"), W("boulder") };
		constexpr Word kFire[]{ W("fire"), W("flame"), W("burn"), W("inferno"), W("magma"), W("lava"), W("vulcan"), W("ember"), W("incendi"), W("incinera"),
			W("immolat"), W("pyro"), W("blaze"), W("scorch"), W("molten") };
		constexpr Word kFrost[]{ W("frost"), Start("ice"), W("icicle"), W("cold"), W("freez"), W("glaci"), W("snow"), W("winter"), W("blizzard"),
			W("rime"), W("gelid"), W("arctic"), W("frigid"), W("hail"), W("chill"), W("cryo") };
		constexpr Word kArcane[]{ W("arcane"), W("magic"), W("mana"), W("missile"), W("telekin"), W("rune"), W("mystic"), W("eldritch"), W("chaos"),
			W("force"), W("warp"), Start("mage") };
		constexpr Word kStorm[]{ W("storm") };

		// the first family with a word in the text wins, in this order (Fire and Frost late: their words are the commonest;
		// a storm is lightning only when nothing else names it - an ice storm is frost)
		constexpr Family kFamilies[]{ { Look::kPhysical, kPhysical }, { Look::kAbsorb, kAbsorb }, { Look::kBlood, kBlood }, { Look::kShock, kShock },
			{ Look::kNecrotic, kNecrotic }, { Look::kShadow, kShadow }, { Look::kSun, kSun }, { Look::kAstral, kAstral },
			{ Look::kPoison, kPoison }, { Look::kWater, kWater }, { Look::kWind, kWind }, { Look::kEarth, kEarth }, { Look::kFire, kFire },
			{ Look::kFrost, kFrost }, { Look::kShock, kStorm }, { Look::kArcane, kArcane } };

		// the game's and the overhauls' own element tags: an exact name, or a part of one
		constexpr std::pair<std::string_view, Look> kExactTags[]{ { "MagicDamageFire", Look::kFire }, { "MagicDamageFrost", Look::kFrost },
			{ "MagicDamageShock", Look::kShock } };
		constexpr std::pair<std::string_view, Look> kPartTags[]{ { "NaturaDestructionWater", Look::kWater },
			{ "NaturaDestructionWind", Look::kWind }, { "NaturaDestructionStone", Look::kEarth }, { "MagicDamageSun", Look::kSun },
			{ "MagicDamageShadow", Look::kShadow }, { "MagicDamagePoison", Look::kPoison }, { "MagicPoison", Look::kPoison } };

		[[nodiscard]] std::optional<Look> HueLook(Rgb a_light) noexcept
		{
			const float r = a_light.r / 255.0f, g = a_light.g / 255.0f, b = a_light.b / 255.0f;
			const float hi = (std::max)({ r, g, b }), lo = (std::min)({ r, g, b });
			if (hi <= 0.0f) {
				return std::nullopt;
			}
			const float l = (hi + lo) / 2.0f;
			const float d = hi - lo;
			const float s = d <= 0.0f ? 0.0f : (l <= 0.5f ? d / (hi + lo) : d / (2.0f - hi - lo));
			if (s < 0.15f) {
				return l > 0.7f ? Look::kSun : (l < 0.3f ? Look::kShadow : Look::kArcane);
			}
			float h = 0.0f;
			if (hi == r) {
				h = std::fmod((g - b) / d, 6.0f);
			} else if (hi == g) {
				h = (b - r) / d + 2.0f;
			} else {
				h = (r - g) / d + 4.0f;
			}
			h *= 60.0f;
			if (h < 0.0f) {
				h += 360.0f;
			}
			if (h < 15.0f || h >= 345.0f) {
				return l < 0.45f ? Look::kBlood : Look::kFire;
			}
			if (h < 50.0f) {
				return Look::kFire;
			}
			if (h < 70.0f) {
				return Look::kSun;
			}
			if (h < 160.0f) {
				return Look::kPoison;
			}
			if (h < 200.0f) {
				return Look::kFrost;
			}
			if (h < 250.0f) {
				return Look::kArcane;
			}
			if (h < 300.0f) {
				return l > 0.5f ? Look::kShock : Look::kShadow;
			}
			return Look::kShadow;
		}
	}

	std::string_view LookName(Look a_look) noexcept
	{
		const auto i = static_cast<std::size_t>(a_look);
		return i < kLooks ? kLookNames[i] : std::string_view{ "?" };
	}

	Look LookOf(const LookFacts& a_facts) noexcept
	{
		if (a_facts.arrow) {
			return Look::kPhysical;
		}
		if (a_facts.element == Element::kForce) {
			return Look::kForce;
		}
		for (const auto have : a_facts.keywords) {
			for (const auto& [name, look] : kExactTags) {
				if (SameText(have, name)) {
					return look;
				}
			}
			for (const auto& [part, look] : kPartTags) {
				if (Has(have, W(part))) {
					return look;
				}
			}
		}
		if (a_facts.absorb) {
			return Look::kAbsorb;
		}
		for (const auto& family : kFamilies) {
			if (std::ranges::any_of(family.words, [&](const Word& w) { return Has(a_facts.words, w); })) {
				return family.look;
			}
		}
		switch (a_facts.element) {
		case Element::kFire:
			return Look::kFire;
		case Element::kFrost:
			return Look::kFrost;
		case Element::kShock:
			return Look::kShock;
		case Element::kPoison:
			return Look::kPoison;
		case Element::kPhysical:
			return Look::kPhysical;
		default:
			break;
		}
		if (a_facts.resistsMagic) {
			return Look::kArcane;
		}
		if (a_facts.light) {
			if (const auto hue = HueLook(*a_facts.light)) {
				return *hue;
			}
		}
		return Look::kArcane;
	}

	std::string_view FileOf(std::string_view a_path) noexcept
	{
		const auto cut = a_path.find_last_of("\\/");
		return cut == std::string_view::npos ? a_path : a_path.substr(cut + 1);
	}

	std::string_view PairOf(Look a, Look b, bool a_sameSpell) noexcept
	{
		if (a_sameSpell) {
			return "Resonance";
		}
		const auto is = [&](Look x, Look y) { return (a == x && b == y) || (a == y && b == x); };
		if (is(Look::kFire, Look::kFrost)) {
			return "Steam";
		}
		if (is(Look::kFire, Look::kShock)) {
			return "Plasma";
		}
		if (is(Look::kFrost, Look::kShock)) {
			return "ShatterStorm";
		}
		if (is(Look::kSun, Look::kShadow)) {
			return "Eclipse";
		}
		return {};
	}

	TintMode TintModeOf(std::string_view a_block, float a_red, float a_green, float a_blue) noexcept
	{
		const auto ends = [&](std::string_view s) { return a_block.size() >= s.size() && SameText(a_block.substr(a_block.size() - s.size()), s); };
		if (ends("Hot")) {
			return TintMode::kHot;
		}
		if (ends("Pale")) {
			return TintMode::kPale;
		}
		return a_red >= 0.99f && a_green >= 0.99f && a_blue >= 0.99f ? TintMode::kColor : TintMode::kKeep;
	}

	std::array<float, 3> Tinted(Rgb a_light, TintMode a_mode) noexcept
	{
		const float          top = static_cast<float>((std::max)({ a_light.r, a_light.g, a_light.b }));
		std::array<float, 3> c{ 1.0f, 1.0f, 1.0f };
		if (top > 0.0f) {
			c = { a_light.r / top, a_light.g / top, a_light.b / top };
		}
		const float white = a_mode == TintMode::kHot ? 0.55f : (a_mode == TintMode::kPale ? 0.7f : 0.0f);
		for (auto& v : c) {
			v = v * (1.0f - white) + white;
		}
		return c;
	}

	// ------------------------------------------------------------------ limiter
	void Limiter::BeginFrame(double a_now, int a_perFrame, float a_cooldown) noexcept
	{
		_now = a_now;
		_left = std::max(a_perFrame, 0);
		_cooldown = std::isfinite(a_cooldown) ? std::max(a_cooldown, 0.0f) : 0.0f;
		if (a_now < _lastPrune) {
			_last.clear();  // the clock went back (a save was loaded): nothing remembered still means anything
			_lastPrune = a_now;
		}
		if (a_now - _lastPrune > 5.0) {
			_lastPrune = a_now;
			const double keep = static_cast<double>(_cooldown) * 2.0 + 1.0;
			std::erase_if(_last, [&](const auto& kv) { return a_now - kv.second > keep; });
		}
	}

	bool Limiter::Allow(std::uint64_t a_key)
	{
		if (_left <= 0) {
			return false;
		}
		const auto it = _last.find(a_key);
		if (it != _last.end() && _now - it->second < static_cast<double>(_cooldown) && _now >= it->second) {
			return false;
		}
		_last.insert_or_assign(a_key, _now);
		--_left;
		return true;
	}

	// ------------------------------------------------------------------ spell struggles
	namespace
	{
		constexpr StruggleConfig kStruggleDefaults{};

		constexpr float kMaxSkill = 300.0f;
		constexpr float kMaxLevel = 300.0f;
		constexpr float kMaxCost = 1.0e5f;
		constexpr float kMinMult = 0.01f, kMaxMult = 100.0f;
		constexpr float kMaxStep = 0.25f;                              // the longest frame Advance takes, as Clash.cpp caps it
		constexpr float kNever = 1.0e6f;                               // SecondsToWin when it never comes
		constexpr float kNowhere = std::numeric_limits<float>::max();  // how far off the axis a point that is not finite is
		constexpr float kWorld = 1.0e7f;                               // well beyond any worldspace, as in Valid()
		constexpr float kTubeBack = 100.0f;                            // the tube starts this far behind a muzzle and ends this far beyond the other
		constexpr float kTubeWidth = 150.0f;                           // off the axis at the muzzle ...
		constexpr float kTubeSpread = 0.35f;                           // ... and this much more per unit along it: a spray fans out
		constexpr float kReachFallback = 800.0f, kReachMin = 150.0f, kReachMax = 6000.0f;
		constexpr float kReachSlack = 1.15f, kReachExtra = 100.0f;  // InReach: a little past both reaches, as aim wavers
		constexpr float kMinShare = 0.15f, kMaxShare = 0.85f;
		constexpr float kReachUsed = 0.95f;  // the front goes no further than this share of a stream's reach
		constexpr float kBeamPast = 16.0f, kBeamMin = 32.0f;
		constexpr float kBeamAligned = 0.2f;  // a beam more off the axis than this cosine (about 78 degrees) is not cut
		constexpr float kShakeFade = 3000.0f;
		constexpr float kMaxResist = 85.0f;
		constexpr float kMaxMagnitude = 1.0e6f;

		// a setting as its math uses it: within the range the menu gives it, its default when it is not a number
		[[nodiscard]] float Clamped(float a_value, float a_default, float a_lo, float a_hi) noexcept
		{
			return std::clamp(Sanitize(a_value, a_default), a_lo, a_hi);
		}

		[[nodiscard]] float SkillOf(const Caster& a_caster) noexcept { return std::clamp(Sanitize(a_caster.skill, 0.0f), 0.0f, kMaxSkill); }

		[[nodiscard]] bool OnMap(Vec3 a) noexcept
		{
			return Finite(a) && std::abs(a.x) <= kWorld && std::abs(a.y) <= kWorld && std::abs(a.z) <= kWorld;
		}

		[[nodiscard]] float WithinOne(float a_value) noexcept { return std::isfinite(a_value) ? std::clamp(a_value, -1.0f, 1.0f) : 0.0f; }

		[[nodiscard]] float SafeRadius(float a_radius) noexcept { return std::isfinite(a_radius) ? std::max(a_radius, 0.0f) : 0.0f; }

		// how far off the axis the tube reaches this far along it, for a projectile this big
		[[nodiscard]] float TubeAt(float a_along, float a_radius) noexcept
		{
			return kTubeWidth + kTubeSpread * std::max(a_along, 0.0f) + a_radius;
		}

		[[nodiscard]] float PushTime(const StruggleConfig& a_config) noexcept
		{
			return Clamped(a_config.pushTime, kStruggleDefaults.pushTime, 1.0f, 30.0f);
		}

		[[nodiscard]] float BreakTime(const StruggleConfig& a_config) noexcept
		{
			return Clamped(a_config.breakTime, kStruggleDefaults.breakTime, 0.0f, 5.0f);
		}

		// an ending: an overwhelm leaves the loser reeling, anything else cools down
		void End(Struggle& a_s, StruggleStep& a_step, StruggleEvent a_event, bool a_aWon, const StruggleConfig& a_config) noexcept
		{
			a_step.event = a_event;
			a_step.aWon = a_s.aWon = a_aWon;
			a_s.phase = a_event == StruggleEvent::kOverwhelmed && BreakTime(a_config) > 0.0f ? Phase::kBroken : Phase::kCooldown;
			a_s.timer = 0.0f;
		}
	}

	Stream StreamOf(Kind a_kind, bool a_concentration, bool a_voice) noexcept
	{
		if (a_kind == Kind::kStream) {
			return a_voice ? Stream::kBreath : Stream::kSpray;
		}
		// a one-shot bolt is gone before a lock could show
		return a_kind == Kind::kBeam && a_concentration ? Stream::kBeam : Stream::kNone;
	}

	bool StreamOn(Stream a_stream, const StruggleConfig& a_config) noexcept
	{
		switch (a_stream) {
		case Stream::kSpray:
			return a_config.sprays;
		case Stream::kBeam:
			return a_config.beams;
		case Stream::kBreath:
			return a_config.breath;
		case Stream::kNone:
			break;
		}
		return false;
	}

	bool MayLock(const LockSide& a, const LockSide& b, Action a_action, const StruggleConfig& a_config) noexcept
	{
		const auto ready = [&](const LockSide& s) { return s.actor && s.alive && !s.busy && StreamOn(s.stream, a_config); };
		const auto creature = [](const LockSide& s) { return s.actor && !s.npc && !s.dragon && !s.player; };
		if (!a_config.enabled || !ready(a) || !ready(b)) {
			return false;
		}
		if (a_action != Action::kClash && !(a_action == Action::kAnnihilate && a_config.opposites)) {
			return false;  // the rules say this pair passes, or one simply wins: there is nothing to push against
		}
		if (!a.player && !b.player && !a_config.betweenOthers) {
			return false;
		}
		return a_config.creatures || (!creature(a) && !creature(b));
	}

	float KeepGap(const StruggleConfig& a_config) noexcept
	{
		return std::isfinite(a_config.minGap) ? std::max(a_config.minGap, 0.0f) * kKeepShare : 0.0f;
	}

	bool Facing(Vec3 a_muzzleA, Vec3 a_aimA, Vec3 a_muzzleB, Vec3 a_aimB, float a_minGap) noexcept
	{
		if (!OnMap(a_muzzleA) || !OnMap(a_muzzleB) || !Finite(a_aimA) || !Finite(a_aimB)) {
			return false;
		}
		const Vec3  ab = a_muzzleB - a_muzzleA;
		const float gap = Length(ab);
		if (!(gap >= (std::isfinite(a_minGap) ? std::max(a_minGap, 0.0f) : 0.0f)) || !(gap > 1.0f)) {
			return false;
		}
		const auto aims = [&](Vec3 a_aim, Vec3 a_toOther) {
			const float n = Length(a_aim);
			return std::isfinite(n) && n > 1e-6f && Dot(a_aim, a_toOther) >= kFacingCos * n * gap;
		};
		return aims(a_aimA, ab) && aims(a_aimB, a_muzzleA - a_muzzleB);
	}

	float PickSkill(std::span<const float, 5> a_schools, int a_own, int a_source) noexcept
	{
		std::array<float, 5> s{};
		float                best = 0.0f, sum = 0.0f;
		for (std::size_t i = 0; i < s.size(); ++i) {
			s[i] = std::clamp(Sanitize(a_schools[i], 0.0f), 0.0f, kMaxSkill);
			best = std::max(best, s[i]);
			sum += s[i];
		}
		if (a_source == 2) {
			return sum / static_cast<float>(s.size());
		}
		if (a_source == 1 || a_own < 0 || a_own >= static_cast<int>(s.size())) {
			return best;  // a spell of no school counts the caster's best
		}
		return s[static_cast<std::size_t>(a_own)];
	}

	float MagicSkill(float a_picked, float a_level, bool a_creature, bool a_breath) noexcept
	{
		const float byLevel = std::min(std::clamp(Sanitize(a_level, 1.0f), 1.0f, kMaxLevel), 100.0f);
		if (a_breath) {
			return byLevel;
		}
		const float skill = std::clamp(Sanitize(a_picked, 0.0f), 0.0f, kMaxSkill);
		return a_creature ? std::max(skill, byLevel) : skill;
	}

	float PowerMult(bool a_opposesPlayer, bool a_dragon, int a_words, const StruggleConfig& a_config) noexcept
	{
		float m = 1.0f;
		if (a_opposesPlayer) {
			m *= Clamped(a_config.enemyPower, kStruggleDefaults.enemyPower, 0.25f, 4.0f);
		}
		if (a_dragon) {
			m *= Clamped(a_config.dragonPower, kStruggleDefaults.dragonPower, 0.25f, 4.0f);
		}
		if (a_words > 0) {
			m *= 1.0f + Clamped(a_config.wordBonus, kStruggleDefaults.wordBonus, 0.0f, 1.0f) * static_cast<float>(std::min(a_words, 3) - 1);
		}
		return m;
	}

	float LogPower(const Caster& a_caster, const StruggleConfig& a_config) noexcept
	{
		const auto& d = kStruggleDefaults;
		const float level = std::clamp(Sanitize(a_caster.level, 1.0f), 1.0f, kMaxLevel);
		const float cost = std::clamp(Sanitize(a_caster.cost, 1.0f), 1.0f, kMaxCost);
		const float magicka = a_caster.paysMagicka ? std::clamp(Sanitize(a_caster.magicka, 1.0f), 0.0f, 1.0f) : 1.0f;
		const float mult = std::clamp(Sanitize(a_caster.mult, 1.0f), kMinMult, kMaxMult);
		float       p = kSkillPoint * Clamped(a_config.skillWeight, d.skillWeight, 0.0f, 3.0f) * SkillOf(a_caster);
		p += kLevelPoint * Clamped(a_config.levelWeight, d.levelWeight, 0.0f, 3.0f) * level;
		p += Clamped(a_config.spellWeight, d.spellWeight, 0.0f, 2.0f) * std::log(cost);
		// an empty caster keeps a quarter of its push (at weight 1): a lock is not lost the instant the magicka runs out
		p += Clamped(a_config.magickaWeight, d.magickaWeight, 0.0f, 2.0f) * std::log(0.25f + 0.75f * magicka);
		if (a_caster.dual) {
			p += std::log(Clamped(a_config.dualBonus, d.dualBonus, 1.0f, 3.0f));
		}
		return p + std::log(mult);
	}

	float Advantage(const Caster& a, const Caster& b, const StruggleConfig& a_config, bool a_surgeA, bool a_surgeB) noexcept
	{
		const float surge = std::log(Clamped(a_config.surgePower, kStruggleDefaults.surgePower, 1.0f, 3.0f));
		const float d = (LogPower(a, a_config) + (a_surgeA ? surge : 0.0f)) - (LogPower(b, a_config) + (a_surgeB ? surge : 0.0f));
		// tanh(d / 2) is (Pa - Pb) / (Pa + Pb); taken on |d| so B over A is exactly the negative, whatever the maths library
		float lead = std::isnan(d) ? 0.0f : d >= 0.0f ? std::tanh(0.5f * d) :
		                                                -std::tanh(-0.5f * d);
		if (a_config.skillAlwaysWins && a.school && b.school) {
			const float sa = SkillOf(a), sb = SkillOf(b);
			if (std::abs(sa - sb) >= 1.0f) {
				const float s = sa > sb ? 1.0f : -1.0f;
				lead = s * std::max(s * lead, kMinLead);
			}
		}
		return std::clamp(lead, -1.0f, 1.0f);
	}

	float PushRate(const StruggleConfig& a_config) noexcept { return 3.0f / PushTime(a_config); }

	float SecondsToWin(float a_lead, float a_pace, const StruggleConfig& a_config) noexcept
	{
		const float rate = PushRate(a_config) * std::abs(WithinOne(a_lead)) * (std::isfinite(a_pace) ? std::max(a_pace, 0.0f) : 1.0f);
		return rate > 0.0f ? std::min(kLockIn + 1.0f / rate, kNever) : kNever;
	}

	float MagickaDrain(float a_towardMe, float a_cost, float a_dt, const StruggleConfig& a_config) noexcept
	{
		const float toward = std::isfinite(a_towardMe) ? std::clamp(a_towardMe, 0.0f, 1.0f) : 0.0f;
		const float cost = std::isfinite(a_cost) ? std::clamp(a_cost, 0.0f, kMaxCost) : 0.0f;
		const float dt = std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, kMaxStep) : 0.0f;
		return Clamped(a_config.magickaPressure, kStruggleDefaults.magickaPressure, 0.0f, 3.0f) * cost * toward * dt;
	}

	Struggle Begin(std::uint32_t a_shooterA, std::uint32_t a_shooterB, Action a_reaction, Vec3 a_meet, Vec3 a_muzzleA, Vec3 a_muzzleB,
		float a_reachA, float a_reachB, bool a_bothBeams, bool a_breath, bool a_dragon, std::uint64_t a_seed, const StruggleConfig& a_config) noexcept
	{
		if (a_shooterB < a_shooterA) {
			std::swap(a_shooterA, a_shooterB);
			std::swap(a_muzzleA, a_muzzleB);
			std::swap(a_reachA, a_reachB);
			a_reaction = Mirror(a_reaction);
		}
		Struggle s;
		s.a = a_shooterA;
		s.b = a_shooterB;
		s.reaction = a_reaction;
		s.breath = a_breath;
		s.dragon = a_dragon;
		s.seed = a_seed;
		// two beams touch wherever they cross, which says nothing about who reached further
		s.startShare = a_bothBeams ? 0.5f : StartShare(a_meet, a_muzzleA, a_muzzleB);
		Place(s, a_muzzleA, a_muzzleB, a_reachA, a_reachB);
		s.phase = Roll(a_seed, a_dragon ? a_config.dragonChance : a_config.chance) ? Phase::kLocked : Phase::kDeclined;
		return s;
	}

	void Place(Struggle& a_struggle, Vec3 a_muzzleA, Vec3 a_muzzleB, float a_reachA, float a_reachB) noexcept
	{
		if (!OnMap(a_muzzleA) || !OnMap(a_muzzleB) || !std::isfinite(a_reachA) || !std::isfinite(a_reachB) ||
			!(Length(a_muzzleB - a_muzzleA) >= 1.0f)) {
			return;
		}
		a_struggle.muzzleA = a_muzzleA;
		a_struggle.muzzleB = a_muzzleB;
		a_struggle.reachA = std::clamp(a_reachA, 0.0f, kWorld);
		a_struggle.reachB = std::clamp(a_reachB, 0.0f, kWorld);
		a_struggle.placed = true;
	}

	StruggleStep Advance(Struggle& a_s, const Caster& a, const Caster& b, const Sight& a_sight, float a_dt, const StruggleConfig& a_config) noexcept
	{
		const float dt = std::isfinite(a_dt) && a_dt > 0.0f ? std::min(a_dt, kMaxStep) : 0.0f;
		// only Begin and Advance write a struggle, but nothing that is not a number may stick
		a_s.balance = WithinOne(a_s.balance);
		for (float* f : { &a_s.age, &a_s.quietA, &a_s.quietB, &a_s.timer, &a_s.surgeA, &a_s.surgeB }) {
			*f = std::isfinite(*f) ? std::clamp(*f, 0.0f, kNever) : 0.0f;
		}
		a_s.nextSpark = std::isfinite(a_s.nextSpark) ? std::clamp(a_s.nextSpark, 0.0f, kSparkEvery) : kSparkEvery;
		a_s.lead = WithinOne(a_s.lead);

		StruggleStep step;
		step.lead = a_s.lead;
		step.aWon = a_s.aWon;
		switch (a_s.phase) {
		case Phase::kBroken:
			a_s.timer += dt;
			if (a_s.timer >= BreakTime(a_config)) {
				a_s.phase = Phase::kCooldown;
				a_s.timer = 0.0f;
			}
			return step;
		case Phase::kCooldown:
			a_s.timer += dt;
			if (a_s.timer >= kCooldownTime) {
				step.event = StruggleEvent::kGone;
			}
			return step;
		case Phase::kDeclined:
			a_s.quietA = a_sight.seenA ? 0.0f : a_s.quietA + dt;
			a_s.quietB = a_sight.seenB ? 0.0f : a_s.quietB + dt;
			if (a_s.quietA >= kGrace || a_s.quietB >= kGrace) {
				step.event = StruggleEvent::kGone;
			}
			return step;
		case Phase::kLocked:
			break;
		}
		if (a_s.phase != Phase::kLocked) {
			step.event = StruggleEvent::kGone;  // not a phase at all
			return step;
		}

		if (!a_sight.valid) {
			End(a_s, step, StruggleEvent::kCalledOff, false, a_config);
			return step;
		}
		a_s.age += dt;
		a_s.quietA = a_sight.seenA ? 0.0f : a_s.quietA + dt;
		a_s.quietB = a_sight.seenB ? 0.0f : a_s.quietB + dt;
		const bool both = a_sight.seenA && a_sight.seenB;

		// a side pushed most of the way back surges, once
		a_s.surgeA = std::max(a_s.surgeA - dt, 0.0f);
		a_s.surgeB = std::max(a_s.surgeB - dt, 0.0f);
		if (a_config.surge) {
			if (!a_s.surgedA && a_s.balance <= -kSurgeAt) {
				a_s.surgedA = step.surgeA = true;
				a_s.surgeA = kSurgeTime;
			}
			if (!a_s.surgedB && a_s.balance >= kSurgeAt) {
				a_s.surgedB = step.surgeB = true;
				a_s.surgeB = kSurgeTime;
			}
		}

		a_s.lead = step.lead = Advantage(a, b, a_config, a_config.surge && a_s.surgeA > 0.0f, a_config.surge && a_s.surgeB > 0.0f);
		// one side unseen for a moment (a gap in its spray): the lock holds where it is
		if (both) {
			// the lock holds where the streams met for kLockIn; only the part of this step after that pushes
			const float pushing = std::clamp(a_s.age - kLockIn, 0.0f, dt);
			const float pace = a_s.breath ? kBreathPace : 1.0f;
			a_s.balance = std::clamp(a_s.balance + PushRate(a_config) * a_s.lead * pace * pushing, -1.0f, 1.0f);
			step.drainA = a.paysMagicka ? MagickaDrain(-a_s.balance, a.cost, dt, a_config) : 0.0f;
			step.drainB = b.paysMagicka ? MagickaDrain(a_s.balance, b.cost, dt, a_config) : 0.0f;
			a_s.nextSpark -= dt;
			if (a_s.nextSpark <= 0.0f) {
				step.spark = true;
				a_s.nextSpark += kSparkEvery;
			}
		}

		const float maxTime = Clamped(a_config.maxTime, kStruggleDefaults.maxTime, 0.0f, 60.0f);
		if (std::abs(a_s.balance) >= 1.0f) {
			End(a_s, step, StruggleEvent::kOverwhelmed, a_s.balance > 0.0f, a_config);
		} else if (a_s.quietA >= kGrace && a_s.quietB >= kGrace) {
			End(a_s, step, StruggleEvent::kReleased, false, a_config);
		} else if (a_s.quietA >= kGrace) {
			// stopping while being pushed back is losing: it ran dry, or let go rather than be overwhelmed
			End(a_s, step, -a_s.balance >= kOutlast ? StruggleEvent::kOverwhelmed : StruggleEvent::kGaveWay, false, a_config);
		} else if (a_s.quietB >= kGrace) {
			End(a_s, step, a_s.balance >= kOutlast ? StruggleEvent::kOverwhelmed : StruggleEvent::kGaveWay, true, a_config);
		} else if (maxTime > 0.0f && a_s.age >= maxTime) {
			if (std::abs(a_s.balance) > kDrawBand) {
				End(a_s, step, StruggleEvent::kOverwhelmed, a_s.balance > 0.0f, a_config);
			} else {
				End(a_s, step, StruggleEvent::kDraw, false, a_config);
			}
		}
		return step;
	}

	AxisPoint Project(Vec3 p, Vec3 a_from, Vec3 a_to) noexcept
	{
		if (!OnMap(p) || !OnMap(a_from) || !OnMap(a_to)) {
			return { 0.0f, kNowhere };
		}
		const Vec3  axis = a_to - a_from, d = p - a_from;
		const float len = Length(axis);
		if (len < 1e-6f) {
			return { 0.0f, Length(d) };
		}
		const Vec3  u = axis * (1.0f / len);
		const float along = Dot(d, u);
		return { along, Length(d - u * along) };
	}

	bool InCorridor(AxisPoint a_point, float a_length, float a_radius) noexcept
	{
		if (!std::isfinite(a_point.along) || !std::isfinite(a_point.off) || !std::isfinite(a_length)) {
			return false;
		}
		return a_point.along >= -kTubeBack && a_point.along <= a_length + kTubeBack && a_point.off <= TubeAt(a_point.along, SafeRadius(a_radius));
	}

	bool BeamAims(Vec3 a_from, Vec3 a_dir, Vec3 a_target, float a_radius) noexcept
	{
		const float n = Length(a_dir);
		if (!OnMap(a_from) || !OnMap(a_target) || !Finite(a_dir) || !std::isfinite(n) || n < 1e-6f) {
			return false;
		}
		const Vec3  u = a_dir * (1.0f / n), d = a_target - a_from;
		const float along = Dot(d, u);
		return along > 0.0f && Length(d - u * along) <= TubeAt(along, SafeRadius(a_radius));
	}

	float StartShare(Vec3 a_point, Vec3 a_muzzleA, Vec3 a_muzzleB) noexcept
	{
		const float     length = Length(a_muzzleB - a_muzzleA);
		const AxisPoint p = Project(a_point, a_muzzleA, a_muzzleB);
		if (!std::isfinite(length) || length < 1.0f || p.off == kNowhere) {
			return 0.5f;
		}
		return std::clamp(p.along / length, kMinShare, kMaxShare);
	}

	float StreamReach(float a_range, float a_speed, float a_lifetime) noexcept
	{
		float reach = std::numeric_limits<float>::infinity();
		if (std::isfinite(a_range) && a_range > 0.0f) {
			reach = a_range;
		}
		if (std::isfinite(a_speed) && a_speed > 0.0f && std::isfinite(a_lifetime) && a_lifetime > 0.0f) {
			reach = std::min(reach, a_speed * a_lifetime);  // an overflow to infinity loses to the range, or falls back
		}
		return std::clamp(std::isfinite(reach) ? reach : kReachFallback, kReachMin, kReachMax);
	}

	bool InReach(float a_distance, float a_reachA, float a_reachB) noexcept
	{
		const auto reach = [](float r) { return std::isfinite(r) ? std::clamp(r, 0.0f, kWorld) : 0.0f; };
		return !std::isnan(a_distance) && a_distance <= kReachSlack * (reach(a_reachA) + reach(a_reachB)) + kReachExtra;
	}

	float Wobble(float a_age) noexcept
	{
		if (!std::isfinite(a_age)) {
			return 0.0f;
		}
		return std::clamp(0.6f * std::sin(5.1f * a_age) + 0.4f * std::sin(8.3f * a_age + 1.3f), -1.0f, 1.0f);
	}

	Front FrontOf(const Struggle& a_s) noexcept
	{
		Front f;
		if (!a_s.placed || !OnMap(a_s.muzzleA) || !OnMap(a_s.muzzleB)) {
			return f;
		}
		const Vec3  ab = a_s.muzzleB - a_s.muzzleA;
		const float length = Length(ab);
		if (!std::isfinite(length) || length < 1.0f) {
			return f;
		}
		const auto  reach = [](float r) { return std::isfinite(r) ? std::clamp(r, 0.0f, kWorld) : 0.0f; };
		const float share = std::isfinite(a_s.startShare) ? std::clamp(a_s.startShare, 0.0f, 1.0f) : 0.5f;
		// neither hand, nor beyond what either stream reaches: B's stream reaches from its end back to `lo`
		float lo = std::max(kHandGap, length - kReachUsed * reach(a_s.reachB));
		float hi = std::min(length - kHandGap, kReachUsed * reach(a_s.reachA));
		if (lo > hi) {
			// too close, or streams too short to meet: it stays where it is
			lo = hi = length < 2.0f * kHandGap + 1.0f ? 0.5f * length : std::clamp(share * length, kHandGap, length - kHandGap);
		}
		const float x0 = std::clamp(share * length, lo, hi);
		// the wobble fades toward either end, so the ends are still exactly the hands and the order never turns
		const float balance = WithinOne(a_s.balance);
		const float shown = balance + kWobble * Wobble(a_s.age) * (1.0f - std::abs(balance));
		f.at = std::clamp(shown >= 0.0f ? x0 + shown * (hi - x0) : x0 + shown * (x0 - lo), lo, hi);
		f.from = a_s.muzzleA;
		f.axis = ab * (1.0f / length);
		f.point = f.from + f.axis * f.at;
		f.length = length;
		f.valid = true;
		return f;
	}

	bool PastFront(const Front& a_front, bool a_sideA, Vec3 a_point, float a_radius) noexcept
	{
		if (!a_front.valid || !OnMap(a_point)) {
			return false;
		}
		const Vec3  d = a_point - a_front.from;
		const float fromA = Dot(d, a_front.axis);
		const float off = Length(d - a_front.axis * fromA);
		// each side is measured from its own muzzle, in its own tube
		const float along = a_sideA ? fromA : a_front.length - fromA;
		const float at = a_sideA ? a_front.at : a_front.length - a_front.at;
		const float r = SafeRadius(a_radius);
		return InCorridor({ along, off }, a_front.length, r) && along + r > at;
	}

	float BeamCut(const Front& a_front, Vec3 a_origin, Vec3 a_dir, float a_full) noexcept
	{
		const float full = std::isfinite(a_full) ? std::clamp(a_full, 0.0f, kWorld) : 0.0f;
		const float n = Length(a_dir);
		if (!a_front.valid || full <= kBeamMin || !OnMap(a_origin) || !Finite(a_dir) || !std::isfinite(n) || n < 1e-6f) {
			return full;
		}
		const float aligned = Dot(a_dir, a_front.axis) / n;
		if (!(std::abs(aligned) >= kBeamAligned)) {
			return full;  // nearly square to the axis: the plane is no measure of where it should end
		}
		const float toPlane = Dot(a_front.point - a_origin, a_front.axis) / aligned;
		return std::clamp(toPlane + kBeamPast, kBeamMin, full);
	}

	std::uint64_t Mix(std::uint64_t a_x) noexcept
	{
		std::uint64_t z = a_x + 0x9E3779B97F4A7C15ull;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}

	bool Roll(std::uint64_t a_seed, float a_percent) noexcept
	{
		if (!(a_percent > 0.0f)) {
			return false;
		}
		if (a_percent >= 100.0f) {
			return true;
		}
		// mixed again, so a seed that is only a counter still rolls fairly
		const std::uint64_t top = Mix(a_seed) >> 40;  // 24 bits
		return static_cast<double>(top) < static_cast<double>(a_percent) * (16777216.0 / 100.0);
	}

	float OverwhelmHit(float a_magnitude, float a_lead, float a_resist, const StruggleConfig& a_config) noexcept
	{
		const float magnitude = std::isfinite(a_magnitude) ? std::clamp(a_magnitude, 0.0f, kMaxMagnitude) : 0.0f;
		const float resist = std::isfinite(a_resist) ? std::clamp(a_resist, 0.0f, kMaxResist) : 0.0f;
		const float scale = Clamped(a_config.overwhelmDamage, kStruggleDefaults.overwhelmDamage, 0.0f, 5.0f);
		return magnitude * 2.0f * scale * (0.75f + 0.5f * std::abs(WithinOne(a_lead))) * (1.0f - resist / 100.0f);
	}

	float ShakeStrength(float a_base, float a_distance, const StruggleConfig& a_config) noexcept
	{
		const float base = std::isfinite(a_base) ? std::max(a_base, 0.0f) : 0.0f;
		const float distance = std::isfinite(a_distance) ? std::max(a_distance, 0.0f) : kShakeFade;  // unknown: too far to feel
		const float fade = std::clamp(1.0f - distance / kShakeFade, 0.0f, 1.0f);
		return std::clamp(Clamped(a_config.cameraShake, kStruggleDefaults.cameraShake, 0.0f, 1.0f) * base * fade, 0.0f, 1.0f);
	}

	float StaggerFor(float a_lead, const StruggleConfig& a_config) noexcept
	{
		return Clamped(a_config.staggerStrength, kStruggleDefaults.staggerStrength, 0.0f, 1.0f) * (0.5f + 0.5f * std::abs(WithinOne(a_lead)));
	}

	Struggle* StruggleBook::Find(std::uint32_t a, std::uint32_t b) noexcept
	{
		const auto it = _book.find(PairKey(a, b));
		return it != _book.end() ? &it->second : nullptr;
	}

	bool StruggleBook::Engaged(std::uint32_t a_shooter) const noexcept
	{
		// a handful at most: a look at each is cheaper than a second map to keep in step
		return std::ranges::any_of(_book, [&](const auto& a_kv) {
			const Struggle& s = a_kv.second;
			return (s.a == a_shooter || s.b == a_shooter) && (s.phase == Phase::kLocked || s.phase == Phase::kBroken);
		});
	}

	bool StruggleBook::Loser(std::uint32_t a_shooter) const noexcept
	{
		return std::ranges::any_of(_book, [&](const auto& a_kv) {
			const Struggle& s = a_kv.second;
			return s.phase == Phase::kBroken && (s.aWon ? s.b : s.a) == a_shooter;
		});
	}

	bool StruggleBook::Holds(std::uint32_t a, std::uint32_t b) const noexcept
	{
		const auto it = _book.find(PairKey(a, b));
		return it != _book.end() && (it->second.phase == Phase::kLocked || it->second.phase == Phase::kBroken);
	}

	Struggle& StruggleBook::Put(const Struggle& a_struggle) { return _book.insert_or_assign(PairKey(a_struggle.a, a_struggle.b), a_struggle).first->second; }

	void StruggleBook::Erase(std::uint64_t a_key) { _book.erase(a_key); }

	// ------------------------------------------------------------------ form references
	bool ParseFormRef(std::string_view a_text, FormRef& a_out)
	{
		a_text = Trim(a_text);
		if (a_text.empty()) {
			return false;
		}
		const auto bar = a_text.find_first_of("|~");
		if (bar == std::string_view::npos) {
			// an editor ID: letters, digits and _ only
			if (!std::ranges::all_of(a_text, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; })) {
				return false;
			}
			a_out = { {}, 0, std::string(a_text) };
			return true;
		}
		const auto file = Trim(a_text.substr(0, bar));
		auto       id = Trim(a_text.substr(bar + 1));
		if (file.empty() || file.find_first_of("|~") != std::string_view::npos) {
			return false;
		}
		if (id.size() > 2 && id[0] == '0' && (id[1] == 'x' || id[1] == 'X')) {
			id.remove_prefix(2);
		}
		if (id.empty() || id.size() > 8) {
			return false;
		}
		std::uint32_t value = 0;
		const auto [end, ec] = std::from_chars(id.data(), id.data() + id.size(), value, 16);
		if (ec != std::errc{} || end != id.data() + id.size()) {
			return false;
		}
		a_out = { std::string(file), value, {} };
		return true;
	}

	// ------------------------------------------------------------------ the settings files
	namespace
	{
		enum class Type : std::uint8_t
		{
			kBool,
			kInt,
			kFloat
		};

		struct Setting
		{
			std::string_view section, key;
			Type             type;
			float            lo, hi;
			float (*get)(const Config&);
			void (*set)(Config&, float);
			std::string_view note;
		};

#define CF_BOOL(sec, key, field, note)                                                                                                                  \
	Setting                                                                                                                                             \
	{                                                                                                                                                   \
		sec, key, Type::kBool, 0.0f, 1.0f, [](const Config& c) { return c.field ? 1.0f : 0.0f; }, [](Config& c, float v) { c.field = v != 0.0f; }, note \
	}
#define CF_INT(sec, key, field, lo, hi, note)                                                                                                                                   \
	Setting                                                                                                                                                                     \
	{                                                                                                                                                                           \
		sec, key, Type::kInt, lo, hi, [](const Config& c) { return static_cast<float>(c.field); }, [](Config& c, float v) { c.field = static_cast<int>(std::lround(v)); }, note \
	}
#define CF_FLOAT(sec, key, field, lo, hi, note)                                                                                \
	Setting                                                                                                                    \
	{                                                                                                                          \
		sec, key, Type::kFloat, lo, hi, [](const Config& c) { return c.field; }, [](Config& c, float v) { c.field = v; }, note \
	}

		const std::array kSettings{
			CF_BOOL("General", "Enabled", enabled, "0 turns the clashes off"),
			CF_INT("General", "Who", who, 0.0f, 1.0f, "0: every projectile can clash; 1: only a clash that involves one of yours"),
			CF_BOOL("General", "IgnoreAllies", ignoreAllies, "projectiles of shooters who are not hostile to each other pass through each other"),
			CF_FLOAT("General", "MaxDistance", maxDistance, 500.0f, 30000.0f, "projectiles further than this from you are left alone (units; 70 is a metre)"),
			CF_FLOAT("Hits", "RadiusScale", radiusScale, 0.1f, 10.0f, "times each projectile's own collision radius"),
			CF_FLOAT("Hits", "RadiusBonus", radiusBonus, 0.0f, 200.0f, "units added to every projectile's radius, so a shot does not have to be perfect"),
			CF_FLOAT("Hits", "MinRadius", minRadius, 0.0f, 100.0f, "no projectile is smaller than this"),
			CF_FLOAT("Hits", "MaxRadius", maxRadius, 1.0f, 2000.0f, "no projectile is bigger than this (a cone keeps growing as it goes)"),
			CF_FLOAT("Hits", "BarrierHeight", barrierHeight, 10.0f, 1000.0f, "how tall a wall spell stands, for catching projectiles"),
			CF_FLOAT("Hits", "HeadOnCatch", headOnCatch, 0.0f, 300.0f, "percent: how much further two bolts flying head on at each other catch each other (100 = 40 units each, 0 = off), so a near miss still clashes"),
			CF_FLOAT("Hits", "HeadOnCone", headOnCone, 0.0f, 60.0f, "degrees off straight at each other that still count as head on"),
			CF_FLOAT("Clash", "OverpowerRatio", overpowerRatio, 1.0f, 100.0f, "a projectile this many times stronger than the other destroys it and flies on"),
			CF_BOOL("Clash", "WeakenSurvivor", weakenSurvivor, "the one that flies on loses the strength it beat"),
			CF_BOOL("Clash", "WeakenDamage", weakenDamage, "and does that much less when it lands"),
			CF_BOOL("Clash", "BoltsMeet", boltsMeet, "two one-shot bolts (Lightning Bolt and its like) that cross burst and both go"),
			CF_FLOAT("Clash", "BoltLinger", boltLinger, 0.0f, 1.0f, "seconds a fired bolt's line still clashes after its projectile is gone, so two bolts need not be cast in the same instant"),
			CF_FLOAT("Clash", "StreamShare", tuning.streamShare, 0.0f, 10.0f, "a spray particle's strength, as a share of its spell's cost"),
			CF_FLOAT("Clash", "ArrowScale", tuning.arrowScale, 0.0f, 100.0f, "an arrow's strength per point of its damage"),
			CF_FLOAT("Clash", "ShoutStrength", tuning.shoutStrength, 0.0f, 100000.0f, "every shout's strength (shouts cost no magicka)"),
			CF_FLOAT("Clash", "MinSpellStrength", tuning.minSpellStrength, 0.0f, 1000.0f, "no spell is weaker than this"),
			CF_BOOL("Struggle", "Struggles", struggle.enabled, "when enemy sprays, held beams or breath meet, they lock together and push (0: they pass through each other)"),
			CF_BOOL("Struggle", "LockSprays", struggle.sprays, "Flames, Frostbite and other sprays lock"),
			CF_BOOL("Struggle", "LockBeams", struggle.beams, "Sparks, Lightning Storm and other held beams lock (a one-shot bolt never does)"),
			CF_BOOL("Struggle", "LockBreath", struggle.breath, "Fire and Frost Breath, yours or a dragon's, lock with breath, sprays and beams"),
			CF_BOOL("Struggle", "OppositesLock", struggle.opposites, "opposites that would cancel each other (Fire and Frost) lock like any other pair"),
			CF_BOOL("Struggle", "BeamsStop", struggle.beamsStop, "a locked beam is cut short at the meeting point (experimental)"),
			CF_BOOL("Struggle", "NPCStruggles", struggle.betweenOthers, "two enemies of each other (your follower and a necromancer) can lock too"),
			CF_BOOL("Struggle", "CreatureStruggles", struggle.creatures, "atronachs, hagravens, draugr and other creatures that cast a spray or beam take part"),
			CF_FLOAT("Struggle", "MinDistance", struggle.minGap, 0.0f, 300.0f, "casters whose hands are closer than this do not lock (units; 70 is a metre)"),
			CF_FLOAT("Struggle", "StruggleChance", struggle.chance, 0.0f, 100.0f, "chance (percent) two casters lock when their streams meet, rolled once each time"),
			CF_FLOAT("Struggle", "DragonChance", struggle.dragonChance, 0.0f, 100.0f, "the lock chance when one side is a dragon (0: never with dragons)"),
			CF_FLOAT("Struggle", "PushTime", struggle.pushTime, 1.0f, 30.0f, "seconds for a caster twice as strong to push from where they met to the other's hands"),
			CF_FLOAT("Struggle", "MaxStruggleTime", struggle.maxTime, 0.0f, 60.0f, "seconds; when time runs out, whoever is ahead breaks through (0: no limit)"),
			CF_BOOL("Power", "SkillAlwaysWins", struggle.skillAlwaysWins, "when both cast from a school of magic, the higher skill always pushes through"),
			CF_INT("Power", "SkillSource", struggle.skillSource, 0.0f, 2.0f, "which skill counts as a caster's magic level: 0 the spell's own school, 1 the best school, 2 the average of the five"),
			CF_FLOAT("Power", "SkillWeight", struggle.skillWeight, 0.0f, 3.0f, "how much magic skill counts (at 1, each point over the other is worth 2% more push)"),
			CF_FLOAT("Power", "LevelWeight", struggle.levelWeight, 0.0f, 3.0f, "how much character level counts (at 1, each level over the other is worth 2%)"),
			CF_FLOAT("Power", "SpellWeight", struggle.spellWeight, 0.0f, 2.0f, "how much a costlier spell counts (0: only the caster matters)"),
			CF_FLOAT("Power", "MagickaWeight", struggle.magickaWeight, 0.0f, 2.0f, "a caster low on magicka pushes less (0: magicka left does not matter)"),
			CF_FLOAT("Power", "DualCastBonus", struggle.dualBonus, 1.0f, 3.0f, "extra push for a dual cast, a staff, or streaming with both hands at once"),
			CF_FLOAT("Power", "BreathStrength", struggle.breathStrength, 1.0f, 500.0f, "what a breath counts as: a spell of this magicka cost (Flames is about 14)"),
			CF_FLOAT("Power", "ShoutWordBonus", struggle.wordBonus, 0.0f, 1.0f, "each word of a breath shout past the first adds this share of push"),
			CF_FLOAT("Power", "EnemyPower", struggle.enemyPower, 0.25f, 4.0f, "how hard whoever struggles against you pushes (above 1 is harder)"),
			CF_FLOAT("Power", "DragonPower", struggle.dragonPower, 0.25f, 4.0f, "how hard a dragon's breath pushes"),
			CF_FLOAT("Power", "MagickaPressure", struggle.magickaPressure, 0.0f, 3.0f, "the caster being pushed back pays extra magicka, more the further back (0: only the spell's own cost)"),
			CF_BOOL("Power", "Surge", struggle.surge, "once per struggle, a caster pushed most of the way back surges for 3 seconds"),
			CF_FLOAT("Power", "SurgePower", struggle.surgePower, 1.0f, 3.0f, "how much harder a surging caster pushes"),
			CF_BOOL("Aftermath", "BreakCast", struggle.breakCast, "the overwhelmed caster's spell is interrupted, in the hand that lost"),
			CF_FLOAT("Aftermath", "BreakTime", struggle.breakTime, 0.0f, 5.0f, "seconds the overwhelmed caster reels: any spray or beam they cast in that time fizzles"),
			CF_BOOL("Aftermath", "StaggerLoser", struggle.stagger, "the overwhelmed caster staggers, with the game's own stagger (not dragons)"),
			CF_BOOL("Aftermath", "StaggerPlayer", struggle.staggerPlayer, "you also stagger when you are overwhelmed"),
			CF_FLOAT("Aftermath", "StaggerStrength", struggle.staggerStrength, 0.0f, 1.0f, "how hard the stagger is (a clearer win staggers harder)"),
			CF_FLOAT("Aftermath", "OverwhelmDamage", struggle.overwhelmDamage, 0.0f, 5.0f, "an extra hit on the overwhelmed caster, about two seconds of the winning stream (0: none)"),
			CF_BOOL("Aftermath", "Finishers", struggle.finishers, "a breakthrough that kills throws the body away from the winner (experimental)"),
			CF_FLOAT("Aftermath", "FinisherForce", struggle.finisherForce, 0.0f, 20.0f, "how hard the body is thrown"),
			CF_BOOL("Aftermath", "Intimidate", struggle.intimidate, "when you break through and kill, nearby enemies of lower level than your victim are frightened"),
			CF_FLOAT("Aftermath", "StruggleXP", struggle.xp, 0.0f, 200.0f, "skill experience in your spell's school each time you overwhelm someone, doubled if it kills (0: none)"),
			CF_BOOL("Aftermath", "StruggleMessages", struggle.messages, "a message when you overwhelm someone, they overwhelm you, they give way, or the spells burst between you"),
			CF_BOOL("Show", "LockBursts", struggle.lockBursts, "bursts where streams lock, now and then while they grind, and where one breaks"),
			CF_FLOAT("Show", "CameraShake", struggle.cameraShake, 0.0f, 1.0f, "a kick when your stream locks, a rumble as the lock nears you, a jolt on a breakthrough (0: off)"),
			CF_BOOL("Show", "StruggleBar", struggle.bar, "while you are locked, a bar shows who is pushing (needs SKSE Menu Framework)"),
			CF_FLOAT("Show", "BarHeight", struggle.barHeight, 0.0f, 100.0f, "how far down the screen the bar sits (percent)"),
			CF_FLOAT("Show", "BarScale", struggle.barScale, 0.5f, 2.0f, "how big the bar is"),
			CF_FLOAT("Show", "BarOpacity", struggle.barOpacity, 0.1f, 1.0f, "how solid the bar is"),
			CF_BOOL("Show", "BarNames", struggle.barNames, "your name and theirs over the bar"),
			CF_BOOL("Show", "BarSkills", struggle.barSkills, "both magic skills under the bar"),
			CF_BOOL("Effects", "Explosions", explosions, "a destroyed projectile bursts where it was hit, with its own explosion"),
			CF_BOOL("Effects", "SafeExplosionsOnly", safeExplosionsOnly, "skip an explosion that would do more than show (damage, an enchantment, something it spawns)"),
			CF_BOOL("Effects", "StandInBursts", standInBursts, "a spell with no explosion of its own (Firebolt, Ice Spike, Lightning Bolt) bursts as its element's spells do"),
			CF_FLOAT("Effects", "BurstScale", burstScale, 0.5f, 3.0f, "how big a clash's burst is"),
			CF_INT("Effects", "MaxExplosionsPerFrame", maxExplosionsPerFrame, 0.0f, 32.0f, "at most this many bursts in one frame"),
			CF_FLOAT("Effects", "ExplosionCooldown", explosionCooldown, 0.0f, 5.0f, "seconds between bursts for the same two shooters (sprays clash many times a second)"),
			CF_INT("Effects", "MaxContactsPerFrame", maxContactsPerFrame, 1.0f, 512.0f, "at most this many clashes are settled in one frame"),
			CF_FLOAT("Player", "SkillXP", skillXP, 0.0f, 1000.0f, "skill experience for each enemy projectile one of yours destroys (0: none)"),
			CF_BOOL("AutoLock", "LockYours", lockYours, "your bolts fly at the enemy they are cast toward"),
			CF_BOOL("AutoLock", "LockOthers", lockOthers, "every other caster's bolts fly at whoever it fights"),
			CF_BOOL("AutoLock", "SteerIntoSpells", steer, "a bolt turns into an oncoming enemy spell that would nearly miss it"),
			CF_BOOL("AutoLock", "LockArrows", lockArrows, "arrows lock and steer too"),
			CF_FLOAT("AutoLock", "LockCone", lockCone, 1.0f, 60.0f, "degrees: a bolt aimed further off its target than this keeps its own aim"),
			CF_FLOAT("AutoLock", "LockRange", lockRange, 200.0f, 10000.0f, "a target further away than this is not locked onto (units; 70 is a metre)"),
			CF_FLOAT("AutoLock", "SteerRange", steerRange, 50.0f, 2000.0f, "how near an oncoming spell must be before a bolt turns into it"),
			CF_FLOAT("AutoLock", "SteerCone", steerCone, 1.0f, 45.0f, "degrees: both spells must fly within this of the line between them"),
			CF_FLOAT("AutoLock", "SteerTurn", steerTurn, 10.0f, 720.0f, "degrees a second a bolt can turn while it steers"),
			CF_BOOL("Events", "ModEvents", modEvents, "send the Papyrus mod event ArcaneOverdrive_Clash"),
			CF_BOOL("Debug", "Log", debugLog, "write every clash to SpellMasteryHeat.log"),
		};
#undef CF_BOOL
#undef CF_INT
#undef CF_FLOAT

		constexpr std::array<std::string_view, 12> kSettingSections{ "General", "Hits", "Clash", "Struggle", "Power", "Aftermath", "Show",
			"Effects", "Player", "AutoLock", "Events", "Debug" };

		[[nodiscard]] bool ParseNumber(std::string_view a_text, float& a_out) noexcept
		{
			a_text = Trim(a_text);
			if (!a_text.empty() && a_text.front() == '+') {
				a_text.remove_prefix(1);
			}
			if (a_text.empty()) {
				return false;
			}
			float value = 0.0f;
			const auto [end, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), value);
			if (ec != std::errc{} || end != a_text.data() + a_text.size() || !std::isfinite(value)) {
				return false;
			}
			a_out = value;
			return true;
		}

		[[nodiscard]] bool ParseBool(std::string_view a_text, float& a_out) noexcept
		{
			a_text = Trim(a_text);
			for (const auto yes : { "1", "true", "yes", "on" }) {
				if (SameText(a_text, yes)) {
					a_out = 1.0f;
					return true;
				}
			}
			for (const auto no : { "0", "false", "no", "off" }) {
				if (SameText(a_text, no)) {
					a_out = 0.0f;
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] std::string FormatNumber(float a_value)
		{
			char buf[64];
			const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), a_value);
			return ec == std::errc{} ? std::string(buf, end) : std::string("0");
		}

		// "a, b ,c" -> {"a","b","c"}, empties dropped
		template <class F>
		void ForEachItem(std::string_view a_list, F&& a_f)
		{
			while (!a_list.empty()) {
				const auto comma = a_list.find(',');
				const auto item = Trim(a_list.substr(0, comma));
				if (!item.empty()) {
					a_f(item);
				}
				if (comma == std::string_view::npos) {
					break;
				}
				a_list.remove_prefix(comma + 1);
			}
		}

		// "Fire.Frost", "Fire vs Frost", "Fire:Frost", "Fire,Frost"; either side may be *
		[[nodiscard]] bool SplitPair(std::string_view a_key, std::string_view& a_left, std::string_view& a_right) noexcept
		{
			a_key = Trim(a_key);
			for (std::size_t i = 0; i + 4 <= a_key.size(); ++i) {
				if (SameText(a_key.substr(i, 4), " vs ")) {
					a_left = Trim(a_key.substr(0, i));
					a_right = Trim(a_key.substr(i + 4));
					return !a_left.empty() && !a_right.empty();
				}
			}
			const auto sep = a_key.find_first_of(".:,");
			if (sep == std::string_view::npos) {
				return false;
			}
			a_left = Trim(a_key.substr(0, sep));
			a_right = Trim(a_key.substr(sep + 1));
			return !a_left.empty() && !a_right.empty() && a_right.find_first_of(".:,") == std::string_view::npos;
		}

		[[nodiscard]] bool ElementsOf(std::string_view a_side, std::vector<Element>& a_out) noexcept
		{
			a_out.clear();
			if (a_side == "*") {
				for (std::size_t i = 0; i < kElements; ++i) {
					a_out.push_back(static_cast<Element>(i));
				}
				return true;
			}
			Element e{};
			if (!ParseElement(a_side, e)) {
				return false;
			}
			a_out.push_back(e);
			return true;
		}
	}

	Config::Config()
	{
		keywords[static_cast<std::size_t>(Element::kFire)] = { "MagicDamageFire" };
		keywords[static_cast<std::size_t>(Element::kFrost)] = { "MagicDamageFrost" };
		keywords[static_cast<std::size_t>(Element::kShock)] = { "MagicDamageShock" };
	}

	Range RangeOf(std::string_view a_key) noexcept
	{
		for (const auto& s : kSettings) {
			if (SameText(s.key, a_key)) {
				return { s.lo, s.hi };
			}
		}
		return { 0.0f, 0.0f };
	}

	void ParseIni(std::string_view a_text, Config& a_config, std::vector<std::string>& a_warnings)
	{
		const auto warn = [&](std::size_t a_line, std::string_view a_what, std::string_view a_detail) {
			if (a_warnings.size() < 200) {  // a garbage file must not flood the log
				a_warnings.push_back("line " + std::to_string(a_line) + ": " + std::string(a_what) + " '" +
									 std::string(a_detail.substr(0, 80)) + "'");
			}
		};
		if (a_text.starts_with("\xEF\xBB\xBF")) {
			a_text.remove_prefix(3);  // a UTF-8 byte order mark, as Notepad writes
		}
		enum class Where : std::uint8_t
		{
			kNone,
			kSettings,
			kElementLists,
			kReactions,
			kExclude,
			kUnknown
		};
		Where                where = Where::kNone;
		std::string          section;
		std::size_t          lineNo = 0;
		std::vector<Element> left, right;
		while (!a_text.empty()) {
			const auto nl = a_text.find_first_of("\r\n");
			auto       line = a_text.substr(0, nl);
			if (nl == std::string_view::npos) {
				a_text = {};
			} else {
				// \r\n is one line end
				const std::size_t skip = (a_text[nl] == '\r' && nl + 1 < a_text.size() && a_text[nl + 1] == '\n') ? 2 : 1;
				a_text.remove_prefix(nl + skip);
			}
			++lineNo;
			line = Trim(line);
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			if (line.front() == '[') {
				const auto close = line.find(']');
				if (close == std::string_view::npos) {
					warn(lineNo, "a section with no ]", line);
					where = Where::kUnknown;
					continue;
				}
				section = std::string(Trim(line.substr(1, close - 1)));
				where = Where::kUnknown;
				for (const auto s : kSettingSections) {
					if (SameText(section, s)) {
						where = Where::kSettings;
						section = std::string(s);
					}
				}
				if (SameText(section, "Elements")) {
					where = Where::kElementLists;
				} else if (SameText(section, "Reactions")) {
					where = Where::kReactions;
				} else if (SameText(section, "Exclude")) {
					where = Where::kExclude;
				}
				if (where == Where::kUnknown) {
					warn(lineNo, "an unknown section", section);
				}
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string_view::npos) {
				warn(lineNo, "no = in", line);
				continue;
			}
			const auto key = Trim(line.substr(0, eq));
			auto       value = line.substr(eq + 1);
			if (const auto comment = value.find_first_of(";#"); comment != std::string_view::npos) {
				value = value.substr(0, comment);
			}
			value = Trim(value);
			if (key.empty()) {
				warn(lineNo, "no key before =", line);
				continue;
			}
			switch (where) {
			case Where::kNone:
				warn(lineNo, "a setting before any section", key);
				break;
			case Where::kUnknown:
				break;  // said once, at the section
			case Where::kSettings:
				{
					const Setting* found = nullptr;
					for (const auto& s : kSettings) {
						if (SameText(s.section, section) && SameText(s.key, key)) {
							found = &s;
						}
					}
					if (!found) {
						warn(lineNo, "an unknown setting", key);
						break;
					}
					float v = 0.0f;
					if (!(found->type == Type::kBool ? ParseBool(value, v) : ParseNumber(value, v))) {
						warn(lineNo, found->type == Type::kBool ? "not 0 or 1" : "not a number", value);
						break;
					}
					if (found->type == Type::kInt) {
						v = std::round(v);
					}
					if (v < found->lo || v > found->hi) {
						warn(lineNo, "out of range, clamped", key);
						v = std::clamp(v, found->lo, found->hi);
					}
					found->set(a_config, v);
					break;
				}
			case Where::kElementLists:
				{
					Element e{};
					if (!ParseElement(key, e)) {
						warn(lineNo, "not an element", key);
						break;
					}
					auto& list = a_config.keywords[static_cast<std::size_t>(e)];
					if (!value.empty() && value.front() == '+') {
						value.remove_prefix(1);
					} else {
						list.clear();
					}
					ForEachItem(value, [&](std::string_view a_kw) {
						if (std::ranges::none_of(list, [&](const std::string& s) { return SameText(s, a_kw); })) {
							list.emplace_back(a_kw);
						}
					});
					break;
				}
			case Where::kReactions:
				{
					std::string_view l, r;
					Action           action{};
					if (!SplitPair(key, l, r) || !ElementsOf(l, left) || !ElementsOf(r, right)) {
						warn(lineNo, "not a pair of elements", key);
						break;
					}
					if (!ParseAction(value, action)) {
						warn(lineNo, "not Clash, Annihilate, Pass, Wins or Loses", value);
						break;
					}
					for (const auto a : left) {
						for (const auto b : right) {
							// "Force.*=Wins" must not turn Force against itself into a contest it then loses
							if (a == b && left.size() * right.size() > 1 && (action == Action::kWins || action == Action::kLoses)) {
								continue;
							}
							SetReaction(a_config.reactions, a, b, action);
						}
					}
					break;
				}
			case Where::kExclude:
				ForEachItem(value, [&](std::string_view a_item) {
					FormRef ref;
					if (!ParseFormRef(a_item, ref)) {
						warn(lineNo, "not Plugin.esp|0x123456 or an editor ID", a_item);
					} else if (std::ranges::find(a_config.exclude, ref) == a_config.exclude.end()) {
						a_config.exclude.push_back(std::move(ref));
					}
				});
				break;
			}
		}
	}

	std::string WriteSettings(const Config& a_config)
	{
		std::string out =
			"; Arcane Overdrive's clashes and spell struggles - written by its menu (SKSE Menu Framework). Rules (elements,\n"
			"; reactions, exclusions) are in SpellMasteryHeat Clash Rules.ini, which the menu never writes.\n";
		for (const auto sec : kSettingSections) {
			out += "\n[";
			out += sec;
			out += "]\n";
			for (const auto& s : kSettings) {
				if (s.section != sec) {
					continue;
				}
				out += "; ";
				out += s.note;
				out += "\n";
				out += s.key;
				out += "=";
				const float v = s.get(a_config);
				out += s.type == Type::kFloat ? FormatNumber(v) : std::to_string(static_cast<int>(std::lround(v)));
				out += "\n";
			}
		}
		return out;
	}
}

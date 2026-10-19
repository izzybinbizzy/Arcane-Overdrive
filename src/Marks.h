// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The scorch and frost marks, as plain numbers (his 2026-10-07 lingering-effects design: "at Blazing+ also a scorch / frost
// mark ... capped"; his answer 2026-10-18, checklist 28 (b): the marks hurt, behind a menu switch, on by default). Marks.cpp
// finds the impacts and draws and hurts; this keeps the marks alive, capped, refreshed and ticking. Tested in tests/test_heat.cpp.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Marks
{
	struct Point
	{
		float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
	};

	struct Rules
	{
		std::size_t max{ 6 };         // marks alive at once (the design's "at most 6")
		float       life{ 8.0f };     // seconds a mark lasts
		float       merge{ 60.0f };   // a new impact this close to a live mark of the same element refreshes it instead
		float       radius{ 80.0f };  // how far a mark reaches (across, and up / down)
		float       tick{ 0.5f };     // seconds between two hurts
	};

	struct Mark
	{
		std::uint32_t id{ 0 };
		Point         at{};
		int           element{ 0 };  // 1 fire (a scorch), 2 frost
		float         age{ 0.0f };
		float         tickIn{ 0.0f };  // seconds to its next hurt
		std::uint64_t place{ 0 };      // where it was left: an interior cell, or an exterior worldspace (Marks.cpp PlaceOf)
	};

	struct Added
	{
		std::uint32_t id{ 0 };         // the mark that holds the impact now (0: none - a bad point)
		bool          fresh{ false };  // a new mark (else a live one was refreshed)
		std::uint32_t ended{ 0 };      // a mark ended to make room (0: none)
	};

	[[nodiscard]] inline bool Finite(const Point& a_p) noexcept { return std::isfinite(a_p.x) && std::isfinite(a_p.y) && std::isfinite(a_p.z); }

	// within a mark's reach: across (in the ground's plane) and up / down, each within the radius
	[[nodiscard]] inline bool Inside(const Point& a_mark, const Point& a_actor, float a_radius) noexcept
	{
		if (!Finite(a_mark) || !Finite(a_actor) || !(a_radius > 0.0f)) {
			return false;
		}
		const float dx = a_actor.x - a_mark.x, dy = a_actor.y - a_mark.y, dz = a_actor.z - a_mark.z;
		return dx * dx + dy * dy <= a_radius * a_radius && std::abs(dz) <= a_radius;
	}

	// one hurt: a_perSecond over one tick, x1.5 in Overdrive ("scaled by heat"), less the target's resistance (percent, capped
	// at 85 like the game's own)
	[[nodiscard]] inline float Hurt(float a_perSecond, float a_tick, bool a_overdrive, float a_resistPct) noexcept
	{
		if (!std::isfinite(a_perSecond) || !std::isfinite(a_tick) || a_perSecond <= 0.0f || a_tick <= 0.0f) {
			return 0.0f;
		}
		const float resist = std::isfinite(a_resistPct) ? std::clamp(a_resistPct, 0.0f, 85.0f) : 0.0f;
		return a_perSecond * a_tick * (a_overdrive ? 1.5f : 1.0f) * (1.0f - resist / 100.0f);
	}

	class Pool
	{
	public:
		[[nodiscard]] const std::vector<Mark>& All() const noexcept { return _marks; }

		// an impact at a_at of a_element: refreshes a live mark of the same element close by, else a new mark (the oldest
		// ends first when the pool is full)
		Added Add(const Point& a_at, int a_element, const Rules& a_r, std::uint64_t a_place = 0)
		{
			Added out;
			if (!Finite(a_at) || a_r.max == 0) {
				return out;
			}
			for (auto& m : _marks) {
				const float dx = m.at.x - a_at.x, dy = m.at.y - a_at.y, dz = m.at.z - a_at.z;
				if (m.element == a_element && m.place == a_place && dx * dx + dy * dy + dz * dz <= a_r.merge * a_r.merge) {
					m.age = 0.0f;
					out.id = m.id;
					return out;
				}
			}
			if (_marks.size() >= a_r.max) {
				const auto oldest = std::ranges::max_element(_marks, {}, &Mark::age);
				out.ended = oldest->id;
				_marks.erase(oldest);
			}
			_marks.push_back({ ++_next, a_at, a_element, 0.0f, a_r.tick, a_place });
			out.id = _marks.back().id;
			out.fresh = true;
			return out;
		}

		// the player is somewhere else now (a load door, fast travel): every mark left in another place ends (their ids into
		// a_ended) - their coordinates mean nothing here, and an actor standing at the same numbers must not be hurt
		void DropElsewhere(std::uint64_t a_here, std::vector<std::uint32_t>& a_ended)
		{
			std::erase_if(_marks, [&](const Mark& m) {
				if (m.place == a_here) {
					return false;
				}
				a_ended.push_back(m.id);
				return true;
			});
		}

		// a_dt seconds on: the marks whose time is up end (their ids into a_ended), the ones whose hurt is due tick (into
		// a_ticked). One tick per mark per call at most, so a long frame never hurts twice at once.
		void Advance(float a_dt, const Rules& a_r, std::vector<std::uint32_t>& a_ended, std::vector<std::uint32_t>& a_ticked)
		{
			const float dt = std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, 0.25f) : 0.0f;
			const float tick = a_r.tick > 0.05f ? a_r.tick : 0.05f;
			for (auto it = _marks.begin(); it != _marks.end();) {
				it->age += dt;
				if (it->age >= a_r.life) {
					a_ended.push_back(it->id);
					it = _marks.erase(it);
					continue;
				}
				it->tickIn -= dt;
				if (it->tickIn <= 0.0f) {
					a_ticked.push_back(it->id);
					it->tickIn = std::max(it->tickIn + tick, 0.0f);
				}
				++it;
			}
		}

		[[nodiscard]] const Mark* Find(std::uint32_t a_id) const noexcept
		{
			const auto it = std::ranges::find(_marks, a_id, &Mark::id);
			return it == _marks.end() ? nullptr : &*it;
		}

		void Clear() noexcept { _marks.clear(); }

	private:
		std::vector<Mark> _marks;
		std::uint32_t     _next{ 0 };
	};
}

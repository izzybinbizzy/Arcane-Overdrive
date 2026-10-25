// Arcane Overdrive - tests for the clashes' and spell struggles' game-free core (src/ClashCore.*, Crossfire's). Built by
// plugin build.py (xmake target test_clash) and by clang check.py under its sanitizers - one file, so the core is compiled in.
// GPL-3.0-or-later; see LICENSE.txt.

#include "ClashCore.cpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace Crossfire::Core;

namespace
{
	int gChecks = 0, gFailed = 0;

	void Check(bool a_ok, const char* a_what, const char* a_file, int a_line)
	{
		++gChecks;
		if (!a_ok) {
			++gFailed;
			std::printf("FAIL %s:%d: %s\n", a_file, a_line, a_what);
		}
	}
#define CHECK(x) Check(static_cast<bool>(x), #x, __FILE__, __LINE__)
#define NEAR(a, b, eps) Check(std::abs(static_cast<double>(a) - static_cast<double>(b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

	constexpr float kPi = 3.14159265358979f;

	Body Sphere(std::uint32_t id, Vec3 from, Vec3 to, float r, Element e = Element::kFire, float s = 10.0f, std::uint32_t shooter = 0)
	{
		Body b;
		b.id = id;
		b.shooter = shooter ? shooter : id + 1000;
		b.shape = Shape::kSphere;
		b.from = from;
		b.to = to;
		b.radius = r;
		b.element = e;
		b.strength = s;
		return b;
	}

	Body Beam(std::uint32_t id, Vec3 from, Vec3 to, float r)
	{
		Body b = Sphere(id, from, to, r, Element::kShock);
		b.shape = Shape::kBeam;
		b.immune = true;
		return b;
	}

	Body Wall(std::uint32_t id, Vec3 base, Vec3 across, float halfWidth, float height, float r = 0.0f)
	{
		Body b = Sphere(id, base, base, r, Element::kFrost);
		b.shape = Shape::kBarrier;
		b.immune = true;
		b.across = across;
		b.halfWidth = halfWidth;
		b.height = height;
		return b;
	}

	void TestVectors()
	{
		const Vec3 north = DirectionFromAngles(0.0f, 0.0f);
		NEAR(north.x, 0.0f, 1e-6);
		NEAR(north.y, 1.0f, 1e-6);
		NEAR(north.z, 0.0f, 1e-6);
		const Vec3 east = DirectionFromAngles(0.0f, kPi / 2);
		NEAR(east.x, 1.0f, 1e-6);
		NEAR(east.y, 0.0f, 1e-6);
		const Vec3 down = DirectionFromAngles(kPi / 2, 1.234f);
		NEAR(down.z, -1.0f, 1e-6);
		NEAR(Length(down), 1.0f, 1e-6);
		for (int i = 0; i < 100; ++i) {
			NEAR(Length(DirectionFromAngles(static_cast<float>(i) * 0.37f, static_cast<float>(i) * 0.91f)), 1.0f, 1e-5);
		}
		NEAR(DistancePointSegment({ 0, 5, 0 }, { -10, 0, 0 }, { 10, 0, 0 }), 5.0f, 1e-6);
		NEAR(DistancePointSegment({ 20, 0, 0 }, { -10, 0, 0 }, { 10, 0, 0 }), 10.0f, 1e-6);
		NEAR(DistancePointSegment({ 3, 4, 0 }, { 0, 0, 0 }, { 0, 0, 0 }), 5.0f, 1e-6);  // degenerate segment
		CHECK(Finite({ 1, 2, 3 }));
		CHECK(!Finite({ NAN, 0, 0 }));
		CHECK(!Finite({ 0, INFINITY, 0 }));
	}

	void TestSphereSphere()
	{
		Touch t;
		// head on, each covering 200 units in the frame: without a swept test they would pass through each other
		CHECK(FirstTouch(Sphere(1, { 0, -100, 0 }, { 0, 100, 0 }, 5), Sphere(2, { 0, 100, 0 }, { 0, -100, 0 }, 5), t));
		NEAR(t.t, 0.475f, 1e-5);
		NEAR(t.point.y, 0.0f, 1e-3);
		// unequal radii: the point sits where the surfaces meet
		CHECK(FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, 100, 0 }, 10), Sphere(2, { 0, 100, 0 }, { 0, 100, 0 }, 30), t));
		NEAR(t.t, 0.6f, 1e-5);
		NEAR(t.point.y, 70.0f, 1e-3);
		// grazing: centres pass 10.01 apart with radii 5 + 5
		CHECK(!FirstTouch(Sphere(1, { 10.01f, -100, 0 }, { 10.01f, 100, 0 }, 5), Sphere(2, { 0, 100, 0 }, { 0, -100, 0 }, 5), t));
		CHECK(FirstTouch(Sphere(1, { 9.99f, -100, 0 }, { 9.99f, 100, 0 }, 5), Sphere(2, { 0, 100, 0 }, { 0, -100, 0 }, 5), t));
		// flying side by side at the same speed never meet
		CHECK(!FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, 500, 0 }, 5), Sphere(2, { 20, 0, 0 }, { 20, 500, 0 }, 5), t));
		// already touching at the start of the frame
		CHECK(FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, 500, 0 }, 5), Sphere(2, { 8, 0, 0 }, { 8, 500, 0 }, 5), t));
		NEAR(t.t, 0.0f, 0);
		// moving apart
		CHECK(!FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, -100, 0 }, 5), Sphere(2, { 0, 20, 0 }, { 0, 120, 0 }, 5), t));
		// both still, apart / overlapping
		CHECK(!FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, 0, 0 }, 5), Sphere(2, { 0, 20, 0 }, { 0, 20, 0 }, 5), t));
		CHECK(FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, 0, 0 }, 5), Sphere(2, { 0, 9, 0 }, { 0, 9, 0 }, 5), t));
		// would touch only after the frame ends
		CHECK(!FirstTouch(Sphere(1, { 0, 0, 0 }, { 0, 10, 0 }, 1), Sphere(2, { 0, 100, 0 }, { 0, 90, 0 }, 1), t));
		// zero radii crossing exactly
		CHECK(FirstTouch(Sphere(1, { -1, 0, 0 }, { 1, 0, 0 }, 0), Sphere(2, { 1, 0, 0 }, { -1, 0, 0 }, 0), t));
		NEAR(t.t, 0.5f, 1e-6);
		// order does not matter
		Touch      u;
		const Body a = Sphere(1, { 3, -50, 7 }, { -2, 60, 1 }, 6), b = Sphere(2, { 0, 70, 3 }, { 1, -40, 2 }, 4);
		CHECK(FirstTouch(a, b, t) && FirstTouch(b, a, u));
		NEAR(t.t, u.t, 1e-6);
		NEAR(t.point.y, u.point.y, 1e-2);
	}

	void TestSphereBeam()
	{
		Touch      t;
		const Body beam = Beam(9, { -1000, 0, 0 }, { 1000, 0, 0 }, 2);
		// a missile crossing the bolt from y=-100 to y=100: first touch where y = -(5 + 2)
		CHECK(FirstTouch(Sphere(1, { 0, -100, 0 }, { 0, 100, 0 }, 5), beam, t));
		NEAR(t.t, (100.0f - 7.0f) / 200.0f, 1e-4);
		NEAR(t.point.y, -2.0f, 1e-2);
		CHECK(FirstTouch(beam, Sphere(1, { 0, -100, 0 }, { 0, 100, 0 }, 5), t));  // either order
		// passing beyond the end of the bolt
		CHECK(!FirstTouch(Sphere(1, { 1010, -100, 0 }, { 1010, 100, 0 }, 5), beam, t));
		// over it
		CHECK(!FirstTouch(Sphere(1, { 0, -100, 8 }, { 0, 100, 8 }, 5), beam, t));
		CHECK(FirstTouch(Sphere(1, { 0, -100, 6.9f }, { 0, 100, 6.9f }, 5), beam, t));
		// flying along it, already inside
		CHECK(FirstTouch(Sphere(1, { -500, 0, 0 }, { 500, 0, 0 }, 5), beam, t));
		NEAR(t.t, 0.0f, 0);
	}

	void TestSphereBarrier()
	{
		Touch      t;
		const Body wall = Wall(9, { 0, 0, 0 }, { 1, 0, 0 }, 100, 150);
		CHECK(FirstTouch(Sphere(1, { 0, -100, 50 }, { 0, 100, 50 }, 5), wall, t));
		NEAR(t.t, 95.0f / 200.0f, 1e-4);
		NEAR(t.point.y, 0.0f, 1e-2);
		NEAR(t.point.z, 50.0f, 1e-2);
		CHECK(FirstTouch(wall, Sphere(1, { 0, -100, 50 }, { 0, 100, 50 }, 5), t));
		CHECK(!FirstTouch(Sphere(1, { 0, -100, 160 }, { 0, 100, 160 }, 5), wall, t));    // over the top
		CHECK(FirstTouch(Sphere(1, { 0, -100, 154 }, { 0, 100, 154 }, 5), wall, t));     // clipping the top
		CHECK(!FirstTouch(Sphere(1, { 120, -100, 50 }, { 120, 100, 50 }, 5), wall, t));  // beside it
		CHECK(!FirstTouch(Sphere(1, { 0, -100, -10 }, { 0, 100, -10 }, 5), wall, t));    // under it (below its base)
		// a wall turned 45 degrees
		const float s = std::sqrt(0.5f);
		const Body  turned = Wall(9, { 0, 0, 0 }, { s, s, 0 }, 100, 150);
		CHECK(FirstTouch(Sphere(1, { 50, -50, 50 }, { -50, 50, 50 }, 1), turned, t));
		NEAR(t.point.x, 0.0f, 1.0);
		NEAR(DistancePointBarrier({ 0, 10, 50 }, wall), 10.0f, 1e-4);
		NEAR(DistancePointBarrier({ 0, 0, 200 }, wall), 50.0f, 1e-4);
		NEAR(DistancePointBarrier({ 103, 4, 50 }, wall), 5.0f, 1e-4);
	}

	void TestBeamBeamAndOthers()
	{
		Touch t;
		CHECK(FirstTouch(Beam(1, { -10, 0, 0 }, { 10, 0, 0 }, 1), Beam(2, { 0, -10, 1.5f }, { 0, 10, 1.5f }, 1), t));
		CHECK(!FirstTouch(Beam(1, { -10, 0, 0 }, { 10, 0, 0 }, 1), Beam(2, { 0, -10, 2.5f }, { 0, 10, 2.5f }, 1), t));
		CHECK(!FirstTouch(Beam(1, { -10, 0, 0 }, { 10, 0, 0 }, 1), Wall(2, { 0, 0, 0 }, { 0, 1, 0 }, 10, 10), t));
		CHECK(!FirstTouch(Wall(1, { 0, 0, 0 }, { 1, 0, 0 }, 10, 10), Wall(2, { 0, 0, 0 }, { 0, 1, 0 }, 10, 10), t));
	}

	void TestValid()
	{
		CHECK(Valid(Sphere(1, { 0, 0, 0 }, { 1, 1, 1 }, 5)));
		CHECK(!Valid(Sphere(1, { NAN, 0, 0 }, { 1, 1, 1 }, 5)));
		CHECK(!Valid(Sphere(1, { 0, 0, 0 }, { 1, INFINITY, 1 }, 5)));
		CHECK(!Valid(Sphere(1, { 0, 0, 0 }, { 1, 1, 1 }, -1)));
		CHECK(!Valid(Sphere(1, { 0, 0, 0 }, { 1, 1, 1 }, NAN)));
		CHECK(!Valid(Sphere(1, { 2e7f, 0, 0 }, { 1, 1, 1 }, 5)));
		Body b = Sphere(1, { 0, 0, 0 }, { 1, 1, 1 }, 5);
		b.strength = NAN;
		CHECK(!Valid(b));
		b.strength = -1;
		CHECK(!Valid(b));
		b = Sphere(1, { 0, 0, 0 }, { 1, 1, 1 }, 5);
		b.element = static_cast<Element>(99);
		CHECK(!Valid(b));
		b = Sphere(1, { 0, 0, 0 }, { 1, 1, 1 }, 5);
		b.shape = static_cast<Shape>(7);
		CHECK(!Valid(b));
		CHECK(Valid(Wall(1, { 0, 0, 0 }, { 1, 0, 0 }, 10, 10)));
		CHECK(!Valid(Wall(1, { 0, 0, 0 }, { 2, 0, 0 }, 10, 10)));  // not a unit axis
		CHECK(!Valid(Wall(1, { 0, 0, 0 }, { 0, 0, 1 }, 10, 10)));  // not horizontal
		CHECK(!Valid(Wall(1, { 0, 0, 0 }, { 1, 0, 0 }, 0, 10)));   // no width
		CHECK(!Valid(Wall(1, { 0, 0, 0 }, { 1, 0, 0 }, 10, -1)));  // no height
		CHECK(!Valid(Wall(1, { 0, 0, 0 }, { NAN, 0, 0 }, 10, 10)));
		const Box box = Bounds(Wall(1, { 0, 0, 0 }, { 1, 0, 0 }, 10, 20, 1));
		NEAR(box.lo.x, -11, 1e-6);
		NEAR(box.hi.x, 11, 1e-6);
		NEAR(box.hi.z, 21, 1e-6);
		NEAR(box.lo.y, -1, 1e-6);
	}

	// random bodies for the property tests
	Body RandomBody(std::mt19937& g, std::uint32_t id, float span)
	{
		std::uniform_real_distribution<float> pos(-span, span), step(-300.0f, 300.0f), rad(0.0f, 40.0f), unit(0.0f, 1.0f);
		const float                           kind = unit(g);
		const Vec3                            p{ pos(g), pos(g), pos(g) * 0.2f };
		if (kind < 0.75f) {
			// every draw in its own statement: a function's arguments are evaluated in no fixed order, and each compiler
			// (g++, clang, MSVC) must test the same worlds
			const Vec3  move{ step(g), step(g), step(g) * 0.3f };
			const float r = rad(g), strength = 1.0f + unit(g) * 100.0f;
			Body        b = Sphere(id, p, p + move, r, static_cast<Element>(id % kElements), strength,
				1 + id % 7);
			b.immune = unit(g) < 0.1f;  // a cone
			return b;
		}
		if (kind < 0.9f) {
			const Vec3  reach{ step(g) * 3, step(g) * 3, step(g) };
			const float r = rad(g) * 0.2f;
			Body        b = Beam(id, p, p + reach, r);
			b.shooter = 1 + id % 7;
			b.lockable = id % 2 == 0;  // from the id, not a draw: every world stays what it was
			b.bolt = id % 3 == 0;      // the same: a one-shot bolt (BoltsMeet)
			return b;
		}
		const float angle = unit(g) * 2 * kPi;
		const float w = 20 + rad(g) * 5, h = 50 + rad(g) * 5, r = rad(g) * 0.1f;
		Body        b = Wall(id, p, { std::cos(angle), std::sin(angle), 0 }, w, h, r);
		b.shooter = 1 + id % 7;
		return b;
	}

	void TestFindContactsAgainstBruteForce()
	{
		std::mt19937 g(12345);
		std::size_t  total = 0;
		for (int round = 0; round < 300; ++round) {
			const std::size_t n = 2 + static_cast<std::size_t>(round % 60);
			std::vector<Body> bodies;
			for (std::size_t i = 0; i < n; ++i) {
				bodies.push_back(RandomBody(g, static_cast<std::uint32_t>(i), 200.0f + static_cast<float>(round % 5) * 300.0f));
			}
			if (round % 7 == 0) {
				bodies[0].from.x = NAN;  // an invalid body is skipped, not tested
			}
			const auto           may = [&](std::size_t i, std::size_t j) { return bodies[i].shooter != bodies[j].shooter; };
			std::vector<Contact> fast;
			FindContacts(bodies, may, 100000, fast);
			std::set<std::pair<std::size_t, std::size_t>> slow;
			for (std::size_t i = 0; i < n; ++i) {
				for (std::size_t j = i + 1; j < n; ++j) {
					Touch      t;
					const bool bothImmune = bodies[i].immune && bodies[j].immune && !(bodies[i].lockable && bodies[j].lockable) &&
					                        !(bodies[i].bolt && bodies[j].bolt);
					if (Valid(bodies[i]) && Valid(bodies[j]) && !bothImmune && may(i, j) && FirstTouch(bodies[i], bodies[j], t)) {
						slow.insert({ i, j });
					}
				}
			}
			std::set<std::pair<std::size_t, std::size_t>> got;
			for (std::size_t k = 0; k < fast.size(); ++k) {
				got.insert({ fast[k].a, fast[k].b });
				CHECK(fast[k].a < fast[k].b);
				if (k) {
					CHECK(fast[k - 1].touch.t <= fast[k].touch.t);
				}
				CHECK(fast[k].touch.t >= 0.0f && fast[k].touch.t <= 1.0f);
				CHECK(Finite(fast[k].touch.point));
			}
			CHECK(got == slow);
			CHECK(got.size() == fast.size());  // no pair twice
			total += fast.size();
		}
		CHECK(total > 200);  // the random worlds really did collide
		std::printf("  brute force agreed on %zu contacts in 300 random worlds\n", total);
	}

	void TestFindContactsRules()
	{
		std::vector<Body> bodies{
			Sphere(1, { 0, -100, 0 }, { 0, 100, 0 }, 5, Element::kFire, 10, 1),
			Sphere(2, { 0, 100, 0 }, { 0, -100, 0 }, 5, Element::kFrost, 10, 2),
			Sphere(3, { 0, -100, 0 }, { 0, 100, 0 }, 5, Element::kFire, 10, 1),  // same shooter as 1, same path
		};
		std::size_t          asked = 0;
		std::vector<Contact> out;
		FindContacts(bodies, [&](std::size_t i, std::size_t j) { ++asked; return bodies[i].shooter != bodies[j].shooter; }, 10, out);
		CHECK(out.size() == 2);
		CHECK(asked == 3);
		FindContacts(bodies, [&](std::size_t, std::size_t) { return true; }, 1, out);
		CHECK(out.size() == 1);
		NEAR(out[0].touch.t, 0.0f, 0);  // 1 and 3 overlap from the start: the earliest is kept
		FindContacts(bodies, [&](std::size_t, std::size_t) { return true; }, 0, out);
		CHECK(out.empty());
		FindContacts(std::span<const Body>{}, [&](std::size_t, std::size_t) { return true; }, 10, out);
		CHECK(out.empty());
		// two immune bodies are never offered
		std::vector<Body> walls{ Wall(1, { 0, 0, 0 }, { 1, 0, 0 }, 50, 50), Beam(2, { 0, -50, 10 }, { 0, 50, 10 }, 2) };
		asked = 0;
		FindContacts(walls, [&](std::size_t, std::size_t) { ++asked; return true; }, 10, out);
		CHECK(asked == 0 && out.empty());
		// two held beams crossing are paired only when both may lock; a beam and a wall never are
		for (int both = 0; both < 4; ++both) {
			std::vector<Body> beams{ Beam(1, { -50, 0, 0 }, { 50, 0, 0 }, 2), Beam(2, { 0, -50, 1 }, { 0, 50, 1 }, 2) };
			beams[0].lockable = (both & 1) != 0;
			beams[1].lockable = (both & 2) != 0;
			FindContacts(beams, [&](std::size_t, std::size_t) { return true; }, 10, out);
			CHECK(out.size() == (both == 3 ? 1u : 0u));
		}
		// two one-shot bolts crossing are paired only when both are marked bolts (BoltsMeet), though both are immune
		for (int both = 0; both < 4; ++both) {
			std::vector<Body> bolts{ Beam(1, { -50, 0, 0 }, { 50, 0, 0 }, 2), Beam(2, { 0, -50, 1 }, { 0, 50, 1 }, 2) };
			bolts[0].bolt = (both & 1) != 0;
			bolts[1].bolt = (both & 2) != 0;
			FindContacts(bolts, [&](std::size_t, std::size_t) { return true; }, 10, out);
			CHECK(out.size() == (both == 3 ? 1u : 0u));
		}
		std::vector<Body> beamWall{ Beam(1, { -50, 0, 10 }, { 50, 0, 10 }, 2), Wall(2, { 0, 0, 0 }, { 0, 1, 0 }, 50, 50) };
		beamWall[0].lockable = beamWall[1].lockable = true;
		FindContacts(beamWall, [&](std::size_t, std::size_t) { return true; }, 10, out);
		CHECK(out.empty());
		// far apart in x: the sweep stops early and never asks
		std::vector<Body> apart{ Sphere(1, { 0, 0, 0 }, { 1, 0, 0 }, 5), Sphere(2, { 5000, 0, 0 }, { 5001, 0, 0 }, 5) };
		asked = 0;
		FindContacts(apart, [&](std::size_t, std::size_t) { ++asked; return true; }, 10, out);
		CHECK(asked == 0);
		// same x band, apart in y
		std::vector<Body> apartY{ Sphere(1, { 0, 0, 0 }, { 1, 0, 0 }, 5), Sphere(2, { 0, 5000, 0 }, { 1, 5000, 0 }, 5) };
		asked = 0;
		FindContacts(apartY, [&](std::size_t, std::size_t) { ++asked; return true; }, 10, out);
		CHECK(asked == 0);
	}

	void TestTable()
	{
		const Table t = DefaultTable();
		const auto  at = [&](Element a, Element b) { return t[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)]; };
		CHECK(at(Element::kFire, Element::kFrost) == Action::kAnnihilate);
		CHECK(at(Element::kFrost, Element::kFire) == Action::kAnnihilate);
		CHECK(at(Element::kForce, Element::kPhysical) == Action::kWins);
		CHECK(at(Element::kPhysical, Element::kForce) == Action::kLoses);
		CHECK(at(Element::kForce, Element::kForce) == Action::kPass);
		CHECK(at(Element::kFire, Element::kFire) == Action::kClash);
		CHECK(at(Element::kShock, Element::kPhysical) == Action::kClash);
		for (std::size_t i = 0; i < kElements; ++i) {
			for (std::size_t j = 0; j < kElements; ++j) {
				CHECK(t[i][j] == Mirror(t[j][i]));  // the table is always consistent with its mirror
			}
		}
		Table u = DefaultTable();
		SetReaction(u, Element::kFire, Element::kFire, Action::kWins);
		CHECK(u[0][0] == Action::kClash);
		SetReaction(u, static_cast<Element>(50), Element::kFire, Action::kPass);  // ignored, no crash
		CHECK(Mirror(Action::kWins) == Action::kLoses && Mirror(Action::kLoses) == Action::kWins && Mirror(Action::kClash) == Action::kClash);
		Element e{};
		CHECK(ParseElement(" frost ", e) && e == Element::kFrost);
		CHECK(ParseElement("PHYSICAL", e) && e == Element::kPhysical);
		CHECK(!ParseElement("Frosty", e));
		CHECK(!ParseElement("", e));
		Action a{};
		CHECK(ParseAction("beats", a) && a == Action::kWins);
		CHECK(ParseAction("Cancel", a) && a == Action::kAnnihilate);
		CHECK(!ParseAction("Explode", a));
		for (std::size_t i = 0; i < kElements; ++i) {
			Element back{};
			CHECK(ParseElement(ElementName(static_cast<Element>(i)), back) && back == static_cast<Element>(i));
		}
		CHECK(ElementName(static_cast<Element>(99)) == "?");
	}

	void TestResolve()
	{
		const Table t = DefaultTable();
		using enum Element;
		// the contest
		auto o = Resolve(t, 2.0f, true, kFire, 100, false, kShock, 40, false);
		CHECK(o.happened && !o.killA && o.killB);
		NEAR(o.strengthA, 60.0f, 1e-4);
		o = Resolve(t, 2.0f, false, kFire, 100, false, kShock, 40, false);
		CHECK(!o.killA && o.killB);
		NEAR(o.strengthA, 100.0f, 0);
		o = Resolve(t, 2.0f, true, kFire, 30, false, kShock, 40, false);
		CHECK(o.killA && o.killB);
		o = Resolve(t, 2.0f, true, kShock, 40, false, kFire, 100, false);
		CHECK(o.killA && !o.killB);
		NEAR(o.strengthB, 60.0f, 1e-4);
		// opposites cancel whatever their size
		o = Resolve(t, 2.0f, true, kFire, 1000, false, kFrost, 1, false);
		CHECK(o.killA && o.killB);
		// force wins
		o = Resolve(t, 2.0f, true, kForce, 1, true, kFire, 1000, false);
		CHECK(!o.killA && o.killB);
		o = Resolve(t, 2.0f, true, kFire, 1000, false, kForce, 1, true);
		CHECK(o.killA && !o.killB);
		o = Resolve(t, 2.0f, true, kForce, 1, true, kForce, 1, false);
		CHECK(!o.happened);
		// immune sides
		o = Resolve(t, 2.0f, true, kShock, 10, true, kFire, 1000, false);  // a weak bolt against a big fireball
		CHECK(!o.killA && !o.killB && o.happened);
		NEAR(o.strengthB, 990.0f, 1e-3);
		NEAR(o.strengthA, 10.0f, 0);                                     // an immune side is never weakened
		o = Resolve(t, 2.0f, true, kShock, 10, true, kFire, 12, false);  // even: only the mortal one goes
		CHECK(!o.killA && o.killB);
		o = Resolve(t, 2.0f, true, kShock, 10, true, kFire, 12, true);
		CHECK(!o.happened);
		o = Resolve(t, 2.0f, true, kFrost, 10, true, kFire, 1000, false);  // a frost wall stops any fire
		CHECK(!o.killA && o.killB);
		// ratio 1: equals both go; exactly beaten goes to zero and dies
		o = Resolve(t, 1.0f, true, kFire, 50, false, kShock, 50, false);
		CHECK(o.killA && o.killB);
		o = Resolve(t, 1.0f, true, kFire, 51, false, kShock, 50, false);
		CHECK(!o.killA && o.killB);
		NEAR(o.strengthA, 1.0f, 1e-4);
		// bad input is survived
		o = Resolve(t, NAN, true, kFire, NAN, false, kShock, 50, false);
		CHECK(o.killA);                                                    // a NaN strength counts as none
		o = Resolve(t, 0.5f, true, kFire, 100, false, kShock, 60, false);  // a ratio below 1 is taken as 1
		CHECK(!o.killA && o.killB);
		o = Resolve(t, 2.0f, true, static_cast<Element>(42), 1, false, kFire, 1, false);
		CHECK(!o.happened);
		o = Resolve(t, 2.0f, true, kFire, 0, false, kShock, 0, false);
		CHECK(o.killA && o.killB);
		// pass
		Table p = DefaultTable();
		SetReaction(p, kFire, kShock, Action::kPass);
		o = Resolve(p, 2.0f, true, kFire, 100, false, kShock, 1, false);
		CHECK(!o.happened && !o.killA && !o.killB);
		// symmetry: swapping the sides swaps the outcome, for any table and strengths
		std::mt19937                          g(7);
		std::uniform_real_distribution<float> s(0.0f, 200.0f);
		std::uniform_int_distribution<int>    act(0, 4), el(0, static_cast<int>(kElements) - 1), coin(0, 3);
		for (int i = 0; i < 20000; ++i) {
			Table r = DefaultTable();
			for (int k = 0; k < 6; ++k) {
				const auto e1 = static_cast<Element>(el(g)), e2 = static_cast<Element>(el(g));
				const auto a = static_cast<Action>(act(g));
				SetReaction(r, e1, e2, a);
			}
			const Element ea = static_cast<Element>(el(g)), eb = static_cast<Element>(el(g));
			const float   sa = s(g), sb = s(g), ratio = 1.0f + s(g) / 50.0f;
			const bool    ia = coin(g) == 0, ib = coin(g) == 0, weaken = coin(g) < 2;
			const auto    x = Resolve(r, ratio, weaken, ea, sa, ia, eb, sb, ib);
			const auto    y = Resolve(r, ratio, weaken, eb, sb, ib, ea, sa, ia);
			CHECK(x.killA == y.killB && x.killB == y.killA && x.happened == y.happened);
			NEAR(x.strengthA, y.strengthB, 1e-4);
			if (ia) {
				CHECK(!x.killA && x.strengthA == sa);
			}
			CHECK(x.strengthA >= 0.0f && x.strengthB >= 0.0f && x.strengthA <= sa && x.strengthB <= sb);
			CHECK(!(x.killA && x.strengthA != 0.0f));
		}
	}

	void TestStrength()
	{
		Tuning t;
		NEAR(BaseStrength(Kind::kSpell, 41, 1, 0, t), 41.0f, 1e-4);
		NEAR(BaseStrength(Kind::kSpell, 41, 2.2f, 0, t), 90.2f, 1e-3);  // dual cast
		NEAR(BaseStrength(Kind::kSpell, 1, 1, 0, t), 5.0f, 1e-4);       // the floor
		NEAR(BaseStrength(Kind::kStream, 20, 1, 0, t), 3.0f, 1e-4);
		NEAR(BaseStrength(Kind::kArrow, 0, 1, 18, t), 36.0f, 1e-4);
		NEAR(BaseStrength(Kind::kArrow, 0, 1, 0, t), 2.0f, 1e-4);  // at least one point of damage
		NEAR(BaseStrength(Kind::kVoice, 0, 1, 0, t), 200.0f, 1e-4);
		NEAR(BaseStrength(Kind::kCone, 50, 1, 0, t), 50.0f, 1e-4);
		// nonsense in, something sane out
		for (const float bad : { NAN, INFINITY, -INFINITY, -5.0f, 0.0f, 1e30f }) {
			for (const Kind k : { Kind::kSpell, Kind::kStream, Kind::kArrow, Kind::kVoice, Kind::kBeam, Kind::kBarrier, Kind::kCone }) {
				const float s = BaseStrength(k, bad, bad, bad, t);
				CHECK(std::isfinite(s) && s >= 0.01f && s <= 1e9f);
			}
		}
		Tuning weird{ NAN, -1, INFINITY, -3 };
		CHECK(std::isfinite(BaseStrength(Kind::kStream, 10, 1, 0, weird)));
		CHECK(std::isfinite(BaseStrength(Kind::kVoice, 10, 1, 0, weird)));
		NEAR(ConeRadius(10, 100, 0.5f), 60.0f, 1e-4);
		NEAR(ConeRadius(-10, -100, 0.5f), 0.0f, 0);
		NEAR(ConeRadius(NAN, 100, NAN), 0.0f, 0);
		NEAR(ConeRadius(10, 1e20f, 10), 1.0e4f, 0);
	}

	void TestClassify()
	{
		std::array<std::vector<std::string>, kElements> kw;
		kw[0] = { "MagicDamageFire" };
		kw[1] = { "MagicDamageFrost" };
		kw[2] = { "MagicDamageShock", "MyShock" };
		const std::string_view fireFrost[]{ "MagicDamageFrost", "magicdamagefire" };
		CHECK(Classify({ fireFrost, "", false, false }, kw) == Element::kFire);  // the first element's list wins
		const std::string_view mine[]{ "Unrelated", "MYSHOCK" };
		CHECK(Classify({ mine, "ResistFire", false, false }, kw) == Element::kShock);  // keywords before the resist
		CHECK(Classify({ {}, "ResistFrost", false, false }, kw) == Element::kFrost);
		CHECK(Classify({ {}, "ElectricResist", false, false }, kw) == Element::kShock);
		CHECK(Classify({ {}, "PoisonResist", false, false }, kw) == Element::kPoison);
		CHECK(Classify({ {}, "", true, true }, kw) == Element::kForce);
		CHECK(Classify({ {}, "", true, false }, kw) == Element::kArcane);
		CHECK(Classify({ {}, "", false, true }, kw) == Element::kArcane);
		CHECK(Classify({ {}, "MagicResist", false, false }, kw) == Element::kArcane);
		std::array<std::vector<std::string>, kElements> none;
		CHECK(Classify({ fireFrost, "", false, false }, none) == Element::kArcane);
	}

	void TestLimiter()
	{
		Limiter l;
		l.BeginFrame(10.0, 2, 0.5f);
		CHECK(l.Allow(1));
		CHECK(!l.Allow(1));  // cooldown
		CHECK(l.Allow(2));
		CHECK(!l.Allow(3));  // the frame's cap
		l.BeginFrame(10.2, 2, 0.5f);
		CHECK(!l.Allow(1));
		CHECK(l.Allow(3));
		l.BeginFrame(10.6, 2, 0.5f);
		CHECK(l.Allow(1));
		l.BeginFrame(1.0, 2, 0.5f);  // a save was loaded: the clock went back
		CHECK(l.Allow(1));
		l.BeginFrame(2.0, 0, 0.0f);
		CHECK(!l.Allow(99));
		l.BeginFrame(3.0, -5, NAN);
		CHECK(!l.Allow(99));
		for (std::uint64_t k = 0; k < 1000; ++k) {
			l.BeginFrame(4.0 + static_cast<double>(k) * 0.001, 1, 0.1f);
			CHECK(l.Allow(k));
		}
		l.BeginFrame(100.0, 1, 0.1f);  // long after: the old keys are forgotten
		CHECK(l.Remembered() == 0);
	}

	void TestFormRef()
	{
		FormRef r;
		CHECK(ParseFormRef("Skyrim.esm|0x012EB7", r) && r.file == "Skyrim.esm" && r.id == 0x12EB7 && r.editorID.empty());
		CHECK(ParseFormRef(" My Mod.esp | 0XABC ", r) && r.file == "My Mod.esp" && r.id == 0xABC);
		CHECK(ParseFormRef("Dawnguard.esm~00F1A2", r) && r.file == "Dawnguard.esm" && r.id == 0xF1A2);
		CHECK(ParseFormRef("Update.esm|FFFFFFFF", r) && r.id == 0xFFFFFFFF);
		CHECK(ParseFormRef("MyProjectile_01", r) && r.file.empty() && r.editorID == "MyProjectile_01");
		for (const char* bad : { "", "   ", "|0x1", "Skyrim.esm|", "Skyrim.esm|0x", "Skyrim.esm|0xZZ", "Skyrim.esm|0x123456789", "a|b|c",
				 "has space", "Skyrim.esm|-1", "Skyrim.esm|+1", "Skyrim.esm|12 34", "Edit-ID", "a~b|1" }) {
			CHECK(!ParseFormRef(bad, r));
		}
	}

	void TestIni()
	{
		{
			Config                   c;
			std::vector<std::string> w;
			ParseIni(
				"\xEF\xBB\xBF; a comment\r\n[General]\r\nEnabled = off\r\nWho=1 ; inline\r\nMaxDistance=+7000\r\n"
				"[hits]\rRadiusBonus=20\rMinRadius=-5\n[Clash]\nOverpowerRatio=3.5\nWeakenSurvivor=no\nShoutStrength=1e3\n"
				"[Effects]\nMaxExplosionsPerFrame=2.6\nExplosionCooldown=nan\n[Debug]\nLog=yes\n",
				c, w);
			CHECK(!c.enabled);
			CHECK(c.who == 1);
			NEAR(c.maxDistance, 7000.0f, 0);
			NEAR(c.radiusBonus, 20.0f, 0);
			NEAR(c.minRadius, 0.0f, 0);  // clamped
			NEAR(c.overpowerRatio, 3.5f, 0);
			CHECK(!c.weakenSurvivor);
			NEAR(c.tuning.shoutStrength, 1000.0f, 0);
			CHECK(c.maxExplosionsPerFrame == 3);  // an int is rounded
			NEAR(c.explosionCooldown, 0.12f, 0);  // nan refused, the default kept
			CHECK(c.debugLog);
			CHECK(w.size() == 2);  // MinRadius clamped, nan refused
		}
		{
			Config                   c;
			std::vector<std::string> w;
			ParseIni("Enabled=0\n[Nope]\nx=1\ny=2\n[General]\nNoSuchThing=1\nWho\n=5\nWho=abc\nWho=12\n[General\nEnabled=0\n", c, w);
			CHECK(c.enabled);   // the first line had no section; the last is under a broken one
			CHECK(c.who == 1);  // 12 clamped to 1
			CHECK(w.size() == 8);
			for (const auto& s : w) {
				CHECK(s.starts_with("line "));
			}
		}
		{
			Config                   c;
			std::vector<std::string> w;
			ParseIni("[Elements]\nFire=A, B ,,b\nFrost=+X\nFrost=+x, Y\nShock=\nWater=Z\n", c, w);
			CHECK((c.keywords[0] == std::vector<std::string>{ "A", "B" }));                      // replaced, empties and case-duplicates dropped
			CHECK((c.keywords[1] == std::vector<std::string>{ "MagicDamageFrost", "X", "Y" }));  // appended
			CHECK(c.keywords[2].empty());
			CHECK(w.size() == 1);
		}
		{
			Config                   c;
			std::vector<std::string> w;
			ParseIni(
				"[Reactions]\nFire.Shock=Pass\nArcane vs Physical=Wins\n*.Poison=Annihilate\nPhysical.*=Loses\nFire.Nope=Pass\n"
				"Fire=Pass\nFire.Frost=Boom\nA.B.C=Pass\n",
				c, w);
			const auto at = [&](Element a, Element b) { return c.reactions[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)]; };
			CHECK(at(Element::kFire, Element::kShock) == Action::kPass && at(Element::kShock, Element::kFire) == Action::kPass);
			CHECK(at(Element::kPoison, Element::kFire) == Action::kAnnihilate);
			CHECK(at(Element::kPhysical, Element::kArcane) == Action::kLoses && at(Element::kArcane, Element::kPhysical) == Action::kWins);
			CHECK(at(Element::kPhysical, Element::kPhysical) == Action::kClash);  // a wildcard never sets an element against itself to Wins/Loses
			CHECK(at(Element::kPhysical, Element::kPoison) == Action::kLoses);    // later lines win
			CHECK(at(Element::kFire, Element::kFrost) == Action::kAnnihilate);
			CHECK(w.size() == 4);
			for (std::size_t i = 0; i < kElements; ++i) {
				for (std::size_t j = 0; j < kElements; ++j) {
					CHECK(c.reactions[i][j] == Mirror(c.reactions[j][i]));
				}
			}
		}
		{
			Config                   c;
			std::vector<std::string> w;
			ParseIni("[Exclude]\nForms=Skyrim.esm|0x10F7ED, Bad Thing, MyProj\nMore=Skyrim.esm|10F7ED,Dawnguard.esm~1\n", c, w);
			CHECK(c.exclude.size() == 3);  // the duplicate is kept once
			CHECK(w.size() == 1);
		}
		{
			// a flood of bad lines is capped in the warnings
			std::string text = "[General]\n";
			for (int i = 0; i < 1000; ++i) {
				text += "Bad" + std::to_string(i) + "=1\n";
			}
			Config                   c;
			std::vector<std::string> w;
			ParseIni(text, c, w);
			CHECK(w.size() == 200);
		}
		// what the menu writes reads back exactly
		std::mt19937                          g(99);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		for (int round = 0; round < 500; ++round) {
			Config                   c;
			std::vector<std::string> w;
			c.enabled = u(g) < 0.5f;
			c.who = u(g) < 0.5f ? 1 : 0;
			c.ignoreAllies = u(g) < 0.5f;
			c.maxDistance = 500 + u(g) * 29500;
			c.radiusScale = 0.1f + u(g) * 9.9f;
			c.radiusBonus = u(g) * 200;
			c.minRadius = u(g) * 100;
			c.maxRadius = 1 + u(g) * 1999;
			c.barrierHeight = 10 + u(g) * 990;
			c.overpowerRatio = 1 + u(g) * 99;
			c.weakenSurvivor = u(g) < 0.5f;
			c.weakenDamage = u(g) < 0.5f;
			c.tuning.streamShare = u(g) * 10;
			c.tuning.arrowScale = u(g) * 100;
			c.tuning.shoutStrength = u(g) * 100000;
			c.tuning.minSpellStrength = u(g) * 1000;
			c.explosions = u(g) < 0.5f;
			c.safeExplosionsOnly = u(g) < 0.5f;
			c.standInBursts = u(g) < 0.5f;
			c.boltsMeet = u(g) < 0.5f;
			c.boltLinger = u(g);
			c.burstScale = 0.5f + u(g) * 2.5f;
			c.maxExplosionsPerFrame = static_cast<int>(u(g) * 32);
			c.explosionCooldown = u(g) * 5;
			c.maxContactsPerFrame = 1 + static_cast<int>(u(g) * 511);
			c.skillXP = u(g) * 1000;
			c.modEvents = u(g) < 0.5f;
			c.debugLog = u(g) < 0.5f;
			auto& k = c.struggle;
			k.enabled = u(g) < 0.5f;
			k.sprays = u(g) < 0.5f;
			k.beams = u(g) < 0.5f;
			k.breath = u(g) < 0.5f;
			k.opposites = u(g) < 0.5f;
			k.beamsStop = u(g) < 0.5f;
			k.betweenOthers = u(g) < 0.5f;
			k.creatures = u(g) < 0.5f;
			k.chance = u(g) * 100;
			k.dragonChance = u(g) * 100;
			k.pushTime = 1 + u(g) * 29;
			k.maxTime = u(g) * 60;
			k.skillAlwaysWins = u(g) < 0.5f;
			k.skillSource = static_cast<int>(u(g) * 3);
			k.skillWeight = u(g) * 3;
			k.levelWeight = u(g) * 3;
			k.spellWeight = u(g) * 2;
			k.magickaWeight = u(g) * 2;
			k.dualBonus = 1 + u(g) * 2;
			k.breathStrength = 1 + u(g) * 499;
			k.wordBonus = u(g);
			k.enemyPower = 0.25f + u(g) * 3.75f;
			k.dragonPower = 0.25f + u(g) * 3.75f;
			k.magickaPressure = u(g) * 3;
			k.surge = u(g) < 0.5f;
			k.surgePower = 1 + u(g) * 2;
			k.breakCast = u(g) < 0.5f;
			k.breakTime = u(g) * 5;
			k.stagger = u(g) < 0.5f;
			k.staggerPlayer = u(g) < 0.5f;
			k.staggerStrength = u(g);
			k.overwhelmDamage = u(g) * 5;
			k.finishers = u(g) < 0.5f;
			k.finisherForce = u(g) * 20;
			k.intimidate = u(g) < 0.5f;
			k.xp = u(g) * 200;
			k.messages = u(g) < 0.5f;
			k.lockBursts = u(g) < 0.5f;
			k.cameraShake = u(g);
			k.bar = u(g) < 0.5f;
			k.barHeight = u(g) * 100;
			k.barScale = 0.5f + u(g) * 1.5f;
			k.barOpacity = 0.1f + u(g) * 0.9f;
			k.barNames = u(g) < 0.5f;
			k.barSkills = u(g) < 0.5f;
			Config back;
			ParseIni(WriteSettings(c), back, w);
			CHECK(w.empty());
			const auto& kb = back.struggle;
			CHECK(kb.enabled == k.enabled && kb.sprays == k.sprays && kb.beams == k.beams && kb.breath == k.breath && kb.opposites == k.opposites &&
				  kb.beamsStop == k.beamsStop && kb.betweenOthers == k.betweenOthers && kb.creatures == k.creatures && kb.chance == k.chance &&
				  kb.dragonChance == k.dragonChance && kb.pushTime == k.pushTime && kb.maxTime == k.maxTime);
			CHECK(kb.skillAlwaysWins == k.skillAlwaysWins && kb.skillSource == k.skillSource && kb.skillWeight == k.skillWeight &&
				  kb.levelWeight == k.levelWeight && kb.spellWeight == k.spellWeight && kb.magickaWeight == k.magickaWeight &&
				  kb.dualBonus == k.dualBonus && kb.breathStrength == k.breathStrength && kb.wordBonus == k.wordBonus &&
				  kb.enemyPower == k.enemyPower && kb.dragonPower == k.dragonPower && kb.magickaPressure == k.magickaPressure &&
				  kb.surge == k.surge && kb.surgePower == k.surgePower);
			CHECK(kb.breakCast == k.breakCast && kb.breakTime == k.breakTime && kb.stagger == k.stagger && kb.staggerPlayer == k.staggerPlayer &&
				  kb.staggerStrength == k.staggerStrength && kb.overwhelmDamage == k.overwhelmDamage && kb.finishers == k.finishers &&
				  kb.finisherForce == k.finisherForce && kb.intimidate == k.intimidate && kb.xp == k.xp && kb.messages == k.messages);
			CHECK(kb.lockBursts == k.lockBursts && kb.cameraShake == k.cameraShake && kb.bar == k.bar && kb.barHeight == k.barHeight &&
				  kb.barScale == k.barScale && kb.barOpacity == k.barOpacity && kb.barNames == k.barNames && kb.barSkills == k.barSkills);
			CHECK(k.skillSource >= 0 && k.skillSource <= 2);
			CHECK(back.enabled == c.enabled && back.who == c.who && back.ignoreAllies == c.ignoreAllies && back.maxDistance == c.maxDistance &&
				  back.radiusScale == c.radiusScale && back.radiusBonus == c.radiusBonus && back.minRadius == c.minRadius &&
				  back.maxRadius == c.maxRadius && back.barrierHeight == c.barrierHeight && back.overpowerRatio == c.overpowerRatio &&
				  back.weakenSurvivor == c.weakenSurvivor && back.weakenDamage == c.weakenDamage &&
				  back.tuning.streamShare == c.tuning.streamShare && back.tuning.arrowScale == c.tuning.arrowScale &&
				  back.tuning.shoutStrength == c.tuning.shoutStrength && back.tuning.minSpellStrength == c.tuning.minSpellStrength &&
				  back.explosions == c.explosions && back.safeExplosionsOnly == c.safeExplosionsOnly && back.standInBursts == c.standInBursts &&
				  back.burstScale == c.burstScale && back.boltsMeet == c.boltsMeet && back.boltLinger == c.boltLinger &&
				  back.maxExplosionsPerFrame == c.maxExplosionsPerFrame && back.explosionCooldown == c.explosionCooldown &&
				  back.maxContactsPerFrame == c.maxContactsPerFrame && back.skillXP == c.skillXP && back.modEvents == c.modEvents &&
				  back.debugLog == c.debugLog);
		}
		// the shipped defaults are what a fresh Config holds
		Config                   fresh, reread;
		std::vector<std::string> w;
		ParseIni(WriteSettings(fresh), reread, w);
		CHECK(w.empty());
		CHECK(WriteSettings(fresh) == WriteSettings(reread));
		const Range r = RangeOf("radiusbonus");
		CHECK(r.lo == 0.0f && r.hi == 200.0f);
		const Range none = RangeOf("nothing");
		CHECK(none.lo == 0.0f && none.hi == 0.0f);

		// the sections in their order, and every key once: RangeOf finds a setting by its key alone, whatever the section
		const std::string text = WriteSettings(fresh);
		std::size_t       at = 0;
		for (const char* sec : { "[General]", "[Hits]", "[Clash]", "[Struggle]", "[Power]", "[Aftermath]", "[Show]", "[Effects]", "[Player]",
				 "[Events]", "[Debug]" }) {
			const auto found = text.find(sec, at);
			CHECK(found != std::string::npos);
			at = found == std::string::npos ? at : found;
		}
		std::set<std::string> keys;
		std::size_t           written = 0;
		for (std::size_t start = 0; start < text.size();) {
			const auto        end = text.find('\n', start);
			const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
			start = end == std::string::npos ? text.size() : end + 1;
			const auto eq = line.find('=');
			if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) {
				continue;
			}
			std::string key = line.substr(0, eq);
			const Range kr = RangeOf(key);
			CHECK(kr.hi > kr.lo);  // found, and a real range
			for (auto& ch : key) {
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			keys.insert(key);
			++written;
		}
		CHECK(written == 83);  // 28 before the struggles, 46 of them, 9 of auto lock
		CHECK(keys.size() == written);
		const Range push = RangeOf("PushTime"), scale = RangeOf("barscale"), source = RangeOf("SkillSource");
		CHECK(push.lo == 1.0f && push.hi == 30.0f);
		CHECK(scale.lo == 0.5f && scale.hi == 2.0f);
		CHECK(source.lo == 0.0f && source.hi == 2.0f);
		{
			Config                   c;
			std::vector<std::string> warns;
			ParseIni("[struggle]\nPushTime=99\n", c, warns);
			NEAR(c.struggle.pushTime, 30.0f, 0);
			CHECK(warns.size() == 1);
			warns.clear();
			ParseIni("[Power]\nSkillSource=1.6\nNoSuchPower=2\nskillweight=2.5\n[Show]\nStruggleBar=off\n", c, warns);
			CHECK(c.struggle.skillSource == 2);
			NEAR(c.struggle.skillWeight, 2.5f, 0);
			CHECK(!c.struggle.bar);
			CHECK(warns.size() == 1);  // the unknown key
			warns.clear();
			ParseIni("[Power]\nPushTime=5\n", c, warns);  // a key in the wrong section is unknown there
			CHECK(warns.size() == 1);
			NEAR(c.struggle.pushTime, 30.0f, 0);
		}
	}

	void TestFrameDecisions()
	{
		Config c;  // bonus 12, scale 1, min 4, max 256
		NEAR(Radius(10, c), 22.0f, 1e-5);
		NEAR(Radius(0, c), 12.0f, 1e-5);
		NEAR(Radius(-5, c), 12.0f, 1e-5);
		NEAR(Radius(NAN, c), 12.0f, 1e-5);
		NEAR(Radius(1e9f, c), 256.0f, 0);
		c.radiusBonus = 0;
		NEAR(Radius(1, c), 4.0f, 0);  // the minimum
		c.maxRadius = 2;              // a maximum below the minimum: the minimum wins
		NEAR(Radius(100, c), 4.0f, 0);
		c.radiusScale = NAN;
		c.maxRadius = 256;
		NEAR(Radius(10, c), 10.0f, 1e-5);
		for (int i = 0; i < 64; ++i) {
			const float h = static_cast<float>(i) * 0.3f - 6.0f;
			const Vec3  across = BarrierAcross(h);
			NEAR(Length(across), 1.0f, 1e-5);
			NEAR(across.z, 0.0f, 0);
			NEAR(Dot(across, DirectionFromAngles(0.0f, h)), 0.0f, 1e-5);  // square to the way it faces
			Body wall = Wall(1, { 0, 0, 0 }, across, 10, 10);
			CHECK(Valid(wall));
		}
		CHECK(Valid(Wall(1, { 0, 0, 0 }, BarrierAcross(NAN), 10, 10)));

		// first sight: back along the velocity, never further than flown
		Vec3 p = Backtrack({ 100, 0, 0 }, { 1000, 0, 0 }, { 0, 0, 0 }, 0.05f, 1000.0f);
		NEAR(p.x, 50.0f, 1e-3);
		p = Backtrack({ 100, 0, 0 }, { 1000, 0, 0 }, { 0, 0, 0 }, 0.05f, 20.0f);
		NEAR(p.x, 80.0f, 1e-3);
		p = Backtrack({ 100, 0, 0 }, { 0, 0, 0 }, { 0, 2000, 0 }, 0.05f, NAN);  // the fallback velocity, no limit
		NEAR(p.y, -100.0f, 1e-3);
		p = Backtrack({ 100, 0, 0 }, { NAN, 0, 0 }, { 0, 0, 0 }, 0.05f, 10.0f);
		CHECK(p == (Vec3{ 100, 0, 0 }));
		p = Backtrack({ 100, 0, 0 }, { 1000, 0, 0 }, { 0, 0, 0 }, NAN, 10.0f);
		CHECK(p == (Vec3{ 100, 0, 0 }));
		p = Backtrack({ 100, 0, 0 }, { 1000, 0, 0 }, { 0, 0, 0 }, 0.05f, -1.0f);  // a negative distance is ignored
		NEAR(p.x, 50.0f, 1e-3);
		p = Backtrack({ 100, 0, 0 }, { 0.5f, 0, 0 }, { 0, 0, 0 }, 0.05f, 10.0f);  // barely moving: where it is
		CHECK(p == (Vec3{ 100, 0, 0 }));

		// who may clash: every combination against the rule written out plainly
		for (int bits = 0; bits < 256; ++bits) {
			Shooters s;
			s.a = (bits & 1) ? 7u : 0u;
			s.b = (bits & 2) ? 7u : ((bits & 4) ? 9u : 0u);
			s.playerA = (bits & 8) != 0;
			s.playerB = (bits & 16) != 0;
			s.actorA = (bits & 32) != 0;
			s.actorB = (bits & 64) != 0;
			const bool hostile = (bits & 128) != 0;
			for (int who = 0; who < 2; ++who) {
				for (int allies = 0; allies < 2; ++allies) {
					bool       asked = false;
					const bool got = MayInteract(s, who, allies != 0, [&]() { asked = true; return hostile; });
					bool       want = s.a != s.b && (who == 0 || s.playerA || s.playerB);
					if (want && allies && s.actorA && s.actorB) {
						want = hostile;
					}
					CHECK(got == want);
					CHECK(!asked || (allies && s.actorA && s.actorB));  // hostility is only asked when it matters
				}
			}
		}

		NEAR(DamageScale(100, 40), 0.4f, 1e-6);
		NEAR(DamageScale(100, 1), 0.05f, 0);   // never below a twentieth
		NEAR(DamageScale(100, 150), 1.0f, 0);  // never stronger
		NEAR(DamageScale(0, 5), 1.0f, 0);
		NEAR(DamageScale(NAN, 5), 1.0f, 0);
		NEAR(DamageScale(10, NAN), 1.0f, 0);
	}

	// the rules file the mod ships reads without a warning and gives exactly the built-in defaults
	void TestShippedRules()
	{
		std::string here = __FILE__;  // the package beside the tests, wherever the tests run from
		here = here.substr(0, here.find_last_of("/\\") + 1);
		const std::string rules = here + "../package/SKSE/Plugins/SpellMasteryHeat Clash Rules.ini";
		const char*       path = rules.c_str();
		std::FILE*        f = std::fopen(path, "rb");
		CHECK(f != nullptr);
		if (!f) {
			return;
		}
		std::string text;
		char        buf[4096];
		for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) {
			text.append(buf, n);
		}
		std::fclose(f);
		Config                   c;
		std::vector<std::string> w;
		ParseIni(text, c, w);
		for (const auto& s : w) {
			std::printf("  shipped rules: %s\n", s.c_str());
		}
		CHECK(w.empty());
		CHECK(c.reactions == DefaultTable());
		CHECK(c.keywords == Config{}.keywords);
		CHECK(c.exclude.empty());
	}

	void TestGarbageNeverCrashes()
	{
		std::mt19937                       g(2026);
		std::uniform_int_distribution<int> len(0, 400), byte(0, 255), pick(0, 9);
		const char*                        pieces[]{ "[General]", "[Elements]", "[Reactions]", "[Exclude]", "=", "\n", "\r", ",", "*.", "|0x" };
		for (int i = 0; i < 20000; ++i) {
			std::string s;
			const int   n = len(g);
			for (int k = 0; k < n; ++k) {
				if (pick(g) < 3) {
					s += pieces[pick(g)];
				} else {
					s += static_cast<char>(byte(g));
				}
			}
			Config                   c;
			std::vector<std::string> w;
			ParseIni(s, c, w);
			FormRef r;
			(void)ParseFormRef(s, r);
			CHECK(w.size() <= 200);
		}
	}

	// ------------------------------------------------------------------ spell struggles
	// a caster for the struggle tests: Flames (cost 14) at full magicka unless said
	Caster Mage(float a_skill, float a_level = 30.0f, float a_cost = 14.0f)
	{
		Caster c;
		c.skill = a_skill;
		c.level = a_level;
		c.cost = a_cost;
		return c;
	}

	// a breath: its skill is the shouter's level, it costs BreathStrength and pays no magicka
	Caster Breath(float a_level, float a_mult, const StruggleConfig& k)
	{
		Caster c;
		c.skill = MagicSkill(0.0f, a_level, false, true);
		c.level = a_level;
		c.cost = k.breathStrength;
		c.school = false;
		c.paysMagicka = false;
		c.mult = a_mult;
		return c;
	}

	// A's muzzle at the origin and B's 1000 units along x, both reaching 2000, met halfway: locked at the default chance
	Struggle Locked(const StruggleConfig& k, bool a_breath = false)
	{
		return Begin(1, 2, Action::kClash, { 500, 0, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 2000.0f, 2000.0f, false, a_breath, false, 0, k);
	}

	constexpr Sight kBoth{ true, true, true };

	// both seen, steps of a_dt until something ends it: the seconds it took (a_limit if nothing did)
	double Run(Struggle& s, const Caster& a, const Caster& b, float a_dt, const StruggleConfig& k, StruggleStep& a_last, double a_limit = 120.0)
	{
		double t = 0.0;
		while (t < a_limit) {
			a_last = Advance(s, a, b, kBoth, a_dt, k);
			t += static_cast<double>(a_dt);
			if (a_last.event != StruggleEvent::kNone) {
				break;
			}
		}
		return t;
	}

	LockSide Side(Stream a_stream, bool a_player, bool a_npc, bool a_dragon)
	{
		LockSide s;
		s.stream = a_stream;
		s.actor = s.alive = true;
		s.player = a_player;
		s.npc = a_npc;
		s.dragon = a_dragon;
		return s;
	}

	void TestStreams()
	{
		using enum Stream;
		for (const Kind kind : { Kind::kSpell, Kind::kStream, Kind::kArrow, Kind::kVoice, Kind::kBeam, Kind::kBarrier, Kind::kCone }) {
			for (int bits = 0; bits < 4; ++bits) {
				const bool held = (bits & 1) != 0, voice = (bits & 2) != 0;
				Stream     want = kNone;
				if (kind == Kind::kStream) {
					want = voice ? kBreath : kSpray;
				} else if (kind == Kind::kBeam && held) {
					want = kBeam;
				}
				CHECK(StreamOf(kind, held, voice) == want);
			}
		}
		CHECK(StreamOf(Kind::kBeam, false, false) == kNone);  // a one-shot bolt
		CHECK(StreamOf(Kind::kSpell, true, false) == kNone);  // a missile, even from a held spell
		CHECK(StreamOf(Kind::kCone, true, true) == kNone);
		CHECK(StreamOf(Kind::kBarrier, true, false) == kNone);
		CHECK(StreamOf(Kind::kStream, true, false) == kSpray);
		CHECK(StreamOf(Kind::kStream, false, true) == kBreath);
		for (int bits = 0; bits < 8; ++bits) {
			StruggleConfig k;
			k.sprays = (bits & 1) != 0;
			k.beams = (bits & 2) != 0;
			k.breath = (bits & 4) != 0;
			CHECK(StreamOn(kSpray, k) == k.sprays && StreamOn(kBeam, k) == k.beams && StreamOn(kBreath, k) == k.breath && !StreamOn(kNone, k));
		}
		CHECK(!StreamOn(static_cast<Stream>(9), StruggleConfig{}));
	}

	void TestMayLock()
	{
		// every side (stream kind and six flags), every pair of them, every action and every combination of the switches,
		// against the rule written out plainly
		const auto side = [](int a_bits) {
			LockSide s;
			s.stream = static_cast<Stream>(a_bits & 3);
			s.actor = (a_bits & 4) != 0;
			s.alive = (a_bits & 8) != 0;
			s.player = (a_bits & 16) != 0;
			s.npc = (a_bits & 32) != 0;
			s.dragon = (a_bits & 64) != 0;
			s.busy = (a_bits & 128) != 0;
			return s;
		};
		std::array<LockSide, 256> sides;
		for (int i = 0; i < 256; ++i) {
			sides[static_cast<std::size_t>(i)] = side(i);
		}
		long wrong = 0, lopsided = 0, cases = 0;
		for (int sw = 0; sw < 128; ++sw) {
			StruggleConfig k;
			k.enabled = (sw & 1) != 0;
			k.sprays = (sw & 2) != 0;
			k.beams = (sw & 4) != 0;
			k.breath = (sw & 8) != 0;
			k.opposites = (sw & 16) != 0;
			k.betweenOthers = (sw & 32) != 0;
			k.creatures = (sw & 64) != 0;
			const auto on = [&](const LockSide& s) {
				return (s.stream == Stream::kSpray && k.sprays) || (s.stream == Stream::kBeam && k.beams) || (s.stream == Stream::kBreath && k.breath);
			};
			const auto creature = [](const LockSide& s) { return s.actor && !s.npc && !s.dragon && !s.player; };
			for (const LockSide& a : sides) {
				for (const LockSide& b : sides) {
					for (int act = 0; act < 5; ++act) {
						const auto action = static_cast<Action>(act);
						bool       want = k.enabled && a.actor && b.actor && a.alive && b.alive && !a.busy && !b.busy && on(a) && on(b);
						want = want && (action == Action::kClash || (action == Action::kAnnihilate && k.opposites));
						want = want && (a.player || b.player || k.betweenOthers);
						want = want && (k.creatures || (!creature(a) && !creature(b)));
						const bool got = MayLock(a, b, action, k);
						wrong += got != want ? 1 : 0;
						lopsided += got != MayLock(b, a, action, k) ? 1 : 0;
						++cases;
					}
				}
			}
		}
		CHECK(wrong == 0);
		CHECK(lopsided == 0);
		CHECK(cases == 128L * 256 * 256 * 5);

		const StruggleConfig k;
		const LockSide       you = Side(Stream::kSpray, true, true, false), mage = Side(Stream::kSpray, false, true, false);
		const LockSide       atronach = Side(Stream::kBeam, false, false, false), dragon = Side(Stream::kBreath, false, false, true);
		CHECK(MayLock(you, mage, Action::kClash, k));
		for (const Action a : { Action::kPass, Action::kWins, Action::kLoses }) {
			CHECK(!MayLock(you, mage, a, k));
		}
		CHECK(MayLock(you, mage, Action::kAnnihilate, k));
		StruggleConfig noOpposites;
		noOpposites.opposites = false;
		CHECK(!MayLock(you, mage, Action::kAnnihilate, noOpposites));
		CHECK(MayLock(you, mage, Action::kClash, noOpposites));
		LockSide busy = mage, dead = mage, trap = mage;
		busy.busy = true;
		dead.alive = false;
		trap.actor = false;
		CHECK(!MayLock(you, busy, Action::kClash, k) && !MayLock(dead, you, Action::kClash, k) && !MayLock(you, trap, Action::kClash, k));
		StruggleConfig onlyYours;
		onlyYours.betweenOthers = false;
		CHECK(!MayLock(mage, atronach, Action::kClash, onlyYours) && !MayLock(mage, dragon, Action::kClash, onlyYours));
		CHECK(MayLock(you, mage, Action::kClash, onlyYours) && MayLock(dragon, you, Action::kClash, onlyYours));
		StruggleConfig noCreatures;
		noCreatures.creatures = false;
		CHECK(!MayLock(you, atronach, Action::kClash, noCreatures) && !MayLock(atronach, mage, Action::kClash, noCreatures));
		CHECK(MayLock(you, mage, Action::kClash, noCreatures) && MayLock(you, dragon, Action::kClash, noCreatures) &&
			  MayLock(mage, dragon, Action::kClash, noCreatures));
		CHECK(MayLock(you, atronach, Action::kClash, k));
		const LockSide missile = Side(Stream::kNone, false, true, false);
		CHECK(!MayLock(you, missile, Action::kClash, k));
		StruggleConfig off;
		off.enabled = false;
		CHECK(!MayLock(you, mage, Action::kClash, off));
	}

	void TestFacing()
	{
		const Vec3      a{ 0, 0, 0 }, b{ 1000, 0, 0 };
		constexpr float kGap = 50.0f;  // MinDistance's default
		const auto      aim = [](float a_degrees, bool a_up) {
			const float r = a_degrees * kPi / 180.0f;
			return a_up ? Vec3{ std::cos(r), 0, std::sin(r) } : Vec3{ std::cos(r), std::sin(r), 0 };
		};
		const auto back = [](Vec3 v) { return Vec3{ -v.x, v.y, v.z }; };  // the same angle, aimed from B toward A
		for (const bool up : { false, true }) {
			CHECK(Facing(a, aim(49, up), b, back(aim(0, up)), kGap));
			CHECK(!Facing(a, aim(51, up), b, back(aim(0, up)), kGap));
			CHECK(Facing(a, aim(0, up), b, back(aim(49, up)), kGap));
			CHECK(!Facing(a, aim(0, up), b, back(aim(51, up)), kGap));
			CHECK(!Facing(a, aim(-51, up), b, back(aim(-51, up)), kGap));
		}
		CHECK(!Facing(a, { 1, 0, 0 }, { 49, 0, 0 }, { -1, 0, 0 }, kGap));  // closer than MinDistance
		CHECK(Facing(a, { 1, 0, 0 }, { 50, 0, 0 }, { -1, 0, 0 }, kGap));
		CHECK(!Facing(a, { 1, 0, 0 }, { 149, 0, 0 }, { -1, 0, 0 }, 150.0f));  // MinDistance is the setting's
		CHECK(Facing(a, { 1, 0, 0 }, { 2, 0, 0 }, { -1, 0, 0 }, 0.0f));       // 0: any distance
		CHECK(!Facing(a, { 1, 0, 0 }, { 0.5f, 0, 0 }, { -1, 0, 0 }, 0.0f));   // but not one point
		CHECK(Facing(a, { 1, 0, 0 }, { 60, 0, 0 }, { -1, 0, 0 }, NAN));       // a broken setting counts as 0
		StruggleConfig k;
		CHECK(k.minGap == kGap && KeepGap(k) < kGap && KeepGap(k) > 0.0f);
		CHECK(Facing(a, { 500, 0, 0 }, b, { -0.01f, 0, 0 }, kGap));  // an aim need not be a unit
		CHECK(!Facing(a, { 0, 0, 0 }, b, { -1, 0, 0 }, kGap));       // no aim at all
		CHECK(!Facing(a, { -1, 0, 0 }, b, { -1, 0, 0 }, kGap));      // A turned away
		CHECK(!Facing(a, { NAN, 0, 0 }, b, { -1, 0, 0 }, kGap));
		CHECK(!Facing({ NAN, 0, 0 }, { 1, 0, 0 }, b, { -1, 0, 0 }, kGap));
		CHECK(!Facing(a, { 1, 0, 0 }, { INFINITY, 0, 0 }, { -1, 0, 0 }, kGap));
		CHECK(!Facing(a, { 1, 0, 0 }, b, { -INFINITY, 0, 0 }, kGap));
		std::mt19937                          g(17);
		std::uniform_real_distribution<float> pos(-2000.0f, 2000.0f), dir(-1.0f, 1.0f);
		int                                   facing = 0;
		for (int i = 0; i < 20000; ++i) {
			const Vec3 ma{ pos(g), pos(g), pos(g) * 0.1f }, mb{ pos(g), pos(g), pos(g) * 0.1f };
			// half aimed roughly at each other, half anywhere
			Vec3 aa{ dir(g), dir(g), dir(g) }, ab{ dir(g), dir(g), dir(g) };
			if (i % 2 == 0) {
				const float d = std::max(Length(mb - ma), 1.0f);
				aa = aa * 0.6f + (mb - ma) * (1.0f / d);
				ab = ab * 0.6f + (ma - mb) * (1.0f / d);
			}
			const bool x = Facing(ma, aa, mb, ab, 150.0f);
			CHECK(x == Facing(mb, ab, ma, aa, 150.0f));
			facing += x ? 1 : 0;
		}
		CHECK(facing > 1000);  // the random cases really did face each other now and then
	}

	void TestSkill()
	{
		const std::array<float, 5> s{ 20, 35, 80, 15, 50 };
		NEAR(PickSkill(s, 2, 0), 80.0f, 0);  // Destruction, its own
		NEAR(PickSkill(s, 4, 0), 50.0f, 0);
		NEAR(PickSkill(s, 4, 1), 80.0f, 0);     // the best
		NEAR(PickSkill(s, 4, 2), 40.0f, 1e-5);  // the mean
		NEAR(PickSkill(s, -1, 0), 80.0f, 0);    // no school of its own: the best
		NEAR(PickSkill(s, 5, 0), 80.0f, 0);
		NEAR(PickSkill(s, -7, 0), 80.0f, 0);
		NEAR(PickSkill(s, 0, 7), 20.0f, 0);  // an unknown source is the own school
		const std::array<float, 5> bad{ NAN, INFINITY, -5, 400, 10 };
		NEAR(PickSkill(bad, 0, 0), 0.0f, 0);  // NaN counts as 0
		NEAR(PickSkill(bad, 1, 0), 0.0f, 0);
		NEAR(PickSkill(bad, 2, 0), 0.0f, 0);
		NEAR(PickSkill(bad, 3, 0), 300.0f, 0);
		NEAR(PickSkill(bad, 0, 1), 300.0f, 0);
		NEAR(PickSkill(bad, 0, 2), 62.0f, 1e-4);

		NEAR(MagicSkill(50, 30, false, true), 30.0f, 0);    // a breath: the level
		NEAR(MagicSkill(50, 150, false, true), 100.0f, 0);  // at most 100
		NEAR(MagicSkill(50, 30, true, true), 30.0f, 0);
		NEAR(MagicSkill(10, 40, true, false), 40.0f, 0);  // a creature: at least its level
		NEAR(MagicSkill(60, 40, true, false), 60.0f, 0);
		NEAR(MagicSkill(10, 400, true, false), 100.0f, 0);
		NEAR(MagicSkill(10, 40, false, false), 10.0f, 0);
		NEAR(MagicSkill(500, 1, false, false), 300.0f, 0);
		NEAR(MagicSkill(-5, 10, false, false), 0.0f, 0);
		NEAR(MagicSkill(NAN, NAN, true, false), 1.0f, 0);
		NEAR(MagicSkill(NAN, 20, false, false), 0.0f, 0);
		NEAR(MagicSkill(INFINITY, -INFINITY, false, true), 1.0f, 0);

		StruggleConfig k;
		k.enemyPower = 2;
		k.dragonPower = 3;
		k.wordBonus = 0.5f;
		NEAR(PowerMult(false, false, 0, k), 1.0f, 0);
		NEAR(PowerMult(true, false, 0, k), 2.0f, 0);
		NEAR(PowerMult(false, true, 0, k), 3.0f, 0);
		NEAR(PowerMult(true, true, 0, k), 6.0f, 0);
		NEAR(PowerMult(false, false, 1, k), 1.0f, 0);
		NEAR(PowerMult(false, false, 2, k), 1.5f, 0);
		NEAR(PowerMult(false, false, 3, k), 2.0f, 0);
		NEAR(PowerMult(false, false, 9, k), 2.0f, 0);  // three words at most
		NEAR(PowerMult(false, false, -2, k), 1.0f, 0);
		NEAR(PowerMult(true, true, 3, StruggleConfig{}), 1.5f, 1e-6);
		k.enemyPower = NAN;
		k.dragonPower = 100;
		k.wordBonus = -1;
		NEAR(PowerMult(true, true, 3, k), 4.0f, 0);  // the default, the most, none
	}

	// a garbage number now and then, else one in [lo, hi]
	float Garbage(std::mt19937& g, float a_lo, float a_hi)
	{
		std::uniform_int_distribution<int>    pick(0, 11);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		switch (pick(g)) {
		case 0:
			return NAN;
		case 1:
			return INFINITY;
		case 2:
			return -INFINITY;
		case 3:
			return -1e30f;
		case 4:
			return 1e30f;
		case 5:
			return -u(g) * 1000.0f;
		default:
			return a_lo + u(g) * (a_hi - a_lo);
		}
	}

	Caster RandomCaster(std::mt19937& g, bool a_garbage)
	{
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		const auto                            val = [&](float lo, float hi) { return a_garbage ? Garbage(g, lo, hi) : lo + u(g) * (hi - lo); };
		Caster                                c;
		c.skill = val(0, 300);
		c.level = val(1, 300);
		c.magicka = val(0, 1);
		c.cost = val(1, 500);
		c.mult = val(0.1f, 10);
		c.dual = u(g) < 0.3f;
		c.school = u(g) < 0.8f;
		c.paysMagicka = u(g) < 0.8f;
		return c;
	}

	StruggleConfig RandomWeights(std::mt19937& g, bool a_garbage)
	{
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		const auto                            val = [&](float lo, float hi) { return a_garbage ? Garbage(g, lo, hi) : lo + u(g) * (hi - lo); };
		StruggleConfig                        k;
		k.skillWeight = val(0, 3);
		k.levelWeight = val(0, 3);
		k.spellWeight = val(0, 2);
		k.magickaWeight = val(0, 2);
		k.dualBonus = val(1, 3);
		k.surgePower = val(1, 3);
		k.skillAlwaysWins = u(g) < 0.5f;
		k.surge = u(g) < 0.5f;
		return k;
	}

	void TestLogPower()
	{
		std::mt19937 g(11);
		for (int i = 0; i < 20000; ++i) {
			const float p = LogPower(RandomCaster(g, true), RandomWeights(g, true));
			CHECK(std::isfinite(p) && std::abs(p) < 100.0f);
		}
		// the most there can be, and the least
		Caster top = Mage(300, 300, 1e30f);
		top.dual = true;
		top.mult = 1e30f;
		StruggleConfig heavy;
		heavy.skillWeight = heavy.levelWeight = 3;
		heavy.spellWeight = heavy.magickaWeight = 2;
		heavy.dualBonus = 3;
		CHECK(LogPower(top, heavy) < 100.0f && LogPower(top, heavy) > 60.0f);
		Caster bottom = Mage(0, 1, 0);
		bottom.magicka = 0;
		bottom.mult = 0;
		CHECK(LogPower(bottom, heavy) > -100.0f);

		// a weight at 0 takes its term away
		StruggleConfig z;
		z.skillWeight = 0;
		CHECK(LogPower(Mage(0), z) == LogPower(Mage(300), z));
		z = {};
		z.levelWeight = 0;
		CHECK(LogPower(Mage(50, 1), z) == LogPower(Mage(50, 300), z));
		z = {};
		z.spellWeight = 0;
		CHECK(LogPower(Mage(50, 30, 1), z) == LogPower(Mage(50, 30, 5000), z));
		z = {};
		z.magickaWeight = 0;
		Caster empty = Mage(50), dual = Mage(50);
		empty.magicka = 0;
		CHECK(LogPower(empty, z) == LogPower(Mage(50), z));
		z = {};
		z.dualBonus = 1;
		dual.dual = true;
		CHECK(LogPower(dual, z) == LogPower(Mage(50), z));
		// breath and staffs keep their push whatever the magicka
		Caster staff = empty;
		staff.paysMagicka = false;
		CHECK(LogPower(staff, StruggleConfig{}) == LogPower(Mage(50), StruggleConfig{}));

		// more of anything never pushes less, and pushes more when it counts
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		const auto                            weight = [&](float hi) { return u(g) < 0.2f ? 0.0f : 0.05f + u(g) * (hi - 0.05f); };
		for (int i = 0; i < 5000; ++i) {
			StruggleConfig k;
			k.skillWeight = weight(3);
			k.levelWeight = weight(3);
			k.spellWeight = weight(2);
			k.magickaWeight = weight(2);
			k.dualBonus = u(g) < 0.2f ? 1.0f : 1.05f + u(g) * 1.95f;
			Caster c = Mage(u(g) * 250, 1 + u(g) * 249, 1 + u(g) * 999);
			c.magicka = u(g) * 0.9f;
			c.mult = 0.1f + u(g) * 9.9f;
			const float before = LogPower(c, k);
			Caster      x = c;
			x.skill += 1 + u(g) * 49;
			CHECK(LogPower(x, k) >= before && (k.skillWeight == 0 || LogPower(x, k) > before));
			x = c;
			x.level += 1 + u(g) * 49;
			CHECK(LogPower(x, k) >= before && (k.levelWeight == 0 || LogPower(x, k) > before));
			x = c;
			x.cost *= 1.1f + u(g) * 1.9f;
			CHECK(LogPower(x, k) >= before && (k.spellWeight == 0 || LogPower(x, k) > before));
			x = c;
			x.magicka += 0.05f + u(g) * 0.05f;
			CHECK(LogPower(x, k) >= before && (k.magickaWeight == 0 || LogPower(x, k) > before));
			x = c;
			x.mult *= 1.1f + u(g) * 1.9f;
			CHECK(LogPower(x, k) > before);
			x = c;
			x.dual = true;
			CHECK(LogPower(x, k) >= before && (k.dualBonus == 1 || LogPower(x, k) > before));
		}
	}

	void TestAdvantage()
	{
		std::mt19937                          g(23);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		for (int i = 0; i < 200000; ++i) {
			const bool           garbage = i % 4 == 0;
			const Caster         a = RandomCaster(g, garbage), b = RandomCaster(g, garbage);
			const StruggleConfig k = RandomWeights(g, garbage);
			const bool           sa = u(g) < 0.3f, sb = u(g) < 0.3f;
			const float          x = Advantage(a, b, k, sa, sb), y = Advantage(b, a, k, sb, sa);
			CHECK(std::isfinite(x) && x >= -1.0f && x <= 1.0f);
			NEAR(x, -y, 1e-6);
		}
		const StruggleConfig k;
		CHECK(Advantage(Mage(50), Mage(50), k) == 0.0f);  // equal: exactly even
		CHECK(Advantage(Mage(50), Mage(50), k, true, true) == 0.0f);
		StruggleConfig plain;
		plain.skillAlwaysWins = false;
		NEAR(Advantage(Mage(90), Mage(40), plain), 0.4621f, 1e-4);  // 50 points: tanh(0.5)
		NEAR(Advantage(Mage(80), Mage(40), k), 0.3799f, 1e-4);
		// twice as strong leads by a third, whatever the reason
		Caster twice = Mage(50);
		twice.mult = 2;
		NEAR(Advantage(twice, Mage(50), k), 1.0f / 3.0f, 1e-5);
		NEAR(Advantage(Mage(50), twice, k), -1.0f / 3.0f, 1e-5);
		// a surge pushes harder by SurgePower
		StruggleConfig surge;
		surge.surgePower = 2;
		NEAR(Advantage(Mage(50), Mage(50), surge, true, false), 1.0f / 3.0f, 1e-5);
	}

	// the owner's rule: the higher magic skill always wins
	void TestSkillAlwaysWins()
	{
		std::mt19937                          g(31);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		StruggleConfig                        plain;
		plain.skillAlwaysWins = false;
		for (int i = 0; i < 20000; ++i) {
			StruggleConfig k;
			k.enemyPower = 4;
			k.surge = true;
			k.surgePower = 1 + u(g) * 2;
			k.skillWeight = u(g) * 3;
			k.levelWeight = u(g) * 3;
			k.spellWeight = u(g) * 2;
			k.magickaWeight = u(g) * 2;
			k.dualBonus = 1 + u(g) * 2;
			const float gap = i % 10 == 0 ? 1.0f : 1.0f + u(g) * 99.0f;
			const float high = gap + u(g) * (300.0f - gap);
			Caster      strong = Mage(high, 1 + u(g) * 20, 1), weak = Mage(high - gap, 250 + u(g) * 50, 500);
			strong.magicka = 0;
			weak.dual = true;
			weak.mult = PowerMult(true, u(g) < 0.5f, 0, k);
			const bool surging = u(g) < 0.5f;
			CHECK(Advantage(strong, weak, k, false, surging) >= kMinLead);
			CHECK(Advantage(weak, strong, k, surging, false) <= -kMinLead);
		}
		// less than a point apart, or a breath on either side: a plain contest
		const StruggleConfig k;
		Caster               dual = Mage(50.5f);
		dual.dual = true;
		CHECK(Advantage(Mage(50), dual, k) == Advantage(Mage(50), dual, plain));
		CHECK(Advantage(Mage(50), dual, k) < 0.0f);
		const Caster breath = Breath(10, 1, k);
		CHECK(Advantage(Mage(80), breath, k) == Advantage(Mage(80), breath, plain));
		CHECK(Advantage(breath, Mage(80), k) == Advantage(breath, Mage(80), plain));
		// with the rule off, the less skilled can win
		Caster dual45 = Mage(45);
		dual45.dual = true;
		CHECK(Advantage(dual45, Mage(50), plain) > 0.0f);
		NEAR(Advantage(dual45, Mage(50), k), -kMinLead, 1e-6);
		Caster boosted = Mage(20, 30);
		boosted.mult = 4;
		CHECK(Advantage(boosted, Mage(60), plain) > 0.0f && Advantage(boosted, Mage(60), k) < 0.0f);
	}

	// the worked examples in the design, at the defaults
	void TestWorkedExamples()
	{
		const StruggleConfig k;
		const auto           lead = [&](const Caster& a, const Caster& b) { return Advantage(a, b, k); };
		NEAR(lead(Mage(80), Mage(40)), 0.38f, 0.005);
		NEAR(SecondsToWin(lead(Mage(80), Mage(40)), 1, k), 3.9f, 0.05);
		NEAR(lead(Mage(60), Mage(40)), 0.197f, 0.005);
		NEAR(SecondsToWin(lead(Mage(60), Mage(40)), 1, k), 7.2f, 0.05);
		NEAR(lead(Mage(50), Mage(45)), 0.15f, 1e-6);  // raised from 0.05
		NEAR(SecondsToWin(lead(Mage(50), Mage(45)), 1, k), 9.3f, 0.05);
		Caster dual45 = Mage(45);
		dual45.dual = true;
		NEAR(lead(Mage(50), dual45), 0.15f, 1e-6);  // skill rules
		StruggleConfig plain;
		plain.skillAlwaysWins = false;
		NEAR(SecondsToWin(Advantage(dual45, Mage(50), plain), 1, plain), 9.2f, 0.05);
		Caster half = Mage(60);
		half.magicka = 0.5f;
		NEAR(lead(half, Mage(40)), 0.15f, 1e-6);
		NEAR(SecondsToWin(lead(half, Mage(40)), 1, k), 9.3f, 0.05);
		CHECK(lead(Mage(40), Mage(40)) == 0.0f);
		// a level 40 player at Destruction 80 with Flames against a dragon's breath
		const Caster player = Mage(80, 40);
		const auto   dragon = [&](float a_level) { return Breath(a_level, PowerMult(true, true, 1, k), k); };
		NEAR(SecondsToWin(lead(player, dragon(10)), kBreathPace, k), 1.5f, 0.05);
		NEAR(SecondsToWin(lead(player, dragon(32)), kBreathPace, k), 2.3f, 0.05);
		NEAR(SecondsToWin(lead(player, dragon(40)), kBreathPace, k), 3.1f, 0.05);
		NEAR(lead(player, dragon(50)), 0.11f, 0.01);
		NEAR(lead(player, dragon(62)), -0.08f, 0.01);
		CHECK(lead(player, dragon(75)) < 0.0f);
		NEAR(SecondsToWin(lead(player, dragon(75)), kBreathPace, k), 2.9f, 0.05);
		// a level 30 player's three-word Fire Breath against a Destruction 40, level 20 mage
		const Caster shout = Breath(30, PowerMult(false, false, 3, k), k);
		const Caster mage = Mage(40, 20);
		NEAR(lead(shout, mage), 0.29f, 0.005);
		NEAR(SecondsToWin(lead(shout, mage), kBreathPace, k), 2.7f, 0.05);
	}

	void TestSecondsToWin()
	{
		const StruggleConfig k;
		NEAR(SecondsToWin(0.0f, 1, k), 1e6f, 0);
		NEAR(SecondsToWin(NAN, 1, k), 1e6f, 0);
		NEAR(SecondsToWin(0.5f, 0, k), 1e6f, 0);
		NEAR(SecondsToWin(0.5f, -1, k), 1e6f, 0);
		NEAR(SecondsToWin(1e-9f, 1, k), 1e6f, 0);            // capped
		NEAR(SecondsToWin(-1.0f / 3.0f, 1, k), 4.4f, 1e-5);  // either way round: twice as strong takes PushTime
		NEAR(SecondsToWin(5.0f, 1, k), SecondsToWin(1.0f, 1, k), 0);
		CHECK(std::isfinite(SecondsToWin(1, INFINITY, k)) && std::isfinite(SecondsToWin(1, NAN, k)));
		NEAR(PushRate(k), 0.75f, 1e-7);
		// the simulated struggle ends when SecondsToWin says, within two frames, at any frame rate
		StruggleConfig open;
		open.maxTime = 0;
		const std::pair<float, float>         pairs[]{ { 80.0f, 40.0f }, { 60.0f, 40.0f }, { 50.0f, 45.0f }, { 100.0f, 20.0f }, { 300.0f, 0.0f }, { 45.0f, 50.0f } };
		std::mt19937                          g(3);
		std::uniform_real_distribution<float> jitter(0.5f, 1.5f);
		for (const float dt : { 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f }) {
			for (const auto& [sa, sb] : pairs) {
				for (const bool breath : { false, true }) {
					const Caster a = Mage(sa), b = Mage(sb);
					const float  lead = Advantage(a, b, open);
					const float  want = SecondsToWin(lead, breath ? kBreathPace : 1.0f, open);
					Struggle     s = Locked(open, breath);
					StruggleStep last;
					NEAR(Run(s, a, b, dt, open, last), want, 2.0 * static_cast<double>(dt));
					CHECK(last.event == StruggleEvent::kOverwhelmed && last.aWon == (lead > 0.0f));
					// uneven frames
					s = Locked(open, breath);
					double t = 0.0;
					float  longest = 0.0f;
					while (t < 120.0) {
						const float step = dt * jitter(g);
						longest = std::max(longest, step);
						t += static_cast<double>(step);
						if (Advance(s, a, b, kBoth, step, open).event != StruggleEvent::kNone) {
							break;
						}
					}
					NEAR(t, want, 2.0 * static_cast<double>(longest));
				}
			}
		}
		const auto seconds = [&](float sa, float sb) { return SecondsToWin(Advantage(Mage(sa), Mage(sb), k), 1, k); };
		NEAR(seconds(80, 40), 3.9f, 0.05);
		NEAR(seconds(60, 40), 7.2f, 0.05);
		NEAR(seconds(50, 45), 9.3f, 0.05);
	}

	void TestLockIn()
	{
		const StruggleConfig k;
		for (const bool aStrong : { true, false }) {
			const Caster strong = Mage(300, 300, 1000), weak = Mage(0, 1, 1);
			Struggle     s = Locked(k);
			while (s.age + 1.0f / 60.0f <= kLockIn) {
				(void)Advance(s, aStrong ? strong : weak, aStrong ? weak : strong, kBoth, 1.0f / 60.0f, k);
				CHECK(s.balance == 0.0f);
			}
			NEAR(std::abs(s.lead), 0.9f, 0.1);  // it would push hard
			for (int i = 0; i < 3; ++i) {
				(void)Advance(s, aStrong ? strong : weak, aStrong ? weak : strong, kBoth, 1.0f / 60.0f, k);
			}
			CHECK(aStrong ? s.balance > 0.0f : s.balance < 0.0f);
		}
	}

	void TestGrace()
	{
		const StruggleConfig k;
		const float          dt = 1.0f / 60.0f;
		const Caster         a = Mage(60), b = Mage(40);
		Struggle             s = Locked(k);
		for (int i = 0; i < 60; ++i) {
			CHECK(Advance(s, a, b, kBoth, dt, k).event == StruggleEvent::kNone);
		}
		const float held = s.balance;
		CHECK(held > 0.0f);
		for (int i = 0; i < 18; ++i) {  // B unseen for 0.3 s: the lock holds, nothing ends
			const StruggleStep step = Advance(s, a, b, { true, true, false }, dt, k);
			CHECK(step.event == StruggleEvent::kNone && s.balance == held && step.drainA == 0.0f && step.drainB == 0.0f && !step.spark);
		}
		CHECK(Advance(s, a, b, kBoth, dt, k).event == StruggleEvent::kNone);  // back again: it goes on
		CHECK(s.balance > held && s.quietB == 0.0f);

		// one that stops: how far it was pushed back decides whether it lost or gave way
		struct Quit
		{
			bool          aStops;
			float         balance;
			StruggleEvent event;
			bool          aWon;
		};
		for (const Quit q : { Quit{ true, -0.25f, StruggleEvent::kOverwhelmed, false }, Quit{ true, -0.2f, StruggleEvent::kOverwhelmed, false },
				 Quit{ true, -0.1f, StruggleEvent::kGaveWay, false }, Quit{ true, 0.5f, StruggleEvent::kGaveWay, false },
				 Quit{ false, 0.25f, StruggleEvent::kOverwhelmed, true }, Quit{ false, 0.1f, StruggleEvent::kGaveWay, true },
				 Quit{ false, -0.5f, StruggleEvent::kGaveWay, true } }) {
			s = Locked(k);
			s.age = 1.0f;
			s.balance = q.balance;
			const Sight  sight{ true, !q.aStops, q.aStops };
			StruggleStep step;
			double       t = 0.0;
			while (t < 5.0 && (step = Advance(s, Mage(50), Mage(50), sight, dt, k)).event == StruggleEvent::kNone) {
				t += static_cast<double>(dt);
			}
			CHECK(step.event == q.event && step.aWon == q.aWon);
			NEAR(t + static_cast<double>(dt), static_cast<double>(kGrace), 1.5 * static_cast<double>(dt));
			CHECK(s.phase == (q.event == StruggleEvent::kOverwhelmed ? Phase::kBroken : Phase::kCooldown));
			NEAR(s.balance, q.balance, 0);  // it never moved while one side was unseen
		}
		s = Locked(k);
		StruggleStep step;
		for (int i = 0; i < 30 && step.event == StruggleEvent::kNone; ++i) {
			step = Advance(s, a, b, { true, false, false }, dt, k);
		}
		CHECK(step.event == StruggleEvent::kReleased && s.phase == Phase::kCooldown);
		s = Locked(k);
		step = Advance(s, a, b, { false, true, true }, dt, k);
		CHECK(step.event == StruggleEvent::kCalledOff && s.phase == Phase::kCooldown && s.timer == 0.0f);
	}

	void TestTimeLimit()
	{
		const StruggleConfig k;  // 10 s
		const float          dt = 1.0f / 60.0f;
		struct Case
		{
			float         balance;
			StruggleEvent event;
			bool          aWon;
		};
		for (const Case c : { Case{ 0.03f, StruggleEvent::kOverwhelmed, true }, Case{ -0.03f, StruggleEvent::kOverwhelmed, false },
				 Case{ 0.015f, StruggleEvent::kDraw, false }, Case{ -0.015f, StruggleEvent::kDraw, false }, Case{ 0.0f, StruggleEvent::kDraw, false } }) {
			Struggle s = Locked(k);
			s.balance = c.balance;
			StruggleStep last;
			NEAR(Run(s, Mage(50), Mage(50), dt, k, last), 10.0, 1.5 * dt);
			CHECK(last.event == c.event && last.aWon == c.aWon);
		}
		// five points of skill always win by the time limit, however slow the push, whatever else there is
		std::mt19937                          g(41);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		for (const float pushTime : { 4.0f, 30.0f }) {
			for (int i = 0; i < 200; ++i) {
				StruggleConfig kk;
				kk.pushTime = pushTime;
				kk.enemyPower = 4;
				kk.surge = u(g) < 0.5f;
				const float high = 5 + u(g) * 295;
				Caster      strong = Mage(high, 1 + u(g) * 50), weak = Mage(high - 5, 1 + u(g) * 299, 1 + u(g) * 200);
				strong.magicka = u(g);
				weak.dual = u(g) < 0.5f;
				weak.mult = PowerMult(true, false, 0, kk);
				const bool   strongIsA = i % 2 == 0;
				Struggle     s = Locked(kk);
				StruggleStep last;
				(void)Run(s, strongIsA ? strong : weak, strongIsA ? weak : strong, dt, kk, last, 20.0);
				CHECK(last.event == StruggleEvent::kOverwhelmed && last.aWon == strongIsA && s.age <= 10.0f + 1e-3f);
			}
		}
		// equal casters, no time limit: nothing ever moves
		StruggleConfig open;
		open.maxTime = 0;
		Struggle s = Locked(open);
		bool     quiet = true;
		for (int i = 0; i < 3600; ++i) {
			quiet = quiet && Advance(s, Mage(50), Mage(50), kBoth, dt, open).event == StruggleEvent::kNone;
		}
		CHECK(quiet && s.balance == 0.0f && s.phase == Phase::kLocked);
	}

	void TestSurgeDrainsSparks()
	{
		const float dt = 1.0f / 60.0f;
		// a surge: once per side, when pushed 0.6 of the way back, for three seconds
		for (const bool aLoses : { true, false }) {
			StruggleConfig k;
			k.surge = true;
			k.pushTime = 30;
			k.maxTime = 0;
			const Caster a = Mage(aLoses ? 40.0f : 80.0f), b = Mage(aLoses ? 80.0f : 40.0f);
			Struggle     s = Locked(k);
			int          surges = 0, others = 0;
			double       t = 0.0, began = -1.0, ended = -1.0;
			float        leadBefore = 0.0f, leadDuring = 0.0f;
			while (t < 200.0) {
				const float        before = s.balance, lastLead = s.lead;
				const StruggleStep step = Advance(s, a, b, kBoth, dt, k);
				t += static_cast<double>(dt);
				if (aLoses ? step.surgeA : step.surgeB) {
					++surges;
					began = t;
					CHECK(aLoses ? (before <= -kSurgeAt && s.balance <= -kSurgeAt) : (before >= kSurgeAt && s.balance >= kSurgeAt));
					leadBefore = lastLead;
					leadDuring = s.lead;
				}
				others += (aLoses ? step.surgeB : step.surgeA) ? 1 : 0;
				if (began >= 0.0 && ended < 0.0 && (aLoses ? s.surgeA : s.surgeB) == 0.0f) {
					ended = t;
				}
				if (step.event != StruggleEvent::kNone) {
					CHECK(step.event == StruggleEvent::kOverwhelmed && step.aWon == !aLoses);  // it slows the loss, never turns it
					break;
				}
			}
			CHECK(surges == 1 && others == 0);
			CHECK(aLoses ? s.surgedA && !s.surgedB : s.surgedB && !s.surgedA);
			NEAR(ended - began, static_cast<double>(kSurgeTime), 1.5 * static_cast<double>(dt));
			CHECK(std::abs(leadDuring) < std::abs(leadBefore) && std::abs(leadDuring) >= kMinLead);
		}
		{
			StruggleConfig k;  // surge off: never
			k.maxTime = 0;
			Struggle     s = Locked(k);
			StruggleStep last;
			bool         any = false;
			for (int i = 0; i < 2000 && last.event == StruggleEvent::kNone; ++i) {
				last = Advance(s, Mage(40), Mage(80), kBoth, dt, k);
				any = any || last.surgeA || last.surgeB;
			}
			CHECK(!any && !s.surgedA && last.event == StruggleEvent::kOverwhelmed);
		}

		// drains: what MagickaDrain says for the side pushed back, and nothing for breath, staffs, at even, or unseen
		NEAR(MagickaDrain(0.5f, 14, 0.1f, StruggleConfig{}), 0.35f, 1e-6);
		NEAR(MagickaDrain(-1, 14, 0.1f, StruggleConfig{}), 0.0f, 0);
		NEAR(MagickaDrain(2, 14, 0.1f, StruggleConfig{}), 0.7f, 1e-6);
		NEAR(MagickaDrain(0.5f, 14, 10, StruggleConfig{}), 0.875f, 1e-6);  // a step is at most a quarter of a second
		NEAR(MagickaDrain(NAN, NAN, NAN, StruggleConfig{}), 0.0f, 0);
		std::mt19937                          g(5);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		for (int round = 0; round < 40; ++round) {
			StruggleConfig k;
			k.maxTime = 0;
			k.magickaPressure = u(g) * 3;
			Caster a = Mage(20 + u(g) * 100), b = Mage(20 + u(g) * 100);
			a.paysMagicka = round % 4 != 1;
			b.paysMagicka = round % 4 != 2;
			Struggle s = Locked(k, round % 3 == 0);
			for (int i = 0; i < 1200; ++i) {
				const Sight        sight{ true, u(g) < 0.9f, u(g) < 0.9f };
				const StruggleStep step = Advance(s, a, b, sight, dt, k);
				const bool         both = sight.seenA && sight.seenB;
				CHECK(step.drainA == (both && a.paysMagicka ? MagickaDrain(-s.balance, a.cost, dt, k) : 0.0f));
				CHECK(step.drainB == (both && b.paysMagicka ? MagickaDrain(s.balance, b.cost, dt, k) : 0.0f));
				CHECK(step.drainA >= 0.0f && step.drainB >= 0.0f && (step.drainA == 0.0f || step.drainB == 0.0f));
				if (s.balance == 0.0f) {
					CHECK(step.drainA == 0.0f && step.drainB == 0.0f);
				}
				CHECK(!step.spark || both);
				if (step.event != StruggleEvent::kNone) {
					break;
				}
			}
		}
		// sparks: every half second while both push
		{
			StruggleConfig k;
			k.maxTime = 0;
			Struggle s = Locked(k);
			int      sparks = 0;
			double   t = 0.0, last = 0.0;
			for (int i = 0; i < 600; ++i) {
				t += static_cast<double>(dt);
				if (Advance(s, Mage(50), Mage(50), kBoth, dt, k).spark) {
					++sparks;
					NEAR(t - last, static_cast<double>(kSparkEvery), 1.5 * static_cast<double>(dt));
					last = t;
				}
			}
			CHECK(sparks == 20 || sparks == 19);
		}
	}

	void TestPhases()
	{
		const float dt = 1.0f / 60.0f;
		for (const float breakTime : { 1.5f, 0.0f, 5.0f }) {
			StruggleConfig k;
			k.breakTime = breakTime;
			Struggle s = Locked(k);
			s.age = 1.0f;
			s.balance = 0.999f;
			const StruggleStep won = Advance(s, Mage(80), Mage(40), kBoth, dt, k);
			CHECK(won.event == StruggleEvent::kOverwhelmed && won.aWon && s.aWon);
			CHECK(s.phase == (breakTime > 0.0f ? Phase::kBroken : Phase::kCooldown));
			double t = 0.0;
			while (s.phase == Phase::kBroken && t < 10.0) {
				CHECK(Advance(s, Mage(80), Mage(40), kBoth, dt, k).event == StruggleEvent::kNone);
				t += static_cast<double>(dt);
			}
			NEAR(t, static_cast<double>(breakTime), 1.5 * static_cast<double>(dt));
			CHECK(s.phase == Phase::kCooldown);
			t = 0.0;
			StruggleStep step;
			while (t < 10.0 && (step = Advance(s, Mage(80), Mage(40), kBoth, dt, k)).event == StruggleEvent::kNone) {
				t += static_cast<double>(dt);
			}
			CHECK(step.event == StruggleEvent::kGone);
			NEAR(t + static_cast<double>(dt), static_cast<double>(kCooldownTime), 1.5 * static_cast<double>(dt));
		}
		// a missed roll: they pass through each other until one stops streaming at all
		StruggleConfig never;
		never.chance = 0;
		Struggle s = Locked(never);
		CHECK(s.phase == Phase::kDeclined);
		for (int i = 0; i < 300; ++i) {
			CHECK(Advance(s, Mage(80), Mage(40), kBoth, dt, never).event == StruggleEvent::kNone);
		}
		CHECK(s.phase == Phase::kDeclined && s.balance == 0.0f);
		double       t = 0.0;
		StruggleStep step;
		while (t < 5.0 && (step = Advance(s, Mage(80), Mage(40), { true, true, false }, dt, never)).event == StruggleEvent::kNone) {
			t += static_cast<double>(dt);
		}
		CHECK(step.event == StruggleEvent::kGone);
		NEAR(t + static_cast<double>(dt), static_cast<double>(kGrace), 1.5 * static_cast<double>(dt));
		// not a phase at all
		s = Locked(StruggleConfig{});
		s.phase = static_cast<Phase>(42);
		CHECK(Advance(s, Mage(1), Mage(1), kBoth, dt, StruggleConfig{}).event == StruggleEvent::kGone);
	}

	bool AllFinite(const Struggle& s, const StruggleStep& a_step)
	{
		for (const float f : { s.balance, s.startShare, s.age, s.quietA, s.quietB, s.timer, s.lead, s.surgeA, s.surgeB, s.nextSpark, s.reachA,
				 s.reachB, a_step.lead, a_step.drainA, a_step.drainB }) {
			if (!std::isfinite(f)) {
				return false;
			}
		}
		return Finite(s.muzzleA) && Finite(s.muzzleB) && s.balance >= -1.0f && s.balance <= 1.0f && a_step.drainA >= 0.0f && a_step.drainB >= 0.0f;
	}

	void TestStruggleGarbage()
	{
		std::mt19937                          g(77);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		const float                           dts[]{ NAN, -1.0f, INFINITY, -INFINITY, 1e9f, 0.0f, 1.0f / 60.0f, 1e-30f };
		for (int round = 0; round < 400; ++round) {
			const Caster   a = RandomCaster(g, true), b = RandomCaster(g, true);
			StruggleConfig k = RandomWeights(g, round % 2 == 0);
			k.pushTime = Garbage(g, 1, 30);
			k.maxTime = Garbage(g, 0, 60);
			k.breakTime = Garbage(g, 0, 5);
			k.magickaPressure = Garbage(g, 0, 3);
			k.chance = Garbage(g, 0, 100);
			const Vec3   meet{ Garbage(g, 0, 1000), 0, 0 }, far{ Garbage(g, -5000, 5000), 0, 0 };
			const float  reachA = Garbage(g, 0, 3000), reachB = Garbage(g, 0, 3000);
			Struggle     s = Begin(7, 3, Action::kClash, meet, { 0, 0, 0 }, far, reachA, reachB, round % 5 == 0, round % 3 == 0, round % 7 == 0,
				static_cast<std::uint64_t>(round), k);
			StruggleStep step;
			CHECK(AllFinite(s, step));
			for (int i = 0; i < 200; ++i) {
				const Sight sight{ u(g) < 0.97f, u(g) < 0.8f, u(g) < 0.8f };
				step = Advance(s, a, b, sight, dts[static_cast<std::size_t>(i) % std::size(dts)], k);
				CHECK(AllFinite(s, step));
				const Front f = FrontOf(s);
				CHECK(!f.valid || (std::isfinite(f.at) && Finite(f.point) && f.at >= 0.0f && f.at <= f.length));
				if (step.event == StruggleEvent::kGone) {
					break;
				}
			}
		}
		// garbage in the struggle itself is not kept
		Struggle s = Locked(StruggleConfig{});
		s.balance = NAN;
		s.age = INFINITY;
		s.quietA = -5;
		s.nextSpark = NAN;
		s.lead = INFINITY;
		const StruggleStep step = Advance(s, Mage(50), Mage(40), kBoth, 1.0f / 60.0f, StruggleConfig{});
		CHECK(AllFinite(s, step));
	}

	void TestStruggleSymmetry()
	{
		// the same fight seen from the other side is the mirror image, step by step
		std::mt19937                          g(8);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		for (int round = 0; round < 300; ++round) {
			StruggleConfig k = RandomWeights(g, false);
			k.surge = false;
			k.maxTime = u(g) < 0.5f ? 0.0f : 3 + u(g) * 20;
			k.magickaPressure = u(g) * 3;
			const Caster a = RandomCaster(g, false), b = RandomCaster(g, false);
			Struggle     ab = Locked(k, round % 3 == 0), ba = ab;
			const float  dt = 1.0f / (30.0f + u(g) * 114.0f);
			for (int i = 0; i < 3000; ++i) {
				const bool         sa = u(g) < 0.93f, sb = u(g) < 0.93f;
				const StruggleStep x = Advance(ab, a, b, { true, sa, sb }, dt, k);
				const StruggleStep y = Advance(ba, b, a, { true, sb, sa }, dt, k);
				CHECK(ab.balance == -ba.balance && x.lead == -y.lead);
				CHECK(x.drainA == y.drainB && x.drainB == y.drainA && x.spark == y.spark);
				CHECK(x.event == y.event);
				if (x.event == StruggleEvent::kOverwhelmed || x.event == StruggleEvent::kGaveWay) {
					CHECK(x.aWon != y.aWon);
				}
				if (x.event != StruggleEvent::kNone) {
					break;
				}
			}
		}

		// Begin keeps the sides in handle order, and everything that goes with them
		const StruggleConfig k;
		Struggle             s = Begin(9, 4, Action::kWins, { 300, 0, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 700, 1500, false, false, false, 1, k);
		CHECK(s.a == 4 && s.b == 9 && s.reaction == Action::kLoses && s.placed && s.phase == Phase::kLocked);
		CHECK(s.muzzleA == (Vec3{ 1000, 0, 0 }) && s.muzzleB == (Vec3{ 0, 0, 0 }) && s.reachA == 1500.0f && s.reachB == 700.0f);
		NEAR(s.startShare, 0.7f, 1e-5);
		s = Begin(4, 9, Action::kWins, { 300, 0, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 700, 1500, false, false, false, 1, k);
		CHECK(s.a == 4 && s.b == 9 && s.reaction == Action::kWins && s.reachA == 700.0f);
		NEAR(s.startShare, 0.3f, 1e-5);
		s = Begin(4, 9, Action::kClash, { 300, 0, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 700, 1500, true, false, false, 1, k);
		NEAR(s.startShare, 0.5f, 0);  // two beams: halfway
		s = Begin(4, 9, Action::kClash, { 10, 0, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 700, 1500, false, false, false, 1, k);
		NEAR(s.startShare, 0.15f, 0);
		s = Begin(4, 9, Action::kClash, { 990, 50, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 700, 1500, false, false, false, 1, k);
		NEAR(s.startShare, 0.85f, 0);
		s = Begin(4, 9, Action::kClash, { NAN, 0, 0 }, { 0, 0, 0 }, { 1000, 0, 0 }, 700, 1500, false, false, false, 1, k);
		NEAR(s.startShare, 0.5f, 0);
		CHECK(s.seed == 1);
		StruggleConfig dragonsNever, onlyDragons;
		dragonsNever.dragonChance = 0;
		onlyDragons.chance = 0;
		for (std::uint64_t seed = 0; seed < 200; ++seed) {
			const auto phase = [&](bool a_dragon, const StruggleConfig& a_k) {
				return Begin(1, 2, Action::kClash, {}, { 0, 0, 0 }, { 1000, 0, 0 }, 1000, 1000, false, a_dragon, a_dragon, seed, a_k).phase;
			};
			CHECK(phase(true, dragonsNever) == Phase::kDeclined && phase(false, dragonsNever) == Phase::kLocked);
			CHECK(phase(true, onlyDragons) == Phase::kLocked && phase(false, onlyDragons) == Phase::kDeclined);
		}
	}

	void TestRoll()
	{
		for (std::uint64_t seed = 0; seed < 2000; ++seed) {
			CHECK(!Roll(seed, 0.0f) && !Roll(seed, -5.0f) && !Roll(seed, NAN) && !Roll(seed, -INFINITY));
			CHECK(Roll(seed, 100.0f) && Roll(seed, 1000.0f) && Roll(seed, INFINITY));
		}
		for (const float p : { 25.0f, 50.0f, 75.0f }) {
			int hits = 0;
			for (std::uint64_t seed = 0; seed < 200000; ++seed) {
				hits += Roll(seed, p) ? 1 : 0;
			}
			NEAR(hits / 200000.0, static_cast<double>(p) / 100.0, 0.01);
		}
		CHECK(Roll(12345, 50) == Roll(12345, 50) && Roll(98765, 33) == Roll(98765, 33));
		CHECK(Mix(0) == 0xE220A8397B1DCDAFull);  // SplitMix64's first output from 0
		CHECK(Mix(42) == Mix(42) && Mix(1) != Mix(2));
	}

	void TestStruggleGeometry()
	{
		const Vec3 o{ 0, 0, 0 }, x1000{ 1000, 0, 0 };
		AxisPoint  p = Project({ 50, 0, 0 }, o, { 100, 0, 0 });
		NEAR(p.along, 50.0f, 1e-5);
		NEAR(p.off, 0.0f, 1e-5);
		p = Project({ 50, 30, 40 }, o, { 100, 0, 0 });
		NEAR(p.along, 50.0f, 1e-5);
		NEAR(p.off, 50.0f, 1e-4);
		p = Project({ -20, 0, 0 }, o, { 100, 0, 0 });  // behind
		NEAR(p.along, -20.0f, 1e-5);
		p = Project({ 3, 4, 0 }, o, o);  // no axis
		NEAR(p.along, 0.0f, 0);
		NEAR(p.off, 5.0f, 1e-5);
		p = Project({ 0, 10, 0 }, { 0, 0, 0 }, { 0, 0, 7 });
		NEAR(p.along, 0.0f, 1e-6);
		NEAR(p.off, 10.0f, 1e-5);
		CHECK(!InCorridor(Project({ NAN, 0, 0 }, o, x1000), 1000, 0));
		CHECK(!InCorridor(Project({ 10, 0, 0 }, { INFINITY, 0, 0 }, x1000), 1000, 0));

		CHECK(InCorridor({ -100, 0 }, 1000, 0) && !InCorridor({ -101, 0 }, 1000, 0));
		CHECK(InCorridor({ 1100, 0 }, 1000, 0) && !InCorridor({ 1101, 0 }, 1000, 0));
		CHECK(InCorridor({ 0, 150 }, 1000, 0) && !InCorridor({ 0, 151 }, 1000, 0));
		CHECK(InCorridor({ -50, 150 }, 1000, 0) && !InCorridor({ -50, 151 }, 1000, 0));  // no narrower behind the muzzle
		CHECK(InCorridor({ 0, 160 }, 1000, 10));
		CHECK(InCorridor({ 100, 185 }, 1000, 0) && !InCorridor({ 100, 186 }, 1000, 0));  // a spray fans out
		CHECK(!InCorridor({ NAN, 0 }, 1000, 0) && !InCorridor({ 0, NAN }, 1000, 0) && !InCorridor({ 0, 0 }, NAN, 0));
		CHECK(InCorridor({ 0, 150 }, 1000, NAN) && !InCorridor({ 0, 151 }, 1000, -50));

		CHECK(BeamAims(o, { 2, 0, 0 }, x1000, 0));
		CHECK(BeamAims(o, { 1, 0, 0 }, { 1000, 499, 0 }, 0) && !BeamAims(o, { 1, 0, 0 }, { 1000, 501, 0 }, 0));
		CHECK(BeamAims(o, { 1, 0, 0 }, { 1000, 509, 0 }, 10));
		CHECK(!BeamAims(o, { 1, 0, 0 }, { -10, 0, 0 }, 0));  // behind
		CHECK(!BeamAims(o, { 0, 0, 0 }, x1000, 0) && !BeamAims(o, { NAN, 1, 0 }, x1000, 0) && !BeamAims({ NAN, 0, 0 }, { 1, 0, 0 }, x1000, 0));

		NEAR(StartShare({ 500, 0, 0 }, o, x1000), 0.5f, 1e-6);
		NEAR(StartShare({ 300, 200, 0 }, o, x1000), 0.3f, 1e-6);
		NEAR(StartShare({ 50, 0, 0 }, o, x1000), 0.15f, 0);
		NEAR(StartShare({ 2000, 0, 0 }, o, x1000), 0.85f, 0);
		NEAR(StartShare({ -300, 0, 0 }, o, x1000), 0.15f, 0);
		NEAR(StartShare({ 500, 0, 0 }, o, o), 0.5f, 0);
		NEAR(StartShare({ 500, 0, 0 }, o, { 0.5f, 0, 0 }), 0.5f, 0);
		NEAR(StartShare({ NAN, 0, 0 }, o, x1000), 0.5f, 0);

		NEAR(StreamReach(1000, 2000, 1), 1000.0f, 0);
		NEAR(StreamReach(5000, 1000, 2), 2000.0f, 0);
		NEAR(StreamReach(NAN, 1000, 1.5f), 1500.0f, 0);
		NEAR(StreamReach(-1, 1000, 1.5f), 1500.0f, 0);
		NEAR(StreamReach(1200, NAN, 1), 1200.0f, 0);
		NEAR(StreamReach(1200, 1000, -1), 1200.0f, 0);
		NEAR(StreamReach(0, 0, 0), 800.0f, 0);  // nothing known
		NEAR(StreamReach(NAN, INFINITY, 2), 800.0f, 0);
		NEAR(StreamReach(INFINITY, 1e30f, 1e30f), 800.0f, 0);  // flies further than a float holds: not a number to use
		NEAR(StreamReach(5000, 1e30f, 1e30f), 5000.0f, 0);
		NEAR(StreamReach(10, 1000, 1), 150.0f, 0);
		NEAR(StreamReach(1e9f, 1e9f, 10), 6000.0f, 0);

		CHECK(InReach(2400, 1000, 1000) && !InReach(2400.5f, 1000, 1000));
		CHECK(InReach(100, NAN, -5) && !InReach(101, NAN, -5));
		CHECK(!InReach(NAN, 1000, 1000) && !InReach(INFINITY, 1000, 1000) && InReach(-INFINITY, 0, 0));
	}

	void TestFront()
	{
		const StruggleConfig k;
		Struggle             s = Locked(k);  // D 1000, reaches 2000, met halfway: lo 60, hi 940, x0 500
		const auto           at = [&](float a_balance, float a_age) {
			s.balance = a_balance;
			s.age = a_age;
			return FrontOf(s).at;
		};
		const auto  plain = [](float b, float lo, float x0, float hi) { return b >= 0.0f ? x0 + b * (hi - x0) : x0 + b * (x0 - lo); };
		const Front f = FrontOf(s);
		CHECK(f.valid);
		NEAR(f.length, 1000.0f, 1e-3);
		NEAR(f.axis.x, 1.0f, 1e-6);
		NEAR(f.point.x, f.at, 1e-3);
		for (int ages = 0; ages < 50; ++ages) {
			const float age = static_cast<float>(ages) * 0.37f;
			NEAR(at(0, age), 500.0f, kWobble * 440.0f + 1e-3f);  // where they met, give or take the wobble
			NEAR(at(1, age), 940.0f, 1e-3);                      // B's hands, less the gap
			NEAR(at(-1, age), 60.0f, 1e-3);
			float last = -1.0f;
			bool  ordered = true, near = true;
			for (int i = 0; i <= 2000; ++i) {
				const float b = -1.0f + static_cast<float>(i) / 1000.0f;
				const float x = at(b, age);
				ordered = ordered && x >= last && x >= 60.0f && x <= 940.0f;  // in order, and never out of bounds
				near = near && std::abs(x - plain(b, 60, 500, 940)) <= kWobble * 440.0f + 1e-3f;
				last = x;
			}
			CHECK(ordered && near);
		}
		// never beyond what either stream reaches
		s.reachA = 300;
		NEAR(at(1, 0), 285.0f, 1e-3);
		s.reachA = 2000;
		s.reachB = 400;
		NEAR(at(-1, 0), 620.0f, 1e-3);
		// streams too short to meet: one point, wherever they balance
		s.reachA = 300;
		for (const float b : { -1.0f, -0.3f, 0.0f, 0.6f, 1.0f }) {
			NEAR(at(b, 1.3f), 500.0f, 0);
		}
		// casters too close for two hand gaps: halfway
		Struggle close = Begin(1, 2, Action::kClash, { 10, 0, 0 }, { 0, 0, 0 }, { 100, 0, 0 }, 2000, 2000, false, false, false, 0, k);
		close.balance = 0.8f;
		NEAR(FrontOf(close).at, 50.0f, 1e-4);
		// not placed, or the muzzles on top of each other: no front
		CHECK(!FrontOf(Struggle{}).valid);
		Struggle one = Locked(k);
		one.muzzleB = one.muzzleA + Vec3{ 0.5f, 0, 0 };
		CHECK(!FrontOf(one).valid);
		one.muzzleB = { NAN, 0, 0 };
		CHECK(!FrontOf(one).valid);
		// Place keeps the last good geometry
		Struggle moved = Locked(k);
		Place(moved, { 0, 0, 0 }, { 0.5f, 0, 0 }, 100, 100);
		CHECK(moved.muzzleB == (Vec3{ 1000, 0, 0 }) && moved.reachA == 2000.0f);
		Place(moved, { 0, 0, 0 }, { 800, 0, 0 }, NAN, 100);
		CHECK(moved.muzzleB == (Vec3{ 1000, 0, 0 }));
		Place(moved, { 0, 0, 0 }, { 800, 0, 0 }, 900, 100);
		CHECK(moved.muzzleB == (Vec3{ 800, 0, 0 }) && moved.reachA == 900.0f && moved.reachB == 100.0f);
		// the wobble, at any age and balance, never more than its share of the way
		std::mt19937                          g(9);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		Struggle                              w = Locked(k);
		w.startShare = 0.3f;  // x0 300: lo 60, hi 940
		for (int i = 0; i < 20000; ++i) {
			w.balance = u(g) * 2 - 1;
			w.age = u(g) * 1000;
			const float x = FrontOf(w).at;
			CHECK(std::abs(x - plain(w.balance, 60, 300, 940)) <= kWobble * 640.0f + 1e-3f && x >= 60.0f && x <= 940.0f);
			const float wob = Wobble(w.age);
			CHECK(wob >= -1.0f && wob <= 1.0f);
		}
		CHECK(Wobble(NAN) == 0.0f && Wobble(INFINITY) == 0.0f);
	}

	void TestPastFront()
	{
		// A at the origin, B at 1000, the front 400 from A; and the same seen from B
		Front f;
		f.from = { 0, 0, 0 };
		f.axis = { 1, 0, 0 };
		f.length = 1000;
		f.at = 400;
		f.point = { 400, 0, 0 };
		f.valid = true;
		Front m = f;
		m.from = { 1000, 0, 0 };
		m.axis = { -1, 0, 0 };
		m.at = 600;
		CHECK(PastFront(f, true, { 500, 0, 0 }, 10) && PastFront(f, true, { 395, 0, 0 }, 10) && !PastFront(f, true, { 350, 0, 0 }, 10));
		CHECK(PastFront(f, false, { 350, 0, 0 }, 10) && PastFront(f, false, { 405, 0, 0 }, 10) && !PastFront(f, false, { 500, 0, 0 }, 10));
		CHECK(!PastFront(f, true, { 500, 400, 0 }, 10));  // outside the tube
		CHECK(PastFront(f, true, { 500, 300, 0 }, 10));   // inside it: the tube fans out
		CHECK(!PastFront(f, true, { -50, 0, 0 }, 10));    // behind its own muzzle
		CHECK(!PastFront(f, false, { 1050, 0, 0 }, 10));
		CHECK(!PastFront(f, true, { 1200, 0, 0 }, 10));  // beyond the other's tube
		CHECK(!PastFront(Front{}, true, { 500, 0, 0 }, 10) && !PastFront(f, true, { NAN, 0, 0 }, 10));
		std::mt19937                       g(10);
		std::uniform_int_distribution<int> px(-300, 1300), py(-600, 600), rad(0, 40);
		int                                past = 0;
		for (int i = 0; i < 20000; ++i) {
			const Vec3  p{ static_cast<float>(px(g)), static_cast<float>(py(g)), static_cast<float>(py(g) / 3) };
			const float r = static_cast<float>(rad(g));
			CHECK(PastFront(f, true, p, r) == PastFront(m, false, p, r));
			CHECK(PastFront(f, false, p, r) == PastFront(m, true, p, r));
			CHECK(!(PastFront(f, true, p, 0) && PastFront(f, false, p, 0)));  // with no size, past the front for one side at most
			past += PastFront(f, true, p, r) ? 1 : 0;
		}
		CHECK(past > 1000);

		NEAR(BeamCut(f, { 0, 0, 0 }, { 1, 0, 0 }, 3000), 416.0f, 1e-3);
		NEAR(BeamCut(f, { 1000, 0, 0 }, { -3, 0, 0 }, 3000), 616.0f, 1e-3);  // B's beam, to the same plane
		const float s60 = std::sin(60.0f * kPi / 180.0f), c60 = std::cos(60.0f * kPi / 180.0f);
		NEAR(BeamCut(f, { 0, 0, 0 }, { c60, s60, 0 }, 3000), 816.0f, 1e-2);
		const float c79 = std::cos(79.0f * kPi / 180.0f), s79 = std::sin(79.0f * kPi / 180.0f);
		const float c77 = std::cos(77.0f * kPi / 180.0f), s77 = std::sin(77.0f * kPi / 180.0f);
		NEAR(BeamCut(f, { 0, 0, 0 }, { c79, s79, 0 }, 3000), 3000.0f, 0);  // too far off the axis to cut
		CHECK(BeamCut(f, { 0, 0, 0 }, { c77, s77, 0 }, 3000) < 3000.0f);
		NEAR(BeamCut(f, { 0, 0, 0 }, { 1, 0, 0 }, 100), 100.0f, 0);    // never longer than it is
		NEAR(BeamCut(f, { 500, 0, 0 }, { 1, 0, 0 }, 3000), 32.0f, 0);  // already past the plane: the shortest
		NEAR(BeamCut(f, { 0, 0, 0 }, { 1, 0, 0 }, 20), 20.0f, 0);      // shorter than that: left as it is
		NEAR(BeamCut(Front{}, { 0, 0, 0 }, { 1, 0, 0 }, 3000), 3000.0f, 0);
		NEAR(BeamCut(f, { 0, 0, 0 }, { 0, 0, 0 }, 3000), 3000.0f, 0);
		CHECK(std::isfinite(BeamCut(f, { 0, 0, 0 }, { 1, 0, 0 }, NAN)) && std::isfinite(BeamCut(f, { NAN, 0, 0 }, { 1, 0, 0 }, 3000)));
		std::uniform_real_distribution<float> u(-1.0f, 1.0f);
		for (int i = 0; i < 20000; ++i) {
			const Vec3  origin{ u(g) * 2000, u(g) * 500, u(g) * 500 }, dir{ u(g), u(g), u(g) };
			const float full = 100 + (u(g) + 1) * 3000;
			const float cut = BeamCut(f, origin, dir, full);
			CHECK(cut >= 32.0f && cut <= full);
		}
	}

	void TestAftermath()
	{
		const StruggleConfig k;
		NEAR(OverwhelmHit(10, 0.5f, 20, k), 16.0f, 1e-4);
		NEAR(OverwhelmHit(10, 0, 0, k), 15.0f, 1e-4);
		NEAR(OverwhelmHit(10, -1, 0, k), 25.0f, 1e-4);
		NEAR(OverwhelmHit(10, 0, 85, k) / OverwhelmHit(10, 0, 0, k), 0.15f, 1e-5);  // resistance leaves at least 15%
		NEAR(OverwhelmHit(10, 0, 500, k), OverwhelmHit(10, 0, 85, k), 0);
		NEAR(OverwhelmHit(10, 0, -30, k), OverwhelmHit(10, 0, 0, k), 0);
		NEAR(OverwhelmHit(-5, 0.5f, 0, k), 0.0f, 0);
		StruggleConfig none;
		none.overwhelmDamage = 0;
		NEAR(OverwhelmHit(10, 1, 0, none), 0.0f, 0);
		NEAR(ShakeStrength(1, 0, k), 0.4f, 1e-6);
		NEAR(ShakeStrength(0.5f, 1500, k), 0.1f, 1e-6);
		NEAR(ShakeStrength(1, 3000, k), 0.0f, 0);
		NEAR(ShakeStrength(1, 5000, k), 0.0f, 0);
		NEAR(ShakeStrength(1, -10, k), 0.4f, 1e-6);
		NEAR(ShakeStrength(NAN, 0, k), 0.0f, 0);
		NEAR(ShakeStrength(1, NAN, k), 0.0f, 0);
		StruggleConfig hard;
		hard.cameraShake = 1;
		NEAR(ShakeStrength(5, 0, hard), 1.0f, 0);
		NEAR(StaggerFor(0.5f, k), 0.45f, 1e-6);
		NEAR(StaggerFor(0, k), 0.3f, 1e-6);
		NEAR(StaggerFor(-1, k), 0.6f, 1e-6);
		NEAR(StaggerFor(NAN, k), 0.3f, 1e-6);
		hard.staggerStrength = 5;
		NEAR(StaggerFor(1, hard), 1.0f, 0);
		std::mt19937 g(12);
		for (int i = 0; i < 20000; ++i) {
			StruggleConfig w;
			w.overwhelmDamage = Garbage(g, 0, 5);
			w.cameraShake = Garbage(g, 0, 1);
			w.staggerStrength = Garbage(g, 0, 1);
			const float magnitude = Garbage(g, 0, 500), lead = Garbage(g, -1, 1), resist = Garbage(g, 0, 100);
			const float hit = OverwhelmHit(magnitude, lead, resist, w);
			const float base = Garbage(g, 0, 1), distance = Garbage(g, 0, 6000);
			const float shake = ShakeStrength(base, distance, w);
			const float stagger = StaggerFor(Garbage(g, -1, 1), w);
			CHECK(std::isfinite(hit) && hit >= 0.0f);
			CHECK(std::isfinite(shake) && shake >= 0.0f && shake <= 1.0f);
			CHECK(std::isfinite(stagger) && stagger >= 0.0f && stagger <= 1.0f);
		}
	}

	// what the struggle maths clamps each setting to is what the menu and the file allow
	template <class M, class S>
	void SameRange(const char* a_key, const M& a_measure, const S& a_set)
	{
		const Range    r = RangeOf(a_key);
		StruggleConfig lo, below, hi, above;
		a_set(lo, r.lo);
		a_set(below, r.lo - 1.0f);
		a_set(hi, r.hi);
		a_set(above, r.hi + 10.0f);
		CHECK(r.hi > r.lo);
		CHECK(a_measure(lo) == a_measure(below));
		CHECK(a_measure(hi) == a_measure(above));
		CHECK(a_measure(lo) != a_measure(hi));
	}

	void TestStruggleRanges()
	{
		Caster c = Mage(80, 40, 30);
		c.magicka = 0.3f;
		c.dual = true;
		const auto power = [&](const StruggleConfig& k) { return LogPower(c, k); };
		SameRange("SkillWeight", power, [](StruggleConfig& k, float v) { k.skillWeight = v; });
		SameRange("LevelWeight", power, [](StruggleConfig& k, float v) { k.levelWeight = v; });
		SameRange("SpellWeight", power, [](StruggleConfig& k, float v) { k.spellWeight = v; });
		SameRange("MagickaWeight", power, [](StruggleConfig& k, float v) { k.magickaWeight = v; });
		SameRange("DualCastBonus", power, [](StruggleConfig& k, float v) { k.dualBonus = v; });
		SameRange("SurgePower", [](const StruggleConfig& k) { return Advantage(Mage(50), Mage(50), k, true, false); }, [](StruggleConfig& k, float v) { k.surgePower = v; });
		SameRange("EnemyPower", [](const StruggleConfig& k) { return PowerMult(true, false, 0, k); }, [](StruggleConfig& k, float v) { k.enemyPower = v; });
		SameRange("DragonPower", [](const StruggleConfig& k) { return PowerMult(false, true, 0, k); }, [](StruggleConfig& k, float v) { k.dragonPower = v; });
		SameRange("ShoutWordBonus", [](const StruggleConfig& k) { return PowerMult(false, false, 3, k); }, [](StruggleConfig& k, float v) { k.wordBonus = v; });
		SameRange("MagickaPressure", [](const StruggleConfig& k) { return MagickaDrain(1, 20, 0.1f, k); }, [](StruggleConfig& k, float v) { k.magickaPressure = v; });
		SameRange("PushTime", [](const StruggleConfig& k) { return PushRate(k); }, [](StruggleConfig& k, float v) { k.pushTime = v; });
		SameRange("OverwhelmDamage", [](const StruggleConfig& k) { return OverwhelmHit(10, 0.5f, 0, k); }, [](StruggleConfig& k, float v) { k.overwhelmDamage = v; });
		SameRange("CameraShake", [](const StruggleConfig& k) { return ShakeStrength(1, 0, k); }, [](StruggleConfig& k, float v) { k.cameraShake = v; });
		SameRange("StaggerStrength", [](const StruggleConfig& k) { return StaggerFor(0.5f, k); }, [](StruggleConfig& k, float v) { k.staggerStrength = v; });
		// the time limit and the reeling, by how long a struggle takes to end or move on
		SameRange("MaxStruggleTime", [](const StruggleConfig& k) {
				Struggle s = Locked(k);
				s.balance = 0.5f;
				StruggleStep last;
				return Run(s, Mage(50), Mage(50), 0.05f, k, last, 200.0); }, [](StruggleConfig& k, float v) { k.maxTime = v; });
		SameRange("BreakTime", [](const StruggleConfig& k) {
				Struggle s = Locked(k);
				s.age = 1.0f;
				s.balance = 1.0f;
				(void)Advance(s, Mage(50), Mage(50), kBoth, 0.05f, k);
				int steps = 0;
				while (s.phase == Phase::kBroken && steps < 1000) {
					(void)Advance(s, Mage(50), Mage(50), kBoth, 0.05f, k);
					++steps;
				}
				return steps; }, [](StruggleConfig& k, float v) { k.breakTime = v; });
	}

	void TestStruggleBook()
	{
		StruggleBook book;
		Struggle     s;
		s.a = 3;
		s.b = 7;
		book.Put(s);
		CHECK(book.Size() == 1 && book.Find(3, 7) && book.Find(7, 3) == book.Find(3, 7) && !book.Find(3, 8) && !book.Find(7, 7));
		for (const Phase phase : { Phase::kLocked, Phase::kBroken, Phase::kDeclined, Phase::kCooldown }) {
			book.Find(3, 7)->phase = phase;
			const bool live = phase == Phase::kLocked || phase == Phase::kBroken;
			CHECK(book.Engaged(3) == live && book.Engaged(7) == live && !book.Engaged(8) && !book.Engaged(0));
			CHECK(book.Holds(3, 7) == live && book.Holds(7, 3) == live && !book.Holds(3, 8));
			for (const bool aWon : { false, true }) {
				book.Find(3, 7)->aWon = aWon;
				const bool reeling = phase == Phase::kBroken;
				CHECK(book.Loser(7) == (reeling && aWon) && book.Loser(3) == (reeling && !aWon) && !book.Loser(8));
			}
		}
		Struggle t = s;
		t.balance = 0.5f;
		book.Put(t);  // the same pair: it replaces
		CHECK(book.Size() == 1 && book.Find(7, 3)->balance == 0.5f);
		Struggle u;
		u.a = 1;
		u.b = 2;
		Struggle& stored = book.Put(u);
		stored.balance = 0.25f;
		CHECK(book.Size() == 2 && book.Find(2, 1)->balance == 0.25f);
		std::size_t seen = 0;
		for (const auto& [key, struggle] : book) {
			CHECK(key == PairKey(struggle.a, struggle.b));
			++seen;
		}
		CHECK(seen == 2);
		book.Erase(PairKey(7, 3));
		CHECK(book.Size() == 1 && !book.Find(3, 7) && book.Find(1, 2));
		book.Erase(PairKey(40, 41));  // not there: nothing
		CHECK(book.Size() == 1);
		book.Clear();
		CHECK(book.Size() == 0 && !book.Find(1, 2) && !book.Engaged(1));
		CHECK(PairKey(5, 9) == PairKey(9, 5) && PairKey(5, 9) == ((std::uint64_t{ 5 } << 32) | 9));
		static_assert(PairKey(0xFFFFFFFFu, 1) == 0x1FFFFFFFFull);
	}

	void Bench()
	{
		std::mt19937      g(5);
		std::vector<Body> bodies;
		for (std::uint32_t i = 0; i < 400; ++i) {
			bodies.push_back(RandomBody(g, i, 4000.0f));
		}
		std::vector<Contact> out;
		const auto           may = [&](std::size_t i, std::size_t j) { return bodies[i].shooter != bodies[j].shooter; };
		const auto           start = std::chrono::steady_clock::now();
		constexpr int        kRuns = 2000;
		std::size_t          found = 0;
		for (int i = 0; i < kRuns; ++i) {
			FindContacts(bodies, may, 64, out);
			found += out.size();
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRuns;
		std::printf("  bench: 400 projectiles, %.1f microseconds a frame (%zu contacts)\n", us, found / kRuns);
		CHECK(us < 2000.0);  // generous: sanitizer builds are slow; an optimised build is far below this
	}

	// 64 struggles at once, each stepped, its front found and 60 particles held against it, as a frame does
	void BenchStruggles()
	{
		std::mt19937                          g(6);
		std::uniform_real_distribution<float> u(0.0f, 1.0f);
		constexpr std::size_t                 kStruggles = 64, kParticles = 60;
		StruggleConfig                        k;
		k.maxTime = 0;
		k.surge = true;
		std::vector<Struggle> start;
		std::vector<Caster>   casters;
		std::vector<Vec3>     points;
		for (std::size_t i = 0; i < kStruggles; ++i) {
			const Vec3  a{ u(g) * 4000, u(g) * 4000, 0 }, b = a + Vec3{ 300 + u(g) * 1500, u(g) * 300, u(g) * 50 };
			const auto  id = static_cast<std::uint32_t>(2 * i + 1);
			const float reach = 800 + u(g) * 2000;
			Struggle    s = Begin(id, id + 1, Action::kClash, Lerp(a, b, 0.5f), a, b, reach, reach, false, i % 5 == 0, false, i, k);
			s.age = 1 + u(g) * 5;
			s.balance = u(g) * 1.6f - 0.8f;
			start.push_back(s);
			casters.push_back(RandomCaster(g, false));
			casters.push_back(RandomCaster(g, false));
			for (std::size_t p = 0; p < kParticles; ++p) {
				points.push_back(Lerp(a, b, u(g)) + Vec3{ u(g) * 200 - 100, u(g) * 200 - 100, u(g) * 100 - 50 });
			}
		}
		std::vector<Struggle> live;
		constexpr int         kRuns = 2000;
		std::size_t           snuffed = 0;
		float                 moved = 0.0f;
		const auto            begin = std::chrono::steady_clock::now();
		for (int run = 0; run < kRuns; ++run) {
			live = start;  // the same frame each run: none of them ends
			for (std::size_t i = 0; i < kStruggles; ++i) {
				Struggle&          s = live[i];
				const StruggleStep step = Advance(s, casters[2 * i], casters[2 * i + 1], kBoth, 1.0f / 60.0f, k);
				moved += step.drainA + step.drainB;
				const Front f = FrontOf(s);
				for (std::size_t p = 0; p < kParticles; ++p) {
					snuffed += PastFront(f, p % 2 == 0, points[i * kParticles + p], 12.0f) ? 1 : 0;
				}
			}
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / kRuns;
		std::printf("  bench: 64 struggles, %.1f microseconds a frame (%zu particles past a front, drain %.1f)\n", us, snuffed / kRuns,
			static_cast<double>(moved) / kRuns);
		CHECK(us < 2000.0);  // generous, as above: shared CI machines and sanitizer builds are slow; an optimised build is far below
	}

	// auto lock: the launch lock, the steering into an oncoming spell, the turn limit and the model's rotation
	void TestAutoLock()
	{
		constexpr float kDeg = 0.017453292f;
		// angles round-trip with DirectionFromAngles, whatever the direction
		for (int i = 0; i < 40; ++i) {
			const float pitch = (static_cast<float>(i % 9) - 4.0f) * 0.3f, heading = static_cast<float>(i) * 0.47f - 3.0f;
			const Vec3  d = DirectionFromAngles(pitch, heading);
			const auto  a = AnglesFromDirection(d);
			const Vec3  back = DirectionFromAngles(a.pitch, a.heading);
			NEAR(Dot(d, back), 1.0f, 1e-5);
		}
		NEAR(AnglesFromDirection({ 0, 0, -1 }).pitch, 1.5707963f, 1e-5);  // straight down is pitch +90 (positive looks down)

		// the lock: inside the cone the bolt flies at the aim point; outside it, or too far, it keeps its own way
		const Vec3 from{ 0, 0, 100 };
		const Vec3 ahead{ 0, 1, 0 };
		const auto locked = LockDirection(from, ahead, { 50, 330, 120 }, 25.0f * kDeg, 4000.0f);
		CHECK(locked.has_value());
		if (locked) {
			NEAR(Length(*locked), 1.0f, 1e-5);
			NEAR(Dot(*locked, UnitOf(Vec3{ 50, 330, 120 } - from)), 1.0f, 1e-5);
		}
		CHECK(!LockDirection(from, ahead, { 330, 50, 100 }, 25.0f * kDeg, 4000.0f));  // 81 degrees off: never pulled round
		CHECK(!LockDirection(from, ahead, { 0, 5000, 100 }, 25.0f * kDeg, 4000.0f));  // out of range
		CHECK(!LockDirection(from, ahead, from, 25.0f * kDeg, 4000.0f));              // on top of it: no way to go
		CHECK(!LockDirection(from, Vec3{}, { 0, 330, 100 }, 25.0f * kDeg, 4000.0f));  // no way of its own: nothing to compare

		// the turn: never more than the limit, and straight to the wanted way within it
		const Vec3 east{ 1, 0, 0 };
		const Vec3 half = TurnToward(ahead, east, 30.0f * kDeg);
		NEAR(std::acos(std::clamp(Dot(half, ahead), -1.0f, 1.0f)), 30.0f * kDeg, 1e-4);
		NEAR(Length(half), 1.0f, 1e-5);
		CHECK(Dot(half, east) > 0.0f);
		CHECK(TurnToward(ahead, UnitOf({ 0.1f, 1, 0 }), 30.0f * kDeg) == UnitOf({ 0.1f, 1, 0 }));
		CHECK(TurnToward(ahead, { 0, -1, 0 }, 30.0f * kDeg) == ahead);  // straight behind: it does not flip

		// steering: two bolts coming at each other a little off the line turn toward each other; one going the same way, one
		// flying across, or one too far does not
		const Vec3 a{ 0, 0, 100 }, b{ 20, 300, 100 };
		const auto turn = 10.0f * kDeg;
		const auto steer = SteerInto(a, ahead, b, { 0, -1, 0 }, 600.0f, 20.0f * kDeg, turn);
		CHECK(steer.has_value());
		if (steer) {
			CHECK(steer->x > 0.0f);                        // toward b, which is to the east of a's way
			NEAR(Dot(*steer, UnitOf(b - a)), 1.0f, 1e-6);  // 3.8 degrees off: reached at once
		}
		CHECK(!SteerInto(a, ahead, b, ahead, 600.0f, 20.0f * kDeg, turn));                       // b flies away from a
		CHECK(!SteerInto(a, ahead, b, east, 600.0f, 20.0f * kDeg, turn));                        // b flies across
		CHECK(!SteerInto(a, ahead, { 0, 900, 100 }, { 0, -1, 0 }, 600.0f, 20.0f * kDeg, turn));  // too far
		CHECK(!SteerInto(a, east, b, { 0, -1, 0 }, 600.0f, 20.0f * kDeg, turn));                 // a flies away from b

		// the model's rotation: forward along the way, right flat, an orthonormal basis with no roll
		for (const Vec3 d : { Vec3{ 0, 1, 0 }, Vec3{ 1, 0, 0 }, UnitOf({ 0.3f, -0.8f, 0.4f }), Vec3{ 0, 0, 1 }, Vec3{ 0, 0, -1 } }) {
			const Basis bs = LookBasis(d);
			NEAR(Dot(bs.forward, d), 1.0f, 1e-5);
			NEAR(Dot(bs.right, bs.forward), 0.0f, 1e-5);
			NEAR(Dot(bs.up, bs.forward), 0.0f, 1e-5);
			NEAR(Dot(bs.right, bs.up), 0.0f, 1e-5);
			NEAR(Length(bs.right), 1.0f, 1e-5);
			NEAR(Length(bs.up), 1.0f, 1e-5);
			NEAR(bs.right.z, 0.0f, 1e-5);
		}
		const Basis north = LookBasis({ 0, 1, 0 });  // the model as it was made: no turn at all
		NEAR(north.right.x, 1.0f, 1e-6);
		NEAR(north.up.z, 1.0f, 1e-6);

		// the settings: in the file the menu writes, read back, clamped
		Config                   c;
		std::vector<std::string> warnings;
		ParseIni("[AutoLock]\nLockYours=0\nSteerIntoSpells=0\nLockArrows=1\nLockCone=90\nSteerTurn=5\n", c, warnings);
		CHECK(!c.lockYours);
		CHECK(c.lockOthers);
		CHECK(!c.steer);
		CHECK(c.lockArrows);
		NEAR(c.lockCone, 60.0f, 0);   // clamped
		NEAR(c.steerTurn, 10.0f, 0);  // clamped
		const std::string text = WriteSettings(c);
		CHECK(text.find("[AutoLock]") != std::string::npos);
		CHECK(text.find("LockYours=0") != std::string::npos);
		Config back;
		ParseIni(text, back, warnings);
		CHECK(back.lockArrows && !back.lockYours && !back.steer);
	}
}

// a file name is read as UTF-8 text, whatever its letters (path::string() would throw on Windows for one the ANSI
// code page cannot show, and settings are read while the game loads)
static void TestPathText()
{
	const std::filesystem::path p(std::u8string(u8"Data/SKSE/Plugins/Crossfire/\u9b54\u6cd5 patch.ini"));
	CHECK(PathText(p.filename()) == "\xE9\xAD\x94\xE6\xB3\x95 patch.ini");
	CHECK(PathText(p.extension()) == ".ini");
	CHECK(PathText(std::filesystem::path("Crossfire_Rules.ini")) == "Crossfire_Rules.ini");
	CHECK(PathText(std::filesystem::path()).empty());
}

int main()
{
	TestPathText();
	TestVectors();
	TestSphereSphere();
	TestSphereBeam();
	TestSphereBarrier();
	TestBeamBeamAndOthers();
	TestValid();
	TestFindContactsAgainstBruteForce();
	TestFindContactsRules();
	TestTable();
	TestResolve();
	TestStrength();
	TestClassify();
	TestLimiter();
	TestFormRef();
	TestIni();
	TestFrameDecisions();
	TestShippedRules();
	TestGarbageNeverCrashes();
	TestStreams();
	TestMayLock();
	TestFacing();
	TestSkill();
	TestLogPower();
	TestAdvantage();
	TestSkillAlwaysWins();
	TestWorkedExamples();
	TestSecondsToWin();
	TestLockIn();
	TestGrace();
	TestTimeLimit();
	TestSurgeDrainsSparks();
	TestPhases();
	TestStruggleGarbage();
	TestStruggleSymmetry();
	TestRoll();
	TestStruggleGeometry();
	TestFront();
	TestPastFront();
	TestAftermath();
	TestStruggleRanges();
	TestStruggleBook();
	TestAutoLock();
	Bench();
	BenchStruggles();
	std::printf("%d checks, %d failed\n", gChecks, gFailed);
	return gFailed == 0 ? 0 : 1;
}

// Arcane Overdrive - SKSE plugin: the clashes and spell struggles (Crossfire's code, brought home)
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// What the files share. The file map is at the top of main.cpp.

#pragma once

#include "ClashCore.h"

namespace Crossfire
{
	// a game point as the core's Vec3, and back
	[[nodiscard]] inline Core::Vec3   V(const RE::NiPoint3& a_p) noexcept { return { a_p.x, a_p.y, a_p.z }; }
	[[nodiscard]] inline RE::NiPoint3 N(Core::Vec3 a_v) noexcept { return { a_v.x, a_v.y, a_v.z }; }

	// ------------------------------------------------------------------ ClashSettings.cpp: the files
	//
	// Read in this order, each on top of the last: SpellMasteryHeat Clash Rules.ini (shipped: elements, reactions,
	// exclusions), every SpellMasteryHeat Clash\*.ini (patches, alphabetical), SpellMasteryHeat Clashes.ini (the menu's). Live() is the game's main thread's own copy;
	// the menu works on a copy of its own (MenuCopy/Submit), handed over as a task on the main thread.
	void                              LoadSettings();                                     // main thread: at data load, and "Reload" in the menu
	[[nodiscard]] const Core::Config& Live();                                             // main thread only
	[[nodiscard]] Core::Config        MenuCopy();                                         // any thread
	[[nodiscard]] Core::Config        MenuCopy(std::uint64_t& a_gen);                     // any thread; a_gen: which change it is
	void                              Submit(const Core::Config& a_config, bool a_save);  // any thread; the rules are kept
	// Submit, unless the menu's copy changed since a_gen (DevBench, a preset): then false, and a_save writes what is there now
	bool SubmitIf(const Core::Config& a_config, bool a_save, std::uint64_t a_gen);
	// the three clash effects the Visuals presets cover (SettingsText kVisuals): designs, the clash light, particle lights
	[[nodiscard]] std::array<bool, 3>      VisualsNow();                                              // any thread
	void                                   SetVisuals(const std::array<bool, 3>& a_on, bool a_save);  // any thread
	void                                   ReloadSoon();                                              // any thread
	[[nodiscard]] std::vector<std::string> LoadNotes();                                               // any thread: what the last load said (warnings, files read)
	[[nodiscard]] bool                     Excluded(const RE::TESForm* a_form);                       // main thread

	// ------------------------------------------------------------------ ClashClassify.cpp: what a projectile is to Crossfire
	struct Info
	{
		bool                     eligible{ false };  // false: Crossfire never touches it
		Core::Element            element{ Core::Element::kArcane };
		Core::Kind               kind{ Core::Kind::kSpell };
		Core::Shape              shape{ Core::Shape::kSphere };
		bool                     immune{ false };
		bool                     cone{ false };
		Core::Stream             stream{ Core::Stream::kNone };    // what kind of stream it is, if one that can lock in a struggle
		float                    cost{ 0.0f };                     // the spell's base cost (a spray: per second; a shout's spray: ShoutStrength)
		RE::ActorValue           skill{ RE::ActorValue::kNone };   // what the player's skill experience goes to
		bool                     staff{ false };                   // cast from a staff: pays no magicka, counts as two hands in a struggle
		bool                     absorb{ false };                  // an Absorb effect: a breakthrough also heals the winner
		float                    magnitude{ 0.0f };                // the costliest effect's magnitude
		RE::ActorValue           resist{ RE::ActorValue::kNone };  // what resists it
		const RE::MagicItem*     spell{ nullptr };
		Core::Look               look{ Core::Look::kArcane };  // which clash design shows it
		std::optional<Core::Rgb> light;                        // its light's color: the design is tinted with it
	};
	[[nodiscard]] const Info&       InfoOf(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base);  // main thread
	void                            ClearInfo();                                                            // main thread
	[[nodiscard]] RE::BGSExplosion* ExplosionOf(RE::Projectile* a_projectile, const RE::BGSProjectile* a_base);
	[[nodiscard]] bool              SafeExplosion(const RE::BGSExplosion* a_explosion);
	void                            Survey();                               // once, at data load: logs how many projectiles there are of each kind
	void                            FindFearSpell();                        // once, at data load: the game's own Fear, for Intimidate
	void                            FindStandInBursts();                    // once, at data load: a burst per element for spells with none
	[[nodiscard]] RE::BGSExplosion* StandInBurst(Core::Element a_element);  // nullptr: none that only shows
	[[nodiscard]] const char*       StandInModel(Core::Element a_element);  // the model of one that carries harm, or nullptr
	void                            FindClashArt();                         // once, at data load: meshes\ArcaneOverdrive\Clash<Look>.nif and ClashPair<Pair>.nif that exist
	[[nodiscard]] const char*       ClashArt(Core::Look a_look);            // that mesh (under meshes\), or nullptr
	[[nodiscard]] const char*       PairArt(std::string_view a_pair);       // a pair's mesh, or nullptr
	[[nodiscard]] bool              ClashArtReady();                        // the art has been looked up (data load)
	[[nodiscard]] RE::SpellItem*    FearSpell();                            // null when none was found

	// ------------------------------------------------------------------ ClashPass.cpp: the pass, once a frame
	void Update(float a_delta);  // main thread, from the player's update
	void Reset();                // a save is loading, or a new game: every handle we hold is meaningless now

	struct Stats
	{
		std::atomic<std::uint32_t> tracked{ 0 };                                                                           // projectiles looked at in the last frame
		std::atomic<std::uint64_t> clashes{ 0 }, destroyed{ 0 }, weakened{ 0 }, explosions{ 0 }, models{ 0 }, yours{ 0 };  // models: a burst shown by its model only
		// spell struggles: begun, broken through, won and lost by you, given way, drawn, and particles snuffed at a lock
		std::atomic<std::uint64_t> struggles{ 0 }, overwhelms{ 0 }, won{ 0 }, lost{ 0 }, gaveWay{ 0 }, draws{ 0 }, cut{ 0 };
		std::atomic<std::uint64_t> locked{ 0 }, steered{ 0 };  // auto lock: bolts turned at launch; bolts that steered
		std::atomic<std::uint64_t> pairs{ 0 }, tinted{ 0 };    // a special pair's design shown; a design tinted with its spell's light
	};
	[[nodiscard]] Stats& Counters();

	// ------------------------------------------------------------------ ClashAim.cpp: auto lock (main thread)
	void FindTdm();                                                                                               // at post load: True Directional Movement's interface
	void TurnProjectile(RE::Projectile* a_p, Core::Vec3 a_dir);                                                   // its velocity, angles and model together
	bool LockAtLaunch(RE::Projectile* a_p, const Info& a_info, RE::Actor* a_shooter, const Core::Config& a_cfg);  // a bolt the pass first sees
	void LockBeam(RE::Projectile* a_p);                                                                           // a one-shot bolt, as it picks its target

	// one stream projectile of this frame's pass, for ClashStruggle.cpp; `entry` stays valid until the end of the pass
	struct StreamRef
	{
		std::size_t                    entry{ 0 };
		RE::Projectile*                projectile{ nullptr };
		RE::ProjectileHandle           handle;
		std::uint32_t                  native{ 0 }, shooter{ 0 };
		RE::ObjectRefHandle            shooterHandle;
		RE::Actor*                     actor{ nullptr };
		bool                           player{ false };
		const Info*                    info{ nullptr };
		const RE::BGSProjectile*       base{ nullptr };
		Core::Vec3                     now, dir;                                       // where it is, and which way it goes (a beam: along itself)
		float                          radius{ 0.0f }, power{ 1.0f }, length{ 0.0f };  // length: a beam's, as drawn
		RE::MagicSystem::CastingSource source{ RE::MagicSystem::CastingSource::kOther };
	};
	void Snuff(std::size_t a_entry);  // kill it quietly: no burst, no event, no experience
	// its own explosion, and (a_design) its clash design with `a_also`'s when that looks different
	bool BurstAt(std::size_t a_entry, Core::Vec3 a_at, std::uint64_t a_pair, const Core::Config& a_config, const Info* a_also = nullptr,
		bool a_design = true);
	// ------------------------------------------------------------------ ClashLights.cpp: the light a design casts (main thread)
	void LightDesign(RE::NiAVObject* a_root, Core::Rgb a_color, float a_scale, float a_percent);  // hung on the design's mesh
	// a light that starts at a_fade and fades out over a_seconds on a node that stays (a hand, a hit target) - Overload.cpp's lingering
	void LightAfterglow(RE::NiNode* a_node, Core::Rgb a_color, float a_reach, float a_fade, float a_seconds);
	// the top of the tree a node hangs in: it changes when its actor or cell leaves the world (a parent alone stays)
	[[nodiscard]] inline const RE::NiAVObject* TopOf(const RE::NiAVObject* a_node) noexcept
	{
		while (a_node && a_node->parent) {
			a_node = a_node->parent;
		}
		return a_node;
	}
	void                      UpdateLights(float a_delta);  // each light flared and faded along its design, let go when done or gone
	void                      ClearLights();                // a load, or the clashes reset: every light let go
	[[nodiscard]] std::size_t LightsLit();
	// DevBench's look at a design: a look's (or a pair's) design played a_ahead in front of the player and a_up above her
	// feet, turned to her heading and tinted as a clash would; queued for the main thread. False: no such look
	bool ShowDesign(std::string_view a_look, std::string_view a_pair, std::optional<Core::Rgb> a_color, float a_ahead, float a_up);

	// ------------------------------------------------------------------ ClashStruggle.cpp: streams that lock (main thread)
	namespace Struggles
	{
		[[nodiscard]] bool Reeling(std::uint32_t a_shooter);  // it lost a struggle a moment ago: its streams fizzle
		void               Frame(std::span<const StreamRef> a_streams, const Core::Config& a_config, float a_delta, std::uint32_t a_frame);
		[[nodiscard]] bool Holds(std::uint32_t a, std::uint32_t b);  // locked together, or one reels from the other
		// two streams of different shooters touched: true when that starts a lock (or one is already on), so the touch
		// is not settled as a clash
		[[nodiscard]] bool TryBegin(const StreamRef& a, const StreamRef& b, Core::Vec3 a_point, const Core::Config& a_config);
		[[nodiscard]] bool Active();
		// a locked spray's hit lands past where the two streams meet: the hook drops it
		[[nodiscard]] bool FlameHitPastFront(RE::Projectile* a_flame, const RE::NiPoint3& a_at);
		// the spray is locked in a struggle right now: it picks no target (its damage stops at the meeting point)
		[[nodiscard]] bool FlameLocked(RE::Projectile* a_flame);
		void               Reset(bool a_restoreBeams);  // false on a load: the handles belong to the world that went away
	}

	// what the struggle bar shows; a copy, from any thread
	struct StruggleView
	{
		bool         active{ false };
		float        balance{ 0.0f };  // + : you are pushing it toward them
		float        lead{ 0.0f }, seconds{ 0.0f };
		std::uint8_t mine{ 0 }, theirs{ 0 };  // elements
		Core::Rgb    myColor, theirColor;     // each stream's own light color (its design's when it has none)
		float        shoveAge{ -1.0f };       // the lock's age at your last Overdrive Breakpoint shove, -1 none
		bool         shoveOnBeat{ false };
		int          mySkill{ 0 }, theirSkill{ 0 };
		bool         myBreath{ false }, theirBreath{ false };
		char         school[16]{}, theirSchool[16]{};
		char         foe[64]{};
		bool         bar{ true };
		float        barHeight{ 82.0f }, barScale{ 1.0f }, barOpacity{ 0.9f };
		bool         barNames{ false }, barSkills{ false };
	};
	[[nodiscard]] StruggleView StruggleNow();

	// ------------------------------------------------------------------ ClashMenu.cpp: the two pages and the struggle bar
	void RegisterMenu();
	void RequestBarPreview();  // any thread: the struggle bar's sample shows for a few seconds (the menu's button, DevBench)

	// ------------------------------------------------------------------ ClashDevBench.cpp
	void OfferToDevBench();
	// ClashDevBench.cpp: the arcaneclash.clash / arcaneclash.struggle events (nothing happens without DevBench)
	void ClashEvent(std::string_view a_elementA, std::string_view a_elementB, bool a_destroyedA, bool a_destroyedB, bool a_yours);
	void StruggleEvent(std::string_view a_state, std::string_view a_nameA, std::string_view a_nameB, float a_balance, float a_seconds);
}

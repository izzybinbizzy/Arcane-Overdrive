// Spell Mastery Heat - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The lights on the player's casting hands grow with the heat. This plugin makes no light of its own unless asked (Hand
// glow): it scales the ones other mods hang there - Illuminated's (through Light Placer), Let There Be Glow's, RELight
// Spell Addon's - anything that is a NiPointLight under a hand's magic node ("NPC L MagicNode [LMag]" and
// "NPC R MagicNode [RMag]", in both the first- and the third-person skeleton). The casting art those mods light
// attaches there.
//
// The scaling is the fading module's, exactly (Scaled.h): each light's fade and radius are multiplied from a remembered
// base value - whatever the light holds that we did not write is its new base - so running twice in a frame never
// compounds, and another plugin scaling the same light the same way (Illuminated's Brightness, the fading module in
// Illuminated or RELight - Spell Addon) is noticed and held
// instead of driven to white. A light no longer under a hand is put back as it was found, unless someone else has
// written it since. Only fade and radius are touched: the game rewrites a light's lod dimmer every frame.
//
// Hand glow: on a hand with a spell in it, weapons drawn, Warm or hotter, and no other light under it, a light of this
// plugin's own (made and registered the ReLight way - one master light cloned for every use - after ReLight by Truman,
// github.com/TrumanGIT/ReLight, GPL-3.0-or-later), in the spell's element's color. It stands down the
// moment another mod lights that hand.
// Everything here runs on the main thread, from the player's update.

#include "Plugin.h"

namespace Plugin
{
	namespace
	{
		constexpr const char*   kNodes[2]{ "NPC L MagicNode [LMag]", "NPC R MagicNode [RMag]" };
		constexpr const char*   kOwnName = "SpellMasteryHeatLight";
		constexpr std::uint32_t kKeepFrames = 2;  // a light not seen for this many frames is put back
		constexpr float         kOwnReach = 160.0f, kOwnFade = 0.9f;
		constexpr std::uint32_t kInverseSquare = 1u << 10;
		constexpr float         kK = 3918.88f, kSize = 1.414f;

		struct Seen
		{
			RE::NiPointer<RE::NiPointLight> light;
			Glow::Scaled                    fade, radius;
			std::uint32_t                   frame{ 0 };
		};

		struct Own
		{
			RE::NiPointer<RE::NiPointLight> light;
			RE::NiPointer<RE::BSLight>      registered;
			RE::NiNode*                     node{ nullptr };  // compared only, never followed
		};

		std::uint32_t                               gFrame = 1;
		std::unordered_map<RE::NiPointLight*, Seen> gLights;
		Own                                         gOwn[2];
		RE::NiPointer<RE::NiPointLight>             gMaster;
		std::atomic<std::size_t>                    gScaled{ 0 }, gFrozen{ 0 }, gOwnCount{ 0 };
		std::atomic<float>                          gFactor[2]{ 1.0f, 1.0f };

		RE::ShadowSceneNode* Scene() { return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]; }

		// Community Shaders' inverse square lights: its own shader, or (Bottled Shaders 1.0.10 and later, which dropped that
		// file) Light Limit Fix's attenuation reading each light's InverseSquare flag - Illuminated's Lighting.cpp reads both
		bool Isl()
		{
			static const bool isl = [] {
				std::error_code ec;
				if (std::filesystem::exists("Data/Shaders/InverseSquareLighting/InverseSquareLighting.hlsli", ec)) {
					return true;
				}
				std::ifstream in("Data/Shaders/LightLimitFix/Attenuation.hlsli", std::ios::binary);
				if (!in) {
					return false;
				}
				const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
				return text.find("InverseSquare") != std::string::npos;
			}();
			return isl;
		}

		void Restore(Seen& a_seen)
		{
			if (!a_seen.light) {
				return;
			}
			auto& data = a_seen.light->GetLightRuntimeData();
			data.fade = a_seen.fade.Restore(data.fade);
			if (a_seen.radius.Written()) {
				const float r = a_seen.radius.Restore(data.radius.x);
				data.radius.x = r;
				data.radius.y = r;
			}
		}

		void Scale(RE::NiPointLight* a_light, float a_factor)
		{
			auto [it, added] = gLights.try_emplace(a_light);
			auto& seen = it->second;
			if (added) {
				seen.light.reset(a_light);
			}
			seen.frame = gFrame;
			if (a_factor == 1.0f && !seen.fade.Written()) {
				return;  // nothing to do, and nothing of ours on it yet
			}
			auto& data = a_light->GetLightRuntimeData();
			data.fade = seen.fade.Apply(data.fade, a_factor);
			const float r = seen.radius.Apply(data.radius.x, a_factor);
			data.radius.x = r;  // x and y are the reach, z is the size
			data.radius.y = r;
		}

		// ---- our own light (Hand glow)
		RE::NiPointLight* CloneMaster()
		{
			if (!gMaster) {
				const RE::NiPointer<RE::NiPointLight> fresh(RE::NiPointLight::Create());
				auto*                                 clone = fresh ? netimmerse_cast<RE::NiPointLight*>(fresh->Clone()) : nullptr;
				if (!clone) {
					return nullptr;
				}
				gMaster.reset(clone);
			}
			return netimmerse_cast<RE::NiPointLight*>(gMaster->Clone());
		}

		void DropOwn(Own& a_own)
		{
			if (a_own.registered) {
				if (auto* scene = Scene()) {
					scene->RemoveLight(a_own.registered);
				}
			}
			if (a_own.light && a_own.light->parent) {
				a_own.light->parent->DetachChild(a_own.light.get());
			}
			a_own = {};
		}

		RE::NiColor ColorOf(int a_element)
		{
			switch (a_element) {
			case 1:
				return { 1.0f, 0.55f, 0.2f };  // fire
			case 2:
				return { 0.55f, 0.8f, 1.0f };  // frost
			case 3:
				return { 0.75f, 0.7f, 1.0f };  // shock
			default:
				return { 1.0f, 0.92f, 0.8f };
			}
		}

		void KeepOwn(Own& a_own, RE::NiNode* a_node, int a_element, float a_factor)
		{
			if (!a_node) {
				DropOwn(a_own);
				return;
			}
			if (!a_own.light || a_own.node != a_node || a_own.light->parent != a_node) {
				DropOwn(a_own);
				auto*                           scene = Scene();
				RE::NiPointer<RE::NiPointLight> light(CloneMaster());
				if (!scene || !light) {
					return;
				}
				light->name = kOwnName;
				auto& data = light->GetLightRuntimeData();
				data.radius = { kOwnReach, kOwnReach, kSize };
				light->SetLightAttenuation(kOwnReach);
				a_node->AttachChild(light.get(), true);
				RE::NiUpdateData update{};
				light->Update(update);
				RE::ShadowSceneNode::LIGHT_CREATE_PARAMS params{};
				params.dynamic = true;
				params.shadowLight = false;
				params.portalStrict = true;
				params.affectLand = true;
				params.affectWater = true;
				params.neverFades = true;
				params.fov = 90.0f;
				params.falloff = 1.0f;
				params.nearDistance = 5.0f;
				params.depthBias = 1.0f;
				auto* registered = scene->AddLight(light.get(), params);
				if (!registered) {
					a_node->DetachChild(light.get());
					return;
				}
				a_own.light = std::move(light);
				a_own.registered.reset(registered);
				a_own.node = a_node;
			}
			// its brightness and reach follow the stage directly (it is ours alone)
			auto&       data = a_own.light->GetLightRuntimeData();
			const float fade = kOwnFade * a_factor;
			const float reach = kOwnReach * a_factor;
			data.diffuse = ColorOf(a_element);
			data.fade = fade;
			data.radius.x = reach;
			data.radius.y = reach;
			if (Isl()) {
				data.ambient.red = std::bit_cast<float>(std::bit_cast<std::uint32_t>(data.ambient.red) | kInverseSquare);
				data.ambient.green = std::clamp(kK * fade / (reach * reach + kSize * kSize), 0.01f, 0.99f);
			}
		}

		// a hand's magic node in one skeleton (a_view 0 first person, 1 third), searched by name only when that skeleton changes
		// or once a second: a name search walks the whole skeleton, every frame it was most of this mod's own frame time
		RE::NiNode* MagicNodeOf(RE::PlayerCharacter* a_player, std::size_t a_hand, std::size_t a_view)
		{
			struct Found
			{
				RE::NiAVObject*           root{ nullptr };
				RE::NiPointer<RE::NiNode> node;
				std::uint32_t             frame{ 0 };
			};
			static std::array<Found, 4>    found{};
			static const RE::BSFixedString kNames[2]{ kNodes[0], kNodes[1] };
			constexpr std::uint32_t        kRefind = 60;
			auto*                          root = a_player->Get3D(a_view == 0);
			auto&                          f = found[a_hand * 2 + a_view];
			if (f.root != root || gFrame - f.frame > kRefind) {
				auto* obj = root ? root->GetObjectByName(kNames[a_hand]) : nullptr;
				f.root = root;
				f.node.reset(obj ? obj->AsNode() : nullptr);
				f.frame = gFrame;
			}
			return f.node.get();
		}

		// the hand's magic nodes in both skeletons, and the caster's own (the one in view)
		std::array<RE::NiNode*, 3> NodesOf(RE::PlayerCharacter* a_player, std::size_t a_hand)
		{
			std::array<RE::NiNode*, 3> nodes{};
			if (auto* caster = a_player->GetActorRuntimeData().magicCasters[a_hand]) {
				nodes[0] = caster->magicNode;
			}
			for (std::size_t v = 0; v < 2; ++v) {
				auto* node = MagicNodeOf(a_player, a_hand, v);
				if (node && node != nodes[0] && node != nodes[1]) {
					nodes[v + 1] = node;
				}
			}
			return nodes;
		}

		// where our own hand light hangs, in BOTH views: the third-person body's magic node of this hand. The caster's own node
		// is the first-person one in first person, and a light under the first-person skeleton lights nothing in the world on ENB
		// / Vanilla (measured). Hidden with that body in first person, the light still shines.
		RE::NiNode* ThirdPersonMagicNode(RE::PlayerCharacter* a_player, std::size_t a_hand) { return MagicNodeOf(a_player, a_hand, 1); }
	}

	void UpdateHands(Heat::Stage a_left, Heat::Stage a_right, const Settings& a_s)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		++gFrame;
		const Heat::Stage stages[2]{ a_left, a_right };
		const bool        drawn = player->AsActorState()->IsWeaponDrawn();
		for (std::size_t hand = 0; hand < 2; ++hand) {
			// the burnout's sputter dips the light with its spell (Overload.cpp; 1 otherwise)
			const float factor = (a_s.lightGrowth ? Heat::LightFactor(a_s.lightPerStage, stages[hand]) : 1.0f) * HandLightFactor(hand);
			gFactor[hand] = factor;
			const auto  nodes = NodesOf(player, hand);
			std::size_t others = 0;
			for (auto* node : nodes) {
				RE::BSVisit::TraverseScenegraphLights(node, [&](RE::NiPointLight* a_light) {
					if (a_light && a_light->name != kOwnName) {
						Scale(a_light, factor);
						++others;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			// our own light: only where nothing else lights the hand
			const auto* item = player->GetEquippedObject(hand == 0);
			const auto* spell = item ? item->As<RE::MagicItem>() : nullptr;
			const bool  want = a_s.handGlow && drawn && spell && others == 0 && stages[hand] >= Heat::Stage::kWarm;
			KeepOwn(gOwn[hand], want ? ThirdPersonMagicNode(player, hand) : nullptr, spell ? Read(spell).element : 0,
				Heat::LightFactor(std::max(a_s.lightPerStage, 0.15f), stages[hand]) * HandLightFactor(hand));
		}
		// lights that left the hands go back as they were
		std::size_t frozen = 0;
		for (auto it = gLights.begin(); it != gLights.end();) {
			if (gFrame - it->second.frame > kKeepFrames) {
				Restore(it->second);
				it = gLights.erase(it);
			} else {
				frozen += it->second.fade.frozen ? 1 : 0;
				++it;
			}
		}
		gScaled = gLights.size();
		gFrozen = frozen;
		gOwnCount = (gOwn[0].light ? 1u : 0u) + (gOwn[1].light ? 1u : 0u);
	}

	void ReleaseHands()
	{
		for (auto& [light, seen] : gLights) {
			Restore(seen);
		}
		gLights.clear();
		DropOwn(gOwn[0]);
		DropOwn(gOwn[1]);
		gScaled = 0;
		gFrozen = 0;
		gOwnCount = 0;
		gFactor[0] = 1.0f;
		gFactor[1] = 1.0f;
	}

	HandsView HandsNow()
	{
		return { gScaled.load(), gFrozen.load(), gOwnCount.load(), { gFactor[0].load(), gFactor[1].load() } };
	}
}

// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The live spell bars (Bar style "Spell bars"), drawn by HudBars.cpp on the render thread - the SKSE Menu Framework HUD
// element Menu.cpp registers calls this. Kept out of Plugin.h, which knows nothing of ImGui.

#pragma once

#include "Plugin.h"

namespace ImGuiMCP
{
	struct ImDrawList;
}

namespace Plugin
{
	// draws the spell bar of a_view's spell where the bar is; false when the framework's draw calls are missing (Menu.cpp
	// then draws the flame bar, so the HUD never shows nothing)
	[[nodiscard]] bool DrawBars(ImGuiMCP::ImDrawList* a_list, const Settings& a_settings, const MeterView& a_view, float a_alpha, float a_delta);
}

// Arcane Overdrive - SKSE plugin
// Copyright (C) 2026 izzydoingit
// GPL-3.0-or-later; see LICENSE.txt and the notice at the top of main.cpp.
//
// The high-res bars - the Overdrive bar and the Heat bar above it - drawn by HudBars.cpp on the render thread; the SKSE Menu
// Framework HUD element Menu.cpp registers calls this. Kept out of Plugin.h, which knows nothing of ImGui.

#pragma once

#include "Plugin.h"

namespace ImGuiMCP
{
	struct ImDrawList;
	struct ImGuiIO;
}

namespace Plugin
{
	// how far above the Overdrive bar's top the thin Heat strip starts, in bar units (x Bar size); the stage's name sits over it
	inline constexpr float kHeatStripAbove = 17.0f;

	// draws both bars in a_view's spell's look where the bar is (a plain bar while the shader compiles or when it cannot);
	// false only without a draw list
	bool DrawBars(ImGuiMCP::ImDrawList* a_list, const Settings& a_settings, const MeterView& a_view, float a_alpha, float a_delta);

	// ImGui's io and the foreground draw list, through the framework calls looked up once (Menu.cpp's HUD element)
	ImGuiMCP::ImGuiIO*    HudIO();
	ImGuiMCP::ImDrawList* HudList();
}

#pragma once
// Launcher look: the backdrop behind the panels (our own procedural Wii U-menu-style sky with drifting white icons, or
// the image <user data>/theme/background.png|jpg if the user put one there) and the light/dark panel choice.
#include <imgui.h>

namespace HostTheme
{
	// starts loading the user's background image in the background (call once)
	void Initialize();
	// fills the whole viewport. textureProvider uploads RGBA once per key (see HostUI::SetTextureProvider)
	void DrawBackground(ImDrawList* drawList, ImVec2 min, ImVec2 max, void* (*getTexture)(uint64 key, const uint8* rgba, int width, int height));

	bool IsLightPanel();
	void SetLightPanel(bool light); // saved to <user data>/theme/theme.txt

	// panel colours for the current choice
	ImU32 PanelFill();
	ImU32 PanelBorder();
	ImU32 PanelText();
	ImU32 PanelTextDim();
	ImU32 Accent();
}

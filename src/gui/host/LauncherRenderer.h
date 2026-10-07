#pragma once

#include "interface/WindowSystem.h"

#include <memory>

struct ImDrawData;
struct ImGuiContext;

// Small D3D12 renderer for the launcher UI (Dear ImGui's DX12 backend). It owns its own device objects and swap chain
// and is destroyed before the emulator's D3D12Renderer creates its swap chain for the same window (a UWP CoreWindow can
// only have one swap chain at a time).
class LauncherRenderer
{
public:
	// throws std::runtime_error on failure
	LauncherRenderer(const WindowSystem::WindowHandleInfo& target, int width, int height, ImGuiContext* imguiContext);
	~LauncherRenderer();

	LauncherRenderer(const LauncherRenderer&) = delete;
	LauncherRenderer& operator=(const LauncherRenderer&) = delete;

	// must be called with the ImGui context current, before ImGui::NewFrame()
	void NewFrame();
	// renders and presents. Returns false if the device was lost
	bool Render(ImDrawData* drawData, const float clearColor[4]);
	void Resize(int width, int height);

	// font atlas changed (font size / scale), recreate the font texture before the next frame
	void InvalidateFonts();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

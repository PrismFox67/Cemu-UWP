#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);

// lets a frontend draw its own UI (e.g. an in-game menu) on top of the main window's output. The callback runs on the
// GPU thread with the main window's ImGui context current, after the regular overlays
void LatteOverlay_setHostOverlayCallback(void (*callback)());
void LatteOverlay_renderHostOverlay(bool pad_view);
// picture-in-picture: the screen that isn't shown full screen (GamePad when the TV is shown, TV when swapped) drawn small
// in a corner of the main window, e.g. MK8's minimap only exists on the GamePad. Set by the frontend
struct LatteScreenPiP
{
	bool enabled = false;
	sint32 corner = 3; // position: 0 top left, 1 top right, 2 bottom left, 3 bottom right, 4 center, 5 side by side
	static constexpr sint32 kCenter = 4, kSideBySide = 5, kPositionCount = 6;
	sint32 size = 0; // 0 small, 1 medium, 2 large
	float opacity = 1.0f;
};
void LatteOverlay_setScreenPiP(const LatteScreenPiP& pip);
LatteScreenPiP LatteOverlay_getScreenPiP();
// height of the overlay as a fraction of the window height
float LatteOverlay_screenPiPHeight(sint32 size);
// side by side: the main image is scaled by this and aligned to the left edge, the overlay fills the space to its right
float LatteOverlay_screenPiPSideBySideScale(sint32 size);
// where the overlay of a texW x texH screen goes in a fullWidth x fullHeight window
void LatteOverlay_screenPiPRect(const LatteScreenPiP& pip, sint32 fullWidth, sint32 fullHeight, sint32 texWidth, sint32 texHeight, sint32& x, sint32& y, sint32& width, sint32& height);
// forget the remembered scan buffers (a new game is starting)
void LatteOverlay_resetScreenPiPSources();

void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);
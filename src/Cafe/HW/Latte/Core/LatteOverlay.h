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
	sint32 corner = 3; // 0 top left, 1 top right, 2 bottom left, 3 bottom right
	sint32 size = 0; // 0 small, 1 medium, 2 large
	float opacity = 1.0f;
};
void LatteOverlay_setScreenPiP(const LatteScreenPiP& pip);
LatteScreenPiP LatteOverlay_getScreenPiP();
// height of the overlay as a fraction of the window height
float LatteOverlay_screenPiPHeight(sint32 size);
// forget the remembered scan buffers (a new game is starting)
void LatteOverlay_resetScreenPiPSources();

void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);
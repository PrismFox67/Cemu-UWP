#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);

// lets a frontend draw its own UI (e.g. an in-game menu) on top of the main window's output. The callback runs on the
// GPU thread with the main window's ImGui context current, after the regular overlays
void LatteOverlay_setHostOverlayCallback(void (*callback)());
void LatteOverlay_renderHostOverlay(bool pad_view);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);
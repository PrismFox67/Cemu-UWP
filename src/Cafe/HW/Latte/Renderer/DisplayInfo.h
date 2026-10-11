#pragma once

#include <atomic>

// Refresh rate of the display mode the host put the TV in (Xbox: the HDMI mode it requested), 0 if unknown. Frame
// interpolation uses it when vsynced presentation runs slower than the output: Xbox Dev Mode syncs app presents to
// 60 Hz even while the HDMI link runs at 120 Hz, but presents without vsync reach the TV at its real rate
inline std::atomic<double> g_hostDisplayModeRefreshHz{ 0.0 };

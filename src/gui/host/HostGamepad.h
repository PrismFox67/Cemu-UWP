#pragma once

// Direct XInput polling for the frontend's own UI (launcher navigation and the in-game menu). This is independent from
// the emulator's InputManager so the UI works before any controller profile exists. On UWP this links xinputuap.
namespace HostGamepad
{
	enum Button : uint32
	{
		kUp = 1 << 0,
		kDown = 1 << 1,
		kLeft = 1 << 2,
		kRight = 1 << 3,
		kA = 1 << 4,
		kB = 1 << 5,
		kX = 1 << 6,
		kY = 1 << 7,
		kStart = 1 << 8, // Menu
		kBack = 1 << 9, // View
		kLB = 1 << 10,
		kRB = 1 << 11,
		kLT = 1 << 12,
		kRT = 1 << 13,
		kL3 = 1 << 14,
		kR3 = 1 << 15,
	};

	struct State
	{
		bool connected = false;
		uint32 buttons = 0; // held
		float leftX = 0.0f, leftY = 0.0f; // -1..1, y up
		float rightX = 0.0f, rightY = 0.0f;
		float leftTrigger = 0.0f, rightTrigger = 0.0f;
	};

	constexpr int kMaxPads = 4; // XInput user indices

	// combined state of all connected controllers (any controller can drive the UI)
	State Poll();

	// state of a single controller, connected is false if there is none at this index
	State PollPad(int index);

	// feeds the polled state into the current ImGui context as gamepad navigation input
	void FeedImGui(const State& state);
}

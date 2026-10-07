#include "gui/host/HostGamepad.h"

#include <imgui.h>

#include <Windows.h>
#include <Xinput.h>

namespace
{
	float _NormalizeStick(SHORT value, SHORT deadzone)
	{
		if (value > deadzone)
			return std::min(1.0f, (float)(value - deadzone) / (32767.0f - deadzone));
		if (value < -deadzone)
			return std::max(-1.0f, (float)(value + deadzone) / (32768.0f - deadzone));
		return 0.0f;
	}
}

HostGamepad::State HostGamepad::Poll()
{
	State result;
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
	{
		XINPUT_STATE xs{};
		if (XInputGetState(i, &xs) != ERROR_SUCCESS)
			continue;
		result.connected = true;
		const WORD b = xs.Gamepad.wButtons;
		auto map = [&](WORD xinputButton, Button button) {
			if (b & xinputButton)
				result.buttons |= button;
		};
		map(XINPUT_GAMEPAD_DPAD_UP, kUp);
		map(XINPUT_GAMEPAD_DPAD_DOWN, kDown);
		map(XINPUT_GAMEPAD_DPAD_LEFT, kLeft);
		map(XINPUT_GAMEPAD_DPAD_RIGHT, kRight);
		map(XINPUT_GAMEPAD_A, kA);
		map(XINPUT_GAMEPAD_B, kB);
		map(XINPUT_GAMEPAD_X, kX);
		map(XINPUT_GAMEPAD_Y, kY);
		map(XINPUT_GAMEPAD_START, kStart);
		map(XINPUT_GAMEPAD_BACK, kBack);
		map(XINPUT_GAMEPAD_LEFT_SHOULDER, kLB);
		map(XINPUT_GAMEPAD_RIGHT_SHOULDER, kRB);
		map(XINPUT_GAMEPAD_LEFT_THUMB, kL3);
		map(XINPUT_GAMEPAD_RIGHT_THUMB, kR3);
		const float lt = xs.Gamepad.bLeftTrigger / 255.0f;
		const float rt = xs.Gamepad.bRightTrigger / 255.0f;
		if (lt > 0.5f)
			result.buttons |= kLT;
		if (rt > 0.5f)
			result.buttons |= kRT;
		result.leftTrigger = std::max(result.leftTrigger, lt);
		result.rightTrigger = std::max(result.rightTrigger, rt);
		auto pickLarger = [](float current, float candidate) { return std::abs(candidate) > std::abs(current) ? candidate : current; };
		result.leftX = pickLarger(result.leftX, _NormalizeStick(xs.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE));
		result.leftY = pickLarger(result.leftY, _NormalizeStick(xs.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE));
		result.rightX = pickLarger(result.rightX, _NormalizeStick(xs.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
		result.rightY = pickLarger(result.rightY, _NormalizeStick(xs.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
	}
	return result;
}

void HostGamepad::FeedImGui(const State& state)
{
	ImGuiIO& io = ImGui::GetIO();
	if (!state.connected)
	{
		io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		return;
	}
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	auto button = [&](ImGuiKey key, Button b) { io.AddKeyEvent(key, (state.buttons & b) != 0); };
	auto analog = [&](ImGuiKey key, float value) { io.AddKeyAnalogEvent(key, value > 0.1f, std::clamp(value, 0.0f, 1.0f)); };
	button(ImGuiKey_GamepadStart, kStart);
	button(ImGuiKey_GamepadBack, kBack);
	button(ImGuiKey_GamepadFaceDown, kA);
	button(ImGuiKey_GamepadFaceRight, kB);
	button(ImGuiKey_GamepadFaceLeft, kX);
	button(ImGuiKey_GamepadFaceUp, kY);
	button(ImGuiKey_GamepadDpadLeft, kLeft);
	button(ImGuiKey_GamepadDpadRight, kRight);
	button(ImGuiKey_GamepadDpadUp, kUp);
	button(ImGuiKey_GamepadDpadDown, kDown);
	button(ImGuiKey_GamepadL1, kLB);
	button(ImGuiKey_GamepadR1, kRB);
	button(ImGuiKey_GamepadL3, kL3);
	button(ImGuiKey_GamepadR3, kR3);
	analog(ImGuiKey_GamepadL2, state.leftTrigger);
	analog(ImGuiKey_GamepadR2, state.rightTrigger);
	analog(ImGuiKey_GamepadLStickLeft, -state.leftX);
	analog(ImGuiKey_GamepadLStickRight, state.leftX);
	analog(ImGuiKey_GamepadLStickUp, state.leftY);
	analog(ImGuiKey_GamepadLStickDown, -state.leftY);
	analog(ImGuiKey_GamepadRStickLeft, -state.rightX);
	analog(ImGuiKey_GamepadRStickRight, state.rightX);
	analog(ImGuiKey_GamepadRStickUp, state.rightY);
	analog(ImGuiKey_GamepadRStickDown, -state.rightY);
}

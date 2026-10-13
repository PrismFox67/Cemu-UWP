#include "gui/host/HostControllers.h"
#include "gui/host/HostGamepad.h"

#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "input/api/XInput/XInputController.h"
#include "input/api/Keyboard/KeyboardController.h"
#include "input/emulated/VPADController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/ClassicController.h"

#include <Windows.h>

namespace
{
	struct
	{
		std::atomic_bool active = false;
		std::atomic_int nextPlayer = 0;
		bool disconnectUnpaired = true;
		uint32 lastButtons[HostGamepad::kMaxPads]{};
		bool padUsed[HostGamepad::kMaxPads]{};
		HostControllers::Kind previousKinds[HostControllers::kPlayerCount]{};
	} s_pairing;

	EmulatedController::Type _ToEmulatedType(HostControllers::Kind kind)
	{
		switch (kind)
		{
		case HostControllers::Kind::GamePad: return EmulatedController::Type::VPAD;
		case HostControllers::Kind::Classic: return EmulatedController::Type::Classic;
		default: return EmulatedController::Type::Pro;
		}
	}

	HostControllers::Kind _FromEmulatedType(EmulatedController::Type type)
	{
		switch (type)
		{
		case EmulatedController::Type::VPAD: return HostControllers::Kind::GamePad;
		case EmulatedController::Type::Pro: return HostControllers::Kind::Pro;
		case EmulatedController::Type::Classic: return HostControllers::Kind::Classic;
		default: return HostControllers::Kind::None; // Wii Remotes can only be set up by editing the profile
		}
	}

	// keyboard on the GamePad so the PC build can be tested without a controller
	void _AddKeyboard(const EmulatedControllerPtr& vpad)
	{
		auto keyboard = std::make_shared<KeyboardController>();
		vpad->add_controller(keyboard);
		const std::pair<VPADController::ButtonId, uint32> keyboardMapping[] = {
			{ VPADController::kButtonId_A, 'K' }, { VPADController::kButtonId_B, 'J' },
			{ VPADController::kButtonId_X, 'I' }, { VPADController::kButtonId_Y, 'U' },
			{ VPADController::kButtonId_L, 'Q' }, { VPADController::kButtonId_R, 'E' },
			{ VPADController::kButtonId_ZL, 'Z' }, { VPADController::kButtonId_ZR, 'C' },
			{ VPADController::kButtonId_Plus, VK_RETURN }, { VPADController::kButtonId_Minus, VK_BACK },
			{ VPADController::kButtonId_Home, 'H' },
			{ VPADController::kButtonId_Up, VK_UP }, { VPADController::kButtonId_Down, VK_DOWN },
			{ VPADController::kButtonId_Left, VK_LEFT }, { VPADController::kButtonId_Right, VK_RIGHT },
			{ VPADController::kButtonId_StickL_Up, 'W' }, { VPADController::kButtonId_StickL_Down, 'S' },
			{ VPADController::kButtonId_StickL_Left, 'A' }, { VPADController::kButtonId_StickL_Right, 'D' },
			{ VPADController::kButtonId_StickR_Up, VK_NUMPAD8 }, { VPADController::kButtonId_StickR_Down, VK_NUMPAD2 },
			{ VPADController::kButtonId_StickR_Left, VK_NUMPAD4 }, { VPADController::kButtonId_StickR_Right, VK_NUMPAD6 },
		};
		for (const auto& [button, key] : keyboardMapping)
			vpad->set_mapping(button, keyboard, key);
	}

	// Every emulated button is mapped to exactly one physical controller, so the keyboard mapping takes those buttons away
	// from the controller (on Xbox only the stick clicks were left). The keyboard is only for testing the PC build without
	// a controller
	bool _WantsKeyboard(int pad)
	{
#if defined(CEMU_UWP)
		return false;
#else
		return pad < 0 || !HostGamepad::PollPad(pad).connected;
#endif
	}

	// Cemu's default XInput mapping follows the button positions (Xbox B is where the Wii U's A is). On a console players
	// expect the labels to match, so map A/B/X/Y to the Xbox buttons with the same name. The button ids are the same for
	// the GamePad, Pro and Classic Controller (1-4), XInput reports A/B/X/Y as buttons 12-15
	void _UseLabelLayout(const EmulatedControllerPtr& emulated, const std::shared_ptr<ControllerBase>& xinput)
	{
		emulated->set_mapping(1, xinput, 12); // A
		emulated->set_mapping(2, xinput, 13); // B
		emulated->set_mapping(3, xinput, 14); // X
		emulated->set_mapping(4, xinput, 15); // Y
	}

	// Xbox buttons that can be mapped: HostGamepad flag for detecting the press, XInputController button id, display name
	struct XboxButton
	{
		uint32 hostButton;
		uint64 xinputButton;
		const char* name;
	};
	constexpr XboxButton kXboxButtons[] = {
		{ HostGamepad::kA, kButton12, "A" }, { HostGamepad::kB, kButton13, "B" },
		{ HostGamepad::kX, kButton14, "X" }, { HostGamepad::kY, kButton15, "Y" },
		{ HostGamepad::kLB, kButton8, "LB" }, { HostGamepad::kRB, kButton9, "RB" },
		{ HostGamepad::kLT, kTriggerXP, "LT" }, { HostGamepad::kRT, kTriggerYP, "RT" },
		{ HostGamepad::kStart, kButton4, "Menu" }, { HostGamepad::kBack, kButton5, "View" },
		{ HostGamepad::kL3, kButton6, "Left stick click" }, { HostGamepad::kR3, kButton7, "Right stick click" },
		{ HostGamepad::kUp, kButton0, "D-pad up" }, { HostGamepad::kDown, kButton1, "D-pad down" },
		{ HostGamepad::kLeft, kButton2, "D-pad left" }, { HostGamepad::kRight, kButton3, "D-pad right" },
	};

	std::shared_ptr<ControllerBase> _GetXInput(const EmulatedControllerPtr& emulated)
	{
		for (const auto& c : emulated->get_controllers())
		{
			if (c->api() == InputAPI::XInput)
				return c;
		}
		return {};
	}

	// the XInput button a Wii U button is mapped to. EmulatedController only exposes the button's name, so it's matched by name
	const XboxButton* _GetMappedXboxButton(const EmulatedControllerPtr& emulated, const std::shared_ptr<ControllerBase>& xinput, uint64 id)
	{
		if (emulated->get_mapping_controller(id) != xinput)
			return nullptr;
		const std::string name = emulated->get_mapping_name(id);
		for (const auto& b : kXboxButtons)
		{
			if (xinput->get_button_name(b.xinputButton) == name)
				return &b;
		}
		return nullptr;
	}

	struct
	{
		std::atomic_bool active = false;
		bool waitingForRelease = false;
		int player = -1;
		int pad = -1;
		uint64 id = 0;
		uint32 lastButtons = 0;
		std::chrono::steady_clock::time_point deadline;
	} s_capture;
	constexpr auto kCaptureTimeout = std::chrono::seconds(5);

	void _FinishPairing()
	{
		// players that didn't get a controller are disconnected, so the game doesn't see phantom controllers. Not while a
		// game runs: removing controllers changes the input topology (see SetPlayer)
		if (s_pairing.disconnectUnpaired)
		{
			for (int p = s_pairing.nextPlayer; p < HostControllers::kPlayerCount; p++)
				HostControllers::SetPlayer(p, HostControllers::Kind::None, -1);
		}
		s_pairing.active = false;
		cemuLog_log(LogType::Force, "Host: controller pairing finished with {} player(s)", s_pairing.nextPlayer.load());
	}
}

const char* HostControllers::KindName(Kind kind)
{
	switch (kind)
	{
	case Kind::GamePad: return "Wii U GamePad";
	case Kind::Pro: return "Pro Controller";
	case Kind::Classic: return "Classic Controller";
	default: return "Not connected";
	}
}

bool HostControllers::IsKindAllowed(int player, Kind kind)
{
	if (kind == Kind::GamePad)
		return player < (int)InputManager::kMaxVPADControllers;
	return kind < Kind::Count;
}

void HostControllers::SetupDefaults()
{
	auto& input = InputManager::instance();
	// players 2-4 are only created once, so a player removed by the user stays removed
	std::error_code ec;
	const fs::path marker = ActiveSettings::GetConfigPath("controllerProfiles/.hostDefaults");
	const bool createOtherPlayers = !fs::exists(marker, ec);
	for (int player = 0; player < kPlayerCount; player++)
	{
		if (input.get_controller(player) || (player > 0 && !createOtherPlayers))
			continue;
		SetPlayer(player, player == 0 ? Kind::GamePad : Kind::Pro, player);
	}
	if (createOtherPlayers)
		std::ofstream(marker) << "default controller profiles were created\n";
	// earlier builds used the positional layout (Xbox B = Wii U A), switch those profiles to matching labels
	for (int player = 0; player < kPlayerCount; player++)
	{
		const auto emulated = input.get_controller(player);
		if (!emulated)
			continue;
		for (const auto& c : emulated->get_controllers())
		{
			if (c->api() == InputAPI::XInput && emulated->get_mapping_controller(1) == c && emulated->get_mapping_name(1) == "B")
			{
				_UseLabelLayout(emulated, c);
				input.save(player);
				cemuLog_log(LogType::Force, "Host: player {} now uses the Xbox button labels for A/B/X/Y", player + 1);
				break;
			}
		}
	}
	// earlier builds always added the keyboard to player 1, which left the controller without its buttons
	const PlayerInfo player1 = GetPlayer(0);
	if (player1.kind == Kind::GamePad && player1.hasKeyboard && player1.pad >= 0 && !_WantsKeyboard(player1.pad))
	{
		SetPlayer(0, Kind::GamePad, player1.pad);
		cemuLog_log(LogType::Force, "Host: removed the keyboard from player 1, it overrode the controller's buttons");
	}
	cemuLog_log(LogType::Force, "Host: controllers: {} / {} / {} / {}", KindName(GetPlayer(0).kind), KindName(GetPlayer(1).kind), KindName(GetPlayer(2).kind), KindName(GetPlayer(3).kind));
}

HostControllers::PlayerInfo HostControllers::GetPlayer(int player)
{
	PlayerInfo info;
	const auto emulated = InputManager::instance().get_controller(player);
	if (!emulated)
		return info;
	info.kind = _FromEmulatedType(emulated->type());
	for (const auto& c : emulated->get_controllers())
	{
		if (c->api() == InputAPI::XInput && info.pad < 0)
		{
			info.pad = std::atoi(c->uuid().c_str());
			info.deadzone = c->get_settings().axis.deadzone;
			info.range = c->get_settings().axis.range;
		}
		else if (c->api() == InputAPI::Keyboard)
			info.hasKeyboard = true;
	}
	if (info.pad >= 0)
		info.padConnected = HostGamepad::PollPad(info.pad).connected;
	return info;
}

void HostControllers::SetPlayer(int player, Kind kind, int pad)
{
	if (player < 0 || player >= kPlayerCount)
		return;
	auto& input = InputManager::instance();
	try
	{
		if (kind == Kind::None)
		{
			input.delete_controller(player, true);
			return;
		}
		if (!IsKindAllowed(player, kind))
			kind = Kind::Pro;
		// Only replace the emulated controller if its type changes. Recreating it changes the input topology the game
		// sees, which another Xbox UWP Cemu port found can terminate the process while a title is running
		auto emulated = input.get_controller(player);
		// a custom button mapping is kept when the player gets another controller (pairing) but the same controller type
		std::vector<std::pair<uint64, const XboxButton*>> customMapping;
		if (emulated && emulated->type() == _ToEmulatedType(kind))
		{
			if (const auto oldXInput = _GetXInput(emulated))
			{
				for (const auto& button : GetMappableButtons(kind))
					customMapping.emplace_back(button.id, _GetMappedXboxButton(emulated, oldXInput, button.id));
			}
		}
		if (!emulated || emulated->type() != _ToEmulatedType(kind))
			emulated = input.set_controller(player, _ToEmulatedType(kind));
		if (!emulated)
			return;
		emulated->clear_controllers(); // set_controller carries over the controllers of the previous profile
		if (pad >= 0 && pad < HostGamepad::kMaxPads)
		{
			auto xinput = std::make_shared<XInputController>((uint32)pad);
			emulated->add_controller(xinput);
			emulated->set_default_mapping(xinput);
			_UseLabelLayout(emulated, xinput);
			for (const auto& [id, xbox] : customMapping)
			{
				if (xbox)
					emulated->set_mapping(id, xinput, xbox->xinputButton);
				else
					emulated->delete_mapping(id);
			}
		}
		if (player == 0 && kind == Kind::GamePad && _WantsKeyboard(pad))
			_AddKeyboard(emulated);
		input.save(player);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Host: failed to set up controller for player {}: {}", player + 1, ex.what());
	}
}

void HostControllers::SetDeadzone(int player, float deadzone)
{
	auto emulated = InputManager::instance().get_controller(player);
	if (!emulated)
		return;
	deadzone = std::clamp(deadzone, 0.0f, 0.9f);
	for (const auto& c : emulated->get_controllers())
	{
		if (c->api() != InputAPI::XInput)
			continue;
		auto settings = c->get_settings();
		settings.axis.deadzone = settings.rotation.deadzone = deadzone;
		c->set_settings(settings);
	}
	InputManager::instance().save(player);
}

void HostControllers::SetStickRange(int player, float range)
{
	auto emulated = InputManager::instance().get_controller(player);
	if (!emulated)
		return;
	range = std::clamp(range, 0.5f, 2.0f);
	for (const auto& c : emulated->get_controllers())
	{
		if (c->api() != InputAPI::XInput)
			continue;
		auto settings = c->get_settings();
		settings.axis.range = settings.rotation.range = range;
		c->set_settings(settings);
	}
	InputManager::instance().save(player);
}

std::vector<HostControllers::MappableButton> HostControllers::GetMappableButtons(Kind kind)
{
	// the button ids differ between the controller types (the GamePad's Home comes after Mic and Screen)
	auto buttons = [](auto a, auto b, auto x, auto y, auto l, auto r, auto zl, auto zr, auto plus, auto minus, auto home,
					  auto up, auto down, auto left, auto right) {
		return std::vector<MappableButton>{
			{ "A", (uint64)a }, { "B", (uint64)b }, { "X", (uint64)x }, { "Y", (uint64)y },
			{ "L", (uint64)l }, { "R", (uint64)r }, { "ZL", (uint64)zl }, { "ZR", (uint64)zr },
			{ "+ (Plus)", (uint64)plus }, { "- (Minus)", (uint64)minus }, { "Home", (uint64)home },
			{ "D-pad up", (uint64)up }, { "D-pad down", (uint64)down }, { "D-pad left", (uint64)left }, { "D-pad right", (uint64)right },
		};
	};
	std::vector<MappableButton> result;
	switch (kind)
	{
	case Kind::GamePad:
	{
		using C = VPADController;
		result = buttons(C::kButtonId_A, C::kButtonId_B, C::kButtonId_X, C::kButtonId_Y, C::kButtonId_L, C::kButtonId_R, C::kButtonId_ZL, C::kButtonId_ZR,
			C::kButtonId_Plus, C::kButtonId_Minus, C::kButtonId_Home, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
		result.push_back({ "Left stick click", C::kButtonId_StickL });
		result.push_back({ "Right stick click", C::kButtonId_StickR });
		break;
	}
	case Kind::Pro:
	{
		using C = ProController;
		result = buttons(C::kButtonId_A, C::kButtonId_B, C::kButtonId_X, C::kButtonId_Y, C::kButtonId_L, C::kButtonId_R, C::kButtonId_ZL, C::kButtonId_ZR,
			C::kButtonId_Plus, C::kButtonId_Minus, C::kButtonId_Home, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
		result.push_back({ "Left stick click", C::kButtonId_StickL });
		result.push_back({ "Right stick click", C::kButtonId_StickR });
		break;
	}
	case Kind::Classic:
	{
		using C = ClassicController;
		result = buttons(C::kButtonId_A, C::kButtonId_B, C::kButtonId_X, C::kButtonId_Y, C::kButtonId_L, C::kButtonId_R, C::kButtonId_ZL, C::kButtonId_ZR,
			C::kButtonId_Plus, C::kButtonId_Minus, C::kButtonId_Home, C::kButtonId_Up, C::kButtonId_Down, C::kButtonId_Left, C::kButtonId_Right);
		break;
	}
	default:
		break;
	}
	return result;
}

std::string HostControllers::GetButtonMappingName(int player, uint64 id)
{
	const auto emulated = InputManager::instance().get_controller(player);
	if (!emulated)
		return {};
	const auto xinput = _GetXInput(emulated);
	if (!xinput)
		return {};
	const XboxButton* xbox = _GetMappedXboxButton(emulated, xinput, id);
	return xbox ? xbox->name : std::string();
}

void HostControllers::ResetButtonMapping(int player)
{
	const auto emulated = InputManager::instance().get_controller(player);
	if (!emulated)
		return;
	const auto xinput = _GetXInput(emulated);
	if (!xinput)
		return;
	for (const auto& button : GetMappableButtons(_FromEmulatedType(emulated->type())))
		emulated->delete_mapping(button.id);
	emulated->set_default_mapping(xinput);
	_UseLabelLayout(emulated, xinput);
	InputManager::instance().save(player);
	cemuLog_log(LogType::Force, "Host: player {} button mapping reset to the default", player + 1);
}

void HostControllers::BeginCapture(int player, uint64 id)
{
	const PlayerInfo info = GetPlayer(player);
	if (info.pad < 0 || s_capture.active)
		return;
	s_capture.player = player;
	s_capture.pad = info.pad;
	s_capture.id = id;
	// the A that selected the button doesn't count
	s_capture.lastButtons = HostGamepad::PollPad(info.pad).buttons;
	s_capture.waitingForRelease = false;
	s_capture.deadline = std::chrono::steady_clock::now() + kCaptureTimeout;
	s_capture.active = true;
}

bool HostControllers::IsCapturing()
{
	return s_capture.active;
}

int HostControllers::GetCapturePlayer()
{
	return s_capture.player;
}

uint64 HostControllers::GetCaptureButton()
{
	return s_capture.id;
}

float HostControllers::GetCaptureSecondsLeft()
{
	return std::max(0.0f, std::chrono::duration<float>(s_capture.deadline - std::chrono::steady_clock::now()).count());
}

void HostControllers::UpdateCapture()
{
	if (!s_capture.active)
		return;
	const HostGamepad::State pad = HostGamepad::PollPad(s_capture.pad);
	if (s_capture.waitingForRelease)
	{
		if (pad.buttons == 0 || !pad.connected)
			s_capture.active = false;
		return;
	}
	const uint32 pressed = pad.buttons & ~s_capture.lastButtons;
	s_capture.lastButtons = pad.buttons;
	if (!pad.connected || std::chrono::steady_clock::now() > s_capture.deadline)
	{
		s_capture.waitingForRelease = true;
		return;
	}
	const XboxButton* xbox = nullptr;
	for (const auto& b : kXboxButtons)
	{
		if (pressed & b.hostButton)
		{
			xbox = &b;
			break;
		}
	}
	if (!xbox)
		return;
	s_capture.waitingForRelease = true;

	const int player = s_capture.player;
	const auto emulated = InputManager::instance().get_controller(player);
	if (!emulated)
		return;
	const auto xinput = _GetXInput(emulated);
	if (!xinput)
		return;
	// the Wii U button that used this Xbox button gets the one the changed button had, so every button stays reachable
	const XboxButton* previous = _GetMappedXboxButton(emulated, xinput, s_capture.id);
	for (const auto& button : GetMappableButtons(_FromEmulatedType(emulated->type())))
	{
		if (button.id == s_capture.id || _GetMappedXboxButton(emulated, xinput, button.id) != xbox)
			continue;
		if (previous)
			emulated->set_mapping(button.id, xinput, previous->xinputButton);
		else
			emulated->delete_mapping(button.id);
	}
	emulated->set_mapping(s_capture.id, xinput, xbox->xinputButton);
	InputManager::instance().save(player);
	cemuLog_log(LogType::Force, "Host: player {} mapped Xbox {} to Wii U button {}", player + 1, xbox->name, s_capture.id);
}

void HostControllers::BeginPairing(bool gameRunning)
{
	s_pairing.disconnectUnpaired = !gameRunning;
	for (int p = 0; p < kPlayerCount; p++)
		s_pairing.previousKinds[p] = GetPlayer(p).kind;
	for (int i = 0; i < HostGamepad::kMaxPads; i++)
	{
		// buttons held when pairing starts (the A that selected the menu entry) don't count
		s_pairing.lastButtons[i] = HostGamepad::PollPad(i).buttons;
		s_pairing.padUsed[i] = false;
	}
	s_pairing.nextPlayer = 0;
	s_pairing.active = true;
}

void HostControllers::CancelPairing()
{
	if (!s_pairing.active)
		return;
	// keep what was paired so far, the remaining players keep their old setup
	s_pairing.active = false;
}

bool HostControllers::IsPairing()
{
	return s_pairing.active;
}

int HostControllers::GetPairingPlayer()
{
	return s_pairing.nextPlayer;
}

void HostControllers::UpdatePairing()
{
	if (!s_pairing.active)
		return;
	for (int i = 0; i < HostGamepad::kMaxPads; i++)
	{
		const HostGamepad::State pad = HostGamepad::PollPad(i);
		const uint32 pressed = pad.buttons & ~s_pairing.lastButtons[i];
		s_pairing.lastButtons[i] = pad.buttons;
		if (!pad.connected)
			continue;
		if ((pressed & HostGamepad::kStart) && s_pairing.nextPlayer > 0)
		{
			_FinishPairing();
			return;
		}
		if ((pressed & HostGamepad::kA) && !s_pairing.padUsed[i])
		{
			const int player = s_pairing.nextPlayer;
			Kind kind = s_pairing.previousKinds[player];
			if (kind == Kind::None || !IsKindAllowed(player, kind))
				kind = player == 0 ? Kind::GamePad : Kind::Pro;
			SetPlayer(player, kind, i);
			s_pairing.padUsed[i] = true;
			s_pairing.nextPlayer = player + 1;
			if (s_pairing.nextPlayer >= kPlayerCount)
			{
				_FinishPairing();
				return;
			}
		}
	}
}

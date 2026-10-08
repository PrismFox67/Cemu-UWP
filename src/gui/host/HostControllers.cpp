#include "gui/host/HostControllers.h"
#include "gui/host/HostGamepad.h"

#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "input/api/XInput/XInputController.h"
#include "input/api/Keyboard/KeyboardController.h"
#include "input/emulated/VPADController.h"

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
			info.pad = std::atoi(c->uuid().c_str());
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
		}
		if (player == 0 && kind == Kind::GamePad)
			_AddKeyboard(emulated);
		input.save(player);
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Host: failed to set up controller for player {}: {}", player + 1, ex.what());
	}
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

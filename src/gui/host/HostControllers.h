#pragma once

// Player slots of the host frontend: which emulated controller (GamePad, Pro Controller, ...) each player uses and which
// physical controller drives it. Stored as regular Cemu controller profiles (controllerProfiles/controller<N>.xml).
//
// Pairing works like the controller order screen of a console: "press A on the controller for player 1", then player 2
// and so on. Press Menu (Start) to finish, players without a controller are disconnected.
namespace HostControllers
{
	constexpr int kPlayerCount = 4;

	enum class Kind
	{
		None,
		GamePad, // Wii U GamePad (players 1 and 2 only, the Wii U supports two)
		Pro, // Wii U Pro Controller
		Classic, // Classic Controller
		Count,
	};

	struct PlayerInfo
	{
		Kind kind = Kind::None;
		int pad = -1; // XInput index, -1 if no controller is assigned
		bool padConnected = false;
		bool hasKeyboard = false;
	};

	const char* KindName(Kind kind);
	bool IsKindAllowed(int player, Kind kind);

	// creates the default profiles: player 1 GamePad on controller 1 + keyboard, players 2-4 Pro Controllers on
	// controllers 2-4. Existing profiles are kept
	void SetupDefaults();

	PlayerInfo GetPlayer(int player);
	void SetPlayer(int player, Kind kind, int pad);

	// pairing. UpdatePairing() polls the controllers and has to be called every frame from the UI thread while pairing
	void BeginPairing();
	void CancelPairing();
	bool IsPairing();
	int GetPairingPlayer(); // player waiting for a controller
	void UpdatePairing();
}

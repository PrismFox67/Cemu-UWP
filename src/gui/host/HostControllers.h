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
		float deadzone = 0.15f; // stick deadzone of the controller (0..1)
		float range = 1.0f; // stick range, above 1 reaches full tilt before the stick is all the way out
	};

	const char* KindName(Kind kind);
	bool IsKindAllowed(int player, Kind kind);

	// creates the default profiles: player 1 GamePad on controller 1 + keyboard, players 2-4 Pro Controllers on
	// controllers 2-4. Existing profiles are kept
	void SetupDefaults();

	PlayerInfo GetPlayer(int player);
	void SetPlayer(int player, Kind kind, int pad);
	// deadzone of both sticks, raise it for controllers with stick drift
	void SetDeadzone(int player, float deadzone);
	// stick range of both sticks, raise it for worn sticks that no longer reach the edge
	void SetStickRange(int player, float range);

	// button mapping: which Xbox button drives each Wii U button. Stick directions stay on the sticks
	struct MappableButton
	{
		const char* name;
		uint64 id; // emulated controller mapping id
	};
	std::vector<MappableButton> GetMappableButtons(Kind kind);
	std::string GetButtonMappingName(int player, uint64 id); // Xbox button name, empty if unmapped
	void ResetButtonMapping(int player);

	// capture: the next button pressed on the player's controller is mapped to the Wii U button. Swaps with the Wii U
	// button that had it before. Cancelled after a few seconds without a press. UpdateCapture() has to be called every
	// frame like UpdatePairing(); IsCapturing() stays true until the pressed button is released so the UI doesn't see it
	void BeginCapture(int player, uint64 id);
	bool IsCapturing();
	int GetCapturePlayer();
	uint64 GetCaptureButton();
	float GetCaptureSecondsLeft();
	void UpdateCapture();

	// pairing. UpdatePairing() polls the controllers and has to be called every frame from the UI thread while pairing
	// in game, players without a controller keep their setup instead of being disconnected
	void BeginPairing(bool gameRunning);
	void CancelPairing();
	bool IsPairing();
	int GetPairingPlayer(); // player waiting for a controller
	void UpdatePairing();
}

#pragma once

// Tester helpers of the host frontend: noticing that the last session ended unexpectedly, copying the logs and the save
// data to the game drive, and checking GitHub for a newer build.

namespace HostTools
{
	// --- last session ---
	// a marker file exists while a game runs. If it's still there at startup, the app was closed with a game running
	void MarkGameStarted(const std::string& gameName);
	void MarkGameStopped();
	// the console suspended / resumed the app (it may terminate a suspended app without notice, that's not a crash)
	void MarkSuspended(bool suspended);

	struct LastSessionProblem
	{
		std::string title;
		std::string text;
	};
	// checks the marker and log_previous.txt. Call once at startup, after the log file was created (which moves the
	// previous session's log to log_previous.txt)
	std::optional<LastSessionProblem> CheckLastSession();

	// --- copies to the game drive ---
	// the folder on the drive of the first game folder that isn't on the app's own storage, empty if there's none
	fs::path GetExportRoot();
	// both run on a worker thread, the result is shown with HostUI::PushMessage
	void StartSaveLogs();
	void StartBackupSaves();
	bool IsBusy();
	std::string GetBusyText();

	// --- newer build ---
	// asks GitHub on a worker thread whether the ci-latest release is newer than this build
	void StartUpdateCheck();
	// the newer build's commit and title, empty if there's none (or the check didn't finish)
	std::string GetNewerBuild();
	const char* GetDownloadUrl();
}

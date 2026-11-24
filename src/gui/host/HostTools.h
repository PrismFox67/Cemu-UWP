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
	// GitHub #6: makes <folder> Cemu's mlc01 (saves, installed updates/DLC) from the next start, e.g. on the USB drive so
	// saves survive a Dev Mode reset and are shared by all Xbox profiles. Copies the current saves there first (never
	// moves or deletes them, existing files at the target are kept). Empty folder = back to the app's own storage
	void StartSetSaveFolder(fs::path folder);
	// set at start when the save folder's drive is missing and the app's own mlc01 is used instead
	bool IsSaveFolderMissing();
	void SetSaveFolderMissing();
	// #849: makes <folder> the Cemu data folder (shaderCache, graphicPacks, theme, dimensions, skylanders, screenshots) from
	// the next start. Copies the current ones there first, existing files at the target are kept, nothing is deleted.
	// Empty folder = back to the app's own storage
	void StartSetDataFolder(fs::path folder);
	bool IsDataFolderMissing();
	void SetDataFolderMissing();
	// #925 save safety net: before each launch the game's save (usr/save/<high>/<low>) is copied to
	// <save folder>/../CemuBackups/auto/<title id>/<time>, the last 3 are kept, saves over 64 MB are skipped
	void BackupTitleSave(uint64 titleId);
	struct SaveBackup
	{
		uint64 titleId;
		std::string newest; // folder name (time) of the newest backup
		size_t count;
	};
	std::vector<SaveBackup> ListSaveBackups();
	// replaces the game's save with its newest backup (the current save is kept as another backup first)
	void StartRestoreSaveBackup(uint64 titleId);
	// #938 save manager. Export copies the game's save to <USB>/CemuSaves/<game name> (<title id>)/ (an earlier export
	// there is kept as '... previous'). Import lists those folders, mlc01-style usr/save/<high>/<low> folders on the drive
	// (desktop Cemu, Save backups to USB) and replaces the save with one of them; the current save is backed up first
	// (CemuBackups/auto, 'replaced-'), the import is refused if that backup fails
	bool HasTitleSave(uint64 titleId);
	void StartExportSave(uint64 titleId, std::string gameName);
	struct ImportableSave
	{
		std::string label; // for the UI, relative to the drive
		fs::path path; // the folder with user/ and meta/
	};
	std::vector<ImportableSave> ListImportableSaves(uint64 titleId);
	void StartImportSave(uint64 titleId, fs::path from);
	bool IsBusy();
	std::string GetBusyText();

	// --- newer build ---
	// asks GitHub on a worker thread whether the ci-latest release is newer than this build
	void StartUpdateCheck();
	// the newer build's commit and title, empty if there's none (or the check didn't finish)
	std::string GetNewerBuild();
	const char* GetDownloadUrl();
	// #947 EXPERIMENTAL in-app update: downloads cemu-uwp-x64.zip of ci-latest, checks size and SHA-256, unpacks it and
	// asks the platform to install the msix as an update (the installer closes the app). If that is refused the package
	// stays on the USB drive / in TempState and a message explains the Device Portal steps
	void StartUpdate();
}

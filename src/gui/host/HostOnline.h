#pragma once

// #956 online play (Pretendo) for the Xbox frontend. The core already has upstream Cemu's online support; this is the
// setup around it: which of the files dumped from the user's own Wii U are there, copying them from the USB drive
// (<USB>/CemuOnline/), the account and the network service. Never logs or shows the contents of those files.

namespace HostOnline
{
	struct Status
	{
		std::string otp; // "OK", "missing", "wrong size"
		std::string seeprom;
		size_t missingCertificates = 0; // files of sys/title/0005001b/10054000/content
		std::string otpPath; // where Cemu looks, for the UI
		std::string mlcPath;
	};
	Status GetStatus();
	bool IsReady(const Status& s);

	struct AccountInfo
	{
		uint32 persistentId;
		std::string miiName;
		bool onlineAccount; // has a linked NNID/PNID (account id + password cache)
	};
	std::vector<AccountInfo> GetAccounts(); // re-reads the account files
	uint32 GetActiveAccount();
	void SetActiveAccount(uint32 persistentId);

	// 0 Offline, 2 Pretendo, 3 Custom (Nintendo's servers are gone, not offered)
	int GetService();
	void SetService(int service); // for the active account; also turns online mode on/off
	std::string GetCustomUrl(); // network_services.xml, written by the user; empty if there is none

	// <USB>/CemuOnline/{otp.bin, seeprom.bin, mlc01/usr/save/system/act/<id>/, mlc01/sys/title/0005001b/10054000/}
	fs::path GetImportFolder();
	// copies what is there; returns a summary for a message ("copied otp.bin, seeprom.bin, 1 account, 30 certificate files")
	std::string ImportFromUsb();
}

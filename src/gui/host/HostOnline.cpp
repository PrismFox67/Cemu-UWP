#include "gui/host/HostOnline.h"
#include "gui/host/HostTools.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "Cafe/Account/Account.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"

#include <boost/nowide/convert.hpp>

// #956, see the header. Only file names, sizes and counts are logged, never contents

namespace HostOnline
{
	std::string _FileState(const fs::path& path, uintmax_t size)
	{
		std::error_code ec;
		if (!fs::is_regular_file(path, ec))
			return "missing";
		return fs::file_size(path, ec) == size ? "OK" : "wrong size";
	}

	fs::path _CertFolder()
	{
		return ActiveSettings::GetMlcPath("sys/title/0005001b/10054000/content");
	}

	Status GetStatus()
	{
		Status s;
		s.otp = _FileState(ActiveSettings::GetUserDataPath("otp.bin"), 1024);
		s.seeprom = _FileState(ActiveSettings::GetUserDataPath("seeprom.bin"), 512);
		s.otpPath = _pathToUtf8(ActiveSettings::GetUserDataPath());
		s.mlcPath = _pathToUtf8(ActiveSettings::GetMlcPath());
		std::error_code ec;
		for (const auto& list : { iosuCrypt_getCertificateKeys(), iosuCrypt_getCertificateNames() })
			for (const wchar_t* name : list)
				if (name && *name && !fs::is_regular_file(_CertFolder() / name, ec))
					s.missingCertificates++;
		return s;
	}

	bool IsReady(const Status& s)
	{
		return s.otp == "OK" && s.seeprom == "OK" && s.missingCertificates == 0;
	}

	std::vector<AccountInfo> GetAccounts()
	{
		std::vector<AccountInfo> out;
		for (const Account& a : Account::RefreshAccounts())
			out.push_back({ a.GetPersistentId(), boost::nowide::narrow(std::wstring(a.GetMiiName())), a.IsValidOnlineAccount() });
		return out;
	}

	uint32 GetActiveAccount()
	{
		return GetConfig().account.m_persistent_id.GetValue();
	}

	void SetActiveAccount(uint32 persistentId)
	{
		GetConfig().account.m_persistent_id = persistentId;
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Online: active account {:08x}", persistentId);
	}

	int GetService()
	{
		return (int)GetConfig().GetAccountNetworkService(GetActiveAccount());
	}

	void SetService(int service)
	{
		if (service != (int)NetworkService::Pretendo && service != (int)NetworkService::Custom)
			service = (int)NetworkService::Offline;
		auto& config = GetConfig();
		config.SetAccountSelectedService(GetActiveAccount(), (NetworkService)service);
		config.account.legacy_online_enabled = service != (int)NetworkService::Offline;
		GetConfigHandle().Save();
		const char* names[] = { "Offline", "Nintendo", "Pretendo", "Custom" };
		cemuLog_log(LogType::Force, "Online: network service {} for account {:08x}", names[service], GetActiveAccount());
	}

	std::string GetCustomUrl()
	{
		if (!NetworkConfig::XMLExists())
			return {};
		NetworkConfig::LoadOnce();
		return GetNetworkConfig().urls.ACT.GetValue();
	}

	fs::path GetImportFolder()
	{
		const fs::path root = HostTools::GetExportRoot();
		return root.empty() ? fs::path() : root / "CemuOnline";
	}

	std::string ImportFromUsb()
	{
		const fs::path from = GetImportFolder();
		std::error_code ec;
		if (from.empty() || !fs::is_directory(from, ec))
		{
			cemuLog_log(LogType::Force, "Online: no import folder ({})", from.empty() ? "no USB drive" : _pathToUtf8(from));
			return fmt::format("No CemuOnline folder found{}.", from.empty() ? " (no USB drive)" : " at " + _pathToUtf8(from));
		}
		std::vector<std::string> done, failed;
		// otp.bin / seeprom.bin, only if the size is right
		for (const auto& [name, size] : { std::pair<const char*, uintmax_t>{ "otp.bin", 1024 }, { "seeprom.bin", 512 } })
		{
			const fs::path src = from / name;
			if (!fs::is_regular_file(src, ec))
				continue;
			if (fs::file_size(src, ec) != size)
			{
				failed.push_back(fmt::format("{} (wrong size)", name));
				continue;
			}
			fs::copy_file(src, ActiveSettings::GetUserDataPath(name), fs::copy_options::overwrite_existing, ec);
			(ec ? failed : done).push_back(name);
		}
		// accounts: an account already there is moved to <user>/online_backup/<time>/ first
		const fs::path actFrom = from / "mlc01/usr/save/system/act";
		const fs::path actTo = ActiveSettings::GetMlcPath("usr/save/system/act");
		size_t accounts = 0;
		std::string stamp;
		{
			const std::time_t now = std::time(nullptr);
			std::tm tm{};
			localtime_s(&tm, &now);
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
			stamp = buf;
		}
		for (auto it = fs::directory_iterator(actFrom, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			const std::string id = _pathToUtf8(it->path().filename());
			std::error_code ec2;
			if (id.size() != 8 || id.rfind("8", 0) != 0 || !fs::is_regular_file(it->path() / "account.dat", ec2))
				continue;
			const fs::path to = actTo / id;
			if (fs::exists(to, ec2))
			{
				const fs::path backup = ActiveSettings::GetUserDataPath("online_backup") / stamp / id;
				fs::create_directories(backup.parent_path(), ec2);
				fs::rename(to, backup, ec2);
				if (ec2)
				{
					failed.push_back(fmt::format("account {} (the one already there couldn't be backed up)", id));
					continue;
				}
			}
			fs::create_directories(to, ec2);
			fs::copy(it->path(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec2);
			if (ec2)
				failed.push_back("account " + id);
			else
				accounts++;
		}
		ec.clear();
		if (accounts)
		{
			done.push_back(fmt::format("{} account{}", accounts, accounts == 1 ? "" : "s"));
			Account::RefreshAccounts();
			Account::UpdatePersisidDat();
		}
		// certificates (sys/title/0005001b/10054000), copied whole
		const fs::path certFrom = from / "mlc01/sys/title/0005001b/10054000";
		if (fs::is_directory(certFrom, ec))
		{
			const fs::path to = ActiveSettings::GetMlcPath("sys/title/0005001b/10054000");
			fs::create_directories(to, ec);
			fs::copy(certFrom, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
			(ec ? failed : done).push_back("certificate title 0005001b-10054000");
		}
		std::string summary = done.empty() ? "Nothing copied." : "Copied: " + fmt::format("{}", fmt::join(done, ", ")) + ".";
		if (!failed.empty())
			summary += " Failed: " + fmt::format("{}", fmt::join(failed, ", ")) + ".";
		cemuLog_log(LogType::Force, "Online: import from {}: {}", _pathToUtf8(from), summary);
		const Status s = GetStatus();
		cemuLog_log(LogType::Force, "Online: otp.bin {}, seeprom.bin {}, {} certificate files missing", s.otp, s.seeprom, s.missingCertificates);
		return summary;
	}
}

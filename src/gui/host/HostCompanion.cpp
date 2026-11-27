#include "gui/host/HostCompanion.h"
#include "gui/host/HostIcons.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/crypto/crc32.h"
#include "util/helpers/helpers.h"

#include <boost/asio.hpp>

// #954, see the header. Wire format (little endian), the DSU header:
//   0 magic "DSUC" (us) / "DSUS" (app), 4 u16 protocol 1001, 6 u16 payload size + 4, 8 u32 CRC32 of the packet with
//   this field 0, 12 u32 sender id, 16 u32 message type, 20 payload

namespace HostCompanion
{
	namespace asio = boost::asio;
	using asio::ip::tcp;
	using asio::ip::udp;

	constexpr size_t kHeader = 20;
	constexpr uint32 kSenderId = 0xCEE00001u;
	constexpr uint32 kMsgDsuData = 0x100002;
	constexpr uint32 kMsgKeyboard = 0x120000;
	constexpr uint32 kMsgState = 0x140000;
	constexpr uint32 kMsgLaunch = 0x180004;
	constexpr uint32 kMsgLaunchStatus = 0x180005;
	constexpr uint32 kMsgSelection = 0x180006;
	// state, remote commands, launch by id, catalogue, catalogue media, app selection (no streaming)
	constexpr uint32 kCapabilities = (1u << 0) | (1u << 6) | (1u << 9) | (1u << 13) | (1u << 14) | (1u << 15);
	constexpr const char* kPlatform = "Nintendo - Wii U";
	constexpr const char* kAppName = "Cemu";
	enum Media : uint32 { kMarquee = 0, kCover = 1 };
	enum LaunchStatus : uint32 { kAccepted = 0, kStale = 1, kUnknown = 2, kBusy = 4 };

	std::mutex s_mutex; // everything below
	std::vector<Entry> s_entries;
	uint32 s_revision = 1;
	uint64 s_selection = 0;
	bool s_selectionChanged = false;
	std::optional<uint64> s_pendingLaunch;
	std::optional<uint64> s_pendingSelection;
	std::array<uint32, 11> s_keys{};
	uint32 s_navPressed = 0;
	std::string s_status;
	std::chrono::steady_clock::time_point s_lastHeard{};

	std::atomic_bool s_running = false;
	std::unique_ptr<asio::io_context> s_io;
	std::unique_ptr<udp::socket> s_udp;
	udp::endpoint s_phone;
	std::unique_ptr<tcp::acceptor> s_acceptor;
	std::thread s_udpThread, s_httpThread;

	void _Put16(uint8* p, uint16 v) { memcpy(p, &v, 2); }
	void _Put32(uint8* p, uint32 v) { memcpy(p, &v, 4); }
	void _Put64(uint8* p, uint64 v) { memcpy(p, &v, 8); }
	uint16 _Get16(const uint8* p) { uint16 v; memcpy(&v, p, 2); return v; }
	uint32 _Get32(const uint8* p) { uint32 v; memcpy(&v, p, 4); return v; }
	uint64 _Get64(const uint8* p) { uint64 v; memcpy(&v, p, 8); return v; }

	std::vector<uint8> _Packet(uint32 type, size_t payloadSize)
	{
		std::vector<uint8> p(kHeader + payloadSize, 0);
		memcpy(p.data(), "DSUC", 4);
		_Put16(p.data() + 4, 1001);
		_Put16(p.data() + 6, (uint16)(payloadSize + 4));
		_Put32(p.data() + 12, kSenderId);
		_Put32(p.data() + 16, type);
		return p;
	}

	void _Finish(std::vector<uint8>& p)
	{
		_Put32(p.data() + 8, 0);
		_Put32(p.data() + 8, crc32_calc(p.data(), p.size()));
	}

	void _PutStr(uint8* dst, size_t capacity, std::string_view s)
	{
		memcpy(dst, s.data(), std::min(s.size(), capacity - 1));
	}

	std::vector<uint8> _LaunchStatus(uint32 status, uint32 revision, uint64 token)
	{
		auto p = _Packet(kMsgLaunchStatus, 144);
		_Put32(p.data() + kHeader, status);
		_Put32(p.data() + kHeader + 4, revision);
		_Put64(p.data() + kHeader + 8, token);
		const char* msg = status == kAccepted ? "Launch accepted" : status == kStale ? "Library changed; refresh required"
			: status == kUnknown ? "Unknown library entry" : "A game is running or another launch is pending";
		_PutStr(p.data() + kHeader + 16, 128, msg);
		_Finish(p);
		return p;
	}

	std::vector<uint8> _Selection(uint32 revision, uint64 token)
	{
		auto p = _Packet(kMsgSelection, 12);
		_Put32(p.data() + kHeader, revision);
		_Put64(p.data() + kHeader + 4, token);
		_Finish(p);
		return p;
	}

	std::vector<uint8> BuildStatePacket()
	{
		const std::string title = CafeSystem::IsTitleRunning() ? CafeSystem::GetForegroundTitleName() : std::string();
		auto p = _Packet(kMsgState, 1 + 64 * 4 + 4);
		uint8* d = p.data() + kHeader;
		d[0] = title.empty() ? 0 : 1;
		_PutStr(d + 1, 64, title);
		_PutStr(d + 65, 64, kPlatform);
		_PutStr(d + 129, 64, kAppName);
		_PutStr(d + 193, 64, kAppName);
		_Put32(d + 257, kCapabilities);
		_Finish(p);
		return p;
	}

	// DSU "send pad data for all pads": keeps us registered as a client, so the app talks to this socket
	std::vector<uint8> _DsuRegister()
	{
		auto p = _Packet(kMsgDsuData, 8);
		_Finish(p);
		return p;
	}

	uint32 GetRevision()
	{
		std::lock_guard lock(s_mutex);
		return s_revision;
	}

	void SetCatalogue(std::vector<Entry> entries)
	{
		uint32 hash = 2166136261u; // FNV-1a over ids and names, so a rescan with the same games keeps the revision
		for (const Entry& e : entries)
		{
			for (int i = 0; i < 8; i++)
				hash = (hash ^ (uint8)(e.titleId >> (i * 8))) * 16777619u;
			for (char c : e.name)
				hash = (hash ^ (uint8)c) * 16777619u;
		}
		std::lock_guard lock(s_mutex);
		s_entries = std::move(entries);
		s_revision = hash ? hash : 1;
	}

	void SetSelection(uint64 titleId)
	{
		std::lock_guard lock(s_mutex);
		if (s_selection != titleId)
		{
			s_selection = titleId;
			s_selectionChanged = true;
		}
	}

	std::optional<uint64> TakeLaunch()
	{
		std::lock_guard lock(s_mutex);
		return std::exchange(s_pendingLaunch, std::nullopt);
	}

	std::optional<uint64> TakeSelection()
	{
		std::lock_guard lock(s_mutex);
		return std::exchange(s_pendingSelection, std::nullopt);
	}

	uint32 TakeNav()
	{
		std::lock_guard lock(s_mutex);
		return std::exchange(s_navPressed, 0);
	}

	bool _Known(uint64 token)
	{
		return std::any_of(s_entries.begin(), s_entries.end(), [token](const Entry& e) { return e.titleId == token; });
	}

	std::vector<std::vector<uint8>> HandlePacket(const uint8* data, size_t size)
	{
		std::vector<std::vector<uint8>> replies;
		if (size < kHeader || memcmp(data, "DSUS", 4) != 0)
			return replies;
		const uint16 length = _Get16(data + 6);
		if (length < 4 || kHeader + length - 4 > size)
			return replies;
		const size_t total = kHeader + length - 4;
		const uint32 type = _Get32(data + 16);
		std::vector<uint8> check(data, data + total);
		_Put32(check.data() + 8, 0);
		if (crc32_calc(check.data(), total) != _Get32(data + 8))
			return replies;
		const uint8* payload = data + kHeader;
		const size_t payloadSize = total - kHeader;
		std::lock_guard lock(s_mutex);
		s_lastHeard = std::chrono::steady_clock::now();
		if (type == kMsgKeyboard && payloadSize >= 51)
		{
			// SDL key codes: 273 up, 274 down, 275 right, 276 left
			std::array<uint32, 11> keys{};
			for (size_t i = 0; i < keys.size() && 7 + i * 4 + 4 <= payloadSize; i++)
				keys[i] = _Get32(payload + 7 + i * 4);
			auto down = [](const std::array<uint32, 11>& k, uint32 key) { return (k[key / 32] >> (key % 32)) & 1; };
			const std::pair<uint32, uint32> map[] = { { 276, kNavLeft }, { 275, kNavRight }, { 273, kNavUp }, { 274, kNavDown } };
			for (const auto& [key, nav] : map)
				if (down(keys, key) && !down(s_keys, key))
					s_navPressed |= nav;
			s_keys = keys;
		}
		else if (type == kMsgLaunch && payloadSize >= 12)
		{
			const uint32 revision = _Get32(payload);
			const uint64 token = _Get64(payload + 4);
			uint32 status;
			if (revision != s_revision)
				status = kStale;
			else if (!_Known(token))
				status = kUnknown;
			else if (s_pendingLaunch == token)
				status = kAccepted; // the app repeats the request until it sees the answer
			else if (s_pendingLaunch || CafeSystem::IsTitleRunning())
				status = kBusy;
			else
			{
				s_pendingLaunch = token;
				status = kAccepted;
			}
			cemuLog_log(LogType::Force, "Companion: launch request {:016x} (revision {}) -> status {}", token, revision, status);
			replies.emplace_back(_LaunchStatus(status, revision, token));
		}
		else if (type == kMsgSelection && payloadSize >= 12)
		{
			const uint64 token = _Get64(payload + 4);
			if (token && _Known(token))
			{
				s_pendingSelection = token;
				s_selection = token; // don't echo it back
			}
		}
		return replies;
	}

	bool IsPrivateAddress(const std::string& ip)
	{
		boost::system::error_code ec;
		const auto a = asio::ip::make_address(ip, ec);
		if (ec)
			return false;
		if (a.is_v6())
			return a.is_loopback() || a.to_v6().is_link_local() || (a.to_v6().is_v4_mapped() && IsPrivateAddress(asio::ip::make_address_v4(asio::ip::v4_mapped, a.to_v6()).to_string()));
		const uint32 v = a.to_v4().to_uint();
		return (v >> 24) == 10 || (v >> 24) == 127 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE;
	}

	std::string _JsonEscape(std::string_view s)
	{
		std::string out;
		for (char ch : s)
		{
			if (ch == '"' || ch == '\\')
				out += '\\', out += ch;
			else if ((unsigned char)ch < 0x20)
				out += fmt::format("\\u{:04x}", (int)(unsigned char)ch);
			else
				out += ch;
		}
		return out;
	}

	// PNG of an RGBA image, uncompressed deflate blocks (icons are 128x128, ~64 KB)
	std::string _Png(const HostIcons::Icon& icon)
	{
		std::string raw;
		const size_t stride = (size_t)icon.width * 4;
		for (int y = 0; y < icon.height; y++)
		{
			raw += '\0';
			raw.append((const char*)icon.rgba.data() + y * stride, stride);
		}
		std::string z = "\x78\x01";
		for (size_t pos = 0; pos < raw.size() || pos == 0;)
		{
			const size_t n = std::min<size_t>(65535, raw.size() - pos);
			const bool last = pos + n >= raw.size();
			z += (char)(last ? 1 : 0);
			z += (char)(n & 0xFF), z += (char)(n >> 8), z += (char)(~n & 0xFF), z += (char)((~n >> 8) & 0xFF);
			z.append(raw, pos, n);
			pos += n;
			if (last)
				break;
		}
		uint32 a = 1, b = 0;
		for (unsigned char c : raw)
			a = (a + c) % 65521, b = (b + a) % 65521;
		const uint32 adler = (b << 16) | a;
		for (int i = 3; i >= 0; i--)
			z += (char)(adler >> (i * 8));
		std::string png = "\x89PNG\r\n\x1a\n";
		auto chunk = [&png](const char* type, const std::string& data) {
			auto be32 = [](uint32 v) { std::string s; for (int i = 3; i >= 0; i--) s += (char)(v >> (i * 8)); return s; };
			std::string body = std::string(type, 4) + data;
			png += be32((uint32)data.size()) + body + be32(crc32_calc(body.data(), body.size()));
		};
		std::string ihdr;
		for (uint32 v : { (uint32)icon.width, (uint32)icon.height })
			for (int i = 3; i >= 0; i--)
				ihdr += (char)(v >> (i * 8));
		ihdr += std::string("\x08\x06\x00\x00\x00", 5); // 8 bit RGBA
		chunk("IHDR", ihdr);
		chunk("IDAT", z);
		chunk("IEND", "");
		return png;
	}

	fs::path _CoverPath(uint64 titleId)
	{
		return ActiveSettings::GetUserDataPath("covers") / fmt::format("{:016x}.jpg", titleId);
	}

	uint32 _MediaMask(uint64 titleId)
	{
		uint32 mask = 0;
		if (HostIcons::Get(titleId))
			mask |= (1u << kMarquee) | (1u << kCover);
		std::error_code ec;
		if (fs::is_regular_file(_CoverPath(titleId), ec))
			mask |= 1u << kCover;
		return mask;
	}

	std::string _Query(std::string_view query, std::string_view key)
	{
		while (!query.empty())
		{
			const size_t amp = query.find('&');
			const std::string_view part = query.substr(0, amp);
			if (part.size() > key.size() && part.substr(0, key.size()) == key && part[key.size()] == '=')
				return std::string(part.substr(key.size() + 1));
			if (amp == std::string_view::npos)
				break;
			query = query.substr(amp + 1);
		}
		return {};
	}

	int HandleHttp(const std::string& target, std::string& contentType, std::string& body)
	{
		const size_t qm = target.find('?');
		const std::string path = target.substr(0, qm);
		const std::string_view query = qm == std::string::npos ? std::string_view() : std::string_view(target).substr(qm + 1);
		std::vector<Entry> entries;
		uint32 revision;
		{
			std::lock_guard lock(s_mutex);
			entries = s_entries;
			revision = s_revision;
		}
		contentType = "text/plain";
		if (path == "/catalogue")
		{
			std::string out = fmt::format("{{\"version\":1,\"application\":\"{}\",\"playlists\":[{{\"name\":\"{}\",\"platform_media\":0,"
				"\"platform_media_revision\":{},\"platform_media_token\":\"0000000000000000\",\"count\":{},\"entries\":[",
				kAppName, kPlatform, revision, entries.size());
			bool first = true;
			for (const Entry& e : entries)
			{
				const std::string token = fmt::format("{:016X}", e.titleId);
				out += fmt::format("{}{{\"label\":\"{}\",\"path\":\"cemu://{}/{}\",\"file\":\"{}\",\"core\":\"{}\",\"core_path\":\"\","
					"\"db\":\"{}\",\"platform\":\"{}\",\"crc32\":\"\",\"img\":\"\",\"img_full\":\"\",\"img_short\":\"\",\"runtime\":\"\","
					"\"last_played\":\"\",\"app\":\"{}\",\"launch_revision\":{},\"launch_token\":\"{}\",\"media\":{},\"region\":\"{}\","
					"\"format\":\"\",\"version\":\"{}\"}}",
					first ? "" : ",", _JsonEscape(e.name), revision, token, _JsonEscape(e.file), kAppName, kPlatform, kPlatform,
					kAppName, revision, token, _MediaMask(e.titleId), _JsonEscape(e.region), e.version);
				first = false;
			}
			body = out + "]}]}";
			contentType = "application/json";
			return 200;
		}
		if (path.rfind("/media/", 0) == 0)
		{
			uint32 type = 0;
			try { type = (uint32)std::stoul(path.substr(7)); } catch (...) { type = 999; }
			uint64 token = 0;
			try { token = std::stoull(_Query(query, "token"), nullptr, 16); } catch (...) {}
			const bool known = std::any_of(entries.begin(), entries.end(), [token](const Entry& e) { return e.titleId == token; });
			if (known && type == kCover)
			{
				std::ifstream f(_CoverPath(token), std::ios::binary);
				if (f)
				{
					body.assign(std::istreambuf_iterator<char>(f), {});
					contentType = "image/jpeg";
					return 200;
				}
			}
			if (known && (type == kCover || type == kMarquee))
			{
				if (const HostIcons::Icon* icon = HostIcons::Get(token); icon && icon->width > 0)
				{
					body = _Png(*icon);
					contentType = "image/png";
					return 200;
				}
			}
		}
		body = "404 Not Found";
		return 404;
	}

	void _Send(const std::vector<uint8>& p)
	{
		boost::system::error_code ec;
		s_udp->send_to(asio::buffer(p), s_phone, 0, ec);
	}

	void _UdpLoop()
	{
		SetThreadName("Companion-UDP");
		auto nextTick = std::chrono::steady_clock::now();
		bool connected = false;
		std::array<uint8, 2048> buf;
		while (s_running)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextTick)
			{
				nextTick = now + std::chrono::seconds(1);
				_Send(_DsuRegister());
				_Send(BuildStatePacket());
				std::lock_guard lock(s_mutex);
				const bool heard = s_lastHeard.time_since_epoch().count() && now - s_lastHeard < std::chrono::seconds(5);
				if (heard != connected)
				{
					connected = heard;
					s_status = heard ? "App connected" : "Waiting for the app";
					cemuLog_log(LogType::Force, "Companion: {}", s_status);
				}
			}
			bool selectionChanged = false;
			uint32 revision;
			uint64 selection;
			{
				std::lock_guard lock(s_mutex);
				selectionChanged = std::exchange(s_selectionChanged, false);
				revision = s_revision;
				selection = s_selection;
			}
			if (selectionChanged && selection)
				_Send(_Selection(revision, selection));
			udp::endpoint from;
			boost::system::error_code ec;
			const size_t len = s_udp->receive_from(asio::buffer(buf), from, 0, ec);
			if (ec || len < kHeader || from.address() != s_phone.address())
				continue;
			if (_Get32(buf.data() + 16) < kMsgKeyboard)
			{
				std::lock_guard lock(s_mutex);
				s_lastHeard = std::chrono::steady_clock::now(); // normal DSU answers: the app is there
				continue;
			}
			for (const auto& reply : HandlePacket(buf.data(), len))
				_Send(reply);
		}
	}

	void _Serve(tcp::socket& socket)
	{
		boost::system::error_code ec;
		const auto remote = socket.remote_endpoint(ec);
		if (ec || !IsPrivateAddress(remote.address().to_string()))
		{
			cemuLog_log(LogType::Force, "Companion: refused HTTP from {}", ec ? std::string("?") : remote.address().to_string());
			return;
		}
		const DWORD timeoutMs = 3000;
		setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
		setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
		asio::streambuf buf;
		asio::read_until(socket, buf, "\r\n\r\n", ec);
		if (ec || buf.size() > 16384)
			return;
		std::istream in(&buf);
		std::string method, target;
		in >> method >> target;
		std::string contentType, body;
		const int status = method == "GET" ? HandleHttp(target, contentType, body) : 404;
		const std::string head = fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
			status, status == 200 ? "OK" : "Not Found", contentType, body.size());
		asio::write(socket, asio::buffer(head), ec);
		for (size_t pos = 0; !ec && pos < body.size(); pos += 64 * 1024)
			asio::write(socket, asio::buffer(body.data() + pos, std::min<size_t>(64 * 1024, body.size() - pos)), ec);
		socket.shutdown(tcp::socket::shutdown_both, ec);
		if (target.rfind("/catalogue", 0) == 0)
			cemuLog_log(LogType::Force, "Companion: catalogue sent to {} (revision {})", remote.address().to_string(), GetRevision());
	}

	void _Accept()
	{
		s_acceptor->async_accept([](boost::system::error_code ec, tcp::socket socket) {
			if (ec)
				return;
			_Serve(socket);
			_Accept();
		});
	}

	bool Start(const std::string& phoneIp)
	{
		Stop();
		try
		{
			s_io = std::make_unique<asio::io_context>();
			udp::resolver resolver(*s_io);
			s_phone = *resolver.resolve(udp::v4(), phoneIp, std::to_string(kDsuPort)).begin();
			s_udp = std::make_unique<udp::socket>(*s_io, udp::endpoint(udp::v4(), 0));
			const DWORD timeoutMs = 200;
			setsockopt(s_udp->native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
			s_acceptor = std::make_unique<tcp::acceptor>(*s_io, tcp::endpoint(tcp::v4(), kHttpPort));
		}
		catch (const std::exception& ex)
		{
			s_udp.reset();
			s_acceptor.reset();
			s_io.reset();
			std::lock_guard lock(s_mutex);
			s_status = fmt::format("Can't start: {}", ex.what());
			cemuLog_log(LogType::Force, "Companion: {}", s_status);
			return false;
		}
		{
			std::lock_guard lock(s_mutex);
			s_status = "Waiting for the app";
			s_lastHeard = {};
			s_keys = {};
			s_navPressed = 0;
			s_pendingLaunch.reset();
			s_pendingSelection.reset();
			s_selectionChanged = true;
		}
		s_running = true;
		_Accept();
		s_httpThread = std::thread([] { SetThreadName("Companion-HTTP"); s_io->run(); });
		s_udpThread = std::thread(_UdpLoop);
		cemuLog_log(LogType::Force, "Companion: started, app at {}:{}, catalogue on port {}", s_phone.address().to_string(), kDsuPort, kHttpPort);
		return true;
	}

	void Stop()
	{
		if (!s_running)
			return;
		s_running = false;
		s_io->stop();
		if (s_httpThread.joinable())
			s_httpThread.join();
		if (s_udpThread.joinable())
			s_udpThread.join();
		s_acceptor.reset();
		s_udp.reset();
		s_io.reset();
		std::lock_guard lock(s_mutex);
		s_status.clear();
		cemuLog_log(LogType::Force, "Companion: stopped");
	}

	bool IsRunning()
	{
		return s_running;
	}

	std::string GetStatus()
	{
		std::lock_guard lock(s_mutex);
		return s_status;
	}
}

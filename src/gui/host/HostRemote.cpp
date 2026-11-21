#include "gui/host/HostRemote.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"

#include <boost/asio.hpp>
#include <random>

// #936 phone remote. One connection at a time on a worker thread; every request needs the token (?k=). Settings are
// written straight into the config: they're the ones the renderer reads every frame, so they apply immediately.

namespace HostRemote
{
	namespace asio = boost::asio;
	using asio::ip::tcp;

	constexpr uint16 kPort = 8090;

	std::mutex s_mutex;
	std::unique_ptr<asio::io_context> s_io;
	std::unique_ptr<tcp::acceptor> s_acceptor;
	std::thread s_thread;
	std::string s_token;
	std::string s_url;
	std::string s_error;
	bool s_running = false;

	struct Setting
	{
		const char* name;
		const char* label;
		int min, max;
		std::vector<const char*> choices; // empty: slider min..max
		std::function<int()> get;
		std::function<void(int)> set;
	};

	const std::vector<Setting>& _Settings()
	{
		static const std::vector<Setting> s_settings = [] {
			auto& c = GetConfig();
			std::vector<Setting> v;
			v.push_back({ "vsync", "VSync", 0, 1, { "Off", "On" }, [&c] { return c.vsync.GetValue() != 0 ? 1 : 0; }, [&c](int x) { c.vsync = x; } });
			v.push_back({ "fps", "Show FPS", 0, 1, { "Off", "On" },
				[&c] { return (c.overlay.position != ScreenPosition::kDisabled && c.overlay.fps) ? 1 : 0; },
				[&c](int x) { c.overlay.position = x ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled; c.overlay.fps = x != 0; } });
			v.push_back({ "aniso", "Anisotropic filtering", 0, 4, { "Game default", "2x", "4x", "8x", "16x" },
				[&c] { return (int)c.anisotropic_filtering.GetValue(); }, [&c](int x) { c.anisotropic_filtering = x; } });
			v.push_back({ "aa", "Anti-aliasing", 0, 1, { "Off", "FXAA" },
				[&c] { return (int)c.output_antialiasing.GetValue(); }, [&c](int x) { c.output_antialiasing = x; } });
			v.push_back({ "sharpening", "Sharpening", 0, 1, { "Off", "AMD CAS" },
				[&c] { return (int)c.output_sharpening.GetValue(); }, [&c](int x) { c.output_sharpening = x; } });
			v.push_back({ "sharpness", "Sharpness %", 0, 100, {},
				[&c] { return (int)c.output_sharpness.GetValue(); }, [&c](int x) { c.output_sharpness = x; } });
			v.push_back({ "scaling", "Scaling", 0, 1, { "Keep aspect ratio", "Stretch" },
				[&c] { return (int)c.fullscreen_scaling.GetValue(); }, [&c](int x) { c.fullscreen_scaling = x; } });
			v.push_back({ "async", "Compile shaders asynchronously", 0, 1, { "Off", "On" },
				[&c] { return c.async_compile.GetValue() ? 1 : 0; }, [&c](int x) { c.async_compile = x != 0; } });
			v.push_back({ "wait", "Pipeline wait", 0, 2, { "Smooth", "Balanced", "Accurate" },
				[&c] { return std::clamp<int>(c.pipeline_wait.GetValue(), 0, 2); }, [&c](int x) { c.pipeline_wait = x; } });
			return v;
		}();
		return s_settings;
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

	std::map<std::string, std::string> _Query(std::string_view target)
	{
		std::map<std::string, std::string> q;
		const size_t qm = target.find('?');
		if (qm == std::string_view::npos)
			return q;
		std::string_view rest = target.substr(qm + 1);
		while (!rest.empty())
		{
			const size_t amp = rest.find('&');
			std::string_view part = rest.substr(0, amp);
			const size_t eq = part.find('=');
			if (eq != std::string_view::npos)
				q[std::string(part.substr(0, eq))] = std::string(part.substr(eq + 1));
			if (amp == std::string_view::npos)
				break;
			rest = rest.substr(amp + 1);
		}
		return q;
	}

	std::string _StateJson()
	{
		std::string game;
		if (CafeSystem::IsTitleRunning())
			game = CafeSystem::GetForegroundTitleName();
		std::string out = fmt::format("{{\"game\":\"{}\",\"settings\":[", _JsonEscape(game));
		bool first = true;
		for (const Setting& s : _Settings())
		{
			std::string choices;
			for (const char* ch : s.choices)
				choices += fmt::format("{}\"{}\"", choices.empty() ? "" : ",", ch);
			out += fmt::format("{}{{\"name\":\"{}\",\"label\":\"{}\",\"min\":{},\"max\":{},\"value\":{},\"choices\":[{}]}}",
				first ? "" : ",", s.name, s.label, s.min, s.max, s.get(), choices);
			first = false;
		}
		return out + "]}";
	}

	const char* kPage = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cemu remote</title><style>
body{font-family:sans-serif;background:#15171c;color:#eee;margin:0;padding:16px}h1{font-size:20px}#game{color:#9cf;margin-bottom:16px}
.row{background:#22252c;border-radius:10px;padding:12px;margin:10px 0}.row b{display:block;margin-bottom:8px}
button{background:#333844;color:#eee;border:0;border-radius:8px;padding:10px 14px;margin:3px;font-size:15px}button.on{background:#2d7ff9}
input[type=range]{width:100%}#err{color:#f88}</style></head><body>
<h1>Cemu remote</h1><div id="game"></div><div id="err"></div><div id="list"></div><script>
const k=new URLSearchParams(location.search).get('k')||'';
function set(n,v){fetch('/api/set?k='+k+'&name='+n+'&value='+v,{method:'POST'}).then(load);}
function load(){fetch('/api/state?k='+k).then(r=>{if(!r.ok)throw r.status;return r.json()}).then(s=>{
document.getElementById('err').textContent='';
document.getElementById('game').textContent=s.game?('Running: '+s.game):'No game running';
const l=document.getElementById('list');l.innerHTML='';
for(const o of s.settings){const d=document.createElement('div');d.className='row';const b=document.createElement('b');b.textContent=o.label;d.appendChild(b);
if(o.choices.length){o.choices.forEach((c,i)=>{const e=document.createElement('button');e.textContent=c;if(i==o.value)e.className='on';e.onclick=()=>set(o.name,i);d.appendChild(e);});}
else{const r=document.createElement('input');r.type='range';r.min=o.min;r.max=o.max;r.value=o.value;const t=document.createElement('span');t.textContent=' '+o.value;
r.oninput=()=>t.textContent=' '+r.value;r.onchange=()=>set(o.name,r.value);d.appendChild(r);d.appendChild(t);}
l.appendChild(d);}}).catch(e=>{document.getElementById('err').textContent='Not connected ('+e+'). Scan the QR code again.';});}
load();setInterval(()=>{if(!document.querySelector('input[type=range]:active'))load();},5000);
</script></body></html>)HTML";

	int HandleRequest(const std::string& method, const std::string& target, std::string& contentType, std::string& body)
	{
		std::string path = target.substr(0, target.find('?'));
		auto query = _Query(target);
		std::string token;
		{
			std::lock_guard lock(s_mutex);
			token = s_token;
		}
		contentType = "text/plain";
		if (token.empty() || query["k"] != token)
		{
			body = "Forbidden: scan the QR code in Cemu's settings again";
			return 403;
		}
		if (method == "GET" && path == "/")
		{
			contentType = "text/html; charset=utf-8";
			body = kPage;
			return 200;
		}
		if (method == "GET" && path == "/api/state")
		{
			contentType = "application/json";
			body = _StateJson();
			return 200;
		}
		if (method == "POST" && path == "/api/set")
		{
			for (const Setting& s : _Settings())
			{
				if (query["name"] != s.name)
					continue;
				int value;
				try
				{
					value = std::stoi(query["value"]);
				}
				catch (...)
				{
					body = "bad value";
					return 400;
				}
				s.set(std::clamp(value, s.min, s.max));
				GetConfigHandle().Save();
				cemuLog_log(LogType::Force, "Phone remote: {} = {}", s.name, std::clamp(value, s.min, s.max));
				body = "ok";
				return 200;
			}
			body = "unknown setting";
			return 404;
		}
		body = "not found";
		return 404;
	}

	void _Serve(tcp::socket& socket)
	{
		boost::system::error_code ec;
		// a phone that connects and sends nothing mustn't hold the server (and Stop) for long
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
		const int status = HandleRequest(method, target, contentType, body);
		const char* reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 400 ? "Bad Request" : "Not Found";
		std::string response = fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
			status, reason, contentType, body.size()) + body;
		asio::write(socket, asio::buffer(response), ec);
		socket.shutdown(tcp::socket::shutdown_both, ec);
	}

	void _Accept()
	{
		s_acceptor->async_accept([](boost::system::error_code ec, tcp::socket socket) {
			if (ec)
				return; // stopped
			_Serve(socket);
			_Accept();
		});
	}

	std::string _LocalIp()
	{
		// a UDP "connect" sends nothing, it only picks the interface the LAN is reached through
		try
		{
			asio::io_context io;
			asio::ip::udp::socket s(io);
			s.connect(asio::ip::udp::endpoint(asio::ip::make_address("192.168.0.1"), 9));
			return s.local_endpoint().address().to_string();
		}
		catch (...)
		{
			return "127.0.0.1";
		}
	}

	bool Start()
	{
		Stop();
		std::random_device rd;
		std::string token = fmt::format("{:08x}{:08x}{:08x}", rd(), rd(), rd());
		try
		{
			auto io = std::make_unique<asio::io_context>();
			auto acceptor = std::make_unique<tcp::acceptor>(*io, tcp::endpoint(tcp::v4(), kPort));
			std::lock_guard lock(s_mutex);
			s_io = std::move(io);
			s_acceptor = std::move(acceptor);
			s_token = token;
			s_url = fmt::format("http://{}:{}/?k={}", _LocalIp(), kPort, token);
			s_error.clear();
			s_running = true;
		}
		catch (const std::exception& ex)
		{
			std::lock_guard lock(s_mutex);
			s_error = fmt::format("Port {} can't be opened: {}", kPort, ex.what());
			cemuLog_log(LogType::Force, "Phone remote: {}", s_error);
			return false;
		}
		_Accept();
		s_thread = std::thread([] { s_io->run(); });
		cemuLog_log(LogType::Force, "Phone remote: listening on port {}", kPort);
		return true;
	}

	void Stop()
	{
		if (!s_running)
			return;
		s_io->stop();
		if (s_thread.joinable())
			s_thread.join();
		std::lock_guard lock(s_mutex);
		s_acceptor.reset();
		s_io.reset();
		s_token.clear();
		s_url.clear();
		s_running = false;
		cemuLog_log(LogType::Force, "Phone remote: stopped");
	}

	bool IsRunning()
	{
		return s_running;
	}

	std::string GetError()
	{
		std::lock_guard lock(s_mutex);
		return s_error;
	}

	std::string GetUrl()
	{
		std::lock_guard lock(s_mutex);
		return s_url;
	}
}

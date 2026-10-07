// Win32 shell for the wxWidgets-free frontend. Same launcher and emulator setup as the UWP app, in a normal desktop
// window, so the Xbox frontend can be tested on a PC. Built as Cemu_Host.exe (ENABLE_HOST_FRONTEND).

#include "interface/WindowSystem.h"
#include "gui/host/HostApp.h"
#include "gui/host/HostPlatform.h"
#include "config/LaunchSettings.h"
#include "util/helpers/helpers.h"

#include <Windows.h>
#include <windowsx.h>
#include <shobjidl.h>

namespace
{
	HWND s_hwnd = nullptr;
	bool s_quit = false;
	bool s_isFullscreen = false;
	RECT s_windowedRect{};

	double _DpiScale(HWND hwnd)
	{
		using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
		static auto getDpiForWindow = (GetDpiForWindow_t)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		if (getDpiForWindow && hwnd)
			return getDpiForWindow(hwnd) / 96.0;
		return 1.0;
	}

	void _NotifySize()
	{
		RECT rc{};
		GetClientRect(s_hwnd, &rc);
		HostApp::OnResize(rc.right - rc.left, rc.bottom - rc.top, _DpiScale(s_hwnd));
	}

	void _ToggleFullscreen()
	{
		const LONG style = GetWindowLongW(s_hwnd, GWL_STYLE);
		if (!s_isFullscreen)
		{
			GetWindowRect(s_hwnd, &s_windowedRect);
			MONITORINFO mi{ sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
			SetWindowLongW(s_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(s_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
		else
		{
			SetWindowLongW(s_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
			SetWindowPos(s_hwnd, nullptr, s_windowedRect.left, s_windowedRect.top, s_windowedRect.right - s_windowedRect.left, s_windowedRect.bottom - s_windowedRect.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
		s_isFullscreen = !s_isFullscreen;
	}

	LRESULT CALLBACK _WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_SIZE:
			if (s_hwnd && wParam != SIZE_MINIMIZED)
				_NotifySize();
			return 0;
		case WM_DPICHANGED:
		{
			const RECT* suggested = (const RECT*)lParam;
			SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		case WM_ACTIVATEAPP:
			HostApp::OnFocusChanged(wParam != FALSE);
			return 0;
		case WM_MOUSEMOVE:
			HostApp::OnMouseMove((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
			return 0;
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
			HostApp::OnMouseButton(0, msg == WM_LBUTTONDOWN);
			return 0;
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
			HostApp::OnMouseButton(1, msg == WM_RBUTTONDOWN);
			return 0;
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
			HostApp::OnMouseButton(2, msg == WM_MBUTTONDOWN);
			return 0;
		case WM_MOUSEWHEEL:
			HostApp::OnMouseWheel((float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
			return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))
			{
				_ToggleFullscreen();
				return 0;
			}
			if (wParam == VK_F11)
			{
				_ToggleFullscreen();
				return 0;
			}
			HostApp::OnKey((uint32)wParam, true);
			if (msg == WM_SYSKEYDOWN)
				break;
			return 0;
		case WM_KEYUP:
		case WM_SYSKEYUP:
			HostApp::OnKey((uint32)wParam, false);
			if (msg == WM_SYSKEYUP)
				break;
			return 0;
		case WM_CHAR:
			HostApp::OnChar((uint32)wParam);
			return 0;
		case WM_CLOSE:
			s_quit = true;
			return 0;
		case WM_DESTROY:
			s_quit = true;
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	fs::path _RunFileDialog(bool pickFolder)
	{
		fs::path result;
		IFileOpenDialog* dialog = nullptr;
		if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
			return result;
		DWORD options = 0;
		dialog->GetOptions(&options);
		dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (pickFolder ? FOS_PICKFOLDERS : 0));
		if (!pickFolder)
		{
			COMDLG_FILTERSPEC filters[] = {
				{ L"Wii U games (*.wud, *.wux, *.wua, *.rpx, *.elf, *.wuhb)", L"*.wud;*.wux;*.wua;*.rpx;*.elf;*.wuhb" },
				{ L"All files", L"*.*" },
			};
			dialog->SetFileTypes((UINT)std::size(filters), filters);
		}
		if (SUCCEEDED(dialog->Show(s_hwnd)))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)))
			{
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
				{
					result = fs::path(path);
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dialog->Release();
		return result;
	}
}

WindowSystem::WindowHandleInfo HostPlatform::GetRenderTarget()
{
	WindowSystem::WindowHandleInfo info{};
	info.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
	info.surface = s_hwnd;
	return info;
}

void HostPlatform::PickFolder(std::function<void(const fs::path&)> onPicked)
{
	onPicked(_RunFileDialog(true));
}

void HostPlatform::PickFile(std::function<void(const fs::path&)> onPicked)
{
	onPicked(_RunFileDialog(false));
}

void HostPlatform::ShowTextInput(bool)
{
}

bool HostPlatform::IsXbox()
{
	return false;
}

void HostPlatform::RequestQuit()
{
	s_quit = true;
}

const char* HostPlatform::GetName()
{
	return "Win32";
}

void WindowSystem::Create()
{
	SetThreadName("UIThread");
	HINSTANCE instance = GetModuleHandleW(nullptr);

	using SetProcessDpiAwarenessContext_t = BOOL(WINAPI*)(HANDLE);
	if (auto setDpiAwareness = (SetProcessDpiAwarenessContext_t)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
		setDpiAwareness((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);

	WNDCLASSEXW wc{ sizeof(wc) };
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = _WndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
	wc.lpszClassName = L"CemuHostWindow";
	RegisterClassExW(&wc);

	RECT rc{ 0, 0, 1280, 720 };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	s_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Cemu (D3D12 / UWP frontend test)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, nullptr);
	if (!s_hwnd)
	{
		MessageBoxW(nullptr, L"Failed to create the main window", L"Cemu", MB_OK | MB_ICONERROR);
		return;
	}
	ShowWindow(s_hwnd, SW_SHOWDEFAULT);
	_NotifySize();

	// portable layout: user data in "user" next to the executable, read only data (resources, gameProfiles) next to it
	wchar_t exePath[4096]{};
	GetModuleFileNameW(nullptr, exePath, (DWORD)std::size(exePath));
	const fs::path exeDir = fs::path(exePath).parent_path();
	HostApp::InitOptions options;
	options.userDataPath = exeDir / "user";
	options.dataPath = exeDir;
	if (auto loadFile = LaunchSettings::GetLoadFile())
		options.launchPath = *loadFile;

	std::string error;
	if (!HostApp::Initialize(options, error))
	{
		MessageBoxW(nullptr, boost::nowide::widen(error).c_str(), L"Cemu", MB_OK | MB_ICONERROR);
		DestroyWindow(s_hwnd);
		return;
	}

	while (!s_quit)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (s_quit)
			break;
		if (!HostApp::RunFrame())
			break;
	}
	HostApp::Shutdown();
	DestroyWindow(s_hwnd);
	s_hwnd = nullptr;
	// the emulator's threads are not designed to be torn down completely, leave like the wx frontend does
	ExitProcess(0);
}

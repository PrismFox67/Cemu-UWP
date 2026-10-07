// UWP shell for the wxWidgets-free frontend (Xbox Dev Mode and Windows). A CoreApplication with a CoreWindow; the
// launcher and the emulator both render into the CoreWindow with D3D12.
//
// Approach follows other UWP emulator ports (e.g. Dolphin's UWP fork): pump CoreWindow events and drive the frontend
// from the view's Run() loop, swallow the back button so B doesn't suspend the app on Xbox, query the HDMI mode on Xbox
// so rendering happens at the TV's real resolution, and save state when the app is suspended.

#include "Common/precompiled.h" // this file is built without the shared PCH (C++/WinRT)

#include "interface/WindowSystem.h"
#include "gui/host/HostApp.h"
#include "gui/host/HostPlatform.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "util/helpers/helpers.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;

namespace
{
	CoreWindow s_window{ nullptr };
	bool s_quit = false;
	bool s_visible = true;
	bool s_activated = false;
	bool s_isXbox = false;
	fs::path s_activationLaunchPath;

	fs::path _ToPath(const winrt::hstring& s)
	{
		return fs::path(std::wstring_view(s));
	}

	// physical pixel size of the window. On Xbox the CoreWindow is 1920x1080 view pixels regardless of the TV, render at
	// the HDMI output resolution instead (like other UWP emulators do)
	void _GetPhysicalSize(int& width, int& height, double& scale)
	{
		const Rect bounds = s_window.Bounds();
		scale = 1.0;
		try
		{
			scale = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
		}
		catch (...)
		{
		}
		width = (int)std::lround(bounds.Width * scale);
		height = (int)std::lround(bounds.Height * scale);
		if (s_isXbox)
		{
			try
			{
				if (auto hdmi = winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation::GetForCurrentView())
				{
					auto mode = hdmi.GetCurrentDisplayMode();
					if (mode && mode.ResolutionWidthInRawPixels() > 0)
					{
						width = (int)mode.ResolutionWidthInRawPixels();
						height = (int)mode.ResolutionHeightInRawPixels();
						scale = bounds.Height > 0.0f ? height / bounds.Height : 1.0;
					}
				}
			}
			catch (...)
			{
			}
		}
	}

	void _NotifySize()
	{
		int w, h;
		double scale;
		_GetPhysicalSize(w, h, scale);
		HostApp::OnResize(w, h, scale);
	}

	uint32 _ToVirtualKey(winrt::Windows::System::VirtualKey key)
	{
		return (uint32)key; // Windows.System.VirtualKey values are the Win32 VK_* codes
	}

	fs::path _LaunchPathFromProtocol(const IActivatedEventArgs& args)
	{
		// cemu:?path=<game> (or cmd=<game>, the convention used by Xbox frontends for Dolphin/RetroArch)
		auto protocolArgs = args.as<ProtocolActivatedEventArgs>();
		auto query = protocolArgs.Uri().QueryParsed();
		for (uint32_t i = 0; i < query.Size(); i++)
		{
			auto entry = query.GetAt(i);
			if (entry.Name() == L"path" || entry.Name() == L"cmd")
			{
				std::wstring value(entry.Value());
				// tolerate a quoted path or a full "Cemu.exe -g <path>" command line
				if (const auto pos = value.find(L"-g "); pos != std::wstring::npos)
					value = value.substr(pos + 3);
				while (!value.empty() && (value.front() == L'"' || value.front() == L' '))
					value.erase(value.begin());
				while (!value.empty() && (value.back() == L'"' || value.back() == L' '))
					value.pop_back();
				return fs::path(value);
			}
		}
		return {};
	}

	struct App : implements<App, IFrameworkViewSource, IFrameworkView>
	{
		IFrameworkView CreateView()
		{
			return *this;
		}

		void Initialize(const CoreApplicationView& view)
		{
			view.Activated({ this, &App::OnActivated });
			CoreApplication::Suspending({ this, &App::OnSuspending });
			CoreApplication::Resuming({ this, &App::OnResuming });
		}

		void SetWindow(const CoreWindow& window)
		{
			s_window = window;
			window.SizeChanged([](const CoreWindow&, const WindowSizeChangedEventArgs&) { _NotifySize(); });
			window.VisibilityChanged([](const CoreWindow&, const VisibilityChangedEventArgs& args) { s_visible = args.Visible(); });
			window.Activated([](const CoreWindow&, const WindowActivatedEventArgs& args) {
				HostApp::OnFocusChanged(args.WindowActivationState() != CoreWindowActivationState::Deactivated);
			});
			window.Closed([](const CoreWindow&, const CoreWindowEventArgs&) { s_quit = true; });
			window.PointerMoved([](const CoreWindow&, const PointerEventArgs& args) {
				const auto p = args.CurrentPoint().Position();
				const double scale = WindowSystem::GetWindowInfo().dpi_scale;
				HostApp::OnMouseMove((float)(p.X * scale), (float)(p.Y * scale));
			});
			auto onButtons = [](const CoreWindow&, const PointerEventArgs& args) {
				const auto props = args.CurrentPoint().Properties();
				HostApp::OnMouseButton(0, props.IsLeftButtonPressed());
				HostApp::OnMouseButton(1, props.IsRightButtonPressed());
				HostApp::OnMouseButton(2, props.IsMiddleButtonPressed());
			};
			window.PointerPressed(onButtons);
			window.PointerReleased(onButtons);
			window.PointerWheelChanged([](const CoreWindow&, const PointerEventArgs& args) {
				HostApp::OnMouseWheel((float)args.CurrentPoint().Properties().MouseWheelDelta() / 120.0f);
			});
			window.KeyDown([](const CoreWindow&, const KeyEventArgs& args) {
				HostApp::OnKey(_ToVirtualKey(args.VirtualKey()), true);
				args.Handled(true);
			});
			window.KeyUp([](const CoreWindow&, const KeyEventArgs& args) {
				HostApp::OnKey(_ToVirtualKey(args.VirtualKey()), false);
				args.Handled(true);
			});
			window.CharacterReceived([](const CoreWindow&, const CharacterReceivedEventArgs& args) {
				const uint32 c = args.KeyCode();
				if (c >= 32 && c != 127)
					HostApp::OnChar(c);
			});

			// B / Back would navigate away and suspend the app on Xbox
			SystemNavigationManager::GetForCurrentView().BackRequested([](const IInspectable&, const BackRequestedEventArgs& args) {
				args.Handled(HostApp::OnBackRequested());
			});

			// use the whole screen, no TV safe area border on Xbox
			try
			{
				ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
			}
			catch (...)
			{
			}
		}

		void Load(const hstring&)
		{
		}

		void Run()
		{
			SetThreadName("UIThread");
			CoreDispatcher dispatcher = s_window.Dispatcher();
			// wait for activation (carries the launch arguments)
			while (!s_activated && !s_quit)
				dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);

			HostApp::InitOptions options;
			options.userDataPath = _ToPath(ApplicationData::Current().LocalFolder().Path());
			options.dataPath = _ToPath(Package::Current().InstalledLocation().Path());
			if (!s_activationLaunchPath.empty())
				options.launchPath = s_activationLaunchPath;
			else if (auto loadFile = LaunchSettings::GetLoadFile())
				options.launchPath = *loadFile;

			_NotifySize();
			std::string error;
			if (!HostApp::Initialize(options, error))
			{
				cemuLog_log(LogType::Force, "Host: initialization failed: {}", error);
				// nothing to render with, show the error through the launcher if possible, otherwise just quit
				s_quit = true;
			}

			while (!s_quit)
			{
				if (s_visible)
				{
					dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
					if (s_quit || !HostApp::RunFrame())
						break;
				}
				else
				{
					// minimized / in the background: block until something happens
					dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
				}
			}
			HostApp::Shutdown();
			CoreApplication::Exit();
		}

		void Uninitialize()
		{
		}

		void OnActivated(const CoreApplicationView&, const IActivatedEventArgs& args)
		{
			try
			{
				if (args.Kind() == ActivationKind::Protocol)
					s_activationLaunchPath = _LaunchPathFromProtocol(args);
			}
			catch (...)
			{
			}
			s_activated = true;
			CoreWindow::GetForCurrentThread().Activate();
		}

		void OnSuspending(const IInspectable&, const SuspendingEventArgs& args)
		{
			// the console may terminate a suspended app without further notice, keep the settings safe
			auto deferral = args.SuspendingOperation().GetDeferral();
			GetConfigHandle().Save();
			deferral.Complete();
		}

		void OnResuming(const IInspectable&, const IInspectable&)
		{
		}
	};

	winrt::fire_and_forget _PickFolderAsync(std::function<void(const fs::path&)> onPicked)
	{
		fs::path result;
		try
		{
			Pickers::FolderPicker picker;
			picker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
			picker.FileTypeFilter().Append(L"*");
			StorageFolder folder = co_await picker.PickSingleFolderAsync();
			if (folder)
			{
				// keep access to the folder across restarts
				AccessCache::StorageApplicationPermissions::FutureAccessList().Add(folder);
				result = _ToPath(folder.Path());
			}
		}
		catch (const winrt::hresult_error& e)
		{
			cemuLog_log(LogType::Force, "Host: folder picker failed: {}", winrt::to_string(e.message()));
		}
		onPicked(result);
	}

	winrt::fire_and_forget _PickFileAsync(std::function<void(const fs::path&)> onPicked)
	{
		fs::path result;
		try
		{
			Pickers::FileOpenPicker picker;
			picker.ViewMode(Pickers::PickerViewMode::List);
			picker.SuggestedStartLocation(Pickers::PickerLocationId::ComputerFolder);
			for (const wchar_t* ext : { L".wud", L".wux", L".wua", L".rpx", L".elf", L".wuhb" })
				picker.FileTypeFilter().Append(ext);
			StorageFile file = co_await picker.PickSingleFileAsync();
			if (file)
			{
				AccessCache::StorageApplicationPermissions::FutureAccessList().Add(file);
				result = _ToPath(file.Path());
			}
		}
		catch (const winrt::hresult_error& e)
		{
			cemuLog_log(LogType::Force, "Host: file picker failed: {}", winrt::to_string(e.message()));
		}
		onPicked(result);
	}
}

WindowSystem::WindowHandleInfo HostPlatform::GetRenderTarget()
{
	WindowSystem::WindowHandleInfo info{};
	info.backend = WindowSystem::WindowHandleInfo::Backend::UWPCoreWindow;
	info.surface = winrt::get_abi(s_window); // IUnknown* of the CoreWindow, see D3D12SwapChain
	return info;
}

void HostPlatform::PickFolder(std::function<void(const fs::path&)> onPicked)
{
	_PickFolderAsync(std::move(onPicked));
}

void HostPlatform::PickFile(std::function<void(const fs::path&)> onPicked)
{
	_PickFileAsync(std::move(onPicked));
}

void HostPlatform::ShowTextInput(bool show)
{
	try
	{
		auto inputView = winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView();
		if (show)
			inputView.TryShow(winrt::Windows::UI::ViewManagement::Core::CoreInputViewKind::Keyboard);
		else
			inputView.TryHide();
	}
	catch (...)
	{
	}
}

bool HostPlatform::IsXbox()
{
	return s_isXbox;
}

void HostPlatform::RequestQuit()
{
	s_quit = true;
}

const char* HostPlatform::GetName()
{
	return s_isXbox ? "Xbox (UWP)" : "UWP";
}

void WindowSystem::Create()
{
	winrt::init_apartment();
	try
	{
		s_isXbox = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
	}
	catch (...)
	{
	}
	CoreApplication::Run(winrt::make<App>());
}

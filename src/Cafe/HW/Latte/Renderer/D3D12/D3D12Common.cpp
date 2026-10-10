#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

#include <atomic>
#include <stdexcept>

std::string D3D12_HResultToString(HRESULT hr)
{
	switch (hr)
	{
	case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
	case E_INVALIDARG: return "E_INVALIDARG";
	case E_FAIL: return "E_FAIL";
	case E_NOTIMPL: return "E_NOTIMPL";
	case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
	case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
	case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
	case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
	case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
	case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
	default: return fmt::format("0x{:08x}", (uint32)hr);
	}
}

void D3D12_ThrowIfFailed(HRESULT hr, const char* what)
{
	if (SUCCEEDED(hr))
		return;
	std::string msg = fmt::format("D3D12: {} failed with {}", what, D3D12_HResultToString(hr));
	cemuLog_log(LogType::Force, "{}", msg);
	throw std::runtime_error(msg);
}

bool D3D12_EnableDeviceRemovedDiagnostics()
{
	// D3D12 returns the same device for an adapter until it is released, so this has to happen before the first device
	// of the process is created (the host frontend's launcher creates one before the emulator)
	static const bool s_enabled = []() {
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
		if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
			return false;
		settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		return true;
	}();
	return s_enabled;
}

static std::atomic<const char*> s_lastGoodCheckpoint{ "(none)" };
static std::atomic<bool> s_removalReported{ false };

bool D3D12_Checkpoint(ID3D12Device* device, const char* where)
{
	if (!device)
		return true;
	const HRESULT reason = device->GetDeviceRemovedReason();
	if (SUCCEEDED(reason))
	{
		s_lastGoodCheckpoint.store(where, std::memory_order_relaxed);
		return true;
	}
	if (!s_removalReported.exchange(true))
		cemuLog_log(LogType::Force, "D3D12: device removed ({}), first noticed after: {}. Last check where it was still fine: {}",
			D3D12_HResultToString(reason), where, s_lastGoodCheckpoint.load(std::memory_order_relaxed));
	return false;
}

HANDLE D3D12_CreateLargeStackThread(std::function<void()> func)
{
	// 256 MB of address space, committed on use. Driver shader compilers and FXC recurse deeply on large shaders; on Xbox
	// a default sized stack overflowed (0xC00000FD) while Mario Kart 8 loaded its attract mode race
	auto* arg = new std::function<void()>(std::move(func));
	HANDLE thread = CreateThread(nullptr, 256ull * 1024 * 1024, [](LPVOID p) -> DWORD {
		std::unique_ptr<std::function<void()>> f((std::function<void()>*)p);
		(*f)();
		return 0;
	}, arg, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (!thread)
	{
		delete arg;
		throw std::runtime_error("D3D12: CreateThread failed");
	}
	return thread;
}

void D3D12_SetDebugName(ID3D12Object* object, const std::string& name)
{
	if (!object)
		return;
	std::wstring wname(name.begin(), name.end());
	object->SetName(wname.c_str());
}

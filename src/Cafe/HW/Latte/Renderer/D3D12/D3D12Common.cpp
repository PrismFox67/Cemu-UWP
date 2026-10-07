#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"

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

void D3D12_SetDebugName(ID3D12Object* object, const std::string& name)
{
	if (!object)
		return;
	std::wstring wname(name.begin(), name.end());
	object->SetName(wname.c_str());
}

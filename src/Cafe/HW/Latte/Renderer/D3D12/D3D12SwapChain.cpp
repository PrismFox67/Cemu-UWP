#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"

#include <dxgi1_5.h>

D3D12SwapChain::D3D12SwapChain(ID3D12Device* device, IDXGIFactory4* factory, ID3D12CommandQueue* queue, D3D12StagingDescriptorHeap* rtvHeap, TargetType type, void* nativeHandle, uint32 width, uint32 height)
	: m_device(device), m_rtvHeap(rtvHeap), m_type(type), m_width(std::max<uint32>(width, 1)), m_height(std::max<uint32>(height, 1))
{
	// tearing (uncapped presentation with vsync off) is only available for HWND swap chains on desktop
	ComPtr<IDXGIFactory5> factory5;
	if (type == TargetType::HWND && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
	{
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
			m_allowTearing = allowTearing != FALSE;
	}

	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = m_width;
	desc.Height = m_height;
	desc.Format = kFormat;
	desc.Stereo = FALSE;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = D3D12Const::kSwapChainBufferCount;
	desc.Scaling = (type == TargetType::HWND) ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	HRESULT hr = E_FAIL;
	switch (type)
	{
	case TargetType::HWND:
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
		hr = factory->CreateSwapChainForHwnd(queue, (HWND)nativeHandle, &desc, nullptr, nullptr, &swapChain1);
		if (SUCCEEDED(hr))
			factory->MakeWindowAssociation((HWND)nativeHandle, DXGI_MWA_NO_ALT_ENTER); // fullscreen is handled by the UI
#else
		cemuLog_log(LogType::Force, "D3D12: HWND swap chains are not available in UWP builds");
#endif
		break;
	case TargetType::CoreWindow:
		hr = factory->CreateSwapChainForCoreWindow(queue, (IUnknown*)nativeHandle, &desc, nullptr, &swapChain1);
		break;
	case TargetType::Composition:
		hr = factory->CreateSwapChainForComposition(queue, &desc, nullptr, &swapChain1);
		break;
	}
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: Failed to create swap chain: {}", D3D12_HResultToString(hr));
		return;
	}
	D3D12_ThrowIfFailed(swapChain1.As(&m_swapChain), "IDXGISwapChain3");
	CreateBufferViews();
}

D3D12SwapChain::~D3D12SwapChain()
{
	ReleaseBufferViews();
	m_swapChain.Reset();
}

void D3D12SwapChain::CreateBufferViews()
{
	for (uint32 i = 0; i < D3D12Const::kSwapChainBufferCount; i++)
	{
		auto& b = m_buffers[i];
		D3D12_ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&b.resource)), "IDXGISwapChain::GetBuffer");
		D3D12_SetDebugName(b.resource.Get(), fmt::format("Backbuffer{}", i));
		b.rtv = m_rtvHeap->Allocate();
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = kFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		m_device->CreateRenderTargetView(b.resource.Get(), &rtvDesc, b.rtv);
		D3D12_Checkpoint(m_device, "swap chain backbuffer RTV created");
		b.state = D3D12_RESOURCE_STATE_PRESENT;
	}
	m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
	m_hasDefinedContent = false;
}

void D3D12SwapChain::ReleaseBufferViews()
{
	for (auto& b : m_buffers)
	{
		if (b.rtv.ptr)
			m_rtvHeap->Free(b.rtv);
		b.rtv = {};
		b.resource.Reset();
	}
}

void D3D12SwapChain::Resize(uint32 width, uint32 height)
{
	width = std::max<uint32>(width, 1);
	height = std::max<uint32>(height, 1);
	if (!m_swapChain || (width == m_width && height == m_height))
		return;
	ReleaseBufferViews();
	HRESULT hr = m_swapChain->ResizeBuffers(D3D12Const::kSwapChainBufferCount, width, height, kFormat, m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: ResizeBuffers failed: {}", D3D12_HResultToString(hr));
		return;
	}
	m_width = width;
	m_height = height;
	CreateBufferViews();
}

bool D3D12SwapChain::Present(uint32 syncInterval)
{
	UINT flags = 0;
	if (syncInterval == 0 && m_allowTearing)
		flags |= DXGI_PRESENT_ALLOW_TEARING;
	HRESULT hr = m_swapChain->Present(syncInterval, flags);
	m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
	m_hasDefinedContent = false;
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		return false;
	if (FAILED(hr))
		cemuLog_logDebug(LogType::Force, "D3D12: Present returned {}", D3D12_HResultToString(hr));
	return true;
}

void D3D12SwapChain::RequestResize(uint32 width, uint32 height)
{
	std::lock_guard _l(m_resizeMutex);
	m_resizePending = true;
	m_pendingWidth = width;
	m_pendingHeight = height;
}

bool D3D12SwapChain::ConsumeResizeRequest(uint32& width, uint32& height)
{
	std::lock_guard _l(m_resizeMutex);
	if (!m_resizePending)
		return false;
	m_resizePending = false;
	width = m_pendingWidth;
	height = m_pendingHeight;
	return true;
}

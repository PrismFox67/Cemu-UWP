#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"

namespace WindowSystem
{
	struct WindowHandleInfo;
}

// Flip-model swap chain for either a Win32 HWND, a UWP CoreWindow or a XAML SwapChainPanel (composition)
class D3D12SwapChain
{
public:
	enum class TargetType
	{
		HWND,
		CoreWindow,
		Composition,
	};

	static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	D3D12SwapChain(ID3D12Device* device, IDXGIFactory4* factory, ID3D12CommandQueue* queue, D3D12StagingDescriptorHeap* rtvHeap, TargetType type, void* nativeHandle, uint32 width, uint32 height);
	~D3D12SwapChain();

	// all buffers must be idle (call after WaitForIdle)
	void Resize(uint32 width, uint32 height);

	bool IsValid() const { return m_swapChain != nullptr; }
	IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }
	uint32 GetWidth() const { return m_width; }
	uint32 GetHeight() const { return m_height; }

	ID3D12Resource* GetCurrentBackbuffer() const { return m_buffers[m_currentIndex].resource.Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const { return m_buffers[m_currentIndex].rtv; }
	D3D12_RESOURCE_STATES& GetCurrentBackbufferState() { return m_buffers[m_currentIndex].state; }

	// true if something has been drawn to the current backbuffer since the last present
	bool HasDefinedContent() const { return m_hasDefinedContent; }
	void SetDefinedContent() { m_hasDefinedContent = true; }

	// returns false if presentation failed because the device was removed
	bool Present(uint32 syncInterval);

	// non-zero if a resize was requested by the UI thread and has to be applied on the render thread
	void RequestResize(uint32 width, uint32 height);
	bool ConsumeResizeRequest(uint32& width, uint32& height);

private:
	void CreateBufferViews();
	void ReleaseBufferViews();

	ID3D12Device* m_device;
	D3D12StagingDescriptorHeap* m_rtvHeap;
	ComPtr<IDXGISwapChain3> m_swapChain;
	TargetType m_type;
	uint32 m_width;
	uint32 m_height;
	bool m_allowTearing = false;
	uint32 m_currentIndex = 0;
	bool m_hasDefinedContent = false;

	struct Buffer
	{
		ComPtr<ID3D12Resource> resource;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;
	};
	Buffer m_buffers[D3D12Const::kSwapChainBufferCount];

	std::mutex m_resizeMutex;
	bool m_resizePending = false;
	uint32 m_pendingWidth = 0;
	uint32 m_pendingHeight = 0;
};

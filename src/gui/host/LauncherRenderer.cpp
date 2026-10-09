#include "gui/host/LauncherRenderer.h"

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

#include <dxgi1_4.h>

namespace
{
	constexpr uint32 kFramesInFlight = D3D12Const::kSwapChainBufferCount;

	D3D12SwapChain::TargetType _TargetTypeFromBackend(WindowSystem::WindowHandleInfo::Backend backend)
	{
		switch (backend)
		{
		case WindowSystem::WindowHandleInfo::Backend::Windows:
			return D3D12SwapChain::TargetType::HWND;
		case WindowSystem::WindowHandleInfo::Backend::UWPCoreWindow:
			return D3D12SwapChain::TargetType::CoreWindow;
		case WindowSystem::WindowHandleInfo::Backend::UWPSwapChainPanel:
			return D3D12SwapChain::TargetType::Composition;
		default:
			throw std::runtime_error("Launcher: unsupported window backend");
		}
	}
}

struct LauncherRenderer::Impl
{
	ImGuiContext* imguiContext = nullptr;
	ComPtr<IDXGIFactory4> factory;
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> allocators[kFramesInFlight];
	uint64 allocatorFence[kFramesInFlight]{};
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	uint64 fenceValue = 0;
	uint32 frameIndex = 0;
	ComPtr<ID3D12DescriptorHeap> srvHeap; // shader visible, holds the font texture SRV
	std::unique_ptr<D3D12StagingDescriptorHeap> rtvHeap;
	std::unique_ptr<D3D12SwapChain> swapChain;
	bool backendInitialized = false;

	void WaitForFence(uint64 value)
	{
		if (fence->GetCompletedValue() >= value)
			return;
		fence->SetEventOnCompletion(value, fenceEvent);
		WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
	}

	void WaitIdle()
	{
		if (!queue || !fence)
			return;
		fenceValue++;
		queue->Signal(fence.Get(), fenceValue);
		WaitForFence(fenceValue);
	}
};

LauncherRenderer::LauncherRenderer(const WindowSystem::WindowHandleInfo& target, int width, int height, ImGuiContext* imguiContext)
	: m_impl(std::make_unique<Impl>())
{
	auto& d = *m_impl;
	d.imguiContext = imguiContext;
	D3D12_ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory)), "CreateDXGIFactory2");
	D3D12_EnableDeviceRemovedDiagnostics(); // the emulator later gets the same device, see D3D12Renderer::LogDeviceRemovedData
	D3D12_ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)), "D3D12CreateDevice");

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	D3D12_ThrowIfFailed(d.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d.queue)), "CreateCommandQueue");
	for (auto& allocator : d.allocators)
		D3D12_ThrowIfFailed(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
	D3D12_ThrowIfFailed(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocators[0].Get(), nullptr, IID_PPV_ARGS(&d.cmdList)), "CreateCommandList");
	d.cmdList->Close();
	D3D12_ThrowIfFailed(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)), "CreateFence");
	d.fenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_ThrowIfFailed(d.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&d.srvHeap)), "CreateDescriptorHeap");
	d.rtvHeap = std::make_unique<D3D12StagingDescriptorHeap>(d.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16, "LauncherRTVs");

	d.swapChain = std::make_unique<D3D12SwapChain>(d.device.Get(), d.factory.Get(), d.queue.Get(), d.rtvHeap.get(), _TargetTypeFromBackend(target.backend), target.surface, (uint32)std::max(width, 1), (uint32)std::max(height, 1));
	if (!d.swapChain->IsValid())
		throw std::runtime_error("Launcher: failed to create the swap chain");

	ImGuiContext* previous = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(d.imguiContext);
	if (!ImGui_ImplDX12_Init(d.device.Get(), (int)kFramesInFlight, D3D12SwapChain::kFormat, d.srvHeap.Get(), d.srvHeap->GetCPUDescriptorHandleForHeapStart(), d.srvHeap->GetGPUDescriptorHandleForHeapStart()))
	{
		ImGui::SetCurrentContext(previous);
		throw std::runtime_error("Launcher: ImGui_ImplDX12_Init failed");
	}
	d.backendInitialized = true;
	ImGui::SetCurrentContext(previous);
}

LauncherRenderer::~LauncherRenderer()
{
	auto& d = *m_impl;
	d.WaitIdle();
	if (d.backendInitialized)
	{
		ImGuiContext* previous = ImGui::GetCurrentContext();
		ImGui::SetCurrentContext(d.imguiContext);
		ImGui_ImplDX12_Shutdown();
		ImGui::SetCurrentContext(previous);
	}
	// the swap chain has to be completely released before the emulator creates its own for the same window
	d.swapChain.reset();
	d.rtvHeap.reset();
	d.cmdList.Reset();
	for (auto& allocator : d.allocators)
		allocator.Reset();
	d.srvHeap.Reset();
	d.queue.Reset();
	d.fence.Reset();
	if (d.fenceEvent)
		CloseHandle(d.fenceEvent);
	d.device.Reset();
	d.factory.Reset();
}

void LauncherRenderer::NewFrame()
{
	ImGui_ImplDX12_NewFrame();
}

void LauncherRenderer::InvalidateFonts()
{
	m_impl->WaitIdle();
	ImGui_ImplDX12_InvalidateDeviceObjects();
}

void LauncherRenderer::Resize(int width, int height)
{
	auto& d = *m_impl;
	if (width <= 0 || height <= 0)
		return;
	if ((uint32)width == d.swapChain->GetWidth() && (uint32)height == d.swapChain->GetHeight())
		return;
	d.WaitIdle();
	d.swapChain->Resize((uint32)width, (uint32)height);
}

bool LauncherRenderer::Render(ImDrawData* drawData, const float clearColor[4])
{
	auto& d = *m_impl;
	// reuse the allocator of the frame that was submitted kFramesInFlight frames ago
	const uint32 slot = d.frameIndex % kFramesInFlight;
	d.WaitForFence(d.allocatorFence[slot]);
	d.allocators[slot]->Reset();
	d.cmdList->Reset(d.allocators[slot].Get(), nullptr);

	ID3D12Resource* backbuffer = d.swapChain->GetCurrentBackbuffer();
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = backbuffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	d.cmdList->ResourceBarrier(1, &barrier);

	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.swapChain->GetCurrentRTV();
	d.cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
	d.cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	ID3D12DescriptorHeap* heaps[] = { d.srvHeap.Get() };
	d.cmdList->SetDescriptorHeaps(1, heaps);
	if (drawData)
		ImGui_ImplDX12_RenderDrawData(drawData, d.cmdList.Get());

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	d.cmdList->ResourceBarrier(1, &barrier);
	d.cmdList->Close();

	ID3D12CommandList* lists[] = { d.cmdList.Get() };
	d.queue->ExecuteCommandLists(1, lists);
	const bool presented = d.swapChain->Present(1);
	d.fenceValue++;
	d.queue->Signal(d.fence.Get(), d.fenceValue);
	d.allocatorFence[slot] = d.fenceValue;
	d.frameIndex++;
	return presented;
}

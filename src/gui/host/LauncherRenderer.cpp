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
	ComPtr<ID3D12DescriptorHeap> srvHeap; // shader visible: the font texture SRV, then the image textures
	uint32 srvDescriptorSize = 0;
	std::unique_ptr<D3D12StagingDescriptorHeap> rtvHeap;
	std::unique_ptr<D3D12SwapChain> swapChain;
	bool backendInitialized = false;

	struct Texture
	{
		ComPtr<ID3D12Resource> resource;
		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
	};
	std::unordered_map<uint64, Texture> textures;
	uint32 nextTextureSlot = 1; // slot 0 is the font
	static constexpr uint32 kMaxTextures = 512;

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
	heapDesc.NumDescriptors = 1 + Impl::kMaxTextures;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_ThrowIfFailed(d.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&d.srvHeap)), "CreateDescriptorHeap");
	d.srvDescriptorSize = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
	d.textures.clear();
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

void* LauncherRenderer::GetOrCreateTexture(uint64 key, const uint8* rgba, int width, int height)
{
	auto& d = *m_impl;
	if (const auto it = d.textures.find(key); it != d.textures.end())
		return (void*)it->second.gpuHandle.ptr;
	if (!rgba || width <= 0 || height <= 0 || d.nextTextureSlot > Impl::kMaxTextures)
		return nullptr;

	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = (uint64)width;
	desc.Height = (uint32)height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	Impl::Texture texture;
	if (FAILED(d.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.resource))))
		return nullptr;

	// upload through a staging buffer with the row pitch the copy needs
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	uint64 uploadSize = 0;
	d.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);
	D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = uploadSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> upload;
	if (FAILED(d.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
		return nullptr;
	uint8* mapped = nullptr;
	if (FAILED(upload->Map(0, nullptr, (void**)&mapped)))
		return nullptr;
	for (int y = 0; y < height; y++)
		memcpy(mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, rgba + (size_t)y * width * 4, (size_t)width * 4);
	upload->Unmap(0, nullptr);

	// a one-off copy on the frame's queue, waited for right away (only a few game icons are uploaded)
	d.WaitIdle();
	d.allocators[0]->Reset();
	d.cmdList->Reset(d.allocators[0].Get(), nullptr);
	D3D12_TEXTURE_COPY_LOCATION dst{ texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
	src.PlacedFootprint = footprint;
	d.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = texture.resource.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	d.cmdList->ResourceBarrier(1, &barrier);
	d.cmdList->Close();
	ID3D12CommandList* lists[] = { d.cmdList.Get() };
	d.queue->ExecuteCommandLists(1, lists);
	d.WaitIdle();

	const uint32 slot = d.nextTextureSlot++;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = d.srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += (size_t)slot * d.srvDescriptorSize;
	texture.gpuHandle = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
	texture.gpuHandle.ptr += (uint64)slot * d.srvDescriptorSize;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	d.device->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
	const void* id = (void*)texture.gpuHandle.ptr;
	d.textures.emplace(key, std::move(texture));
	return (void*)id;
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
	// log the refresh rate presentation actually runs at a few times after startup (DXGI frame statistics, and presents
	// per second: the launcher presents every refresh). On Xbox this shows whether a 120 Hz HDMI mode reaches the app
	{
		static auto s_start = std::chrono::steady_clock::now();
		static auto s_windowStart = s_start;
		static int s_reports = 0;
		const auto now = std::chrono::steady_clock::now();
		const double window = std::chrono::duration<double>(now - s_windowStart).count();
		if (s_reports < 6 && window >= 5.0)
		{
			const D3D12SwapChain::PresentStats stats = d.swapChain->TakePresentStats();
			const double hz = d.swapChain->MeasureRefreshRate();
			if (s_reports > 0) // the first window only primes the measurement
				cemuLog_log(LogType::Force, "Launcher: display {:.2f} Hz (DXGI frame statistics), {:.1f} presents/s, Present blocks {:.1f} ms average",
					hz, stats.presents / window, stats.presents ? stats.blockedMs / stats.presents : 0.0);
			s_windowStart = now;
			s_reports++;
		}
	}
	d.fenceValue++;
	d.queue->Signal(d.fence.Get(), d.fenceValue);
	d.allocatorFence[slot] = d.fenceValue;
	d.frameIndex++;
	return presented;
}

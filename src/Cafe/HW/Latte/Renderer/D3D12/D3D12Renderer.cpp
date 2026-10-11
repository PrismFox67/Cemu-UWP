#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RootSignature.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ImGui.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/LatteTextureViewD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12TextureReadback.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12InternalPipelines.h"
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "config/CemuConfig.h"
#include "WindowSystem.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"

#include <dxgi1_4.h>
#include <chrono>
#include <thread>

D3D12Renderer* D3D12Renderer::GetInstance()
{
	cemu_assert_debug(g_renderer && g_renderer->GetType() == RendererAPI::D3D12);
	return static_cast<D3D12Renderer*>(g_renderer.get());
}

std::vector<D3D12Renderer::AdapterInfo> D3D12Renderer::GetAdapters()
{
	std::vector<AdapterInfo> result;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return result;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
		{
			std::wstring wname(desc.Description);
			AdapterInfo info;
			info.name = std::string(wname.begin(), wname.end());
			info.luid = ((uint64)(uint32)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
			result.push_back(std::move(info));
		}
		adapter.Reset();
	}
	return result;
}

D3D12Renderer::D3D12Renderer()
	: Renderer(RendererAPI::D3D12)
{
	CreateDevice();

	m_stagingViewHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, "StagingViews");
	m_stagingSamplerHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, "StagingSamplers");
	m_stagingRTVHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, "StagingRTVs");
	m_stagingDSVHeap = std::make_unique<D3D12StagingDescriptorHeap>(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, "StagingDSVs");
	m_gpuViewHeap = std::make_unique<D3D12GpuViewHeap>(m_device.Get(), this, D3D12Const::kGpuViewHeapSize);
	m_gpuSamplerHeap = std::make_unique<D3D12GpuSamplerHeap>(m_device.Get(), D3D12Const::kGpuSamplerHeapSize);
	m_samplerCache = std::make_unique<D3D12SamplerCache>(m_device.Get(), m_stagingSamplerHeap.get());

	m_uploadRing = std::make_unique<D3D12UploadRing>(m_device.Get(), this, kUploadRingSize, "UploadRing");
	m_indexAllocator = std::make_unique<D3D12UploadHeapAllocator>(m_device.Get(), this, 8 * 1024 * 1024, "IndexHeap");
	m_readbackRing = std::make_unique<D3D12ReadbackRing>(m_device.Get(), kReadbackRingSize, "ReadbackRing");

	CreateRootSignature();
	CreateNullDescriptors();

	m_pipelineCache = std::make_unique<D3D12PipelineCache>(this);

	std::string err;
	if (!D3D12ShaderCompiler::Initialize(m_highestShaderModel, err))
		throw std::runtime_error(err);
	RendererShaderD3D12::Init();

	// first command list
	ResetCommandList();
	D3D12_Checkpoint(m_device.Get(), "renderer constructed");
}

D3D12Renderer::~D3D12Renderer()
{
	// Shutdown() is expected to have been called already
	if (m_device)
		WaitForIdle();
	occlusionQuery_destroyAll();
	m_imgui.reset();
	m_pipelineCache.reset();
	m_internal.reset();
	m_swapChainMain.reset();
	m_swapChainPad.reset();
	m_deferredReleaser.ReleaseAll();
	RendererShaderD3D12::Shutdown();
	D3D12ShaderCompiler::Shutdown();
	if (m_fenceEvent)
		CloseHandle(m_fenceEvent);
}

void D3D12Renderer::CreateDevice()
{
	UINT factoryFlags = 0;
#ifdef CEMU_DEBUG_ASSERT
	m_debugMode = true;
#endif
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
	char* envDebug = nullptr;
	size_t envLen = 0;
	if (_dupenv_s(&envDebug, &envLen, "CEMU_D3D12_DEBUG") == 0 && envDebug)
	{
		m_debugMode = envDebug[0] == '1';
		free(envDebug);
	}
#endif
	if (m_debugMode)
	{
		ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			debug->EnableDebugLayer();
			factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			cemuLog_log(LogType::Force, "D3D12: Debug layer enabled");
		}
	}
	D3D12_ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_dxgiFactory)), "CreateDXGIFactory2");

	// Device Removed Extended Data: the runtime records the GPU operations of every command list and the allocation of a
	// GPU page fault, which LogDeviceRemovedData() writes to the log. It is part of the runtime (no Graphics Tools needed)
	// and the only way to find out what killed the device on a machine without a debugger, like an Xbox
	m_dredEnabled = D3D12_EnableDeviceRemovedDiagnostics();
	cemuLog_log(LogType::Force, "D3D12: Device removed diagnostics (DRED) {}", m_dredEnabled ? "enabled" : "not available");

	// CEMU_D3D12_WARP=1 uses WARP, Microsoft's software reference implementation. Drivers differ in how lenient they are
	// (Intel rendered Mario Kart 8's button icons correctly, the Xbox didn't), WARP shows how the API is specified
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
	{
		char* envWarp = nullptr;
		size_t envWarpLen = 0;
		if (_dupenv_s(&envWarp, &envWarpLen, "CEMU_D3D12_WARP") == 0 && envWarp)
		{
			const bool useWarp = envWarp[0] == '1';
			free(envWarp);
			ComPtr<IDXGIAdapter> warp;
			if (useWarp && SUCCEEDED(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
				m_selectedDeviceName = "WARP (software)";
		}
	}
#endif

	// pick the adapter. 0 means "default"
	const uint64 requestedLuid = GetConfig().d3d12_adapter_luid;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; !m_device && m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
	{
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			adapter.Reset();
			continue;
		}
		const uint64 luid = ((uint64)(uint32)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
		if (requestedLuid != 0 && luid != requestedLuid)
		{
			adapter.Reset();
			continue;
		}
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device))))
		{
			m_adapter = adapter;
			std::wstring wname(desc.Description);
			m_selectedDeviceName = std::string(wname.begin(), wname.end());
			switch (desc.VendorId)
			{
			case 0x1002: m_vendor = GfxVendor::AMD; break;
			case 0x10DE: m_vendor = GfxVendor::Nvidia; break;
			case 0x8086: m_vendor = GfxVendor::Intel; break;
			default: m_vendor = GfxVendor::Generic; break;
			}
			break;
		}
		adapter.Reset();
	}
	if (!m_device)
	{
		// fall back to the default adapter (on Xbox this is the only one)
		D3D12_ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");
		m_selectedDeviceName = "Default adapter";
	}
	cemuLog_log(LogType::Force, "D3D12: Using device {}", m_selectedDeviceName);

	if (m_debugMode)
	{
		ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(m_device.As(&infoQueue)))
		{
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
		}
	}

	// capabilities. Older runtimes reject shader models they don't know about, so query from the highest one down.
	// Shader model 5.1 (DXBC, the default FXC path) is supported by every D3D12 device. 6.0 is only needed for DXIL
	D3D12_FEATURE_DATA_SHADER_MODEL sm{};
	m_highestShaderModel = D3D_SHADER_MODEL_5_1;
	for (uint32 candidate = 0x67; candidate >= 0x60; candidate--)
	{
		sm.HighestShaderModel = (D3D_SHADER_MODEL)candidate;
		if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
		{
			m_highestShaderModel = sm.HighestShaderModel;
			break;
		}
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
		m_bindingTier = options.ResourceBindingTier;
	if (m_bindingTier < D3D12_RESOURCE_BINDING_TIER_3)
		cemuLog_log(LogType::Force, "D3D12: Resource binding tier {} detected. Tier 3 is recommended, shaders that use more than 14 uniform blocks per stage may fail", (uint32)m_bindingTier);
	D3D12Format::InitFormatSupport(m_device.Get(), m_formatSupport);

	// pipeline libraries (driver pipeline cache on disk). The Xbox UWP driver (SraKmd) removes the device with
	// DXGI_ERROR_DRIVER_INTERNAL_ERROR when one is created, instead of failing the call, so it is never used there
	D3D12_FEATURE_DATA_SHADER_CACHE shaderCache{};
	m_pipelineLibrarySupported = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &shaderCache, sizeof(shaderCache))) &&
		(shaderCache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) != 0;
	// single pipeline blobs (GetCachedBlob / CachedPSO): the same driver also removes the device on GetCachedBlob
	m_pipelineBlobsSupported = (shaderCache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO) != 0;
	if (m_selectedDeviceName.rfind("SraKmd", 0) == 0)
	{
		m_pipelineLibrarySupported = false;
		m_pipelineBlobsSupported = false;
	}
	cemuLog_log(LogType::Force, "D3D12: Pipeline cache: libraries {}, single pipeline blobs {} (shader cache flags 0x{:x})", m_pipelineLibrarySupported ? "supported" : "not supported",
		m_pipelineBlobsSupported ? "supported" : "not supported", (uint32)shaderCache.SupportFlags);

	// queue + fence
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	D3D12_ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");
	D3D12_ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
	m_fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	if (!m_fenceEvent)
		throw std::runtime_error("D3D12: CreateEventEx failed");

	// command list (created closed, reset in ResetCommandList)
	ComPtr<ID3D12CommandAllocator> allocator;
	D3D12_ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
	D3D12_ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
	m_cmdList->Close();
	m_freeAllocators.push_back({ allocator, 0 });
}

void D3D12Renderer::CreateRootSignature()
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> error;
	HRESULT hr = D3D12_SerializeCemuRootSignature(&blob, &error);
	if (FAILED(hr))
	{
		std::string msg = error ? std::string((const char*)error->GetBufferPointer(), error->GetBufferSize()) : D3D12_HResultToString(hr);
		throw std::runtime_error("D3D12: Failed to serialize root signature: " + msg);
	}
	D3D12_ThrowIfFailed(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");
	D3D12_SetDebugName(m_rootSignature.Get(), "CemuRootSignature");
}

void D3D12Renderer::CreateNullDescriptors()
{
	m_nullRTV = m_stagingRTVHeap->Allocate();
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	m_device->CreateRenderTargetView(nullptr, &rtvDesc, m_nullRTV);

	m_nullCBV = m_stagingViewHeap->Allocate();
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
	cbvDesc.BufferLocation = 0;
	cbvDesc.SizeInBytes = 0;
	m_device->CreateConstantBufferView(&cbvDesc, m_nullCBV);

	m_nullUAV = m_stagingViewHeap->Allocate();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	m_device->CreateUnorderedAccessView(nullptr, nullptr, &uavDesc, m_nullUAV);

	auto makeNullSRV = [&](D3D12_SRV_DIMENSION dim) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_stagingViewHeap->Allocate();
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.ViewDimension = dim;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		switch (dim)
		{
		case D3D12_SRV_DIMENSION_TEXTURE1D: desc.Texture1D.MipLevels = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE2D: desc.Texture2D.MipLevels = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: desc.Texture2DArray.MipLevels = 1; desc.Texture2DArray.ArraySize = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: desc.TextureCubeArray.MipLevels = 1; desc.TextureCubeArray.NumCubes = 1; break;
		case D3D12_SRV_DIMENSION_TEXTURE3D: desc.Texture3D.MipLevels = 1; break;
		default: break;
		}
		m_device->CreateShaderResourceView(nullptr, &desc, h);
		return h;
	};
	m_nullSRV1D = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE1D);
	m_nullSRV2D = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE2D);
	m_nullSRV2DArray = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
	m_nullSRVCubeArray = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURECUBEARRAY);
	m_nullSRV3D = makeNullSRV(D3D12_SRV_DIMENSION_TEXTURE3D);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::GetNullSRV(Latte::E_DIM dim) const
{
	switch (dim)
	{
	case Latte::E_DIM::DIM_1D: return m_nullSRV1D;
	case Latte::E_DIM::DIM_2D_ARRAY: return m_nullSRV2DArray;
	case Latte::E_DIM::DIM_CUBEMAP: return m_nullSRVCubeArray;
	case Latte::E_DIM::DIM_3D: return m_nullSRV3D;
	default: return m_nullSRV2D;
	}
}

/* --- submission --- */

uint64 D3D12Renderer::GetCompletedSubmissionId()
{
	m_completedFenceValue = std::max(m_completedFenceValue, m_fence->GetCompletedValue());
	if (m_completedFenceValue == UINT64_MAX)
		HandleDeviceError(m_device->GetDeviceRemovedReason(), "Fence (device removed)");
	return m_completedFenceValue;
}

void D3D12Renderer::WaitForSubmission(uint64 submissionId)
{
	cemu_assert_debug(submissionId <= m_lastSubmittedFenceValue);
	if (GetCompletedSubmissionId() >= submissionId)
		return;
	D3D12_ThrowIfFailed(m_fence->SetEventOnCompletion(submissionId, m_fenceEvent), "SetEventOnCompletion");
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	GetCompletedSubmissionId();
}

void D3D12Renderer::WaitForIdle()
{
	if (m_hasRecordedWork)
		SubmitCommandList(false);
	WaitForSubmission(m_lastSubmittedFenceValue);
	ProcessFinishedSubmissions();
}

void D3D12Renderer::ResetCommandList()
{
	// reuse the oldest allocator if the GPU is done with it
	ProcessFinishedSubmissions();
	if (!m_inFlightAllocators.empty() && m_inFlightAllocators.front().fenceValue <= m_completedFenceValue)
	{
		m_freeAllocators.push_back(std::move(m_inFlightAllocators.front()));
		m_inFlightAllocators.pop_front();
	}
	if (m_freeAllocators.empty())
	{
		CommandAllocatorEntry e;
		D3D12_ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e.allocator)), "CreateCommandAllocator");
		e.fenceValue = 0;
		m_freeAllocators.push_back(std::move(e));
	}
	m_currentAllocator = std::move(m_freeAllocators.front().allocator);
	m_freeAllocators.pop_front();
	D3D12_ThrowIfFailed(m_currentAllocator->Reset(), "ID3D12CommandAllocator::Reset");
	D3D12_ThrowIfFailed(m_cmdList->Reset(m_currentAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
	if (m_dredEnabled) // identifies the command list in the device removed diagnostics
		D3D12_SetDebugName(m_cmdList.Get(), fmt::format("Cemu submission #{}", m_lastSubmittedFenceValue + 1));
	m_drawsInCommandList = 0;
	m_hasRecordedWork = false;
	InvalidateDrawState();
	BindRootSignatureAndHeaps();
	occlusionQuery_notifyBeginCommandList();
	D3D12_Checkpoint(m_device.Get(), "command list reset");
}

void D3D12Renderer::SubmitCommandList(bool waitIdle)
{
	occlusionQuery_notifyEndCommandList();
	FlushBarriers();
	HRESULT hr = m_cmdList->Close();
	if (FAILED(hr))
		HandleDeviceError(hr, "ID3D12GraphicsCommandList::Close");
	ID3D12CommandList* lists[] = { m_cmdList.Get() };
	D3D12_Checkpoint(m_device.Get(), "command list closed");
	m_queue->ExecuteCommandLists(1, lists);
	const uint64 submissionId = m_lastSubmittedFenceValue + 1;
	hr = m_queue->Signal(m_fence.Get(), submissionId);
	if (FAILED(hr))
		HandleDeviceError(hr, "ID3D12CommandQueue::Signal");
	m_uploadRing->OnSubmit(submissionId);
	m_gpuViewHeap->OnSubmit(submissionId);
	m_inFlightAllocators.push_back({ std::move(m_currentAllocator), submissionId });
	m_lastSubmittedFenceValue = submissionId;
	m_statSubmitsPerFrame++;
	m_submitSoon = false;

	// The first submissions are waited for and logged, so a GPU that rejects startup work (seen on Xbox) is caught at the
	// exact submission that caused it instead of somewhere later
	constexpr uint64 kLoggedStartupSubmissions = 40;
	if (submissionId <= kLoggedStartupSubmissions)
	{
		const uint32 draws = m_drawsInCommandList;
		WaitForSubmission(submissionId);
		const HRESULT removed = m_device->GetDeviceRemovedReason();
		cemuLog_log(LogType::Force, "D3D12: startup submission #{} ({} draws): {}", submissionId, draws, SUCCEEDED(removed) ? "ok" : D3D12_HResultToString(removed));
		if (FAILED(removed))
			HandleDeviceError(removed, fmt::format("Startup submission #{}", submissionId).c_str());
	}
	else if (waitIdle)
		WaitForSubmission(submissionId);
	ResetCommandList();
}

void D3D12Renderer::ProcessFinishedSubmissions()
{
	const uint64 previous = m_completedFenceValue;
	const uint64 completed = GetCompletedSubmissionId();
	m_uploadRing->Retire(completed);
	m_gpuViewHeap->Retire(completed);
	m_indexAllocator->Retire(completed);
	m_deferredReleaser.Retire(completed);
	if (completed != previous)
		LatteTextureReadback_UpdateFinishedTransfers(false);
}

void D3D12Renderer::BindRootSignatureAndHeaps()
{
	ID3D12DescriptorHeap* heaps[] = { m_gpuViewHeap->GetHeap(), m_gpuSamplerHeap->GetHeap() };
	m_cmdList->SetDescriptorHeaps(2, heaps);
	m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
	m_state.rootSignatureBound = true;
}

void D3D12Renderer::InvalidateDrawState()
{
	m_state.renderTargetsDirty = true;
	m_state.vertexBufferDirtyMask = 0xFFFF;
	m_state.viewportDirty = true;
	m_state.scissorDirty = true;
	m_state.currentPSO = nullptr;
	m_state.currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_state.currentIndexBuffer = {};
	m_state.currentBlendFactor[0] = -1.0f;
	m_state.currentStencilRef = 0xFFFFFFFF;
	m_state.rootSignatureBound = false;
}

void D3D12Renderer::HandleDeviceError(HRESULT hr, const char* what)
{
	HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : hr;
	std::string msg = fmt::format("D3D12: {} failed ({}). Device removed reason: {}", what, D3D12_HResultToString(hr), D3D12_HResultToString(reason));
	cemuLog_log(LogType::Force, "{}", msg);
	if (FAILED(reason))
		LogDeviceRemovedData();
	throw std::runtime_error(msg);
}

namespace
{
	const char* _BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
	{
		static const char* const s_names[] = {
			"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
			"CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
			"ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
			"BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64",
			"ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
			"ProcessFrames1", "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
			"CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
			"ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh",
		};
		return (uint32)op < std::size(s_names) ? s_names[op] : "Unknown";
	}

	std::string _DredName(const char* nameA, const wchar_t* nameW)
	{
		if (nameA)
			return nameA;
		if (nameW)
		{
			std::wstring w(nameW);
			return std::string(w.begin(), w.end());
		}
		return "(unnamed)";
	}
}

void D3D12Renderer::LogDeviceRemovedData()
{
	static bool s_logged = false;
	if (s_logged)
		return;
	s_logged = true;
	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (!m_dredEnabled || !m_device || FAILED(m_device.As(&dred)))
	{
		cemuLog_log(LogType::Force, "D3D12: No device removed diagnostics (DRED) available");
		return;
	}
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		uint32 nodeCount = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
		{
			const uint32 completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			nodeCount++;
			if (completed >= node->BreadcrumbCount)
				continue; // finished on the GPU, not the cause
			cemuLog_log(LogType::Force, "DRED: {} on {}: {} of {} operations completed", _DredName(node->pCommandListDebugNameA, node->pCommandListDebugNameW),
				_DredName(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW), completed, node->BreadcrumbCount);
			const uint32 first = completed > 8 ? completed - 8 : 0;
			const uint32 last = std::min<uint32>(node->BreadcrumbCount, completed + 8);
			for (uint32 i = first; i < last; i++)
				cemuLog_log(LogType::Force, "DRED:   [{}] {}{}", i, _BreadcrumbOpName(node->pCommandHistory[i]), i == completed ? "   <- not completed, likely the faulting operation" : "");
		}
		cemuLog_log(LogType::Force, "DRED: {} command list(s) recorded", nodeCount);
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA != 0)
	{
		cemuLog_log(LogType::Force, "DRED: GPU page fault at address 0x{:016x}", (uint64)pageFault.PageFaultVA);
		for (const D3D12_DRED_ALLOCATION_NODE* a = pageFault.pHeadExistingAllocationNode; a; a = a->pNext)
			cemuLog_log(LogType::Force, "DRED:   existing allocation at that address: {} (type {})", _DredName(a->ObjectNameA, a->ObjectNameW), (uint32)a->AllocationType);
		for (const D3D12_DRED_ALLOCATION_NODE* a = pageFault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
			cemuLog_log(LogType::Force, "DRED:   recently freed allocation at that address: {} (type {})", _DredName(a->ObjectNameA, a->ObjectNameW), (uint32)a->AllocationType);
	}
	else
		cemuLog_log(LogType::Force, "DRED: no GPU page fault recorded");
}

void D3D12Renderer::ReleaseResourceDeferred(ComPtr<ID3D12Resource> resource)
{
	if (!resource)
		return;
	ComPtr<IUnknown> unk;
	resource.As(&unk);
	m_deferredReleaser.Release(std::move(unk), GetRecordingSubmissionId());
}

void D3D12Renderer::ReleaseObjectDeferred(ComPtr<IUnknown> object)
{
	m_deferredReleaser.Release(std::move(object), GetRecordingSubmissionId());
}

/* --- barriers --- */

void D3D12Renderer::TransitionTexture(LatteTextureD3D12* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount, D3D12_RESOURCE_STATES newState)
{
	if (!texture->GetResource())
		return;
	const bool is3D = texture->Is3DTexture();
	if (is3D)
	{
		firstSlice = 0;
		sliceCount = 1;
	}
	const uint32 planeCount = texture->GetPlaneCount();
	// whole resource transition if possible
	if (firstMip == 0 && mipCount == texture->GetMipCount() && firstSlice == 0 && sliceCount == texture->GetArraySize())
	{
		D3D12_RESOURCE_STATES first = texture->GetSubresourceState(0);
		if (texture->AllSubresourcesInState(first))
		{
			if (first == newState)
				return;
			m_pendingBarriers.push_back(D3D12_TransitionBarrier(texture->GetResource(), first, newState));
			texture->SetAllSubresourceStates(newState);
			return;
		}
	}
	for (uint32 plane = 0; plane < planeCount; plane++)
	{
		for (uint32 slice = firstSlice; slice < firstSlice + sliceCount; slice++)
		{
			for (uint32 mip = firstMip; mip < firstMip + mipCount; mip++)
			{
				const uint32 sub = texture->GetSubresourceIndex(mip, slice, plane);
				const D3D12_RESOURCE_STATES current = texture->GetSubresourceState(sub);
				if (current == newState)
					continue;
				m_pendingBarriers.push_back(D3D12_TransitionBarrier(texture->GetResource(), current, newState, sub));
				texture->SetSubresourceState(sub, newState);
			}
		}
	}
}

void D3D12Renderer::TransitionTextureAll(LatteTextureD3D12* texture, D3D12_RESOURCE_STATES newState)
{
	TransitionTexture(texture, 0, texture->GetMipCount(), 0, texture->GetArraySize(), newState);
}

void D3D12Renderer::TransitionResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES& trackedState, D3D12_RESOURCE_STATES newState)
{
	if (trackedState == newState)
		return;
	m_pendingBarriers.push_back(D3D12_TransitionBarrier(resource, trackedState, newState));
	trackedState = newState;
}

void D3D12Renderer::UAVBarrier(ID3D12Resource* resource)
{
	m_pendingBarriers.push_back(D3D12_UAVBarrier(resource));
}

void D3D12Renderer::FlushBarriers()
{
	if (m_pendingBarriers.empty())
		return;
	m_cmdList->ResourceBarrier((UINT)m_pendingBarriers.size(), m_pendingBarriers.data());
	m_statBarriersPerFrame += (uint32)m_pendingBarriers.size();
	m_pendingBarriers.clear();
	m_hasRecordedWork = true;
}

/* --- object lifetime notifications --- */

void D3D12Renderer::NotifyTextureRelease(LatteTextureD3D12* texture)
{
	// drop pending barriers that reference the texture, the resource will be released once the GPU is done with it
	// (barriers that were already recorded keep the resource alive through the deferred releaser)
	m_pendingBarriers.erase(std::remove_if(m_pendingBarriers.begin(), m_pendingBarriers.end(), [texture](const D3D12_RESOURCE_BARRIER& b) {
		return b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == texture->GetResource();
	}), m_pendingBarriers.end());
}

void D3D12Renderer::NotifyTextureViewRelease(LatteTextureViewD3D12* view)
{
	for (auto& t : m_state.boundTexture)
		if (t == view)
			t = nullptr;
	if (m_internal)
		surfaceCopy_notifyViewRelease(view);
}

void D3D12Renderer::NotifyFBORelease(CachedFBOD3D12* fbo)
{
	if (m_state.activeFBO == fbo)
	{
		m_state.activeFBO = nullptr;
		m_state.renderTargetsDirty = true;
	}
}

/* --- initialization --- */

void D3D12Renderer::Initialize()
{
	Renderer::Initialize();
	D3D12_Checkpoint(m_device.Get(), "Renderer::Initialize");
	m_imgui = std::make_unique<D3D12ImGuiRenderer>(this);
	D3D12_Checkpoint(m_device.Get(), "ImGui renderer created (internal shaders)");
	surfaceCopy_init();
	D3D12_Checkpoint(m_device.Get(), "surfaceCopy_init");
}

void D3D12Renderer::Shutdown()
{
	WaitForIdle();
	DeleteFontTextures();
	m_imgui.reset();
	surfaceCopy_shutdown();
	m_pipelineCache->Clear();
	Renderer::Shutdown();
	WaitForIdle();
	m_deferredReleaser.ReleaseAll();
}

void D3D12Renderer::EnableDebugMode()
{
	// the debug layer has to be enabled before device creation, see CEMU_D3D12_DEBUG
}

bool D3D12Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
	ComPtr<IDXGIAdapter3> adapter3;
	if (m_adapter && SUCCEEDED(m_adapter.As(&adapter3)))
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		{
			totalInMB = (int)(info.Budget / 1000 / 1000);
			usageInMB = (int)(info.CurrentUsage / 1000 / 1000);
			return true;
		}
	}
	return Renderer::GetVRAMInfo(usageInMB, totalInMB);
}

/* --- surfaces --- */

void D3D12Renderer::InitializeSurface(const Vector2i& size, bool mainWindow)
{
	auto& windowInfo = WindowSystem::GetWindowInfo();
	const WindowSystem::WindowHandleInfo& handle = mainWindow ? windowInfo.canvas_main : windowInfo.canvas_pad;
	D3D12SwapChain::TargetType type;
	switch (handle.backend)
	{
	case WindowSystem::WindowHandleInfo::Backend::Windows:
		type = D3D12SwapChain::TargetType::HWND;
		break;
	case WindowSystem::WindowHandleInfo::Backend::UWPCoreWindow:
		type = D3D12SwapChain::TargetType::CoreWindow;
		break;
	case WindowSystem::WindowHandleInfo::Backend::UWPSwapChainPanel:
		type = D3D12SwapChain::TargetType::Composition;
		break;
	default:
		throw std::runtime_error("D3D12: Unsupported window system backend");
	}
	auto swapChain = std::make_unique<D3D12SwapChain>(m_device.Get(), m_dxgiFactory.Get(), m_queue.Get(), m_stagingRTVHeap.get(), type, handle.surface, (uint32)size.x, (uint32)size.y);
	if (!swapChain->IsValid())
		throw std::runtime_error("D3D12: Failed to create swap chain");
	D3D12_Checkpoint(m_device.Get(), mainWindow ? "TV swap chain created" : "GamePad swap chain created");
	if (mainWindow)
		m_swapChainMain = std::move(swapChain);
	else
		m_swapChainPad = std::move(swapChain);
}

void D3D12Renderer::ShutdownSurface(bool mainWindow)
{
	if (mainWindow)
	{
		WaitForIdle();
		m_swapChainMain.reset();
		return;
	}
	// called from the UI thread. Ask the render thread to release the pad swap chain and wait for it to do so
	m_padShutdownRequested = true;
	for (int i = 0; i < 100 && m_padShutdownRequested; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (m_padShutdownRequested)
	{
		// render thread is not running (e.g. no game loaded), destroy it here
		std::lock_guard _l(m_padSwapChainMutex);
		m_swapChainPad.reset();
		m_padShutdownRequested = false;
	}
}

void D3D12Renderer::ProcessPadSwapChainShutdown()
{
	if (!m_padShutdownRequested)
		return;
	std::lock_guard _l(m_padSwapChainMutex);
	WaitForIdle();
	m_swapChainPad.reset();
	m_padShutdownRequested = false;
}

void D3D12Renderer::ResizeSurface(const Vector2i& size, bool mainWindow)
{
	// called from the UI thread. The swap chain is resized on the render thread before the next frame
	D3D12SwapChain* swapChain = GetSwapChainObj(mainWindow);
	if (swapChain)
		swapChain->RequestResize((uint32)size.x, (uint32)size.y);
}

IDXGISwapChain1* D3D12Renderer::GetSwapChain(bool mainWindow)
{
	D3D12SwapChain* swapChain = GetSwapChainObj(mainWindow);
	return swapChain ? swapChain->GetSwapChain() : nullptr;
}

bool D3D12Renderer::IsPadWindowActive()
{
	ProcessPadSwapChainShutdown();
	return m_swapChainPad != nullptr && m_swapChainPad->IsValid();
}

bool D3D12Renderer::AcquireBackbuffer(bool mainWindow)
{
	if (!mainWindow)
		ProcessPadSwapChainShutdown();
	D3D12SwapChain* swapChain = GetSwapChainObj(mainWindow);
	if (!swapChain || !swapChain->IsValid())
		return false;
	uint32 w, h;
	if (swapChain->ConsumeResizeRequest(w, h))
	{
		WaitForIdle();
		swapChain->Resize(w, h);
		D3D12_Checkpoint(m_device.Get(), "swap chain resized");
	}
	TransitionResource(swapChain->GetCurrentBackbuffer(), swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
	D3D12_Checkpoint(m_device.Get(), "backbuffer acquired");
	return true;
}

void D3D12Renderer::PresentSwapChain(bool mainWindow)
{
	D3D12SwapChain* swapChain = GetSwapChainObj(mainWindow);
	if (!swapChain || !swapChain->IsValid())
		return;
	if (!swapChain->HasDefinedContent())
	{
		// make sure the backbuffer content is defined
		AcquireBackbuffer(mainWindow);
		FlushBarriers();
		const float clearColor[4] = { 0, 0, 0, 1 };
		m_cmdList->ClearRenderTargetView(swapChain->GetCurrentRTV(), clearColor, 0, nullptr);
	}
	TransitionResource(swapChain->GetCurrentBackbuffer(), swapChain->GetCurrentBackbufferState(), D3D12_RESOURCE_STATE_PRESENT);
	SubmitCommandList(false);
	const int vsync = GetConfig().vsync.GetValue();
	if (!swapChain->Present(vsync != 0 ? 1 : 0))
		HandleDeviceError(m_device->GetDeviceRemovedReason(), "Present");
	D3D12_Checkpoint(m_device.Get(), "present");
}

void D3D12Renderer::ClearColorbuffer(bool padView)
{
	if (!AcquireBackbuffer(!padView))
		return;
	D3D12SwapChain* swapChain = GetSwapChainObj(!padView);
	FlushBarriers();
	const float clearColor[4] = { 0, 0, 0, 0 };
	m_cmdList->ClearRenderTargetView(swapChain->GetCurrentRTV(), clearColor, 0, nullptr);
	swapChain->SetDefinedContent();
	m_hasRecordedWork = true;
}

bool D3D12Renderer::BeginFrame(bool mainWindow)
{
	if (!AcquireBackbuffer(mainWindow))
		return false;
	ClearColorbuffer(!mainWindow);
	return true;
}

void D3D12Renderer::DrawEmptyFrame(bool mainWindow)
{
	if (!BeginFrame(mainWindow))
		return;
	SwapBuffers(mainWindow, !mainWindow);
}

void D3D12Renderer::SwapBuffers(bool swapTV, bool swapDRC)
{
	if (swapTV && m_swapChainMain)
		PresentSwapChain(true);
	if (swapDRC && m_swapChainPad)
		PresentSwapChain(false);
	if (!swapTV && !swapDRC)
		SubmitCommandList(false);
	m_statDrawsPerFrame = 0;
	m_statSubmitsPerFrame = 0;
	m_statBarriersPerFrame = 0;
}

void D3D12Renderer::Flush(bool waitIdle)
{
	if (m_hasRecordedWork || m_drawsInCommandList > 0)
		SubmitCommandList(false);
	if (waitIdle)
		WaitForIdle();
}

void D3D12Renderer::NotifyLatteCommandProcessorIdle()
{
	if (m_submitSoon && m_hasRecordedWork)
		SubmitCommandList(false);
}

void D3D12Renderer::AppendOverlayDebugInfo()
{
	ImGui::Text("--- D3D12 debug info ---");
	ImGui::Text("Device         %s", m_selectedDeviceName.c_str());
	ImGui::Text("ShaderModel    %x", (uint32)m_highestShaderModel);
	ImGui::Text("BindingTier    %u", (uint32)m_bindingTier);
	ImGui::Text("Pipelines      %u", m_pipelineCache->GetPipelineCount());
	ImGui::Text("Samplers       %u", m_samplerCache->GetCount());
	ImGui::Text("SamplerHeap    %u / %u", m_gpuSamplerHeap->GetUsedCount(), D3D12Const::kGpuSamplerHeapSize);
	ImGui::Text("UploadRing     %uKB / %uKB", (uint32)(m_uploadRing->GetUsedBytes() / 1024), (uint32)(m_uploadRing->GetSize() / 1024));
	ImGui::Text("DeferredRel    %u", (uint32)m_deferredReleaser.GetCount());
	ImGui::Text("Draws/f        %u", m_statDrawsPerFrame);
	ImGui::Text("Submits/f      %u", m_statSubmitsPerFrame);
	ImGui::Text("Barriers/f     %u", m_statBarriersPerFrame);
	uint32 bufferCacheHeapSize = 0, bufferCacheAllocationSize = 0, bufferCacheNumAllocations = 0;
	LatteBufferCache_getStats(bufferCacheHeapSize, bufferCacheAllocationSize, bufferCacheNumAllocations);
	ImGui::Text("Buffer         %06uKB / %06uKB Allocs: %u", (bufferCacheAllocationSize + 1023) / 1024, (bufferCacheHeapSize + 1023) / 1024, bufferCacheNumAllocations);
}

/* --- imgui --- */

bool D3D12Renderer::ImguiBegin(bool mainWindow)
{
	if (!Renderer::ImguiBegin(mainWindow))
		return false;
	if (!AcquireBackbuffer(mainWindow))
		return false;
	m_imgui->EnsureFontTexture();
	ImGui_UpdateWindowInformation(mainWindow);
	ImGui::NewFrame();
	m_imguiActive = true;
	m_imguiMainWindow = mainWindow;
	return true;
}

void D3D12Renderer::ImguiEnd()
{
	ImGui::Render();
	D3D12SwapChain* swapChain = GetSwapChainObj(m_imguiMainWindow);
	if (swapChain && swapChain->IsValid())
	{
		m_imgui->Render(ImGui::GetDrawData(), swapChain->GetCurrentRTV(), D3D12SwapChain::kFormat, swapChain->GetWidth(), swapChain->GetHeight());
		swapChain->SetDefinedContent();
	}
	m_imguiActive = false;
}

ImTextureID D3D12Renderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
	// data is RGB8
	std::vector<uint8> rgba((size_t)size.x * size.y * 4);
	for (size_t i = 0; i < data.size() / 3 && i < (size_t)size.x * size.y; ++i)
	{
		rgba[i * 4 + 0] = data[i * 3 + 0];
		rgba[i * 4 + 1] = data[i * 3 + 1];
		rgba[i * 4 + 2] = data[i * 3 + 2];
		rgba[i * 4 + 3] = 0xFF;
	}
	D3D12_Checkpoint(m_device.Get(), "before GenerateTexture");
	ImTextureID texture = m_imgui->CreateTexture(rgba.data(), size.x, size.y);
	D3D12_Checkpoint(m_device.Get(), "GenerateTexture");
	return texture;
}

void D3D12Renderer::DeleteTexture(ImTextureID id)
{
	if (m_imgui)
		m_imgui->DestroyTexture(id);
}

void D3D12Renderer::DeleteFontTextures()
{
	if (m_imgui)
		m_imgui->DestroyFontTexture();
}

/* --- shaders --- */

RendererShader* D3D12Renderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader)
{
	return new RendererShaderD3D12(type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

RendererShaderD3D12* D3D12Renderer::CreateInternalShader(RendererShader::ShaderType type, const std::string& glsl)
{
	auto* shader = new RendererShaderD3D12(type, 0, 0, false, false, glsl);
	shader->PreponeCompilation(true);
	D3D12_Checkpoint(m_device.Get(), "internal shader compiled");
	return shader;
}

void D3D12Renderer::SetPushConstants(const void* data, uint32 sizeInBytes)
{
	cemu_assert(sizeInBytes <= D3D12Const::kPushConstantDwords * 4);
	m_cmdList->SetGraphicsRoot32BitConstants(D3D12Const::kRootParamPushConstants, (sizeInBytes + 3) / 4, data, 0);
}

void D3D12Renderer::WriteCBV(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_GPU_VIRTUAL_ADDRESS address, uint32 size)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
	desc.BufferLocation = address;
	desc.SizeInBytes = AlignUp<uint32>(std::max<uint32>(size, 16), D3D12Const::kCBVAlignment);
	m_device->CreateConstantBufferView(&desc, dst);
}

bool D3D12Renderer::TryGetSamplerTable(const D3D12_SAMPLER_DESC* samplers, uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE& tableOut)
{
	uint32 ids[D3D12Const::kMaxSamplersPerStage];
	D3D12_CPU_DESCRIPTOR_HANDLE handles[D3D12Const::kMaxSamplersPerStage];
	count = std::min<uint32>(count, D3D12Const::kMaxSamplersPerStage);
	if (count == 0)
	{
		// tables must point somewhere valid even if unused, use a single default sampler
		D3D12_SAMPLER_DESC def{};
		def.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		def.MaxLOD = D3D12_FLOAT32_MAX;
		def.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		handles[0] = m_samplerCache->GetSampler(def, ids[0]);
		count = 1;
	}
	else
	{
		for (uint32 i = 0; i < count; i++)
			handles[i] = m_samplerCache->GetSampler(samplers[i], ids[i]);
	}
	return m_gpuSamplerHeap->GetOrCreateTable(ids, handles, count, tableOut);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::GetSamplerTable(const D3D12_SAMPLER_DESC* samplers, uint32 count)
{
	D3D12_GPU_DESCRIPTOR_HANDLE table{};
	if (TryGetSamplerTable(samplers, count, table))
		return table;
	// shader-visible sampler heap is full. Flush everything that references it and start over
	cemuLog_logDebug(LogType::Force, "D3D12: Sampler heap full, resetting");
	SubmitCommandList(true);
	m_gpuSamplerHeap->Reset();
	bool ok = TryGetSamplerTable(samplers, count, table);
	cemu_assert(ok);
	return table;
}

/* --- render targets --- */

void D3D12Renderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
	// halfZ is handled in the vertex shader (same as Vulkan). Unlike Vulkan we don't need a negative viewport height,
	// D3D's clip space Y already points up
	D3D12_VIEWPORT vp;
	vp.TopLeftX = x;
	vp.TopLeftY = y;
	vp.Width = width;
	vp.Height = height;
	vp.MinDepth = std::clamp(nearZ, 0.0f, 1.0f);
	vp.MaxDepth = std::clamp(farZ, 0.0f, 1.0f);
	if (memcmp(&vp, &m_state.viewport, sizeof(vp)) == 0)
		return;
	m_state.viewport = vp;
	m_state.viewportDirty = true;
}

void D3D12Renderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
	D3D12_RECT r{ scissorX, scissorY, scissorX + std::max(scissorWidth, 0), scissorY + std::max(scissorHeight, 0) };
	if (memcmp(&r, &m_state.scissor, sizeof(r)) == 0)
		return;
	m_state.scissor = r;
	m_state.scissorDirty = true;
}

LatteCachedFBO* D3D12Renderer::rendertarget_createCachedFBO(uint64 key)
{
	return new CachedFBOD3D12(this, key);
}

void D3D12Renderer::rendertarget_deleteCachedFBO(LatteCachedFBO* fbo)
{
	if (m_state.activeFBO == fbo)
	{
		m_state.activeFBO = nullptr;
		m_state.renderTargetsDirty = true;
	}
}

void D3D12Renderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
	if (m_state.activeFBO != cfbo)
	{
		m_state.activeFBO = (CachedFBOD3D12*)cfbo;
		m_state.renderTargetsDirty = true;
	}
}

/* --- textures --- */

void* D3D12Renderer::texture_acquireTextureUploadBuffer(uint32 size)
{
	if (m_textureUploadBuffer.size() < size)
		m_textureUploadBuffer.resize(size);
	return m_textureUploadBuffer.data();
}

void D3D12Renderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
	cemu_assert_debug(mem == m_textureUploadBuffer.data());
}

TextureDecoder* D3D12Renderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
	D3D12TextureFormatInfo info;
	D3D12Format::GetTextureFormatInfo(m_formatSupport, format, isDepth, info);
	return info.decoder;
}

LatteTexture* D3D12Renderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
	uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	return new LatteTextureD3D12(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
}

void D3D12Renderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
	cemu_assert_debug(textureUnit < std::size(m_state.boundTexture));
	m_state.boundTexture[textureUnit] = static_cast<LatteTextureViewD3D12*>(textureView);
}

void D3D12Renderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	auto* tex = static_cast<LatteTextureD3D12*>(hostTexture);
	ID3D12Resource* resource = tex->GetResource();
	if (!resource)
		return;
	const D3D12TextureFormatInfo& fmt = tex->GetFormatInfo();
	const bool is3D = tex->Is3DTexture();
	if ((uint32)mipIndex >= tex->GetMipCount())
		return;

	TransitionTexture(tex, mipIndex, 1, is3D ? 0 : sliceIndex, 1, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();

	const D3D12_RESOURCE_DESC& desc = tex->GetDesc();
	const uint32 blockDim = fmt.isCompressed ? 4 : 1;
	const uint32 widthInBlocks = (uint32)(width + blockDim - 1) / blockDim;
	const uint32 heightInBlocks = (uint32)(height + blockDim - 1) / blockDim;

	auto copyPlane = [&](uint32 plane, const uint8* src, uint32 srcRowBytes) {
		const uint32 sub = tex->GetSubresourceIndex(mipIndex, sliceIndex, plane);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
		UINT numRows;
		UINT64 rowSize, totalBytes;
		m_device->GetCopyableFootprints(&desc, sub, 1, 0, &layout, &numRows, &rowSize, &totalBytes);
		// the guest data may be smaller than the host subresource (e.g. block alignment), copy what we have
		const uint32 copyRows = std::min<uint32>(heightInBlocks, numRows);
		const uint32 copyRowBytes = std::min<uint32>(srcRowBytes, (uint32)rowSize);
		const uint32 rowPitch = AlignUp<uint32>(copyRowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		D3D12UploadAllocation upload = AllocateUpload((uint64)rowPitch * copyRows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		for (uint32 row = 0; row < copyRows; row++)
			memcpy(upload.cpuPtr + (size_t)row * rowPitch, src + (size_t)row * srcRowBytes, copyRowBytes);
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = resource;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = sub;
		D3D12_TEXTURE_COPY_LOCATION srcLoc{};
		srcLoc.pResource = upload.resource;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint.Offset = upload.offset;
		srcLoc.PlacedFootprint.Footprint.Format = layout.Footprint.Format;
		srcLoc.PlacedFootprint.Footprint.Width = std::min<uint32>(layout.Footprint.Width, widthInBlocks * blockDim);
		srcLoc.PlacedFootprint.Footprint.Height = std::min<uint32>(layout.Footprint.Height, copyRows * blockDim);
		srcLoc.PlacedFootprint.Footprint.Depth = 1;
		srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;
		m_cmdList->CopyTextureRegion(&dst, 0, 0, is3D ? (UINT)sliceIndex : 0, &srcLoc, nullptr);
	};

	if (!fmt.hasStencil)
	{
		const uint32 rowBytes = heightInBlocks ? compressedImageSize / heightInBlocks : 0;
		copyPlane(0, (const uint8*)pixelData, rowBytes);
	}
	else
	{
		// depth/stencil formats are planar in D3D12. Split the interleaved guest data
		const uint32 texelCount = (uint32)width * (uint32)height;
		const uint32 bytesPerTexel = texelCount ? compressedImageSize / texelCount : 0;
		std::vector<uint8> depthPlane((size_t)texelCount * 4);
		std::vector<uint8> stencilPlane(texelCount);
		const uint8* src = (const uint8*)pixelData;
		if (bytesPerTexel == 4)
		{
			// D24S8: depth in the low 24 bits, stencil in the high 8 bits. Plane 0 is a 32 bit footprint with depth in the low bits
			for (uint32 i = 0; i < texelCount; i++)
			{
				uint32 v;
				memcpy(&v, src + i * 4, 4);
				uint32 d = v & 0xFFFFFF;
				memcpy(depthPlane.data() + i * 4, &d, 4);
				stencilPlane[i] = (uint8)(v >> 24);
			}
		}
		else if (bytesPerTexel == 8)
		{
			// D32F + S8 + X24
			for (uint32 i = 0; i < texelCount; i++)
			{
				memcpy(depthPlane.data() + i * 4, src + i * 8, 4);
				stencilPlane[i] = src[i * 8 + 4];
			}
		}
		else
		{
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Unexpected depth/stencil upload size");
			return;
		}
		copyPlane(0, depthPlane.data(), (uint32)width * 4);
		copyPlane(1, stencilPlane.data(), (uint32)width);
	}
	m_hasRecordedWork = true;
}

static D3D12_CPU_DESCRIPTOR_HANDLE _CreateTempRTV(D3D12Renderer* r, LatteTextureD3D12* tex, sint32 sliceIndex, sint32 mipIndex)
{
	D3D12_RENDER_TARGET_VIEW_DESC desc{};
	desc.Format = tex->GetFormatInfo().rtvFormat;
	if (tex->Is3DTexture())
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice = mipIndex;
		desc.Texture3D.FirstWSlice = sliceIndex;
		desc.Texture3D.WSize = 1;
	}
	else if (tex->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = mipIndex;
		desc.Texture1DArray.FirstArraySlice = sliceIndex;
		desc.Texture1DArray.ArraySize = 1;
	}
	else
	{
		desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice = mipIndex;
		desc.Texture2DArray.FirstArraySlice = sliceIndex;
		desc.Texture2DArray.ArraySize = 1;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE h = r->GetStagingRTVHeap().Allocate();
	r->GetDevice()->CreateRenderTargetView(tex->GetResource(), &desc, h);
	return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE _CreateTempDSV(D3D12Renderer* r, LatteTextureD3D12* tex, sint32 sliceIndex, sint32 mipIndex)
{
	D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
	desc.Format = tex->GetFormatInfo().dsvFormat;
	if (tex->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
	{
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice = mipIndex;
		desc.Texture1DArray.FirstArraySlice = sliceIndex;
		desc.Texture1DArray.ArraySize = 1;
	}
	else
	{
		desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice = mipIndex;
		desc.Texture2DArray.FirstArraySlice = sliceIndex;
		desc.Texture2DArray.ArraySize = 1;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE h = r->GetStagingDSVHeap().Allocate();
	r->GetDevice()->CreateDepthStencilView(tex->GetResource(), &desc, h);
	return h;
}

void D3D12Renderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
	auto* tex = static_cast<LatteTextureD3D12*>(hostTexture);
	if (tex->isDepth)
		texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, tex->hasStencil, 0.0f, 0);
	else
		texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
}

void D3D12Renderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
	auto* tex = static_cast<LatteTextureD3D12*>(hostTexture);
	if (tex->isDepth)
	{
		cemu_assert_suspicious();
		return;
	}
	if (!tex->GetResource())
		return;
	if (tex->GetFormatInfo().rtvFormat == DXGI_FORMAT_UNKNOWN)
	{
		// compressed formats can't be cleared through an RTV
		cemuLog_logDebugOnce(LogType::Force, "D3D12: Clear of non-renderable texture format {:04x} ignored", (uint32)tex->format);
		return;
	}
	TransitionTexture(tex, mipIndex, 1, sliceIndex, 1, D3D12_RESOURCE_STATE_RENDER_TARGET);
	FlushBarriers();
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = _CreateTempRTV(this, tex, sliceIndex, mipIndex);
	const float color[4] = { r, g, b, a };
	m_cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
	m_stagingRTVHeap->Free(rtv); // RTV descriptors are consumed at record time
	m_state.renderTargetsDirty = true;
	m_hasRecordedWork = true;
}

void D3D12Renderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
	auto* tex = static_cast<LatteTextureD3D12*>(hostTexture);
	if (!tex->isDepth)
	{
		cemu_assert_suspicious();
		return;
	}
	if (!tex->GetResource())
		return;
	D3D12_CLEAR_FLAGS flags = (D3D12_CLEAR_FLAGS)0;
	if (clearDepth)
		flags |= D3D12_CLEAR_FLAG_DEPTH;
	if (clearStencil && tex->GetFormatInfo().hasStencil)
		flags |= D3D12_CLEAR_FLAG_STENCIL;
	if (flags == 0)
		return;
	TransitionTexture(tex, mipIndex, 1, sliceIndex, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	FlushBarriers();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = _CreateTempDSV(this, tex, sliceIndex, mipIndex);
	m_cmdList->ClearDepthStencilView(dsv, flags, std::clamp(depthValue, 0.0f, 1.0f), (UINT8)stencilValue, 0, nullptr);
	m_stagingDSVHeap->Free(dsv);
	m_state.renderTargetsDirty = true;
	m_hasRecordedWork = true;
}

void D3D12Renderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth)
{
	auto* srcTex = static_cast<LatteTextureD3D12*>(src);
	auto* dstTex = static_cast<LatteTextureD3D12*>(dst);
	if (!srcTex->GetResource() || !dstTex->GetResource())
		return;
	const auto& srcFmt = srcTex->GetFormatInfo();
	const auto& dstFmt = dstTex->GetFormatInfo();
	const bool srcIs3D = srcTex->Is3DTexture();
	const bool dstIs3D = dstTex->Is3DTexture();

	TransitionTexture(srcTex, srcMip, 1, srcIs3D ? 0 : srcSlice, srcIs3D ? 1 : srcDepth, D3D12_RESOURCE_STATE_COPY_SOURCE);
	TransitionTexture(dstTex, dstMip, 1, dstIs3D ? 0 : dstSlice, dstIs3D ? 1 : srcDepth, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();

	const bool sameFamily = D3D12Format::GetTypelessFormat(srcTex->GetDesc().Format) == D3D12Format::GetTypelessFormat(dstTex->GetDesc().Format);
	const bool planeCompatible = srcFmt.hasStencil == dstFmt.hasStencil;
	const uint32 blockDimSrc = srcFmt.isCompressed ? 4 : 1;
	const uint32 blockDimDst = dstFmt.isCompressed ? 4 : 1;

	for (sint32 d = 0; d < srcDepth; d++)
	{
		const uint32 sSlice = srcSlice + d;
		const uint32 dSlice = dstSlice + d;
		if (sameFamily && planeCompatible)
		{
			for (uint32 plane = 0; plane < srcTex->GetPlaneCount(); plane++)
			{
				D3D12_TEXTURE_COPY_LOCATION s{}, t{};
				s.pResource = srcTex->GetResource();
				s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				s.SubresourceIndex = srcTex->GetSubresourceIndex(srcMip, sSlice, plane);
				t.pResource = dstTex->GetResource();
				t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				t.SubresourceIndex = dstTex->GetSubresourceIndex(dstMip, dSlice, plane);
				D3D12_BOX box;
				box.left = effectiveSrcX;
				box.top = effectiveSrcY;
				box.front = srcIs3D ? sSlice : 0;
				box.right = effectiveSrcX + effectiveCopyWidth;
				box.bottom = effectiveSrcY + effectiveCopyHeight;
				box.back = box.front + 1;
				m_cmdList->CopyTextureRegion(&t, effectiveDstX, effectiveDstY, dstIs3D ? dSlice : 0, &s, &box);
			}
			continue;
		}
		// different format families (e.g. depth -> color or RGBA8 -> R32). Bounce through a buffer, which reinterprets
		// the bits like Vulkan's vkCmdCopyImage does for size-compatible formats
		const uint32 srcBpb = D3D12Format::GetBytesPerBlock(srcTex->GetDesc().Format);
		const uint32 dstBpb = D3D12Format::GetBytesPerBlock(dstTex->GetDesc().Format);
		if (srcBpb != dstBpb || srcFmt.hasStencil || dstFmt.hasStencil)
		{
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Unsupported texture copy between formats {:04x} and {:04x}", (uint32)srcTex->format, (uint32)dstTex->format);
			continue;
		}
		const uint32 widthBlocks = (effectiveCopyWidth + blockDimSrc - 1) / blockDimSrc;
		const uint32 heightBlocks = (effectiveCopyHeight + blockDimSrc - 1) / blockDimSrc;
		const uint32 rowPitch = AlignUp<uint32>(widthBlocks * srcBpb, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		const uint32 bufferSize = rowPitch * heightBlocks;
		if (m_bufferCacheScratchSize < bufferSize)
		{
			ReleaseResourceDeferred(std::move(m_bufferCacheScratch));
			m_bufferCacheScratchSize = AlignUp<uint32>(bufferSize, 1024 * 1024);
			D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			D3D12_RESOURCE_DESC bd = D3D12_BufferDesc(m_bufferCacheScratchSize);
			D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCacheScratch)), "CreateCommittedResource (scratch)");
			m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
		}
		TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
		FlushBarriers();
		D3D12_TEXTURE_COPY_LOCATION s{}, buf{};
		s.pResource = srcTex->GetResource();
		s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		s.SubresourceIndex = srcTex->GetSubresourceIndex(srcMip, sSlice, 0);
		buf.pResource = m_bufferCacheScratch.Get();
		buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		buf.PlacedFootprint.Offset = 0;
		buf.PlacedFootprint.Footprint.Format = D3D12Format::GetTypelessFormat(srcTex->GetDesc().Format);
		buf.PlacedFootprint.Footprint.Width = widthBlocks * blockDimSrc;
		buf.PlacedFootprint.Footprint.Height = heightBlocks * blockDimSrc;
		buf.PlacedFootprint.Footprint.Depth = 1;
		buf.PlacedFootprint.Footprint.RowPitch = rowPitch;
		D3D12_BOX box{ (UINT)effectiveSrcX, (UINT)effectiveSrcY, srcIs3D ? sSlice : 0, (UINT)(effectiveSrcX + widthBlocks * blockDimSrc), (UINT)(effectiveSrcY + heightBlocks * blockDimSrc), (srcIs3D ? sSlice : 0) + 1 };
		m_cmdList->CopyTextureRegion(&buf, 0, 0, 0, &s, &box);
		TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		FlushBarriers();
		D3D12_TEXTURE_COPY_LOCATION t{};
		t.pResource = dstTex->GetResource();
		t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		t.SubresourceIndex = dstTex->GetSubresourceIndex(dstMip, dSlice, 0);
		buf.PlacedFootprint.Footprint.Format = D3D12Format::GetTypelessFormat(dstTex->GetDesc().Format);
		buf.PlacedFootprint.Footprint.Width = widthBlocks * blockDimDst;
		buf.PlacedFootprint.Footprint.Height = heightBlocks * blockDimDst;
		m_cmdList->CopyTextureRegion(&t, effectiveDstX, effectiveDstY, dstIs3D ? dSlice : 0, &buf, nullptr);
	}
	m_hasRecordedWork = true;
}

LatteTextureReadbackInfo* D3D12Renderer::texture_createReadback(LatteTextureView* textureView)
{
	auto* result = new LatteTextureReadbackInfoD3D12(this, textureView);
	if (result->GetImageSize() == 0)
	{
		delete result;
		return nullptr;
	}
	return result;
}

/* --- buffer cache --- */

static constexpr D3D12_RESOURCE_STATES kBufferCacheReadState = (D3D12_RESOURCE_STATES)((UINT)D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | (UINT)D3D12_RESOURCE_STATE_INDEX_BUFFER | (UINT)D3D12_RESOURCE_STATE_COPY_SOURCE);

void D3D12Renderer::bufferCache_init(const sint32 bufferSize)
{
	m_bufferCacheSize = (uint32)bufferSize;
	D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC desc = D3D12_BufferDesc((uint64)bufferSize);
	D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCache)), "CreateCommittedResource (buffer cache)");
	D3D12_SetDebugName(m_bufferCache.Get(), "BufferCache");
	m_bufferCacheState = D3D12_RESOURCE_STATE_COMMON;

	m_xfbRingBufferSize = AlignUp<uint32>(LatteStreamout_GetRingBufferSize() * 4, 64 * 1024); // same headroom as the Metal backend
	D3D12_RESOURCE_DESC xfbDesc = D3D12_BufferDesc(m_xfbRingBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &xfbDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_xfbRingBuffer)), "CreateCommittedResource (XFB)");
	D3D12_SetDebugName(m_xfbRingBuffer.Get(), "XfbRingBuffer");
	m_xfbRingBufferState = D3D12_RESOURCE_STATE_COMMON;
}

void D3D12Renderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
	D3D12UploadAllocation upload = AllocateUpload((uint64)size, 16);
	memcpy(upload.cpuPtr, buffer, size);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCache.Get(), bufferOffset, upload.resource, upload.offset, (UINT64)size);
	m_hasRecordedWork = true;
}

void D3D12Renderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	// copies within the same buffer need the resource in COPY_SOURCE and COPY_DEST at the same time, which D3D12 does
	// not allow. Bounce through the scratch buffer
	if (m_bufferCacheScratchSize < size)
	{
		ReleaseResourceDeferred(std::move(m_bufferCacheScratch));
		m_bufferCacheScratchSize = AlignUp<uint32>(size, 1024 * 1024);
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC bd = D3D12_BufferDesc(m_bufferCacheScratchSize);
		D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCacheScratch)), "CreateCommittedResource (scratch)");
		m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
	}
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, kBufferCacheReadState);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCacheScratch.Get(), 0, m_bufferCache.Get(), srcOffset, size);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_COPY_DEST);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCache.Get(), dstOffset, m_bufferCacheScratch.Get(), 0, size);
	m_hasRecordedWork = true;
}

void D3D12Renderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
	TransitionResource(m_xfbRingBuffer.Get(), m_xfbRingBufferState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	m_cmdList->CopyBufferRegion(m_bufferCache.Get(), dstOffset, m_xfbRingBuffer.Get(), srcOffset, size);
	m_hasRecordedWork = true;
}

void D3D12Renderer::buffer_bindVertexBuffers(std::span<BindBufferParam> bindings)
{
	const D3D12_GPU_VIRTUAL_ADDRESS base = m_bufferCache->GetGPUVirtualAddress();
	for (const auto& b : bindings)
	{
		cemu_assert_debug(b.index < LATTE_MAX_VERTEX_BUFFERS);
		D3D12_VERTEX_BUFFER_VIEW view;
		view.BufferLocation = base + b.bindOffset;
		view.SizeInBytes = std::min<uint32>(b.bindSize, m_bufferCacheSize - b.bindOffset);
		view.StrideInBytes = b.stride;
		auto& cur = m_state.vertexBuffers[b.index];
		if (cur.BufferLocation != view.BufferLocation || cur.SizeInBytes != view.SizeInBytes || cur.StrideInBytes != view.StrideInBytes)
		{
			cur = view;
			m_state.vertexBufferDirtyMask |= (1u << b.index);
		}
	}
}

void D3D12Renderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
	cemu_assert_debug(bufferIndex < LATTE_NUM_MAX_UNIFORM_BUFFERS);
	auto& stage = m_state.stage[(uint32)D3D12StageFromLatte(shaderType)];
	if (size == 0)
	{
		stage.uniformBufferAddress[bufferIndex] = 0;
		stage.uniformBufferSize[bufferIndex] = 0;
		return;
	}
	stage.uniformBufferAddress[bufferIndex] = GetAlignedUniformBufferAddress(offset, size);
	stage.uniformBufferSize[bufferIndex] = size;
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Renderer::GetAlignedUniformBufferAddress(uint32 cacheOffset, uint32 size)
{
	// CBVs must start at 256 byte boundaries. GX2 requires the same alignment for uniform blocks and the buffer cache
	// preserves it, so this is the common case
	if ((cacheOffset % D3D12Const::kCBVAlignment) == 0 && (uint64)cacheOffset + AlignUp<uint32>(size, D3D12Const::kCBVAlignment) <= m_bufferCacheSize)
		return m_bufferCache->GetGPUVirtualAddress() + cacheOffset;
	// otherwise copy the data to an aligned location in the scratch buffer
	cemuLog_logDebugOnce(LogType::Force, "D3D12: Unaligned uniform buffer at cache offset {:08x}", cacheOffset);
	const uint32 alignedSize = AlignUp<uint32>(size, D3D12Const::kCBVAlignment);
	if (!m_bufferCacheScratch || m_bufferCacheScratchSize < alignedSize * 2)
	{
		ReleaseResourceDeferred(std::move(m_bufferCacheScratch));
		m_bufferCacheScratchSize = std::max<uint32>(AlignUp<uint32>(alignedSize * 2, 1024 * 1024), 4 * 1024 * 1024);
		D3D12_HEAP_PROPERTIES heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		D3D12_RESOURCE_DESC bd = D3D12_BufferDesc(m_bufferCacheScratchSize);
		D3D12_ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bufferCacheScratch)), "CreateCommittedResource (scratch)");
		m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
		m_bufferCacheScratchOffset = 0;
	}
	if (m_bufferCacheScratchOffset + alignedSize > m_bufferCacheScratchSize)
	{
		// wrap around. Submit so previous users of the scratch area are ordered before the new copy
		SubmitCommandList(false);
		m_bufferCacheScratchOffset = 0;
	}
	const uint32 dstOffset = m_bufferCacheScratchOffset;
	m_bufferCacheScratchOffset += alignedSize;
	TransitionResource(m_bufferCache.Get(), m_bufferCacheState, kBufferCacheReadState);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
	FlushBarriers();
	const uint32 copySize = std::min<uint32>(size, m_bufferCacheSize - cacheOffset);
	m_cmdList->CopyBufferRegion(m_bufferCacheScratch.Get(), dstOffset, m_bufferCache.Get(), cacheOffset, copySize);
	TransitionResource(m_bufferCacheScratch.Get(), m_bufferCacheScratchState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	m_hasRecordedWork = true;
	return m_bufferCacheScratch->GetGPUVirtualAddress() + dstOffset;
}

/* --- index data --- */

Renderer::IndexAllocation D3D12Renderer::indexData_reserveIndexMemory(uint32 size)
{
	auto* allocation = m_indexAllocator->Allocate(size, 4);
	if (!allocation)
		return { nullptr, nullptr };
	return { allocation->cpuPtr, allocation };
}

void D3D12Renderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	m_indexAllocator->Free((D3D12UploadHeapAllocator::Allocation*)allocation.rendererInternal);
	allocation.rendererInternal = nullptr;
}

void D3D12Renderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
	// upload heap memory is write-combined and coherent, nothing to do
}

/* --- streamout --- */

void D3D12Renderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
	m_state.streamoutBuffer[bufferIndex].enabled = true;
	m_state.streamoutBuffer[bufferIndex].ringBufferOffset = ringBufferOffset;
}

void D3D12Renderer::streamout_begin()
{
	// writes happen through a UAV in the vertex shader, see draw path
}

void D3D12Renderer::streamout_rendererFinishDrawcall()
{
	// make the UAV writes visible to the copy that follows (bufferCache_copyStreamoutToMainBuffer)
	UAVBarrier(m_xfbRingBuffer.Get());
}

/* --- screenshots --- */

void D3D12Renderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)
{
	if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
		return;
	if (IsPadWindowActive())
	{
		if (m_screenshot_state == ScreenshotState::Main && padView)
			return;
		if (m_screenshot_state == ScreenshotState::Pad && !padView)
			return;
		if (m_screenshot_state == ScreenshotState::None)
			m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
		else
			m_screenshot_state = ScreenshotState::None;
	}
	else
		m_screenshot_state = ScreenshotState::None;

	auto* view = static_cast<LatteTextureViewD3D12*>(texView);
	auto* tex = view->GetBaseTexture();
	if (!tex->GetResource())
		return;
	if (view->firstMip != 0)
	{
		cemuLog_log(LogType::Force, "Failed to capture screenshot: capturing non-zero mip is not supported");
		return;
	}
	const DXGI_FORMAT typeless = D3D12Format::GetTypelessFormat(tex->GetDesc().Format);
	if (typeless != DXGI_FORMAT_R8G8B8A8_TYPELESS)
	{
		cemuLog_log(LogType::Force, "Failed to capture screenshot: texture format {:04x} is not supported by the D3D12 backend", (uint32)tex->format);
		return;
	}
	sint32 width, height;
	tex->GetEffectiveSize(width, height, 0);
	const uint32 rowPitch = AlignUp<uint32>((uint32)width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const uint64 size = (uint64)rowPitch * height;
	const uint64 offset = m_readbackRing->Allocate(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

	TransitionTexture(tex, 0, 1, view->firstSlice, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
	FlushBarriers();
	D3D12_TEXTURE_COPY_LOCATION s{}, d{};
	s.pResource = tex->GetResource();
	s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	s.SubresourceIndex = tex->GetSubresourceIndex(0, view->firstSlice);
	d.pResource = m_readbackRing->GetResource();
	d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	d.PlacedFootprint.Offset = offset;
	d.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, (UINT)width, (UINT)height, 1, rowPitch };
	D3D12_BOX box{ 0, 0, 0, (UINT)width, (UINT)height, 1 };
	m_cmdList->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
	SubmitCommandList(true);

	const bool formatIsSRGB = tex->GetFormatInfo().srvFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	const bool dstUsesSRGB = (!padView && LatteGPUState.tvBufferUsesSRGB) || (padView && LatteGPUState.drcBufferUsesSRGB);
	std::vector<uint8> rgb((size_t)width * height * 3);
	const uint8* src = m_readbackRing->GetCPUPtr(offset);
	for (sint32 y = 0; y < height; y++)
	{
		for (sint32 x = 0; x < width; x++)
		{
			const uint8* p = src + (size_t)y * rowPitch + (size_t)x * 4;
			uint8* o = rgb.data() + ((size_t)y * width + x) * 3;
			for (int c = 0; c < 3; c++)
			{
				uint8 v = p[c];
				if (formatIsSRGB && !dstUsesSRGB)
					v = SRGBComponentToRGB(v);
				else if (!formatIsSRGB && dstUsesSRGB)
					v = RGBComponentToSRGB(v);
				o[c] = v;
			}
		}
	}
	SaveScreenshot(rgb, width, height, !padView);
}

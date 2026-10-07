#pragma once

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Memory.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Format.h"
#include "util/math/vector2.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

class LatteTextureD3D12;
class LatteTextureViewD3D12;
class CachedFBOD3D12;
class RendererShaderD3D12;
class D3D12SwapChain;
class D3D12PipelineCache;
struct D3D12PipelineInfo;
class D3D12ImGuiRenderer;
class LatteQueryObjectD3D12;
struct LatteDecompilerShader;

namespace WindowSystem
{
	struct WindowHandleInfo;
}

// content of one per-stage CBV/SRV/UAV descriptor table, indexed by shader register
struct D3D12StageTableDesc
{
	D3D12_GPU_VIRTUAL_ADDRESS cbvAddress[D3D12Const::kMaxCBVsPerStage]{};
	uint32 cbvSize[D3D12Const::kMaxCBVsPerStage]{};
	D3D12_CPU_DESCRIPTOR_HANDLE srv[D3D12Const::kMaxSRVsPerStage]{};
	ID3D12Resource* uavResource = nullptr;
	uint32 uavSize = 0;
};

class D3D12Renderer : public Renderer, public D3D12SubmissionTracker
{
	friend class LatteQueryObjectD3D12;
	friend class LatteTextureReadbackInfoD3D12;
	friend class D3D12ImGuiRenderer;

public:
	// textures are uploaded through the shared upload ring; anything larger goes to a dedicated buffer
	static constexpr uint64 kUploadRingSize = 128ull * 1024 * 1024;
	static constexpr uint64 kReadbackRingSize = 64ull * 1024 * 1024;
	static constexpr uint32 kOcclusionQueryCount = 4096;
	// submit after this many draws so CPU and GPU work overlaps and the descriptor ring stays bounded
	static constexpr uint32 kDrawsPerSubmission = 2000;

	static D3D12Renderer* GetInstance();

	struct AdapterInfo
	{
		std::string name;
		uint64 luid; // stored in the config as d3d12Adapter
	};
	static std::vector<AdapterInfo> GetAdapters();

	D3D12Renderer();
	~D3D12Renderer() override;

	// --- surface management ---
	// The native handle is read from WindowSystem::GetWindowInfo().canvas_main / canvas_pad:
	//   Backend::Windows           -> surface is an HWND (desktop)
	//   Backend::UWPCoreWindow     -> surface is an IUnknown* to the app's CoreWindow
	//   Backend::UWPSwapChainPanel -> surface is unused, a composition swap chain is created and the host attaches it
	//                                 to its SwapChainPanel via ISwapChainPanelNative::SetSwapChain(GetSwapChain(...))
	void InitializeSurface(const Vector2i& size, bool mainWindow);
	void ShutdownSurface(bool mainWindow);
	void ResizeSurface(const Vector2i& size, bool mainWindow);
	IDXGISwapChain1* GetSwapChain(bool mainWindow);

	// --- Renderer interface ---
	void Initialize() override;
	void Shutdown() override;
	bool IsPadWindowActive() override;

	bool GetVRAMInfo(int& usageInMB, int& totalInMB) const override;
	void EnableDebugMode() override;

	void ClearColorbuffer(bool padView) override;
	void DrawEmptyFrame(bool mainWindow) override;
	void SwapBuffers(bool swapTV, bool swapDRC) override;

	void HandleScreenshotRequest(LatteTextureView* texView, bool padView) override;

	void DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
		sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
		bool padView, bool clearBackground) override;
	bool BeginFrame(bool mainWindow) override;

	void Flush(bool waitIdle = false) override;
	void NotifyLatteCommandProcessorIdle() override;

	bool ImguiBegin(bool mainWindow) override;
	void ImguiEnd() override;
	ImTextureID GenerateTexture(const std::vector<uint8>& data, const Vector2i& size) override;
	void DeleteTexture(ImTextureID id) override;
	void DeleteFontTextures() override;

	bool UseTFViaSSBO() const override { return true; }
	void AppendOverlayDebugInfo() override;

	void renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ = false) override;
	void renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight) override;

	LatteCachedFBO* rendertarget_createCachedFBO(uint64 key) override;
	void rendertarget_deleteCachedFBO(LatteCachedFBO* fbo) override;
	void rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo) override;

	void* texture_acquireTextureUploadBuffer(uint32 size) override;
	void texture_releaseTextureUploadBuffer(uint8* mem) override;

	TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height) override;

	void texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex) override;
	void texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize) override;
	void texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a) override;
	void texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue) override;

	LatteTexture* texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth) override;

	void texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit) override;
	void texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth) override;

	LatteTextureReadbackInfo* texture_createReadback(LatteTextureView* textureView) override;

	void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height) override;

	void bufferCache_init(const sint32 bufferSize) override;
	void bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset) override;
	void bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size) override;
	void bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size) override;

	void buffer_bindVertexBuffers(std::span<BindBufferParam> bindings) override;
	void buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size) override;

	RendererShader* shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool compileAsync, bool isGfxPackSource) override;

	void streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize) override;
	void streamout_begin() override;
	void streamout_rendererFinishDrawcall() override;

	void draw_beginSequence() override;
	void draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext) override;
	void draw_endSequence() override;

	IndexAllocation indexData_reserveIndexMemory(uint32 size) override;
	void indexData_releaseIndexMemory(IndexAllocation& allocation) override;
	void indexData_uploadIndexMemory(IndexAllocation& allocation) override;

	LatteQueryObject* occlusionQuery_create() override;
	void occlusionQuery_destroy(LatteQueryObject* queryObj) override;
	void occlusionQuery_flush() override;
	void occlusionQuery_updateState() override;

	// --- D3D12SubmissionTracker ---
	uint64 GetRecordingSubmissionId() const override { return m_lastSubmittedFenceValue + 1; }
	uint64 GetCompletedSubmissionId() override;
	void WaitForSubmission(uint64 submissionId) override;

	// --- internal API used by the other backend objects ---
	ID3D12Device* GetDevice() const { return m_device.Get(); }
	ID3D12GraphicsCommandList* GetCommandList() const { return m_cmdList.Get(); }
	const D3D12FormatSupport& GetFormatSupport() const { return m_formatSupport; }
	bool IsDebugNamingEnabled() const { return m_debugMode; }
	uint64 GenUniqueId() { return ++m_uniqueIdCounter; }

	D3D12StagingDescriptorHeap& GetStagingViewHeap() { return *m_stagingViewHeap; }
	D3D12StagingDescriptorHeap& GetStagingRTVHeap() { return *m_stagingRTVHeap; }
	D3D12StagingDescriptorHeap& GetStagingDSVHeap() { return *m_stagingDSVHeap; }
	D3D12_CPU_DESCRIPTOR_HANDLE GetNullRTV() const { return m_nullRTV; }
	ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }

	D3D12UploadAllocation AllocateUpload(uint64 size, uint64 alignment) { return m_uploadRing->Allocate(size, alignment); }
	void ReleaseResourceDeferred(ComPtr<ID3D12Resource> resource);
	void ReleaseObjectDeferred(ComPtr<IUnknown> object);

	// called from destructors so cached references can be dropped
	void NotifyTextureRelease(LatteTextureD3D12* texture);
	void NotifyTextureViewRelease(LatteTextureViewD3D12* view);
	void NotifyFBORelease(CachedFBOD3D12* fbo);

	// state tracking. Barriers are batched and flushed before the next command that needs them
	void TransitionTexture(LatteTextureD3D12* texture, uint32 firstMip, uint32 mipCount, uint32 firstSlice, uint32 sliceCount, D3D12_RESOURCE_STATES newState);
	void TransitionTextureAll(LatteTextureD3D12* texture, D3D12_RESOURCE_STATES newState);
	void TransitionResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES& trackedState, D3D12_RESOURCE_STATES newState);
	void UAVBarrier(ID3D12Resource* resource);
	void FlushBarriers();

	void SubmitCommandList(bool waitIdle = false);
	void WaitForIdle();
	void RequestSubmitSoon() { m_submitSoon = true; }

	[[noreturn]] void HandleDeviceError(HRESULT hr, const char* what);

	// used by internal shaders (surface copy, output blit, imgui)
	RendererShaderD3D12* CreateInternalShader(RendererShader::ShaderType type, const std::string& glsl);
	void BindRootSignatureAndHeaps();
	void SetPushConstants(const void* data, uint32 sizeInBytes);
	D3D12_GPU_DESCRIPTOR_HANDLE WriteStageTable(const D3D12StageTableDesc& tableDesc);
	// May submit the command list and reset the shader-visible sampler heap if it is full. All command list state is
	// lost when that happens, so call this before recording any state that the following draw depends on.
	D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerTable(const D3D12_SAMPLER_DESC* samplers, uint32 count);
	void WriteCBV(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_GPU_VIRTUAL_ADDRESS address, uint32 size);
	void InvalidateDrawState(); // forces all cached command list state to be re-applied

private:
	struct DrawState;
	struct StageBindings;

	void CreateDevice();
	void CreateRootSignature();
	void CreateNullDescriptors();
	void ResetCommandList();
	void ProcessFinishedSubmissions();

	D3D12SwapChain* GetSwapChainObj(bool mainWindow) { return mainWindow ? m_swapChainMain.get() : m_swapChainPad.get(); }
	bool AcquireBackbuffer(bool mainWindow);
	void PresentSwapChain(bool mainWindow);

	// draw path (D3D12RendererCore.cpp)
	void draw_handleSpecialState5();
	bool draw_prepareUniformVars(LatteDecompilerShader* shader, D3D12_GPU_VIRTUAL_ADDRESS& addressOut, uint32& sizeOut);
	struct PreparedStageBindings
	{
		D3D12Const::Stage stage;
		D3D12StageTableDesc tableDesc;
		D3D12_SAMPLER_DESC samplers[D3D12Const::kMaxSamplersPerStage];
		uint32 samplerCount;
		D3D12_GPU_DESCRIPTOR_HANDLE samplerTable;
	};
	void draw_buildStageBindings(LatteDecompilerShader* shader, RendererShaderD3D12* hostShader, const D3D12_GPU_VIRTUAL_ADDRESS uniformVarAddress, uint32 uniformVarSize, PreparedStageBindings& out);
	void draw_acquireSamplerTables(PreparedStageBindings* stages, uint32 count);
	bool TryGetSamplerTable(const D3D12_SAMPLER_DESC* samplers, uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE& tableOut);
	void draw_buildSamplerDesc(LatteDecompilerShader* shader, uint32 relativeTextureUnit, LatteTextureViewD3D12* view, D3D12_SAMPLER_DESC& desc);
	void draw_applyRenderTargets();
	void draw_transitionInputTextures(LatteDecompilerShader* shader);
	D3D12_CPU_DESCRIPTOR_HANDLE GetNullSRV(Latte::E_DIM dim) const;
	D3D12_GPU_VIRTUAL_ADDRESS GetAlignedUniformBufferAddress(uint32 cacheOffset, uint32 size);

	// surface copy (D3D12SurfaceCopy.cpp)
	void surfaceCopy_init();
	void surfaceCopy_shutdown();
	void surfaceCopy_notifyViewRelease(LatteTextureViewD3D12* view);

	void occlusionQuery_notifyEndCommandList();
	void occlusionQuery_notifyBeginCommandList();
	void occlusionQuery_destroyAll();
	void ProcessPadSwapChainShutdown();

	// device objects
	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGIAdapter1> m_adapter;
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_queue;
	ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent = nullptr;
	uint64 m_lastSubmittedFenceValue = 0;
	uint64 m_completedFenceValue = 0;
	bool m_debugMode = false;
	uint64 m_uniqueIdCounter = 0;
	D3D12FormatSupport m_formatSupport;
	D3D_SHADER_MODEL m_highestShaderModel = D3D_SHADER_MODEL_6_0;
	D3D12_RESOURCE_BINDING_TIER m_bindingTier = D3D12_RESOURCE_BINDING_TIER_1;

	// command recording
	struct CommandAllocatorEntry
	{
		ComPtr<ID3D12CommandAllocator> allocator;
		uint64 fenceValue;
	};
	std::deque<CommandAllocatorEntry> m_freeAllocators;
	std::deque<CommandAllocatorEntry> m_inFlightAllocators;
	ComPtr<ID3D12CommandAllocator> m_currentAllocator;
	ComPtr<ID3D12GraphicsCommandList> m_cmdList;
	std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
	uint32 m_drawsInCommandList = 0;
	bool m_submitSoon = false;
	bool m_hasRecordedWork = false;

	// root signature + heaps
	ComPtr<ID3D12RootSignature> m_rootSignature;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingViewHeap;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingSamplerHeap;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingRTVHeap;
	std::unique_ptr<D3D12StagingDescriptorHeap> m_stagingDSVHeap;
	std::unique_ptr<D3D12GpuViewHeap> m_gpuViewHeap;
	std::unique_ptr<D3D12GpuSamplerHeap> m_gpuSamplerHeap;
	std::unique_ptr<D3D12SamplerCache> m_samplerCache;
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullRTV{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullCBV{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullUAV{};
	D3D12_CPU_DESCRIPTOR_HANDLE m_nullSRV1D{}, m_nullSRV2D{}, m_nullSRV2DArray{}, m_nullSRVCubeArray{}, m_nullSRV3D{};

	// memory
	std::unique_ptr<D3D12UploadRing> m_uploadRing;
	std::unique_ptr<D3D12UploadHeapAllocator> m_indexAllocator;
	std::unique_ptr<D3D12ReadbackRing> m_readbackRing;
	D3D12DeferredReleaser m_deferredReleaser;

	ComPtr<ID3D12Resource> m_bufferCache;
	D3D12_RESOURCE_STATES m_bufferCacheState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_bufferCacheSize = 0;
	ComPtr<ID3D12Resource> m_bufferCacheScratch; // used for overlapping copies and unaligned uniform buffers
	D3D12_RESOURCE_STATES m_bufferCacheScratchState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_bufferCacheScratchSize = 0;
	uint32 m_bufferCacheScratchOffset = 0;
	ComPtr<ID3D12Resource> m_xfbRingBuffer;
	D3D12_RESOURCE_STATES m_xfbRingBufferState = D3D12_RESOURCE_STATE_COMMON;
	uint32 m_xfbRingBufferSize = 0;

	// texture upload staging memory handed to the texture loader
	std::vector<uint8> m_textureUploadBuffer;

	// swap chains
	std::unique_ptr<D3D12SwapChain> m_swapChainMain;
	std::unique_ptr<D3D12SwapChain> m_swapChainPad;
	// the pad window is closed from the UI thread while the render thread may still be using its swap chain
	std::atomic<bool> m_padShutdownRequested{ false };
	std::mutex m_padSwapChainMutex;

	// pipelines
	std::unique_ptr<D3D12PipelineCache> m_pipelineCache;

	// draw state
	struct StageBindings
	{
		D3D12_GPU_VIRTUAL_ADDRESS uniformBufferAddress[LATTE_NUM_MAX_UNIFORM_BUFFERS]{};
		uint32 uniformBufferSize[LATTE_NUM_MAX_UNIFORM_BUFFERS]{};
	};
	struct DrawState
	{
		bool skipDrawSequence = false;
		CachedFBOD3D12* activeFBO = nullptr;
		bool renderTargetsDirty = true;
		LatteTextureViewD3D12* boundTexture[LATTE_CEMU_GS_TEX_UNIT_BASE + LATTE_NUM_MAX_TEX_UNITS]{};
		D3D12_VERTEX_BUFFER_VIEW vertexBuffers[LATTE_MAX_VERTEX_BUFFERS]{};
		uint32 vertexBufferDirtyMask = 0xFFFF;
		StageBindings stage[3];
		D3D12_VIEWPORT viewport{ 0, 0, 1, 1, 0, 1 };
		D3D12_RECT scissor{ 0, 0, 1, 1 };
		bool viewportDirty = true;
		bool scissorDirty = true;
		ID3D12PipelineState* currentPSO = nullptr;
		D3D_PRIMITIVE_TOPOLOGY currentTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		D3D12_INDEX_BUFFER_VIEW currentIndexBuffer{};
		float currentBlendFactor[4]{ -1, -1, -1, -1 };
		uint32 currentStencilRef = 0xFFFFFFFF;
		bool rootSignatureBound = false;
		// streamout
		struct
		{
			bool enabled;
			uint32 ringBufferOffset;
		} streamoutBuffer[LATTE_NUM_STREAMOUT_BUFFER]{};
		uint32 verticesPerInstance = 0;
	} m_state;

	std::vector<uint8> m_uniformScratch; // CPU side staging for uniform var blocks

	// imgui
	std::unique_ptr<D3D12ImGuiRenderer> m_imgui;
	bool m_imguiActive = false;
	bool m_imguiMainWindow = true;

	// occlusion queries
	struct
	{
		ComPtr<ID3D12QueryHeap> heap;
		ComPtr<ID3D12Resource> resultBuffer; // readback heap, one uint64 per query
		uint64* results = nullptr;
		std::vector<uint32> freeIndices;
		std::vector<LatteQueryObjectD3D12*> active;
		std::vector<LatteQueryObjectD3D12*> cached;
		uint64 lastSubmission = 0;
	} m_occlusionQueries;

	// surface copy / output blit
	struct InternalPipelines;
	std::unique_ptr<InternalPipelines> m_internal;

	// stats
	uint32 m_statDrawsPerFrame = 0;
	uint32 m_statSubmitsPerFrame = 0;
	uint32 m_statBarriersPerFrame = 0;
};

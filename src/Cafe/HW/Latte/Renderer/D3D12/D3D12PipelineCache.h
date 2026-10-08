#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

class D3D12Renderer;
class CachedFBOD3D12;
class RendererShaderD3D12;
class D3D12PipelineCompileQueue;
struct D3D12PipelineCompileJob;
struct LatteFetchShader;
struct LatteDecompilerShader;

struct D3D12PipelineInfo
{
	ComPtr<ID3D12PipelineState> pso; // written once by whichever thread creates it, then published through isReady
	D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	bool usesBlendConstants = false;
	bool blendConstantAlphaOnly = false; // only CONSTANT_ALPHA factors are used, replicate alpha into RGB
	bool usesStencil = false;
	uint32 stencilRef = 0;
	std::unique_ptr<RendererShaderD3D12> rectEmulationGS; // generated geometry shader for RECTS primitives
	std::atomic<bool> isReady{ false }; // false while the PSO is still being created on a compile thread
	bool isValid() const { return isReady.load(std::memory_order_acquire) && pso != nullptr; }
};

// Translates the current Latte register state into D3D12 pipeline state objects.
// The register -> state mapping mirrors PipelineCompiler in the Vulkan backend.
class D3D12PipelineCache
{
public:
	explicit D3D12PipelineCache(D3D12Renderer* renderer);
	~D3D12PipelineCache();

	// With async compile enabled, new pipelines are created on worker threads and waited for at most 2 seconds; if the
	// driver takes longer the returned pipeline is not valid and the draw is skipped until it is ready. Driver shader
	// compilers (Intel's in particular) can take minutes for large Latte shaders, which would otherwise freeze the GPU
	// thread. Pipelines found in the disk cache are ready within milliseconds
	D3D12PipelineInfo* GetOrCreate(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
		CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType);

	// a pipeline for drawing with internal shaders (no vertex input, triangle list, single render target)
	ID3D12PipelineState* GetInternalPipeline(RendererShaderD3D12* vs, RendererShaderD3D12* ps, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, bool depthWrite, bool alphaBlend);

	uint32 GetPipelineCount() const { return (uint32)m_pipelines.size(); }
	void Clear();
	// writes newly created pipelines to the per-title disk cache (also done periodically and on destruction)
	void SaveDiskCache();

	// Creates all pipelines the title used in earlier runs while the shader cache loading screen is shown, so gameplay
	// doesn't wait for the driver. The pipelines go into the driver cache, draws load them from there. Called on the GPU
	// thread after the shader cache was loaded: BeginPreload returns the number of pipelines, UpdatePreload is called
	// repeatedly and returns false once all are done
	uint32 BeginPreload(uint64 titleId);
	bool UpdatePreload(uint32& finishedCount);
	void EndPreload();

	static uint64 CalculateHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader,
		const CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType);

private:
	// fills the job with a self-contained pipeline description, returns false if no pipeline can be created
	bool PreparePipeline(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
		CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType, D3D12PipelineCompileJob& job);

	D3D12Renderer* m_renderer;
	std::unordered_map<uint64, std::shared_ptr<D3D12PipelineInfo>> m_pipelines;
	std::shared_ptr<D3D12PipelineCompileQueue> m_compileQueue;
	void OpenDiskCaches(uint64 titleId);
	bool ResolveRecordedShaders(D3D12PipelineCompileJob& job);

	std::shared_ptr<class D3D12PipelineDiskCache> m_diskCache; // driver cache of the running title, opened on first use
	std::unique_ptr<class D3D12PipelineRecordFile> m_recordFile; // descriptions of the pipelines the title used
	bool m_diskCacheOpened = false;
	struct
	{
		size_t next = 0;
		uint32 submitted = 0;
		uint32 skipped = 0; // records whose shaders are missing (e.g. the shader cache was deleted)
		std::shared_ptr<std::atomic<uint32>> finished;
		std::chrono::steady_clock::time_point start;
	} m_preload;
	std::unordered_map<uint64, ComPtr<ID3D12PipelineState>> m_internalPipelines;
};

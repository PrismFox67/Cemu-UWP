#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

#include <atomic>
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

	static uint64 CalculateHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader,
		const CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType);

private:
	// fills the job with a self-contained pipeline description, returns false if no pipeline can be created
	bool PreparePipeline(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
		CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType, D3D12PipelineCompileJob& job);

	D3D12Renderer* m_renderer;
	std::unordered_map<uint64, std::shared_ptr<D3D12PipelineInfo>> m_pipelines;
	std::shared_ptr<D3D12PipelineCompileQueue> m_compileQueue;
	std::shared_ptr<class D3D12PipelineDiskCache> m_diskCache; // created for the running title on first use
	bool m_diskCacheOpened = false;
	std::unordered_map<uint64, ComPtr<ID3D12PipelineState>> m_internalPipelines;
};

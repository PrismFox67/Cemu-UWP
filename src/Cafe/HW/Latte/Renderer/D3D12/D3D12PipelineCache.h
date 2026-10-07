#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

#include <memory>
#include <unordered_map>

class D3D12Renderer;
class CachedFBOD3D12;
class RendererShaderD3D12;
struct LatteFetchShader;
struct LatteDecompilerShader;

struct D3D12PipelineInfo
{
	ComPtr<ID3D12PipelineState> pso;
	D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	bool usesBlendConstants = false;
	bool blendConstantAlphaOnly = false; // only CONSTANT_ALPHA factors are used, replicate alpha into RGB
	bool usesStencil = false;
	uint32 stencilRef = 0;
	std::unique_ptr<RendererShaderD3D12> rectEmulationGS; // generated geometry shader for RECTS primitives
	bool isValid() const { return pso != nullptr; }
};

// Translates the current Latte register state into D3D12 pipeline state objects.
// The register -> state mapping mirrors PipelineCompiler in the Vulkan backend.
class D3D12PipelineCache
{
public:
	explicit D3D12PipelineCache(D3D12Renderer* renderer);
	~D3D12PipelineCache();

	D3D12PipelineInfo* GetOrCreate(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
		CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType);

	// a pipeline for drawing with internal shaders (no vertex input, triangle list, single render target)
	ID3D12PipelineState* GetInternalPipeline(RendererShaderD3D12* vs, RendererShaderD3D12* ps, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, bool depthWrite, bool alphaBlend);

	uint32 GetPipelineCount() const { return (uint32)m_pipelines.size(); }
	void Clear();

	static uint64 CalculateHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader,
		const CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType);

private:
	std::unique_ptr<D3D12PipelineInfo> CreatePipeline(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
		CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType);

	D3D12Renderer* m_renderer;
	std::unordered_map<uint64, std::unique_ptr<D3D12PipelineInfo>> m_pipelines;
	std::unordered_map<uint64, ComPtr<ID3D12PipelineState>> m_internalPipelines;
};

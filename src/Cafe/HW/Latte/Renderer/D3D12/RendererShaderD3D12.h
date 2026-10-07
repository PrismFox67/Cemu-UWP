#pragma once

#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12ShaderCompiler.h"
#include "util/helpers/Semaphore.h"

// A shader stage compiled to DXIL. Compilation runs asynchronously on a small thread pool, just like RendererShaderVk.
// Compiled DXIL (plus the binding remap table) is stored in a per-title FileCache so subsequent runs skip glslang and
// spirv_to_dxil entirely.
class RendererShaderD3D12 : public RendererShader
{
	friend class _ShaderD3D12ThreadPool;

	enum class COMPILATION_STATE : uint32
	{
		NONE,
		QUEUED,
		COMPILING,
		DONE
	};

public:
	static void Init();
	static void Shutdown();

	static void ShaderCacheLoading_begin(uint64 cacheTitleId);
	static void ShaderCacheLoading_end();
	static void ShaderCacheLoading_Close();

	RendererShaderD3D12(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslCode);
	~RendererShaderD3D12() override;

	void PreponeCompilation(bool isRenderThread) override;
	bool IsCompiled() override;
	bool WaitForCompiled() override;

	// valid only once IsCompiled() is true
	bool IsValid() const { return !m_dxil.empty(); }
	D3D12_SHADER_BYTECODE GetBytecode() const { return { m_dxil.data(), m_dxil.size() }; }
	const D3D12BindingRemap& GetBindingRemap() const { return m_remap; }
	D3D12Const::Stage GetStage() const { return D3D12ShaderCompiler::StageFromShaderType(m_type); }
	uint64 GetUniqueId() const { return m_uniqueId; }

private:
	void CompileInternal();
	bool LoadFromCache();
	void StoreInCache();

	std::string m_glslCode;
	std::vector<uint8> m_dxil;
	D3D12BindingRemap m_remap;
	uint64 m_uniqueId;
	StateSemaphore<COMPILATION_STATE> m_compilationState{ COMPILATION_STATE::NONE };
};

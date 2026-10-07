#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "config/ActiveSettings.h"
#include "Cemu/FileCache/FileCache.h"
#include "util/helpers/helpers.h"

#include <thread>

extern std::atomic_int g_compiled_shaders_total;
extern std::atomic_int g_compiled_shaders_async;

namespace
{
	bool s_isLoadingShaders = false;
	FileCache* s_dxilCache = nullptr;
	std::atomic<uint64> s_nextUniqueId{ 1 };

	// bump whenever the cached data format or the compilation pipeline changes in an incompatible way
	constexpr uint32 kDXILCacheVersion = 1;
	constexpr uint32 kDXILCacheMagic = 0x4C495844; // 'DXIL'

	struct CacheHeader
	{
		uint32 magic;
		uint32 version;
		uint32 dxilSize;
		uint8 cbvCount;
		uint8 srvCount;
		uint8 uavCount;
		uint8 padding;
		sint8 cbv[D3D12BindingRemap::kMaxBinding];
		sint8 srv[D3D12BindingRemap::kMaxBinding];
		sint8 uav[D3D12BindingRemap::kMaxBinding];
	};
}

class _ShaderD3D12ThreadPool
{
public:
	void StartThreads()
	{
		if (m_threadsActive.exchange(true))
			return;
		// spirv_to_dxil is considerably more expensive than glslang alone, use a few more threads than the Vulkan backend
		const uint32 threadCount = std::clamp<uint32>(std::thread::hardware_concurrency() / 2, 2, 4);
		for (uint32 i = 0; i < threadCount; ++i)
			m_threads.emplace_back(&_ShaderD3D12ThreadPool::CompilerThreadFunc, this);
	}

	void StopThreads()
	{
		if (!m_threadsActive.exchange(false))
			return;
		for (size_t i = 0; i < m_threads.size(); ++i)
			m_queueCount.increment();
		for (auto& it : m_threads)
			it.join();
		m_threads.clear();
	}

	~_ShaderD3D12ThreadPool()
	{
		StopThreads();
	}

	void CompilerThreadFunc()
	{
		SetThreadName("d3d12ShaderComp");
		while (m_threadsActive.load(std::memory_order::relaxed))
		{
			m_queueCount.decrementWithWait();
			m_queueMutex.lock();
			if (m_queue.empty())
			{
				m_queueMutex.unlock();
				continue;
			}
			RendererShaderD3D12* job = m_queue.front();
			m_queue.pop_front();
			job->m_compilationState.setValue(RendererShaderD3D12::COMPILATION_STATE::COMPILING);
			m_queueMutex.unlock();
			job->CompileInternal();
			++g_compiled_shaders_async;
			job->m_compilationState.setValue(RendererShaderD3D12::COMPILATION_STATE::DONE);
		}
	}

	bool HasThreadsRunning() const { return m_threadsActive; }

	std::deque<RendererShaderD3D12*> m_queue;
	CounterSemaphore m_queueCount;
	std::mutex m_queueMutex;

private:
	std::vector<std::thread> m_threads;
	std::atomic<bool> m_threadsActive{ false };
} s_shaderD3D12ThreadPool;

void RendererShaderD3D12::Init()
{
	s_shaderD3D12ThreadPool.StartThreads();
}

void RendererShaderD3D12::Shutdown()
{
	s_shaderD3D12ThreadPool.StopThreads();
}

RendererShaderD3D12::RendererShaderD3D12(ShaderType type, uint64 baseHash, uint64 auxHash, bool isGameShader, bool isGfxPackShader, const std::string& glslCode)
	: RendererShader(type, baseHash, auxHash, isGameShader, isGfxPackShader), m_glslCode(glslCode)
{
	m_uniqueId = s_nextUniqueId.fetch_add(1);
	cemu_assert_debug(s_shaderD3D12ThreadPool.HasThreadsRunning());
	std::lock_guard _l(s_shaderD3D12ThreadPool.m_queueMutex);
	m_compilationState.setValue(COMPILATION_STATE::QUEUED);
	s_shaderD3D12ThreadPool.m_queue.push_back(this);
	s_shaderD3D12ThreadPool.m_queueCount.increment();
}

RendererShaderD3D12::~RendererShaderD3D12()
{
	// make sure no compiler thread still references this object
	s_shaderD3D12ThreadPool.m_queueMutex.lock();
	auto& q = s_shaderD3D12ThreadPool.m_queue;
	q.erase(std::remove(q.begin(), q.end(), this), q.end());
	s_shaderD3D12ThreadPool.m_queueMutex.unlock();
	if (m_compilationState.hasState(COMPILATION_STATE::COMPILING))
		m_compilationState.waitUntilValue(COMPILATION_STATE::DONE);
}

bool RendererShaderD3D12::LoadFromCache()
{
	if (!s_dxilCache || !m_isGameShader || m_isGfxPackShader)
		return false;
	uint64 h1, h2;
	GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, h1, h2);
	std::vector<uint8> data;
	if (!s_dxilCache->GetFile({ h1, h2 }, data))
		return false;
	if (data.size() < sizeof(CacheHeader))
		return false;
	CacheHeader header;
	memcpy(&header, data.data(), sizeof(CacheHeader));
	if (header.magic != kDXILCacheMagic || header.version != kDXILCacheVersion || data.size() != sizeof(CacheHeader) + header.dxilSize)
		return false;
	m_remap.cbvCount = header.cbvCount;
	m_remap.srvCount = header.srvCount;
	m_remap.uavCount = header.uavCount;
	memcpy(m_remap.cbv, header.cbv, sizeof(header.cbv));
	memcpy(m_remap.srv, header.srv, sizeof(header.srv));
	memcpy(m_remap.uav, header.uav, sizeof(header.uav));
	m_dxil.assign(data.begin() + sizeof(CacheHeader), data.end());
	return true;
}

void RendererShaderD3D12::StoreInCache()
{
	if (!s_dxilCache || !m_isGameShader || m_isGfxPackShader || m_dxil.empty())
		return;
	uint64 h1, h2;
	GenerateShaderPrecompiledCacheFilename(m_type, m_baseHash, m_auxHash, h1, h2);
	CacheHeader header{};
	header.magic = kDXILCacheMagic;
	header.version = kDXILCacheVersion;
	header.dxilSize = (uint32)m_dxil.size();
	header.cbvCount = m_remap.cbvCount;
	header.srvCount = m_remap.srvCount;
	header.uavCount = m_remap.uavCount;
	memcpy(header.cbv, m_remap.cbv, sizeof(header.cbv));
	memcpy(header.srv, m_remap.srv, sizeof(header.srv));
	memcpy(header.uav, m_remap.uav, sizeof(header.uav));
	std::vector<uint8> data(sizeof(CacheHeader) + m_dxil.size());
	memcpy(data.data(), &header, sizeof(CacheHeader));
	memcpy(data.data() + sizeof(CacheHeader), m_dxil.data(), m_dxil.size());
	s_dxilCache->AddFile({ h1, h2 }, data.data(), (sint32)data.size());
}

void RendererShaderD3D12::CompileInternal()
{
	if (LoadFromCache())
	{
		m_glslCode.clear();
		m_glslCode.shrink_to_fit();
		return;
	}

	auto fail = [&](const char* step, const std::string& log) {
		cemuLog_log(LogType::Force, "D3D12: {} failed for shader {:016x}_{:016x}: {}", step, m_baseHash, m_auxHash, log);
		cemuLog_logDebug(LogType::Force, "GLSL source:\n{}", m_glslCode);
		m_dxil.clear();
		m_glslCode.clear();
		m_glslCode.shrink_to_fit();
	};

	std::string log;
	std::vector<uint32> spirv;
	if (!D3D12ShaderCompiler::CompileGLSLToSPIRV(m_type, m_glslCode, spirv, log))
		return fail("GLSL to SPIR-V", log);
	if (!D3D12_RemapSpirvBindings(spirv, GetStage(), m_remap, log))
		return fail("Binding remap", log);
	if (!D3D12ShaderCompiler::CompileSPIRVToDXIL(spirv, m_type, m_dxil, log))
		return fail("SPIR-V to DXIL", log);

	StoreInCache();
	if (!s_isLoadingShaders && m_isGameShader)
		++g_compiled_shaders_total;
	m_glslCode.clear();
	m_glslCode.shrink_to_fit();
}

void RendererShaderD3D12::PreponeCompilation(bool isRenderThread)
{
	s_shaderD3D12ThreadPool.m_queueMutex.lock();
	bool isStillQueued = m_compilationState.hasState(COMPILATION_STATE::QUEUED);
	if (isStillQueued)
	{
		auto& q = s_shaderD3D12ThreadPool.m_queue;
		q.erase(std::remove(q.begin(), q.end(), this), q.end());
		m_compilationState.setValue(COMPILATION_STATE::COMPILING);
	}
	s_shaderD3D12ThreadPool.m_queueMutex.unlock();
	if (!isStillQueued)
	{
		m_compilationState.waitUntilValue(COMPILATION_STATE::DONE);
		--g_compiled_shaders_async; // stalled, so it doesn't count as async
		return;
	}
	CompileInternal();
	m_compilationState.setValue(COMPILATION_STATE::DONE);
}

bool RendererShaderD3D12::IsCompiled()
{
	return m_compilationState.hasState(COMPILATION_STATE::DONE);
}

bool RendererShaderD3D12::WaitForCompiled()
{
	m_compilationState.waitUntilValue(COMPILATION_STATE::DONE);
	return true;
}

void RendererShaderD3D12::ShaderCacheLoading_begin(uint64 cacheTitleId)
{
	if (s_dxilCache)
	{
		delete s_dxilCache;
		s_dxilCache = nullptr;
	}
	const uint32 cacheMagic = GeneratePrecompiledCacheId() ^ kDXILCacheVersion;
	const std::string cacheFilename = fmt::format("{:016x}_dxil.bin", cacheTitleId);
	const fs::path cachePath = ActiveSettings::GetCachePath("shaderCache/precompiled/{}", cacheFilename);
	s_dxilCache = FileCache::Open(cachePath, true, cacheMagic);
	if (!s_dxilCache)
		cemuLog_log(LogType::Force, "Unable to open DXIL cache {}", cacheFilename);
	s_isLoadingShaders = true;
}

void RendererShaderD3D12::ShaderCacheLoading_end()
{
	// keep the cache open, new shaders are added while the game is running
	s_isLoadingShaders = false;
}

void RendererShaderD3D12::ShaderCacheLoading_Close()
{
	delete s_dxilCache;
	s_dxilCache = nullptr;
}

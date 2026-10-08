#include "Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Format.h"
#include "Cafe/HW/Latte/Renderer/D3D12/CachedFBOD3D12.h"
#include "Cafe/HW/Latte/Renderer/D3D12/RendererShaderD3D12.h"
#include "Cafe/HW/Latte/Renderer/RendererCore.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/helpers/helpers.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>

extern std::atomic_int g_compiling_pipelines;
extern std::atomic_int g_compiling_pipelines_async;

// Driver compiled pipelines of one title, stored with ID3D12PipelineLibrary. Creating a pipeline can take the driver
// seconds (Intel), which leaves the scene black while draws are skipped; loading it from the library takes milliseconds.
// Entries are keyed by a hash of the complete description, and the runtime additionally rejects a load whose description
// doesn't match, so a stale entry only costs a cache miss. Files of another driver version are discarded.
class D3D12PipelineDiskCache
{
public:
	static std::shared_ptr<D3D12PipelineDiskCache> Open(ID3D12Device* device, const fs::path& path)
	{
		ComPtr<ID3D12Device1> device1;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
			return nullptr;
		auto cache = std::make_shared<D3D12PipelineDiskCache>();
		cache->m_path = path;
		{
			std::ifstream file(path, std::ios::binary);
			if (file)
				cache->m_blob.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}
		HRESULT hr = E_FAIL;
		if (!cache->m_blob.empty())
		{
			// the library references the blob for its whole lifetime
			hr = device1->CreatePipelineLibrary(cache->m_blob.data(), cache->m_blob.size(), IID_PPV_ARGS(&cache->m_library));
			if (FAILED(hr))
				cemuLog_log(LogType::Force, "D3D12: Discarding the pipeline cache ({}), it was created by another driver or is damaged", D3D12_HResultToString(hr));
			else
				cemuLog_log(LogType::Force, "D3D12: Loaded pipeline cache ({} KB)", cache->m_blob.size() / 1024);
		}
		if (FAILED(hr))
		{
			cache->m_blob.clear();
			hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&cache->m_library));
			if (FAILED(hr))
			{
				cemuLog_log(LogType::Force, "D3D12: Pipeline libraries are not supported ({}), pipelines are not cached", D3D12_HResultToString(hr));
				return nullptr;
			}
		}
		cache->m_lastSave = std::chrono::steady_clock::now();
		return cache;
	}

	bool Load(const std::wstring& name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ComPtr<ID3D12PipelineState>& pso)
	{
		std::lock_guard lock(m_mutex);
		return SUCCEEDED(m_library->LoadGraphicsPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pso)));
	}

	void Store(const std::wstring& name, ID3D12PipelineState* pso)
	{
		std::lock_guard lock(m_mutex);
		if (SUCCEEDED(m_library->StorePipeline(name.c_str(), pso)))
			m_dirty = true;
	}

	// save at most every 30 seconds while pipelines are being created, the app may be closed without a clean shutdown
	void SaveIfDue()
	{
		{
			std::lock_guard lock(m_mutex);
			if (!m_dirty || std::chrono::steady_clock::now() - m_lastSave < std::chrono::seconds(30))
				return;
		}
		Save();
	}

	void Save()
	{
		std::vector<uint8> data;
		{
			std::lock_guard lock(m_mutex);
			if (!m_dirty)
				return;
			data.resize(m_library->GetSerializedSize());
			if (data.empty() || FAILED(m_library->Serialize(data.data(), data.size())))
				return;
			m_dirty = false;
			m_lastSave = std::chrono::steady_clock::now();
		}
		std::lock_guard fileLock(m_fileMutex);
		std::error_code ec;
		fs::create_directories(m_path.parent_path(), ec);
		const fs::path tmpPath = m_path.string() + ".tmp";
		{
			std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
			if (!file.write((const char*)data.data(), (std::streamsize)data.size()))
				return;
		}
		fs::rename(tmpPath, m_path, ec); // replace the old file only once the new one is complete
		if (ec)
			cemuLog_log(LogType::Force, "D3D12: Failed to save the pipeline cache: {}", ec.message());
	}

private:
	std::mutex m_mutex;
	std::mutex m_fileMutex;
	fs::path m_path;
	std::vector<uint8> m_blob; // declared before m_library, which references it
	ComPtr<ID3D12PipelineLibrary> m_library;
	bool m_dirty = false;
	std::chrono::steady_clock::time_point m_lastSave;
};

// A pipeline description that no longer references any Latte or renderer object, so it can be compiled on another thread
// while the GPU thread continues (shaders may even be deleted in the meantime)
struct D3D12PipelineCompileJob
{
	std::shared_ptr<D3D12PipelineInfo> info;
	std::shared_ptr<D3D12PipelineDiskCache> diskCache;
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12RootSignature> rootSignature;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
	std::vector<uint8> vsBytecode, gsBytecode, psBytecode;
	uint64 vsHash = 0;
	uint64 psHash = 0;

	void Run()
	{
		auto toBytecode = [](const std::vector<uint8>& v) { return v.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{ v.data(), v.size() }; };
		desc.pRootSignature = rootSignature.Get();
		desc.InputLayout.pInputElementDescs = inputElements.data();
		desc.InputLayout.NumElements = (UINT)inputElements.size();
		desc.VS = toBytecode(vsBytecode);
		desc.GS = toBytecode(gsBytecode);
		desc.PS = toBytecode(psBytecode);
		std::wstring cacheName;
		if (diskCache)
		{
			const std::string key = fmt::format("{:016x}", CalculateCacheKey());
			cacheName.assign(key.begin(), key.end());
			if (diskCache->Load(cacheName, desc, info->pso))
			{
				info->isReady.store(true, std::memory_order_release);
				return;
			}
		}
		const auto start = std::chrono::steady_clock::now();
		HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&info->pso));
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		if (FAILED(hr))
		{
			cemuLog_log(LogType::Force, "D3D12: CreateGraphicsPipelineState failed ({}) for VS {:016x} PS {:016x}", D3D12_HResultToString(hr), vsHash, psHash);
			info->pso.Reset();
		}
		else
		{
			if (ms >= 2000)
				cemuLog_log(LogType::Force, "D3D12: The driver took {}ms to create the pipeline for VS {:016x} PS {:016x}", ms, vsHash, psHash);
			if (diskCache)
				diskCache->Store(cacheName, info->pso.Get());
		}
		info->isReady.store(true, std::memory_order_release);
		if (diskCache)
			diskCache->SaveIfDue();
	}

private:
	// identifies the complete pipeline description, see D3D12PipelineDiskCache
	uint64 CalculateCacheKey() const
	{
		uint64 h = 0xcbf29ce484222325ull ^ 2; // version, bump when the hashed data changes
		auto add = [&h](const void* data, size_t size) {
			const uint8* p = (const uint8*)data;
			for (size_t i = 0; i < size; i++)
				h = (h ^ p[i]) * 0x100000001b3ull;
		};
		auto addVector = [&](const std::vector<uint8>& v) {
			const uint64 size = v.size();
			add(&size, sizeof(size));
			add(v.data(), v.size());
		};
		addVector(vsBytecode);
		addVector(gsBytecode);
		addVector(psBytecode);
		for (const auto& e : inputElements)
		{
			add(e.SemanticName, strlen(e.SemanticName));
			add(&e.SemanticIndex, sizeof(e.SemanticIndex));
			add(&e.Format, sizeof(e.Format));
			add(&e.InputSlot, sizeof(e.InputSlot));
			add(&e.AlignedByteOffset, sizeof(e.AlignedByteOffset));
			add(&e.InputSlotClass, sizeof(e.InputSlotClass));
			add(&e.InstanceDataStepRate, sizeof(e.InstanceDataStepRate));
		}
		// plain value structs. Padding bytes are zero because the description is value-initialized; if they weren't,
		// the result would only be a cache miss
		add(&desc.BlendState, sizeof(desc.BlendState));
		add(&desc.SampleMask, sizeof(desc.SampleMask));
		add(&desc.RasterizerState, sizeof(desc.RasterizerState));
		add(&desc.DepthStencilState, sizeof(desc.DepthStencilState));
		add(&desc.IBStripCutValue, sizeof(desc.IBStripCutValue));
		add(&desc.PrimitiveTopologyType, sizeof(desc.PrimitiveTopologyType));
		add(&desc.NumRenderTargets, sizeof(desc.NumRenderTargets));
		add(desc.RTVFormats, sizeof(desc.RTVFormats));
		add(&desc.DSVFormat, sizeof(desc.DSVFormat));
		add(&desc.SampleDesc, sizeof(desc.SampleDesc));
		add(&desc.Flags, sizeof(desc.Flags));
		return h;
	}
};

class D3D12PipelineCompileQueue
{
public:
	static std::shared_ptr<D3D12PipelineCompileQueue> Create()
	{
		auto queue = std::make_shared<D3D12PipelineCompileQueue>();
		// pipeline creation is mostly the driver's single threaded shader compiler, so it scales with threads. Leave
		// half of the cores to emulation
		const uint32 threadCount = std::clamp<uint32>(std::thread::hardware_concurrency() / 2, 2, 4);
		for (uint32 i = 0; i < threadCount; i++)
			queue->m_threads.emplace_back([queue]() { queue->ThreadFunc(); }); // threads keep the queue alive if detached
		return queue;
	}

	void Push(std::unique_ptr<D3D12PipelineCompileJob> job)
	{
		std::lock_guard lock(m_mutex);
		m_jobs.push_back(std::move(job));
		m_jobAvailable.notify_one();
	}

	void Stop()
	{
		std::unique_lock lock(m_mutex);
		m_stop = true;
		m_jobs.clear();
		m_jobAvailable.notify_all();
		// a driver compile can take minutes or hang, it must not block closing the game
		const bool idle = m_jobFinished.wait_for(lock, std::chrono::seconds(3), [this]() { return m_busyThreads == 0; });
		lock.unlock();
		if (!idle)
			cemuLog_log(LogType::Force, "D3D12: A pipeline is still being created by the driver, not waiting for it");
		for (auto& t : m_threads)
		{
			if (idle)
				t.join();
			else
				t.detach();
		}
		m_threads.clear();
	}

private:
	void ThreadFunc()
	{
		SetThreadName("d3d12PsoComp");
		std::unique_lock lock(m_mutex);
		while (true)
		{
			m_jobAvailable.wait(lock, [this]() { return m_stop || !m_jobs.empty(); });
			if (m_stop)
				return;
			std::unique_ptr<D3D12PipelineCompileJob> job = std::move(m_jobs.front());
			m_jobs.pop_front();
			m_busyThreads++;
			lock.unlock();
			job->Run();
			job.reset();
			lock.lock();
			m_busyThreads--;
			m_jobFinished.notify_all();
		}
	}

	std::mutex m_mutex;
	std::condition_variable m_jobAvailable;
	std::condition_variable m_jobFinished;
	std::deque<std::unique_ptr<D3D12PipelineCompileJob>> m_jobs;
	std::vector<std::thread> m_threads;
	uint32 m_busyThreads = 0;
	bool m_stop = false;
};

// returned while shaders or the PSO are still compiling, never valid
static D3D12PipelineInfo s_pendingPipeline;

// GLSL geometry shader that turns the 3 vertices of a GPU7 RECT primitive into a quad. Same approach as
// rectsEmulationGS_generate() in the Vulkan backend, except that every pixel shader input is passed through: under D3D12
// the vertex shader declares all of them (LatteDecompilerOptions::declareAllPSInputs) and the signatures of all stages
// have to match
static RendererShaderD3D12* _GenerateRectEmulationGS(D3D12Renderer* renderer, LatteDecompilerShader* vertexShader, const LatteContextRegister& latteRegister)
{
	(void)vertexShader;
	(void)latteRegister;
	LatteShaderPSInputTable* psInputTable = LatteSHRC_GetPSInputTable();
	std::vector<uint32> semantics;
	std::string gsSrc;
	gsSrc.append("#version 450\r\n");
	gsSrc.append("layout(triangles) in;\r\n");
	gsSrc.append("layout(triangle_strip) out;\r\n");
	gsSrc.append("layout(max_vertices = 4) out;\r\n");
	for (sint32 i = 0; i < psInputTable->count; i++)
	{
		const auto& psImport = psInputTable->import[i];
		if (psImport.semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			continue;
		semantics.emplace_back(psImport.semanticId);
		for (sint32 f = 0; f < 2; f++)
		{
			gsSrc.append(fmt::format("layout(location = {}) ", i));
			if (psImport.isFlat)
				gsSrc.append("flat ");
			if (psImport.isNoPerspective)
				gsSrc.append("noperspective ");
			if (f == 0)
				gsSrc.append(fmt::format("in vec4 passParameterSem{}In[];\r\n", psImport.semanticId));
			else
				gsSrc.append(fmt::format("out vec4 passParameterSem{}Out;\r\n", psImport.semanticId));
		}
	}
	gsSrc.append("vec4 gen4thVertexA(vec4 a, vec4 b, vec4 c)\r\n{\r\nreturn b - (c - a);\r\n}\r\n");
	gsSrc.append("vec4 gen4thVertexB(vec4 a, vec4 b, vec4 c)\r\n{\r\nreturn c - (b - a);\r\n}\r\n");
	gsSrc.append("vec4 gen4thVertexC(vec4 a, vec4 b, vec4 c)\r\n{\r\nreturn c + (b - a);\r\n}\r\n");

	auto emitVertices = [&](sint32 p0, sint32 p1, sint32 p2, sint32 p3, const char* variant) {
		const sint32 order[4] = { p0, p1, p2, p3 };
		for (sint32 v : order)
		{
			for (uint32 sem : semantics)
			{
				if (v == 3)
					gsSrc.append(fmt::format("passParameterSem{0}Out = gen4thVertex{1}(passParameterSem{0}In[0], passParameterSem{0}In[1], passParameterSem{0}In[2]);\r\n", sem, variant));
				else
					gsSrc.append(fmt::format("passParameterSem{0}Out = passParameterSem{0}In[{1}];\r\n", sem, v));
			}
			if (v == 3)
				gsSrc.append(fmt::format("gl_Position = gen4thVertex{}(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_in[2].gl_Position);\r\n", variant));
			else
				gsSrc.append(fmt::format("gl_Position = gl_in[{}].gl_Position;\r\n", v));
			gsSrc.append("EmitVertex();\r\n");
		}
	};
	gsSrc.append("void main()\r\n{\r\n");
	gsSrc.append("float dist0_1 = length(gl_in[1].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n");
	gsSrc.append("float dist0_2 = length(gl_in[2].gl_Position.xy - gl_in[0].gl_Position.xy);\r\n");
	gsSrc.append("float dist1_2 = length(gl_in[2].gl_Position.xy - gl_in[1].gl_Position.xy);\r\n");
	gsSrc.append("if(dist0_1 > dist0_2 && dist0_1 > dist1_2)\r\n{\r\n");
	emitVertices(2, 1, 0, 3, "A");
	gsSrc.append("} else if ( dist0_2 > dist0_1 && dist0_2 > dist1_2 ) {\r\n");
	emitVertices(1, 2, 0, 3, "B");
	gsSrc.append("} else {\r\n");
	emitVertices(0, 1, 2, 3, "C");
	gsSrc.append("}\r\n}\r\n");
	return renderer->CreateInternalShader(RendererShader::ShaderType::kGeometry, gsSrc);
}

static D3D_PRIMITIVE_TOPOLOGY _GetTopology(Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE mode, D3D12_PRIMITIVE_TOPOLOGY_TYPE& typeOut, bool& isStripOut)
{
	using E = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
	isStripOut = false;
	switch (mode)
	{
	case E::POINTS:
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
	case E::LINES:
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
	case E::LINE_STRIP:
	case E::LINE_LOOP: // converted to a strip by LatteIndices
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		isStripOut = true;
		return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
	case E::LINE_STRIP_ADJACENT:
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		isStripOut = true;
		return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
	case E::TRIANGLES:
	case E::QUADS: // converted to triangles by LatteIndices
	case E::QUAD_STRIP:
	case E::RECTS: // expanded by the rect emulation geometry shader
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	case E::TRIANGLE_FAN: // D3D12 has no fans, LatteIndices reorders them into a strip (same as for Metal)
	case E::TRIANGLE_STRIP:
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		isStripOut = true;
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	default:
		cemuLog_logDebug(LogType::Force, "D3D12: Unsupported primitive mode {}", (uint32)mode);
		typeOut = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
}

D3D12PipelineCache::D3D12PipelineCache(D3D12Renderer* renderer)
	: m_renderer(renderer)
{
	m_compileQueue = D3D12PipelineCompileQueue::Create();
}

D3D12PipelineCache::~D3D12PipelineCache()
{
	m_compileQueue->Stop();
	SaveDiskCache();
	Clear();
}

void D3D12PipelineCache::SaveDiskCache()
{
	if (m_diskCache)
		m_diskCache->Save();
}

void D3D12PipelineCache::Clear()
{
	// pipelines still being created are owned by their compile job until it finishes, they were never used by the GPU
	for (auto& it : m_pipelines)
		if (it.second->isValid())
			m_renderer->ReleaseObjectDeferred(it.second->pso);
	m_pipelines.clear();
	for (auto& it : m_internalPipelines)
		m_renderer->ReleaseObjectDeferred(it.second);
	m_internalPipelines.clear();
}

uint64 D3D12PipelineCache::CalculateHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader,
	const CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType)
{
	uint32* ctxRegister = lcr.GetRawView();
	uint64 h = 0;
	auto mix = [&h](uint64 v, int rot) {
		h = std::rotl<uint64>(h, rot);
		h += v;
	};
	// vertex input layout (strides are dynamic in D3D12, only formats/offsets/slots matter)
	mix(fetchShader->key, 7);
	mix(fetchShader->getVkPipelineHashFragment(), 7);
	// shaders. Use the host shader ids, which are unique for the lifetime of the process
	auto shaderId = [](const LatteDecompilerShader* s) -> uint64 {
		if (!s || !s->shader)
			return 0;
		return static_cast<RendererShaderD3D12*>(s->shader)->GetUniqueId();
	};
	mix(shaderId(vertexShader), 13);
	mix(shaderId(geometryShader), 13);
	mix(shaderId(pixelShader), 13);
	// primitive & index type (strip cut value)
	mix(ctxRegister[mmVGT_PRIMITIVE_TYPE], 7);
	mix((uint64)indexType, 3);
	mix(ctxRegister[mmVGT_STRMOUT_EN], 3);
	// rasterizer
	const uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	mix(polygonCtrl, 7);
	mix(ctxRegister[Latte::REGADDR::PA_CL_CLIP_CNTL], 7);
	mix(ctxRegister[Latte::REGADDR::PA_CL_VTE_CNTL], 7); // VPORT_X_OFFSET_ENA affects the rasterization kill workaround
	if (polygonCtrl & (1 << 11))
	{
		// D3D12 depth bias is static pipeline state
		mix(lcr.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue(), 11);
		mix(lcr.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue(), 11);
		mix(lcr.PA_SU_POLY_OFFSET_CLAMP.getRawValue(), 11);
	}
	// blend
	const uint32 colorControl = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
	mix(colorControl, 7);
	mix(ctxRegister[Latte::REGADDR::CB_TARGET_MASK], 7);
	const uint32 blendEnableMask = (colorControl >> 8) & 0xFF;
	for (uint32 i = 0; i < 8; i++)
	{
		if (blendEnableMask & (1 << i))
			mix(ctxRegister[Latte::REGADDR::CB_BLEND0_CONTROL + i], 7);
	}
	// depth/stencil (stencil ref is dynamic, masks are static)
	uint32 depthControl = ctxRegister[Latte::REGADDR::DB_DEPTH_CONTROL];
	if (depthControl & 1)
	{
		mix(ctxRegister[mmDB_STENCILREFMASK] & 0xFFFFFF00, 17);
		if (depthControl & (1 << 7))
			mix(ctxRegister[mmDB_STENCILREFMASK_BF] & 0xFFFFFF00, 13);
	}
	else
		depthControl &= 0xFF;
	mix(depthControl, 17);
	// render target formats
	mix(fbo->GetFormatHash(), 17);
	return h;
}

D3D12PipelineInfo* D3D12PipelineCache::GetOrCreate(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
	CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType)
{
	const uint64 hash = CalculateHash(fetchShader, vertexShader, geometryShader, pixelShader, fbo, lcr, indexType);
	auto it = m_pipelines.find(hash);
	if (it != m_pipelines.end())
		return it->second.get();

	const bool async = GetConfig().async_compile.GetValue();
	for (LatteDecompilerShader* shader : { vertexShader, geometryShader, pixelShader })
	{
		auto* s = shader ? static_cast<RendererShaderD3D12*>(shader->shader) : nullptr;
		if (!s)
			continue;
		if (!async)
			s->PreponeCompilation(true);
		else if (!s->IsCompiled())
			return &s_pendingPipeline; // skip the draw, the pipeline is created once the shaders are done
	}

	if (!m_diskCacheOpened)
	{
		m_diskCacheOpened = true;
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		m_diskCache = D3D12PipelineDiskCache::Open(m_renderer->GetDevice(), ActiveSettings::GetCachePath("shaderCache/driver/d3d12/{:016x}.bin", titleId));
	}

	auto job = std::make_unique<D3D12PipelineCompileJob>();
	job->info = std::make_shared<D3D12PipelineInfo>();
	job->diskCache = m_diskCache;
	D3D12PipelineInfo* result = job->info.get();
	m_pipelines.emplace(hash, job->info);
	if (!PreparePipeline(fetchShader, vertexShader, geometryShader, pixelShader, fbo, lcr, indexType, *job))
	{
		result->isReady.store(true, std::memory_order_release); // stays invalid, the draws are skipped
		return result;
	}
	g_compiling_pipelines++;
	if (async)
	{
		g_compiling_pipelines_async++;
		m_compileQueue->Push(std::move(job));
	}
	else
		job->Run();
	return result;
}

bool D3D12PipelineCache::PreparePipeline(const LatteFetchShader* fetchShader, LatteDecompilerShader* vertexShader, LatteDecompilerShader* geometryShader, LatteDecompilerShader* pixelShader,
	CachedFBOD3D12* fbo, const LatteContextRegister& lcr, Renderer::INDEX_TYPE indexType, D3D12PipelineCompileJob& job)
{
	D3D12PipelineInfo* info = job.info.get();
	const auto& support = m_renderer->GetFormatSupport();

	auto* vsHost = vertexShader ? static_cast<RendererShaderD3D12*>(vertexShader->shader) : nullptr;
	auto* gsHost = geometryShader ? static_cast<RendererShaderD3D12*>(geometryShader->shader) : nullptr;
	auto* psHost = pixelShader ? static_cast<RendererShaderD3D12*>(pixelShader->shader) : nullptr;
	if (!vsHost || !vsHost->IsValid() || (gsHost && !gsHost->IsValid()) || (psHost && !psHost->IsValid()))
	{
		cemuLog_logDebug(LogType::Force, "D3D12: Pipeline creation skipped due to invalid shader(s)");
		return false;
	}

	const auto primitiveMode = lcr.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
	const bool isPrimitiveRect = primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS;
	if (isPrimitiveRect && !gsHost)
	{
		info->rectEmulationGS.reset(_GenerateRectEmulationGS(m_renderer, vertexShader, lcr));
		if (!info->rectEmulationGS->IsValid())
			return false;
		gsHost = info->rectEmulationGS.get();
	}

	// the job owns copies of everything the description points to (see D3D12PipelineCompileJob::Run)
	job.device = m_renderer->GetDevice();
	job.rootSignature = m_renderer->GetRootSignature();
	job.vsHash = vertexShader ? vertexShader->baseHash : 0;
	job.psHash = pixelShader ? pixelShader->baseHash : 0;
	auto copyBytecode = [](RendererShaderD3D12* s, std::vector<uint8>& out) {
		const D3D12_SHADER_BYTECODE bc = s->GetBytecode();
		out.assign((const uint8*)bc.pShaderBytecode, (const uint8*)bc.pShaderBytecode + bc.BytecodeLength);
	};
	copyBytecode(vsHost, job.vsBytecode);
	if (gsHost)
		copyBytecode(gsHost, job.gsBytecode);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc = job.desc;

	// --- input layout ---
	std::vector<D3D12_INPUT_ELEMENT_DESC>& inputElements = job.inputElements;
	inputElements.reserve(16);
	for (auto& bufferGroup : fetchShader->bufferGroups)
	{
		for (sint32 j = 0; j < bufferGroup.attribCount; ++j)
		{
			auto& attr = bufferGroup.attrib[j];
			sint32 location = vertexShader->resourceMapping.attributeMapping[attr.semanticId];
			if (location < 0)
				continue;
			D3D12_INPUT_ELEMENT_DESC e{};
			// the HLSL translation (and spirv_to_dxil) name vertex inputs TEXCOORD<location>
			e.SemanticName = "TEXCOORD";
			e.SemanticIndex = (UINT)location;
			e.Format = D3D12Format::GetVertexFormat(attr.format);
			e.InputSlot = attr.attributeBufferIndex;
			e.AlignedByteOffset = attr.offset;
			if (attr.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA)
			{
				e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
				e.InstanceDataStepRate = (UINT)std::max(1, attr.aluDivisor); // D3D12 supports arbitrary divisors natively
			}
			else
			{
				e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				e.InstanceDataStepRate = 0;
			}
			inputElements.push_back(e);
		}
	}

	// --- input assembly ---
	D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType;
	bool isStrip;
	info->topology = _GetTopology(primitiveMode, topologyType, isStrip);
	desc.PrimitiveTopologyType = topologyType;
	desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	if (isStrip)
	{
		if (indexType == Renderer::INDEX_TYPE::U16)
			desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
		else if (indexType == Renderer::INDEX_TYPE::U32)
			desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
	}

	// --- rasterizer ---
	const auto& polygonControlReg = lcr.PA_SU_SC_MODE_CNTL;
	const auto frontFace = polygonControlReg.get_FRONT_FACE();
	uint32 cullFront = polygonControlReg.get_CULL_FRONT();
	uint32 cullBack = polygonControlReg.get_CULL_BACK();
	const bool polyOffsetFrontEnable = polygonControlReg.get_OFFSET_FRONT_ENABLED();
	if (isPrimitiveRect)
	{
		if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
			cullFront = cullBack;
		else
			cullBack = cullFront;
	}
	bool rasterizerDiscard = lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL();
	if (!lcr.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerDiscard = false; // GX2SetSpecialState(0, true) workaround, same as Vulkan
	// D3D12 can't cull front and back faces at the same time. Both that and rasterizer discard are emulated by
	// dropping the pixel shader and disabling all render target, depth and stencil writes
	const bool discardAllFragments = rasterizerDiscard || (cullFront && cullBack);

	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	if (cullFront && !cullBack)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	else if (cullBack && !cullFront)
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
	else
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	// Cemu's Vulkan backend uses a Y-flipped viewport and D3D12 uses the native orientation, both result in the same
	// framebuffer positions and therefore the same visual winding
	desc.RasterizerState.FrontCounterClockwise = (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW) ? TRUE : FALSE;
	if (polyOffsetFrontEnable)
	{
		const float frontScale = lcr.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f;
		const float frontOffset = lcr.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
		const float offsetClamp = lcr.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();
		desc.RasterizerState.DepthBias = (INT)std::lround(frontOffset);
		desc.RasterizerState.SlopeScaledDepthBias = frontScale;
		desc.RasterizerState.DepthBiasClamp = offsetClamp;
	}
	const bool zClipEnable = lcr.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() == false;
	desc.RasterizerState.DepthClipEnable = zClipEnable ? TRUE : FALSE;
	desc.RasterizerState.MultisampleEnable = FALSE;
	desc.RasterizerState.AntialiasedLineEnable = FALSE;
	desc.RasterizerState.ForcedSampleCount = 0;
	desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	if (!discardAllFragments && psHost)
		copyBytecode(psHost, job.psBytecode);

	// --- blend ---
	const Latte::LATTE_CB_COLOR_CONTROL& colorControlReg = lcr.CB_COLOR_CONTROL;
	const uint32 blendEnableMask = colorControlReg.get_BLEND_MASK();
	const uint32 renderTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	const auto logicOp = colorControlReg.get_ROP();
	bool useLogicOp = false;
	D3D12_LOGIC_OP d3dLogicOp = D3D12_LOGIC_OP_COPY;
	bool clearViaBlend = false;
	if (logicOp != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY)
	{
		switch (logicOp)
		{
		case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::SET:
			d3dLogicOp = D3D12_LOGIC_OP_SET;
			break;
		case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR:
			d3dLogicOp = D3D12_LOGIC_OP_CLEAR;
			break;
		case Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::OR:
			d3dLogicOp = D3D12_LOGIC_OP_OR;
			break;
		default:
			cemu_assert_unimplemented();
			break;
		}
		// D3D12 logic ops only apply to UINT render targets
		bool allInteger = fbo->GetNumRTVs() > 0;
		for (uint32 i = 0; i < fbo->GetNumRTVs(); i++)
			if (fbo->GetRTVFormat(i) != DXGI_FORMAT_UNKNOWN && !fbo->IsColorSlotInteger(i))
				allInteger = false;
		if (support.logicOp && allInteger)
			useLogicOp = true;
		else if (logicOp == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR)
			clearViaBlend = true; // src*0 + dst*0
		else
			cemuLog_logDebugOnce(LogType::Force, "D3D12: Logic op {} is not supported for the bound render targets", (uint32)logicOp);
	}

	desc.BlendState.AlphaToCoverageEnable = FALSE;
	desc.BlendState.IndependentBlendEnable = useLogicOp ? FALSE : TRUE;
	bool usesConstantColor = false;
	bool usesConstantAlpha = false;
	bool usesConstantInAlphaSlot = false;
	for (uint32 i = 0; i < 8; i++)
	{
		auto& rt = desc.BlendState.RenderTarget[i];
		rt.RenderTargetWriteMask = discardAllFragments ? 0 : (UINT8)((renderTargetMask >> (i * 4)) & 0xF);
		if (fbo->GetRTVFormat(i) == DXGI_FORMAT_UNKNOWN)
			rt.RenderTargetWriteMask = 0;
		rt.LogicOp = D3D12_LOGIC_OP_NOOP;
		rt.SrcBlend = D3D12_BLEND_ONE;
		rt.DestBlend = D3D12_BLEND_ZERO;
		rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_ZERO;
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		if (useLogicOp)
		{
			if (i == 0)
			{
				rt.LogicOpEnable = TRUE;
				rt.LogicOp = d3dLogicOp;
			}
			continue;
		}
		if (clearViaBlend)
		{
			rt.BlendEnable = TRUE;
			rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
			continue;
		}
		if ((blendEnableMask & (1 << i)) == 0 || fbo->IsColorSlotInteger(i) || fbo->GetRTVFormat(i) == DXGI_FORMAT_UNKNOWN)
			continue;
		const auto& blendControlReg = lcr.CB_BLENDN_CONTROL[i];
		rt.BlendEnable = TRUE;
		auto colorSrc = blendControlReg.get_COLOR_SRCBLEND();
		auto colorDst = blendControlReg.get_COLOR_DSTBLEND();
		auto colorOp = blendControlReg.get_COLOR_COMB_FCN();
		auto alphaSrc = colorSrc;
		auto alphaDst = colorDst;
		auto alphaOp = colorOp;
		if (blendControlReg.get_SEPARATE_ALPHA_BLEND())
		{
			alphaSrc = blendControlReg.get_ALPHA_SRCBLEND();
			alphaDst = blendControlReg.get_ALPHA_DSTBLEND();
			alphaOp = blendControlReg.get_ALPHA_COMB_FCN();
		}
		rt.SrcBlend = D3D12Format::GetBlendFactor(colorSrc, false);
		rt.DestBlend = D3D12Format::GetBlendFactor(colorDst, false);
		rt.BlendOp = D3D12Format::GetBlendOp(colorOp);
		rt.SrcBlendAlpha = D3D12Format::GetBlendFactor(alphaSrc, true);
		rt.DestBlendAlpha = D3D12Format::GetBlendFactor(alphaDst, true);
		rt.BlendOpAlpha = D3D12Format::GetBlendOp(alphaOp);
		// in the color slots CONSTANT_COLOR and CONSTANT_ALPHA need different blend factors. In the alpha slots both read
		// the alpha component of the blend factor, so they never conflict
		for (auto f : { colorSrc, colorDst })
		{
			if (D3D12Format::IsBlendFactorConstantAlpha(f))
				usesConstantAlpha = true;
			else if (D3D12Format::IsBlendFactorConstant(f))
				usesConstantColor = true;
		}
		for (auto f : { alphaSrc, alphaDst })
		{
			if (D3D12Format::IsBlendFactorConstant(f))
				usesConstantInAlphaSlot = true;
		}
	}
	info->usesBlendConstants = usesConstantColor || usesConstantAlpha || usesConstantInAlphaSlot;
	info->blendConstantAlphaOnly = usesConstantAlpha && !usesConstantColor;
	if (usesConstantAlpha && usesConstantColor)
		cemuLog_logDebugOnce(LogType::Force, "D3D12: Pipeline mixes constant color and constant alpha blend factors, results may be inaccurate");
	desc.SampleMask = 0xFFFFFFFF;

	// --- depth/stencil ---
	const auto& depthControl = lcr.DB_DEPTH_CONTROL;
	const bool hasDSV = fbo->HasDSV();
	auto& ds = desc.DepthStencilState;
	ds.DepthEnable = (hasDSV && !discardAllFragments && depthControl.get_Z_ENABLE()) ? TRUE : FALSE;
	ds.DepthWriteMask = depthControl.get_Z_WRITE_ENABLE() ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = D3D12Format::GetCompareFunc((uint32)depthControl.get_Z_FUNC());
	const bool stencilEnable = hasDSV && !discardAllFragments && depthControl.get_STENCIL_ENABLE() && fbo->GetDepthView() && fbo->GetDSVFormat() != DXGI_FORMAT_D32_FLOAT && fbo->GetDSVFormat() != DXGI_FORMAT_D16_UNORM;
	ds.StencilEnable = stencilEnable ? TRUE : FALSE;
	info->usesStencil = stencilEnable;
	if (stencilEnable)
	{
		const auto& refMaskF = lcr.DB_STENCILREFMASK;
		const auto& refMaskB = lcr.DB_STENCILREFMASK_BF;
		ds.StencilReadMask = (UINT8)refMaskF.get_STENCILMASK_F();
		ds.StencilWriteMask = (UINT8)refMaskF.get_STENCILWRITEMASK_F();
		info->stencilRef = refMaskF.get_STENCILREF_F();
		ds.FrontFace.StencilFunc = D3D12Format::GetCompareFunc((uint32)depthControl.get_STENCIL_FUNC_F());
		ds.FrontFace.StencilPassOp = D3D12Format::GetStencilOp((uint32)depthControl.get_STENCIL_ZPASS_F());
		ds.FrontFace.StencilDepthFailOp = D3D12Format::GetStencilOp((uint32)depthControl.get_STENCIL_ZFAIL_F());
		ds.FrontFace.StencilFailOp = D3D12Format::GetStencilOp((uint32)depthControl.get_STENCIL_FAIL_F());
		if (depthControl.get_BACK_STENCIL_ENABLE())
		{
			ds.BackFace.StencilFunc = D3D12Format::GetCompareFunc((uint32)depthControl.get_STENCIL_FUNC_B());
			ds.BackFace.StencilPassOp = D3D12Format::GetStencilOp((uint32)depthControl.get_STENCIL_ZPASS_B());
			ds.BackFace.StencilDepthFailOp = D3D12Format::GetStencilOp((uint32)depthControl.get_STENCIL_ZFAIL_B());
			ds.BackFace.StencilFailOp = D3D12Format::GetStencilOp((uint32)depthControl.get_STENCIL_FAIL_B());
			// without the Agility SDK D3D12 has one set of masks and one reference value for both faces
			if (refMaskB.get_STENCILMASK_B() != refMaskF.get_STENCILMASK_F() || refMaskB.get_STENCILWRITEMASK_B() != refMaskF.get_STENCILWRITEMASK_F() || refMaskB.get_STENCILREF_B() != refMaskF.get_STENCILREF_F())
				cemuLog_logDebugOnce(LogType::Force, "D3D12: Separate back face stencil masks/reference are not supported, using front face values");
		}
		else
			ds.BackFace = ds.FrontFace;
	}
	else
	{
		ds.StencilReadMask = 0xFF;
		ds.StencilWriteMask = 0xFF;
		ds.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
		ds.BackFace = ds.FrontFace;
	}

	// --- render targets ---
	desc.NumRenderTargets = fbo->GetNumRTVs();
	for (uint32 i = 0; i < 8; i++)
		desc.RTVFormats[i] = (i < desc.NumRenderTargets) ? fbo->GetRTVFormat(i) : DXGI_FORMAT_UNKNOWN;
	desc.DSVFormat = hasDSV ? fbo->GetDSVFormat() : DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.NodeMask = 0;
	desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
	return true;
}

ID3D12PipelineState* D3D12PipelineCache::GetInternalPipeline(RendererShaderD3D12* vs, RendererShaderD3D12* ps, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, bool depthWrite, bool alphaBlend)
{
	uint64 key = vs->GetUniqueId();
	key = key * 0x9E3779B97F4A7C15ull + ps->GetUniqueId();
	key = key * 0x9E3779B97F4A7C15ull + (uint64)rtvFormat;
	key = key * 0x9E3779B97F4A7C15ull + (uint64)dsvFormat;
	key = key * 0x9E3779B97F4A7C15ull + (depthWrite ? 1 : 0) + (alphaBlend ? 2 : 0);
	auto it = m_internalPipelines.find(key);
	if (it != m_internalPipelines.end())
		return it->second.Get();

	vs->PreponeCompilation(true);
	ps->PreponeCompilation(true);
	if (!vs->IsValid() || !ps->IsValid())
		return nullptr;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = m_renderer->GetRootSignature();
	desc.VS = vs->GetBytecode();
	desc.PS = ps->GetBytecode();
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	auto& rt = desc.BlendState.RenderTarget[0];
	rt.RenderTargetWriteMask = (rtvFormat != DXGI_FORMAT_UNKNOWN) ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
	rt.BlendEnable = alphaBlend ? TRUE : FALSE;
	rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D12_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	desc.SampleMask = 0xFFFFFFFF;
	desc.DepthStencilState.DepthEnable = depthWrite ? TRUE : FALSE;
	desc.DepthStencilState.DepthWriteMask = depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.NumRenderTargets = (rtvFormat != DXGI_FORMAT_UNKNOWN) ? 1 : 0;
	desc.RTVFormats[0] = rtvFormat;
	desc.DSVFormat = dsvFormat;
	desc.SampleDesc.Count = 1;

	ComPtr<ID3D12PipelineState> pso;
	HRESULT hr = m_renderer->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
	if (FAILED(hr))
	{
		cemuLog_log(LogType::Force, "D3D12: Failed to create internal pipeline: {}", D3D12_HResultToString(hr));
		return nullptr;
	}
	ID3D12PipelineState* result = pso.Get();
	m_internalPipelines.emplace(key, std::move(pso));
	return result;
}

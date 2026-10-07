#include "Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.h"

#include <cstring>

/* D3D12StagingDescriptorHeap */

D3D12StagingDescriptorHeap::D3D12StagingDescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 descriptorsPerBlock, const char* debugName)
	: m_device(device), m_type(type), m_descriptorsPerBlock(descriptorsPerBlock), m_debugName(debugName)
{
	m_incrementSize = device->GetDescriptorHandleIncrementSize(type);
}

void D3D12StagingDescriptorHeap::AddBlock()
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = m_type;
	desc.NumDescriptors = m_descriptorsPerBlock;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ComPtr<ID3D12DescriptorHeap> heap;
	D3D12_ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap (staging)");
	D3D12_SetDebugName(heap.Get(), fmt::format("{}_block{}", m_debugName, m_blocks.size()));
	D3D12_CPU_DESCRIPTOR_HANDLE base = heap->GetCPUDescriptorHandleForHeapStart();
	// push in reverse so allocations come out in ascending order
	for (uint32 i = m_descriptorsPerBlock; i > 0; i--)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = base;
		h.ptr += (SIZE_T)(i - 1) * m_incrementSize;
		m_freeList.push_back(h);
	}
	m_blocks.emplace_back(std::move(heap));
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12StagingDescriptorHeap::Allocate()
{
	std::lock_guard _l(m_mutex);
	if (m_freeList.empty())
		AddBlock();
	D3D12_CPU_DESCRIPTOR_HANDLE h = m_freeList.back();
	m_freeList.pop_back();
	m_allocatedCount++;
	return h;
}

void D3D12StagingDescriptorHeap::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	if (handle.ptr == 0)
		return;
	std::lock_guard _l(m_mutex);
	m_freeList.push_back(handle);
	m_allocatedCount--;
}

/* D3D12GpuViewHeap */

D3D12GpuViewHeap::D3D12GpuViewHeap(ID3D12Device* device, D3D12SubmissionTracker* tracker, uint32 descriptorCount)
	: m_device(device), m_tracker(tracker), m_size(descriptorCount)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = descriptorCount;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)), "CreateDescriptorHeap (shader visible views)");
	D3D12_SetDebugName(m_heap.Get(), "GpuViewHeap");
	m_incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_cpuBase = m_heap->GetCPUDescriptorHandleForHeapStart();
	m_gpuBase = m_heap->GetGPUDescriptorHandleForHeapStart();
}

D3D12GpuViewHeap::Table D3D12GpuViewHeap::AllocateTable(uint32 count)
{
	cemu_assert(count > 0 && count < m_size);
	auto tryAllocate = [&](uint32& startOut) -> bool {
		if (m_markers.empty() && !m_recordingHasData)
		{
			m_head = 0;
			m_tail = 0;
			startOut = 0;
			return true;
		}
		if (m_head == m_tail)
			return false;
		if (m_head > m_tail)
		{
			if (m_head + count <= m_size)
			{
				startOut = m_head;
				return true;
			}
			if (count < m_tail)
			{
				startOut = 0;
				return true;
			}
			return false;
		}
		if (m_head + count < m_tail)
		{
			startOut = m_head;
			return true;
		}
		return false;
	};
	uint32 start = 0;
	bool success = tryAllocate(start);
	while (!success && !m_markers.empty())
	{
		m_tracker->WaitForSubmission(m_markers.front().submissionId);
		Retire(m_tracker->GetCompletedSubmissionId());
		success = tryAllocate(start);
	}
	if (!success)
	{
		// the recording command list alone uses up the entire heap. This should be impossible with a sane heap size
		// since the renderer submits after a bounded number of draws
		cemuLog_log(LogType::Force, "D3D12: Shader visible descriptor heap exhausted by a single command list");
		cemu_assert(false);
	}
	m_head = start + count;
	m_recordingHasData = true;
	Table t;
	t.cpu = OffsetCPU(m_cpuBase, start);
	t.gpu = OffsetGPU(m_gpuBase, start);
	return t;
}

void D3D12GpuViewHeap::OnSubmit(uint64 submissionId)
{
	if (!m_recordingHasData)
		return;
	m_markers.push_back({ submissionId, m_head });
	m_recordingHasData = false;
}

void D3D12GpuViewHeap::Retire(uint64 completedSubmissionId)
{
	while (!m_markers.empty() && m_markers.front().submissionId <= completedSubmissionId)
	{
		m_tail = m_markers.front().end;
		m_markers.pop_front();
	}
}

/* D3D12GpuSamplerHeap */

D3D12GpuSamplerHeap::D3D12GpuSamplerHeap(ID3D12Device* device, uint32 descriptorCount)
	: m_device(device), m_size(descriptorCount)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	desc.NumDescriptors = descriptorCount;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12_ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)), "CreateDescriptorHeap (shader visible samplers)");
	D3D12_SetDebugName(m_heap.Get(), "GpuSamplerHeap");
	m_incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	m_cpuBase = m_heap->GetCPUDescriptorHandleForHeapStart();
	m_gpuBase = m_heap->GetGPUDescriptorHandleForHeapStart();
}

bool D3D12GpuSamplerHeap::GetOrCreateTable(const uint32* samplerIds, const D3D12_CPU_DESCRIPTOR_HANDLE* srcSamplers, uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE& tableOut)
{
	cemu_assert_debug(count > 0 && count <= D3D12Const::kMaxSamplersPerStage);
	TableKey key;
	key.count = count;
	for (uint32 i = 0; i < count; i++)
		key.ids[i] = samplerIds[i];
	auto it = m_tableCache.find(key);
	if (it != m_tableCache.end())
	{
		tableOut = it->second;
		return true;
	}
	if (m_head + count > m_size)
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cpuBase;
	dst.ptr += (SIZE_T)m_head * m_incrementSize;
	for (uint32 i = 0; i < count; i++)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE d = dst;
		d.ptr += (SIZE_T)i * m_incrementSize;
		m_device->CopyDescriptorsSimple(1, d, srcSamplers[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	}
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_gpuBase;
	gpu.ptr += (UINT64)m_head * m_incrementSize;
	m_head += count;
	m_tableCache.emplace(key, gpu);
	tableOut = gpu;
	return true;
}

void D3D12GpuSamplerHeap::Reset()
{
	m_head = 0;
	m_tableCache.clear();
}

/* D3D12SamplerCache */

uint64 D3D12SamplerCache::HashDesc(const D3D12_SAMPLER_DESC& desc)
{
	// FNV-1a over the raw bytes. D3D12_SAMPLER_DESC has no padding (all members are 4 byte sized)
	static_assert(sizeof(D3D12_SAMPLER_DESC) % 4 == 0);
	const uint8* p = (const uint8*)&desc;
	uint64 h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < sizeof(D3D12_SAMPLER_DESC); i++)
	{
		h ^= p[i];
		h *= 0x100000001B3ull;
	}
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SamplerCache::GetSampler(const D3D12_SAMPLER_DESC& desc, uint32& idOut)
{
	uint64 hash = HashDesc(desc);
	auto range = m_samplers.equal_range(hash);
	for (auto it = range.first; it != range.second; ++it)
	{
		if (memcmp(&it->second.desc, &desc, sizeof(D3D12_SAMPLER_DESC)) == 0)
		{
			idOut = it->second.id;
			return it->second.handle;
		}
	}
	Entry e;
	e.desc = desc;
	e.handle = m_stagingHeap->Allocate();
	e.id = (uint32)m_samplers.size() + 1;
	m_device->CreateSampler(&desc, e.handle);
	m_samplers.emplace(hash, e);
	idOut = e.id;
	return e.handle;
}

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Memory.h"

#include <unordered_map>

// CPU-only (non shader-visible) descriptor heap with a free list.
// Used for RTVs, DSVs and as the staging location for SRVs/samplers that are later copied into the shader-visible heaps.
class D3D12StagingDescriptorHeap
{
public:
	D3D12StagingDescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 descriptorsPerBlock, const char* debugName);

	D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
	void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);

	D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_type; }
	uint32 GetAllocatedCount() const { return m_allocatedCount; }

private:
	void AddBlock();

	ID3D12Device* m_device;
	D3D12_DESCRIPTOR_HEAP_TYPE m_type;
	uint32 m_descriptorsPerBlock;
	uint32 m_incrementSize;
	std::string m_debugName;
	std::vector<ComPtr<ID3D12DescriptorHeap>> m_blocks;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_freeList;
	uint32 m_allocatedCount = 0;
	std::mutex m_mutex;
};

// Shader-visible CBV/SRV/UAV heap used as a ring. Tables are written once per draw (or per state change) and
// reclaimed once the submission that used them has completed.
class D3D12GpuViewHeap
{
public:
	D3D12GpuViewHeap(ID3D12Device* device, D3D12SubmissionTracker* tracker, uint32 descriptorCount);

	struct Table
	{
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	};

	Table AllocateTable(uint32 count);
	void OnSubmit(uint64 submissionId);
	void Retire(uint64 completedSubmissionId);

	ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
	uint32 GetIncrementSize() const { return m_incrementSize; }

	D3D12_CPU_DESCRIPTOR_HANDLE OffsetCPU(D3D12_CPU_DESCRIPTOR_HANDLE h, uint32 index) const { h.ptr += (SIZE_T)index * m_incrementSize; return h; }
	D3D12_GPU_DESCRIPTOR_HANDLE OffsetGPU(D3D12_GPU_DESCRIPTOR_HANDLE h, uint32 index) const { h.ptr += (UINT64)index * m_incrementSize; return h; }

private:
	ID3D12Device* m_device;
	D3D12SubmissionTracker* m_tracker;
	ComPtr<ID3D12DescriptorHeap> m_heap;
	uint32 m_size;
	uint32 m_incrementSize;
	D3D12_CPU_DESCRIPTOR_HANDLE m_cpuBase;
	D3D12_GPU_DESCRIPTOR_HANDLE m_gpuBase;
	uint32 m_head = 0;
	uint32 m_tail = 0;
	bool m_recordingHasData = false;
	struct Marker
	{
		uint64 submissionId;
		uint32 end;
	};
	std::deque<Marker> m_markers;
};

// Shader-visible sampler heap. D3D12 limits these to 2048 entries, so instead of a ring we cache whole sampler
// tables (one per unique combination of sampler states). When the heap is full the renderer flushes, waits for idle
// and the cache is reset.
class D3D12GpuSamplerHeap
{
public:
	D3D12GpuSamplerHeap(ID3D12Device* device, uint32 descriptorCount);

	// samplerIds are the unique ids handed out by D3D12SamplerCache and identify the content of the table
	// returns false if the heap is full (caller must submit, wait for idle and call Reset())
	bool GetOrCreateTable(const uint32* samplerIds, const D3D12_CPU_DESCRIPTOR_HANDLE* srcSamplers, uint32 count, D3D12_GPU_DESCRIPTOR_HANDLE& tableOut);
	void Reset();

	ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
	uint32 GetUsedCount() const { return m_head; }

private:
	ID3D12Device* m_device;
	ComPtr<ID3D12DescriptorHeap> m_heap;
	uint32 m_size;
	uint32 m_incrementSize;
	D3D12_CPU_DESCRIPTOR_HANDLE m_cpuBase;
	D3D12_GPU_DESCRIPTOR_HANDLE m_gpuBase;
	uint32 m_head = 0;
	struct TableKey
	{
		std::array<uint32, D3D12Const::kMaxSamplersPerStage> ids{};
		uint32 count{};
		bool operator==(const TableKey& other) const { return count == other.count && ids == other.ids; }
	};
	struct TableKeyHash
	{
		size_t operator()(const TableKey& k) const
		{
			uint64 h = k.count;
			for (uint32 i = 0; i < k.count; i++)
				h = (h ^ k.ids[i]) * 0x100000001B3ull;
			return (size_t)h;
		}
	};
	std::unordered_map<TableKey, D3D12_GPU_DESCRIPTOR_HANDLE, TableKeyHash> m_tableCache;
};

// Deduplicates sampler descriptors. Each unique D3D12_SAMPLER_DESC is created once in a staging heap.
class D3D12SamplerCache
{
public:
	D3D12SamplerCache(ID3D12Device* device, D3D12StagingDescriptorHeap* stagingHeap) : m_device(device), m_stagingHeap(stagingHeap) {}

	// returns the staging descriptor and a unique id for the desc
	D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(const D3D12_SAMPLER_DESC& desc, uint32& idOut);
	uint32 GetCount() const { return (uint32)m_samplers.size(); }

private:
	static uint64 HashDesc(const D3D12_SAMPLER_DESC& desc);

	ID3D12Device* m_device;
	D3D12StagingDescriptorHeap* m_stagingHeap;
	struct Entry
	{
		D3D12_SAMPLER_DESC desc;
		D3D12_CPU_DESCRIPTOR_HANDLE handle;
		uint32 id;
	};
	std::unordered_multimap<uint64, Entry> m_samplers;
};

#pragma once

#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

#include <functional>
#include <optional>

// Submission tracking is shared by every allocator that hands out GPU-visible memory.
// "Submission ids" are the fence values signaled after each ExecuteCommandLists call.
class D3D12SubmissionTracker
{
public:
	virtual ~D3D12SubmissionTracker() = default;
	// id of the command list that is currently being recorded (signaled once it is submitted)
	virtual uint64 GetRecordingSubmissionId() const = 0;
	// highest id that the GPU has finished
	virtual uint64 GetCompletedSubmissionId() = 0;
	// blocks until the GPU has finished the given submission. Must not be called with the recording id
	virtual void WaitForSubmission(uint64 submissionId) = 0;
};

struct D3D12UploadAllocation
{
	uint8* cpuPtr{};
	D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
	ID3D12Resource* resource{};
	uint64 offset{}; // offset within resource
	uint64 size{};
};

// Persistently mapped upload heap used as a ring buffer. Memory is reclaimed once the submission that used it completes.
// If the ring is exhausted by the command list that is still being recorded, a temporary overflow buffer is created
// instead of forcing a mid-draw submit.
class D3D12UploadRing
{
public:
	D3D12UploadRing(ID3D12Device* device, D3D12SubmissionTracker* tracker, uint64 size, const char* debugName);
	~D3D12UploadRing();

	D3D12UploadAllocation Allocate(uint64 size, uint64 alignment);
	// called right before the recording command list is submitted
	void OnSubmit(uint64 submissionId);
	void Retire(uint64 completedSubmissionId);

	uint64 GetSize() const { return m_size; }
	uint64 GetUsedBytes() const;

private:
	std::optional<uint64> TryAllocate(uint64 size, uint64 alignment);
	bool IsEmpty() const { return m_markers.empty() && !m_recordingHasData; }

	ID3D12Device* m_device;
	D3D12SubmissionTracker* m_tracker;
	ComPtr<ID3D12Resource> m_buffer;
	uint8* m_cpuBase{};
	D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase{};
	uint64 m_size;
	uint64 m_head = 0;
	uint64 m_tail = 0;
	bool m_recordingHasData = false;

	struct Marker
	{
		uint64 submissionId;
		uint64 end;
	};
	std::deque<Marker> m_markers;

	struct Overflow
	{
		uint64 submissionId;
		ComPtr<ID3D12Resource> buffer;
	};
	std::deque<Overflow> m_overflow;
};

// Keeps D3D12 objects alive until the GPU no longer references them
class D3D12DeferredReleaser
{
public:
	void Release(ComPtr<IUnknown> object, uint64 submissionId)
	{
		if (!object)
			return;
		std::lock_guard _l(m_mutex);
		m_queue.push_back({ submissionId, std::move(object) });
	}

	void Retire(uint64 completedSubmissionId)
	{
		std::lock_guard _l(m_mutex);
		while (!m_queue.empty() && m_queue.front().submissionId <= completedSubmissionId)
			m_queue.pop_front();
	}

	void ReleaseAll()
	{
		std::lock_guard _l(m_mutex);
		m_queue.clear();
	}

	size_t GetCount()
	{
		std::lock_guard _l(m_mutex);
		return m_queue.size();
	}

private:
	struct Entry
	{
		uint64 submissionId;
		ComPtr<IUnknown> object;
	};
	std::mutex m_mutex;
	std::deque<Entry> m_queue;
};

// Long-lived suballocations in upload memory (used for decoded index data, which Cemu caches across draws).
// Frees are deferred until the GPU has finished using the memory.
class D3D12UploadHeapAllocator : private ChunkedHeap<256>
{
public:
	struct Allocation
	{
		CHAddr addr;
		uint8* cpuPtr{};
		D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
		uint32 size{};
	};

	D3D12UploadHeapAllocator(ID3D12Device* device, D3D12SubmissionTracker* tracker, uint32 chunkSize, const char* debugName);
	~D3D12UploadHeapAllocator();

	Allocation* Allocate(uint32 size, uint32 alignment);
	void Free(Allocation* allocation); // deferred until the recording submission completes
	void Retire(uint64 completedSubmissionId);

	uint32 GetChunkCount() const { return (uint32)m_d3dChunks.size(); }

private:
	uint32 allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize) override;

	struct D3DChunk
	{
		ComPtr<ID3D12Resource> buffer;
		uint8* cpuPtr{};
		D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
	};

	ID3D12Device* m_device;
	D3D12SubmissionTracker* m_tracker;
	uint32 m_chunkSize;
	std::string m_debugName;
	std::vector<D3DChunk> m_d3dChunks;
	struct PendingFree
	{
		uint64 submissionId;
		Allocation* allocation;
	};
	std::deque<PendingFree> m_pendingFrees;
};

// readback heap used for texture readback and occlusion query results
class D3D12ReadbackRing
{
public:
	D3D12ReadbackRing(ID3D12Device* device, uint64 size, const char* debugName);
	~D3D12ReadbackRing();

	// simple linear allocator that wraps around. Callers must ensure that data was consumed before it gets overwritten
	// (texture readback info objects are short lived, the ring is sized generously)
	uint64 Allocate(uint64 size, uint64 alignment);
	ID3D12Resource* GetResource() const { return m_buffer.Get(); }
	uint8* GetCPUPtr(uint64 offset) const { return m_cpuBase + offset; }

private:
	ComPtr<ID3D12Resource> m_buffer;
	uint8* m_cpuBase{};
	uint64 m_size;
	uint64 m_head = 0;
};
